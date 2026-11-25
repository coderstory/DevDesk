// tests/test_modules/test_memory_collect_wiring.cpp
// F1 产品接线集成单测：采集管线单轮直驱（引擎以 mock 形态缺席——管线零引擎依赖，
// 入库（接受卡）才需要引擎）。临时会话目录夹具，不碰真实 ~/.claude 与用户数据根。
// 钉死契约：干净段 → pending 卡；敏感段 → 拦截（审计行 + 动态流可见 + 不入提案）；
// 水位幂等（二轮零重产）；decay 清单缝 UUID 形态（F3 live 定案）。
#include "doctest.h"

#include "memory_f06_gate_fixtures.h"
#include "module_memory/collector/collector.h"
#include "module_memory/memory_module.h"
#include "module_memory/memory_module_internal.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/sanitize/sanitize_gate.h"

#include <eui/dsl.h>

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace module::memory;

namespace {

std::filesystem::path makeTempDir(const std::string& tag) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("memcpp-wiring-" + tag + "-" +
                                 std::to_string(std::time(nullptr)) + "-" +
                                 std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeSession(const std::filesystem::path& file,
                  const std::string& content) {
    std::ofstream f(file, std::ios::binary);
    f << content;
}

const std::string kCleanLine =
    R"({"type":"user","sessionId":"sess-clean","message":{"role":"user","content":"DevDesk 的记忆页走 PanelState 取数，采集管线把会话事件装配为约四千字节分段，逐段过脱敏闸与低价值过滤后生成待审记忆卡，正文长度需要越过极短段门槛。"}})";
const std::string kDirtyLine =
    R"({"type":"user","sessionId":"sess-dirty","message":{"role":"user","content":"我的手机号是 13800138000，帮我注册"}})";

}  // namespace

REGISTER_TEST(memory_collect_pipeline_round,
              "memory: F1 接线——管线单轮：干净段出 pending 卡；敏感段拦截（审计行+动态流 note，零卡）；二轮幂等", {
    const auto dir = makeTempDir("round");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    writeSession(projects / "sess-clean.jsonl", kCleanLine + "\n");
    writeSession(projects / "sess-dirty.jsonl", kDirtyLine + "\n");

    std::vector<std::string> auditLines;  // 脱敏闸审计行（生产同源回调形态）
    sanitize::SanitizeGate gate([&auditLines](const std::string& line) {
        auditLines.push_back(line);
    });

    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    std::vector<pipeline::RoundNote> notes;
    auto r = pipeline::runCollectRound(
        collector, gate, store,
        [&notes](const pipeline::RoundNote& n) { notes.push_back(n); });
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().segments, 2);
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 1);
    DOCTEST_CHECK_EQ(r.value().blocked, 1);

    // 拦截段有审计行（闸回调）+ 动态流可见 note（Blocked 形态）
    bool sawAudit = false;
    for (const auto& l : auditLines) {
        if (l.find("sanitize:applied hits=") != std::string::npos) sawAudit = true;
    }
    DOCTEST_CHECK(sawAudit);
    bool sawBlockedNote = false;
    for (const auto& n : notes) {
        if (n.blocked && n.text.find("拦截") != std::string::npos &&
            n.sessionId == "sess-dirty") {
            sawBlockedNote = true;
            // 动态流行零原文（内容红线）
            DOCTEST_CHECK(n.text.find("13800138000") == std::string::npos);
        }
    }
    DOCTEST_CHECK(sawBlockedNote);

    // store 出 pending 卡：正文零敏感原文、来源 collect
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pending.value()[0].status, std::string("pending"));
    DOCTEST_CHECK_EQ(pending.value()[0].source, std::string("collect"));
    DOCTEST_CHECK(pending.value()[0].resultText.find("13800138000") ==
                  std::string::npos);
    DOCTEST_CHECK(pending.value()[0].resultText.find("PanelState") !=
                  std::string::npos);
    // 需-3 §2.1：卡文本构造处带前缀（默认 [踩坑|会话采集]，人审编辑框可改）
    DOCTEST_CHECK(pending.value()[0].resultText.rfind("[踩坑|会话采集] ", 0) ==
                  0);

    // 二轮幂等：水位已推进，零重产零重建卡
    auto r2 = pipeline::runCollectRound(
        collector, gate, store, {});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().segments, 0);
    DOCTEST_CHECK_EQ(r2.value().cardsCreated, 0);
    auto pending2 = store.listPending();
    DOCTEST_REQUIRE(pending2.isOk());
    DOCTEST_CHECK_EQ(pending2.value().size(), static_cast<std::size_t>(1));
    return true;
})

