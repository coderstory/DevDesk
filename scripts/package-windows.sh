#!/usr/bin/env bash
# DevDesk Windows 打包脚本：Release 构建 → 清理/重拷 assets → ISCC 产出用户级安装包
# 用法：bash scripts/package-windows.sh
#
# 环境变量：
#   ISCC          ISCC.exe 路径（最高优先级；装上 Inno Setup 后一般不需要设）
#   EUI_NEO_DIR   EUI-NEO 目录（worktree 里自动反推主工作树，一般不需要设）
#   DEVDESK_JOBS  并发编译单元数（默认 3）
#
# 本脚本只管出安装包，不跑测试、不启动 app；构建 / 测试 / 启动走 build-test-run.sh。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

# ISCC 定位放在最前：缺 ISCC 时不必先白跑一遍约 10 分钟的 Release 构建才报错。
ISCC_BIN=""
if [ -n "${ISCC:-}" ]; then
  # ISCC 接受绝对路径与 PATH 上的命令名两种形态
  if [ -f "$ISCC" ] || command -v "$ISCC" >/dev/null 2>&1; then
    ISCC_BIN="$ISCC"
  else
    echo "⚠ \$ISCC 已设置但找不到（${ISCC}），继续按默认路径探测" >&2
  fi
fi
if [ -z "$ISCC_BIN" ]; then
  for cand in "${LOCALAPPDATA:-}/Programs/Inno Setup 6/ISCC.exe" \
              "/c/Program Files (x86)/Inno Setup 6/ISCC.exe"; do
    if [ -f "$cand" ]; then ISCC_BIN="$cand"; break; fi
  done
fi
if [ -z "$ISCC_BIN" ]; then
  echo "❌ 找不到 Inno Setup 6 的 ISCC.exe（三段式探测全部落空）" >&2
  echo "   探测顺序：\$ISCC 环境变量 → %LOCALAPPDATA%\\Programs\\Inno Setup 6\\ISCC.exe → C:\\Program Files (x86)\\Inno Setup 6\\ISCC.exe" >&2
  echo "" >&2
  echo "   安装方式（任选其一）：" >&2
  echo "    1) winget install --id JRSoftware.InnoSetup --scope user" >&2
  echo "       ⚠ 公司网络下该源的后端（github.com）可能被拦（症状 InternetOpenUrl() failed 0x80072efd），" >&2
  echo "         需换到能访问 GitHub 的网络执行；内网机器建议走第 2 条。" >&2
  echo "    2) 手工下载官方安装包（https://jrsoftware.org/isdl.php 的 innosetup-6.7.3.exe）后在本机安装" >&2
  echo "    3) 已有 ISCC.exe 时显式指定：ISCC=\"<ISCC.exe 路径>\" bash scripts/package-windows.sh" >&2
  echo "" >&2
  echo "   装好后本脚本的三段式探测会自动找到，无需改脚本。" >&2
  exit 1
fi
echo "── ISCC: $ISCC_BIN ──"

# 杀旧进程：运行中的 DevDesk.exe 会锁住 build-release 目录。
# 用 dash 风格 -F：Git Bash 会把 /F 当成路径转换。
taskkill -F -IM DevDesk.exe 2>/dev/null || true

# 工具链 PATH 不在系统 PATH（Windows 工具链集中装在 /d/software 下），必须在这里补。
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

# 并行度封顶：多 worktree 并发构建时避免互拖。
export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"

# EUI-NEO 路径解析：worktree 里没有同级 ../EUI-NEO，需反推主工作树。
# 优先级：EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO（主工作树）> git 主工作树同级。
# 下面这整段与 build-test-run.sh 的同名解析重复：抽公共库时两处必须一起改，只改一边会让两条
# 门禁/打包路径解析出不同的 EUI-NEO。
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
  echo "   可用 EUI_NEO_DIR=<EUI-NEO 路径> bash scripts/package-windows.sh 显式指定" >&2
  exit 1
fi
echo "── EUI-NEO: $EUI_DIR ──"

