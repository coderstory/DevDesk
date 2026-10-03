#!/usr/bin/env bash
# DevDesk 覆盖率工具（macOS / AppleClang，2026-10-03 已知欠账落地）
# 路线：llvm source-based coverage（-fprofile-instr-generate -fcoverage-mapping）
#   → 跑 devdesk_tests 落 .profraw → xcrun llvm-profdata merge → xcrun llvm-cov 汇总。
# 分层口径 = spec §12.3：core（核心工具类 ≥90%）/ platform（≥70%）/ module_*（Service ≥80%）；
#   EUI-NEO / 测试 / 探针 / app 层源码不插桩（见 CMakeLists.txt 的 DEVDESK_COVERAGE 段），
#   其行数不进分层表（末尾单列「非 DevDesk 层」小计供参考）。
#
# 用法: bash scripts/coverage.sh [选项]
#   (无选项)    全流程：configure（DEVDESK_COVERAGE=ON，独立目录 build-coverage/）
#               → 编 devdesk_tests + DevDesk（后者只为 POST_BUILD 资产）→ 跑全量单测
#               （Failed 非 0 即中止，不出假基线）→ llvm-profdata merge → llvm-cov 分层汇总
#   --clean     删掉 build-coverage/ 全量重来（插桩配置变更 / 缓存漂移时用）
#   --report-only   不构建 / 不跑测试，只用已有 coverage.profdata 重新出汇总表
#   --show <路径后缀…> 逐行标注视图（llvm-cov show）。参数按**路径后缀**匹配且须带
#                   扩展名（如 src/core/sqlite_db.cpp，可给多个）；匹配不到时 llvm-cov
#                   自身会忽略该参数并打印全部文件——见到整库输出 = 过滤词写错了
#   --help      本说明
# 环境变量：DEVDESK_JOBS 并发编译单元数（默认 3，与 build-test-run.sh 同款封顶）；
#           EUI_NEO_DIR 显式指定框架源码（否则按 同级 ../EUI-NEO > git 主工作树同级 定位）。
# 产物（都在 build-coverage/ 下）：
#   coverage-summary.txt  分层表（本次跑的快照，带日期 / git rev / 测试统计）
#   coverage-report.txt   llvm-cov report 全表（人读完整明细，含非 DevDesk 文件）
#   coverage-export.json  llvm-cov export 原始 JSON（机器可再解析）
#   coverage.profdata     合并后的 profile 数据；profraw/ 原始采样
# 注意：
#   * 构建目录固定 build-coverage/，与 build/ / build-release/ 互不覆盖；
#     分层数字只有默认全流程跑出来的才算数（--report-only 不刷新数据）。
#   * CI 未接覆盖率（本地工具，避免 CI 时长膨胀）——登记在 CLAUDE.md 已知欠账。
#   * Windows（MinGW GCC）走 gcovr 路线，TODO 未做；本脚本当前只在 macOS 上可用。
#   * 本脚本与 scripts/build-test-run.sh 的关系：互不调用、互不改动（门禁零变化）；
#     「手敲 cmake」禁令的例外条款适用（脚本化 + 独立目录 + 报告/commit 写明理由）。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下，与 build-test-run.sh 同款）
cd "$(dirname "$0")/.."

# ---- 平台判定（只借 DEVDESK_OS 一处分支，不散落 if）----
DEVDESK_OS="linux"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows" ;;
  Darwin)               DEVDESK_OS="darwin" ;;
  *)                    DEVDESK_OS="linux" ;;
esac

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'

  exit 0
fi

# 本脚本是 macOS（AppleClang + xcrun llvm-*）专用；CMake 侧 DEVDESK_COVERAGE 同样只认
# AppleClang（GCC/MSVC 直接 FATAL_ERROR）。非 macOS 出声拒绝，不静默跑出错误口径。
if [ "$DEVDESK_OS" != "darwin" ]; then
  echo "❌ scripts/coverage.sh 目前只在 macOS（AppleClang）上可用，当前平台：${DEVDESK_OS}" >&2
  echo "   Windows/MinGW GCC 的 gcovr 路线是待办欠账（见 CLAUDE.md 已知欠账表）" >&2
  exit 1
fi

# 工具链 PATH：cmake/ninja/ccache 在 /opt/homebrew/bin（幂等前置，与门禁脚本一致）
case ":$PATH:" in
  *:/opt/homebrew/bin:*) ;;
  *) if [ -d /opt/homebrew/bin ]; then export PATH="/opt/homebrew/bin:$PATH"; fi ;;
