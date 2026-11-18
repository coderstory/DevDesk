// tests/test_modules/test_memory_infer.cpp
// 池3 G-1 控耗接线单测：infer 闸（硬档恰等越线 / 每小时节流）+ 暂存队列 +
// addInferred 契约 + llm_attempt 计数（token/金额聚合不受影响）。
// F-03 波 2 腿 A（池 #333）：队列出队闭环（attempts/状态流转——只进不出
// 回归红队）+ OQ-2 终稿分流三形态（成功 auto_accepted / 失败·低置信兜底）。
// 红绿自查口径：删 infer_batch 闸分支 → 硬档/节流用例必红；删重试耗尽
// 出队分支 → drain/exhaust 用例必红（旧实现深度恒不减）；删低置信前置
// 分流 → lowconf 用例必红（引擎被出机）。
// 纪律：DOCTEST_CHECK 失败即 return false，只可用于 REGISTER_TEST 函数体，
// 夹具成员函数内不得使用。
#include "doctest.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "module_memory/gateway/telemem_client.h"
#include "module_memory/pipeline/infer_batch.h"
#include "module_memory/pipeline/infer_queue.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"

using namespace module::memory;
using pipeline::InferGate;
using pipeline::InferLimits;
using pipeline::InferQueueStore;
using pipeline::inferGateDecision;

namespace {

std::filesystem::path tmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("memcpp-infer-" + tag + "-" +
                std::to_string(std::time(nullptr)) + "-" +
                std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

// 假引擎：只观测 addInferred（infer 通道）
class InferFakeEngine : public gateway::IMemoryRetriever {
public:
    std::vector<std::string> inferred;
    bool fail = false;

    core::Result<std::vector<gateway::MemoryHit>> search(const std::string&,
                                                         int, int) override {
        return core::Result<std::vector<gateway::MemoryHit>>::ok({});
    }
    core::Result<std::string> addDirect(const std::string&) override {
        return core::Result<std::string>::ok(std::string());
    }
    core::Result<std::string> addInferred(const std::string& text) override {
        if (fail) {
            return core::Result<std::string>::fail(core::ErrorCode::HttpError,
                                                   "429 rate_limit_error");
        }
        inferred.push_back(text);
        return core::Result<std::string>::ok(std::string());
    }
    core::Result<void> setMemoryForgotten(const std::string&, bool) override {
        return core::Result<void>::ok();
    }
    core::Result<std::vector<gateway::MemoryListItem>> listAll(int,
                                                               int) override {
        return core::Result<std::vector<gateway::MemoryListItem>>::ok({});
    }
    core::Result<std::int64_t> getMemoryVersion(const std::string&) override {
        return core::Result<std::int64_t>::ok(0);
    }
};

struct StepFixture {
    std::filesystem::path dir;
    InferQueueStore queue;
    proposals::ProposalStore store;  // 腿 A：终稿卡通道
    proposals::UsageLedger ledger;
    InferFakeEngine engine;
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    explicit StepFixture(const std::string& tag)
        : dir(tmpDir(tag)),
          queue(InferQueueStore::open(dir / "infer_queue.db").value()),
          store(proposals::ProposalStore::open(dir / "proposals.db").value()),
          ledger(proposals::UsageLedger::open(dir / "usage_ledger.db").value()) {}

    core::Result<pipeline::InferStepReport> step(long long hardBudget,
                                                 long long hourly) {
        InferLimits limits;
        limits.hardBudgetTokens = hardBudget;
        limits.hourlyLimit = hourly;
        return pipeline::runInferStep(queue, engine, store, ledger, limits,
                                      [](const pipeline::RoundNote&) {});
    }
};

}  // namespace

REGISTER_TEST(memory_infer_args_contract,
              "memory: add_memory 入参契约 infer=true/false（G-1 提炼 vs 直存）+ update_memory 字段（需-3 路②）", {
    const auto inferred = gateway::buildAddMemoryArgs("正文", true);
    DOCTEST_CHECK(inferred.get("infer").isBool());
    DOCTEST_CHECK(inferred.get("infer").asBool());
    DOCTEST_CHECK(inferred.get("text").asString() == "正文");
    const auto direct = gateway::buildAddMemoryArgs("正文", false);
    DOCTEST_CHECK(direct.get("infer").isBool());
    DOCTEST_CHECK_FALSE(direct.get("infer").asBool());
    // update_memory：required = [memory_id, text]（telemem 1.10.1 已核实）
    const auto upd = gateway::buildUpdateMemoryArgs("uuid-1", "新正文");
    DOCTEST_CHECK(upd.get("memory_id").asString() == "uuid-1");
    DOCTEST_CHECK(upd.get("text").asString() == "新正文");
    return true;
})

REGISTER_TEST(memory_infer_gate_decision,
              "memory: inferGateDecision 恰等越线 + 阈值≤0 关闭档", {
    // 硬档：恰等阈值即 Paused；低于则不受硬档拦
    DOCTEST_CHECK(inferGateDecision(3000000, 3000000, 0, 100) ==
                  InferGate::Paused);
    DOCTEST_CHECK(inferGateDecision(2999999, 3000000, 0, 100) ==
                  InferGate::Proceed);
    // 节流：恰等上限即 Throttled；低于则放行
    DOCTEST_CHECK(inferGateDecision(0, 3000000, 4, 4) == InferGate::Throttled);
    DOCTEST_CHECK(inferGateDecision(0, 3000000, 3, 4) == InferGate::Proceed);
    // 硬档优先于节流
    DOCTEST_CHECK(inferGateDecision(3000000, 3000000, 4, 4) ==
                  InferGate::Paused);
    // 阈值 ≤0 = 关闭该档（usage_alert 同款外部防护口径）
    DOCTEST_CHECK(inferGateDecision(99999999, 0, 0, 100) ==
                  InferGate::Proceed);
    DOCTEST_CHECK(inferGateDecision(0, 3000000, 999999, 0) ==
                  InferGate::Proceed);
    DOCTEST_CHECK(inferGateDecision(99999999, -1, 999999, -1) ==
                  InferGate::Proceed);
    return true;
})

REGISTER_TEST(memory_infer_queue_fifo_dedup,
              "memory: 暂存队列 FIFO / 唯一键去重（崩溃重放）/ 出队", {
    StepFixture f("queue");
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);
    DOCTEST_CHECK(f.queue.enqueue("s1", 10, "甲段", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s1", 11, "乙段", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s2", 10, "丙段", f.now).isOk());
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 3);
    // 崩溃重放：同 (session, start_seq) 重复入队 = no-op
    DOCTEST_CHECK(f.queue.enqueue("s1", 10, "甲段", f.now).isOk());
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 3);
    auto batch = f.queue.listBatch(2);
    DOCTEST_CHECK(batch.isOk());
    if (batch.value().size() != 2) return false;
    DOCTEST_CHECK(batch.value()[0].sessionId == "s1");
    DOCTEST_CHECK(batch.value()[0].text == "甲段");
    DOCTEST_CHECK(batch.value()[1].text == "乙段");
    DOCTEST_CHECK(f.queue.remove(batch.value()[0].id).isOk());
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 2);
    return true;
})

