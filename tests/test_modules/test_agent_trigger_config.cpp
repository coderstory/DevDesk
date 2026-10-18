// tests/test_modules/test_agent_trigger_config.cpp
// trigger_config 编解码（事件扩展）：events / cooldown_sec 的往返与非法输入的可判
// 定性。schedule 型的往返与坏输入已有 test_agent_store.cpp 钉住，这里只补事件侧与
// 「至少含非空 cron 或非空 events 之一」的 decode 规则。全部纯函数用例，零 DB 零线程。
// 用例名前缀 event_trigger_cfg_（ASCII，--filter event_trigger_cfg 可定向跑）
#include "doctest.h"
#include "module_agent/trigger_config.h"
#include "platform/json.h"

#include <string>

namespace {
using module::agent::TriggerConfig;
using module::agent::decodeTriggerConfig;
using module::agent::encodeTriggerConfig;
} // namespace

// 事件配置往返：encode → parse（保证产物是合法 JSON）→ decode 逐字段一致
REGISTER_TEST(event_trigger_cfg_event_roundtrip,
              "trigger_config: event 型 encode/decode 往返（events 数组逐项一致）", {
    TriggerConfig cfg;
    cfg.events = {"todo.created", "md.saved"};
    auto encoded = encodeTriggerConfig(cfg);
    DOCTEST_REQUIRE(encoded.isOk());
    auto parsed = platform::json::parse(encoded.value());
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK(parsed.value().isObject());
    auto decoded = decodeTriggerConfig(encoded.value());
    DOCTEST_REQUIRE(decoded.isOk());
    DOCTEST_CHECK_EQ(decoded.value().cron, std::string(""));
    DOCTEST_REQUIRE_EQ(decoded.value().events.size(), 2u);
    DOCTEST_CHECK_EQ(decoded.value().events[0], std::string("todo.created"));
    DOCTEST_CHECK_EQ(decoded.value().events[1], std::string("md.saved"));
    DOCTEST_CHECK_EQ(decoded.value().cooldownSec, 0);
    return true;
})

// cooldown_sec 编进 events 配置；decode 读回；未编码时保持 0（= 未设置）
REGISTER_TEST(event_trigger_cfg_cooldown_roundtrip,
              "trigger_config: events + cooldown_sec 往返；缺省 cooldown 为 0", {
    TriggerConfig cfg;
    cfg.events = {"todo.completed"};
    cfg.cooldownSec = 60;
    auto encoded = encodeTriggerConfig(cfg);
    DOCTEST_REQUIRE(encoded.isOk());
    auto decoded = decodeTriggerConfig(encoded.value());
    DOCTEST_REQUIRE(decoded.isOk());
    DOCTEST_CHECK_EQ(decoded.value().cooldownSec, 60);
    DOCTEST_CHECK_EQ(decoded.value().events.size(), 1u);

    TriggerConfig noCd;
    noCd.events = {"todo.completed"};
    auto plain = encodeTriggerConfig(noCd);
    DOCTEST_REQUIRE(plain.isOk());
    auto plainDec = decodeTriggerConfig(plain.value());
    DOCTEST_REQUIRE(plainDec.isOk());
    DOCTEST_CHECK_EQ(plainDec.value().cooldownSec, 0);
    return true;
})

// decode 规则「至少含非空 cron 或非空 events 之一」：两者皆缺 → fail。
// 坏形：空对象 / events 非数组 / events 空数组 / 元素非字符串 / cooldown 非数字；
// 合法形：events-only（无 cron）/ cron+events 共存 / 多余键忽略
REGISTER_TEST(event_trigger_cfg_decode_rules,
              "trigger_config: events-only 合法；两者皆缺/坏形 → fail（中文文案）", {
    // events-only：无 cron 也合法（schedule 型零回归的反面：event 型不要求 cron）
    auto evOnly = decodeTriggerConfig(R"({"events":["md.saved"]})");
    DOCTEST_REQUIRE(evOnly.isOk());
    DOCTEST_CHECK_EQ(evOnly.value().events[0], std::string("md.saved"));
    DOCTEST_CHECK(evOnly.value().cron.empty());

    // 共存 + cooldown
    auto both = decodeTriggerConfig(
        R"({"cron":"* * * * *","events":["todo.created"],"cooldown_sec":120})");
    DOCTEST_REQUIRE(both.isOk());
    DOCTEST_CHECK_EQ(both.value().cron, std::string("* * * * *"));
    DOCTEST_REQUIRE_EQ(both.value().events.size(), 1u);
    DOCTEST_CHECK_EQ(both.value().cooldownSec, 120);

    // 两者皆缺 / 坏形 → fail
    DOCTEST_CHECK(decodeTriggerConfig("{}").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"events":[]})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"events":"todo.created"})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"events":[1,2]})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"events":[""]})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"events":["a"],"cooldown_sec":"x"})").isFail());
    // 错误信息非空（调度器把它拼进点名 task id 的 WARN）
    auto bad = decodeTriggerConfig(R"({"events":[]})");
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK(!bad.errorMessage().empty());
    return true;
})

