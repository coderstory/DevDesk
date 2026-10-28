// tests/test_modules/test_memory_proposals.cpp
// 卡片数据层/整理器/计量单测（S4-D）。红线：状态机三终态写定不改；baseVersion
// 失配自动过期；同一 undoOf 至多一张未决撤销卡；执行失败保持待审；金额估算单调。
#include "doctest.h"

#include "module_memory/gateway/telemem_client.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace module::memory::proposals;
using module::memory::gateway::IMemoryRetriever;

namespace {

std::filesystem::path tmpDb(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("memcpp-prop-" + tag + "-" +
                std::to_string(std::time(nullptr)) + "-" +
                std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir / "memcpp.db";
}

// 假引擎：记录调用轨迹
class FakeEngine : public IMemoryRetriever {
public:
    std::vector<std::string> calls;
    bool fail = false;
    core::Result<std::vector<module::memory::gateway::MemoryHit>> search(
        const std::string&, int, int) override {
        return core::Result<std::vector<module::memory::gateway::MemoryHit>>::
            ok({});
    }
    core::Result<std::string> addDirect(const std::string& text) override {
        if (fail) {
            return core::Result<std::string>::fail(
                core::ErrorCode::HttpError, "引擎工具错误");
        }
        calls.push_back("add:" + text);
        return core::Result<std::string>::ok("42");
    }
    core::Result<void> setMemoryForgotten(const std::string& memoryId,
                                          bool forgotten) override {
        if (fail) {
            return core::Result<void>::fail(core::ErrorCode::HttpError,
                                            "引擎工具错误");
        }
        calls.push_back(std::string("mark:") + memoryId + ":" +
                        (forgotten ? "1" : "0"));
        return core::Result<void>::ok();
    }
};

ProposalExecutor engineExecutor(FakeEngine& engine) {
    return [&engine](const Proposal& p) { return executeProposal(p, engine); };
}

}  // namespace

REGISTER_TEST(memory_proposal_state_machine,
              "memory: 状态机——pending 唯一起点；accept/reject 终态幂等；TTL 过期回收", {
    auto opened = ProposalStore::open(tmpDb("sm"));
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    const std::int64_t now = 1769472000;

    auto add = store.create("add", {}, "新记忆正文", "fact", "devdesk", {},
                            0.9, "collect", "assistant_derived", now, -1);
    DOCTEST_REQUIRE(add.isOk());
    DOCTEST_CHECK_EQ(add.value().status, std::string("pending"));
    DOCTEST_CHECK(add.value().id.compare(0, 5, "prop_") == 0);
    // TTL 默认 30 天（B4 设计值）
    DOCTEST_CHECK_EQ(add.value().expiresAt - add.value().createdAt,
                     static_cast<std::int64_t>(30 * 86400));

    FakeEngine engine;
    // 未到 TTL 正常接受：accepted + decidedAt/appliedAt 落定 + 引擎真调用
    auto ok = store.accept(add.value().id, now + 10,
                           [](std::int64_t) {
                               return core::Result<std::int64_t>::ok(0);
                           },
                           engineExecutor(engine));
    DOCTEST_REQUIRE(ok.isOk());
    DOCTEST_CHECK_EQ(ok.value().status, std::string("accepted"));
    DOCTEST_CHECK(ok.value().decidedAt >= 0);
    DOCTEST_CHECK(ok.value().appliedAt >= 0);
    DOCTEST_CHECK_EQ(engine.calls.size(), static_cast<std::size_t>(1));
    // 引擎条目 id 回填（撤销需要知道标记谁）
    DOCTEST_REQUIRE_EQ(ok.value().targetIds.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(ok.value().targetIds[0], static_cast<std::int64_t>(42));

    // 终态幂等：重复接受/拒绝 = 直接返回不做事
    auto dup = store.accept(ok.value().id, now + 20,
                            [](std::int64_t) {
                                return core::Result<std::int64_t>::ok(0);
                            },
                            engineExecutor(engine));
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK_EQ(dup.value().status, std::string("accepted"));
    DOCTEST_CHECK_EQ(engine.calls.size(), static_cast<std::size_t>(1));
    auto dupRej = store.reject(ok.value().id, now + 20);
    DOCTEST_REQUIRE(dupRej.isOk());
    DOCTEST_CHECK_EQ(dupRej.value().status, std::string("accepted"));

    // 拒绝路径
    auto rej = store.create("add", {}, "另一条", "fact", "devdesk", {}, 0.5,
                            "collect", "assistant_derived", now, -1);
    DOCTEST_REQUIRE(rej.isOk());
    auto rejDone = store.reject(rej.value().id, now + 10);
    DOCTEST_REQUIRE(rejDone.isOk());
    DOCTEST_CHECK_EQ(rejDone.value().status, std::string("rejected"));

    // TTL 过期：pending 且 expiresAt < now → expired
    auto old = store.create("add", {}, "旧提案", "fact", "devdesk", {}, 0.5,
                            "collect", "assistant_derived", now - 31 * 86400,
                            -1);
    DOCTEST_REQUIRE(old.isOk());
    auto swept = store.expireSweep(now);
    DOCTEST_REQUIRE(swept.isOk());
    auto oldAfter = store.get(old.value().id);
    DOCTEST_REQUIRE(oldAfter.isOk());
    DOCTEST_CHECK_EQ(oldAfter.value().status, std::string("expired"));
    return true;
})

REGISTER_TEST(memory_proposal_base_version_and_apply_failure,
              "memory: baseVersion 失配自动过期；执行失败保持待审且失败原因可见", {
    auto opened = ProposalStore::open(tmpDb("bv"));
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    const std::int64_t now = 1769472000;

    // 遗忘卡：目标 7，出卡时版本 3
    auto forget = store.create("forget", {7}, "", "", "devdesk", {}, 0.8,
                               "decay", "assistant_derived", now, 3);
    DOCTEST_REQUIRE(forget.isOk());
    DOCTEST_CHECK_EQ(forget.value().baseVersion, static_cast<std::int64_t>(3));

    FakeEngine engine;
    // 目标已变（版本 4 ≠ 3）→ expired，引擎零调用
    auto changed = store.accept(
        forget.value().id, now + 5,
        [](std::int64_t) { return core::Result<std::int64_t>::ok(4); },
        engineExecutor(engine));
    DOCTEST_REQUIRE(changed.isOk());
    DOCTEST_CHECK_EQ(changed.value().status, std::string("expired"));
    DOCTEST_CHECK(engine.calls.empty());

    // 版本一致但引擎失败 → 保持 pending（可重试），失败原因进审计
    auto forget2 = store.create("forget", {8}, "", "", "devdesk", {}, 0.8,
                                "decay", "assistant_derived", now, 1);
    DOCTEST_REQUIRE(forget2.isOk());
    engine.fail = true;
    auto failed = store.accept(
        forget2.value().id, now + 5,
        [](std::int64_t) { return core::Result<std::int64_t>::ok(1); },
        engineExecutor(engine));
    DOCTEST_REQUIRE(failed.isOk());
    DOCTEST_CHECK_EQ(failed.value().status, std::string("pending"));
    DOCTEST_CHECK(failed.value().appliedAt < 0);
    // 恢复后重试成功
    engine.fail = false;
    auto retried = store.accept(
        forget2.value().id, now + 15,
        [](std::int64_t) { return core::Result<std::int64_t>::ok(1); },
        engineExecutor(engine));
    DOCTEST_REQUIRE(retried.isOk());
    DOCTEST_CHECK_EQ(retried.value().status, std::string("accepted"));
    return true;
})

REGISTER_TEST(memory_proposal_undo_chain,
              "memory: 撤销链——undoOf 指回原卡、同一 undoOf 至多一张未决卡、撤销执行为逆动作", {
    auto opened = ProposalStore::open(tmpDb("undo"));
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    const std::int64_t now = 1769472000;

    auto add = store.create("add", {}, "会被撤销的新增", "fact", "devdesk", {},
                            0.9, "collect", "assistant_derived", now, -1);
    DOCTEST_REQUIRE(add.isOk());
    FakeEngine engine;
    auto accepted = store.accept(
        add.value().id, now + 5,
        [](std::int64_t) { return core::Result<std::int64_t>::ok(0); },
        engineExecutor(engine));
    DOCTEST_REQUIRE(accepted.isOk());
    // 未决撤销卡唯一：重复 createUndo 复用同一张并刷新
    auto vok = [](std::int64_t) { return core::Result<std::int64_t>::ok(9); };
    auto u1 = store.createUndo(add.value().id, now + 10, vok);
    DOCTEST_REQUIRE(u1.isOk());
    DOCTEST_CHECK_EQ(u1.value().undoOf, add.value().id);
    DOCTEST_CHECK_EQ(u1.value().kind, std::string("invalidate"));
    DOCTEST_CHECK_EQ(u1.value().baseVersion, static_cast<std::int64_t>(9));
    auto u2 = store.createUndo(add.value().id, now + 20, vok);
    DOCTEST_REQUIRE(u2.isOk());
    DOCTEST_CHECK_EQ(u2.value().id, u1.value().id);  // 复用，不建新卡
    // 撤销接受 = 逆动作（给新增条目打失效标记），原卡保持 accepted
    auto undone = store.accept(u1.value().id, now + 30, vok,
                               engineExecutor(engine));
    DOCTEST_REQUIRE(undone.isOk());
    DOCTEST_CHECK_EQ(undone.value().status, std::string("accepted"));
    DOCTEST_CHECK_EQ(engine.calls.back(), std::string("mark:42:1"));
    auto original = store.get(add.value().id);
    DOCTEST_REQUIRE(original.isOk());
    DOCTEST_CHECK_EQ(original.value().status, std::string("accepted"));  // 写定不改
    DOCTEST_CHECK(store.isUndone(add.value().id));
    return true;
})

REGISTER_TEST(memory_curator_propose_add_and_forget,
              "memory: 整理器两类提案——新增必审卡；遗忘按闲置天数扫描且不重复建卡", {
    auto opened = ProposalStore::open(tmpDb("cur"));
    DOCTEST_REQUIRE(opened.isOk());
    auto store = std::move(opened.value());
    const std::int64_t now = 1769472000;

    // ① 新增必审
    auto addCard = Curator::proposeAdd(store, "采集到的结论正文", {}, 0.9, now);
    DOCTEST_REQUIRE(addCard.isOk());
    DOCTEST_CHECK_EQ(addCard.value().status, std::string("pending"));
    DOCTEST_CHECK_EQ(addCard.value().source, std::string("collect"));
    // 接受前 telemem 不入库（FakeEngine 未被调用）
    FakeEngine engine;
    DOCTEST_CHECK(engine.calls.empty());
    auto applied = store.accept(
        addCard.value().id, now + 5,
        [](std::int64_t) { return core::Result<std::int64_t>::ok(0); },
        engineExecutor(engine));
    DOCTEST_REQUIRE(applied.isOk());
    DOCTEST_CHECK_EQ(engine.calls.size(), static_cast<std::size_t>(1));

    // ② 遗忘扫描：闲置 90 天的 1、2 建卡；最近使用过的 3、无使用记录的 4 不动
    auto memories = []() {
        return core::Result<std::vector<MemorySummary>>::ok(
            std::vector<MemorySummary>{
                {1, "久未想起的记忆A", now - 90 * 86400, 5},
                {2, "久未想起的记忆B", now - 95 * 86400, 2},
                {3, "刚被想起的记忆C", now - 3600, 7},
                {4, "从没被想起过的D", 0, 1}});
    };
    auto created = Curator::proposeForget(store, memories, 60, now);
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_CHECK_EQ(created.value().size(), static_cast<std::size_t>(2));
    // 二次扫描不重复建卡
    auto again = Curator::proposeForget(store, memories, 60, now);
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK(again.value().empty());
    return true;
})

REGISTER_TEST(memory_usage_ledger_injection_and_cost_split,
              "memory: 注入账与成本账分轨——inject_* 不进 token/金额聚合、注入今日读数、最近行", {
    auto opened = UsageLedger::open(tmpDb("ledger2"));
    DOCTEST_REQUIRE(opened.isOk());
    auto ledger = std::move(opened.value());
    const std::int64_t now = 1769472000;

    auto ins = ledger.recordInjection(3, 214, now);
    DOCTEST_REQUIRE(ins.isOk());
    // 成本聚合排除 inject_*（注入不调云端，无金额）
    auto today = ledger.todayTokens(now + 10);
    DOCTEST_REQUIRE(today.isOk());
    DOCTEST_CHECK_EQ(today.value(), static_cast<long long>(0));
    auto cost = ledger.todayCostMilliYuan(now + 10);
    DOCTEST_REQUIRE(cost.isOk());
    DOCTEST_CHECK_EQ(cost.value(), static_cast<long long>(0));
    // 注入预算环读数
    auto items = ledger.todayInjectItems(now + 10);
    DOCTEST_REQUIRE(items.isOk());
    DOCTEST_CHECK_EQ(items.value(), static_cast<long long>(3));
    auto chars = ledger.todayInjectChars(now + 10);
    DOCTEST_REQUIRE(chars.isOk());
    DOCTEST_CHECK_EQ(chars.value(), static_cast<long long>(214));

    // 计一笔真实成本 → 聚合含它、注入读数不受影响
    DOCTEST_REQUIRE(ledger.record("llm_input", 100000, now + 20).isOk());
    auto today2 = ledger.todayTokens(now + 30);
    DOCTEST_REQUIRE(today2.isOk());
    DOCTEST_CHECK_EQ(today2.value(), static_cast<long long>(100000));
    auto items2 = ledger.todayInjectItems(now + 30);
    DOCTEST_REQUIRE(items2.isOk());
    DOCTEST_CHECK_EQ(items2.value(), static_cast<long long>(3));

    // 最近行降序 + 字段齐全
    auto rows = ledger.recentRows(10);
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE(rows.value().size() == static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(rows.value()[0].kind, std::string("llm_input"));
    DOCTEST_CHECK_EQ(rows.value()[2].kind, std::string("inject_items"));
    return true;
})

REGISTER_TEST(memory_usage_ledger_accumulates,
              "memory: 消耗计量——token 累计落库、金额估算按调研单价、今日/本月口径", {
    auto opened = UsageLedger::open(tmpDb("ledger"));
    DOCTEST_REQUIRE(opened.isOk());
    auto ledger = std::move(opened.value());
    const std::int64_t now = 1769472000;

    // 估算口径：100 万 token 输入 = 0.8 元 = 800 毫厘
    DOCTEST_CHECK_EQ(estimateCostMilliYuan("llm_input", 1000000),
                     static_cast<long long>(800));
    DOCTEST_CHECK_EQ(estimateCostMilliYuan("llm_output", 1000000),
                     static_cast<long long>(2800));
    DOCTEST_CHECK_EQ(estimateCostMilliYuan("embed", 1000000),
                     static_cast<long long>(500));
    DOCTEST_CHECK_EQ(estimateCostMilliYuan("llm_input", -5),
                     static_cast<long long>(0));

    auto c1 = ledger.record("llm_input", 100000, now);
    DOCTEST_REQUIRE(c1.isOk());
    auto c2 = ledger.record("embed", 50000, now + 60);
    DOCTEST_REQUIRE(c2.isOk());
    DOCTEST_CHECK_EQ(c1.value(), static_cast<long long>(80));
    DOCTEST_CHECK_EQ(c2.value(), static_cast<long long>(25));
    auto today = ledger.todayTokens(now + 120);
    DOCTEST_REQUIRE(today.isOk());
    DOCTEST_CHECK_EQ(today.value(), static_cast<long long>(150000));
    auto month = ledger.monthTokens(now + 120);
    DOCTEST_REQUIRE(month.isOk());
    DOCTEST_CHECK_EQ(month.value(), static_cast<long long>(150000));
    // 明天零点后不计入"今日"（ts 位移 2 天前的记录）
    auto stale = ledger.record("llm_input", 999999, now - 2 * 86400);
    DOCTEST_REQUIRE(stale.isOk());
    auto todayAfter = ledger.todayTokens(now + 120);
    DOCTEST_REQUIRE(todayAfter.isOk());
    DOCTEST_CHECK_EQ(todayAfter.value(), static_cast<long long>(150000));
    return true;
})