REGISTER_TEST(memory_infer_step_normal_records_ledger,
              "memory: 提炼步 Normal 放行 → 记账非零 + 出队（G-2 计数路径）", {
    StepFixture f("normal");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "一段四十字的采集内容", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s1", 2, "二段", f.now).isOk());
    auto stepped = f.step(3000000, 4);
    DOCTEST_CHECK(stepped.isOk());
    const auto report = stepped.value();
    DOCTEST_CHECK(report.gateBlocked == false);
    DOCTEST_CHECK(report.attempted);
    DOCTEST_CHECK(report.succeeded);
    DOCTEST_CHECK_EQ(f.engine.inferred.size(), 1);
    DOCTEST_CHECK(f.engine.inferred[0] == "一段四十字的采集内容");
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);  // 单步只出队 1 条
    // 账面：llm_input 非零 + llm_output 等额估算（OQ-3 裁决）
    DOCTEST_CHECK(f.ledger.monthTokens(f.now).value() > 0);
    const auto rows = f.ledger.recentRows(10).value();
    long long input = 0, output = 0;
    for (const auto& r : rows) {
        if (r.kind == "llm_input") input += r.tokens;
        if (r.kind == "llm_output") output += r.tokens;
    }
    DOCTEST_CHECK(input > 0);
    DOCTEST_CHECK_EQ(output, input);
    DOCTEST_CHECK_EQ(f.ledger.hourAttempts(f.now).value(), 1);
    return true;
})

