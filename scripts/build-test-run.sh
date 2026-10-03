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

# ---- 0b. EUI-NEO 版本前置校验（必须在 configure / 构建之前）----
# 为什么放在这里、而不是构建失败之后再解释：DevDesk 与 EUI-NEO 是两个独立仓库，
# CI 用 `git clone --depth 1 <fork>` 取框架，**不锁版本**。2026-10-03 实测踩过——
# 先推 DevDesk 触发了 CI、之后才推 EUI-NEO，于是编译报
#   error: 'class core::dsl::TextBuilder' has no member named 'overflow'
#   （位置 src/module_home/home_quota_card.cpp:617）
# **报错文件是错的**：那行代码没问题，错的是框架版本，一个跨仓库版本不匹配被伪装成
# 了一个指向错误文件的编译错误，排查方向被带偏一整轮。
# 现在它在构建前就红，并明确说「EUI-NEO 版本不匹配」+ 该怎么处置。
# 判据由 DevDesk 自身源码与 CMakeLists 自动推导（见该脚本文件头），不是手维护清单：
# 新增一处 EUI-NEO 消费点或 EUI-NEO 加一条 include 路径，本守卫都自动跟上，零同步。
echo "── EUI-NEO 版本前置校验 ──"
if ! bash scripts/check-eui-neo-version.sh --root "$PWD" --eui-dir "$EUI_DIR"; then
  echo "❌ EUI-NEO 版本前置校验失败（详见上方）" >&2
  echo "   这是**框架版本**问题，不是 DevDesk 代码写错了；先同步 EUI-NEO，别改 DevDesk" >&2
  exit 1
fi

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
  # ⚠ 退出码必须取 PIPESTATUS[0]（= cmake 的码），不能取管道的码：管道码是**最后一个**
  #   命令的码，这里恒为 `tee` 的 0，于是 `if !` 永远不成立 —— 构建失败也照样打
  #   「✅ 构建成功」。没有 pipefail 时 PIPESTATUS 是唯一正确来源（bash 数组，
  #   读一次即失效，故立刻存进变量）。实测抓到过这个静默假绿。
  cmake --build "$BUILD_DIR" 2>&1 | tee "$_build_log"
  _build_rc=${PIPESTATUS[0]}
  if [ "$_build_rc" -ne 0 ]; then
    echo "──── 错误摘要 ────"
    grep -iE "error|FAILED" "$_build_log" | head -30
    echo "❌ 构建失败（上方为错误摘要）"; rm -f "$_build_log"; exit "$_build_rc"
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
  FAIL_LINES=$(echo "$TEST_OUT" | grep -E "\[FAIL\]|^FAIL \[" | head -20)
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

