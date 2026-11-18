// src/module_memory/pipeline/collect_round.cpp
#include "collect_round.h"

#include <openssl/sha.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/logging.h"

namespace module::memory::pipeline {

namespace {

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

// 拦截段 note：只含会话名与类别（脱敏契约：行内零原文零正文片段）
RoundNote blockedNote(const collector::Segment& seg, const std::string& text) {
    RoundNote n;
    n.blocked = true;
    n.sessionId = seg.sessionId;
    n.text = text;
    return n;
}

// 采集卡统一前缀（正文构造与指纹剥离同源）
constexpr const char* kCollectCardPrefix = "[踩坑|会话采集] ";

// F-06（bug191）：采集卡 confidence 未评估语义。既往硬编码 0.9 使卡面恒显
// 「高」置信误导人审；采集侧无质量评估能力，落 0.3（<0.5 = 卡面「低」档）
constexpr double kUnassessedConfidence = 0.3;

// F-03① 噪声模式定稿 v1（bug191，2026-11-05 回写 PRD §3）：hook/指令回声
// 特征——命令式指令 + 无项目语义。归一化（去空白 + ASCII 折小写）后子串匹配；
// 误杀面 = 讨论这些短语的正当正文（漏卡成本 < 噪声卡风暴，知情接受）
constexpr const char* kNoisePatterns[] = {
    "不要使用任何工具", "只回复", "请直接回复",
    "do not use any tools", "respond with only",
};

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

std::string stripSpaceAsciiLower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (isAsciiSpace(c)) continue;
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                           : c);
    }
    return out;
}

std::string normalizeForFingerprint(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (!isAsciiSpace(c)) out.push_back(c);
    }
    return out;
}

// F-01 内容指纹：归一化正文 SHA256 hex。段文本 = 事件正文纯拼接
//（collector.cpp 无会话特有信息），同内容跨会话/跨轮 → 同指纹
std::string contentFingerprint(const std::string& normalized) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(normalized.data()),
           normalized.size(), digest);
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (unsigned char b : digest) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0xF]);
    }
    return hex;
}

// 卡正文 → 指纹（剥离采集前缀后归一化，与段指纹同口径）
std::string cardFingerprint(const std::string& resultText) {
    std::string t = resultText;
    const std::size_t plen = std::strlen(kCollectCardPrefix);
    if (t.rfind(kCollectCardPrefix, 0) == 0) t.erase(0, plen);
    return contentFingerprint(normalizeForFingerprint(t));
}

// F-01 已知指纹集：pending 全量 + 近期已决 200（跨轮/跨会话去重）。
// 加载失败降级（LOG + 只剩同轮去重），不阻断采集——fail-open 于去重，
// fail-closed 于敏感闸既有语义不变
std::unordered_set<std::string> loadKnownFingerprints(
    proposals::ProposalStore& store) {
    std::unordered_set<std::string> fps;
    if (auto pending = store.listPending(); pending.isOk()) {
        for (const auto& p : pending.value())
            fps.insert(cardFingerprint(p.resultText));
    } else {
        LOG_WARN("指纹去重：pending 清单不可得（降级为仅同轮去重）: " +
                 pending.errorMessage());
    }
    if (auto decided = store.listRecentDecided(200); decided.isOk()) {
        for (const auto& p : decided.value())
            fps.insert(cardFingerprint(p.resultText));
    }
    return fps;
}

// F-03② 有效字符数：UTF-8 码点遍历，CJK 表意文字计 2、ASCII 字母数字计 1
//（半宽），其余（标点/空白/控制）不计；返回 halves/2（半宽两枚折一枚）
int effectiveUnits(const std::string& text) {
    int halves = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        int len = 1;
        std::uint32_t cp = lead;
        if (lead >= 0xF0) {
            len = 4;
            cp = lead & 0x07u;
        } else if (lead >= 0xE0) {
            len = 3;
            cp = lead & 0x0Fu;
        } else if (lead >= 0xC0) {
            len = 2;
            cp = lead & 0x1Fu;
        }
        bool valid = i + static_cast<std::size_t>(len) <= text.size();
        for (int k = 1; valid && k < len; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                valid = false;
        }
        if (!valid) {
            ++i;  // 坏字节跳过（计量口径，不做解码恢复）
            continue;
        }
        for (int k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0xF900 && cp <= 0xFAFF)) {
            halves += 2;
        } else if (len == 1 && ((lead >= '0' && lead <= '9') ||
                                (lead >= 'a' && lead <= 'z') ||
                                (lead >= 'A' && lead <= 'Z'))) {
            halves += 1;
        }
        i += static_cast<std::size_t>(len);
    }
    return halves / 2;
}

