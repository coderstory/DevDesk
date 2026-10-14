#!/usr/bin/env bash
# DevDesk macOS 打包脚本：构建 → DevDesk.app → 补拷 assets → create-dmg 出 DMG
# 用法：bash scripts/package-macos.sh
#
# 环境变量：
#   DEVDESK_BUILD_DIR  构建目录，默认 build/。可给绝对路径，也可给相对仓库根的路径（脚本会
#                      绝对化后透传给 platform/macos/build-app.sh）。指到 build-release 这类
#                      另一棵树上打包，才能保住开发用的 Debug build/ 不被重配成 Release。
#   DEVDESK_JOBS       并发编译单元数，默认 3。
#
# DMG 只能在 macOS 上构建，非 Darwin 直接报错退出（Windows 打包走 scripts/package-windows.sh）。
# 图标（DevDesk.icns / 卷图标）与 Finder 真机表现需在 Mac 上另做。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

# 构建目录：相对路径按仓库根解析 → 后面全部按绝对路径用，传给 build-app.sh 也无歧义
BUILD_DIR="${DEVDESK_BUILD_DIR:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
  /*) : ;;
  *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;;
esac

case "$(uname -s)" in
  Darwin) : ;;
  *)
    echo "❌ 本脚本只能在 macOS 上运行（DMG 只能在 Mac 上构建，见 spec §4.4）" >&2
    echo "   Windows 打包请用：bash scripts/package-windows.sh" >&2
    exit 1 ;;
esac

# 依赖检查放在构建前：缺 create-dmg 时不必先白跑一遍 Release 构建
if ! command -v create-dmg >/dev/null 2>&1; then
  echo "❌ 找不到 create-dmg（安装：brew install create-dmg）" >&2
  exit 1
fi

# -D 会覆盖已 configure 的树，所以打包必须显式写 -DCMAKE_BUILD_TYPE=Release —— 直接对着开发用的
# Debug build/ 跑会把整棵树重配成 Release 并触发全量重编；要保住 Debug 树就用 DEVDESK_BUILD_DIR
# 指到另一棵树（如 build-release）。
echo "── configure（Release，构建目录 $BUILD_DIR）──"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
echo "── 构建 ──"
cmake --build "$BUILD_DIR" --parallel "${DEVDESK_JOBS:-3}" 2>&1 | tail -5

# build-app.sh 会先 rm -rf 再重建 DevDesk.app（.app 内不会残留上次的文件）。
# DEVDESK_SKIP_LSREGISTER=1 必须置上：build-app.sh 里的 lsregister -f 会写用户的 LaunchServices 库，
# 打包流程不该留下这种副作用。
DEVDESK_BUILD_DIR="$BUILD_DIR" DEVDESK_SKIP_LSREGISTER=1 bash platform/macos/build-app.sh
APP_DIR="$BUILD_DIR/DevDesk.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
if [ ! -x "$MACOS_DIR/DevDesk" ]; then
  echo "❌ .app 内没有可执行文件：$MACOS_DIR/DevDesk（build-app.sh 没跑成功？）" >&2
  exit 1
fi

# 补拷 assets 进 .app，必须与 exe 同级（Contents/MacOS/assets）：EUI-NEO 按「exe 旁」找资源，
# 放进 Contents/Resources/assets 不生效。来源是构建树的 assets/（EUI-NEO 字体/音乐/shaders/icon）。
rm -rf "$MACOS_DIR/assets"
cmake -E copy_directory "$BUILD_DIR/assets" "$MACOS_DIR/assets"
# 防残留断言：cmake -E copy_directory 只增不删，所以上面必须先 rm -rf。与 package-windows.sh 同款。
if [ -z "$(ls -A "$MACOS_DIR/assets/shaders/shadertoy" 2>/dev/null)" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/shaders/shadertoy 为空" >&2
  exit 1
fi
echo "✅ assets 就位（Contents/MacOS/assets）"

# 版本号单一源 = 顶层 CMakeLists.txt 的 project(DevDesk VERSION ...)，configure 后落到
# CMakeCache.txt 的 CMAKE_PROJECT_VERSION。不要在别处再写一份版本号。
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 $BUILD_DIR/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# 暂存目录 = create-dmg 的 <SourceFolder>（App + 去隔离脚本 + 安装说明），放
# installer/macos/out/（已 gitignore，与 DMG 输出同处），不跟随 DEVDESK_BUILD_DIR：暂存内容全部
# 来自已组装好的 $APP_DIR 与源码树的 installer/macos/，对构建树没有语义依赖；放进构建树等于往
# 可能正被别的进程使用的 Debug build/ 里塞一份 Release .app 拷贝。
STAGE="installer/macos/out/dmg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP_DIR" "$STAGE/DevDesk.app"
cp "installer/macos/首次运行（去隔离）.command" "$STAGE/"
cp "installer/macos/安装说明.txt" "$STAGE/"
# 本仓不保留可执行位（git ls-files -s scripts/ 全 100644）→ 暂存时补 chmod 并断言，
# 否则 DMG 里双击 .command 无效
chmod 755 "$STAGE/首次运行（去隔离）.command"
if [ ! -x "$STAGE/首次运行（去隔离）.command" ]; then
  echo "❌ 去隔离脚本缺少可执行位" >&2
  exit 1
fi

# create-dmg 的位置参数必须补全 <OutputName.dmg> <SourceFolder>；每个 flag 直接就是窗口几何，
# 漏掉一个不报错，只会给出一个悄悄错位的窗口。
mkdir -p installer/macos/out
DMG="installer/macos/out/DevDesk-$APP_VERSION.dmg"
# --overwrite 必加：否则第二次运行会失败
# 每个进 DMG 的条目都必须给 --icon 坐标：没坐标的项会被 Finder 甩出可视区，用户看不到会以为文件缺失
create-dmg \
  --overwrite \
  --volname "DevDesk" \
  --window-pos 200 120 \
  --window-size 700 440 \
  --icon-size 120 \
  --icon "DevDesk.app" 190 210 \
  --icon "首次运行（去隔离）.command" 110 68 \
  --icon "安装说明.txt" 590 68 \
  --app-drop-link 510 210 \
  --hide-extension "DevDesk.app" \
  --background "installer/macos/background.png" \
  --no-internet-enable \
  "$DMG" \
  "$STAGE"

if [ ! -f "$DMG" ]; then
  echo "❌ 未按预期产出 $DMG" >&2
  exit 1
fi
echo "✅ DMG：$PROJECT_DIR/$DMG（$(du -h "$DMG" | cut -f1)）"