// 限次语义（max_runs）往返 + 「缺省 = 不限次数」的向后兼容对照：旧配置串（没有该键）
// 解码后 maxRuns = nullopt，语义与今天逐字节一致；encode 侧不限次数不写该键——这是
// 「重复任务不回归」的结构保证（旧行没键 → 永远解不出上限 → Scheduler 恒派发）
REGISTER_TEST(event_trigger_cfg_max_runs_roundtrip,
              "trigger_config: max_runs 往返；未设置时不写该键（旧串逐字节不变）", {
    TriggerConfig once;
    once.cron = "0 18 * * *";
    once.maxRuns = 1;
    auto onceText = encodeTriggerConfig(once);
    DOCTEST_REQUIRE(onceText.isOk());
    auto onceDec = decodeTriggerConfig(onceText.value());
    DOCTEST_REQUIRE(onceDec.isOk());
    DOCTEST_REQUIRE(onceDec.value().maxRuns.has_value());
    DOCTEST_CHECK_EQ(*onceDec.value().maxRuns, 1);
    DOCTEST_CHECK_EQ(onceDec.value().cron, std::string("0 18 * * *"));

    // 旧配置串（无 max_runs）：decode 放行且 maxRuns 缺席 = 不限次数（今天的行为）
    auto legacy = decodeTriggerConfig(R"({"cron":"0 18 * * *"})");
    DOCTEST_REQUIRE(legacy.isOk());
    DOCTEST_CHECK_FALSE(legacy.value().maxRuns.has_value());
    // 不限次数的 encode 产物 = 没有新键的今日字节（旧串写回后仍是旧串）
    TriggerConfig unlimited;
    unlimited.cron = "0 18 * * *";
    auto unlimitedText = encodeTriggerConfig(unlimited);
    DOCTEST_REQUIRE(unlimitedText.isOk());
    DOCTEST_CHECK_EQ(unlimitedText.value(), std::string(R"({"cron":"0 18 * * *"})"));
    // 事件型同样可带上限（「下次保存笔记时跑一次」）
    TriggerConfig ev;
    ev.events = {"md.saved"};
    ev.maxRuns = 2;
    auto evText = encodeTriggerConfig(ev);
    DOCTEST_REQUIRE(evText.isOk());
    auto evDec = decodeTriggerConfig(evText.value());
    DOCTEST_REQUIRE(evDec.isOk());
    DOCTEST_REQUIRE(evDec.value().maxRuns.has_value());
    DOCTEST_CHECK_EQ(*evDec.value().maxRuns, 2);
    DOCTEST_CHECK_EQ(evDec.value().events.size(), 1u);
    return true;
})

// 坏 max_runs 一律 fail、不许静默当「不限次数」：静默会让「只跑一次」的配置悄悄变回
// 永久重复任务（正是本单要消灭的失败模式）。判据与 cooldown_sec 同形（类型）+ 正整数
// （0 / 负数 / 超上限都不是合法上限）
REGISTER_TEST(event_trigger_cfg_max_runs_rejects_bad_values,
              "trigger_config: max_runs 非正整数/超上限 → decode fail（不静默当不限次数）", {
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":0})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":-1})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":"1"})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":true})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":null})").isFail());
    // 上限寄存器（kMaxRunsUpperBound）：防手改库塞超大值让每次扫描拉爆 listRuns
    DOCTEST_CHECK(
        decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":1001})").isFail());
    DOCTEST_CHECK(
        decodeTriggerConfig(R"({"events":["md.saved"],"max_runs":1001})").isFail());
    // 边界绿：1 与上限本身都合法
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":1})").isOk());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":1000})").isOk());
    // 错误信息非空（Scheduler / UI 把它拼进点名 task id 的 WARN 与配置异常徽章）
    auto bad = decodeTriggerConfig(R"({"cron":"* * * * *","max_runs":0})");
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK(!bad.errorMessage().empty());
    DOCTEST_CHECK(bad.errorMessage().find("max_runs") != std::string::npos);
    return true;
})

