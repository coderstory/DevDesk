#!/bin/bash
# DevDesk 首次运行（去隔离）
#
# 用法：把 DevDesk.app 拖进「应用程序」后，双击本文件（在「应用程序」同级或拖出去都行）。
# 作用：只做一件事 —— 去掉 DevDesk.app 上的 com.apple.quarantine（"已下载"）标记，
#       让 macOS 允许打开未签名的 DevDesk。
#
# 注意：本脚本**不做**任何 Gatekeeper 全局降级（不碰 spctl、不改系统安全设置），
#       只针对 DevDesk.app 这一个应用。
#
# 设计依据：docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §4.3
set -u

APP_NAME="DevDesk.app"

# 目标优先「应用程序」里的那个（正常安装路径），否则回退到本脚本同级目录的 DevDesk.app
TARGET="/Applications/$APP_NAME"
if [ ! -d "$TARGET" ]; then
  SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
  TARGET="$SELF_DIR/$APP_NAME"
fi

if [ ! -d "$TARGET" ]; then
  echo "❌ 没找到 $APP_NAME"
  echo ""
  echo "请先把 $APP_NAME 拖到「应用程序」文件夹；"
  echo "或者把本脚本和 $APP_NAME 放在同一个文件夹里，再双击本脚本。"
  echo ""
  read -r -p "按回车键关闭此窗口…" _
  exit 1
fi

echo "正在为下面这个应用移除隔离标记："
echo "  $TARGET"
echo ""
if xattr -dr com.apple.quarantine "$TARGET" 2>/dev/null; then
  echo "✅ 完成。现在到「应用程序」里双击 DevDesk 就能打开了。"
else
  echo "⚠ 移除隔离标记失败（可能是权限问题或文件被占用）。"
  echo "  仍然打不开时的替代办法："
  echo "    打开 DevDesk 时若被系统拦下 → 系统设置 → 隐私与安全性 → 点「仍要打开」。"
fi
echo ""
read -r -p "按回车键关闭此窗口…" _
