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
# 环境变量：DEVDESK_JOBS 并发编译单元数（默认 3，多 worktree 并发时的封顶）；
#           ccache 配置已落 ccache.conf（无需环境变量），临时覆盖才 source scripts/ccache-env.sh
# 默认：杀旧进程（释放 exe 锁）→ 增量构建 → 全量测试（挂则不启动）→ 启动 app
# 平台：Windows（Git Bash/MSYS2，发布路径）与 macOS / Linux 通用。平台差异只在四处分支：
#       PATH 前置、EXE_SUFFIX、configure 的 OPENSSL_ROOT_DIR、发布形态守卫；Windows 行为不变。
#
# worktree 感知：git worktree 里没有同级 ../EUI-NEO，脚本按
#   EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO > git 主工作树同级
# 自动定位 EUI-NEO 并传给 configure（-DEUI_NEO_DIR），worktree 首建无需手敲；
# build/ 未初始化（首次/换平台）时脚本自动 configure，无需 --clean。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."

# ---- 平台判定：分支统一走 DEVDESK_OS，不散落成一堆 if ----
DEVDESK_OS="linux"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows" ;;
  Darwin)               DEVDESK_OS="darwin" ;;
  *)                    DEVDESK_OS="linux" ;;
esac
# 可执行文件名后缀：Windows 带 .exe，macOS/Linux 无扩展名（全脚本统一走它，勿再写死）
EXE_SUFFIX=""
if [ "$DEVDESK_OS" = "windows" ]; then EXE_SUFFIX=".exe"; fi

# 工具链 PATH：Windows 的三件套在系统 PATH 之外（CLAUDE.md 指定的固定安装位置）；
# macOS 的 cmake/ninja 由 Homebrew 装在 /opt/homebrew/bin（目录存在且不在 PATH 里才前置，
# 已在 PATH 就不重复追加——幂等）；Linux 不动 PATH（发行版装的工具本就在 PATH 里）。
case "$DEVDESK_OS" in
  windows)
    export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"
    ;;
  darwin)
    case ":$PATH:" in
      *:/opt/homebrew/bin:*) ;;   # 已在 PATH → 不动
      *) if [ -d /opt/homebrew/bin ]; then export PATH="/opt/homebrew/bin:$PATH"; fi ;;
    esac
    ;;
esac

# 并行度封顶：12 核机器上多个 worktree 同时构建时，各自默认 -j(=核数) 会互相拖死
# （CPU/内存全占，单条 build 反而更慢）。默认 3 并发编译单元，可用 DEVDESK_JOBS 覆盖。
export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"