esac

# 并行度封顶（多 worktree 并发时防互拖，同门禁脚本）
export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"

BUILD_DIR="build-coverage"
BUILD_DIR_ABS="$PWD/$BUILD_DIR"
[ -d "$BUILD_DIR" ] && BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd)"
TEST_BIN="$BUILD_DIR_ABS/tests/devdesk_tests"
PROFRAW_DIR="$BUILD_DIR_ABS/profraw"
PROFDATA="$BUILD_DIR_ABS/coverage.profdata"
REPORT_TXT="$BUILD_DIR_ABS/coverage-report.txt"
SUMMARY_TXT="$BUILD_DIR_ABS/coverage-summary.txt"
EXPORT_JSON="$BUILD_DIR_ABS/coverage-export.json"

# llvm 工具走 xcrun：与 AppleClang 编译器同一 toolchain，杜绝 profraw 版本不匹配
# （本机实测 Xcode 工具链自带 llvm-profdata / llvm-cov 21）
LLVM_PROFDATA="$(xcrun --find llvm-profdata)" || { echo "❌ xcrun 找不到 llvm-profdata（要装 Xcode CLT）" >&2; exit 1; }
LLVM_COV="$(xcrun --find llvm-cov)" || { echo "❌ xcrun 找不到 llvm-cov（要装 Xcode CLT）" >&2; exit 1; }

# ---- 参数解析 ----
CLEAN=0; REPORT_ONLY=0; SHOW_MODE=0
for arg in "$@"; do
  case "$arg" in
    --clean)        CLEAN=1 ;;
    --report-only)  REPORT_ONLY=1 ;;
    --show)         SHOW_MODE=1 ;;
    *)              break ;;   # 之后的参数留给对应分支处理
  esac
done

# ---- --show：逐行标注视图（不构建不跑测试，吃已有 profdata）----
if [ "$SHOW_MODE" = "1" ]; then
  while [ "${1:-}" = "--show" ]; do shift; done   # 丢掉 --show 本身，剩余全部当源文件过滤串
  if [ ! -f "$PROFDATA" ] || [ ! -x "$TEST_BIN" ]; then
    echo "❌ 缺 $PROFDATA 或 $TEST_BIN —— 先跑一次全流程（bash scripts/coverage.sh）" >&2
    exit 1
  fi
  exec "$LLVM_COV" show "$TEST_BIN" -instr-profile="$PROFDATA" "$@"
fi

