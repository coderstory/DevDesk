#!/usr/bin/env bash
# scripts/check-no-secret-storage.sh
# 系统机密存储 + 硬编码密钥串禁令（need#490 老板令 2026-11-24）：
#   ① 永久禁止用系统提供的机密存储（macOS Keychain / Windows DPAPI+Credential Manager /
#      Linux libsecret+Secret Service）保存任何应用机密；
#   ② 永久禁止在源码 / 文档 / demo 中出现真实或占位密钥串；验证器密钥只经 otpauth 链接
#      一次性交给验证器（且仅在内存/密文中流转），不落 UI 文本。
#
# 判据 = grep 于 src/ tests/（本守卫只扫代码树；docs/ 与 demo 由 code review 与 CI 外的
# 人工门把关——docs 存在大量历史文档，静态红无法区分叙述与可用密钥）。
# 零白名单：命中即 exit 1 并打印 文件:行:命中行。
#
# 为什么**不**拦 `credential_protection` / `secrets_store` 这两个项目内名字：它们是
# 本仓自研的明文-DB 薄封装（src/platform/credential_protection_*、src/core/secrets_store），
# 不是系统机密存储，且 main.cpp / module_settings / schema.h 合法引用。把它们写进正则会让
# 干净 HEAD 直接红，守卫随即被关掉（比没有守卫更糟）。本守卫拦的是**系统 API 符号本身**。
#
# 本脚本全程绝对路径、不读 ${PWD}、脚本内不 cd（对齐 check-module-isolation.sh）。
# 退出码：0 通过 / 1 违规（清单到 stdout）/ 2 环境错（仓库根或受检目录缺失）。
# --root DIR 供反证测试；--self-test 造临时坏树验证判据未退化成恒真。

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SELF_TEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --root) [ $# -ge 2 ] || { echo "❌ secret 禁令守卫：--root 缺参数" >&2; exit 2; }; ROOT="$2"; shift 2 ;;
    --self-test) SELF_TEST=1; shift ;;
    *) echo "❌ secret 禁令守卫：未知参数 $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "${ROOT}" 2>/dev/null && pwd)" || { echo "❌ secret 禁令守卫：仓库根不存在" >&2; exit 2; }

# OS 机密存储 API 符号（大小写敏感，除 keychain 单独跑大小写不敏感）
STORAGE_RE='(SecItemAdd|SecItemUpdate|SecItemCopyMatching|SecKeychain|kSecClass|Security/Security\.h|CryptProtectData|CryptUnprotectData|CredRead[AW]?|CredWrite[AW]?|wincred\.h|libsecret|secret-tool|org\.freedesktop\.secrets)'
# 硬编码密钥串：secret= 后跟 8+ base32/hex、已知演示 TOTP 密钥、PEM 私钥头
SECRET_RE='(secret=[A-Z2-7]{8,}|secret=[0-9a-fA-F]{16,}|JBSWY3DP|-----BEGIN [A-Z ]*PRIVATE KEY-----[A-Za-z0-9+/=]{32,})'
# 存量豁免（仅限「只读迁移」代码，不是新使用）：Windows 平台层为老用户库保留 DPAPI 解密
# 回迁（credential_protection_win.cpp 不再写 DPAPI，只读旧 blob 转明文后由 secrets_store 接管）。
# 豁免面刻意收窄到这三个文件；任何其它文件出现系统 API 仍红。
ALLOW_RE='(src/platform/credential_protection(_win)?\.(h|cpp)|tests/test_platform/test_credential_protection\.cpp):[0-9]+:'

