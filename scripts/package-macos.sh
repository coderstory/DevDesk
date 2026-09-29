#!/usr/bin/env bash
# DevDesk macOS 打包脚本：构建 → DevDesk.app → 补拷 assets → create-dmg 出 DMG
# 设计依据：docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §4.1 / §4.3
# 用法：bash scripts/package-macos.sh
# 开关：
#   DEVDESK_BUILD_DIR=<dir> —— 构建目录，默认 build/（默认值 = 既有行为）。
#     打 Release 包时指向另一棵树，就不会把开发中的 Debug build/ 重配成 Release：
#       DEVDESK_BUILD_DIR=build-release bash scripts/package-macos.sh
#     可给绝对路径，也可给相对仓库根的路径（脚本会绝对化后透传给 build-app.sh）。
# 说明：
#   * DMG 只能在 macOS 上构建（spec §4.4），非 Darwin 直接报错退出；
#   * 打包流程复用「构建目录」（默认 build/，可用 DEVDESK_BUILD_DIR 换树；
#     platform/macos/build-app.sh 从 <构建目录>/DevDesk 取二进制）；
#   * 图标（DevDesk.icns / 卷图标）与 Finder 真机表现需在 Mac 上另做，见 spec §4.4。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

# ---- 0.0 构建目录（DEVDESK_BUILD_DIR，默认 build/）----
# 相对路径按仓库根解析 → 后面全部按绝对路径用，传给 build-app.sh 也无歧义
BUILD_DIR="${DEVDESK_BUILD_DIR:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
  /*) : ;;
  *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;;
esac

# ---- 0. 平台守卫 ----
case "$(uname -s)" in
  Darwin) : ;;
  *)
    echo "❌ 本脚本只能在 macOS 上运行（DMG 只能在 Mac 上构建，见 spec §4.4）" >&2
    echo "   Windows 打包请用：bash scripts/package-windows.sh" >&2
    exit 1 ;;
esac

# ---- 1. 依赖检查（放在构建前：缺 create-dmg 时不必先白跑一遍构建）----
if ! command -v create-dmg >/dev/null 2>&1; then
  echo "❌ 找不到 create-dmg（安装：brew install create-dmg）" >&2
  exit 1
fi

# ---- 2. 构建（Release）----
# 打包要的是发布档；configure 每次显式带 -DCMAKE_BUILD_TYPE=Release，
# 避免开发机上的 Debug 树被直接打成包（切换会触发一次全量重编）。
# 要让开发用的 Debug build/ 原样保留 → 用 DEVDESK_BUILD_DIR 指到另一棵树（如 build-release）。
echo "── configure（Release，构建目录 $BUILD_DIR）──"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
echo "── 构建 ──"
cmake --build "$BUILD_DIR" --parallel "${DEVDESK_JOBS:-3}" 2>&1 | tail -5

# ---- 3. 生成 .app bundle ----
# build-app.sh 会先 rm -rf 再重建 DevDesk.app（.app 内不会残留上次的文件）。
# ⚠ spec §4.3：build-app.sh 里的 lsregister -f 会写用户 LaunchServices 库，打包流程中不应触发；
#   M4 会给它加 DEVDESK_SKIP_LSREGISTER 开关，这里先把开关置上（开关落地前是无害的环境变量）。
DEVDESK_BUILD_DIR="$BUILD_DIR" DEVDESK_SKIP_LSREGISTER=1 bash platform/macos/build-app.sh
APP_DIR="$BUILD_DIR/DevDesk.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
if [ ! -x "$MACOS_DIR/DevDesk" ]; then
  echo "❌ .app 内没有可执行文件：$MACOS_DIR/DevDesk（build-app.sh 没跑成功？）" >&2
  exit 1
fi

# ---- 4. 补拷 assets 进 .app（与 exe 同级：Contents/MacOS/assets）----
# EUI-NEO 按"exe 旁"找资源，Contents/Resources/assets 无效（spec §4.3）；
# 来源是构建树的 assets/（EUI-NEO 字体/音乐/shaders/icon + 仓库 assets/pets 的 POST_BUILD 合集）。
rm -rf "$MACOS_DIR/assets"
cmake -E copy_directory "$BUILD_DIR/assets" "$MACOS_DIR/assets"
# 与 Windows 侧 §3.2 步骤 5 同款防残留断言（copy_directory 只增不删）
if [ -z "$(ls -A "$MACOS_DIR/assets/shaders/shadertoy" 2>/dev/null)" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/shaders/shadertoy 为空" >&2
  exit 1
fi
if [ ! -f "$MACOS_DIR/assets/pets/README.md" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/pets/README.md 缺失" >&2
  exit 1
fi
# 断言 ②：每个皮肤子目录含 pet.json —— 与 Windows 侧 scripts/package-windows.sh:120-131
# 同款口径（2026-09-28 那边已从「仅 README.md」改过来）：皮肤包随仓库分发、数量会增减，
# 写死「pets 下只能有 README.md」在真实构建树上必失败（DevDesk POST_BUILD 会把 6+ 个
# 皮肤拷进 build/assets/pets），故改为逐目录校验 pet.json。
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

# ---- 5. 版本号取值（单一源 = CMakeLists.txt 的 project(DevDesk VERSION ...)）----
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 $BUILD_DIR/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# ---- 6. 暂存目录（create-dmg 的 <SourceFolder>：App + 去隔离脚本 + 安装说明）----
# 放 installer/macos/out/（已 gitignore，与 DMG 输出同处）而**不**跟随 DEVDESK_BUILD_DIR：
# 暂存内容全部来自已组装好的 $APP_DIR 与源码树的 installer/macos/，对构建树没有语义依赖；
# 放构建树反而会往「可能正被别的进程使用的」Debug build/ 里塞一份 Release .app 拷贝。
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

# ---- 7. create-dmg（位置参数必须补全：<OutputName.dmg> <SourceFolder>；flags 即窗口规格，§4.1）----
mkdir -p installer/macos/out
DMG="installer/macos/out/DevDesk-$APP_VERSION.dmg"
# --overwrite 必加：否则第二次运行会失败
# 每个进 DMG 的条目都必须给 --icon 坐标——没坐标的项会被 Finder 甩出可视区
# （2026-09-30 真机实证：去隔离脚本在窗口里不可见，用户以为缺失）
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

# ---- 8. 产物 ----
if [ ! -f "$DMG" ]; then
  echo "❌ 未按预期产出 $DMG" >&2
  exit 1
fi
echo "✅ DMG：$PROJECT_DIR/$DMG（$(du -h "$DMG" | cut -f1)）"
