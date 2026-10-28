// tests/test_modules/test_memory_collect_wiring.cpp
// F1 产品接线集成单测：采集管线单轮直驱（引擎以 mock 形态缺席——管线零引擎依赖，
// 入库（接受卡）才需要引擎）。临时会话目录夹具，不碰真实 ~/.claude 与用户数据根。
// 钉死契约：干净段 → pending 卡；敏感段 → 拦截（审计行 + 动态流可见 + 不入提案）；
// 水位幂等（二轮零重产）；decay 清单缝 UUID 形态（F3 live 定案）。
#include "doctest.h"

#include "module_memory/collector/collector.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/sanitize/sanitize_gate.h"

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
    proposals::MemoryListFn live = [&kUuid, now]() {
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
