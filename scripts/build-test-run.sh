#!/usr/bin/env bash
# DevDesk 一体化脚本：杀旧进程 + 增量构建 + 测试 + 启动 app
# 输出设计：固定结构（阶段标题 + ✅/❌ 结果行），构建正常只显示尾部、
# 失败才显示错误摘要；测试只显示 [FAIL] 行 + Total/Passed/Failed 统计。
# 用法：./scripts/build-test-run.sh [--clean] [--release] [--no-test] [--no-run] [--install] [--filter 关键词]
#   --clean       全量重建（rm -rf <构建目录> 重新 configure；默认增量）
#   --release     Release 构建（默认 Debug）。构建目录随之切到 build-release/，
#                 与 Debug 的 build/ 互不覆盖
#   --no-test     跳过测试（测试挂时不阻断启动 app）
#   --no-run      只杀+构建+测试，不启动 app（app 在跑时链接可能因文件锁失败，会提示）
#   --install     仅 macOS：构建/测试之后把本次产物重组进 .app 并**覆盖安装**到
#                 /Applications/DevDesk.app，装完硬断言 sha256 与构建产物一致（见下方
#                 「安装（--install）」段）。非 macOS 上给这个开关 = 硬失败，不静默忽略。
#                 默认不开 = 行为与从前逐字一致（这条开关是本脚本唯一的安装入口）
#   --filter X    只跑描述含 X 的测试用例（透传给 devdesk_tests；注意中文
#                 关键词经 Git Bash 传参可能有编码问题，建议用 ASCII 词）
# 环境变量：DEVDESK_JOBS 并发编译单元数（默认 3，多 worktree 并发时的封顶）；
#           DEVDESK_BUILD_TYPE 构建类型（默认 Debug；--release 等价于 Release）；
#           DEVDESK_BUILD_DIR 构建目录（默认按构建类型选：Debug=build/、Release=build-release/；
#                             显式指定时一律以它为准——差分验证 / 一次性探针走独立目录时用它）；
#           DEVDESK_INSTALL_DIR 安装目标（仅 --install 用；默认 /Applications/DevDesk.app，
#                             必须是绝对路径）；
#           ccache 配置已落 ccache.conf（无需环境变量），临时覆盖才 source scripts/ccache-env.sh
# 默认：杀旧进程（释放 exe 锁）→ 增量构建 → 全量测试（挂则不启动）→ 启动 app
# 平台：Windows（Git Bash/MSYS2，发布路径）与 macOS / Linux 通用。平台差异只在四处分支：
#       PATH 前置、EXE_SUFFIX、configure 的 OPENSSL_ROOT_DIR、发布形态守卫；Windows 行为不变。
#
# 构建目录策略：**一类构建一个目录**，Debug/Release 不共用——共用一个目录时后一次
# configure 会静默覆盖前一次的 CMakeCache/build.ninja，产物类型与预期不符。这个坑手敲
# cmake 时代踩过：同时留着 build/ / build-release/ / build-debug/ 三份互不一致的配置。
# 目录里已有 build.ninja 时跳过 configure 走增量（worktree 首建、换平台、上次 configure
# 中途失败这三种情况才需要 configure）；但若缓存里的 CMAKE_BUILD_TYPE 与本次请求不一致
# 则**硬失败**（防 Debug/Release 在同一目录互相污染），改用对应目录或 --clean。
#
# 安装（--install，仅 macOS）——为什么加它，以及为什么必须带防陈旧断言：
#   2026-09-29 实测事故：`bash scripts/build-test-run.sh --release` 只重建**裸可执行**
#   build-release/DevDesk（12:14:56），而 build-release/DevDesk.app/Contents/MacOS/DevDesk
#   还是 11:13:13 的旧文件 —— 因为 bundle 的组装只发生在 platform/macos/build-app.sh 里，
#   而它原本只被 scripts/package-macos.sh（打 DMG）调用，常规构建路径根本不碰 bundle。
#   于是「编译 → cp -R build-release/DevDesk.app /Applications/」拷过去的是一个**装着旧
#   二进制的壳**。铁证：同一个字符串 'claude.detail.contentbox' 在裸 exe 里
#   `grep -c -a -F` = 1，在 bundle 内与 /Applications 里 = 0。
#   后果正是本项目被坑过多次的那一类：「改完、测试也绿了，但用户在旧代码上验证新功能，
#   得出『还是没效果』」；而 CLAUDE.md 阶段 5 的标准「编译 = 编译 + 复制到 /Applications」
#   在脚本层做不到（没有一条命令能把「构建 → 重组 bundle → 补 assets → 覆盖安装」串起来）。
#   → --install 把四步串成一条**可判定**的路径，并在装完后**硬断言**已安装二进制与本次
#     构建产物 sha256 一致；不一致就打印两边 mtime/sha 并 exit 1，绝不静默通过。
#     没有这条断言，同样的坑必然再踩第三次（断言是本次改动的核心交付物，不是装饰）。
#   为什么比对 sha256 而不是 mtime：cp -R / ditto / touch -t 都能造出任意 mtime，时间戳
#     相同不代表内容相同、不同也不代表内容不同；只有字节比对能回答「装上去的是不是这次
#     编出来的」。
#
# worktree 感知：git worktree 里没有同级 ../EUI-NEO，脚本按
#   EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO > git 主工作树同级
# 自动定位 EUI-NEO 并传给 configure（-DEUI_NEO_DIR），worktree 首建无需手敲；
# <构建目录> 未初始化（首次/换平台）时脚本自动 configure，无需 --clean。
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