// encode 侧防线：cron 与 events 两者皆空 → fail（防「encode 出 {} 再 decode fail」
// 的死配置静默落库）
REGISTER_TEST(event_trigger_cfg_encode_rejects_empty,
              "trigger_config: encode 空配置 fail（cron 与 events 皆缺）", {
    DOCTEST_CHECK(encodeTriggerConfig(TriggerConfig{}).isFail());
    TriggerConfig emptyCron;   // 空 cron = 未配置
    DOCTEST_CHECK(encodeTriggerConfig(emptyCron).isFail());
    return true;
})

// 任务级「配置异常」判定的红侧：三类「配置根本解不出来 / 永远不触发」必须产出非空
// 描述（它们的共同点是 Scheduler 每跑必跳过，而任务在列表里长得和正常任务一样）。
// 判定须与 Scheduler 同源：这里的每一条都对应 tickNow / processEvent 里的一处跳过。
REGISTER_TEST(event_trigger_cfg_problem_broken_configs,
              "trigger_config: 配置异常判定——事件型解析失败 / cron 型解析失败 / cron 表达式非法", {
    using module::agent::triggerConfigProblem;
    // ① 事件型：trigger_config 是坏 JSON（Scheduler::processEvent 的 decode 失败路径）
    const std::string evBad = triggerConfigProblem("event", "{不是 JSON");
    DOCTEST_CHECK_MESSAGE(!evBad.empty(), "事件型坏 JSON 未产出问题描述");
    DOCTEST_CHECK(evBad.find("解析") != std::string::npos);
    // decode 能过但内容非法（events 空数组）→ processEvent 逐项比对恒不命中 → 永不触发
    DOCTEST_CHECK(!triggerConfigProblem("event", R"({"events":[]})").empty());
    // ② cron 型：decode 直接失败（与 tickNow 第一道 decode 失败同一处）
    const std::string schBad = triggerConfigProblem("schedule", "not json at all");
    DOCTEST_CHECK_MESSAGE(!schBad.empty(), "cron 型坏 JSON 未产出问题描述");
    // ③ cron 型：decode 能过、表达式非法（tickNow 的第二道 parseCron 失败）
    const std::string cronBad =
        triggerConfigProblem("schedule", R"({"cron":"0 9 * * 8"})");
    DOCTEST_CHECK_MESSAGE(!cronBad.empty(), "非法 cron 表达式未产出问题描述");
    DOCTEST_CHECK(cronBad.find("cron") != std::string::npos);
    // ④ schedule 型但配置里只有 events：decode 过、cron 为空 → parseCron("") 失败，
    //    与 tickNow 同判（同一份配置在调度器那里每跑必跳过）
    DOCTEST_CHECK(
        !triggerConfigProblem("schedule", R"({"events":["todo.created"]})").empty());
    return true;
})

// 任务级「配置异常」判定的绿侧（对照组，防「凡有配置都报异常」的恒真断言）：
// 合法配置与不消费 trigger_config 的触发类型一律空描述 = 正常路径零噪音。
REGISTER_TEST(event_trigger_cfg_problem_clean_controls,
              "trigger_config: 合法 event / 合法 cron / manual·resident·daily → 无问题描述", {
    using module::agent::triggerConfigProblem;
    DOCTEST_CHECK(triggerConfigProblem("event", R"({"events":["todo.created"]})").empty());
    DOCTEST_CHECK(
        triggerConfigProblem("event", R"({"events":["todo.created"],"cooldown_sec":60})")
            .empty());
    DOCTEST_CHECK(triggerConfigProblem("schedule", R"({"cron":"0 9 * * 1-5"})").empty());
    DOCTEST_CHECK(triggerConfigProblem("schedule", R"({"cron":"* * * * *"})").empty());
    // manual / resident 不消费 trigger_config（Scheduler 的 enable+triggerType 过滤
    // 都不含它们）→ 库里的残留串/空串都不许挂徽章
    DOCTEST_CHECK(triggerConfigProblem("manual", "").empty());
    DOCTEST_CHECK(triggerConfigProblem("resident", "{坏 JSON").empty());
    // daily（每天，不指定时刻）同样不消费 trigger_config，残留串不许挂徽章
    DOCTEST_CHECK(triggerConfigProblem("daily", "").empty());
    DOCTEST_CHECK(triggerConfigProblem("daily", "{坏 JSON").empty());
    // 同源反向题：event 型只比 events（processEvent 不碰 cron），配置里塞了非法 cron
    // 也不该算问题——否则 UI 说异常、Scheduler 照常触发，正是本单要消灭的分裂反面
    DOCTEST_CHECK(
        triggerConfigProblem("event", R"({"events":["md.saved"],"cron":"0 9 * * 99"})")
            .empty());
    return true;
})