# Release 构建：独立目录 build-release/，不污染开发用的 Debug 树。
if [ ! -f build-release/build.ninja ]; then
  echo "── 首次 configure（build-release/ 未初始化）──"
  CONF_OUT=$(cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DEUI_NEO_DIR="$EUI_DIR" \
    -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 \
    -DZLIB_LIBRARY=C:/msys64/mingw64/lib/libz.dll.a \
    -DZLIB_INCLUDE_DIR="$EUI_DIR/3rd/zlib-1.3.1" 2>&1) || {
    echo "$CONF_OUT" | tail -30
    echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
  echo "✅ configure 完成"
fi
echo "── 构建（build-release，Release）──"
BUILD_OUT=$(cmake --build build-release 2>&1) || {
  echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
  echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
echo "$BUILD_OUT" | tail -2
echo "✅ 构建成功"

# assets 必须先 rm -rf 再从源头重拷（EUI-NEO/assets + 仓库 assets 根）：cmake -E copy_directory
# 只增不删，不清空就会把已删除的资源留在 build-release/assets 里。
EXE_DIR_POSIX="$(cd build-release && pwd)"
ASSETS_DIR_POSIX="$EXE_DIR_POSIX/assets"
rm -rf "$ASSETS_DIR_POSIX"
cmake -E copy_directory "$EUI_DIR/assets" "$ASSETS_DIR_POSIX"
# DevDesk/assets 根文件（托盘图标 / splash 着色器 / 模板），09-30 后才加入，与 pets 同源幂等
cmake -E copy_directory "$PROJECT_DIR/assets" "$ASSETS_DIR_POSIX"

# H 批（UAT2 2026-10-20）资产瘦身：上游 copy_directory 是全量拷，DevDesk 运行时实际只用
# EUI 的两字体（JingNanJunJunTi + Font Awesome，EUI core/render/text.cpp 默认 UI/icon 字体）+
# icon.png（EUI 默认窗口图标 dsl_app.h iconPathValue，兼托盘兜底）+ icon.ico（devdesk.iss
# SetupIconFile）。以下零引用资产不进安装包（DevDesk src 与 EUI 运行时均无加载点，2026-10-20
# 全量 grep 核实）：music/（示例音频）、shaders/shadertoy/（示例着色器，D-11 后 splash 不再用）、
# YouSheBiaoTiHei-2.ttf / NotoSansSC-VF.ttf（未用字体；NotoSansSC 此前已被 devdesk.iss
# Excludes 挡在包外）、mona-loading-default.gif / Hard drive-rafiki.svg（EUI demo 插画）、
# icon.svg（仅 EUI 测试/示例引用，Windows 窗口图标走 icon.png 的 stbi 路径）。
# 只清打包产物目录，不动 EUI-NEO 仓库源 assets（跨仓库改动走 patch 流程，超出本批范围）。
rm -rf "$ASSETS_DIR_POSIX/music" \
       "$ASSETS_DIR_POSIX/shaders/shadertoy" \
       "$ASSETS_DIR_POSIX/YouSheBiaoTiHei-2.ttf" \
       "$ASSETS_DIR_POSIX/NotoSansSC-VF.ttf" \
       "$ASSETS_DIR_POSIX/mona-loading-default.gif" \
       "$ASSETS_DIR_POSIX/Hard drive-rafiki.svg" \
       "$ASSETS_DIR_POSIX/icon.svg"

# 断言 ①（H 批改写）：零引用资产不得混入打包目录（防回归——瘦身清单被人弄丢 /
# 上游 EUI assets 又新增死重量时此处拦截）。原"shaders/shadertoy 非空"断言随
# shadertoy 废弃（D-11）删除；字体/着色器/图标就位由断言 ③④ 继续钉住。
for gone in "music" "shaders/shadertoy" "YouSheBiaoTiHei-2.ttf" "NotoSansSC-VF.ttf" \
            "mona-loading-default.gif" "Hard drive-rafiki.svg" "icon.svg"; do
  if [ -e "$ASSETS_DIR_POSIX/$gone" ]; then
    echo "❌ 断言失败：零引用资产仍被拷进打包目录：$gone" >&2
    exit 1
  fi
done
# 断言 ③：DevDesk/assets 根文件（托盘图标 / splash 着色器），09-30 新增，缺则安装版托盘空图标 + splash 回退
if [ ! -f "$ASSETS_DIR_POSIX/shaders/splash.frag" ]; then
  echo "❌ 断言失败：$ASSETS_DIR_POSIX/shaders/splash.frag 缺失" >&2
  exit 1
fi
# 断言 ④：托盘图标，缺则安装版任务栏空图标
if [ ! -f "$ASSETS_DIR_POSIX/tray-icon-win.ico" ]; then
  echo "❌ 断言失败：$ASSETS_DIR_POSIX/tray-icon-win.ico 缺失" >&2
  exit 1
fi
echo "✅ assets 就位（两字体 + icon.png/ico + splash.frag + 托盘图标；零引用资产已排除）"

# ISCC 是原生 Windows 程序：路径要 Windows 形态（Git Bash 的 /d/... 形态可能被拒）
if command -v cygpath >/dev/null 2>&1; then
  EXE_DIR_WIN="$(cygpath -w "$EXE_DIR_POSIX")"
else
  EXE_DIR_WIN="$EXE_DIR_POSIX"
fi
ASSETS_DIR_WIN="$EXE_DIR_WIN\\assets"

# 版本号单一源 = 顶层 CMakeLists.txt 的 project(DevDesk VERSION ...)，configure 后落到
# CMakeCache.txt 的 CMAKE_PROJECT_VERSION。不要在别处再写一份版本号。
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' build-release/CMakeCache.txt | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 build-release/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# MSYS2_ARG_CONV_EXCL 按前缀排除 Git Bash 的参数路径转换（否则 /DExeDir=... 可能被改写）
mkdir -p installer/windows/out
ISCC_OUT=$(MSYS2_ARG_CONV_EXCL='/D' "$ISCC_BIN" \
  "/DExeDir=$EXE_DIR_WIN" \
  "/DAssetsDir=$ASSETS_DIR_WIN" \
  "/DMyAppVersion=$APP_VERSION" \
  "installer/windows/devdesk.iss" 2>&1) || {
  echo "$ISCC_OUT" | tail -30
  echo "❌ ISCC 编译失败（上方为错误摘要）"; exit 1; }
echo "$ISCC_OUT" | tail -3

# .iss 里的 OutputDir=out 相对 .iss 所在目录解析，不是相对仓库根。
SETUP_EXE="installer/windows/out/DevDesk-$APP_VERSION-setup.exe"
if [ ! -f "$SETUP_EXE" ]; then
  echo "❌ 未按预期产出 ${SETUP_EXE}（检查 .iss 的 OutputDir 解析位置）" >&2
  exit 1
fi
echo "✅ 安装包：$PROJECT_DIR/${SETUP_EXE}（$(du -h "$SETUP_EXE" | cut -f1)）"
