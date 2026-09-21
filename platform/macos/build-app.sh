#!/bin/bash
# platform/macos/build-app.sh
# 把编译后的 build/DevDesk 二进制打包成 DevDesk.app bundle
# 并注册到 LaunchServices（让 Finder 立即看到文件关联）

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
BUILD_DIR="$PROJECT_DIR/build"
APP_DIR="$BUILD_DIR/DevDesk.app"

# 清理
rm -rf "$APP_DIR"

# 创建 app bundle 结构
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# 复制二进制
cp "$BUILD_DIR/DevDesk" "$APP_DIR/Contents/MacOS/DevDesk"
chmod +x "$APP_DIR/Contents/MacOS/DevDesk"

# 复制 Info.plist
cp "$SCRIPT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"

# 注册到 LaunchServices（让 Finder 立即看到文件关联）
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f "$APP_DIR"

echo "✅ App bundle: $APP_DIR"
echo "✅ 注册到 LaunchServices 完成"
echo ""
echo "测试：在 Finder 右击 .md 文件 → '打开方式' → 应能看到 DevDesk"
