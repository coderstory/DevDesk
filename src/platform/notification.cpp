// src/platform/notification.cpp
// platform::notify 的平台无关 facade：清洗/截断 → 授权 → 去重 → 限速 → 后端。
// 参数值全部来自 spec §5.4，不许改：去重窗口 2s / 缓存上限 64 条 /
// 令牌桶突发 3 条、之后 1 条每秒。平台后端见 *_macos.mm / *_win.cpp / *_posix.cpp。
//
// 线程纪律（sqlite 单句柄教训的同款打法）：下面一把 mutex 保护全部可变状态
//（去重缓存 + 令牌桶 + 时钟覆盖），任何读写都必须持锁；后端调用在锁外
//（Windows 后端可能阻塞 1.5s，持锁会串死所有发送方）。
#include "notification.h"

#include "core/logging.h"
#include "platform/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace platform::notify {
namespace {

using Clock = std::chrono::steady_clock;

// —— 去重/限速参数（spec §5.4，改动 = 行为契约变化，须连 spec 一起改）——
constexpr auto kDedupeWindow = std::chrono::seconds(2);
constexpr size_t kDedupeCapacity = 64;
constexpr double kBucketCapacity = 3.0;   // 突发 3 条
constexpr double kRefillPerSecond = 1.0;  // 之后 1 条每秒

// —— 截断上限：UTF-16 code unit（BMP 码点 1 unit、补充平面 2 unit）——
constexpr size_t kTitleMaxUnits = 63;
constexpr size_t kBodyMaxUnits = 255;

std::mutex g_mu;
std::unordered_map<std::string, Clock::time_point> g_dedupe;
std::deque<std::string> g_dedupeOrder;   // 插入序，容量超限时从最旧驱逐
double g_tokens = kBucketCapacity;
double g_lastElapsed = 0.0;              // 令牌桶上次结算时刻（readNow 的秒数口径）
bool g_bucketSeeded = false;             // 首次使用才起算，避免进程空转期囤积
NowFn g_clock;                           // 空 = steady_clock::now
std::atomic<bool> g_initialized{false};

Clock::time_point readNowLocked() {
    if (g_clock) return g_clock();
    return Clock::now();
}

// —— UTF-8 解出一个码点；输入已被 utf8::sanitize 良构化，防御分支按 1 字节吞掉 ——
size_t decodeCodePoint(const std::string& text, size_t i, unsigned& cp) {
    const auto byte = static_cast<unsigned char>(text[i]);
    size_t len = 1;
    if (byte < 0x80) {
        cp = byte;
    } else if ((byte & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = byte & 0x1F;
        len = 2;
    } else if ((byte & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = byte & 0x0F;
        len = 3;
    } else if ((byte & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = byte & 0x07;
        len = 4;
    } else {
        cp = 0xFFFD;   // 防御：不该出现（sanitize 后必良构）
        return 1;
    }
    for (size_t k = 1; k < len; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    return len;
}

// 码点折算 UTF-16 unit：BMP 1 unit、补充平面（≥ U+10000）2 unit。
size_t codePointUnits(unsigned cp) { return cp >= 0x10000 ? 2 : 1; }

size_t totalUnits(const std::string& text) {
    size_t units = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned cp = 0;
        i += decodeCodePoint(text, i, cp);
        units += codePointUnits(cp);
    }
    return units;
}

// 按UTF-16 unit 上限截断：超限保留前 (max-1) unit 整码点 + U+2026（1 unit），
// 合计 ≤ max。整码点前进保证代理对不被切开。
std::string truncateUnits(const std::string& text, size_t maxUnits) {
    if (totalUnits(text) <= maxUnits) return text;
    const size_t budget = maxUnits - 1;   // 给省略号留 1 unit
    size_t units = 0;
    size_t end = 0;
    while (end < text.size()) {
        unsigned cp = 0;
        const size_t len = decodeCodePoint(text, end, cp);
        const size_t need = codePointUnits(cp);
        if (units + need > budget) break;
        units += need;
        end += len;
    }
    return text.substr(0, end) + "\xE2\x80\xA6";
}

bool isAsciiSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trimAscii(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isAsciiSpace(text[begin])) ++begin;
    while (end > begin && isAsciiSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// title：所有 C0/DEL 与换行制表 → 空格 → 连续空格折叠 → 去首尾空格（单行化）。
std::string sanitizeTitle(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u <= 0x1F || u == 0x7F ? ' ' : c);
    }
    std::string collapsed;
    collapsed.reserve(out.size());
    bool inSpace = false;
    for (const char c : out) {
        if (c == ' ') {
            if (inSpace) continue;
            inSpace = true;
        } else {
            inSpace = false;
        }
        collapsed.push_back(c);
    }
    return trimAscii(collapsed);
}

// body：CRLF/CR → LF（CRLF 只产出一个 LF）；\t 与其余 C0（除 \n）→ 空格；
// 3+ 连续 \n 折叠为 2；去首尾空白。
std::string sanitizeBody(const std::string& raw) {
    std::string unified;
    unified.reserve(raw.size());
    bool afterCR = false;
    for (const char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\r') {
            unified.push_back('\n');
            afterCR = true;
        } else if (c == '\n') {
            if (afterCR) {
                afterCR = false;   // 配对 \n 已由 CR 产出，吞掉
                continue;
            }
            unified.push_back('\n');
        } else {
            afterCR = false;
            unified.push_back(u <= 0x1F || u == 0x7F ? ' ' : c);
        }
    }
    std::string collapsed;
    collapsed.reserve(unified.size());
    int newlineRun = 0;
    for (const char c : unified) {
        if (c == '\n') {
            if (newlineRun >= 2) continue;   // 第 3 个起丢弃
            ++newlineRun;
        } else {
            newlineRun = 0;
        }
        collapsed.push_back(c);
    }
    // 首尾 trim：空格与换行都算空白
    size_t begin = 0;
    size_t end = collapsed.size();
    const auto isBlank = [](char c) { return c == ' ' || c == '\n'; };
    while (begin < end && isBlank(collapsed[begin])) ++begin;
    while (end > begin && isBlank(collapsed[end - 1])) --end;
    return collapsed.substr(begin, end - begin);
}

// FNV-1a 64 位；id 派生与超长 id 替换共用。
std::string fnv1a64Hex(const std::string& text) {
    uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    static const char kDigits[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i) {
        hex[static_cast<size_t>(i)] = kDigits[hash & 0xF];
        hash >>= 4;
    }
    return hex;
}

std::string detailFor(Status status) {
    switch (status) {
        case Status::Accepted:
            return "系统已受理（是否展示取决于系统设置）";
        case Status::Deduped:
            return "与 2 秒内的上一条通知内容重复，已去重跳过";
        case Status::RateLimited:
            return "发送过于频繁（突发 3 条、之后每秒 1 条），已被限速";
        case Status::NotBundled:
            // 只有 macOS 后端会产出 NotBundled；措辞与 macOS bundle guard 一致。
            return "当前进程没有有效的 .app bundle，无法投递系统通知";
        case Status::PermissionDenied:
            return "用户已拒绝系统通知授权";
        case Status::NotDetermined:
            return "用户尚未决定是否允许系统通知，已异步发起授权请求";
        case Status::BackendUnavailable:
            return "系统通知后端不可用";
    }
    return "未知状态";
}

} // namespace

const char* statusToString(Status status) {
    switch (status) {
        case Status::Accepted: return "accepted";
        case Status::Deduped: return "deduped";
        case Status::RateLimited: return "rate_limited";
        case Status::NotBundled: return "not_bundled";
        case Status::PermissionDenied: return "permission_denied";
        case Status::NotDetermined: return "not_determined";
        case Status::BackendUnavailable: return "backend_unavailable";
    }
    return "unknown";
}

const char* authorizationToString(Authorization auth) {
    switch (auth) {
        case Authorization::Unknown: return "unknown";
        case Authorization::NotDetermined: return "not_determined";
        case Authorization::Denied: return "denied";
        case Authorization::Authorized: return "authorized";
    }
    return "unknown";
}

void initialize() {
    if (g_initialized.exchange(true, std::memory_order_relaxed)) {
        return;   // 幂等：只允许真正初始化一次
    }
    backend().initialize();
}

void requestAuthorization() { backend().requestAuthorization(); }

Authorization authorizationStatus() { return backend().authorizationStatus(); }

core::Result<Receipt> send(const Notification& notification) {
    // 1) 清洗 + 截断。utf8::sanitize 先行（非法字节 → U+FFFD），后端拿到的必是
    //    良构、有界文本。截断标志必须按**清洗后、截断前**的长度判定——对截断后
    //    的串算恒为 false。
    const std::string rawTitle = sanitizeTitle(utf8::sanitize(notification.title));
    const std::string rawBody = sanitizeBody(utf8::sanitize(notification.body));
    if (rawTitle.empty()) {
        return core::Result<Receipt>::fail(core::ErrorCode::ValidationError,
                                           "通知标题（title）不能为空");
    }
    if (rawBody.empty()) {
        return core::Result<Receipt>::fail(core::ErrorCode::ValidationError,
                                           "通知正文（body）不能为空");
    }
    const bool titleTruncated = totalUnits(rawTitle) > kTitleMaxUnits;
    const bool bodyTruncated = totalUnits(rawBody) > kBodyMaxUnits;
    const std::string title = truncateUnits(rawTitle, kTitleMaxUnits);
    const std::string body = truncateUnits(rawBody, kBodyMaxUnits);

    // 2) id：清洗 trim；空 = 派生；>64 unit = 哈希替换（spec §5.3.4）。
    std::string id = trimAscii(utf8::sanitize(notification.id));
    if (id.empty()) {
        id = "devdesk-" +
             fnv1a64Hex(std::to_string(static_cast<int>(notification.level)) +
                        "\n" + title + "\n" + body);
    } else if (totalUnits(id) > 64) {
        id = "devdesk-" + fnv1a64Hex(id);
    }

    Receipt receipt;
    receipt.effectiveId = id;
    receipt.titleTruncated = titleTruncated;
    receipt.bodyTruncated = bodyTruncated;

    // 3) 授权检查在前：Denied / NotDetermined 比 RateLimited 更有信息量。
    //    Unknown（无法查询）不拦，继续走后端。
    const Authorization auth = backend().authorizationStatus();
    if (auth == Authorization::Denied) {
        receipt.status = Status::PermissionDenied;
        receipt.detail = detailFor(receipt.status);
        return core::Result<Receipt>::ok(std::move(receipt));
    }
    if (auth == Authorization::NotDetermined) {
        requestAuthorization();   // 异步、幂等；本次发送不投递
        receipt.status = Status::NotDetermined;
        receipt.detail = detailFor(receipt.status);
        return core::Result<Receipt>::ok(std::move(receipt));
    }

    // 4) 去重 + 5) 限速：同一把锁内完成，后端调用在锁外。
    Notification cleaned;
    cleaned.id = id;
    cleaned.title = title;
    cleaned.body = body;
    cleaned.sound = notification.sound;
    cleaned.level = notification.level;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        const Clock::time_point now = readNowLocked();

        const std::string key =
            id + '\x1f' + std::to_string(static_cast<int>(notification.level)) +
            '\x1f' + title + '\x1f' + body + '\x1f' + (notification.sound ? "1" : "0");
        const auto hit = g_dedupe.find(key);
        if (hit != g_dedupe.end()) {
            const auto age = now - hit->second;
            if (age < kDedupeWindow) {
                receipt.status = Status::Deduped;
                receipt.detail = detailFor(receipt.status);
                return core::Result<Receipt>::ok(std::move(receipt));
            }
            g_dedupe.erase(hit);
        }

        // 令牌桶：按流逝时间补满，速率 1 条/秒，容量 3。去重命中不走到这里
        //（重复内容不消耗令牌，spec §5.4）。
        if (!g_bucketSeeded) {
            g_bucketSeeded = true;
            g_tokens = kBucketCapacity;
            g_lastElapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                                now.time_since_epoch()).count();
        }
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(now.time_since_epoch()).count();
        if (elapsed > g_lastElapsed) {
            g_tokens = std::min(kBucketCapacity,
                                g_tokens + (elapsed - g_lastElapsed) * kRefillPerSecond);
            g_lastElapsed = elapsed;
        }
        if (g_tokens < 1.0) {
            receipt.status = Status::RateLimited;
            receipt.retryAfterMs =
                static_cast<int>(std::ceil((1.0 - g_tokens) * 1000.0));
            receipt.detail = detailFor(receipt.status);
            return core::Result<Receipt>::ok(std::move(receipt));
        }
        g_tokens -= 1.0;