# ---- 3a. UI probe（无头渲染几何/结构断言；与 doctest 是两条独立门禁）----
# ⚠ **为什么必须并进来**：doctest 与 probe 覆盖的东西不重叠，**只跑一条会漏掉整类回归**。
#   实测事故（2026-10-03）：批次 E2 把六类配置从 JSON 搬进 SQLite，doctest 全量绿
#   （1229/1223/0），而三条 probe 因为仍在写 JSON 文件、预置静默失效而变红
#   （3/13 红）。两条门禁给出**相反信号**，只跑 doctest 就把回归合进了 dev。
#   合并后必须看到 **两个** 数字，缺一即视为未验收。
# 走 ctest 而非直接跑 probe 可执行文件：它自带超时与 label 过滤。
# `$FILTER` 非空时跳过（那是定点排查单条 doctest 用例的形态，probe 与之无关），
# 并明确打印跳过原因 —— 静默跳过正是这条门禁当初失效的同类毛病。
if [ "$RUN_TEST" = "1" ]; then
  if [ -n "$FILTER" ]; then
    echo "── UI probe ──"
    echo "⏭ 跳过（--filter 定点模式：probe 与该筛选无关）"
  else
    echo "── UI probe ──"
    # ⚠ **必须带 --output-on-failure**：ctest 默认只给失败清单，**不给失败原因**。
    #   实测事故（2026-10-03）：CI 首跑 probe 14/14 全红，而日志里只有「0% tests
    #   passed, 14 tests failed」和一行清单——**没有一个字说明为什么**，排查者只能
    #   本地复现猜。这与本脚本曾经「构建失败也报成功」是同一类毛病：门禁报了状态，
    #   却把决定状态的那部分信息丢了。失败原因必须落在 CI 日志里。
    #
    # ⚠⚠ **Skipped 必须单独数，不能只看 "100% tests passed"**（2026-10-03 实测）：
    #   ctest 对「跳过」的测试**照样**把 "100% tests passed" 打在汇总行上——全部
    #   跳过时也是 100%（实测：一个全跳过的 -L probe 跑仍输出 "100% tests passed
    #   out of 2"）。只认那一句的话，一条「什么都没验」的门禁会长得和全绿一模一样，
    #   **这正是当初这条门禁失效的同类毛病**。所以下面显式解析三态。
    #
    # ⚠ **总数也从本次运行的输出里取**（汇总行 "... out of N" 的 N = 本次 -L probe
    #   匹配到的全部条数，**含跳过项**）。2026-10-03 用三种形态实测过 N 的口径：
    #   1 通过+1 跳过 / 2 跳过 / 1 通过 分别输出 "out of 2" / "out of 2" / "out of 1"
    #   —— 恒等于 `ctest -N` 的 Total Tests，故不必再跑一次 -N（那一次还会把
    #   Testing/Temporary/LastTest.log 清成 121 字节，跳过原因就永远捞不到了）。
    PROBE_OUT=$(cd "$BUILD_DIR" && ctest -L probe --output-on-failure 2>&1) || true
    echo "$PROBE_OUT" | grep -E "tests passed|tests failed" | tail -1
    # 跳过数：ctest 把跳过项列在 "The following tests did not run:" 段里，
    # 形如 "  2 - t_skip (Skipped)"。逐条数 (Skipped) 行，不靠百分比反推。
    PROBE_SKIPPED=$(echo "$PROBE_OUT" | grep -cE '^\s*[0-9]+ - .*\(Skipped\)$' || true)
    : "${PROBE_SKIPPED:=0}"
    # 跳过原因只在 -V / LastTest.log 里（--output-on-failure **不**显示跳过测试的
    # 输出——实测过）。probe 打印的 PROBE_SKIP_NO_DISPLAY 走 stderr，ctest 同样把它
    # 收进 LastTest.log（实测）。**在任何可能改写 LastTest.log 的命令之前**捞。
    PROBE_SKIP_REASONS=""
    if [ "$PROBE_SKIPPED" -gt 0 ]; then
      PROBE_SKIP_REASONS=$(cd "$BUILD_DIR" && grep -h "PROBE_SKIP_NO_DISPLAY" \
          Testing/Temporary/LastTest.log 2>/dev/null | sed 's/^/  /' | sort -u | head -5 || true)
    fi
    PROBE_TOTAL=$(echo "$PROBE_OUT" \
        | sed -n 's/.*out of \([0-9][0-9]*\).*/\1/p' | tail -1 || true)
    : "${PROBE_TOTAL:=0}"
    # ── 失败优先：任何 Failed 都是红（含「有失败也有跳过」）──
    if ! echo "$PROBE_OUT" | grep -q "100% tests passed"; then
      # 失败原因优先透出：probe 的 stderr 才是有用信息，失败清单次之
      echo "$PROBE_OUT" | grep -E "^\s*[0-9]+ - .*Failed" | head -20
      echo "──── probe 失败原因（前 60 行）────"
      echo "$PROBE_OUT" | grep -vE "^\s*([0-9]+/[0-9]+|Test +#|Start +[0-9]+:)|: (Passed|Failed) +[0-9]" | head -60
      echo "❌ UI probe 有失败（上方为失败清单与原因；本地复现: cd $BUILD_DIR && ctest -L probe --output-on-failure）"
      exit 1
    fi
    # ── 有跳过却解析不出总数 = 判不了三态，宁可红（否则「混合态」会被 -ge 判成
    #    全跳过，在 CI 上直接变绿——那正是最该防的静默假绿）──
    if [ "$PROBE_SKIPPED" -gt 0 ] && [ "$PROBE_TOTAL" -le 0 ]; then
      echo "❌ UI probe 有 ${PROBE_SKIPPED} 个跳过，却解析不出总数（汇总行缺 out-of-N）"
      echo "   —— 无法判定三态，按最坏情况报红，不许当通过"
      exit 1
    fi
    # ── 三态判定（2026-10-03 裁决）──────────────────────────────────────
    #   全绿   (0 跳过)        → 绿
    #   全跳过 (跳过数 == 总数) → 本地（无 GITHUB_ACTIONS）红 + 诊断；CI 绿 + 大声警告
    #   混合   (0 < 跳过 < 总数)→ 红 + 诊断（有显示环境不该出现混合态）
    if [ "$PROBE_TOTAL" -le 0 ]; then
      echo "❌ ctest -L probe 匹配到 0 条测试 —— 探针没被注册进构建（门禁空转，不能当绿）"
      exit 1
    fi
    if [ "$PROBE_SKIPPED" -ge "$PROBE_TOTAL" ]; then
      echo "⚠️  UI probe ${PROBE_SKIPPED}/${PROBE_TOTAL} 个**全部跳过**（ctest 记 Skipped，非失败）"
      echo "──── 跳过原因 ────"
      echo "${PROBE_SKIP_REASONS:-  （未能从 LastTest.log 取到原因文本）}"
      if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
        # CI runner 无 GUI 会话是**已知的环境事实**：恒红会毁掉门禁的意义（每次都红 =
        # 没人再看），但**绝不静默绿**——下面这句必须显式出现在 CI 日志里。
        echo "⚠ probe 未执行：CI runner 无 GUI 会话，本次未验证 UI"
        echo "   （因此本次不判 Fail；在有 GUI 会话的机器上跑同一命令即可真跑:"
        echo "     cd $BUILD_DIR && ctest -L probe --output-on-failure）"
      else
        # 本地 mac 有图形会话：全跳过 = 异常（环境真没显示，或跳过判据误判），必须红。
        echo "❌ 无显示环境，probe 未执行 —— 本机应有 GUI 会话，全部跳过是异常（不是通过）"
        echo "   （本地全跳过 ≠ 全绿：${PROBE_TOTAL} 个 UI 断言一个都没验。要么环境确实没显示，"
        echo "     要么 tests/probes/probe_harness.h 的金丝雀判据误判）"
        exit 1
      fi
    elif [ "$PROBE_SKIPPED" -gt 0 ]; then
      # 同一条判据对所有探针一视同仁，正常只有「全跑」或「全跳」两种形态；
      # 部分跳过 = 判据误判或代码/环境异常，必须人工查。
      echo "❌ UI probe 部分跳过（${PROBE_SKIPPED}/${PROBE_TOTAL}）——判定为判据误判或代码 bug，必须人工查"
      echo "──── 跳过原因 ────"
      echo "${PROBE_SKIP_REASONS:-  （未能从 LastTest.log 取到原因文本）}"
      echo "   （有 GUI 会话的机器上不该出现混合态；复现: cd $BUILD_DIR && ctest -L probe --output-on-failure -V）"
      exit 1
    else
      echo "✅ UI probe 全绿"
    fi
  fi
