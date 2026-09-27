#!/usr/bin/env bash
# DevDesk 一体化脚本：杀旧进程 + 增量构建 + 测试 + 启动 app
# 输出设计：固定结构（阶段标题 + ✅/❌ 结果行），构建正常只显示尾部、
# 失败才显示错误摘要；测试只显示 [FAIL] 行 + Total/Passed/Failed 统计。
# 用法：./scripts/build-test-run.sh [--clean] [--no-test] [--no-run] [--filter 关键词]
#   --clean       全量重建（rm -rf build 重新 configure；默认增量）
#   --no-test     跳过测试（测试挂时不阻断启动 app）
#   --no-run      只杀+构建+测试，不启动 app（app 在跑时链接可能因文件锁失败，会提示）
#   --filter X    只跑描述含 X 的测试用例（透传给 devdesk_tests；注意中文
#                 关键词经 Git Bash 传参可能有编码问题，建议用 ASCII 词）
# 默认：杀旧进程（释放 exe 锁）→ 增量构建 → 全量测试（挂则不启动）→ 启动 app
#
# worktree 感知：git worktree 里没有同级 ../EUI-NEO，脚本按
#   EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO > git 主工作树同级
# 自动定位 EUI-NEO 并传给 configure（-DEUI_NEO_DIR），worktree 首建无需手敲；
# build/ 未初始化（首次/换平台）时脚本自动 configure，无需 --clean。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."

# 工具链 PATH（CLAUDE.md 指定的 Windows 工具链，不在系统 PATH）
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

CLEAN=0; RUN_TEST=1; RUN_APP=1; FILTER=""
for arg in "$@"; do
  case "$arg" in
    --clean)    CLEAN=1 ;;
    --no-test)  RUN_TEST=0 ;;
    --no-run)   RUN_APP=0 ;;
    --filter)   shift; FILTER="$1" ;;
    *)          FILTER="$arg" ;;   # 裸词当 filter
  esac
done

# ---- 0. 解析 EUI-NEO 路径（worktree 里没有同级 ../EUI-NEO，需反推主工作树）----
# 优先级：EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO（主工作树）> git 主工作树同级
EUI_DIR="${EUI_NEO_DIR:-}"
if [ -z "$EUI_DIR" ] && [ -f ../EUI-NEO/CMakeLists.txt ]; then
  EUI_DIR="$(cd ../EUI-NEO && pwd)"
fi
if [ -z "$EUI_DIR" ]; then
  # worktree 的 git-common-dir 指向主工作树的 .git，其父目录即主工作树根
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
  echo "   可用 EUI_NEO_DIR=<EUI-NEO 路径> bash scripts/build-test-run.sh 显式指定" >&2
  exit 1
fi
echo "── EUI-NEO: $EUI_DIR ──"

# ---- 1. 杀旧进程（必须在构建前：运行中的 exe 被 Windows 文件锁住，链接必失败）----
if [ "$RUN_APP" = "1" ]; then
  echo "── 杀旧进程 ──"
  taskkill -F -IM DevDesk.exe 2>/dev/null || true
  taskkill -F -IM devdesk_tests.exe 2>/dev/null || true
  echo "✅ 旧进程已清"
elif tasklist 2>/dev/null | grep -qi devdesk; then
  echo "⚠ DevDesk 正在运行（--no-run 不杀）：构建链接 DevDesk.exe 可能因文件锁失败" >&2
fi

# ---- 2. 构建 ----
# configure 条件：--clean（全量重建）或 build/ 未初始化（worktree 首建、换平台、上次 configure 中途失败）
if [ "$CLEAN" = "1" ]; then
  echo "── 全量重建（rm -rf build）──"
  rm -rf build
elif [ ! -f build/build.ninja ]; then
  echo "── 首次 configure（build/ 未初始化）──"
fi
if [ "$CLEAN" = "1" ] || [ ! -f build/build.ninja ]; then
  CONF_OUT=$(cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DEUI_NEO_DIR="$EUI_DIR" \
    -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 \
    -DZLIB_LIBRARY=C:/msys64/mingw64/lib/libz.dll.a \
    -DZLIB_INCLUDE_DIR="$EUI_DIR/3rd/zlib-1.3.1" 2>&1) || {
    echo "$CONF_OUT" | tail -30
    echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
  echo "✅ configure 完成"
fi
echo "── 构建（增量）──"
BUILD_OUT=$(cmake --build build 2>&1) || {
  echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
  echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
echo "$BUILD_OUT" | tail -2
echo "✅ 构建成功"

# ---- 3. 测试（直接跑 devdesk_tests：输出格式稳定，Total/Passed/Failed 必有）----
if [ "$RUN_TEST" = "1" ]; then
  echo "── 测试 ──"
  TEST_OUT=$(./build/tests/devdesk_tests.exe $FILTER 2>&1) || true
  FAIL_LINES=$(echo "$TEST_OUT" | grep "\[FAIL\]" | head -20)
  if [ -n "$FAIL_LINES" ]; then echo "$FAIL_LINES"; fi
  echo "$TEST_OUT" | grep -E "^(Total|Passed|Failed):" | tr '\n' ' '
  echo ""
  if echo "$TEST_OUT" | grep -q "Failed: 0"; then
    echo "✅ 测试全绿"
  else
    echo "❌ 测试有失败（上方为 FAIL 摘要；要看完整输出跑: ./build/tests/devdesk_tests.exe）"
    exit 1
  fi
fi

# ---- 4. 启动 app ----
if [ "$RUN_APP" = "1" ]; then
  echo "── 启动 ──"
  ./build/DevDesk.exe >/dev/null 2>&1 &
  echo "✅ DevDesk 已启动（后台 PID $!）"
fi