REGISTER_TEST(memory_collect_decay_scan_uuid_form,
              "memory: F1/F3 接线——decay 扫描周期体：UUID 清单缝出真形态 forget 卡；清单缝缺席 fail-closed 零卡", {
    const auto dir = makeTempDir("decay");
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    const std::int64_t now = 1769472000;
    const std::string kUuid = "b7e23ec2-9a1d-4c55-8f0a-1b2c3d4e5f60";

    // live 定案形态：清单给 UUID 串 → 卡 targetIds 原样（整型错位形态必挂执行）
    proposals::MemoryListFn live = [&kUuid]() {
        return core::Result<std::vector<proposals::MemorySummary>>::ok(
            std::vector<proposals::MemorySummary>{
                {kUuid, "闲置很久的引擎记忆", now - 120 * 86400, 3}});
    };
    std::vector<pipeline::RoundNote> notes;
    auto r = pipeline::runDecayScan(
        store, live, 60, [&notes](const pipeline::RoundNote& n) {
            notes.push_back(n);
        });
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 1);
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pending.value()[0].kind, std::string("forget"));
    DOCTEST_REQUIRE_EQ(pending.value()[0].targetIds.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pending.value()[0].targetIds[0], kUuid);

    // 生产现状：引擎无列举缝 → fail-closed（不出假卡），缺口如实上报
    proposals::MemoryListFn absent = []() {
        return core::Result<std::vector<proposals::MemorySummary>>::fail(
            core::ErrorCode::NotImplemented, "引擎无列举缝");
    };
    auto r2 = pipeline::runDecayScan(store, absent, 60, {});
    DOCTEST_REQUIRE(r2.isFail());
    DOCTEST_CHECK_EQ(r2.errorCode(), core::ErrorCode::NotImplemented);
    auto pending2 = store.listPending();
    DOCTEST_REQUIRE(pending2.isOk());
    DOCTEST_CHECK_EQ(pending2.value().size(), static_cast<std::size_t>(1));
    return true;
})

// ── 审核页工具条几何（TODO §A P2 批量标签窄窗重叠）──
// 无头 compose 断言沿用 test_mcp_page.cpp 约定：raw eui::Ui + 真 layout
//（compose 回调体内取 frame 全是 0）。真渲染字宽大于估算（实测 1000 宽单行
// 只剩 ~28px 缝、文字贴死），最小窗折两行：状态 chips 降第二行、工具条 88 高；
// 1200 / 1600 单行 56 不变。chips 恒不裁切、不压批量开关组；开关钉右锚
//（toolbar 右缘 −52）三档同值不漂移。

namespace {

void composeReviewFrame(module::memory::MemoryModule& mod, eui::Ui& ui,
                        float screenW, float screenH) {
    eui::Screen s;
    s.width = screenW;
    s.height = screenH;
    s.scale = 1.0f;
    ui.begin("memory_review_toolbar");
    mod.compose(ui, s);
    ui.end();
    ui.layout(screenW, screenH);
}

bool checkToolbarChain(eui::Ui& ui, float screenW) {
    const float contentW = screenW - 64.0f;  // pagePad = spacingLarge×2 = 32
    const auto* toolbar = ui.find("memory.rv.toolbar");
    const auto* lastChip = ui.find("memory.rv.chipSt.1");
    const auto* srcDd = ui.find("memory.rv.srcdd");
    const auto* batchBtn = ui.find("memory.rv.batchbtn.bg");
    DOCTEST_REQUIRE(toolbar != nullptr);
    DOCTEST_REQUIRE(lastChip != nullptr);
    DOCTEST_REQUIRE(srcDd != nullptr);
    DOCTEST_REQUIRE(batchBtn != nullptr);

    DOCTEST_CHECK_MESSAGE(
        std::fabs(toolbar->frame.width - contentW) < 0.5f,
        "工具条内容宽 " + std::to_string(toolbar->frame.width) +
            " ≠ 窗宽 " + std::to_string(screenW) + " − 64");
    // 高度按内容区分档（need:181 §3.2 定高 52；<860 折两行 88 分支保留）
    const float expectH = (contentW < 860.0f) ? 88.0f : 52.0f;
    DOCTEST_CHECK_MESSAGE(std::fabs(toolbar->frame.height - expectH) < 0.5f,
                          "工具条高 " + std::to_string(toolbar->frame.height) +
                              " ≠ " + std::to_string(expectH));
    // chips 行右缘不出工具条（窄窗不裁切）
    DOCTEST_CHECK_MESSAGE(
        lastChip->frame.x + lastChip->frame.width <=
            toolbar->frame.x + toolbar->frame.width - 8.0f,
        "状态 chips 右缘 " +
            std::to_string(lastChip->frame.x + lastChip->frame.width) +
            " 越出工具条右缘 " +
            std::to_string(toolbar->frame.x + toolbar->frame.width));
    // 状态 chips 不压「批量处理」按钮（≥12 中间距令牌；need:183 开关→按钮）
    DOCTEST_CHECK_MESSAGE(
        batchBtn->frame.x - (lastChip->frame.x + lastChip->frame.width) >=
            12.0f,
        "批量按钮左缘 " + std::to_string(batchBtn->frame.x) + " 与 chips 右缘 " +
            std::to_string(lastChip->frame.x + lastChip->frame.width) +
            " 间距不足 12");
    // 来源下拉（第一行）不压批量按钮；按钮钉 toolbar 右缘 −16（右锚群组）
    DOCTEST_CHECK(srcDd->frame.x + srcDd->frame.width <= batchBtn->frame.x);
    DOCTEST_CHECK(
        std::fabs((toolbar->frame.x + toolbar->frame.width) -
                  (batchBtn->frame.x + batchBtn->frame.width) - 16.0f) < 0.5f);
    return true;
}

}  // namespace