REGISTER_TEST(memory_infer_step_hourly_throttle_defers,
              "memory: 每小时节流 → 达上限顺延留队，不报错不丢数据", {
    StepFixture f("throttle");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "甲", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s1", 2, "乙", f.now).isOk());
    auto first = f.step(3000000, 1);  // 上限 1：第 1 步放行
    DOCTEST_CHECK(first.isOk());
    DOCTEST_CHECK(first.value().succeeded);
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);
    auto second = f.step(3000000, 1);  // 第 2 步被节流闸拦
    DOCTEST_CHECK(second.isOk());
    DOCTEST_CHECK(second.value().gateBlocked);
    DOCTEST_CHECK(second.value().paused == false);
    DOCTEST_CHECK(second.value().attempted == false);
    DOCTEST_CHECK_EQ(f.engine.inferred.size(), 1);  // 零新增云端调用
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);   // 留队顺延，不丢弃
    return true;
})

REGISTER_TEST(memory_infer_step_hard_budget_pauses,
              "memory: 硬档暂停 → 零云端出机，队列滞留（新采集入队暂存语义）", {
    StepFixture f("hard");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "甲", f.now).isOk());
    // 预置月账面越硬档（3M 阈值）
    DOCTEST_CHECK(f.ledger.record("llm_input", 3000000, f.now).isOk());
    auto stepped = f.step(3000000, 4);
    DOCTEST_CHECK(stepped.isOk());
    DOCTEST_CHECK(stepped.value().gateBlocked);
    DOCTEST_CHECK(stepped.value().paused);
    DOCTEST_CHECK(stepped.value().attempted == false);
    DOCTEST_CHECK(f.engine.inferred.empty());
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);
    return true;
})

REGISTER_TEST(memory_infer_step_cloud_fail_keeps_queue,
              "memory: 云端失败 → attempt 已计数、留队、token 聚合不受影响", {
    StepFixture f("failpath");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "甲", f.now).isOk());
    f.engine.fail = true;
    auto stepped = f.step(3000000, 4);
    DOCTEST_CHECK(stepped.isOk());
    DOCTEST_CHECK(stepped.value().attempted);
    DOCTEST_CHECK(stepped.value().succeeded == false);
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);  // 留队待恢复补跑
    DOCTEST_CHECK_EQ(f.ledger.hourAttempts(f.now).value(), 1);  // 失败也计数
    DOCTEST_CHECK_EQ(f.ledger.monthTokens(f.now).value(), 0);   // 无 token 入账
    // 失败计数也吃小时预算：上限 1 时下一步被节流拦（重试约束防风暴）
    f.engine.fail = false;
    auto next = f.step(3000000, 1);
    DOCTEST_CHECK(next.isOk());
    DOCTEST_CHECK(next.value().gateBlocked);
    return true;
})

REGISTER_TEST(memory_infer_attempt_rows_isolated_from_budget,
              "memory: llm_attempt 行不进 token/金额聚合，跨小时窗口不计当前", {
    StepFixture f("attempt");
    DOCTEST_CHECK(f.ledger.recordAttempt(f.now).isOk());
    DOCTEST_CHECK(f.ledger.recordAttempt(f.now - 3601).isOk());  // 上一窗口
    DOCTEST_CHECK_EQ(f.ledger.hourAttempts(f.now).value(), 1);
    DOCTEST_CHECK_EQ(f.ledger.todayTokens(f.now).value(), 0);
    DOCTEST_CHECK_EQ(f.ledger.monthTokens(f.now).value(), 0);
    DOCTEST_CHECK_EQ(f.ledger.todayCostMilliYuan(f.now).value(), 0);
    return true;
})

// ── 需-3 路②：清单差分补标（细则 §3）──

namespace {

// 假引擎（路②）：listAll 按调用次序出第一/第二份脚本清单（add 前快照 / 补标
// 差分）；updateMemoryText 记录。addInferred 恒成功。
class TagFakeEngine : public gateway::IMemoryRetriever {
public:
    std::vector<gateway::MemoryListItem> firstList;
    std::vector<gateway::MemoryListItem> secondList;
    int listCalls = 0;
    bool failUpdate = false;
    std::vector<std::pair<std::string, std::string>> updates;

