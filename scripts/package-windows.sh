#!/usr/bin/env bash
# DevDesk Windows 打包脚本：Release 构建 → 清理/重拷 assets → ISCC 产出用户级安装包
# 设计依据：docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §3.1 / §3.2
# 用法：bash scripts/package-windows.sh
# 环境变量：
#   ISCC          ISCC.exe 路径（最高优先级；装上 Inno Setup 后一般不需要设）
#   EUI_NEO_DIR   EUI-NEO 目录（worktree 里自动反推主工作树，一般不需要设）
#   DEVDESK_JOBS  并发编译单元数（默认 3，与 build-test-run.sh 同款封顶）
# 分工：本脚本只管"出安装包"，不跑测试、不启动 app；构建/测试/启动走 build-test-run.sh（D-8）
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

# ---- 1. ISCC 定位（提到最前：缺 ISCC 时不必先白跑约 10 分钟 Release 构建才报错）----
ISCC_BIN=""
if [ -n "${ISCC:-}" ]; then
  # 支持绝对路径与 PATH 上的命令名两种形态
  if [ -f "$ISCC" ] || command -v "$ISCC" >/dev/null 2>&1; then
    ISCC_BIN="$ISCC"
  else
    echo "⚠ \$ISCC 已设置但找不到（$ISCC），继续按默认路径探测" >&2
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

# ---- 2. 杀旧进程（运行中的 DevDesk.exe 会锁住 build-release 目录）----
# 注意用 dash 风格 -F：Git Bash 会把 /F 当路径转换（先例见 build-test-run.sh:81）
taskkill -F -IM DevDesk.exe 2>/dev/null || true

# ---- 3. 工具链 PATH + EUI-NEO 定位 ----
# 工具链 PATH（CLAUDE.md 指定的 Windows 工具链，不在系统 PATH）
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

# 并行度封顶：多 worktree 并发构建时避免互拖（同 build-test-run.sh 的理由）
export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"

# EUI-NEO 路径解析：worktree 里没有同级 ../EUI-NEO，需反推主工作树
# 优先级：EUI_NEO_DIR 环境变量 > 同级 ../EUI-NEO（主工作树）> git 主工作树同级
# ⚠ 后续项（spec §3.2 步骤 3 已登记）：下面这段与 build-test-run.sh:55-76 重复 22 行，
#   计划抽成 scripts/lib/eui-neo-dir.sh 供两处共用；本期先复制，避免与在飞会话争
#   build-test-run.sh 的写权。抽取时两处一起改，别只改一边。
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

# ---- 4. Release 构建（独立目录 build-release/，不污染 Debug 树；D-9）----
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

# ---- 5. assets 清理与重拷（cmake -E copy_directory 只增不删，会残留已删资源）----
# 只从源头重拷（EUI-NEO/assets + 仓库 assets/pets），不引旧构建目录：
# build-release/assets 可能是上次构建的旧资源，且可能残留已删除的美术皮肤。
EXE_DIR_POSIX="$(cd build-release && pwd)"
ASSETS_DIR_POSIX="$EXE_DIR_POSIX/assets"
rm -rf "$ASSETS_DIR_POSIX"
cmake -E copy_directory "$EUI_DIR/assets" "$ASSETS_DIR_POSIX"
cmake -E copy_directory "$PROJECT_DIR/assets/pets" "$ASSETS_DIR_POSIX/pets"

# 断言 ①：字体/音乐/着色器确实被拷进来（shaders/shadertoy 非空）
if [ -z "$(ls -A "$ASSETS_DIR_POSIX/shaders/shadertoy" 2>/dev/null)" ]; then
  echo "❌ 断言失败：$ASSETS_DIR_POSIX/shaders/shadertoy 为空（assets 未正确拷贝？）" >&2
  exit 1
fi
# 断言 ②：assets/pets 下只有 README.md（防已删美术皮肤作为残留被打进安装包）
if [ ! -f "$ASSETS_DIR_POSIX/pets/README.md" ]; then
  echo "❌ 断言失败：$ASSETS_DIR_POSIX/pets/README.md 缺失" >&2
  exit 1
fi
EXTRA_PETS="$(ls -A "$ASSETS_DIR_POSIX/pets" | grep -v '^README\.md$' || true)"
if [ -n "$EXTRA_PETS" ]; then
  echo "❌ 断言失败：assets/pets 下有多余项（应仅 README.md）：" >&2
  echo "$EXTRA_PETS" >&2
  exit 1
fi
echo "✅ assets 就位（shaders/shadertoy 非空；pets 仅 README.md）"

# ISCC 是原生 Windows 程序：路径要 Windows 形态（Git Bash 的 /d/... 形态可能被拒）
if command -v cygpath >/dev/null 2>&1; then
  EXE_DIR_WIN="$(cygpath -w "$EXE_DIR_POSIX")"
else
  EXE_DIR_WIN="$EXE_DIR_POSIX"
fi
ASSETS_DIR_WIN="$EXE_DIR_WIN\\assets"

# ---- 6. 版本号取值（单一源 = CMakeLists.txt 的 project(DevDesk VERSION ...)）----
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' build-release/CMakeCache.txt | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 build-release/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# ---- 7. 调 ISCC 产出安装包 ----
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

# ---- 8. 产物检查与打印（OutputDir=out 相对 .iss 所在目录）----
SETUP_EXE="installer/windows/out/DevDesk-$APP_VERSION-setup.exe"
if [ ! -f "$SETUP_EXE" ]; then
  echo "❌ 未按预期产出 $SETUP_EXE（检查 .iss 的 OutputDir 解析位置）" >&2
  exit 1
fi
echo "✅ 安装包：$PROJECT_DIR/$SETUP_EXE（$(du -h "$SETUP_EXE" | cut -f1)）"