REGISTER_TEST(memory_review_toolbar_narrow_1200_no_overlap,
              "memory: 审核工具条（TODO §A P2）——1200 窄窗 chips 不裁切、不压批量开关标签", {
    MemoryModule mod;
    mod.setActiveViewForTest(1);
    eui::Ui ui;
    composeReviewFrame(mod, ui, 1200.0f, 800.0f);
    if (!checkToolbarChain(ui, 1200.0f)) return false;
    return true;
})

REGISTER_TEST(memory_review_toolbar_min_window_800_wrap_two_rows,
              "memory: 审核工具条——内容区 <860（800×640 窗）状态 chips 折第二行（工具条 88 高）不裁切、不压批量按钮", {
    MemoryModule mod;
    mod.setActiveViewForTest(1);
    eui::Ui ui;
    composeReviewFrame(mod, ui, 800.0f, 640.0f);
    if (!checkToolbarChain(ui, 800.0f)) return false;
    // 折行证据：工具条 88 高；状态 chips 在第二行（y = toolbar.y + 54，
    // need298 M6：chips 行移到「批量处理」钮 y10~42 之下，独占全宽不竞争）
    const auto* toolbar = ui.find("memory.rv.toolbar");
    const auto* stateChip = ui.find("memory.rv.chipSt.0");
    DOCTEST_REQUIRE(toolbar != nullptr);
    DOCTEST_REQUIRE(stateChip != nullptr);
    DOCTEST_CHECK_MESSAGE(std::fabs(toolbar->frame.height - 88.0f) < 0.5f,
                          "工具条高 " + std::to_string(toolbar->frame.height) +
                              " ≠ 88（窄窗未折两行）");
    DOCTEST_CHECK_MESSAGE(std::fabs(stateChip->frame.y -
                                    (toolbar->frame.y + 54.0f)) < 0.5f,
                          "状态 chips y=" + std::to_string(stateChip->frame.y) +
                              " 未落在第二行 " +
                              std::to_string(toolbar->frame.y + 54.0f));
    return true;
})

REGISTER_TEST(memory_review_toolbar_wide_1600_anchored,
              "memory: 审核工具条——1600 宽窗单行不变：chips 不重叠 + 开关右锚不漂移", {
    MemoryModule mod;
    mod.setActiveViewForTest(1);
    eui::Ui ui;
    composeReviewFrame(mod, ui, 1600.0f, 1000.0f);
    if (!checkToolbarChain(ui, 1600.0f)) return false;
    return true;
})

// ── bug191 采集风暴治理（F-01 指纹去重 / F-02 单轮上限 / F-03 低价值过滤 /
//    F-04 TTL 接线 / F-06 confidence 未评估语义）─────────────────────

