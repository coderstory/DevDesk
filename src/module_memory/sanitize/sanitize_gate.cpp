// src/module_memory/sanitize/sanitize_gate.cpp
// 六类检测器：std::regex 无 lookbehind 且 MinGW 实现质量不稳，全部手写确定性扫描器
//（逐码点 + 边界判定，语义与 core.py 母本的正则逐条对齐）。检测一律在 shadow 码点流上
// 做，命中区间经 Cp 记录映射回原文字节偏移。
#include "sanitize_gate.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace module::memory::sanitize {

namespace {

// ── 码点流：shadow 检测的统一载体 ──
struct Cp {
    std::uint32_t cp;  // 码点（shadow 归一后）
    std::size_t off;   // 原文字节偏移
    std::size_t len;   // 原文字节长度
};

// UTF-8 严格解码：拒绝截断/坏续字节/过长编码/代理区/超 10FFFF。
// 任何非法形态 → false（fail-closed 入口，绝不"跳过坏字节继续扫"）。
bool decodeUtf8(const std::string& s, std::vector<Cp>* out) {
    out->clear();
    out->reserve(s.size());
    const auto* b = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c0 = b[i];
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c0 < 0x80) {
            cp = c0;
            len = 1;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            len = 3;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            len = 4;
        } else {
            return false;
        }
        if (len > n - i) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((b[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b[i + k] & 0x3F);
        }
        static constexpr std::uint32_t kMinByLen[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMinByLen[len - 1] || cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // 代理区（UTF-8 禁用）
        out->push_back({cp, i, len});
        i += len;
    }
    return true;
}

// shadow 归一化：必须 1:1（一个码点 → 一个码点），否则区间映射失效（母本契约）。
std::uint32_t shadowOf(std::uint32_t cp) {
    if (cp >= 0xFF10 && cp <= 0xFF19) return '0' + (cp - 0xFF10);  // 全角数字
    if (cp >= 0xFF21 && cp <= 0xFF3A) return 'A' + (cp - 0xFF21);  // 全角大写
    if (cp >= 0xFF41 && cp <= 0xFF5A) return 'a' + (cp - 0xFF41);  // 全角小写
    switch (cp) {
        case 0xFF0D: return '-';  // －
        case 0x2010: return '-';  // ‐
        case 0x2013: return '-';  // –
        case 0x2014: return '-';  // —
        case 0xFF0B: return '+';  // ＋
        case 0x3000: return ' ';  // 全角空格
        case 0xFF1A: return ':';  // ：
        case 0xFF1D: return '=';  // ＝
        case 0xFF20: return '@';  // ＠
        case 0xFF0E: return '.';  // ．
        default: return cp;
    }
}

inline bool isDigit(std::uint32_t c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(std::uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool isAlnum(std::uint32_t c) { return isAlpha(c) || isDigit(c); }
inline bool isWord(std::uint32_t c) { return isAlnum(c) || c == '_'; }
inline std::uint32_t lower(std::uint32_t c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
inline bool isSpace(std::uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

// ASCII 字面量匹配；caseSensitive=false 时大小写无关
bool matchAscii(const std::vector<Cp>& t, std::size_t i, const char* lit,
                bool caseSensitive) {
    for (std::size_t k = 0; lit[k] != '\0'; ++k) {
        if (i + k >= t.size()) return false;
        const std::uint32_t a = t[i + k].cp;
        const std::uint32_t b = static_cast<unsigned char>(lit[k]);
        if (caseSensitive ? (a != b) : (lower(a) != lower(b))) return false;
    }
    return true;
}
std::size_t litLen(const char* lit) {
    std::size_t n = 0;
    while (lit[n] != '\0') ++n;
    return n;
}

// ── 候选与优先级（母本 _PRIO_*）──
struct Cand {
    int prio;
    std::size_t start;  // 码点下标
    std::size_t end;    // 码点下标（不含）
    const char* category;
    std::string detail;
};

constexpr int kPrioPrivateKey = 12;
constexpr int kPrioKvSecret = 10;
constexpr int kPrioBearer = 9;
constexpr int kPrioVendorKey = 8;
constexpr int kPrioJwt = 7;
constexpr int kPrioEmail = 6;
constexpr int kPrioUrlParam = 5;
constexpr int kPrioIdCard = 4;
constexpr int kPrioPhone = 3;
constexpr int kPrioIpv4 = 2;

// 占位/回环值豁免（形如敏感但公认无泄漏；新增须同步 redteam.json）
constexpr const char* kPhoneAllowlist[] = {"13800000000"};
constexpr const char* const kIpv4Allowlist[] = {"127.0.0.1", "0.0.0.0",
                                                "255.255.255.255"};

template <std::size_t N>
bool inList(const char* const (&list)[N], const std::string& v) {
    for (std::size_t i = 0; i < N; ++i) {
        if (v == list[i]) return true;
    }
    return false;
}

// 关键词表按序首个命中（表序 = 母本正则 alternation 序，先来先得）
bool matchKeyword(const std::vector<Cp>& t, std::size_t i,
                  const std::vector<const char*>& kws, std::size_t* lenOut) {
    for (const char* kw : kws) {
        if (matchAscii(t, i, kw, false)) {
            *lenOut = litLen(kw);
            return true;
        }
    }
    return false;
}

// secret kv 键名（母本 _KV_SECRET_RE 第一组；[_-]? 变体逐条展开）。
// 复合形式必须排在自身前缀之前：否则短形式命中后 \b 撞上下划线即死，
// 母本正则靠 alternation 序先试长形式（access_key_id 输入是实证）。
const std::vector<const char*>& kvKeywords() {
    static const std::vector<const char*> kws = {
        "secret_key",     "secret-key",
        "access_key_id",  "access-key-id", "access_key-id", "access-key_id",
        "access_key",     "access-key",
        "api_key",        "api-key",       "apikey",
        "access_token",   "access-token",
        "auth_token",     "auth-token",
        "session_token",  "session-token",
        "client_secret",  "client-secret",
        "app_secret",     "app-secret",
        "authorization",  "password",      "passwd",
        "secret",         "token",         "pwd"};
    return kws;
}

// url_param 参数名（母本 _URL_PARAM_RE；signature 必须排在 sign 前）
const std::vector<const char*>& urlParamKeywords() {
    static const std::vector<const char*> kws = {
        "token",         "access_token", "access-token", "auth_token",
        "auth-token",    "api_key",      "api-key",      "apikey",
        "secret",        "signature",    "sign",         "password",
        "passwd",        "pwd",          "key"};
    return kws;
}

// ── 各检测器：向 cands 追加候选 ──

// [A-Z ]* 贪心回溯：从 starEnd 向 starStart 回退找 lit 首个可匹配位（正则回溯语义）
std::size_t backtrackMatch(const std::vector<Cp>& t, std::size_t starStart,
                           std::size_t starEnd, const char* lit) {
    for (std::size_t p = starEnd + 1; p-- > starStart;) {
        if (matchAscii(t, p, lit, true)) return p;
    }
    return std::string::npos;
}

void scanPrivateKey(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    static constexpr const char* kBegin = "-----BEGIN ";
    // 头部：-----BEGIN [A-Z ]*PRIVATE KEY-----；体到 -----END …PRIVATE KEY-----
    for (std::size_t i = 0; i + litLen(kBegin) <= t.size(); ++i) {
        if (!matchAscii(t, i, kBegin, true)) continue;
        std::size_t j = i + litLen(kBegin);
        while (j < t.size() &&
               ((t[j].cp >= 'A' && t[j].cp <= 'Z') || t[j].cp == ' ')) {
            ++j;
        }
        const std::size_t head =
            backtrackMatch(t, i + litLen(kBegin), j, "PRIVATE KEY-----");
        if (head == std::string::npos) continue;
        const std::size_t headEnd = head + litLen("PRIVATE KEY-----");
        std::size_t end = t.size();
        bool closed = false;
        for (std::size_t k = headEnd; k + litLen("-----END ") <= t.size();
             ++k) {
            if (!matchAscii(t, k, "-----END ", true)) continue;
            std::size_t m = k + litLen("-----END ");
            const std::size_t mStar = m;
            while (m < t.size() &&
                   ((t[m].cp >= 'A' && t[m].cp <= 'Z') || t[m].cp == ' ')) {
                ++m;
            }
            const std::size_t tail =
                backtrackMatch(t, mStar, m, "PRIVATE KEY-----");
            if (tail != std::string::npos) {
                end = tail + litLen("PRIVATE KEY-----");
                closed = true;
                break;
            }
        }
        // 无 END 的裸 BEGIN：脱到文本尾（fail-closed）。块内再出现的 BEGIN 产出
        // 的重叠候选会被优先级去重叠丢弃，与母本的覆盖检查同效。
        cands->push_back({kPrioPrivateKey, i, end, "secret",
                          closed ? "private_key" : "private_key_no_end"});
    }
}

void scanKvSecret(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    const auto isValueChar = [](std::uint32_t c) {
        // 不含 '&'：URL 里它是参数分隔符，与 url_param 边界一致（母本注记）
        return isAlnum(c) || c == '@' || c == '#' || c == '$' || c == '%' ||
               c == '^' || c == '*' || c == '+' || c == '/' || c == '_' ||
               c == '-' || c == '.' || c == '~' || c == '=';
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && (isAlnum(t[i - 1].cp) || t[i - 1].cp == '$')) {
            continue;  // (?<![A-Za-z0-9$])；'-' 允许前置（x-api-key: 形态）
        }
        std::size_t kwLen = 0;
        if (!matchKeyword(t, i, kvKeywords(), &kwLen)) continue;
        std::size_t j = i + kwLen;
        if (j < t.size() && isWord(t[j].cp)) continue;  // \b
        while (j < t.size() && isSpace(t[j].cp)) ++j;
        if (j >= t.size() || (t[j].cp != ':' && t[j].cp != '=')) continue;
        ++j;
        while (j < t.size() && isSpace(t[j].cp)) ++j;
        for (const char* scheme :
             {"bearer", "basic", "digest", "token"}) {
            if (matchAscii(t, j, scheme, false)) {
                j += litLen(scheme);
                while (j < t.size() && isSpace(t[j].cp)) ++j;
                break;
            }
        }
        if (j < t.size() && (t[j].cp == '"' || t[j].cp == '\'')) ++j;
        const std::size_t v0 = j;
        while (j < t.size() && isValueChar(t[j].cp)) ++j;
        if (j - v0 < 4) continue;  // 正则要求值至少 4 字符
        cands->push_back({kPrioKvSecret, i, j, "secret", "kv_credential"});
    }
}

void scanBearer(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    const auto isB64 = [](std::uint32_t c) {
        return isAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
               c == '+' || c == '/';
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && isWord(t[i - 1].cp)) continue;  // \bBearer
        // 母本 _BEARER_RE 无 IGNORECASE：Bearer 三段大小写精确
        if (!matchAscii(t, i, "Bearer", true)) continue;
        std::size_t j = i + litLen("Bearer");
        if (j >= t.size() || !isSpace(t[j].cp)) continue;
        while (j < t.size() && isSpace(t[j].cp)) ++j;
        const std::size_t v0 = j;
        while (j < t.size() && isB64(t[j].cp)) ++j;
        if (j == v0) continue;
        while (j < t.size() && t[j].cp == '=') ++j;  // base64 padding
        bool hasDigit = false;
        for (std::size_t k = v0; k < j; ++k) {
            if (isDigit(t[k].cp)) {
                hasDigit = true;
                break;
            }
        }
        // 至少含一位数字或长度 >=20：避免切走 "Bearer authentication" 类普通语句
        if (!hasDigit && j - v0 < 20) continue;
        cands->push_back({kPrioBearer, i, j, "secret", "bearer"});
    }
}

void scanVendorKey(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    const auto isAZ09 = [](std::uint32_t c) {
        return (c >= 'A' && c <= 'Z') || isDigit(c);
    };
    const auto isKeyRun = [](std::uint32_t c) {
        return isAlnum(c) || c == '_' || c == '-';
    };
    // [start, end) 内取受 cap 限制的游标：返回 run 是否达到下限 minRun
    const auto takeRun = [&](std::size_t* j, std::size_t cap, std::size_t minRun,
                             bool (*pred)(std::uint32_t)) {
        std::size_t k = 0;
        while (k < cap && *j < t.size() && pred(t[*j].cp)) {
            ++*j;
            ++k;
        }
        return k >= minRun;
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && isAlnum(t[i - 1].cp)) continue;  // (?<![A-Za-z0-9])
        std::size_t j = i;
        const char* detail = nullptr;
        if (matchAscii(t, i, "github_pat_", true)) {  // 必须先于 gh 分支
            j = i + litLen("github_pat_");
            if (!takeRun(&j, 255, 22, [](std::uint32_t c) {
                    return isAlnum(c) || c == '_';
                })) {
                continue;
            }
            detail = "github_fine_grained";
        } else if (matchAscii(t, i, "AKIA", true) ||
                   matchAscii(t, i, "ASIA", true) ||
                   matchAscii(t, i, "AIDA", true)) {
            j = i + 4;
            if (!takeRun(&j, 16, 16, isAZ09)) continue;
            detail = "aws_access_key";
        } else if (matchAscii(t, i, "AIza", true)) {
            j = i + 4;
            if (!takeRun(&j, 35, 35, [](std::uint32_t c) {
                    return isAlnum(c) || c == '_' || c == '-';
                })) {
                continue;
            }
            detail = "google_api_key";
        } else if (i + 2 < t.size() && lower(t[i].cp) == 'g' &&
                   lower(t[i + 1].cp) == 'h') {
            // gh[pousr]_；p/o/u/s/r 之外（如 github_pat_ 的 i）不属此分支
            const std::uint32_t c3 = lower(t[i + 2].cp);
            if (c3 != 'p' && c3 != 'o' && c3 != 'u' && c3 != 's' && c3 != 'r') {
                continue;
            }
            if (i + 3 >= t.size() || t[i + 3].cp != '_') continue;
            j = i + 4;
            if (!takeRun(&j, 255, 36,
                         [](std::uint32_t c) { return isAlnum(c); })) {
                continue;
            }
            detail = "github_token";
        } else if (matchAscii(t, i, "sk-", true)) {
            j = i + 3;
            if (matchAscii(t, j, "proj-", true)) j += 5;
            if (!takeRun(&j, t.size(), 20, isKeyRun)) continue;
            detail = "openai_style";
        } else if (i + 4 < t.size() && lower(t[i].cp) == 'x' &&
                   lower(t[i + 1].cp) == 'o' && lower(t[i + 2].cp) == 'x') {
            const std::uint32_t c4 = lower(t[i + 3].cp);
            if (c4 != 'a' && c4 != 'b' && c4 != 'p' && c4 != 'r' && c4 != 's') {
                continue;
            }
            if (t[i + 4].cp != '-') continue;
            j = i + 5;
            if (!takeRun(&j, t.size(), 10, [](std::uint32_t c) {
                    return isAlnum(c) || c == '-';
                })) {
                continue;
            }
            detail = "slack_token";
        } else {
            continue;
        }
        if (j < t.size() && isAlnum(t[j].cp)) continue;  // (?![A-Za-z0-9])
        cands->push_back({kPrioVendorKey, i, j, "secret", detail});
    }
}

void scanJwt(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    const auto isJwt = [](std::uint32_t c) {
        return isAlnum(c) || c == '_' || c == '-';
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 &&
            (isWord(t[i - 1].cp) || t[i - 1].cp == '.' || t[i - 1].cp == '-')) {
            continue;  // (?<![\w.-])
        }
        if (!matchAscii(t, i, "eyJ", true)) continue;
        std::size_t j = i + 3;
        std::size_t seg = 0;
        bool ok = true;
        for (int s = 0; s < 3; ++s) {
            seg = 0;
            while (j < t.size() && isJwt(t[j].cp)) {
                ++j;
                ++seg;
            }
            if (seg < 4) {
                ok = false;
                break;
            }
            if (s < 2) {
                if (j >= t.size() || t[j].cp != '.') {
                    ok = false;
                    break;
                }
                ++j;
            }
        }
        if (!ok) continue;
        if (j < t.size() &&
            (isWord(t[j].cp) || t[j].cp == '.' || t[j].cp == '-')) {
            continue;  // (?![\w.-])
        }
        cands->push_back({kPrioJwt, i, j, "secret", "jwt"});
    }
}

void scanEmail(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    const auto isLocal = [](std::uint32_t c) {
        return isAlnum(c) || c == '.' || c == '_' || c == '%' || c == '+' ||
               c == '-';
    };
    const auto isBoundary = [](std::uint32_t c) {
        return !(isWord(c) || c == '.' || c == '+' || c == '-');
    };
    // label := alnum [alnum-]* alnum（尾 '-' 回退出局）
    const auto parseLabel = [&t](std::size_t* k) {
        if (*k >= t.size() || !isAlnum(t[*k].cp)) return false;
        const std::size_t s = *k;
        ++*k;
        while (*k < t.size() && (isAlnum(t[*k].cp) || t[*k].cp == '-')) ++*k;
        while (*k > s + 1 && t[*k - 1].cp == '-') --*k;
        return true;
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && !isBoundary(t[i - 1].cp)) continue;  // (?<![\w.+-])
        if (!isLocal(t[i].cp)) continue;
        std::size_t j = i;
        while (j < t.size() && isLocal(t[j].cp)) ++j;
        if (j >= t.size() || t[j].cp != '@') continue;
        std::size_t k = j + 1;
        if (!parseLabel(&k)) continue;
        std::size_t ext = 0;
        while (k < t.size() && t[k].cp == '.') {
            const std::size_t save = k;
            ++k;
            if (parseLabel(&k)) {
                ++ext;
                continue;
            }
            k = save;
            break;
        }
        if (ext == 0) continue;  // (?:\.label)+ 至少一段
        if (k < t.size() && !isBoundary(t[k].cp)) continue;  // (?![\w.+-])
        cands->push_back({kPrioEmail, i, k, "email", "email"});
    }
}