# ---- 编译缓存 ccache（可选，缺则自动降级）----
# 三件套（CCACHE_DIR / CCACHE_BASEDIR / CCACHE_NOHASHDIR）已落进 ccache 持久配置，无需环境变量。
# 持久配置**在两个平台上位置不同**（ccache 各自按平台约定读，写错地方等于没写）：
#   Windows: %LOCALAPPDATA%\ccache\ccache.conf（默认缓存目录下的配置，含 cache_dir/base_dir/hash_dir/max_size）
#            + D:/software/ccache-devdesk/ccache.conf（CCACHE_DIR 被显式指定时读的是这一份）
#   macOS:   ~/Library/Preferences/ccache/ccache.conf —— 不是 XDG 的 ~/.config/ccache/ccache.conf，
#            实测写在 XDG 路径下 ccache 完全无视（--show-config 仍显示默认值），放这里才生效
# 为什么必须设 base_dir/hash_dir：每个 worktree 绝对路径都不同，不把路径改写成「相对当前目录」
# 再哈希，同一份源码在 A/B 两棵树算出的哈希就不同 → 缓存永远 miss（ccache 白装）。
# 需要临时改缓存盘/基准目录：source scripts/ccache-env.sh 后再跑本脚本（环境变量优先级最高）。
# 未安装 ccache → 不传 launcher，行为与从前完全一致（不硬失败）。
CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

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
# TIN-02：按**可执行文件绝对路径**杀，不按进程名杀。原来 taskkill -F -IM <名字> 会
# 杀全机同名进程——多条 worktree 并发构建时互相打断别人的测试/应用（临时路径的
# pid 唯一化已做过，只剩这一条）。匹配不到就静默跳过；PowerShell 不可用/执行失败
# 也不中断（沿用本脚本「失败不阻断构建」的容错风格）。Git Bash 下 taskkill -F 的
# dash 风格约定不受影响——本函数只是不再用它按名字杀。
# 非 Windows 平台沿用同一判据：Darwin/Linux 走 pkill -f 按命令行里的绝对路径匹配（见函数内注释）；
# 那里没有 Windows 的文件锁问题，杀旧进程只为避免留下重复实例。
kill_by_exe_path() {
  local exe_unix="$1" exe_win exe_name
  [ -n "$exe_unix" ] || return 0
  if [ "$DEVDESK_OS" != "windows" ]; then
    # Darwin/Linux：pkill -f 匹配**完整命令行**，只有命令行里带本 build 目录绝对路径的进程
    # 才命中——与 Windows 分支同语义（只杀本 build 目录的产物，不按进程名杀全机同名进程）。
    # 因为判据是「命令行里含这个绝对路径」，app / 测试在本平台上也用**绝对路径启动**
    # （见下面的 APP_BIN / TEST_BIN），否则本脚本自己启动的实例匹配不到。
    # 匹配不到时 pkill 返回 1、无 pkill 时返回 127，一律吞掉，不中断脚本。
    pkill -f "$exe_unix" >/dev/null 2>&1 || true
    return 0
  fi
  # PowerShell 的 $_.Path 是 Windows 形态路径，必须先转换再比较
  exe_win="$(cygpath -w "$exe_unix" 2>/dev/null || true)"
  [ -n "$exe_win" ] || exe_win="$exe_unix"
  exe_name="$(basename "$exe_unix")"
  exe_name="${exe_name%.exe}"   # Get-Process -Name 是进程名（无扩展名）
  command -v powershell >/dev/null 2>&1 || return 0
  powershell -NoProfile -NonInteractive -Command \
    "Get-Process -Name '$exe_name' -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq '$exe_win' } | Stop-Process -Force -ErrorAction SilentlyContinue" \
    >/dev/null 2>&1 || true
  return 0
}

# app / 测试二进制的调用路径。Windows 保持原来的相对路径调用（行为不变）；macOS/Linux 用
# 本 build 目录下的**绝对路径**——kill_by_exe_path 的 pkill -f 是按命令行匹配的，只有绝对路径
# 才能把本 build 目录的产物和别的 worktree 的同名产物区分开。
APP_BIN="./build/DevDesk$EXE_SUFFIX"
TEST_BIN="./build/tests/devdesk_tests$EXE_SUFFIX"
if [ "$DEVDESK_OS" != "windows" ]; then
  APP_BIN="$PWD/build/DevDesk$EXE_SUFFIX"
  TEST_BIN="$PWD/build/tests/devdesk_tests$EXE_SUFFIX"
fi

# DevDesk 是否在跑（--no-run 时给个提示；只警告不阻断，故任何失败都当「没在跑」）
desk_running() {
  case "$DEVDESK_OS" in
    windows) tasklist 2>/dev/null | grep -qi devdesk ;;
    # pgrep -f 按完整命令行匹配，与 tasklist 的「宽匹配、只取存在性」语义对齐
    *)       pgrep -f "build/DevDesk$EXE_SUFFIX" >/dev/null 2>&1 ;;
  esac
}

if [ "$RUN_APP" = "1" ]; then
  echo "── 杀旧进程 ──"
  # 传**绝对路径**（两平台都必须）：Windows 侧 PowerShell 比的是进程映像路径 $_.Path，
  # 非 Windows 侧 pkill -f 比的是命令行——两者都只有绝对路径才命中（这也是 APP_BIN 在
  # macOS/Linux 上取绝对路径的原因；Windows 侧启动路径保持原来的相对形式不变）。
  kill_by_exe_path "$PWD/build/DevDesk$EXE_SUFFIX"
  kill_by_exe_path "$PWD/build/tests/devdesk_tests$EXE_SUFFIX"
  echo "✅ 旧进程已清（仅本 build 目录下的产物）"
elif desk_running; then
  echo "⚠ DevDesk 正在运行（--no-run 不杀）：构建链接 DevDesk$EXE_SUFFIX 可能因文件锁失败" >&2
fi