namespace {

std::string userLine(const std::string& sess, const std::string& text) {
    return "{\"type\":\"user\",\"sessionId\":\"" + sess +
           "\",\"message\":{\"role\":\"user\",\"content\":\"" + text + "\"}}";
}

// 中文正文生成（避开脱敏命中与噪声模式；足够长过极短段门槛）
std::string prose(const std::string& seed, int rep) {
    std::string t;
    for (int i = 0; i < rep; ++i) t += seed;
    return t;
}

}  // namespace

REGISTER_TEST(memory_collect_storm_fingerprint_dedup,
              "memory: bug191 F-01——同内容段去重：同轮双段只出 1 卡；跨轮同内容零卡", {
    const auto dir = makeTempDir("storm-dedup");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    sanitize::SanitizeGate gate;
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    // 同内容双会话（正文 ~3KB 中文，单会话单段）
    const std::string body =
        prose("记忆采集管线的内容级指纹去重验证正文，段落足够长以越过极短段门槛并保持语义完整。", 15);
    writeSession(projects / "sess-a.jsonl", userLine("sess-a", body) + "\n");
    writeSession(projects / "sess-b.jsonl", userLine("sess-b", body) + "\n");

    auto r = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().segments, 2);
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 1);  // 同轮段间去重：只第一张成卡
    DOCTEST_CHECK_EQ(r.value().dedupSkipped, 1);  // BD 批（bug191）：deduped 计数钉死
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));

    // 跨轮同内容（新会话）→ 零卡（pending 指纹集命中，不建卡不排队）
    writeSession(projects / "sess-c.jsonl", userLine("sess-c", body) + "\n");
    auto r2 = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().segments, 1);
    DOCTEST_CHECK_EQ(r2.value().cardsCreated, 0);
    DOCTEST_CHECK_EQ(r2.value().dedupSkipped, 1);  // BD 批（bug191）：跨轮 deduped 计数
    auto pending2 = store.listPending();
    DOCTEST_REQUIRE(pending2.isOk());
    DOCTEST_CHECK_EQ(pending2.value().size(), static_cast<std::size_t>(1));
    return true;
})

REGISTER_TEST(memory_collect_storm_round_cap,
              "memory: bug191 F-02+BD 批——单轮单会话上限 10：60 段首轮 10 卡+溢出 40 计数+头尾样本引用；次轮续产恰 10 卡零溢出；三轮清零；总量守恒（丢弃=有样本留痕的终局）", {
    const auto dir = makeTempDir("storm-cap");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    sanitize::SanitizeGate gate;
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    // 60 条互异长正文（各 >1KB → 一条一段封段），单会话
    std::string jsonl;
    for (int i = 0; i < 60; ++i) {
        jsonl += userLine("sess-cap",
                          "批次" + std::to_string(i) + "：" +
                              prose("DevDesk 采集治理单轮上限验证段落，内容互不相同以避开指纹去重。", 45)) +
                 "\n";
    }
    writeSession(projects / "sess-cap.jsonl", jsonl);

    // 首轮：poll 预算 50 段全消费；单会话出卡上限 10 → 10 卡 + 40 溢出丢弃
    auto r = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().segments, 50);
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 10);
    DOCTEST_CHECK_EQ(r.value().overflowDropped, 40);
    // 溢出不静默：头/尾样本引用（会话 + 首事件序号 + 指纹；零正文零敏感）
    DOCTEST_REQUIRE_EQ(r.value().overflowSamples.size(),
                       static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(r.value().overflowSamples[0].sessionId,
                     std::string("sess-cap"));
    DOCTEST_CHECK_EQ(r.value().overflowSamples[1].sessionId,
                     std::string("sess-cap"));
    // 头在前尾在后（段序单调）；指纹互异非空（可回溯定位被丢弃内容）
    DOCTEST_CHECK(r.value().overflowSamples[0].firstEventSeq <
                  r.value().overflowSamples[1].firstEventSeq);
    DOCTEST_CHECK(!r.value().overflowSamples[0].fingerprint.empty());
    DOCTEST_CHECK(r.value().overflowSamples[0].fingerprint !=
                  r.value().overflowSamples[1].fingerprint);

    // 次轮续产：poll 消费剩余 10 段（恰在上限内零溢出——限流不吞正常续产）
    auto r2 = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().segments, 10);
    DOCTEST_CHECK_EQ(r2.value().cardsCreated, 10);
    DOCTEST_CHECK_EQ(r2.value().overflowDropped, 0);
    DOCTEST_CHECK(r2.value().overflowSamples.empty());

    // 三轮：全消费完毕零段零卡（溢出段 commit 记已处理，终局不重放）
    auto r3 = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value().segments, 0);
    DOCTEST_CHECK_EQ(r3.value().cardsCreated, 0);

    // 总量守恒：pending = 首轮 10 + 次轮 10（40 溢出是有样本留痕的终局丢弃）
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_CHECK_EQ(pending.value().size(), static_cast<std::size_t>(20));
    return true;
})