# 构建类型：默认 Debug（既有行为）；--release 或 DEVDESK_BUILD_TYPE 切别的类型。
# 只认 Debug / Release 两种，写错在 configure 前就报错（免得 CMake 静默按空类型构建）。
BUILD_TYPE="${DEVDESK_BUILD_TYPE:-Debug}"
CLEAN=0; RUN_TEST=1; RUN_APP=1; INSTALL=0; FILTER=""
for arg in "$@"; do
  case "$arg" in
    --clean)    CLEAN=1 ;;
    --release)  BUILD_TYPE="Release" ;;
    --no-test)  RUN_TEST=0 ;;
    --no-run)   RUN_APP=0 ;;
    --install)  INSTALL=1 ;;
    --filter)   shift; FILTER="$1" ;;
    *)          FILTER="$arg" ;;   # 裸词当 filter
  esac
done
case "$BUILD_TYPE" in
  Debug|Release) ;;
  *) echo "❌ 未知构建类型 '$BUILD_TYPE'（只支持 Debug / Release；注意大小写）" >&2; exit 1 ;;
esac
# --install 只在 macOS 有实现（.app bundle 是 macOS 形态）。非 macOS 给这个开关必须**出声**：
# 静默忽略等于「以为装上了、其实没装」——与本次要修的「静默装旧二进制」是同一类失败。
# 放在构建之前判，免得白跑一遍几分钟的构建再报错。
if [ "$INSTALL" = "1" ] && [ "$DEVDESK_OS" != "darwin" ]; then
  echo "❌ --install 只在 macOS 上可用（当前平台：${DEVDESK_OS}；本平台没有 .app bundle 形态）" >&2
  case "$DEVDESK_OS" in
    windows) echo "   Windows 侧打安装包请用：bash scripts/package-windows.sh（Inno Setup）" >&2 ;;
    *)       echo "   Linux 侧本项目无安装器实现，直接跑 <构建目录>/DevDesk 即可" >&2 ;;
  esac
  exit 1
fi

# ---- 构建目录：一类构建一个目录（Debug/Release 不共用），DEVDESK_BUILD_DIR 可整体覆盖 ----
# 为什么要按类型分开：configure 会往目录里写 CMakeCache.txt / build.ninja，两类构建共用一个
# 目录 = 后一次 configure 静默覆盖前一次的缓存，产物类型与预期不符（手敲 cmake 时代正是这么
# 踩的：build/ / build-release/ / build-debug/ 三份漂移配置并存）。分开后 --clean 也只删本类目录。
# DEVDESK_BUILD_DIR 显式指定时以它为准——差分验证 / 一次性探针要在独立目录里跑，走这个开关，
# 不用绕回手敲 cmake（与 platform/macos/build-app.sh 的同名变量语义一致）。
if [ -n "${DEVDESK_BUILD_DIR:-}" ]; then
  BUILD_DIR="$DEVDESK_BUILD_DIR"