scan_root() { # $1 = 待扫目录（仓库根），stdout=命中清单，返回 0 干净 / 1 命中 / 2 环境错
  local root="$1"
  if [ ! -d "${root}/src" ] || [ ! -d "${root}/tests" ]; then
    return 2
  fi
  local hits=""
  hits="$(grep -rInE --exclude-dir=build --exclude-dir=build-release --exclude-dir=.git \
          --exclude-dir=node_modules --exclude-dir=third_party "${STORAGE_RE}" \
          "${root}/src" "${root}/tests" 2>/dev/null || true)"
  local hits_kc=""
  hits_kc="$(grep -rIniE --exclude-dir=build --exclude-dir=build-release --exclude-dir=.git \
          --exclude-dir=node_modules --exclude-dir=third_party 'keychain' \
          "${root}/src" "${root}/tests" 2>/dev/null || true)"
  local hits_sec=""
  hits_sec="$(grep -rInE --exclude-dir=build --exclude-dir=build-release --exclude-dir=.git \
          --exclude-dir=node_modules --exclude-dir=third_party "${SECRET_RE}" \
          "${root}/src" "${root}/tests" 2>/dev/null || true)"
  local all=""
  for h in "$hits" "$hits_kc" "$hits_sec"; do
    [ -n "$h" ] && all="${all}${h}"$'\n'
  done
  all="$(printf '%s' "$all" | sed '/^$/d' | sed "s|^${root}/||" | grep -vE "${ALLOW_RE}" | sort -u || true)"
  if [ -n "$all" ]; then
    printf '%s\n' "$all"
    return 1
  fi
  return 0
}

if [ "$SELF_TEST" = "1" ]; then
  TMP_DIR="$(mktemp -d 2>/dev/null)" || { echo "❌ 自检：mktemp 失败" >&2; exit 2; }
  trap 'rm -rf "${TMP_DIR}"' EXIT
  FAIL=0
  # 坏树 1：系统机密存储 API
  mkdir -p "${TMP_DIR}/bad/src" "${TMP_DIR}/bad/tests"
  printf 'void f() { OSStatus s = SecItemAdd(&q, 0); }\n' > "${TMP_DIR}/bad/src/bad.cpp"
  if scan_root "${TMP_DIR}/bad" >/dev/null; then echo "❌ 自检：植入 SecItemAdd 未变红" >&2; FAIL=1; fi
  # 坏树 2：硬编码 TOTP 密钥串
  mkdir -p "${TMP_DIR}/bad2/src" "${TMP_DIR}/bad2/tests"
  printf "var u = 'otpauth://totp/X?secret=JBSWY3DPEHPK3PXP';\n" > "${TMP_DIR}/bad2/src/demo.js"
  if scan_root "${TMP_DIR}/bad2" >/dev/null; then echo "❌ 自检：植入 secret= 串未变红" >&2; FAIL=1; fi
  # 干净树：必须绿
  mkdir -p "${TMP_DIR}/ok/src" "${TMP_DIR}/ok/tests"
  printf 'int main() { return 0; }\n' > "${TMP_DIR}/ok/src/ok.cpp"
  if ! scan_root "${TMP_DIR}/ok" >/dev/null; then echo "❌ 自检：干净树被误报" >&2; FAIL=1; fi
  # 环境错：受检目录缺失必须 2，不静默绿
  mkdir -p "${TMP_DIR}/empty"
  scan_root "${TMP_DIR}/empty" >/dev/null; rc=$?
  if [ "$rc" -ne 2 ]; then echo "❌ 自检：缺 src/ tests/ 时应退出 2，实际 ${rc}" >&2; FAIL=1; fi
  if [ "$FAIL" -ne 0 ]; then exit 1; fi
  echo "✅ secret 禁令守卫自检通过（植入即红 / 干净即绿 / 环境错=2）"
  exit 0
fi

HITS=""
scan_root "${ROOT}" > "${ROOT}/.secret_guard_hits.tmp" 2>/dev/null
rc=$?
HITS="$(cat "${ROOT}/.secret_guard_hits.tmp" 2>/dev/null || true)"
rm -f "${ROOT}/.secret_guard_hits.tmp" 2>/dev/null || true
if [ "$rc" -eq 2 ]; then
  echo "❌ secret 禁令守卫：仓库根定位失败或 src/ tests/ 缺失（${ROOT}）" >&2
  exit 2
fi
if [ "$rc" -ne 0 ] && [ -n "${HITS}" ]; then
  echo "❌ secret 禁令守卫：系统机密存储 / 硬编码密钥串命中（老板令 2026-11-24，永久禁止）："
  printf '%s\n' "${HITS}" | head -40
  echo "   修法：① 机密不得存进 Keychain / DPAPI / Credential Manager / libsecret，一律走 core::secrets_store（app_secrets 表）；"
  echo "         ② 源码 / demo 不写密钥串，验证器密钥只经 otpauth 链接一次性交给验证器。"
  exit 1
fi
echo "✅ secret 禁令守卫通过（src/ tests/ 零系统机密存储 API、零硬编码密钥串）"
exit 0