REGISTER_TEST(memory_collect_storm_low_value_filter,
              "memory: bug191 F-03——低价值过滤：hook 指令回声段与极短段零卡，正常对照段照常成卡", {
    const auto dir = makeTempDir("storm-lowval");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    sanitize::SanitizeGate gate;
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    // ①噪声段：指令回声（>50 有效字符——区分于极短段刀）
    writeSession(projects / "sess-noise.jsonl",
                 userLine("sess-noise",
                          prose("不要使用任何工具。只回复：收到。", 12)) + "\n");
    // ②极短段：过噪声刀但有效字符 <50
    writeSession(projects / "sess-short.jsonl",
                 userLine("sess-short", "今天天气不错，适合写代码。") + "\n");
    // 正常对照段（必须照常成卡——过滤不过杀）
    writeSession(projects / "sess-ok.jsonl",
                 userLine("sess-ok",
                          prose("DevDesk 采集管线低价值过滤的正常对照段落，语义完整必须照常成卡。", 10)) + "\n");

    auto r = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().segments, 3);
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 1);
    DOCTEST_CHECK_EQ(r.value().blocked, 0);
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(pending.value()[0].resultText.find("正常对照段") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(memory_collect_storm_confidence_and_ttl_sweep,
              "memory: bug191 F-04/F-06——到期 pending 一个采集轮内自动沉降；采集卡 confidence 未评估语义不再显示高置信", {
    const auto dir = makeTempDir("storm-ttl");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    sanitize::SanitizeGate gate;
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    // 存量过期 pending（31 天前建 → expires_at = now−1d，已到期）
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    auto old = store.create("add", {}, "旧存量待审卡", "fact", "devdesk", {},
                            0.9, "collect", "assistant_derived",
                            now - 31 * 86400, -1);
    DOCTEST_REQUIRE(old.isOk());

    writeSession(projects / "sess-ok.jsonl",
                 userLine("sess-ok",
                          prose("DevDesk TTL 沉降与置信度语义验证段落，语义完整足够长以照常成卡。", 10)) + "\n");
    auto r = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 1);

    // F-04：到期 pending 在本轮内自动 expired（接线生效）
    auto oldAfter = store.get(old.value().id);
    DOCTEST_REQUIRE(oldAfter.isOk());
    DOCTEST_CHECK_EQ(oldAfter.value().status, std::string("expired"));

    // F-06：新卡 confidence 未评估（<0.5，卡面不再显示「高」置信徽章）
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(pending.value()[0].confidence > 0.0);
    DOCTEST_CHECK(pending.value()[0].confidence < 0.5);
    return true;
})

// ── need:335：配置 tab 折叠全展后尾组可达 ──
// 根因两层：① scrollView MeasureCache 键不含折叠态 → 展开后命中旧测量，
// scrollable 不重判（默认全折叠低于视口 → 永不出滚动条）；② EUI stack
// wrapContent 对绝对定位子元素测量偏小（声明几何 1424 被解析为 1320），
// 即使重算尾部仍差一截。修复：contentKey 拼折叠态签名（保重测语义）+
// contentHeightProvider 供渲染 pass 回写的实占底（need:335 双保险）。
// 几何断言走 compose 全链路（同上 composeReviewFrame），scrollContentFrom +
// offset clamp(0,maxOffset) 保证 contentH > viewportH ⇔ 任意尾部元素滚到底可视。

void composeConfigFrame(module::memory::MemoryModule& mod, eui::Ui& ui,
                        float screenW, float screenH, int frames = 2) {
    eui::Screen s;
    s.width = screenW;
    s.height = screenH;
    s.scale = 1.0f;
    // contentHeightProvider 读上一帧回写的实占底——同真实 app 每帧 compose，
    // 第 2 帧起供高与渲染同源；首帧兜底视口高。
    for (int f = 0; f < frames; ++f) {
        ui.begin("memory_config_scroll_test");
        mod.compose(ui, s);
        ui.end();
        ui.layout(screenW, screenH);
    }
}