# ---- 2. 构建 ----
# configure 的平台参数：OpenSSL 三平台取得方式不同，写成变量拼进命令行
#   Windows: MSYS2 的 OpenSSL 不在 CMake 默认搜索路径 → 必须显式给
#   macOS:   系统不带 OpenSSL，用 Homebrew 的 openssl@3（实测可用）
#   Linux:   发行版装在标准路径，find_package 正常找到 → 不带这个 flag
OPENSSL_ARGS=()
case "$DEVDESK_OS" in
  windows) OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR=C:/msys64/mingw64) ;;
  darwin)  OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3) ;;
esac

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
    "${CCACHE_ARGS[@]}" \
    "${OPENSSL_ARGS[@]}" 2>&1) || {
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

# ---- 2b. 发布形态守卫（PAB-06）----
# 发布契约 = 单 exe / 零外部 DLL（CLAUDE.md「发布目标」）。DEVDESK_STATIC=OFF
# 会把 sqlite3/curl/ssl/crypto 静默换成外部 DLL 依赖，构建与测试都照过；这里
# 在测试前单点断言。判据 = objdump 列出的每个 DLL 名必须存在于 %SystemRoot%/System32
# （按名字判存在性，MSYS2 升级新增 api-ms-win-* 不会误报）。只查 DevDesk.exe，
# 测试产物 devdesk_tests.exe 不入发布约束。
#
# 非 Windows 平台（macOS/Linux）降级为**警告**，理由（不是偷懒，是这套判据本身不适用）：
#   ① 「单 exe / 零外部 DLL」是 Windows 发布契约；DEVDESK_STATIC 是 MinGW 专有开关
#      （cmake/Dependencies.cmake 的静态链接特化整段包在 if(MINGW ...) 里，macOS/Linux 走
#      普通 find_package 路径），非 Windows 的 build/CMakeCache.txt 里根本没有这个条目 ——
#      照搬 Windows 断言必然误报，硬失败会挡住 macOS 上的正常构建。
#   ② 平台各自的动态依赖由工具列出（Darwin: otool -L / Linux: ldd），非系统路径下的库只打印
#      警告，供人工判断（macOS 上 Homebrew 的 libssl/libcrypto 出现在这里属预期）。
#   ③ DEVDESK_STATIC 断言的「跳过」要出声（打印原因），不静默跳过。
DEP_TOOL="ldd"
case "$DEVDESK_OS" in
  windows) DEP_TOOL="objdump" ;;
  darwin)  DEP_TOOL="otool" ;;
esac
# 动态依赖清单（只取库路径；Windows 的 objdump -p 输出格式不同，不走此函数）
list_dynamic_deps() {
  case "$DEVDESK_OS" in
    darwin) otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' ;;
    *)      ldd "$1" 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i}' ;;
  esac
}
if [ "$DEVDESK_OS" = "windows" ]; then
# ↓↓ Windows 分支：原断言逐字保留（含缩进，故意不重新缩进），只把写死的 .exe 换成 $EXE_SUFFIX ↓↓
if [ ! -f build/DevDesk$EXE_SUFFIX ]; then
  echo "⚠ 发布形态守卫跳过：build/DevDesk$EXE_SUFFIX 不存在"
elif ! command -v objdump >/dev/null 2>&1; then
  echo "⚠ 发布形态守卫跳过：objdump 不可用"
else
  SYS32="$(printf '%s' "${SystemRoot:-C:/Windows}" | tr '\\\\' '/')/System32"
  BAD_DLLS=""
  while read -r dll; do
    [ -n "$dll" ] || continue
    if [ ! -f "$SYS32/$dll" ]; then BAD_DLLS="$BAD_DLLS $dll"; fi
  done < <(objdump -p build/DevDesk$EXE_SUFFIX 2>/dev/null | awk '/DLL Name:/{print $3}')
  if [ -n "$BAD_DLLS" ]; then
    echo "❌ 发布形态：DevDesk$EXE_SUFFIX 依赖非系统 DLL:$BAD_DLLS（期望单 exe 零外部 DLL）"
    exit 1
  fi
  if grep -q "DEVDESK_STATIC:BOOL=ON" build/CMakeCache.txt; then
    echo "✅ 发布形态（单 exe / 零外部 DLL；DEVDESK_STATIC=ON）"
  else
    echo "❌ 发布形态：build/CMakeCache.txt 缺 DEVDESK_STATIC:BOOL=ON（静态链接被关掉）"
    exit 1
  fi