        g_dedupe[key] = now;
        // order 每键至多一个条目：过期重建路径会在 order 里留下旧键位，重插前先摘除，
        // 否则容量驱逐从 front 弹到旧键位时会把新 map 条目提前抹掉（残留位再弹成空操作）
        const auto stale =
            std::remove(g_dedupeOrder.begin(), g_dedupeOrder.end(), key);
        g_dedupeOrder.erase(stale, g_dedupeOrder.end());
        g_dedupeOrder.push_back(key);
        while (g_dedupeOrder.size() > kDedupeCapacity) {
            g_dedupe.erase(g_dedupeOrder.front());
            g_dedupeOrder.pop_front();
        }
    }

    // 6) 后端：Result::fail 映射为 BackendUnavailable（spec §5.6），detail 保留
    //    错误消息并 LOG_WARN；不把后端内部错误直接抛给 LLM。
    auto sent = backend().send(cleaned);
    if (sent.isFail()) {
        LOG_WARN("系统通知后端失败: " + sent.errorMessage());
        receipt.status = Status::BackendUnavailable;
        receipt.detail = sent.errorMessage();
        return core::Result<Receipt>::ok(std::move(receipt));
    }
    receipt.status = sent.value();
    receipt.detail = detailFor(receipt.status);
    return core::Result<Receipt>::ok(std::move(receipt));
}

void setClockForTest(NowFn now) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_clock = std::move(now);
}

void resetForTest() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_dedupe.clear();
        g_dedupeOrder.clear();
        g_tokens = kBucketCapacity;
        g_bucketSeeded = false;
        g_lastElapsed = 0.0;
        g_clock = nullptr;
        g_initialized.store(false, std::memory_order_relaxed);
    }
    resetBackendForTest();
}

} // namespace platform::notify