REGISTER_TEST(memory_config_scroll_tail_group_reachable_when_expanded,
              "memory need:335: 配置 tab 全展后内容高超视口（尾组可达），供高随折叠态即时重算", {
    module::memory::MemoryModule mod;
    mod.setActiveViewForTest(module::memory::kViewConfig);

    // 基线：默认全折叠（复刻实测初始态）
    eui::Ui uiFolded;
    composeConfigFrame(mod, uiFolded, 900.0f, 560.0f);
    const auto* contentF = uiFolded.find("memory.config.scroll.content");
    DOCTEST_REQUIRE(contentF != nullptr);

    // 三折叠组全展开（组 3 安全隐私常显，无折叠态）
    mod.setConfigGroupOpenForTest(0, true);
    mod.setConfigGroupOpenForTest(1, true);
    mod.setConfigGroupOpenForTest(2, true);
    eui::Ui uiOpen;
    composeConfigFrame(mod, uiOpen, 900.0f, 560.0f);
    const auto* rootO = uiOpen.find("memory.config.scroll");
    const auto* contentO = uiOpen.find("memory.config.scroll.content");
    DOCTEST_REQUIRE(rootO != nullptr);
    DOCTEST_REQUIRE(contentO != nullptr);
    const float viewportH = rootO->frame.height;
    // 可达性走滚动区供高真值：maxOffset = 供高 − 视口；滚到底（offset =
    // maxOffset）后元素可视 ⇔ maxOffset ≥ 元素底 − 视口
    const float maxOffset = rootO->scrollMaxOffset;
    DOCTEST_CHECK_MESSAGE(
        maxOffset > 0.0f,
        "全展开滚动区 maxOffset=" + std::to_string(maxOffset) +
            " 须 >0（供高超视口才出滚动）");
    // 尾组最深交互件（安全隐私「导出全部记忆」钮）：滚到底须完整可视
    const auto* exportBtn = uiOpen.find("memory.cfg.data.export");
    DOCTEST_REQUIRE(exportBtn != nullptr);
    // 元素 frame 是页面绝对坐标，减内容列绝对顶换回 0 基相对坐标
    const float relBottom =
        exportBtn->frame.y + exportBtn->frame.height - contentO->frame.y;
    DOCTEST_CHECK_MESSAGE(
        maxOffset >= relBottom - viewportH,
        "maxOffset " + std::to_string(maxOffset) + " 不足导出钮相对底 " +
            std::to_string(relBottom) + " − 视口 " + std::to_string(viewportH) +
            "（尾组不可达）");
    // 供高随折叠态即时重算：全展 maxOffset 须大于全折叠（修复前固定供高）
    const float maxOffsetF = uiFolded.find("memory.config.scroll")->scrollMaxOffset;
    DOCTEST_CHECK_MESSAGE(
        maxOffset > maxOffsetF,
        "全展 maxOffset " + std::to_string(maxOffset) + " 未大于全折叠 " +
            std::to_string(maxOffsetF) + "（供高未随折叠态重算）");
    return true;
})

// ── need:436：配置 tab 滚动两边界（默认窗矮内容不出滚动 / 展开后尾件可达）──
// 无滚动缺陷已由 need:335（ae96cb3a）修复（contentKey 折叠态签名 +
// contentHeightProvider 渲染真值供高）。本用例锁另一半验收口径——「内容不足
// 不出滚动」，并覆盖 need:335 回归锁未包含的组 4（安装镜像源，bug476 新增）
// 展开后的尾部可达性：供高若只到旧尾组，组 4 末行会滚不到。
REGISTER_TEST(memory_config_scroll_folded_fits_and_last_group_reachable,
              "memory need:436: 配置 tab 默认窗全折叠内容不足不出滚动；组 4 展开后末行滚到底可达", {
    module::memory::MemoryModule mod;
    mod.setActiveViewForTest(module::memory::kViewConfig);

    // 边界 1：默认窗（1600×1000）全折叠内容不足视口——不出滚动
    eui::Ui uiFolded;
    composeConfigFrame(mod, uiFolded, 1600.0f, 1000.0f);
    const auto* rootF = uiFolded.find("memory.config.scroll");
    DOCTEST_REQUIRE(rootF != nullptr);
    DOCTEST_CHECK_MESSAGE(
        rootF->scrollMaxOffset == 0.0f,
        "全折叠内容未超视口仍出滚动 maxOffset=" +
            std::to_string(rootF->scrollMaxOffset) + "（矮内容应零滚动）");

    // 边界 2：组 0 + 组 4 展开后内容超视口，组 4 末行（镜像说明）滚到底可达
    mod.setConfigGroupOpenForTest(0, true);
    mod.setConfigGroupOpenForTest(4, true);
    eui::Ui uiOpen;
    composeConfigFrame(mod, uiOpen, 1600.0f, 1000.0f);
    const auto* rootO = uiOpen.find("memory.config.scroll");
    const auto* contentO = uiOpen.find("memory.config.scroll.content");
    const auto* note = uiOpen.find("memory.cfg.mirror.note");
    DOCTEST_REQUIRE(rootO != nullptr);
    DOCTEST_REQUIRE(contentO != nullptr);
    DOCTEST_REQUIRE(note != nullptr);
    const float relBottom =
        note->frame.y + note->frame.height - contentO->frame.y;
    DOCTEST_CHECK_MESSAGE(
        rootO->scrollMaxOffset >= relBottom - rootO->frame.height,
        "组 4 末行相对底 " + std::to_string(relBottom) + " 超出 maxOffset " +
            std::to_string(rootO->scrollMaxOffset) + " + 视口 " +
            std::to_string(rootO->frame.height) + "（尾部不可达）");
    return true;
})