fi

# ---- 3b-0. 探针跳过登记守卫（2026-10-03 agent-hd 收尾批）----
# probe 在无显示环境以 77 退出（ctest 的跳过约定），tests/CMakeLists.txt 必须给
# **每个** probe 测试登记 SKIP_RETURN_CODE 77。少一条 = 无显示 CI 里那条 77 被
# ctest 当 Failed（「诚实跳过」退化成「环境红」——正是上面三态要防的假红）。
# 守卫读 **configure 产物**（生成 CTestTestfile.cmake）而不是源码注释，自带
# --self-test 证明非恒真（见脚本文件头）。
echo "── 探针跳过登记守卫 ──"
if ! bash scripts/check-probe-skip-gate.sh --build-dir "$BUILD_DIR"; then
  echo "❌ 探针跳过登记守卫失败（某个 probe 测试没登记 SKIP_RETURN_CODE，或登记值与 harness 的 kSkipExitCode 不一致）"
  exit 1
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

# ---- 3b-2. 余量落库路径守卫（G8；纯静态检查，--no-test 也跑）----
# 把 quota_store.h 不变量 3（「applyFetchResult 是落库唯一入口」）从**注释断言**
# 变成会红的代码断言。少一次全量门禁的成本，远低于某天有人顺手加一句直写、
# 韧性决策静默失效。守卫自带 --self-test 证明它不是恒绿。
echo "── 余量落库路径守卫 ──"
if ! bash scripts/check-quota-write-paths.sh; then
  echo "❌ 余量落库路径守卫失败（profile_quota_result 的 DML 只能在 quota_store.cpp 的 applyFetchResult / deleteResult 里）"
  exit 1
