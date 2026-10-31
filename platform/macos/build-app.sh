#!/bin/bash
# platform/macos/build-app.sh
# 把编译后的 <构建目录>/DevDesk 二进制打包成 DevDesk.app bundle
# 并注册到 LaunchServices（让 Finder 立即看到文件关联）
#
# 开关：
#   DEVDESK_BUILD_DIR=<dir> —— 构建目录，默认 <仓库根>/build（默认值 = 既有行为）。
#   有了它就能打 Release 包而不动开发用的 Debug build/：
#     DEVDESK_BUILD_DIR=/abs/path/to/build-release bash platform/macos/build-app.sh
#   相对路径按**调用时的 cwd** 解析（与 `cmake -B <dir>` 同一语义）。
#   scripts/package-macos.sh 把它绝对化后透传（见该脚本 §2 注释）。
#   DEVDESK_SKIP_LSREGISTER=1 —— 跳过 LaunchServices 注册（打包/DMG 流程用，
#   默认不设 = 保持既有行为）
#   DEVDESK_SKIP_SIGN=1 —— 跳过本脚本末尾的签名步骤。只给「组装后还会补拷资源、需要自己
#   在最后统一签名」的调用方用（当前是 scripts/build-test-run.sh --install，为了保住它
#   签名前的 sha256 防陈旧断言）；默认不设 = 组装完就签。
#
# 产出：$BUILD_DIR/DevDesk.app，其中
#   * Info.plist 的 CFBundleVersion / CFBundleShortVersionString 由构建目录
#     CMakeCache.txt 的 CMAKE_PROJECT_VERSION 在打包时替换（模板里的 0.1.0 是占位值，
#     仓库模板本身不改）；
#   * Contents/Resources/DevDesk.icns 为 App 图标（见下方「图标」段）。
#
# ⚠️ 本脚本是 .app bundle 组装的**唯一入口**：常规构建（scripts/build-test-run.sh）只重建
#    <构建目录>/DevDesk 裸可执行；2026-10-31（债-19）起对**已存在**的 bundle 由
#    build-test-run.sh 构建成功段自动重组（存在才维护）；不调本脚本就下载不到新的 bundle。
#    2026-09-29 事故即由此而来——bundle 停在旧二进制，而「编译 → cp -R .app /Applications/」
#    照旧把旧壳装了过去。安装 / 打包路径都必须调本脚本重组，不要在外面临时手搓 cp 组装。
#    调用方：scripts/package-macos.sh §3（打 DMG）、scripts/build-test-run.sh --install（装到
#    /Applications，装前会做 sha256 防陈旧断言）、scripts/build-test-run.sh 构建成功段
#    （bundle 维护，DEVDESK_SKIP_LSREGISTER=1、默认签名）。

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
BUILD_DIR="${DEVDESK_BUILD_DIR:-$PROJECT_DIR/build}"
APP_DIR="$BUILD_DIR/DevDesk.app"

# 清理
rm -rf "$APP_DIR"

# 创建 app bundle 结构
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# 复制二进制
cp "$BUILD_DIR/DevDesk" "$APP_DIR/Contents/MacOS/DevDesk"
chmod +x "$APP_DIR/Contents/MacOS/DevDesk"

# 复制 Info.plist（模板里的版本键是占位值，紧接着按当次构建版本替换；模板不改）
cp "$SCRIPT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
PLIST="$APP_DIR/Contents/Info.plist"

# 版本号：单一源 = CMakeLists.txt 的 project(DevDesk VERSION ...)（由 VERSION 文件经
# bump_version 目标自增），落在构建目录 CMakeCache.txt 的 CMAKE_PROJECT_VERSION。
# 写的是**打包产物**的 Info.plist，仓库模板保持 0.1.0 占位。
# 读不到只告警不失败：打包不应被版本读取问题阻断，但绝不静默放过。
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -1 | tr -d '\r')"
if [ -n "$APP_VERSION" ] \
    && plutil -replace CFBundleVersion -string "$APP_VERSION" "$PLIST" \
    && plutil -replace CFBundleShortVersionString -string "$APP_VERSION" "$PLIST"; then
    echo "✅ 版本号: ${APP_VERSION}（已写入打包产物 Info.plist）"
else
    echo "⚠️  版本号未替换：读不到 $BUILD_DIR/CMakeCache.txt 的 CMAKE_PROJECT_VERSION，" >&2
    echo "    或 plutil 写入失败 —— .app 将报 Info.plist 里的占位值 0.1.0" >&2
fi