// once_at 往返：Unix 秒原值进出，产物只含 once_at（不写 cron/events/max_runs/cooldown），
// 上下界本身也必须是合法值（边界寄存器与 decode 同口径）
REGISTER_TEST(event_trigger_cfg_once_at_roundtrip,
              "trigger_config: once_at encode/decode 往返（值不变、上下界合法）", {
    TriggerConfig cfg;
    cfg.onceAt = 1767225600LL;   // 2026-01-01 00:00:00 UTC
    auto encoded = encodeTriggerConfig(cfg);
    DOCTEST_REQUIRE(encoded.isOk());
    auto parsed = platform::json::parse(encoded.value());
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    const platform::json::JsonObject obj = parsed.value().asObject();
    DOCTEST_CHECK(obj.has("once_at"));
    DOCTEST_CHECK_FALSE(obj.has("cron"));
    DOCTEST_CHECK_FALSE(obj.has("events"));
    DOCTEST_CHECK_FALSE(obj.has("max_runs"));
    DOCTEST_CHECK_FALSE(obj.has("cooldown_sec"));

    auto decoded = decodeTriggerConfig(encoded.value());
    DOCTEST_REQUIRE(decoded.isOk());
    DOCTEST_REQUIRE(decoded.value().onceAt.has_value());
    DOCTEST_CHECK_EQ(*decoded.value().onceAt, 1767225600LL);
    DOCTEST_CHECK(decoded.value().cron.empty());
    DOCTEST_CHECK(decoded.value().events.empty());
    DOCTEST_CHECK_FALSE(decoded.value().maxRuns.has_value());

    // 下界 1 与上界 kOnceAtUpperBound 都是合法值
    TriggerConfig lo;
    lo.onceAt = 1;
    auto loText = encodeTriggerConfig(lo);
    DOCTEST_REQUIRE(loText.isOk());
    auto loDec = decodeTriggerConfig(loText.value());
    DOCTEST_REQUIRE(loDec.isOk());
    DOCTEST_REQUIRE(loDec.value().onceAt.has_value());
    DOCTEST_CHECK_EQ(*loDec.value().onceAt, 1LL);

    TriggerConfig hi;
    hi.onceAt = module::agent::kOnceAtUpperBound;
    auto hiText = encodeTriggerConfig(hi);
    DOCTEST_REQUIRE(hiText.isOk());
    auto hiDec = decodeTriggerConfig(hiText.value());
    DOCTEST_REQUIRE(hiDec.isOk());
    DOCTEST_REQUIRE(hiDec.value().onceAt.has_value());
    DOCTEST_CHECK_EQ(*hiDec.value().onceAt, module::agent::kOnceAtUpperBound);
    return true;
})

// once_at 与 cron / events 互斥（encode 侧）：同时配置会让「一次性」与「重复」两个
// 语义同时成立，直接 fail 而不是产出取决于消费方读取顺序的配置
REGISTER_TEST(event_trigger_cfg_once_at_encode_rejects_mixed,
              "trigger_config: encode 拒绝 once_at 与 cron/events 同时出现", {
    TriggerConfig cronOnce;
    cronOnce.cron = "0 9 * * *";
    cronOnce.onceAt = 1767225600LL;
    auto cronOnceText = encodeTriggerConfig(cronOnce);
    DOCTEST_CHECK(cronOnceText.isFail());
    DOCTEST_CHECK(cronOnceText.errorMessage().find("once_at") != std::string::npos);

    TriggerConfig eventsOnce;
    eventsOnce.events = {"md.saved"};
    eventsOnce.onceAt = 1767225600LL;
    auto eventsOnceText = encodeTriggerConfig(eventsOnce);
    DOCTEST_CHECK(eventsOnceText.isFail());
    DOCTEST_CHECK(eventsOnceText.errorMessage().find("once_at") != std::string::npos);
    return true;
})