# ---- 汇总生成（全流程末段与 --report-only 共用；吃 $PROFDATA + $TEST_BIN）----
# 全表（llvm-cov report，含非 DevDesk 文件）落 $REPORT_TXT 留档；分层表用 export JSON
# 分组计算——report 终端表会截断长路径，不适合脚本解析，只作人读的完整明细。
generate_report() {
  if [ ! -f "$PROFDATA" ] || [ ! -x "$TEST_BIN" ]; then
    echo "❌ 缺 $PROFDATA 或 $TEST_BIN —— 先跑一次全流程（bash scripts/coverage.sh）" >&2
    exit 1
  fi
  "$LLVM_COV" report "$TEST_BIN" -instr-profile="$PROFDATA" > "$REPORT_TXT"
  "$LLVM_COV" export -format=text -summary-only "$TEST_BIN" -instr-profile="$PROFDATA" > "$EXPORT_JSON"
  RUN_DATE="$(date '+%Y-%m-%d %H:%M')"
  GIT_REV="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  COV_DATE="$RUN_DATE" COV_GIT="$GIT_REV" COV_STATS="${COV_TEST_STATS:-}" \
    python3 - "$EXPORT_JSON" "$SUMMARY_TXT" <<'PY'
import json, os, re, sys

export_path, summary_path = sys.argv[1], sys.argv[2]
with open(export_path) as fh:
    data = json.load(fh)
files = data["data"][0]["files"]

# 分层：路径里的 src/<layer>/ 前缀决定归属（core/platform/ui_kit/module_*）；
# 匹配不到的（EUI-NEO 头、系统头等被内联进插桩 TU 的代码）归「非 DevDesk 层」，
# 只给参考小计，不进分层口径。正则对绝对/相对、正反斜杠路径都成立。
layer_re = re.compile(r"(^|[\\/])src/(core|platform|ui_kit|module_[a-z0-9_]+)([\\/]|$)")
buckets = {}
for f in files:
    m = layer_re.search(f["filename"])
    layer = m.group(2) if m else "__other__"
    b = buckets.setdefault(layer, [0, 0, 0])   # [总行, 覆盖行, 文件数]
    s = f["summary"]["lines"]
    b[0] += s["count"]
    b[1] += s["covered"]
    b[2] += 1

# spec §12.3 目标：核心工具类 ≥90% / Platform ≥70% / Service(各模块) ≥80% / UI 不强制
LABEL = {"core": "core", "platform": "platform", "ui_kit": "ui_kit"}
NEED = {"core": 90.0, "platform": 70.0}
order = [l for l in ("core", "platform", "ui_kit") if l in buckets]
order += sorted(l for l in buckets if l.startswith("module_"))
if "__other__" in buckets:
    order.append("__other__")

def pct(c, t):
    return "%.1f" % (100.0 * c / t) if t else "-"

own = [0, 0, 0]
out = []
out.append("DevDesk 覆盖率（llvm-cov，行覆盖）")
out.append("- 日期: %s   git: %s" % (os.environ.get("COV_DATE", ""), os.environ.get("COV_GIT", "")))
out.append("- 测试: %s" % (os.environ.get("COV_STATS") or "（report-only，未刷新数据）"))
out.append("- 数据: build-coverage/coverage.profdata ← devdesk_tests 全量单测")
out.append("        插桩口径: CMakeLists.txt 的 DEVDESK_COVERAGE 段（EUI-NEO/测试/探针/app 源码不插桩）")
out.append("")
out.append("%-20s %5s %16s %7s  %-16s" % ("层", "文件", "行覆盖", "%", "目标(spec §12.3)"))
out.append("-" * 72)
for layer in order:
    t, c, n = buckets[layer]
    if layer == "__other__":
        label, target, mark = "非DevDesk层(参考)", "-", ""
    else:
        label = layer if layer in LABEL else "module:" + layer[len("module_"):]
        if layer == "ui_kit":
            target, mark = "UI 不强制", ""
        elif layer in NEED:
            target = "核心工具类 >=90%" if layer == "core" else ">=70%"
            mark = "Y" if t and 100.0 * c / t >= NEED[layer] else "N"
        else:
            target, mark = "Service >=80%", "Y" if t and 100.0 * c / t >= 80.0 else "N"
        own[0] += t; own[1] += c; own[2] += n
    out.append("%-20s %5d %16s %7s  %-16s %s" % (label, n, "%d/%d" % (c, t), pct(c, t), target, mark))
out.append("-" * 72)
out.append("%-20s %5d %16s %7s" % ("DevDesk 合计", own[2], "%d/%d" % (own[1], own[0]), pct(own[1], own[0])))
text = "\n".join(out)
print(text)
with open(summary_path, "w") as fh:
    fh.write(text + "\n")
PY
}

if [ "$REPORT_ONLY" = "1" ]; then
  echo "── 汇总（report-only，数据 = 已有 $PROFDATA）──"
  generate_report
  exit 0
fi

if [ "$CLEAN" = "1" ]; then
  echo "── 全量重建（rm -rf ${BUILD_DIR}）──"
  rm -rf "$BUILD_DIR"
fi

# ---- 0. EUI-NEO 定位（worktree 里没有同级 ../EUI-NEO；与门禁脚本同款三级回退）----
EUI_DIR="${EUI_NEO_DIR:-}"
if [ -z "$EUI_DIR" ] && [ -f ../EUI-NEO/CMakeLists.txt ]; then
  EUI_DIR="$(cd ../EUI-NEO && pwd)"
fi
if [ -z "$EUI_DIR" ]; then
  COMMON_DIR="$(git rev-parse --git-common-dir 2>/dev/null || true)"
  if [ -n "$COMMON_DIR" ]; then
    MAIN_ROOT="$(cd "$COMMON_DIR/.." 2>/dev/null && pwd || true)"
    if [ -n "$MAIN_ROOT" ] && [ -f "$MAIN_ROOT/../EUI-NEO/CMakeLists.txt" ]; then
      EUI_DIR="$(cd "$MAIN_ROOT/../EUI-NEO" && pwd)"
    fi
  fi
fi
if [ -z "$EUI_DIR" ]; then
  echo "❌ 找不到 EUI-NEO（试过 \$EUI_NEO_DIR / 同级 ../EUI-NEO / 主工作树同级）" >&2
  exit 1
fi
echo "── EUI-NEO: $EUI_DIR ──"

