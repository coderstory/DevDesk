"""脱敏核心：云端抽取外发前的最后清洗层。

契约（feasibility §5 风险 8 的实现前提）：
- 泄漏 0 优先于误杀：拿不准的形态一律脱敏（fail-closed）。
- 所有检测在等长 shadow 文本上做（全角字符 1:1 映射半角），命中区间直接映射回原文，
  输出只替换命中区间，其余字节不动。
- 反重构警告：不要把 allowlist 当脱敏开关；allowlist 只豁免占位/回环值，
  新增豁免项必须同步补进 redteam.json 并说明理由。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

PLACEHOLDER_TEMPLATE = "[REDACTED:{category}]"

# 泄漏 0 优先：误杀宁可多，不放过。新增检测器时选不小于既有同类优先级。
_PRIO_PRIVATE_KEY = 12
_PRIO_KV_SECRET = 10
_PRIO_BEARER = 9
_PRIO_VENDOR_KEY = 8
_PRIO_JWT = 7
_PRIO_EMAIL = 6
_PRIO_URL_PARAM = 5
_PRIO_ID_CARD = 4
_PRIO_PHONE = 3
_PRIO_IPV4 = 2

# 占位/回环值豁免（形如敏感但公认无泄漏的值）。
_PHONE_ALLOWLIST = {"13800000000"}  # 文档通用假号码
_IPV4_ALLOWLIST = {"127.0.0.1", "0.0.0.0", "255.255.255.255"}

# shadow 归一化：必须 1:1（一个全角字符 -> 一个半角字符），否则区间映射失效。
_SHADOW_TABLE = {
    "０": "0", "１": "1", "２": "2", "３": "3", "４": "4",
    "５": "5", "６": "6", "７": "7", "８": "8", "９": "9",
    "Ａ": "A", "Ｂ": "B", "Ｃ": "C", "Ｄ": "D", "Ｅ": "E", "Ｆ": "F",
    "Ｇ": "G", "Ｈ": "H", "Ｉ": "I", "Ｊ": "J", "Ｋ": "K", "Ｌ": "L",
    "Ｍ": "M", "Ｎ": "N", "Ｏ": "O", "Ｐ": "P", "Ｑ": "Q", "Ｒ": "R",
    "Ｓ": "S", "Ｔ": "T", "Ｕ": "U", "Ｖ": "V", "Ｗ": "W", "Ｘ": "X",
    "Ｙ": "Y", "Ｚ": "Z",
    "ａ": "a", "ｂ": "b", "ｃ": "c", "ｄ": "d", "ｅ": "e", "ｆ": "f",
    "ｇ": "g", "ｈ": "h", "ｉ": "i", "ｊ": "j", "ｋ": "k", "ｌ": "l",
    "ｍ": "m", "ｎ": "n", "ｏ": "o", "ｐ": "p", "ｑ": "q", "ｒ": "r",
    "ｓ": "s", "ｔ": "t", "ｕ": "u", "ｖ": "v", "ｗ": "w", "ｘ": "x",
    "ｙ": "y", "ｚ": "z",
    "－": "-", "‐": "-", "–": "-", "—": "-",
    "＋": "+", "　": " ", "：": ":", "＝": "=", "＠": "@", "．": ".",
}

_ID18_WEIGHTS = (7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2)
_ID18_CHECK = "10X98765432"

_IPV4_RE = re.compile(
    r"(?<![\d.])"
    r"(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}"
    r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
    r"(?![\d.])"
)

_PHONE_RE = re.compile(
    # 11 位 = 1[3-9] + 1 + 4 + 4；分隔符只允许出现在 3/7/8 位之后
    r"(?<!\d)(?:\+?86[- ]?)?1[3-9]\d[- ]?\d{4}[- ]?\d{4}(?!\d)"
)

_ID18_RE = re.compile(
    # 年份不限定 19/20 开头：1880 等早年出生段同样合法，泄漏 0 优先
    r"(?<![0-9Xx])\d{6}[- ]?\d{4}"
    r"(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])[- ]?\d{3}[0-9Xx](?![0-9Xx])"
)

_ID15_RE = re.compile(
    r"(?<!\d)\d{6}\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}(?!\d)"
)

_EMAIL_RE = re.compile(
    r"(?<![\w.+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+"
    r"(?![\w.+-])"
)

_JWT_RE = re.compile(
    r"(?<![\w.-])eyJ[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]{4,}(?![\w.-])"
)

_VENDOR_KEY_RE = re.compile(
    r"(?<![A-Za-z0-9])"
    r"(?:A(?:KIA|SIA|IDA)[0-9A-Z]{16}"          # AWS access key id
    r"|AIza[0-9A-Za-z_\-]{35}"                   # Google API key
    r"|gh[pousr]_[A-Za-z0-9]{36,255}"            # GitHub classic token
    r"|github_pat_[A-Za-z0-9_]{22,255}"          # GitHub fine-grained token
    r"|sk-(?:proj-)?[A-Za-z0-9_\-]{20,}"         # OpenAI 风格
    r"|xox[abprs]-[A-Za-z0-9\-]{10,})"           # Slack
    r"(?![A-Za-z0-9])"
)

_BEARER_RE = re.compile(r"\bBearer\s+([A-Za-z0-9\-._~+/]+=*)")

_KV_SECRET_RE = re.compile(
    r"(?<![A-Za-z0-9$])("  # 允许前置 '-'（x-api-key: 这类 header 形态）
    r"api[_-]?key|apikey|secret[_-]?key|access[_-]?key(?:[_-]?id)?|access[_-]?token"
    r"|auth[_-]?token|session[_-]?token|client[_-]?secret|app[_-]?secret|authorization"
    r"|secret|token|password|passwd|pwd"
    r")\b\s*[:=]\s*(?:bearer|basic|digest|token)?\s*[\"']?"
    r"([A-Za-z0-9@#$%^*+/_\-.~=]{4,})",  # 不含 '&'：URL 里它是参数分隔符，与 url_param 边界一致
    re.IGNORECASE,
)

_URL_PARAM_RE = re.compile(
    r"[?&]("
    r"token|access[_-]?token|auth[_-]?token|api[_-]?key|apikey|secret"
    r"|signature|sign|password|passwd|pwd|key"
    r")=([^\s&\"'<>]{1,})",
    re.IGNORECASE,
)

_PRIVATE_KEY_RE = re.compile(
    r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----",
    re.DOTALL,
)
_PRIVATE_KEY_BEGIN_RE = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")


@dataclass(frozen=True)
class Hit:
    """一次命中。preview 是掩码形式，绝不携带原始敏感值原文。"""

    category: str
    start: int
    end: int
    detail: str
    preview: str


@dataclass
class SanitizeResult:
    text: str
    hits: list[Hit] = field(default_factory=list)

    @property
    def categories(self) -> list[str]:
        return sorted({hit.category for hit in self.hits})


def _shadow(text: str) -> str:
    return "".join(_SHADOW_TABLE.get(ch, ch) for ch in text)


def _masked_preview(value: str) -> str:
    # 不变量：preview 绝不携带原文片段（含首尾字符）。等长遮蔽不额外泄信息——
    # 长度本就由 Hit.start/end 承载；禁止改回"保留首尾 N 字符"的部分掩码。
    return "*" * len(value)


def _id18_detail(raw: str) -> str:
    digits = _digits_only(raw[: len(raw) - 1])  # 末位是校验位，可能是 X
    total = sum(int(d) * w for d, w in zip(digits, _ID18_WEIGHTS))
    expect = _ID18_CHECK[total % 11]
    ok = raw[-1].upper() == expect
    return "id18:checksum_ok" if ok else "id18:checksum_bad"


def _digits_only(value: str) -> str:
    return re.sub(r"\D", "", value)


def _iter_candidates(shadow: str):
    key_blocks = [(m.start(), m.end()) for m in _PRIVATE_KEY_RE.finditer(shadow)]
    for start, end in key_blocks:
        yield (_PRIO_PRIVATE_KEY, start, end, "secret", "private_key")
    for m in _PRIVATE_KEY_BEGIN_RE.finditer(shadow):
        # 无 END 的裸 BEGIN 行：脱到文本尾（fail-closed）。整块已覆盖的不重复报。
        if not any(s <= m.start() < e for s, e in key_blocks):
            yield (_PRIO_PRIVATE_KEY, m.start(), len(shadow), "secret", "private_key_no_end")
    for m in _KV_SECRET_RE.finditer(shadow):
        yield (_PRIO_KV_SECRET, m.start(), m.end(), "secret", "kv_credential")
    for m in _BEARER_RE.finditer(shadow):
        value = m.group(1)
        # 至少含一位数字或长度 >=20，避免把 "Bearer authentication" 这类普通语句切走。
        if any(c.isdigit() for c in value) or len(value) >= 20:
            yield (_PRIO_BEARER, m.start(), m.end(), "secret", "bearer")
    for m in _VENDOR_KEY_RE.finditer(shadow):
        token = m.group(0)
        if token.startswith(("AKIA", "ASIA", "AIDA")):
            detail = "aws_access_key"
        elif token.startswith("AIza"):
            detail = "google_api_key"
        elif token.startswith("github_pat"):
            detail = "github_fine_grained"
        elif token.startswith("gh"):
            detail = "github_token"
        elif token.startswith("sk-"):
            detail = "openai_style"
        else:
            detail = "slack_token"
        yield (_PRIO_VENDOR_KEY, m.start(), m.end(), "secret", detail)
    for m in _JWT_RE.finditer(shadow):
        yield (_PRIO_JWT, m.start(), m.end(), "secret", "jwt")
    for m in _EMAIL_RE.finditer(shadow):
        yield (_PRIO_EMAIL, m.start(), m.end(), "email", "email")
    for m in _URL_PARAM_RE.finditer(shadow):
        yield (_PRIO_URL_PARAM, m.start(), m.end(), "url_param", "url_query")
    for m in _ID18_RE.finditer(shadow):
        yield (_PRIO_ID_CARD, m.start(), m.end(), "id_card", _id18_detail(m.group(0)))
    for m in _ID15_RE.finditer(shadow):
        yield (_PRIO_ID_CARD, m.start(), m.end(), "id_card", "id15")
    for m in _PHONE_RE.finditer(shadow):
        digits = _digits_only(m.group(0))
        if digits in _PHONE_ALLOWLIST or (digits.startswith("86") and digits[2:] in _PHONE_ALLOWLIST):
            continue
        yield (_PRIO_PHONE, m.start(), m.end(), "phone", "cn_mobile")
    for m in _IPV4_RE.finditer(shadow):
        if m.group(0) in _IPV4_ALLOWLIST:
            continue
        yield (_PRIO_IPV4, m.start(), m.end(), "ipv4", "ipv4")


def scan(text: str) -> list[Hit]:
    """返回按位置排序、互不重叠的命中清单（不修改文本）。"""
    shadow = _shadow(text)
    candidates = list(_iter_candidates(shadow))
    candidates.sort(key=lambda c: (-c[0], c[1], -(c[2] - c[1])))
    accepted: list[tuple[int, int, str, str, str]] = []
    for prio, start, end, category, detail in candidates:
        if any(not (end <= s or start >= e) for s, e, *_ in accepted):
            continue
        accepted.append((start, end, category, detail, shadow[start:end]))
    return [
        Hit(
            category=category,
            start=start,
            end=end,
            detail=detail,
            preview=_masked_preview(value),
        )
        for start, end, category, detail, value in sorted(accepted)
    ]


def sanitize(text: str) -> SanitizeResult:
    """外发前唯一入口：返回脱敏文本与命中清单。幂等：对输出再跑一遍不再变化。

    fail-closed 是传播式：任一检测器异常/畸形结果必须沿本函数上抛、中断外发；
    禁止在调用点包兜底 except 吞异常（否则静默变 fail-open，护栏测试钉住此契约）。
    """
    hits = scan(text)
    out = text
    for hit in sorted(hits, key=lambda h: -h.start):
        out = out[: hit.start] + PLACEHOLDER_TEMPLATE.format(category=hit.category) + out[hit.end :]
    return SanitizeResult(text=out, hits=hits)
