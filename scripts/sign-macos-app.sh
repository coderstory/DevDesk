#!/usr/bin/env bash
# scripts/sign-macos-app.sh
# 给**组装完成**的 macOS .app bundle 打 ad-hoc 签名，并立即做严格校验。
#
# 用法：bash scripts/sign-macos-app.sh <path-to-.app>
#   参数缺失 / 路径不存在 / 不是 .app → 非零退出并打印中文错误。
#
# 为什么必须是组装流程的**最后一步**：
#   codesign 会把整个 bundle（主可执行 + Info.plist + Resources + MacOS/assets …）的哈希
#   封进 _CodeSignature/CodeResources。签名之后再写任何文件（Info.plist 版本号、图标、
#   补拷 assets、改 rpath）都会让封条对不上，`codesign --verify --strict` 随即失败。
#   调用方（package-macos.sh / build-test-run.sh --install）在 build-app.sh 之后还会补拷
#   assets，所以它们必须在自己写完后再调一次本脚本。
#
# 为什么是 ad-hoc（--sign -）：
#   本机开发 / 个人分发没有 Apple Developer ID；ad-hoc 已足够让 macOS 把 app 当成有
#   签名的 bundle（UNUserNotificationCenter 等要求 bundle 身份的能力依赖这一点）。
#   CFBundleIdentifier 仍是 com.devdesk.app（platform/macos/Info.plist），这里用
#   --identifier 固定签名标识，避免 codesign 按可执行文件名推导成 "DevDesk"。
#
# 为什么**不加** --options runtime：
#   hardened runtime 的 library validation 会拒绝加载 bundle 外、未签名的动态库；
#   本机 Homebrew OpenSSL（/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib 等）正是这种。
#   加上会让 app 启动即崩，且与「ad-hoc 本机自用」的目标无关。
#
# 为什么**不加** --deep：
#   --deep 会递归给嵌套的框架/插件套用同一签名参数，既慢又容易签错（Apple 已不推荐）。
#   当前 bundle 没有嵌套框架，只签主可执行 + 封资源即可；以后真的引入框架时，应逐个
#   显式签名，而不是用 --deep 一把梭。
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
用法：bash scripts/sign-macos-app.sh <path-to-.app>
  给已组装完成的 .app bundle 打 ad-hoc 签名并校验（必须在所有资源写入之后调用）。
USAGE
}

# 参数个数：多给（比如误传了 flag）也拒绝，避免把参数当路径签错东西
if [ "$#" -ne 1 ]; then
  echo "❌ 参数错误：需要且只需要一个 .app 路径（实际收到 $# 个参数）" >&2
  usage
  exit 2
fi

# 去掉末尾斜杠，让 "Foo.app/" 也按 .app 识别
APP="${1%/}"

if [ ! -e "$APP" ]; then
  echo "❌ 路径不存在：$APP" >&2
  echo "   先跑 platform/macos/build-app.sh 组装 bundle，再来签名。" >&2
  exit 1
fi

case "$APP" in
  *.app) : ;;
  *)
    echo "❌ 不是 .app bundle：$APP" >&2
    echo "   本脚本签整个 bundle，不接受裸二进制或其他目录。" >&2
    exit 1
    ;;
esac

if [ ! -d "$APP" ]; then
  echo "❌ 不是目录（.app 必须是 bundle 目录）：$APP" >&2
  exit 1
fi

# 非 macOS 必须出声失败：静默跳过会让调用方误以为产物已签名
if [ "$(uname -s)" != "Darwin" ]; then
  echo "❌ 本脚本只能在 macOS 上运行（当前平台：$(uname -s)）" >&2
  echo "   codesign 是 macOS 专属工具；非 macOS 不产出 .app，也不会假装签名成功。" >&2
  exit 1
fi

if ! command -v codesign >/dev/null 2>&1; then
  echo "❌ 找不到 codesign（应随 Xcode Command Line Tools 提供）" >&2
  echo "   安装：xcode-select --install" >&2
  exit 1
fi

echo "── 签名（ad-hoc）: $APP ──"
# --force：幂等 —— 重复签同一个 app 时替换旧签名而不是报 "already signed"
# --sign -：ad-hoc 身份，不依赖钥匙串里的证书
# --identifier：固定为 Info.plist 里的 CFBundleIdentifier，供系统按 bundle 身份识别
if ! codesign --force --sign - --identifier com.devdesk.app "$APP"; then
  echo "❌ 签名失败：$APP" >&2
  echo "   → 检查 .app 是否完整（Contents/MacOS 下应有可执行文件）、磁盘是否可写。" >&2
  exit 1
fi

echo "── 校验签名（codesign --verify --strict）──"
if ! codesign --verify --strict --verbose=4 "$APP"; then
  echo "❌ 签名校验失败：$APP" >&2
  echo "   → 最常见原因：签名之后 bundle 又被写入（Info.plist / 图标 / assets / rpath）。" >&2
  echo "     本脚本必须是组装流程的最后一步；把调用点移到所有写操作之后，重新组装再签。" >&2
  echo "   → 排查：codesign -dv --verbose=4 \"$APP\" 查看 Identifier / Sealed Resources 是否符合预期。" >&2
  exit 1
fi

echo "✅ 签名有效：${APP}（Identifier=com.devdesk.app，ad-hoc）"