    core::Result<std::vector<gateway::MemoryHit>> search(const std::string&,
                                                         int, int) override {
        return core::Result<std::vector<gateway::MemoryHit>>::ok({});
    }
    core::Result<std::string> addDirect(const std::string&) override {
        return core::Result<std::string>::ok(std::string());
    }
    core::Result<std::string> addInferred(const std::string&) override {
        return core::Result<std::string>::ok(std::string());
    }
    core::Result<void> setMemoryForgotten(const std::string&,
                                          bool) override {
        return core::Result<void>::ok();
    }
    core::Result<std::vector<gateway::MemoryListItem>> listAll(int,
                                                               int) override {
        ++listCalls;
        if (listCalls == 1) {
            return core::Result<std::vector<gateway::MemoryListItem>>::ok(
                firstList);
        }
        return core::Result<std::vector<gateway::MemoryListItem>>::ok(
            secondList);
    }
    core::Result<std::int64_t> getMemoryVersion(const std::string&) override {
        return core::Result<std::int64_t>::ok(0);
    }
    core::Result<void> updateMemoryText(const std::string& memoryId,
                                        const std::string& text) override {
        if (failUpdate) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "update 挂了");
        }
        updates.push_back({memoryId, text});
        return core::Result<void>::ok();
    }
};

struct TagFixture {
    std::filesystem::path dir;
    InferQueueStore queue;
    proposals::ProposalStore store;  // 腿 A：终稿卡通道
    proposals::UsageLedger ledger;
    TagFakeEngine engine;
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    explicit TagFixture(const std::string& tag)
        : dir(tmpDir(tag)),
          queue(InferQueueStore::open(dir / "infer_queue.db").value()),
          store(proposals::ProposalStore::open(dir / "proposals.db").value()),
          ledger(proposals::UsageLedger::open(dir / "usage_ledger.db").value()) {}

    core::Result<pipeline::InferStepReport> step() {
        InferLimits limits;
        limits.hardBudgetTokens = 3000000;
        limits.hourlyLimit = 10;
        return pipeline::runInferStep(queue, engine, store, ledger, limits,
                                      [](const pipeline::RoundNote&) {});
    }
};

}  // namespace

REGISTER_TEST(memory_infer_route2_tags_unprefixed_product,
              "memory: 需-3 路②——卡侧带前缀 → 差分新条目无前缀 → update_memory 补卡侧前缀；计账出队不受影响", {
    TagFixture f("tag");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "[踩坑|会话采集] 源段正文", f.now)
                      .isOk());
    f.engine.firstList = {};  // add 前快照：空库（首次提炼常态）
    f.engine.secondList = {
        gateway::MemoryListItem{"n1", "云端提炼产物无前缀", 0}};
    auto stepped = f.step();
    DOCTEST_REQUIRE(stepped.isOk());
    DOCTEST_CHECK(stepped.value().attempted);
    DOCTEST_CHECK(stepped.value().succeeded);
    DOCTEST_CHECK(stepped.value().productMissing == false);  // 产物已落库
    // 补标：卡侧前缀 tag + 空格 + 产物原文（细则 §3 update_memory 形态）
    DOCTEST_REQUIRE_EQ(f.engine.updates.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(f.engine.updates[0].first, std::string("n1"));
    DOCTEST_CHECK_EQ(f.engine.updates[0].second,
                     std::string("[踩坑|会话采集] 云端提炼产物无前缀"));
    // 主流程不受影响：计账非零 + 出队
    DOCTEST_CHECK(f.ledger.monthTokens(f.now).value() > 0);
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);
    return true;
})