# 图标：产出 Contents/Resources/DevDesk.icns（Info.plist 的 CFBundleIconFile=DevDesk）。
# 两个来源，已入库的高质量产物优先：
#   * installer/macos/DevDesk.icns —— doc/scripts/gen_app_icns.sh 的交付物（从 1024 源图
#     sips 缩到 16…1024 各档，质量最好；本仓当前未入库，故默认走下面一条）；
#   * $BUILD_DIR/assets/icon.png —— 构建时 POST_BUILD 从 EUI-NEO assets 拷来的
#     icon.png，零手工步骤、每次打包必然可得；与 Windows 侧 Inno 拿 assets/icon.ico
#     当 SetupIconFile 是同一份素材，两平台图标自此同源。
# 生成链：doc/scripts/pad_icon_canvas.py 补画布留白（Pillow，见该脚本头部）→
# sips 缩到 10 档 → iconutil 打包。除 Pillow 外不依赖仓库外路径、不需要手工准备源图。
# 补留白那步失败只降级为「满幅图标 + 明确警告」，不阻断打包（图标属外观，不该拦部署）。
ICON_OUT="$APP_DIR/Contents/Resources/DevDesk.icns"
ICON_SRC_ICNS="$PROJECT_DIR/installer/macos/DevDesk.icns"
ICON_SRC_PNG="$BUILD_DIR/assets/icon.png"
ICON_PAD_HELPER="$PROJECT_DIR/doc/scripts/pad_icon_canvas.py"
ICON_PYTHON="${DEVDESK_PYTHON:-python3}"
if [ -f "$ICON_SRC_ICNS" ]; then
    cp "$ICON_SRC_ICNS" "$ICON_OUT"
    echo "✅ 图标: ${ICON_SRC_ICNS}（已入库 .icns）"
elif [ -f "$ICON_SRC_PNG" ] && command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    # 目录名必须以 .iconset 结尾，否则 iconutil 报 "Invalid Iconset." 拒绝打包
    ICONSET_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/DevDesk-appicon.XXXXXX")"
    ICONSET_DIR="$ICONSET_ROOT/DevDesk.iconset"
    mkdir -p "$ICONSET_DIR"
    ICON_FAIL=0
    # 缩放前先补画布留白：把源图等比缩到最长边 82% 居中放进 1024 透明画布，
    # 10 档全部从这张母图派生（理由见 doc/scripts/pad_icon_canvas.py 头部）。
    # sips 的 --padColor 只吃 6 位十六进制 = 只能补不透明色，补不了透明留白，故走 Pillow。
    ICON_MASTER="$ICONSET_ROOT/icon-master-1024.png"
    ICON_RESIZE_SRC="$ICON_SRC_PNG"
    if [ -f "$ICON_PAD_HELPER" ] && command -v "$ICON_PYTHON" >/dev/null 2>&1 \
       && "$ICON_PYTHON" "$ICON_PAD_HELPER" "$ICON_SRC_PNG" "$ICON_MASTER" >/dev/null 2>&1; then
        ICON_RESIZE_SRC="$ICON_MASTER"
        echo "ℹ️  图标已补画布留白（内容 82%，Dock 里与其他应用等大）"
    else
        echo "⚠️  图标未补画布留白（缺 python3/Pillow 或 ${ICON_PAD_HELPER}）——" >&2
        echo "    Dock 里 DevDesk 图标会比别的应用显得大；装上 Pillow 后重跑本脚本即可" >&2
    fi
    # 标准 iconset 10 档（16/32/128/256/512 各一张 + 各自 @2x）；源图不足 1024 时大档
    # 是放大插值（清晰度略降），但胜在零手工步骤
    while read -r name px; do
        [ -z "$name" ] && continue
        sips -z "$px" "$px" "$ICON_RESIZE_SRC" --out "$ICONSET_DIR/$name" >/dev/null 2>&1 || ICON_FAIL=1
    done <<'ICONSET_SIZES'
icon_16x16.png 16
icon_16x16@2x.png 32
icon_32x32.png 32
icon_32x32@2x.png 64
icon_128x128.png 128
icon_128x128@2x.png 256
icon_256x256.png 256
icon_256x256@2x.png 512
icon_512x512.png 512
icon_512x512@2x.png 1024
ICONSET_SIZES
    if [ "$ICON_FAIL" = "0" ] && iconutil -c icns -o "$ICON_OUT" "$ICONSET_DIR" 2>/dev/null; then
        echo "✅ 图标: ${ICON_OUT}（由 $ICON_SRC_PNG 现生成）"
    else
        rm -f "$ICON_OUT"
        echo "⚠️  图标生成失败（sips/iconutil），.app 将显示通用图标" >&2
    fi
    rm -rf "$ICONSET_ROOT"
else
    echo "⚠️  找不到图标源（$ICON_SRC_ICNS / ${ICON_SRC_PNG}），.app 将显示通用图标" >&2
fi

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

# 签名：bundle 组装的**最后一次写入**。到这里二进制、Info.plist 版本号、图标、assets 都已
# 就位，签完就不能再动 bundle 里任何文件（否则封条失效）。注意：package-macos.sh 和
# --install 路径在本脚本之后还会补拷 assets，它们必须在自己的写操作完成后再签一次。
if [ "${DEVDESK_SKIP_SIGN:-0}" = "1" ]; then
    echo "⏭️  跳过签名（DEVDESK_SKIP_SIGN=1）——调用方必须在所有资源写完后自行签名"
else
    if ! bash "$PROJECT_DIR/scripts/sign-macos-app.sh" "$APP_DIR"; then
        echo "❌ ad-hoc 签名失败：$APP_DIR" >&2
        exit 1
    fi
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