bool isNoiseEcho(const std::string& text) {
    const std::string t = stripSpaceAsciiLower(text);
    for (const char* p : kNoisePatterns) {
        if (t.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// F-06 L2 段级洗（need333 记忆波 1，判据=scheme-card §1 P3/P4，P3 口径按实测
// 4 形态扩）：P4 "[tool_error] " 行前缀剥（保留行内其余正文）；P3 擦除行内
// "[Request interrupted by user…]" 括号段（含 for tool use 变体，覆盖独占行/
// 行尾后缀/拼接形态），洗后空行整行丢。未命中行原样保留（对照集零误洗 =
// 逐字节恒等）。
std::string stripLeakMarkers(const std::string& body, bool* washed) {
    constexpr std::string_view kInterrupt = "[Request interrupted by user";
    constexpr std::string_view kToolErrPrefix = "[tool_error] ";
    // pool:bug:346（BI 审查 P2）：1024B 事件截断（session_reader ③）可把中断
    // 标记从中撕开，残段是 kInterrupt 真前缀 find 不命中→入卡。撕裂只发生在
    // 被截断文本尾部，行尾=真前缀即残段；≥8B「[Request」词根起判（正文裸
    // '[' 等碎前缀不误洗，对照集零误洗契约不破）。返回残段字节数，0=非残段
    const auto tornTailLen = [&kInterrupt](const std::string& l)
                             -> std::size_t {
        constexpr std::size_t kTornMin = 8;  // "[Request" 完整词根
        std::size_t maxLen = kInterrupt.size() - 1;
        if (maxLen > l.size()) maxLen = l.size();
        for (std::size_t len = maxLen; len >= kTornMin; --len) {
            if (l.compare(l.size() - len, len, kInterrupt, 0, len) == 0) {
                return len;
            }
        }
        return 0;
    };
    if (washed != nullptr) *washed = false;
    std::string out;
    out.reserve(body.size());
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const std::size_t nl = body.find('\n', pos);
        const std::size_t end =
            nl == std::string::npos ? body.size() : nl;
        std::string line(body, pos, end - pos);
        bool changed = false;
        if (line.rfind(kToolErrPrefix, 0) == 0) {  // P4：剥前缀留正文
            line.erase(0, kToolErrPrefix.size());
            changed = true;
        }
        std::size_t hit = 0;  // P3：擦中断标记括号段
        while ((hit = line.find(kInterrupt, hit)) != std::string::npos) {
            const std::size_t close = line.find(']', hit);
            line.erase(hit, (close == std::string::npos ? line.size()
                                                         : close + 1) -
                                hit);
            changed = true;
        }
        // pool:bug:346：截断撕开残段兜底（完整命中含无']'尾已由上方循环处理）
        if (const std::size_t torn = tornTailLen(line); torn != 0) {
            line.erase(line.size() - torn);
            changed = true;
        }
        if (changed) {
            if (washed != nullptr) *washed = true;
            // 洗后空行整行丢（P3 独占行形态：删该行，保留两侧正文）
            if (line.find_first_not_of(" \t\r") != std::string::npos) {
                out += line;
                if (nl != std::string::npos) out.push_back('\n');
            }
        } else {
            out += line;
            if (nl != std::string::npos) out.push_back('\n');
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

core::Result<CollectRoundReport> runCollectRound(
    collector::Collector& collector, const sanitize::SanitizeGate& gate,
    proposals::ProposalStore& store,
    const std::function<void(const RoundNote&)>& onNote,
    InferQueueStore* inferQueue) {
    auto segments = collector.pollOnce();
    if (segments.isFail()) {
        return core::Result<CollectRoundReport>::fail(segments.errorCode(),
                                                      segments.errorMessage());
    }
    CollectRoundReport report;
    report.segments = static_cast<int>(segments.value().size());
    const auto now = nowSec();
    // F-04 TTL 回收真实生效（bug191/clue58）：每轮先沉降到期 pending——既往
    // expires_at 只写不执行，没人审的卡永久滞留审核视图。失败只 LOG 不阻断本轮
    if (auto swept = store.expireSweep(now); swept.isFail()) {
        LOG_WARN("过期回收失败（不阻断本轮）: " + swept.errorMessage());
    } else {
        report.expiredSwept = swept.value();
        if (swept.value() > 0 && onNote) {
            RoundNote n;
            n.text = "采集 · TTL 到期沉降 " + std::to_string(swept.value()) +
                     " 张待审卡（30 天无人审）";
            onNote(n);
        }
    }
    // F-01 跨轮/跨会话已知指纹集（pending + 近期已决）；同轮段间去重另计
    std::unordered_set<std::string> knownFps = loadKnownFingerprints(store);
    std::unordered_set<std::string> roundFps;
    // BD 批（bug191）单会话轮内预算：出卡数 + 溢出数 + 头/尾样本引用
    //（头=该会话首个被丢弃段，尾=最后一个；单溢出时头尾合一）
    struct SessionBudget {
        int cards = 0;
        int overflow = 0;
        std::int64_t headSeq = -1;
        std::int64_t tailSeq = -1;
        std::string headFp;
        std::string tailFp;
    };
    std::unordered_map<std::string, SessionBudget> budgets;
    for (const collector::Segment& seg : segments.value()) {
        // 单一管线入口：出机（入库提案）前每段必过闸
        auto gated = gate.process(seg.text);
        if (gated.isFail()) {
            // fail-closed：非法输入视为检测器无法保证 → 中断外发，不入提案
            ++report.blocked;
            RoundNote n = blockedNote(
                seg, "拦截 · 会话 " + seg.sessionId +
                         " 输入非法（fail-closed，已隔离不入提案）");
            if (onNote) onNote(n);
            report.notes.push_back(std::move(n));
            continue;
        }
        if (!gated.value().hits.empty()) {
            // 命中拦截：脱敏预览只进审计口径，原文零落库；不产提案
            ++report.blocked;
            std::string cats;
            for (const auto& c : sanitize::SanitizeResult{gated.value()}.categories()) {
                if (!cats.empty()) cats += ",";
                cats += c;
            }
            RoundNote n = blockedNote(
                seg, "拦截 · 会话 " + seg.sessionId + " 检出 " + cats +
                         "（已隔离不入提案）");
            if (onNote) onNote(n);
            report.notes.push_back(std::move(n));
            continue;
        }
        // F-06 L2 段级洗（need333 记忆波 1）：脱敏闸后、低值过滤前剥泄漏标记
        //（P3 中断行删 / P4 tool_error 前缀剥）；卡面与指纹走洗后正文，evidence
        // 回溯链仍取 seg.text 原文（丢的是脏形态，不是知识本身）
        bool leakWashed = false;
        const std::string body =
            stripLeakMarkers(gated.value().text, &leakWashed);
        if (leakWashed) ++report.washed;
        // F-03 低价值过滤：①指令回声噪声 ②极短段（有效字符 <50）。跳过段
        // 记已处理（不入提案不重扫）；不产 note（风暴量级下动态流不刷屏），
        // 计数走 report 供观测。③纯 tool_error 段由 collector 出段闸保证
        //（collector.cpp:240/251 无 user 正文整段不出），管线不再重复判
        if (isNoiseEcho(body) || effectiveUnits(body) < kMinEffectiveUnits) {
            ++report.lowValueSkipped;
            continue;
        }
        // F-01 内容指纹去重：同轮段间 + 跨轮已知集，命中不建卡不排队
        const std::string fp =
            contentFingerprint(normalizeForFingerprint(body));
        if (roundFps.count(fp) > 0 || knownFps.count(fp) > 0) {
            ++report.dedupSkipped;
            continue;
        }
        roundFps.insert(fp);
        // BD 批（bug191）：单轮单会话出卡上限——超限段丢弃（不建卡不排队），
        // commit 照常记已处理（丢弃=终局不重放，靠样本引用留诊断线索不静默）。
        // 落点在 F-06 洗 / F-03 低值 / F-01 去重之后：overflow 只数真卡级内容
        //（噪声与重复另有计数，不占本上限也不虚增 overflow）
        SessionBudget& budget = budgets[seg.sessionId];
        if (budget.cards >= kMaxCardsPerSessionRound) {
            const std::int64_t seq =
                seg.events.empty()
                    ? 0
                    : static_cast<std::int64_t>(seg.events.front().eventSeq);
            if (budget.headSeq < 0) {
                budget.headSeq = seq;
                budget.headFp = fp;
            }
            budget.tailSeq = seq;
            budget.tailFp = fp;
            ++budget.overflow;
            ++report.overflowDropped;
            continue;
        }
        std::vector<proposals::ProposalEvidence> evidence;
        evidence.reserve(seg.events.size());
        for (const auto& ev : seg.events) {
            evidence.push_back(proposals::ProposalEvidence{
                ev.sessionId, static_cast<std::int64_t>(ev.eventSeq),
                seg.text, now});
        }
        // 需-3 §2.1：前缀在卡文本构造处打——采集无本地分类能力，草稿默认
        // [踩坑|会话采集]（四类语义表细则 §1.2 由人审共用，编辑框改文本即改标，
        // 不做类型选择器 UI §7-7）。同一份带前缀文本同源进直存卡与提炼队列
        // （路②补标类型来源）；执行面零改动：addDirect/撤销回填原样透传。
        const std::string prefixedText =
            std::string(kCollectCardPrefix) + body;
        // G-1 提炼暂存：过闸段入队（commit 之前；崩溃重放唯一键去重）。入队
        // 失败不阻断卡片链（主通道），该段提炼机会丢失如实留痕
        if (inferQueue != nullptr) {
            const std::int64_t startSeq =
                seg.events.empty()
                    ? 0
                    : static_cast<std::int64_t>(seg.events.front().eventSeq);
            if (auto q = inferQueue->enqueue(seg.sessionId, startSeq,
                                             prefixedText, now);
                q.isFail()) {
                LOG_WARN("提炼入队失败: " + q.errorMessage());
            }
        }
        auto card = proposals::Curator::proposeAdd(store, prefixedText,
                                                   evidence,
                                                   kUnassessedConfidence, now);
        if (card.isFail()) {
            // 单段建卡失败不中断整轮（下轮水位未推进该段事件级去重兜底）
            LOG_WARN("采集建卡失败: " + card.errorMessage());
            continue;
        }
        ++report.cardsCreated;
        ++budgets[seg.sessionId].cards;  // BD 批：预算只数成功落库的卡
        RoundNote n;
        n.blocked = false;
        n.sessionId = seg.sessionId;
        n.text = "采集 · 会话 " + seg.sessionId + " 生成待审新增卡";
        if (onNote) onNote(n);
        report.notes.push_back(std::move(n));
    }
    // BD 批（bug191）：溢出样本收集 + 日志留痕（不静默）——每会话头/尾各一条
    //（同段合一）；行内只有会话名/计数/指纹前 8 位（哈希非正文，脱敏红线不破）
    for (const auto& kv : budgets) {
        const SessionBudget& b = kv.second;
        if (b.overflow == 0) continue;
        report.overflowSamples.push_back(
            OverflowSample{kv.first, b.headSeq, b.headFp});
        if (b.tailSeq != b.headSeq) {
            report.overflowSamples.push_back(
                OverflowSample{kv.first, b.tailSeq, b.tailFp});
        }
        LOG_WARN("采集限流：会话 " + kv.first + " 本轮溢出丢弃 " +
                 std::to_string(b.overflow) + " 段（单轮单会话上限 " +
                 std::to_string(kMaxCardsPerSessionRound) + "，头样本 fp=" +
                 b.headFp.substr(0, 8) + " 尾样本 fp=" + b.tailFp.substr(0, 8) +
                 "，样本引用见 CollectRoundReport.overflowSamples）");
    }
    // 记账：处理完才推进水位/事件（拦截段也记已处理——不入提案但不重复扫）
    if (auto c = collector.commit(segments.value()); c.isFail()) {
        return core::Result<CollectRoundReport>::fail(c.errorCode(),
                                                      c.errorMessage());
    }
    return core::Result<CollectRoundReport>::ok(std::move(report));
}

core::Result<int> runDecayScan(proposals::ProposalStore& store,
                               const proposals::MemoryListFn& memories,
                               int idleThresholdDays,
                               const std::function<void(const RoundNote&)>& onNote) {
    auto created = proposals::Curator::proposeForget(
        store, memories, idleThresholdDays, nowSec());
    if (created.isFail()) {
        return core::Result<int>::fail(created.errorCode(),
                                       created.errorMessage());
    }
    if (onNote && !created.value().empty()) {
        RoundNote n;
        n.blocked = false;
        n.text = "整理 · 闲置扫描生成 " +
                 std::to_string(created.value().size()) + " 张待审遗忘卡";
        onNote(n);
    }
    return core::Result<int>::ok(static_cast<int>(created.value().size()));
}

}  // namespace module::memory::pipeline
