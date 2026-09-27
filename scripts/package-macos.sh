#!/usr/bin/env bash
# DevDesk macOS 打包脚本：构建 → DevDesk.app → 补拷 assets → create-dmg 出 DMG
# 设计依据：docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §4.1 / §4.3
# 用法：bash scripts/package-macos.sh
# 说明：
#   * DMG 只能在 macOS 上构建（spec §4.4），非 Darwin 直接报错退出；
#   * 打包流程复用主构建树 build/（platform/macos/build-app.sh 固定从 build/DevDesk 取二进制）；
#   * 图标（DevDesk.icns / 卷图标）与 Finder 真机表现需在 Mac 上另做，见 spec §4.4。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

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
echo "── configure（Release）──"
cmake -B build -DCMAKE_BUILD_TYPE=Release
echo "── 构建 ──"
cmake --build build --parallel "${DEVDESK_JOBS:-3}" 2>&1 | tail -5

# ---- 3. 生成 .app bundle ----
# build-app.sh 会先 rm -rf 再重建 DevDesk.app（.app 内不会残留上次的文件）。
# ⚠ spec §4.3：build-app.sh 里的 lsregister -f 会写用户 LaunchServices 库，打包流程中不应触发；
#   M4 会给它加 DEVDESK_SKIP_LSREGISTER 开关，这里先把开关置上（开关落地前是无害的环境变量）。
DEVDESK_SKIP_LSREGISTER=1 bash platform/macos/build-app.sh
APP_DIR="$PROJECT_DIR/build/DevDesk.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
if [ ! -x "$MACOS_DIR/DevDesk" ]; then
  echo "❌ .app 内没有可执行文件：$MACOS_DIR/DevDesk（build-app.sh 没跑成功？）" >&2
  exit 1
fi

# ---- 4. 补拷 assets 进 .app（与 exe 同级：Contents/MacOS/assets）----
# EUI-NEO 按"exe 旁"找资源，Contents/Resources/assets 无效（spec §4.3）；
# 来源是构建树的 assets/（EUI-NEO 字体/音乐/shaders/icon + 仓库 assets/pets 的 POST_BUILD 合集）。
rm -rf "$MACOS_DIR/assets"
cmake -E copy_directory "$PROJECT_DIR/build/assets" "$MACOS_DIR/assets"
# 与 Windows 侧 §3.2 步骤 5 同款防残留断言（copy_directory 只增不删）
if [ -z "$(ls -A "$MACOS_DIR/assets/shaders/shadertoy" 2>/dev/null)" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/shaders/shadertoy 为空" >&2
  exit 1
fi
if [ ! -f "$MACOS_DIR/assets/pets/README.md" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/pets/README.md 缺失" >&2
  exit 1
fi
EXTRA_PETS="$(ls -A "$MACOS_DIR/assets/pets" | grep -v '^README\.md$' || true)"
if [ -n "$EXTRA_PETS" ]; then
  echo "❌ 断言失败：assets/pets 下有多余项（应仅 README.md）：" >&2
  echo "$EXTRA_PETS" >&2
  exit 1
fi
echo "✅ assets 就位（Contents/MacOS/assets）"

# ---- 5. 版本号取值（单一源 = CMakeLists.txt 的 project(DevDesk VERSION ...)）----
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' build/CMakeCache.txt | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 build/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# ---- 6. 暂存目录（create-dmg 的 <SourceFolder>：App + 去隔离脚本 + 安装说明）----
STAGE="build/dmg-stage"
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
create-dmg \
  --overwrite \
  --volname "DevDesk" \
  --window-pos 200 120 \
  --window-size 700 440 \
  --icon-size 120 \
  --icon "DevDesk.app" 190 210 \
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
