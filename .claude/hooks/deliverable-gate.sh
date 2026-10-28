#!/usr/bin/env bash
# deliverable-gate.sh —— 上线模块交付闸门
# 用法：deliverable-gate.sh [--mode=stop|--mode=session-end]
#   stop（默认）：Stop hook。不通过 → stdout 输出 {"decision":"block","reason":"..."}，exit 0；
#                 通过 → 无输出，exit 0。判定只读文件，不执行任何项目命令。
#   session-end：SessionEnd hook。复用同一判定，把结果写入 session-end-report.md；
#                 不能阻断，缺文件 / JSON 损坏 / 自身出错都 exit 0。
# 依赖：只用 bash 内建（sed/grep 也不依赖，hook 环境不保证 jq）。
# 为什么自己解析 JSON：hook 的 PATH 不保证有 jq；清单是扁平对象数组，bash 正则足够。
# 清单字符串值不要含英文双引号或花括号（README 已写明），否则解析会退化。
set +e
shopt -s nullglob

MODE="stop"
for arg in "$@"; do
  case "$arg" in
    --mode=session-end) MODE="session-end" ;;
    --mode=stop|--mode=) MODE="stop" ;;
  esac
done

ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)}"
DIR="$ROOT/.scratch/release-module"
ACTIVE="$DIR/active"
CHECKLIST="$DIR/checklist.json"
MARKER="$DIR/build.marker"
REPORT="$DIR/session-end-report.md"

INPUT=""
if [ ! -t 0 ]; then
  IFS= read -r -d '' INPUT 2>/dev/null || true
fi

# ---------- 共用判定结果 ----------
CHECKLIST_STATE="ok"          # ok | missing | invalid
UNFINISHED=()                 # "id|behavior|status"
DONE_NO_EVIDENCE=()           # "id|behavior"
DEGRADED_NO_NOTE=()           # "id|behavior"
UNVERIFIED_FILES=()           # 相对 ROOT 的路径
UNVERIFIED_TOTAL=0
MARKER_MISSING=0
FIND_PROBLEM=0             # find 不可用或执行失败：无法证明「改动未验证」这一项
NOTES=()

# 纯参数展开、不开子 shell：Windows Git Bash 上每次命令替换都是一次昂贵 fork。
JSON_ESC=""

