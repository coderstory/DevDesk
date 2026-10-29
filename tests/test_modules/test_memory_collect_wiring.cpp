// tests/test_modules/test_memory_collect_wiring.cpp
// F1 产品接线集成单测：采集管线单轮直驱（引擎以 mock 形态缺席——管线零引擎依赖，
// 入库（接受卡）才需要引擎）。临时会话目录夹具，不碰真实 ~/.claude 与用户数据根。
// 钉死契约：干净段 → pending 卡；敏感段 → 拦截（审计行 + 动态流可见 + 不入提案）；
// 水位幂等（二轮零重产）；decay 清单缝 UUID 形态（F3 live 定案）。
#include "doctest.h"

#include "module_memory/collector/collector.h"
#include "module_memory/memory_module.h"
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
    R"({"type":"user","sessionId":"sess-clean","message":{"role":"user","content":"DevDesk 的记忆页走 PanelState 取数"}})";
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
//（compose 回调体内取 frame 全是 0）。chips 宽改按显示字符估算后，三档窗宽
//（最小 1000 / TODO 档 1200 / 默认 1600）chips 行均须单行容下、不裁切、
// 不压右侧批量开关组；开关钉右锚（toolbar 右缘 −52）三档同值不漂移。

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
    const auto* srcChip = ui.find("memory.rv.chipS.3");
    const auto* label = ui.find("memory.rv.batchlbl");
    const auto* track = ui.find("memory.rv.batchsw.track");
    DOCTEST_REQUIRE(toolbar != nullptr);
    DOCTEST_REQUIRE(lastChip != nullptr);
    DOCTEST_REQUIRE(srcChip != nullptr);
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_REQUIRE(track != nullptr);

    DOCTEST_CHECK_MESSAGE(
        std::fabs(toolbar->frame.width - contentW) < 0.5f,
        "工具条内容宽 " + std::to_string(toolbar->frame.width) +
            " ≠ 窗宽 " + std::to_string(screenW) + " − 64");
    // 单行证据：工具条恒 56 高（chips 一旦换行/降双行必增高）
    DOCTEST_CHECK_MESSAGE(std::fabs(toolbar->frame.height - 56.0f) < 0.5f,
                          "工具条高 " + std::to_string(toolbar->frame.height) +
                              " ≠ 56（单行布局漂移）");
    // chips 行右缘不出工具条（窄窗不裁切）
    DOCTEST_CHECK_MESSAGE(
        lastChip->frame.x + lastChip->frame.width <=
            toolbar->frame.x + toolbar->frame.width - 8.0f,
        "状态 chips 右缘 " +
            std::to_string(lastChip->frame.x + lastChip->frame.width) +
            " 越出工具条右缘 " +
            std::to_string(toolbar->frame.x + toolbar->frame.width));
    // chips 行不压批量标签（TODO §A P2 重叠消除；≥12 中间距令牌）
    DOCTEST_CHECK_MESSAGE(
        label->frame.x - (lastChip->frame.x + lastChip->frame.width) >=
            12.0f,
        "批量标签左缘 " + std::to_string(label->frame.x) + " 与 chips 右缘 " +
            std::to_string(lastChip->frame.x + lastChip->frame.width) +
            " 间距不足 12");
    DOCTEST_CHECK(srcChip->frame.x + srcChip->frame.width <= label->frame.x);
    // 标签不盖开关、贴 8px；开关钉 toolbar 右缘 −52（右锚群组，三档同值）
    DOCTEST_CHECK(label->frame.x + label->frame.width <= track->frame.x);
    DOCTEST_CHECK(std::fabs(track->frame.x -
                            (label->frame.x + label->frame.width) - 8.0f) <
                  0.5f);
    DOCTEST_CHECK(
        std::fabs((toolbar->frame.x + toolbar->frame.width) -
                  (track->frame.x + track->frame.width) - 52.0f) < 0.5f);
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

REGISTER_TEST(memory_review_toolbar_min_window_1000_no_overlap,
              "memory: 审核工具条——最小窗 1000×640 chips 不裁切、不压批量开关标签（1000 宽逐页适配延续）", {
    MemoryModule mod;
    mod.setActiveViewForTest(1);
    eui::Ui ui;
    composeReviewFrame(mod, ui, 1000.0f, 640.0f);
    if (!checkToolbarChain(ui, 1000.0f)) return false;
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