# ccache：与门禁脚本同款（未安装自动降级）；EUI-NEO 不插桩、flag 未变，缓存照常命中
CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# ---- 1. configure（只在目录未初始化或 --clean 后；构建类型固定 Debug）----
# 一致性守卫（同门禁脚本思路）：目录已 configure 过但 DEVDESK_COVERAGE 不是 ON（或
# 构建类型不是 Debug）→ 硬失败，防拿普通构建目录当覆盖率产物、或反之。
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  if ! grep -q "^DEVDESK_COVERAGE:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "❌ $BUILD_DIR 已按 DEVDESK_COVERAGE=OFF（或旧版配置）configure，不能当覆盖率目录用" >&2
    echo "   用 --clean 重建，或删掉该目录" >&2
    exit 1
  fi
  CACHED_TYPE="$(grep -m1 '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- | tr -d '\r')"
  if [ "$CACHED_TYPE" != "Debug" ]; then
    echo "❌ $BUILD_DIR 已按 $CACHED_TYPE 配置，本脚本固定 Debug" >&2
    exit 1
  fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "── configure（DEVDESK_COVERAGE=ON → $BUILD_DIR）──"
  CONF_OUT=$(cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DDEVDESK_COVERAGE=ON \
    -DEUI_NEO_DIR="$EUI_DIR" \
    "${CCACHE_ARGS[@]}" \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 2>&1) || {
    echo "$CONF_OUT" | tail -30
    echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
  echo "✅ configure 完成"
fi

# ---- 2. 构建（devdesk_tests + DevDesk 两个目标；probes 不编——不进覆盖率口径不白编）----
# 为什么还要编 DevDesk：不是要跑它，是吃它的 POST_BUILD 资产拷贝（assets/pets、
# 托盘图标、splash shader → <build>/assets/）——test_tray_icon 的第 3 条断言按
# 「构建产物 assets」形态在 <build>/assets/ 下找 defaultIconPath() 指向的文件，
# 只编 devdesk_tests 时该目录是空的、用例必红（2026-10-03 首跑实测踩到）。
echo "── 构建（devdesk_tests + DevDesk（POST_BUILD 资产），插桩）──"
BUILD_OUT=$(cmake --build "$BUILD_DIR" --target devdesk_tests DevDesk 2>&1) || {
  echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
  echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
echo "$BUILD_OUT" | tail -2
echo "✅ 构建成功"

# ---- 3. 跑全量单测（从仓库根直接跑，与门禁脚本同款；Failed 非 0 即中止不出假基线）----
echo "── 测试（采集 .profraw）──"
mkdir -p "$PROFRAW_DIR"
rm -f "$PROFRAW_DIR"/*.profraw   # 只留本次数据，防陈旧 profraw 混进 merge
# LLVM_PROFILE_FILE 显式指定落点（%p = PID）：绕开「default.profraw 写到哪了」整类坑
export LLVM_PROFILE_FILE="$PROFRAW_DIR/devdesk_tests-%p.profraw"
TEST_OUT=$("$TEST_BIN" 2>&1) || true
FAIL_LINES=$(echo "$TEST_OUT" | grep "\[FAIL\]" | head -20)
if [ -n "$FAIL_LINES" ]; then echo "$FAIL_LINES"; fi
COV_TEST_STATS=$(echo "$TEST_OUT" | grep -E "^(Total|Passed|Failed):" | tr '\n' ' ')
echo "$COV_TEST_STATS"
echo ""
if ! echo "$TEST_OUT" | grep -q "Failed: 0"; then
  echo "❌ 测试有失败——覆盖率基线必须建立在全绿之上，中止（完整输出跑: $TEST_BIN）" >&2
  exit 1
fi
echo "✅ 测试全绿"
PROFRAWS=("$PROFRAW_DIR"/*.profraw)
if [ ! -f "${PROFRAWS[0]}" ]; then
  echo "❌ 测试跑完了但没有 .profraw 落盘（$PROFRAW_DIR 为空）——插桩没生效或运行时没写 profile" >&2
  echo "   排查: ① configure 输出应有「覆盖率插桩: ON」 ② $BUILD_DIR/CMakeCache.txt 有 DEVDESK_COVERAGE:BOOL=ON" >&2
  exit 1
fi

# ---- 4. merge ----
echo "── llvm-profdata merge ──"
"$LLVM_PROFDATA" merge -sparse "${PROFRAWS[@]}" -o "$PROFDATA"
echo "✅ $PROFDATA"

# ---- 5. 汇总 ----
echo "── 汇总 ──"
generate_report
echo "（分层表: $SUMMARY_TXT ；全表明细: $REPORT_TXT ；原始 JSON: $EXPORT_JSON）"