fi

# ---- 3b-3. 探针门禁标签守卫（G8 遗留 4；纯静态检查，--no-test 也跑）----
# 真机出网探针（tools/quota_live_probe.cpp）必须有「想跑的人才找得到」的显式入口，
# 又**绝不能**进默认门禁（真网络 + 本机 cc-switch 凭据 + 每次实打实打四家）。
# 那条「绝不进」原本只是一句注释；本守卫把它钉成会红的断言（自带 --self-test）。
echo "── 探针门禁标签守卫 ──"
if ! bash scripts/check-probe-gate-labels.sh; then
  echo "❌ 探针门禁标签守卫失败（quota_live_probe 必须挂 live 标签、不得挂 probe，门禁脚本不得调用它）"
  exit 1
fi

# ---- 3b-4. 余量链停机顺序守卫（M5；纯静态检查，--no-test 也跑）----
# 「测试线程 join → 刷新器 stopAll+reset → 凭据源 reset → store reset」是本批
# 最贵的并发契约之一，此前**只有注释在说**。静态审计实测两次变异全量零新增失败：
# 把 quotaStore_.reset() 挪到最前（正是要防的悬垂）、把 join 整行删掉（留 joinable
# → 析构 std::terminate）。守卫对**源码形态**断言顺序，成本为零、确定性 100%。
if ! bash scripts/check-quota-shutdown-order.sh; then
  echo "❌ 余量停机顺序守卫失败（顺序错了就是悬垂：store 先死，刷新器下一次扫描解引用即 UB）"
  exit 1
fi

# ---- 3b-4. Windows 交叉编译守卫（2026-10-03；纯静态检查，--no-test 也跑）----
# 受编集合 = **从 CMakeLists 的 WIN32 分支解析出来的、Windows CI 真正会编的源文件**
# （src + tests，共享文件全在内）。早先版本只编 `*_win.cpp`，那是错的：CI 编整棵树，
# 共享文件它也编——`src/module_claude_proxy/quota/quota_store.cpp:43` 缺
# `#include <optional>` 就是在共享文件上炸的，旧门禁压根不编它，红得毫无预警。
# 找不到工具链时它**显式打印跳过原因并退出 0**（CI 的 macOS runner 没装这条链，
# 「别人机器上必红」的守卫会被立刻关掉）；装了链还编不过才是红。
# ⚠ 边界：它只验**编译**，且本机是 clang+libc++、CI 是 GCC+libstdc++，两者标准库
#   不同 ⇒「本机编得过」推不出「CI 编得过」。DPAPI 等运行时行为更是验不到。
echo "── Windows 交叉编译守卫 ──"
if ! bash scripts/check-windows-cross-compile.sh; then
  echo "❌ Windows 交叉编译守卫失败（某个 Windows 源文件编不过；上方为 file:line + 错因）"
  exit 1
fi

