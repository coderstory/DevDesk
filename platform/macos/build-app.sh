#!/bin/bash
# platform/macos/build-app.sh
# 把编译后的 build/DevDesk 二进制打包成 DevDesk.app bundle
# 并注册到 LaunchServices（让 Finder 立即看到文件关联）
#
# 开关：
#   DEVDESK_SKIP_LSREGISTER=1 —— 跳过 LaunchServices 注册（打包/DMG 流程用，
#   默认不设 = 保持既有行为）

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

# 复制 assets 到 Contents/MacOS/assets（与 Contents/MacOS/DevDesk 同级，
# 不是 Contents/Resources）——EUI-NEO 的 text.cpp / image_source.cpp 与桌宠
# 皮肤都按「可执行文件旁 assets/」解析资源，放 Resources 下运行时找不到
#（字体缺失会退化、皮肤发现不到）。源是当次构建目录的 assets（EUI-NEO 与
# DevDesk 各自 POST_BUILD 拷进去的），不从源码树直接拷
if [ -d "$BUILD_DIR/assets" ]; then
    cp -R "$BUILD_DIR/assets" "$APP_DIR/Contents/MacOS/assets"
    if [ ! -d "$APP_DIR/Contents/MacOS/assets" ]; then
        echo "❌ assets 拷贝失败：$BUILD_DIR/assets → Contents/MacOS/assets" >&2
        exit 1
    fi
    echo "✅ 资源: $APP_DIR/Contents/MacOS/assets"
else
    echo "⚠️  未找到 $BUILD_DIR/assets，跳过资源拷贝（先构建 DevDesk 目标生成 assets）" >&2
fi

# 注册到 LaunchServices（让 Finder 立即看到文件关联）
# 打包/DMG 流程置 DEVDESK_SKIP_LSREGISTER=1 跳过：lsregister 会改写构建机的
# 用户 LaunchServices 库，且注册的通常是临时路径/已删的 .app，属于污染
if [ "${DEVDESK_SKIP_LSREGISTER:-0}" = "1" ]; then
    echo "⏭️  跳过 LaunchServices 注册（DEVDESK_SKIP_LSREGISTER=1）"
else
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
        -f "$APP_DIR"
    echo "✅ 注册到 LaunchServices 完成"
fi

echo "✅ App bundle: $APP_DIR"
echo ""
echo "测试：在 Finder 右击 .md 文件 → '打开方式' → 应能看到 DevDesk"