elif [ "$BUILD_TYPE" = "Release" ]; then
  BUILD_DIR="build-release"
else
  BUILD_DIR="build"
fi
# 归一成绝对路径：kill_by_exe_path / APP_BIN 都要绝对路径，而 DEVDESK_BUILD_DIR 可能是相对的；
# 目录已存在时以 cd 后的真实路径为准（消除 `..` / 尾斜杠 / 相对形态的差异）。
case "$BUILD_DIR" in
  /*) BUILD_DIR_ABS="$BUILD_DIR" ;;
  *)  BUILD_DIR_ABS="$PWD/$BUILD_DIR" ;;
esac
if [ -d "$BUILD_DIR" ]; then BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd)"; fi
echo "── 构建: $BUILD_TYPE → $BUILD_DIR/ ──"

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

# app / 测试二进制的调用路径。Windows 保持相对路径调用（行为不变）；macOS/Linux 用
# 本 build 目录下的**绝对路径**——kill_by_exe_path 的 pkill -f 是按命令行匹配的，只有绝对路径
# 才能把本 build 目录的产物和别的 worktree 的同名产物区分开（多个 build 目录并存时同理）。
APP_BIN="$BUILD_DIR/DevDesk$EXE_SUFFIX"
TEST_BIN="$BUILD_DIR/tests/devdesk_tests$EXE_SUFFIX"
if [ "$DEVDESK_OS" != "windows" ]; then
  APP_BIN="$BUILD_DIR_ABS/DevDesk$EXE_SUFFIX"
  TEST_BIN="$BUILD_DIR_ABS/tests/devdesk_tests$EXE_SUFFIX"
fi

# DevDesk 是否在跑（--no-run 时给个提示；只警告不阻断，故任何失败都当「没在跑」）
desk_running() {
  case "$DEVDESK_OS" in
    windows) tasklist 2>/dev/null | grep -qi devdesk ;;
    # pgrep -f 按完整命令行匹配，与 tasklist 的「宽匹配、只取存在性」语义对齐
    *)       pgrep -f "$BUILD_DIR_ABS/DevDesk$EXE_SUFFIX" >/dev/null 2>&1 ;;
  esac
}

if [ "$RUN_APP" = "1" ]; then
  echo "── 杀旧进程 ──"
  # 传**绝对路径**（两平台都必须）：Windows 侧 PowerShell 比的是进程映像路径 $_.Path，
  # 非 Windows 侧 pkill -f 比的是命令行——两者都只有绝对路径才命中（这也是 APP_BIN 在
  # macOS/Linux 上取绝对路径的原因；Windows 侧启动路径保持相对形式不变）。
  kill_by_exe_path "$BUILD_DIR_ABS/DevDesk$EXE_SUFFIX"
  kill_by_exe_path "$BUILD_DIR_ABS/tests/devdesk_tests$EXE_SUFFIX"
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
  windows)
    # 不写死 C:/msys64 —— MSYS2 可以装在别处（CI 的 setup-msys2 就装在
    # D:/a/_temp/msys64），写死会导致「本机过、CI 挂 Could NOT find OpenSSL」。
    # MSYS2 的 shell 一定导出 MINGW_PREFIX（形如 C:/msys64/mingw64），
    # 取它；没有才退回默认安装位置。
    _mingw_root="${MINGW_PREFIX:-C:/msys64/mingw64}"
    OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR="$_mingw_root")
    ;;
  darwin)  OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3) ;;
esac

# configure 条件：--clean（全量重建）或 <构建目录> 未初始化（worktree 首建、换平台、上次 configure 中途失败）
if [ "$CLEAN" = "1" ]; then
  echo "── 全量重建（rm -rf ${BUILD_DIR}）──"
  rm -rf "$BUILD_DIR"
elif [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "── 首次 configure（$BUILD_DIR 未初始化）──"
fi
# 构建类型一致性守卫（对跳过 configure 的增量路径尤其重要）：目录已 configure 过、但缓存里的
# CMAKE_BUILD_TYPE 与本次请求不同 → 硬失败，不静默复用。静默复用的后果是 `--release` 产出
# Debug 二进制（或反之）——产物类型与预期不符且无从察觉，比当场报错危险得多。
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && grep -q "^CMAKE_BUILD_TYPE:" "$BUILD_DIR/CMakeCache.txt"; then
  CACHED_TYPE="$(grep -m1 "^CMAKE_BUILD_TYPE:" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- | tr -d '\r')"
  if [ "$CACHED_TYPE" != "$BUILD_TYPE" ]; then
    echo "❌ $BUILD_DIR 已按 $CACHED_TYPE 配置（CMAKE_BUILD_TYPE=${CACHED_TYPE}），本次请求 $BUILD_TYPE" >&2
    echo "   一类构建一个目录：换目录（DEVDESK_BUILD_DIR=<dir>）或 --clean 重建本目录" >&2
    exit 1
  fi
fi
if [ "$CLEAN" = "1" ] || [ ! -f "$BUILD_DIR/build.ninja" ]; then
  CONF_OUT=$(cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DEUI_NEO_DIR="$EUI_DIR" \
    "${CCACHE_ARGS[@]}" \
    "${OPENSSL_ARGS[@]}" 2>&1) || {
    echo "$CONF_OUT" | tail -30
    echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
  echo "✅ configure 完成"
fi
echo "── 构建（增量）──"
# DEVDESK_BUILD_VERBOSE=1 → 构建输出**实时流式**打印（默认关，行为与从前逐字一致）。
# 为什么需要：默认把 ninja 输出整个吞进变量、只在结束时打尾两行，本地看着清爽，
# 但在 CI 里整个构建阶段一行不刷 —— 十几分钟的编译看起来像卡死（用户实测反馈
# "win 的编译卡住了 没输出"）。CI 里设这个变量即可看到实时进度。
if [ "${DEVDESK_BUILD_VERBOSE:-0}" = "1" ]; then
  _build_log="$(mktemp -t devdesk-build.XXXXXX)"
  if ! cmake --build "$BUILD_DIR" 2>&1 | tee "$_build_log"; then
    echo "──── 错误摘要 ────"
    grep -iE "error|FAILED" "$_build_log" | head -30
    echo "❌ 构建失败（上方为错误摘要）"; rm -f "$_build_log"; exit 1
  fi
  rm -f "$_build_log"
  echo "✅ 构建成功"
else
  BUILD_OUT=$(cmake --build "$BUILD_DIR" 2>&1) || {
    echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
    echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
  echo "$BUILD_OUT" | tail -2
  echo "✅ 构建成功"
fi

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
#      普通 find_package 路径），非 Windows 的 <构建目录>/CMakeCache.txt 里根本没有这个条目 ——
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
# ↓↓ Windows 分支：原断言逐字保留（含缩进，故意不重新缩进），只把写死的 .exe 换成 $EXE_SUFFIX、写死的 build/ 换成 $BUILD_DIR ↓↓
if [ ! -f $BUILD_DIR/DevDesk$EXE_SUFFIX ]; then
  echo "⚠ 发布形态守卫跳过：$BUILD_DIR/DevDesk$EXE_SUFFIX 不存在"
elif ! command -v objdump >/dev/null 2>&1; then
  echo "⚠ 发布形态守卫跳过：objdump 不可用"
else
  SYS32="$(printf '%s' "${SystemRoot:-C:/Windows}" | tr '\\\\' '/')/System32"
  BAD_DLLS=""
  while read -r dll; do
    [ -n "$dll" ] || continue
    if [ ! -f "$SYS32/$dll" ]; then BAD_DLLS="$BAD_DLLS $dll"; fi
  done < <(objdump -p $BUILD_DIR/DevDesk$EXE_SUFFIX 2>/dev/null | awk '/DLL Name:/{print $3}')
  if [ -n "$BAD_DLLS" ]; then
    echo "❌ 发布形态：DevDesk$EXE_SUFFIX 依赖非系统 DLL:${BAD_DLLS}（期望单 exe 零外部 DLL）"
    exit 1
  fi
  if grep -q "DEVDESK_STATIC:BOOL=ON" $BUILD_DIR/CMakeCache.txt; then
    echo "✅ 发布形态（单 exe / 零外部 DLL；DEVDESK_STATIC=ON）"
  else
    echo "❌ 发布形态：$BUILD_DIR/CMakeCache.txt 缺 DEVDESK_STATIC:BOOL=ON（静态链接被关掉）"
    exit 1
  fi
fi
# ↑↑ Windows 分支结束 ↑↑
else
  if [ ! -f "$BUILD_DIR/DevDesk$EXE_SUFFIX" ]; then
    echo "⚠ 发布形态守卫跳过：$BUILD_DIR/DevDesk$EXE_SUFFIX 不存在"
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
    done < <(list_dynamic_deps "$BUILD_DIR/DevDesk$EXE_SUFFIX")
    if [ -n "$NON_SYS_LIBS" ]; then
      echo "⚠ 发布形态（$DEVDESK_OS 上降级为警告）：DevDesk$EXE_SUFFIX 依赖非系统库：$NON_SYS_LIBS"
      echo "   理由：单 exe / 零外部 DLL 是 Windows 发布契约，DEVDESK_STATIC 是 MinGW 专有开关"
      echo "   （cmake/Dependencies.cmake 整段包在 if(MINGW) 里），非 Windows 平台不适用，故不硬失败"
    else
      echo "✅ 发布形态（$DEVDESK_OS 检查：$DEP_TOOL 未列出非系统库依赖）"
    fi
    echo "⏭ 发布形态守卫跳过 DEVDESK_STATIC 断言：该开关是 MinGW 专有（cmake/Dependencies.cmake 整段包在 if(MINGW) 里），非 Windows 的 $BUILD_DIR/CMakeCache.txt 里没有这个条目"
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
    echo "❌ 测试有失败（上方为 FAIL 摘要；要看完整输出跑: ${TEST_BIN}）"
    exit 1
  fi
fi

# ---- 3b. 模块隔离静态守卫（TIN-01；纯静态检查，--no-test 也跑）----
# 原守卫编在 test_settings.cpp 里、靠 __FILE__ 上溯仓库根，ccache 把 __FILE__
# 相对化后 cwd=仓库根 必误报「找不到仓库根」；改由独立脚本承担（脚本自身不读
# $PWD、全程绝对路径）。失败 → 退出码 1，不静默绿。
echo "── 模块隔离守卫 ──"
# 路径用「相对仓库根」而非 $(dirname "$0")：本脚本开头已 cd 到仓库根，
# 且 $0 可能是裸文件名（bash build-test-run.sh 从 scripts/ 内调用），
# 那种形态下 dirname "$0" 会解析错目录。相对根的写法与上方 $BUILD_DIR/... 同风格。
if ! bash scripts/check-module-isolation.sh; then
  echo "❌ 模块隔离守卫失败（上方为 文件:行:命中行；修 settings 侧，勿越模块 link）"
  exit 1
fi

# ---- 3c. 安装到 /Applications（--install，仅 macOS；默认不开则整段不执行）----
# 顺序即「事故的反面」，每一步都在补一个曾经漏掉的环节：
#   ① 重组 bundle：build-app.sh 会 rm -rf 重建 .app + 拷二进制 + 刷 Info.plist 版本 + 拷 assets。
#      **必须走它**，不要手搓 cp —— 手搓正是事故成因（bundle 没人重组，壳一直停在旧版本）。
#   ② assets 补拷 + 三条断言（与 scripts/package-macos.sh §4 同款口径：rm -rf 后
#      cmake -E copy_directory，再断言 shadertoy 非空 / pets/README.md / 每个皮肤目录有 pet.json）。
#      顺序关键：**必须在 ① 之后**——build-app.sh 会 rm -rf 整个 bundle，先补后建等于白补。
#      改这里要同步改 package-macos.sh §4（不发明第二套口径）。
#   ③ 版本号：单一源 = 构建目录 CMakeCache.txt 的 CMAKE_PROJECT_VERSION（同 package-macos.sh §5）；
#      build-app.sh 已把它写进产物 Info.plist，这里断言产物里确实是它——Info.plist 也是「旧壳」
#      会露馅的地方，与二进制同属交付物的一部分。
#   ④ 防陈旧断言 A：bundle 内二进制 == 本次构建产物。
#   ⑤ 覆盖安装（整包替换）。
#   ⑥ 防陈旧断言 B（本任务的核心交付物）：**已安装**二进制 == 本次构建产物，不一致硬失败并打印
#      两边 mtime/sha。断言 B 是唯一能证明「用户接下来跑的就是这次编出来的」的检查；没有它，
#      同样的坑必然再踩第三次（断言 A 只证明 bundle 组对了，证明不了装到 /Applications 的那份对）。
if [ "$INSTALL" = "1" ]; then
  INSTALL_DIR="${DEVDESK_INSTALL_DIR:-/Applications/DevDesk.app}"
  # 安装目标必是绝对路径：下面要 rm -rf 它，相对路径会随 cwd 漂移，风险不值得省一行
  case "$INSTALL_DIR" in
    /*) ;;
    *)  echo "❌ DEVDESK_INSTALL_DIR 必须是绝对路径：$INSTALL_DIR" >&2; exit 1 ;;
  esac
  echo "── 安装（重组 .app → 补 assets → 覆盖 ${INSTALL_DIR}）──"

  file_sha256() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }
  file_stamp()  { stat -f '%Sm  %z bytes' -t '%Y-%m-%d %H:%M:%S' "$1" 2>/dev/null; }
  # 防陈旧断言：$1（本次构建产物）与 $2（待验物）必须**字节一致**；不一致即硬失败，
  # 打印两边 mtime/sha256 后 exit 1（不静默通过、也不降级成警告）
  assert_fresh_binary() {
    local expect="$1" actual="$2" label="$3" sha_exp sha_act
    sha_exp="$(file_sha256 "$expect")"
    sha_act="$(file_sha256 "$actual")"
    if [ -z "$sha_exp" ] || [ "$sha_exp" != "$sha_act" ]; then
      echo "❌ 防陈旧断言失败（${label}）：二进制不是本次构建产物" >&2
      echo "   expect: $expect" >&2
      echo "           sha256=$sha_exp  mtime=$(file_stamp "$expect")" >&2
      echo "   actual: $actual" >&2
      echo "           sha256=$sha_act  mtime=$(file_stamp "$actual")" >&2
      echo "   → 硬失败：装上去/待装的必须是这次编出来的二进制，否则用户会在旧代码上验证新功能" >&2
      exit 1
    fi
    echo "✅ 防陈旧断言（${label}）：sha256 ${sha_exp:0:12}… 与本次构建产物一致"
  }

  # ① 重组 bundle
  DEVDESK_BUILD_DIR="$BUILD_DIR_ABS" DEVDESK_SKIP_LSREGISTER=1 bash platform/macos/build-app.sh
  APP_DIR="$BUILD_DIR_ABS/DevDesk.app"
  MACOS_DIR="$APP_DIR/Contents/MacOS"
  if [ ! -x "$MACOS_DIR/DevDesk" ]; then
    echo "❌ .app 内没有可执行文件：$MACOS_DIR/DevDesk（build-app.sh 没跑成功？）" >&2
    exit 1
  fi

  # ② assets 补拷（必须在 ① 之后）
  if [ ! -d "$BUILD_DIR/assets" ]; then
    echo "❌ 找不到 $BUILD_DIR/assets（构建 DevDesk 目标时 POST_BUILD 会生成，缺了说明构建不完整）" >&2
    exit 1
  fi
  rm -rf "$MACOS_DIR/assets"
  cmake -E copy_directory "$BUILD_DIR/assets" "$MACOS_DIR/assets"
  if [ -z "$(ls -A "$MACOS_DIR/assets/shaders/shadertoy" 2>/dev/null)" ]; then
    echo "❌ 断言失败：$MACOS_DIR/assets/shaders/shadertoy 为空" >&2
    exit 1
  fi
  if [ ! -f "$MACOS_DIR/assets/pets/README.md" ]; then
    echo "❌ 断言失败：$MACOS_DIR/assets/pets/README.md 缺失" >&2
    exit 1
  fi
  PET_COUNT=0
  for petdir in "$MACOS_DIR/assets/pets"/*/; do
    [ -d "$petdir" ] || continue
    if [ ! -f "$petdir/pet.json" ]; then
      echo "❌ 断言失败：皮肤目录缺 pet.json: $petdir" >&2
      exit 1
    fi
    PET_COUNT=$((PET_COUNT + 1))
  done
  echo "✅ assets 就位（Contents/MacOS/assets；pets 含 $PET_COUNT 皮肤包）"

  # ③ 版本号（单一源同 package-macos.sh §5；断言产物 Info.plist 已刷成本次版本）
  APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | head -1 | tr -d '\r')"
  if [ -z "$APP_VERSION" ]; then
    echo "❌ 无法从 $BUILD_DIR/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
    exit 1
  fi
  PLIST_VERSION="$(plutil -extract CFBundleShortVersionString raw "$APP_DIR/Contents/Info.plist" 2>/dev/null || true)"
  if [ "$PLIST_VERSION" != "$APP_VERSION" ]; then
    echo "❌ Info.plist 版本未刷新：bundle 里是 '$PLIST_VERSION'，本次构建是 '$APP_VERSION'" >&2
    echo "   （build-app.sh 的 plutil 步骤没生效？旧壳的第二个露馅点，与二进制同属交付物）" >&2
    exit 1
  fi
  echo "✅ 版本 ${APP_VERSION}（bundle Info.plist 与本次构建一致）"

  # ④ 断言 A：bundle 内二进制 == 本次构建产物
  assert_fresh_binary "$BUILD_DIR/DevDesk" "$MACOS_DIR/DevDesk" "bundle 内二进制"

  # ⑤ 覆盖安装（整包替换）。ditto 是 macOS 原生的 bundle 拷贝（比 cp -R 更忠实于 bundle 结构）
  # 运行中的实例**只警告不杀**：本脚本按规则严禁按进程名杀（会杀掉用户 / 别的 agent 的实例），
  # 按路径杀也只覆盖本 build 目录的产物；这里命中说明用户自己开着 /Applications 的实例，
  # 杀掉会丢他未保存的内容 —— 提示他自己退出重启。
  if pgrep -f "$INSTALL_DIR/Contents/MacOS/DevDesk" >/dev/null 2>&1; then
    echo "⚠ DevDesk 正从 $INSTALL_DIR 运行：覆盖安装不会替换那个已在运行的进程，" >&2
    echo "  需自行退出后重新打开才会用到新二进制（脚本不杀进程）" >&2
  fi
  rm -rf "$INSTALL_DIR"
  ditto "$APP_DIR" "$INSTALL_DIR"
  if [ ! -x "$INSTALL_DIR/Contents/MacOS/DevDesk" ]; then
    echo "❌ 安装后 $INSTALL_DIR/Contents/MacOS/DevDesk 不存在或不可执行" >&2
    exit 1
  fi

  # ⑥ 断言 B（核心交付物）：已安装二进制 == 本次构建产物
  assert_fresh_binary "$BUILD_DIR/DevDesk" "$INSTALL_DIR/Contents/MacOS/DevDesk" "已安装二进制"

  if [ ! -d "$INSTALL_DIR/Contents/MacOS/assets" ]; then
    echo "❌ 安装后缺 $INSTALL_DIR/Contents/MacOS/assets（EUI-NEO 按「exe 旁」找资源，缺了会退化）" >&2
    exit 1
  fi
  echo "✅ 已安装：${INSTALL_DIR}（版本 ${APP_VERSION}，二进制与 $BUILD_DIR/DevDesk 逐字节一致）"
fi

# ---- 4. 启动 app ----
if [ "$RUN_APP" = "1" ]; then
  echo "── 启动 ──"
  "$APP_BIN" >/dev/null 2>&1 &
  echo "✅ DevDesk 已启动（后台 PID $!）"
fi
