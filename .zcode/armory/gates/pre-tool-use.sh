#!/bin/sh
# devflow setup 生成的项目门禁（模板：填充占位符后落盘主宿主目录；跳过的 A/B 选项对应分支先裁掉）
# 占位符填法：BUILD_ENTRY 标记 → 构建入口 case 放行模式（单入口如 *bash\ scripts/build.sh*；多入口逐 pattern 通配交替如 *cmd1*|*cmd2*；空格以 \ 转义）；BUILD_FORBIDDEN_PATTERNS / PROTECTED_PATTERNS 标记 → case 通配交替，如 *cmake\ -B*|*cmake\ --build*；MERGE_FORBIDDEN_PATTERNS 同为 case 通配交替（由禁合并源分支名按 devflow-setup SKILL §2 问项 H 规则生成，未配置填哨兵 *__DEVFLOW_NO_MATCH__*）
# devflow 用户值（setup 填写，重跑 converge 从本行自动提取；缺失则按询问项重新确认）: BUILD_ENTRY=*build-test-run.sh* ;; BUILD_FORBIDDEN_PATTERNS=*cmake\ -B*|*cmake\ --build* ;; PROTECTED_PATTERNS=*/build/*|*/build-release/*|*/analysis/*|*/pr-bodies/*|*/splash-delivery/*|*/devdesk-agent-m1-sdd-archive/*|*/eui-neo-zpatch-copy/*|*eui-neo-dropdown-popup-z.patch*|*pr-push-eui-neo.sh* ;; MERGE_FORBIDDEN_PATTERNS=*__DEVFLOW_NO_MATCH__*
# 用法: pre-tool-use.sh <bash-gate|path-gate>；退出码 2 = 阻断
input=$(cat)
mode="${1:-}"
block() {
  echo "[门禁] $1" >&2
  exit 2
}
# bash-gate 匹配面：剥离 heredoc 正文（<<'EOF' … EOF / <<-EOF … \tEOF），只扫真正会被执行的文本。
# 不做这层剥离时，「写一个内容含禁令模式的脚本/文档」会被门禁自己的规则挡住（bootstrap 悖论，与 debt#57 path-gate 同源）
# 已知边界（防呆不防恶意）：echo/grep 参数里字面提及禁令串仍拦；herestring(<<<) 与同行载荷不剥离
strip_heredoc() {
  printf '%s' "$1" | awk '
    BEGIN { q = sprintf("%c", 39); dq = sprintf("%c", 34); tb = sprintf("%c", 9) }
    {
      s = $0; gsub(tb, " ", s); gsub(/\\t/, " ", s)
      n = split(s, L, "\\\\n")
      out = ""
      for (i = 1; i <= n; i++) {
        line = L[i]
        if (inheredoc) {
          t = line; gsub(/^ +/, "", t); gsub(/ +$/, "", t); gsub(/^"+|"+$/, "", t)
          if (t == delim) inheredoc = 0
          continue
        }
        if (match(line, /<<-? *[^ ]+/)) {
          tok = substr(line, RSTART, RLENGTH); sub(/^<<-? */, "", tok)
          gsub(q, "", tok); gsub(dq, "", tok)
          if (tok ~ /^[A-Za-z_]/) { delim = tok; inheredoc = 1 }
        }
        out = out (i > 1 ? "\\n" : "") line
      }
      print out
    }'
}
# 写风险只看目标路径（debt#57：全内容匹配会误伤「内容提及保护路径/占位符」的合法编辑，且造成 gate 自更新 bootstrap 悖论）
# Windows 传反斜杠路径，统一转 / 再匹配（否则 */build/* 之类保护模式永不命中）
# need#33 波2 D4：fp 提取内建化（[[ =~ ]] flat 口径——bug239 同款：flat 保空格去换行/制表；
# R-sid 裁决统一首匹配，旧 sed 贪婪=尾匹配退役）；反斜杠归一 ${var//}（0 spawn；strip_heredoc awk 保留）
flat=$input
flat=${flat//$'\r'/}
flat=${flat//$'\n'/}
flat=${flat//$'\t'/}
fp=''
if [[ $flat =~ \"file_path\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]]; then fp=${BASH_REMATCH[1]}; fi
# MCP 写工具（如 safe_write）用 "path" 字段：file_path 取不到时回退（bug#74）
if [ -z "$fp" ] && [[ $flat =~ \"path\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]]; then fp=${BASH_REMATCH[1]}; fi
fp=${fp//\\//}
case "$mode" in
  bash-gate)
    # 命令串本身即风险面，全内容匹配是 bash-gate 的语义（防呆不防恶意：载荷含字面串即拦）
    # 例外：heredoc 正文是「要写出去的内容」不是「要执行的命令」，匹配前剥离（见 strip_heredoc 注释）
    scan=$(strip_heredoc "$input")
    # 走正规构建入口的一律放行
    case "$scan" in
      *build-test-run.sh*) exit 0 ;;
    esac
    case "$scan" in
      *cmake\ -B*|*cmake\ --build*)
        block "禁止手敲 cmake（cmake -B / cmake --build）。一律: bash scripts/build-test-run.sh（AGENTS.md §五）" ;;
    esac
    # 禁合并源分支（need124：sit/uat 等环境分支不得作合并源把环境配置/未验证代码带入当前分支；merge+pull 一并拦）
    # 已知边界（防呆不防恶意）：参数前置形态（git merge --no-ff sit / git pull --rebase origin sit）与非 origin 远端名不在字面邻接覆盖内；rebase / cherry-pick 语义非合并、真人终端与服务端管控不在本面（需收再追加）
    case "$scan" in
      *__DEVFLOW_NO_MATCH__*)
        mc=$(printf '%s' "$input" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
        block "禁止把禁合并源分支（merge/pull 均拦）带入当前分支: ${mc:-载荷无 command 字段}；确需合并请经真人终端操作（见 AGENTS.md）" ;;
    esac
    # 保护路径 bash 侧禁直连（构建产物/归档面，沿旧 path-gate 保护集）+ 插件自用豁免（need#74）：
    # 命令串含插件工具/测试路径引用时放行保护路径字面量（POOL_DB 传参/手动调试合法）；臂序=禁止绕过→禁合并→豁免→保护路径
    case "$scan" in
      *devflow/tools/*|*devflow/tests/*) ;;
      */build/*|*/build-release/*|*/analysis/*|*/pr-bodies/*|*/splash-delivery/*|*/devdesk-agent-m1-sdd-archive/*|*/eui-neo-zpatch-copy/*|*eui-neo-dropdown-popup-z.patch*|*pr-push-eui-neo.sh*)
        block "保护路径（bash 侧禁直连）：构建产物目录与归档面禁直接操作；构建一律 bash scripts/build-test-run.sh（AGENTS.md §五/§七）" ;;
    esac
    ;;
  path-gate)
    # 未填充模板防呆收窄：仅当落盘目标是门禁/wrapper 脚本自身时，内容含占位符才危险（运行时 case 全失配=静默 fail-open）
    case "$fp" in
      *pre-tool-use.sh|*evidence-capture.sh)
        case "$input" in
          *'{{'*) block "模板未填充（占位符残留）——按 setup 步骤裁剪填充后再落盘" ;;
        esac ;;
    esac
    case "$fp" in
      */build/*|*/build-release/*|*/analysis/*|*/pr-bodies/*|*/splash-delivery/*|*/devdesk-agent-m1-sdd-archive/*|*/eui-neo-zpatch-copy/*|*eui-neo-dropdown-popup-z.patch*|*pr-push-eui-neo.sh*)
        block "保护路径（只读 / 归档）：构建产物目录不可直接写入（一律 bash scripts/build-test-run.sh，AGENTS.md §五）；归档原件已备份仓库外，勿在仓库内重建（AGENTS.md §七）" ;;
    esac
    ;;
esac
exit 0