// ── F-06 L2 段级洗（need333 记忆波 1，判据=scheme-card §1 P3/P4 + 实测形态扩）──

REGISTER_TEST(memory_collect_f06_wash_redteam_and_control,
              "memory: F-06 L2 红队/对照——P3 中断段擦除/P4 前缀剥标记零残留；对照集逐字节恒等零误洗", {
    // 微型锚（判据钉死：独占行删保两侧 / 行尾后缀擦保正文 / 变体整段擦 /
    // P4 前缀剥留正文 / 干净正文不动）
    bool w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "前面\n[Request interrupted by user]\n后面", &w),
                     std::string("前面\n后面"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "顾问 ID本身就存了把[Request interrupted by user]", &w),
                     std::string("顾问 ID本身就存了把"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(
        pipeline::stripLeakMarkers(
            "工具被打断\n[Request interrupted by user for tool use]\n继续", &w),
        std::string("工具被打断\n继续"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "第一行\n[tool_error] Exit code 2\n第三行", &w),
                     std::string("第一行\nExit code 2\n第三行"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers("干净正文不动", &w),
                     std::string("干净正文不动"));
    DOCTEST_CHECK(!w);

    // P4 残留检查：输出任一行不得再以 [tool_error] 前缀开头
    const auto hasToolErrLinePrefix = [](const std::string& t) {
        std::size_t p = 0;
        while (p <= t.size()) {
            if (t.compare(p, 13, "[tool_error] ") == 0) return true;
            const std::size_t nl = t.find('\n', p);
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        return false;
    };
    // 红队 21 条（实测样本：P3 命中 5 形态 + P4 行级承载 17 条 - P1/P2 重叠）
    int washed = 0;
    for (const auto& s : devdesk::testfix::f06::kWash) {
        bool hit = false;
        const std::string out = pipeline::stripLeakMarkers(s.text, &hit);
        if (!hit) {
            DOCTEST_CHECK_MESSAGE(false, std::string("红队应命中洗: ") + s.id);
        } else {
            ++washed;
        }
        DOCTEST_CHECK_MESSAGE(!out.empty(), std::string("洗后非空: ") + s.id);
        DOCTEST_CHECK_MESSAGE(
            out.find("[Request interrupted by user") == std::string::npos,
            std::string("P3 中断标记零残留: ") + s.id);
        DOCTEST_CHECK_MESSAGE(!hasToolErrLinePrefix(out),
                              std::string("P4 前缀零残留: ") + s.id);
    }
    DOCTEST_CHECK_EQ(washed, 21);
    // 对照 157 条（无任何 F-06 标记的实质样本）：零误洗 = 逐字节恒等
    int identity = 0;
    const int controlN = static_cast<int>(
        sizeof(devdesk::testfix::f06::kControl) /
        sizeof(devdesk::testfix::f06::kControl[0]));
    for (const auto& s : devdesk::testfix::f06::kControl) {
        bool hit = true;  // 反证置位：对照集不得命中
        const std::string out = pipeline::stripLeakMarkers(s.text, &hit);
        if (!hit && out == s.text) {
            ++identity;
        } else {
            DOCTEST_CHECK_MESSAGE(false,
                                  std::string("对照误洗/误改: ") + s.id);
        }
    }
    DOCTEST_CHECK_EQ(identity, controlN);  // 0 误杀硬指标
    return true;
})

// pool:bug:346（BI 审查 P2）：1024B 事件截断 × F-06 洗链交互——session_reader ③
// 截断可把 [Request interrupted by user 从中撕开（残段=真前缀，find 不命中→
// 洗不掉入卡）；洗链对行尾被撕开前缀兜底擦除。
REGISTER_TEST(memory_collect_f06_wash_torn_marker,
              "memory: F-06 洗链×1024B 截断——被撕开的中断标记残段洗后零残留", {
    // 撕开形态微锚：独占尾行整段丢 / 行尾残段保正文 / 词中撕开
    bool w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "正文段落\n[Request interru", &w),
                     std::string("正文段落"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "前面有文字[Request interrupted by us", &w),
                     std::string("前面有文字"));
    DOCTEST_CHECK(w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers(
                         "词中撕开[Request inter", &w),
                     std::string("词中撕开"));
    DOCTEST_CHECK(w);
    w = false;
    // 阈值下限负例：裸 '[' 与 <8B 碎前缀不算残段（零误洗契约不破）
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers("结尾是裸开括号 [", &w),
                     std::string("结尾是裸开括号 ["));
    DOCTEST_CHECK(!w);
    w = false;
    DOCTEST_CHECK_EQ(pipeline::stripLeakMarkers("低于下限 [Reques", &w),
                     std::string("低于下限 [Reques"));
    DOCTEST_CHECK(!w);

    // 断言主体：1000B 正文 + 42B 完整标记，事件级截断切在第 1024 字节恰落在
    // 标记内部撕出 24B 残段（ASCII 无码点回退，与 session_reader truncateUtf8
    // 行为一致）→ 洗后零残段、正文保全
    const std::string full =
        std::string(1000, 'x') + "[Request interrupted by user for tool use]";
    const std::string truncated = full.substr(0, 1024);
    DOCTEST_REQUIRE_EQ(truncated.size(), static_cast<std::size_t>(1024));
    DOCTEST_CHECK_EQ(truncated.substr(1000),
                     std::string("[Request interrupted by "));
    w = false;
    const std::string out = pipeline::stripLeakMarkers(truncated, &w);
    DOCTEST_CHECK(w);
    DOCTEST_CHECK_EQ(out, std::string(1000, 'x'));
    DOCTEST_CHECK(out.find("[Request") == std::string::npos);  // 零残段
    return true;
})