void scanUrlParam(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i].cp != '?' && t[i].cp != '&') continue;
        std::size_t kwLen = 0;
        if (!matchKeyword(t, i + 1, urlParamKeywords(), &kwLen)) continue;
        std::size_t j = i + 1 + kwLen;
        if (j >= t.size() || t[j].cp != '=') continue;
        ++j;
        const std::size_t v0 = j;
        while (j < t.size() && t[j].cp != ' ' && t[j].cp != '\t' &&
               t[j].cp != '\n' && t[j].cp != '\r' && t[j].cp != '&' &&
               t[j].cp != '"' && t[j].cp != '\'' && t[j].cp != '<' &&
               t[j].cp != '>') {
            ++j;
        }
        if (j == v0) continue;
        cands->push_back({kPrioUrlParam, i, j, "url_param", "url_query"});
    }
}

// 月 (0[1-9]|1[0-2]) / 日 (0[1-9]|[12]\d|3[01])（码点流上两位连续判）
bool isMonth(const std::vector<Cp>& t, std::size_t i) {
    if (i + 1 >= t.size() || !isDigit(t[i].cp) || !isDigit(t[i + 1].cp)) {
        return false;
    }
    if (t[i].cp == '0') return t[i + 1].cp >= '1';
    if (t[i].cp == '1') return t[i + 1].cp <= '2';
    return false;
}
bool isDay(const std::vector<Cp>& t, std::size_t i) {
    if (i + 1 >= t.size() || !isDigit(t[i].cp) || !isDigit(t[i + 1].cp)) {
        return false;
    }
    if (t[i].cp == '0') return t[i + 1].cp >= '1';
    if (t[i].cp == '1' || t[i].cp == '2') return true;
    if (t[i].cp == '3') return t[i + 1].cp <= '1';
    return false;
}