fi
# ↑↑ Windows 分支结束 ↑↑
else
  if [ ! -f "build/DevDesk$EXE_SUFFIX" ]; then
    echo "⚠ 发布形态守卫跳过：build/DevDesk$EXE_SUFFIX 不存在"
  elif ! command -v "$DEP_TOOL" >/dev/null 2>&1; then
    echo "⚠ 发布形态守卫跳过：$DEP_TOOL 不可用"
  else
    NON_SYS_LIBS=""
    while read -r lib; do
      [ -n "$lib" ] || continue
      case "$lib" in
        # 系统库随 OS 分发，不算外部依赖；其余一律列出待人工判断
        /usr/lib/*|/System/*|/lib/*|/lib64/*|/usr/lib64/*) ;;
        *) NON_SYS_LIBS="$NON_SYS_LIBS $lib" ;;
      esac
    done < <(list_dynamic_deps "build/DevDesk$EXE_SUFFIX")
    if [ -n "$NON_SYS_LIBS" ]; then
      echo "⚠ 发布形态（$DEVDESK_OS 上降级为警告）：DevDesk$EXE_SUFFIX 依赖非系统库：$NON_SYS_LIBS"
      echo "   理由：单 exe / 零外部 DLL 是 Windows 发布契约，DEVDESK_STATIC 是 MinGW 专有开关"
      echo "   （cmake/Dependencies.cmake 整段包在 if(MINGW) 里），非 Windows 平台不适用，故不硬失败"
    else
      echo "✅ 发布形态（$DEVDESK_OS 检查：$DEP_TOOL 未列出非系统库依赖）"
    fi
    echo "⏭ 发布形态守卫跳过 DEVDESK_STATIC 断言：该开关是 MinGW 专有（cmake/Dependencies.cmake 整段包在 if(MINGW) 里），非 Windows 的 build/CMakeCache.txt 里没有这个条目"
  fi
fi

# ---- 3. 测试（直接跑 devdesk_tests：输出格式稳定，Total/Passed/Failed 必有）----
if [ "$RUN_TEST" = "1" ]; then
  echo "── 测试 ──"
  TEST_OUT=$("$TEST_BIN" $FILTER 2>&1) || true
  FAIL_LINES=$(echo "$TEST_OUT" | grep "\[FAIL\]" | head -20)
  if [ -n "$FAIL_LINES" ]; then echo "$FAIL_LINES"; fi
  echo "$TEST_OUT" | grep -E "^(Total|Passed|Failed):" | tr '\n' ' '
  echo ""
  if echo "$TEST_OUT" | grep -q "Failed: 0"; then
    echo "✅ 测试全绿"
  else
    echo "❌ 测试有失败（上方为 FAIL 摘要；要看完整输出跑: $TEST_BIN）"
    exit 1
  fi
fi

# ---- 3b. 模块隔离静态守卫（TIN-01；纯静态检查，--no-test 也跑）----
# 原守卫编在 test_settings.cpp 里、靠 __FILE__ 上溯仓库根，ccache 把 __FILE__
# 相对化后 cwd=仓库根 必误报「找不到仓库根」；改由独立脚本承担（脚本自身不读
# $PWD、全程绝对路径）。失败 → 退出码 1，不静默绿。
echo "── 模块隔离守卫 ──"
# 路径用「相对仓库根」而非 $(dirname "$0")：本脚本第 22 行已 cd 到仓库根，
# 且 $0 可能是裸文件名（bash build-test-run.sh 从 scripts/ 内调用），
# 那种形态下 dirname "$0" 会解析错目录。相对根的写法与下方 ./build/... 同风格。
if ! bash scripts/check-module-isolation.sh; then
  echo "❌ 模块隔离守卫失败（上方为 文件:行:命中行；修 settings 侧，勿越模块 link）"
  exit 1
fi

# ---- 4. 启动 app ----
if [ "$RUN_APP" = "1" ]; then
  echo "── 启动 ──"
  "$APP_BIN" >/dev/null 2>&1 &
  echo "✅ DevDesk 已启动（后台 PID $!）"
fi