REGISTER_TEST(memory_infer_route2_skip_states,
              "memory: 需-3 路②——产物已带合法前缀跳过；卡未定类型不补；差分排除已知 id；补标失败不阻断出队", {
    // ① 产物已带合法前缀 → 不 update
    {
        TagFixture f("skip-prefixed");
        DOCTEST_CHECK(f.queue.enqueue("s1", 1, "[踩坑|甲] 源段", f.now).isOk());
        f.engine.secondList = {
            gateway::MemoryListItem{"n1", "[决策|乙] 产物自带前缀", 0}};
        auto stepped = f.step();
        DOCTEST_REQUIRE(stepped.isOk());
        DOCTEST_CHECK(stepped.value().succeeded);
        DOCTEST_CHECK(f.engine.updates.empty());
    }
    // ② 卡未定类型（队列条目无合法前缀）→ 不补，产物留未分类
    {
        TagFixture f("skip-untyped");
        DOCTEST_CHECK(f.queue.enqueue("s1", 1, "无前缀的旧条目", f.now).isOk());
        f.engine.secondList = {
            gateway::MemoryListItem{"n1", "云端产物无前缀", 0}};
        auto stepped = f.step();
        DOCTEST_REQUIRE(stepped.isOk());
        DOCTEST_CHECK(stepped.value().succeeded);
        DOCTEST_CHECK(f.engine.updates.empty());
    }
    // ③ 差分：add 前已在库的 id 不补，只补新增
    {
        TagFixture f("diff");
        DOCTEST_CHECK(f.queue.enqueue("s1", 1, "[契约|接口] 源段", f.now)
                          .isOk());
        f.engine.firstList = {
            gateway::MemoryListItem{"old1", "既有条目无前缀", 0}};
        f.engine.secondList = {
            gateway::MemoryListItem{"old1", "既有条目无前缀", 0},
            gateway::MemoryListItem{"n1", "新增产物无前缀", 0}};
        auto stepped = f.step();
        DOCTEST_REQUIRE(stepped.isOk());
        DOCTEST_REQUIRE_EQ(f.engine.updates.size(), static_cast<std::size_t>(1));
        DOCTEST_CHECK_EQ(f.engine.updates[0].first, std::string("n1"));
        DOCTEST_CHECK_EQ(f.engine.updates[0].second,
                         std::string("[契约|接口] 新增产物无前缀"));
    }
    // ④ update 失败 → 尽力而为：主流程 succeeded + 出队，不留队不报错
    {
        TagFixture f("upd-fail");
        DOCTEST_CHECK(f.queue.enqueue("s1", 1, "[环境|本机] 源段", f.now)
                          .isOk());
        f.engine.secondList = {
            gateway::MemoryListItem{"n1", "产物无前缀", 0}};
        f.engine.failUpdate = true;
        auto stepped = f.step();
        DOCTEST_REQUIRE(stepped.isOk());
        DOCTEST_CHECK(stepped.value().succeeded);
        DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);
        DOCTEST_CHECK(f.ledger.monthTokens(f.now).value() > 0);
    }
    return true;
})

REGISTER_TEST(memory_infer_route2_zero_product_visible,
              "memory: 需-3 路②零产物上浮（bug239）——add ok 但清单差分零新增 → productMissing + 兜底卡 + note，照常出队不静默", {
    // bug239 真机形态复现钉：add 回包 ok（异常被引擎兜成 200）→ 记账出队，
    // 但清单差分零新增。旧实现三查全绿、产物静默缺失；腿 A 后转兜底人审
    //（OQ-2：零新增 = add 失败形态）。
    TagFixture f("zero-product");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "[踩坑|会话采集] 源段正文", f.now)
                      .isOk());
    f.engine.firstList = {};   // add 前快照：空库
    f.engine.secondList = {};  // add 后清单仍空：云端零产物
    pipeline::InferLimits limits;
    limits.hardBudgetTokens = 3000000;
    limits.hourlyLimit = 10;
    pipeline::RoundNote noted;
    int noteCount = 0;
    const auto stepped = pipeline::runInferStep(
        f.queue, f.engine, f.store, f.ledger, limits,
        [&noted, &noteCount](const pipeline::RoundNote& n) {
            noted = n;
            ++noteCount;
        });
    DOCTEST_REQUIRE(stepped.isOk());
    DOCTEST_CHECK(stepped.value().attempted);
    DOCTEST_CHECK(stepped.value().succeeded);       // add 成功：出队语义不变
    DOCTEST_CHECK(stepped.value().productMissing);  // 零产物显式上浮
    DOCTEST_REQUIRE_EQ(noteCount, 1);               // 动态流有告警 note
    DOCTEST_CHECK(noted.blocked);
    DOCTEST_CHECK(noted.text.find("零新增") != std::string::npos);
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);   // 照常出队（重试无意义）
    DOCTEST_CHECK(f.ledger.monthTokens(f.now).value() > 0);
    // 腿 A：零新增落兜底 pending 终稿卡（OQ-2 分流）
    auto pendingCards = f.store.listPending();
    DOCTEST_REQUIRE(pendingCards.isOk());
    DOCTEST_REQUIRE_EQ(pendingCards.value().size(),
                       static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pendingCards.value()[0].source,
                     std::string("final_draft"));
    DOCTEST_CHECK_EQ(pendingCards.value()[0].status, std::string("pending"));
    return true;
})