std::string id18Detail(const std::vector<Cp>& t, std::size_t start,
                       std::size_t end) {
    // 末位是校验位；其余位按 GB11643 权重求和模 11 查表（非法仍脱敏，detail 记 bad）
    static constexpr int kWeights[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6,
                                         3, 7, 9,  10, 5, 8, 4, 2};
    static const char* kCheck = "10X98765432";
    int total = 0;
    int n = 0;
    for (std::size_t k = start; k + 1 < end && n < 17; ++k) {
        if (isDigit(t[k].cp)) total += static_cast<int>(t[k].cp - '0') * kWeights[n++];
    }
    const char expect = kCheck[total % 11];
    const char actual = static_cast<char>(lower(t[end - 1].cp));
    return std::string("id18:checksum_") + (actual == expect ? "ok" : "bad");
}

void scanIdCard(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    // 18 位：\d{6}[- ]?\d{4}(MM)(DD)[- ]?\d{3}[0-9Xx]；两个 [- ]? 相互独立，
    // 四种组合都试（贪心语义：有分隔形态优先）。年份不限定 19/20 开头（泄漏 0 优先）。
    const auto sepAt = [&t](std::size_t k) {
        return k < t.size() && (t[k].cp == '-' || t[k].cp == ' ');
    };
    const auto try18 = [&](std::size_t i) -> std::size_t {
        std::size_t j = i;
        std::size_t d = 0;
        while (d < 6 && j < t.size() && isDigit(t[j].cp)) {
            ++j;
            ++d;
        }
        if (d != 6) return 0;
        for (int sep1 = 1; sep1 >= 0; --sep1) {
            if (sep1 && !sepAt(j)) continue;
            std::size_t k = j + (sep1 ? 1 : 0);
            // 分隔间 = \d{4}+MM+DD 连续 8 位（月窗 [k-4,k-2)、日窗 [k-2,k)）
            std::size_t d2 = 0;
            while (d2 < 8 && k < t.size() && isDigit(t[k].cp)) {
                ++k;
                ++d2;
            }
            if (d2 != 8 || !isMonth(t, k - 4) || !isDay(t, k - 2)) continue;
            for (int sep2 = 1; sep2 >= 0; --sep2) {
                if (sep2 && !sepAt(k)) continue;
                std::size_t m = k + (sep2 ? 1 : 0);
                std::size_t d3 = 0;
                while (d3 < 3 && m < t.size() && isDigit(t[m].cp)) {
                    ++m;
                    ++d3;
                }
                if (d3 != 3) continue;
                if (m >= t.size() ||
                    !(isDigit(t[m].cp) || t[m].cp == 'X' || t[m].cp == 'x')) {
                    continue;
                }
                ++m;
                if (m < t.size() &&
                    (isDigit(t[m].cp) || t[m].cp == 'X' || t[m].cp == 'x')) {
                    continue;  // (?![0-9Xx])
                }
                return m;
            }
        }
        return 0;
    };
    // 15 位：\d{8}(MM)(DD)\d{3}（连续 15 位数字）
    const auto try15 = [&](std::size_t i) -> std::size_t {
        std::size_t j = i;
        std::size_t d = 0;
        while (d < 15 && j < t.size() && isDigit(t[j].cp)) {
            ++j;
            ++d;
        }
        if (d != 15) return 0;
        if (!isMonth(t, i + 8) || !isDay(t, i + 10)) return 0;
        return j;
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        // 18 位前置 (?<![0-9Xx])；15 位前置 (?<!\d)（较宽，单独判）
        const bool prevDigit = i > 0 && isDigit(t[i - 1].cp);
        const bool prevIdChar =
            prevDigit || (i > 0 && (t[i - 1].cp == 'X' || t[i - 1].cp == 'x'));
        if (!prevIdChar) {
            const std::size_t e18 = try18(i);
            if (e18 != 0) {
                cands->push_back(
                    {kPrioIdCard, i, e18, "id_card", id18Detail(t, i, e18)});
                continue;
            }
        }
        if (!prevDigit) {
            const std::size_t e15 = try15(i);
            if (e15 != 0) {
                cands->push_back({kPrioIdCard, i, e15, "id_card", "id15"});
            }
        }
    }
}

