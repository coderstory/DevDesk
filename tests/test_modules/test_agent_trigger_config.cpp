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

// encode 侧防线：cron 与 events 两者皆空 → fail（防「encode 出 {} 再 decode fail」
// 的死配置静默落库）
REGISTER_TEST(event_trigger_cfg_encode_rejects_empty,
              "trigger_config: encode 空配置 fail（cron 与 events 皆缺）", {
    DOCTEST_CHECK(encodeTriggerConfig(TriggerConfig{}).isFail());
    TriggerConfig emptyCron;   // 空 cron = 未配置
    DOCTEST_CHECK(encodeTriggerConfig(emptyCron).isFail());
    return true;
})