parse_checklist() {
  local text obj id behavior status evidence note count=0
  if [ ! -f "$CHECKLIST" ]; then
    CHECKLIST_STATE="missing"
    return 0
  fi
  text="$(<"$CHECKLIST")" 2>/dev/null
  text="${text//$'\r'/ }"
  text="${text//$'\n'/ }"
  # 只取叶子对象（items[] 的元素），顶层对象含花括号不会被 [^{}]* 命中
  while [[ "$text" =~ \{[^{}]*\} ]]; do
    obj="${BASH_REMATCH[0]}"
    text="${text#*"$obj"}"
    [[ "$obj" =~ \"status\"[[:space:]]*: ]] || continue
    count=$((count + 1))
    id=""; behavior=""; status=""; evidence=""; note=""
    [[ "$obj" =~ \"id\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]] && id="${BASH_REMATCH[1]}"
    [[ "$obj" =~ \"behavior\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]] && behavior="${BASH_REMATCH[1]}"
    [[ "$obj" =~ \"status\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]] && status="${BASH_REMATCH[1]}"
    [[ "$obj" =~ \"evidence\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]] && evidence="${BASH_REMATCH[1]}"
    [[ "$obj" =~ \"note\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]] && note="${BASH_REMATCH[1]}"
    status="${status//[[:space:]]/}"
    [ -z "$id" ] && id="(无 id)"
    [ -z "$behavior" ] && behavior="(无 behavior)"
    if [ "$status" != "done" ]; then
      UNFINISHED+=("$id|$behavior|${status:-缺失}")
    elif [ -z "${evidence//[[:space:]]/}" ]; then
      DONE_NO_EVIDENCE+=("$id|$behavior")
    fi
    if [ "$status" = "degraded" ] && [ -z "${note//[[:space:]]/}" ]; then
      DEGRADED_NO_NOTE+=("$id|$behavior")
    fi
  done
  [ "$count" -eq 0 ] && CHECKLIST_STATE="invalid"
  return 0
}

check_unverified() {
  local first f
  if [ ! -d "$ROOT/src" ] && [ ! -d "$ROOT/tests" ]; then
    NOTES+=("src/ 与 tests/ 均不存在，跳过「改动未验证」检查")
    return 0
  fi
  if [ ! -f "$MARKER" ]; then
    MARKER_MISSING=1
    return 0
  fi
  # 用单个 find 而不是 bash 逐文件 -nt：Windows 上 bash 每次 stat 都很贵，
  # 500 个文件会到 1.5s；find -print -quit 无命中也要全扫，但一次进程内完成。
  if ! command -v find >/dev/null 2>&1; then
    FIND_PROBLEM=1
    return 0
  fi
  first="$(find "$ROOT/src" "$ROOT/tests" -type f -newer "$MARKER" -print -quit 2>/dev/null)"
  [ $? -ne 0 ] && { FIND_PROBLEM=1; return 0; }
  [ -z "$first" ] && return 0
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    UNVERIFIED_TOTAL=$((UNVERIFIED_TOTAL + 1))
    [ "${#UNVERIFIED_FILES[@]}" -lt 5 ] && UNVERIFIED_FILES+=("${f#"$ROOT"/}")
  done < <(find "$ROOT/src" "$ROOT/tests" -type f -newer "$MARKER" -print 2>/dev/null)
  return 0
}

checklist_has_findings() {
  [ "$CHECKLIST_STATE" != "ok" ] && return 0
  [ "${#UNFINISHED[@]}" -gt 0 ] && return 0
  [ "${#DONE_NO_EVIDENCE[@]}" -gt 0 ] && return 0
  [ "${#DEGRADED_NO_NOTE[@]}" -gt 0 ] && return 0
  return 1
}

# ---------- Stop：格式化阻断 JSON ----------
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  JSON_ESC="$s"
}

stop_main() {
  local compact reason line first=1
  [ -f "$ACTIVE" ] || return 0
  # 只看输入前 4KB：顶层的 hook_event_name / stop_hook_active 在最前面；
  # 后面的 last_assistant_message 可能很长，甚至恰好提到这个字段名（整串搜索会误放行）。
  compact="${INPUT:0:4096}"
  compact="${compact//[[:space:]]/}"
  case "$compact" in
    *'"stop_hook_active":true'*)
      echo "[交付闸门] 闸门已跳过（防循环）" >&2
      return 0
      ;;
  esac

  parse_checklist
  # 清单本身已经能阻断时不再扫 src/tests（省掉最贵的 find）；交付前清单全绿才会付这次扫描。
  checklist_has_findings || check_unverified

  local findings=()
  case "$CHECKLIST_STATE" in
    missing) findings+=("checklist.json 不存在：无法判定交付项") ;;
    invalid) findings+=("checklist.json 无法解析或没有任何条目：无法判定交付项") ;;
  esac
  if [ "${#UNFINISHED[@]}" -gt 0 ]; then
    findings+=("未完成项 ${#UNFINISHED[@]} 项（status 不是 done）：")
    for line in "${UNFINISHED[@]}"; do
      IFS='|' read -r id behavior status <<< "$line"
      findings+=("  - ${id} ${behavior} [${status}]")
    done
  fi
  if [ "${#DONE_NO_EVIDENCE[@]}" -gt 0 ]; then
    findings+=("${#DONE_NO_EVIDENCE[@]} 项声称完成但没有证据：")
    for line in "${DONE_NO_EVIDENCE[@]}"; do
      IFS='|' read -r id behavior <<< "$line"
      findings+=("  - ${id} ${behavior} (status=done, evidence 为空)")
    done
  fi
  if [ "${#DEGRADED_NO_NOTE[@]}" -gt 0 ]; then
    findings+=("${#DEGRADED_NO_NOTE[@]} 项 degraded 但未说明降级范围：")
    for line in "${DEGRADED_NO_NOTE[@]}"; do
      IFS='|' read -r id behavior <<< "$line"
      findings+=("  - ${id} ${behavior} (status=degraded, note 为空)")
    done
  fi
  if [ "$FIND_PROBLEM" -eq 1 ]; then
    findings+=("无法检查「改动未验证」：find 不可用或执行失败，无法证明 src/tests 没有晚于 build.marker 的改动")
  elif [ "$MARKER_MISSING" -eq 1 ]; then
    findings+=("改了代码但没有更新构建标记：build.marker 不存在，无法证明 src/ 或 tests/ 的改动已验证")
  elif [ "$UNVERIFIED_TOTAL" -gt 0 ]; then
    findings+=("改了代码但没有更新构建标记（src/ tests/ 有文件晚于 build.marker）：")
    for line in "${UNVERIFIED_FILES[@]}"; do
      findings+=("  - ${line}")
    done
    [ "$UNVERIFIED_TOTAL" -gt "${#UNVERIFIED_FILES[@]}" ] && findings+=("  - …共 ${UNVERIFIED_TOTAL} 个文件")
  fi

  [ "${#findings[@]}" -eq 0 ] && return 0

  reason=""
  for line in "${findings[@]}"; do
    json_escape "$line"
    if [ "$first" -eq 1 ]; then
      reason="$JSON_ESC"
      first=0
    else
      reason="${reason}\\n$JSON_ESC"
    fi
  done
  printf '{"decision":"block","reason":"%s"}\n' "$reason"
  return 0
}

# ---------- SessionEnd：写存档报告，永不阻断 ----------
write_report_header() {
  printf '# 上线模块交付存档（SessionEnd）\n\n'
  printf -- '- 时间：%s\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '未知')"
}

write_list_section() {
  local title="$1"; shift
  printf '\n## %s\n\n' "$title"
  if [ "$#" -eq 0 ]; then
    printf '无\n'
    return 0
  fi
  local line
  for line in "$@"; do
    printf -- '- %s\n' "$line"
  done
}

session_end_main() {
  local active_state="启用" conclusion="全部通过"
  local unfinished_lines=() done_no_evidence_lines=() degraded_lines=()
  local unverified_lines=() note_lines=() line id behavior status
  [ -f "$ACTIVE" ] || active_state="未启用（active 不存在）"

  if [ -f "$ACTIVE" ]; then
    parse_checklist
    check_unverified
  else
    CHECKLIST_STATE="skipped（闸门未启用）"
    NOTES+=("闸门未启用（active 不存在）：按「无待办」处理，未做清单判定")
  fi

  case "$CHECKLIST_STATE" in
    missing) NOTES+=("checklist.json 不存在：按「无待办」处理") ;;
    invalid) NOTES+=("checklist.json 无法解析或没有任何条目：按「无待办」处理") ;;
  esac
  for line in "${UNFINISHED[@]}"; do
    IFS='|' read -r id behavior status <<< "$line"
    unfinished_lines+=("${id} | ${behavior} | ${status}")
  done
  for line in "${DONE_NO_EVIDENCE[@]}"; do
    IFS='|' read -r id behavior <<< "$line"
    done_no_evidence_lines+=("${id} | ${behavior} | status=done 但 evidence 为空")
  done
  for line in "${DEGRADED_NO_NOTE[@]}"; do
    IFS='|' read -r id behavior <<< "$line"
    degraded_lines+=("${id} | ${behavior} | status=degraded 但 note 为空")
  done
  if [ "$FIND_PROBLEM" -eq 1 ]; then
    unverified_lines+=("find 不可用或执行失败，无法检查 src/tests 是否有晚于 build.marker 的改动")
  fi
  if [ "$MARKER_MISSING" -eq 1 ]; then
    unverified_lines+=("build.marker 不存在，无法证明 src/ 或 tests/ 的改动已验证")
  fi
  for line in "${UNVERIFIED_FILES[@]}"; do
    unverified_lines+=("${line}")
  done
  if [ "$UNVERIFIED_TOTAL" -gt "${#UNVERIFIED_FILES[@]}" ]; then
    unverified_lines+=("…共 ${UNVERIFIED_TOTAL} 个文件晚于 build.marker")
  fi
  note_lines=("${NOTES[@]}")

  if [ "$active_state" = "启用" ]; then
    if [ "$CHECKLIST_STATE" != "ok" ]; then
      conclusion="无法判定（checklist.json ${CHECKLIST_STATE}），按「无待办」处理"
    elif [ "${#UNFINISHED[@]}" -gt 0 ] || [ "${#DONE_NO_EVIDENCE[@]}" -gt 0 ] || \
         [ "${#DEGRADED_NO_NOTE[@]}" -gt 0 ] || [ "$MARKER_MISSING" -eq 1 ] || \
         [ "$FIND_PROBLEM" -eq 1 ] || [ "$UNVERIFIED_TOTAL" -gt 0 ]; then
      conclusion="未通过闸门——下次开工按「未完成项 / 未验证改动」清单接续"
    fi
  else
    conclusion="闸门未启用（无待办）"
  fi

  mkdir -p "$DIR" 2>/dev/null || true
  {
    write_report_header
    printf -- '- 闸门：%s\n' "$active_state"
    printf -- '- checklist.json：%s\n' "$CHECKLIST_STATE"
    printf -- '- 结论：%s\n' "$conclusion"
    write_list_section "未完成项（id | behavior | status）" "${unfinished_lines[@]}"
    write_list_section "声称完成但没有证据" "${done_no_evidence_lines[@]}"
    write_list_section "degraded 但未说明降级范围" "${degraded_lines[@]}"
    write_list_section "未验证改动（src/ tests/ 晚于 build.marker）" "${unverified_lines[@]}"
    write_list_section "备注" "${note_lines[@]}"
  } > "$REPORT" 2>/dev/null || true
  return 0
}

if [ "$MODE" = "session-end" ]; then
  session_end_main || true
  exit 0
fi

stop_main || true
exit 0