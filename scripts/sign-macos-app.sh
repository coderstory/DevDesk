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
#   嵌套代码确实存在且必须签——Darwin 27 的 codesign 把 Contents/MacOS/ 下的**所有常规
#   文件**（含 assets 数据文件：字体/shader/图标）按嵌套代码对待，外层签完
#   --verify --strict 会逐个报 "unsigned subcomponent"。处理方式是下面这段显式预签：
#   逐个 ad-hoc 签 MacOS/ 下的常规文件，再签 bundle 外层——与既定口径一致
#   （引入嵌套代码就逐个显式签，不用 --deep 一把梭；--deep 与否和 hardened runtime
#   是两件独立的事，禁的始终是 --options runtime：library validation 会拒 bundle 外
#   的 Homebrew OpenSSL，见上一段）。
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

echo "── 预签嵌套代码（Contents/MacOS 下常规文件）──"
# MacOS/ 下除主可执行外的一切常规文件（assets 字体/shader/图标等）都是嵌套代码，
# 不签则外层 --verify --strict 报 "unsigned subcomponent"。逐个显式 ad-hoc 签。
# 主可执行必须留给外层 bundle 签名去签：直接签它会让 codesign 走 bundle 感知重封，
# 此时其余嵌套文件若还没签完就报 "code object is not signed at all"（实测踩过）。
# 只扫 MacOS/：Resources 下的文件按资源哈希封进 CodeResources，不需要也不应该签。
EXE_NAME="$(plutil -extract CFBundleExecutable raw "$APP/Contents/Info.plist" 2>/dev/null || echo DevDesk)"
NESTED_SIGN_FAILED=0
while IFS= read -r -d '' nested; do
    [ "$(basename "$nested")" = "$EXE_NAME" ] && continue
    if ! codesign --force --sign - "$nested" >/dev/null; then
        echo "❌ 嵌套代码预签失败：$nested" >&2
        NESTED_SIGN_FAILED=1
    fi
done < <(find "$APP/Contents/MacOS" -type f -print0)
if [ "$NESTED_SIGN_FAILED" != "0" ]; then
    echo "❌ 存在预签失败的嵌套文件，中止（外层签名必然过不了严格校验）" >&2
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