void scanPhone(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    // (?<!\d)(?:\+?86[- ]?)?1[3-9]\d[- ]?\d{4}[- ]?\d{4}(?!\d)
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && isDigit(t[i - 1].cp)) continue;
        std::size_t j = i;
        const std::size_t save = j;
        if (j < t.size() && t[j].cp == '+') ++j;
        if (j + 1 < t.size() && t[j].cp == '8' && t[j + 1].cp == '6') {
            j += 2;
            if (j < t.size() && (t[j].cp == '-' || t[j].cp == ' ')) ++j;
        } else {
            j = save;
        }
        if (j + 1 >= t.size() || t[j].cp != '1' || t[j + 1].cp < '3' ||
            t[j + 1].cp > '9') {
            continue;
        }
        j += 2;
        if (j >= t.size() || !isDigit(t[j].cp)) continue;
        ++j;
        if (j < t.size() && (t[j].cp == '-' || t[j].cp == ' ')) ++j;
        std::size_t d = 0;
        while (d < 4 && j < t.size() && isDigit(t[j].cp)) {
            ++j;
            ++d;
        }
        if (d != 4) continue;
        if (j < t.size() && (t[j].cp == '-' || t[j].cp == ' ')) ++j;
        d = 0;
        while (d < 4 && j < t.size() && isDigit(t[j].cp)) {
            ++j;
            ++d;
        }
        if (d != 4) continue;
        if (j < t.size() && isDigit(t[j].cp)) continue;  // (?!\d)
        std::string digits;
        digits.reserve(j - i);
        for (std::size_t x = i; x < j; ++x) {
            if (isDigit(t[x].cp)) digits.push_back(static_cast<char>(t[x].cp));
        }
        if (inList(kPhoneAllowlist, digits)) continue;
        if (digits.size() == 13 && digits.compare(0, 2, "86") == 0 &&
            inList(kPhoneAllowlist, digits.substr(2))) {
            continue;
        }
        cands->push_back({kPrioPhone, i, j, "phone", "cn_mobile"});
    }
}