# ---- 3b-5. std include 守卫（2026-10-03；纯静态检查，--no-test 也跑）----
# 上面那个守卫**抓不到缺标准头**：本机交叉链是 libc++，`quota_store.cpp` 删掉
# `#include <optional>` 照样编过（`<optional>` 被 core/sqlite_db.h → <map> →
# __node_handle 第 8 层传递带进来），而 MSYS2 的 libstdc++ 不带 → 只有 CI 会红。
# 所以另立一道与标准库无关的判据：本文件直接用了 std::X，就得显式 include 对应头。
echo "── std include 守卫 ──"
if ! bash scripts/check-std-includes.sh; then
  echo "❌ std include 守卫失败（有文件直接用了 std 符号却只靠标准库内部传递包含才看得见）"
  exit 1
fi

# ---- 3b-6. 守卫自检（2026-10-03 门禁加固）----
# 这一段跑的**不是**代码，是上面那些守卫**自己**。它回答的是一个前面所有段都回答
# 不了的问题：「这些守卫现在还活着吗？」
#
# 为什么必须接进链里（这一批的立项理由）：在此之前，check-quota-write-paths /
# check-quota-shutdown-order / check-probe-gate-labels / check-std-includes 四道守卫
# 都带 --self-test，但**没有一个在门禁链里跑过**——它们全是手动点的。后果是：
# 某道守卫被改坏、或它的判据退化成恒真，**CI 永远发现不了**，而门禁日志看上去全绿。
# 上一轮审计的原话是「契约和守卫都声称覆盖了某个失败模式，实际实现没有」，这里
# 是同一类问题的最后一层：连「守卫是否还在断言」都没有机制保证。
#
# ---- 关于「self-test 红的语义与真守卫红不同，混在一起输出难读」----
# 上一批修复者据此**不把 self-test 接进链**，理由是「每次门禁跑 mktemp + 树构造，
# 且两种红的语义不同」。**这条理由本批推翻**，因为它把「难读」当成了硬阻塞：
#   · 「难读」有确定解法 —— 就是下面这个分段标题。两种红天然分成两段：
#       上面的「── xxx 守卫 ──」= **你的代码坏了**（守卫正常工作，报告违规）；
#       本段的「── 守卫自检 ──」  = **守卫自己坏了**（判据退化/变异没生效）。
#     读日志的人扫一眼标题就知道该去修代码还是该修守卫，比现在「根本没有这段、
#     出了问题也没人知道」强得多。
#   · 另一半理由（跑 mktemp + 树构造）不是缺点而是**必要成本**：自检的全部价值
#     就在于「造一棵坏树，确认它真的会红」。不造树就没有这条保证，成本省下来
#     的那点时间买不到任何东西。
#   · 真正的代价是**耗时**（本段约 40~60 秒：造树 + 多轮全量扫描）。换来的是
#     「守卫被改坏」从「无人知晓」变成「CI 立刻红」。这笔账是划算的。
#
# 本段**故意放在所有真守卫之后**：代码真的有问题时，先报代码问题，别让守卫自检
# 的噪音盖住它。
echo "── 守卫自检（验证上面各守卫自己没坏，非你的代码问题）──"
SELF_TEST_FAIL=0
for _guard in check-module-isolation \
               check-quota-write-paths \
               check-quota-shutdown-order \
               check-probe-gate-labels \
               check-probe-skip-gate \
               check-std-includes; do
  if ! bash "scripts/${_guard}.sh" --self-test; then
    echo "❌ 守卫自检失败：scripts/${_guard}.sh --self-test —— 这是**守卫自己**坏了（判据退化成恒真或变异没生效），不是代码问题" >&2
    SELF_TEST_FAIL=1
  fi
done
if ! bash scripts/check-eui-neo-version.sh --eui-dir "$EUI_DIR" --self-test; then
  echo "❌ 守卫自检失败：scripts/check-eui-neo-version.sh --self-test —— 同样是守卫自己坏了" >&2
  SELF_TEST_FAIL=1
fi
if [ "$SELF_TEST_FAIL" -ne 0 ]; then
  echo "❌ 守卫自检未通过（见上方逐条；修完守卫本身再重跑门禁）" >&2
  exit 1
fi
echo "✅ 守卫自检全过（7 道守卫的自检均好树绿 / 坏树红 —— 判据仍有效）"

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