// ── F-03 波 2 腿 A（池 #333）：队列出队闭环 + OQ-2 终稿分流三形态 ──

REGISTER_TEST(memory_infer_queue_failure_accounting,
              "memory: 腿 A 失败记账——recordFailure 累计 attempts 并留最近原因；入队缺省置信 0.3", {
    StepFixture f("recfail");
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "甲", f.now).isOk());
    auto a1 = f.queue.recordFailure(1, "第一次失败原因");
    DOCTEST_REQUIRE(a1.isOk());
    DOCTEST_CHECK_EQ(a1.value(), 1);
    auto a2 = f.queue.recordFailure(1, "第二次失败原因");
    DOCTEST_REQUIRE(a2.isOk());
    DOCTEST_CHECK_EQ(a2.value(), 2);
    auto batch = f.queue.listBatch(1);
    DOCTEST_REQUIRE(batch.isOk());
    DOCTEST_REQUIRE_EQ(batch.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(batch.value()[0].attempts, 2);
    DOCTEST_CHECK_EQ(batch.value()[0].lastError,
                     std::string("第二次失败原因"));
    DOCTEST_CHECK_EQ(batch.value()[0].confidence, 0.3);  // collect 既有值缺省
    return true;
})

REGISTER_TEST(memory_infer_final_draft_auto_accepted,
              "memory: 腿 A OQ-2 成功形态——修饰成功 → 差分产物回填 targetIds → auto_accepted 终稿卡（撤销锚点）", {
    TagFixture f("auto");
    DOCTEST_CHECK(f.queue.enqueue("s1", 7, "[踩坑|会话采集] 源段正文", f.now)
                      .isOk());
    f.engine.firstList = {};  // add 前快照：空库
    f.engine.secondList = {
        gateway::MemoryListItem{"uuid-9", "[踩坑|会话采集] 云端终稿", 0}};
    auto stepped = f.step();
    DOCTEST_REQUIRE(stepped.isOk());
    DOCTEST_CHECK(stepped.value().succeeded);
    DOCTEST_CHECK(stepped.value().autoAccepted);
    DOCTEST_CHECK(stepped.value().fallbackPending == false);
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);
    // 终稿卡：auto_accepted 终态 + 产物 id 回填（撤销链锚点）+ 提炼正文
    auto decided = f.store.listRecentDecided(10);
    DOCTEST_REQUIRE(decided.isOk());
    DOCTEST_REQUIRE_EQ(decided.value().size(), static_cast<std::size_t>(1));
    const auto& card = decided.value()[0];
    DOCTEST_CHECK_EQ(card.status, std::string("auto_accepted"));
    DOCTEST_CHECK_EQ(card.source, std::string("final_draft"));
    DOCTEST_CHECK_EQ(card.kind, std::string("add"));
    DOCTEST_REQUIRE_EQ(card.targetIds.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(card.targetIds[0], std::string("uuid-9"));
    DOCTEST_CHECK_EQ(card.resultText,
                     std::string("[踩坑|会话采集] 云端终稿"));
    DOCTEST_CHECK(card.decidedAt > 0);
    DOCTEST_CHECK(card.appliedAt > 0);
    // 不进待审（已入库）；进入「已处理」可撤销视图
    auto pendingCards = f.store.listPending();
    DOCTEST_REQUIRE(pendingCards.isOk());
    DOCTEST_CHECK(pendingCards.value().empty());
    return true;
})