REGISTER_TEST(memory_collect_f06_wash_in_round,
              "memory: F-06 L2 接线——管线轮内洗：中断标记不入卡、两侧正文保留、report.washed 计数", {
    const auto dir = makeTempDir("f06-round");
    const auto projects = dir / "projects";
    std::filesystem::create_directories(projects);
    sanitize::SanitizeGate gate;
    auto opened = proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    collector::Collector collector(dir / "collector.db", projects);

    // pool:bug:349 夹具双修（出生即红，非产品回归）：①换行必须以 \n 转义进
    // JSON——collector 按 getline 逐行读 jsonl，真实换行把首行截成坏行 →
    // 0 事件 0 卡；②解析后 body 须 ≤kMaxEventBytes(1024B)（session_reader ③
    // 单事件截断）——原 12 rep 时标记在截断点之后永远洗不到。9 rep+标记+尾段
    // =969B，极短段门槛（≥50 有效字符）照过
    const std::string bodyJson =
        prose("F-06 管线内洗验证正文段落，语义完整足够长以照常越过极短段门槛成卡。", 9) +
        "\\n[Request interrupted by user]\\n后续两侧正文照常保留在卡内不可丢弃。";
    writeSession(projects / "sess-f06.jsonl",
                 userLine("sess-f06", bodyJson) + "\n");

    auto r = pipeline::runCollectRound(collector, gate, store, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().cardsCreated, 1);
    DOCTEST_CHECK_EQ(r.value().washed, 1);
    auto pending = store.listPending();
    DOCTEST_REQUIRE(pending.isOk());
    DOCTEST_REQUIRE_EQ(pending.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(pending.value()[0].resultText.find(
                      "[Request interrupted by user") == std::string::npos);
    DOCTEST_CHECK(pending.value()[0].resultText.find("后续两侧正文照常保留") !=
                  std::string::npos);
    return true;
})