// decode 侧互斥：两种重复载荷分别与 once_at 共存都必须 fail（错误信息点名 once_at）
REGISTER_TEST(event_trigger_cfg_once_at_decode_rejects_mixed,
              "trigger_config: decode 拒绝 once_at + cron / once_at + events", {
    auto cronOnce = decodeTriggerConfig(
        R"({"cron":"0 9 * * *","once_at":1767225600})");
    DOCTEST_CHECK(cronOnce.isFail());
    DOCTEST_CHECK(cronOnce.errorMessage().find("once_at") != std::string::npos);

    auto eventsOnce = decodeTriggerConfig(
        R"({"events":["md.saved"],"once_at":1767225600})");
    DOCTEST_CHECK(eventsOnce.isFail());
    DOCTEST_CHECK(eventsOnce.errorMessage().find("once_at") != std::string::npos);
    return true;
})

// once_at 坏值：0 / 负数 / 小数 / 字符串 / 布尔 / null / 超上限 / 毫秒单位。
// 严格整数字面量——带小数点的 `123.0` 也拒绝（platform::json 走 Double 分支，
// 不能把 123.0 静默截成 123，否则「小数」与「单位写错」都失去可判定性）
REGISTER_TEST(event_trigger_cfg_once_at_rejects_bad_values,
              "trigger_config: once_at 非正整数/小数/非数字/超上限 → decode fail", {
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":0})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":-1})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":1.5})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":123.0})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":"1767225600"})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":true})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":null})").isFail());
    // 超上限 1 秒 + 典型「毫秒当秒」值（2026 年约 1.7e12）
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":4102444801})").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"({"once_at":1767225600000})").isFail());
    // 错误信息非空且点名 once_at（Scheduler / UI 拼进 WARN 与配置异常徽章）
    auto bad = decodeTriggerConfig(R"({"once_at":0})");
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK(!bad.errorMessage().empty());
    DOCTEST_CHECK(bad.errorMessage().find("once_at") != std::string::npos);
    return true;
})

// 任务级「配置异常」判定的 once 侧：合法 once 零噪音；带 cron/events、缺 once_at、
// 非法 once_at 都必须有具体描述。manual / resident / daily 仍不消费该列。
REGISTER_TEST(event_trigger_cfg_once_problem,
              "trigger_config: once 型配置异常判定——合法空、带 cron/events/缺 once_at 非空", {
    using module::agent::triggerConfigProblem;
    // 绿：合法 once（含可选的 max_runs）零噪音
    DOCTEST_CHECK(triggerConfigProblem("once", R"({"once_at":1767225600})").empty());
    DOCTEST_CHECK(
        triggerConfigProblem("once", R"({"once_at":1767225600,"max_runs":1})").empty());
    // 红：cron-only / events-only 给 once → 明确「不支持 cron/events」
    const std::string cronCase = triggerConfigProblem("once", R"({"cron":"0 9 * * *"})");
    DOCTEST_CHECK_MESSAGE(!cronCase.empty(), "once 型带 cron 未产出问题描述");
    DOCTEST_CHECK(cronCase.find("cron") != std::string::npos);
    const std::string eventsCase = triggerConfigProblem("once", R"({"events":["md.saved"]})");
    DOCTEST_CHECK_MESSAGE(!eventsCase.empty(), "once 型带 events 未产出问题描述");
    DOCTEST_CHECK(eventsCase.find("events") != std::string::npos);
    // 红：配置里没有可消费载荷 → decode 直接失败（缺 once_at 的最直接形态）
    const std::string missing = triggerConfigProblem("once", R"({"max_runs":1})");
    DOCTEST_CHECK_MESSAGE(!missing.empty(), "once 型缺 once_at 未产出问题描述");
    DOCTEST_CHECK(missing.find("解析") != std::string::npos);
    // 红：once_at + cron 同时存在 → decode 的互斥错误
    const std::string mixed =
        triggerConfigProblem("once", R"({"cron":"0 9 * * *","once_at":1767225600})");
    DOCTEST_CHECK_MESSAGE(!mixed.empty(), "once 型 once_at+cron 未产出问题描述");
    DOCTEST_CHECK(mixed.find("once_at") != std::string::npos);
    // 红：非法 once_at
    const std::string badOnce = triggerConfigProblem("once", R"({"once_at":0})");
    DOCTEST_CHECK_MESSAGE(!badOnce.empty(), "once 型非法 once_at 未产出问题描述");
    DOCTEST_CHECK(badOnce.find("once_at") != std::string::npos);
    // 不消费 trigger_config 的类型：残留 once 载荷也不许挂徽章
    DOCTEST_CHECK(triggerConfigProblem("manual", R"({"once_at":1767225600})").empty());
    DOCTEST_CHECK(triggerConfigProblem("resident", R"({"once_at":0})").empty());
    DOCTEST_CHECK(triggerConfigProblem("daily", R"({"once_at":1767225600})").empty());
    return true;
})