REGISTER_TEST(memory_infer_final_draft_low_confidence_fallback,
              "memory: 腿 A OQ-2 低置信形态——入队置信 <0.3 不出机（零引擎调用零记账），直接兜底 pending 卡", {
    TagFixture f("lowconf");
    DOCTEST_CHECK(f.queue.enqueue("s1", 7, "低置信源段", f.now, 0.2).isOk());
    auto stepped = f.step();
    DOCTEST_REQUIRE(stepped.isOk());
    DOCTEST_CHECK(stepped.value().attempted == false);  // 零云端出机
    DOCTEST_CHECK(stepped.value().fallbackPending);
    DOCTEST_CHECK(stepped.value().fallbackReason.find("置信") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(f.engine.listCalls, 0);  // 快照也不取（前置分流）
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);  // 出队落兜底卡
    auto pendingCards = f.store.listPending();
    DOCTEST_REQUIRE(pendingCards.isOk());
    DOCTEST_REQUIRE_EQ(pendingCards.value().size(),
                       static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pendingCards.value()[0].source,
                     std::string("final_draft"));
    DOCTEST_CHECK_EQ(pendingCards.value()[0].resultText,
                     std::string("低置信源段"));
    // 账面零消耗（不出机不记 attempt / token）
    DOCTEST_CHECK_EQ(f.ledger.hourAttempts(f.now).value(), 0);
    DOCTEST_CHECK_EQ(f.ledger.monthTokens(f.now).value(), 0);
    return true;
})

REGISTER_TEST(memory_infer_final_draft_retry_exhausted_fallback,
              "memory: 腿 A OQ-2 失败形态——重试耗尽出队落兜底卡（原因入 reason 与 note），未耗尽留队", {
    StepFixture f("exhaust");
    f.engine.fail = true;  // 云端持续 429
    DOCTEST_CHECK(f.queue.enqueue("s1", 7, "会耗尽的源段", f.now).isOk());
    pipeline::RoundNote noted;
    int noteCount = 0;
    InferLimits limits;
    limits.hardBudgetTokens = 3000000;
    limits.hourlyLimit = 0;  // 关节流：纯看条目级 attempts 上限
    for (int i = 0; i < 5; ++i) {
        auto s = pipeline::runInferStep(
            f.queue, f.engine, f.store, f.ledger, limits,
            [&noted, &noteCount](const pipeline::RoundNote& n) {
                noted = n;
                ++noteCount;
            });
        DOCTEST_REQUIRE(s.isOk());
        if (i < 4) {
            DOCTEST_CHECK(s.value().fallbackPending == false);  // 未耗尽留队
            DOCTEST_CHECK_EQ(f.queue.depth().value(), 1);
        }
    }
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);  // 耗尽出队（闭环）
    auto pendingCards = f.store.listPending();
    DOCTEST_REQUIRE(pendingCards.isOk());
    DOCTEST_REQUIRE_EQ(pendingCards.value().size(),
                       static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(pendingCards.value()[0].source,
                     std::string("final_draft"));
    // 耗尽 note 上浮（每步失败都有 note，最后一条 = 耗尽兜底）
    DOCTEST_CHECK(noteCount >= 5);
    DOCTEST_CHECK(noted.text.find("重试耗尽") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_infer_queue_drains_under_persistent_failure,
              "memory: 腿 A 只进不出回归红队——持续云端失败下队列必排空（3 条 × 5 次耗尽全部出队落兜底）", {
    StepFixture f("drain");
    f.engine.fail = true;  // 云端持续失败（旧实现：队首永久滞留，深度恒 3）
    DOCTEST_CHECK(f.queue.enqueue("s1", 1, "段甲", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s1", 2, "段乙", f.now).isOk());
    DOCTEST_CHECK(f.queue.enqueue("s2", 1, "段丙", f.now).isOk());
    for (int i = 0; i < 15 && f.queue.depth().value() > 0; ++i) {
        auto s = f.step(3000000, 0);
        DOCTEST_REQUIRE(s.isOk());
    }
    DOCTEST_CHECK_EQ(f.queue.depth().value(), 0);  // 出队闭环：队列排空
    auto pendingCards = f.store.listPending();
    DOCTEST_REQUIRE(pendingCards.isOk());
    DOCTEST_REQUIRE_EQ(pendingCards.value().size(),
                       static_cast<std::size_t>(3));  // 全部落兜底卡
    for (const auto& p : pendingCards.value()) {
        DOCTEST_CHECK_EQ(p.source, std::string("final_draft"));
        DOCTEST_CHECK_EQ(p.status, std::string("pending"));
    }
    // 失败也全额吃小时预算（attempts 口径不变）
    DOCTEST_CHECK_EQ(f.ledger.hourAttempts(f.now).value(), 15);
    return true;
})