void scanIpv4(const std::vector<Cp>& t, std::vector<Cand>* cands) {
    // 八位组：25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d（无前导零，除单 0）
    const auto parseOctet = [&t](std::size_t* p) {
        const std::size_t j = *p;
        if (j + 2 < t.size() && isDigit(t[j].cp) && isDigit(t[j + 1].cp) &&
            isDigit(t[j + 2].cp)) {
            const bool ok = (t[j].cp == '2' && t[j + 1].cp == '5' &&
                             t[j + 2].cp <= '5') ||
                            (t[j].cp == '2' && t[j + 1].cp <= '4') ||
                            t[j].cp == '1';
            if (ok) {
                *p = j + 3;
                return true;
            }
            return false;  // 三位但越界/前导零
        }
        if (j < t.size() && isDigit(t[j].cp)) {
            if (t[j].cp == '0') {
                *p = j + 1;  // 单 0
            } else if (j + 1 < t.size() && isDigit(t[j + 1].cp)) {
                *p = j + 2;
            } else {
                *p = j + 1;
            }
            return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && (isDigit(t[i - 1].cp) || t[i - 1].cp == '.')) continue;
        std::size_t j = i;
        bool ok = true;
        for (int o = 0; o < 4; ++o) {
            if (o > 0) {
                if (j >= t.size() || t[j].cp != '.') {
                    ok = false;
                    break;
                }
                ++j;
            }
            if (!parseOctet(&j)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        if (j < t.size() && (isDigit(t[j].cp) || t[j].cp == '.')) continue;
        std::string text;
        text.reserve(j - i);
        for (std::size_t x = i; x < j; ++x) {
            text.push_back(static_cast<char>(t[x].cp));
        }
        if (inList(kIpv4Allowlist, text)) continue;
        cands->push_back({kPrioIpv4, i, j, "ipv4", "ipv4"});
    }
}

}  // namespace

std::vector<std::string> SanitizeResult::categories() const {
    std::vector<std::string> out;
    for (const SanitizeHit& h : hits) {
        if (std::find(out.begin(), out.end(), h.category) == out.end()) {
            out.push_back(h.category);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

core::Result<std::vector<SanitizeHit>> scanText(const std::string& utf8Text) {
    std::vector<Cp> t;
    if (!decodeUtf8(utf8Text, &t)) {
        // fail-closed：非法 UTF-8 无法保证检测完备，必须沿调用链中断外发
        return core::Result<std::vector<SanitizeHit>>::fail(
            core::ErrorCode::ValidationError,
            "sanitize: 输入不是合法 UTF-8，按泄漏 0 原则拒绝外发");
    }
    for (Cp& c : t) c.cp = shadowOf(c.cp);

    std::vector<Cand> cands;
    scanPrivateKey(t, &cands);
    scanKvSecret(t, &cands);
    scanBearer(t, &cands);
    scanVendorKey(t, &cands);
    scanJwt(t, &cands);
    scanEmail(t, &cands);
    scanUrlParam(t, &cands);
    scanIdCard(t, &cands);
    scanPhone(t, &cands);
    scanIpv4(t, &cands);

    // 优先级降序 → 位置升序 → 长度降序；贪心收无重叠者（母本 scan()）
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.prio != b.prio) return a.prio > b.prio;
        if (a.start != b.start) return a.start < b.start;
        return (a.end - a.start) > (b.end - b.start);
    });
    std::vector<Cand> accepted;
    for (const Cand& c : cands) {
        bool overlap = false;
        for (const Cand& a : accepted) {
            if (!(c.end <= a.start || c.start >= a.end)) {
                overlap = true;
                break;
            }
        }
        if (!overlap) accepted.push_back(c);
    }
    std::sort(accepted.begin(), accepted.end(),
              [](const Cand& a, const Cand& b) { return a.start < b.start; });

    std::vector<SanitizeHit> hits;
    hits.reserve(accepted.size());
    for (const Cand& c : accepted) {
        SanitizeHit h;
        h.category = c.category;
        h.start = t[c.start].off;
        h.end = t[c.end - 1].off + t[c.end - 1].len;
        h.detail = c.detail;
        h.preview = std::string(c.end - c.start, '*');  // 等长全遮蔽
        hits.push_back(std::move(h));
    }
    return core::Result<std::vector<SanitizeHit>>::ok(std::move(hits));
}

core::Result<SanitizeResult> sanitizeText(const std::string& utf8Text) {
    SanitizeResult r;
    auto scanned = scanText(utf8Text);
    if (scanned.isFail()) {
        return core::Result<SanitizeResult>::fail(scanned.errorCode(),
                                                  scanned.errorMessage());
    }
    r.hits = std::move(scanned.value());
    r.text = utf8Text;
    // 从尾向前替换：前序替换不改更靠前的字节偏移
    for (auto it = r.hits.rbegin(); it != r.hits.rend(); ++it) {
        r.text.replace(it->start, it->end - it->start,
                       "[REDACTED:" + it->category + "]");
    }
    return core::Result<SanitizeResult>::ok(std::move(r));
}

SanitizeGate::SanitizeGate(AuditFn auditLine)
    : auditLine_(std::move(auditLine)) {}

core::Result<SanitizeResult> SanitizeGate::process(
    const std::string& utf8Text) const {
    if (!enabled_) {
        // 关 = 旁路直通，但记一条审计日志行（行内只有长度，绝无内容）
        if (auditLine_) {
            auditLine_("sanitize:bypass bytes=" +
                       std::to_string(utf8Text.size()));
        }
        SanitizeResult r;
        r.text = utf8Text;
        return core::Result<SanitizeResult>::ok(std::move(r));
    }
    auto r = sanitizeText(utf8Text);
    if (auditLine_) {
        if (r.isOk()) {
            auditLine_("sanitize:applied hits=" +
                       std::to_string(r.value().hits.size()) + " bytes=" +
                       std::to_string(utf8Text.size()));
        } else {
            auditLine_("sanitize:rejected bytes=" +
                       std::to_string(utf8Text.size()));
        }
    }
    return r;
}

}  // namespace module::memory::sanitize
