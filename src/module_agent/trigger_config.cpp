// trigger_config 编解码实现（契约见 trigger_config.h）
#include "module_agent/trigger_config.h"

#include <utility>

#include "module_agent/cron.h"
#include "platform/json.h"

namespace module::agent {
namespace {

namespace pj = platform::json;

constexpr const char* kCronKey = "cron";
constexpr const char* kEventsKey = "events";
constexpr const char* kCooldownKey = "cooldown_sec";
constexpr const char* kMaxRunsKey = "max_runs";

} // namespace

core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg) {
    if (cfg.cron.empty() && cfg.events.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::ValidationError,
            "trigger_config 须至少含非空 cron 或非空 events 之一");
    }
    pj::JsonObject o;
    if (!cfg.cron.empty()) o.set(kCronKey, pj::makeString(cfg.cron));
    if (!cfg.events.empty()) o.set(kEventsKey, pj::makeStringArray(cfg.events));
    if (cfg.cooldownSec > 0) o.set(kCooldownKey, pj::makeInt(cfg.cooldownSec));
    // 不限次数（缺省 / nullopt / 非正值）不写该键：产物与加键之前逐字节相同，
    // 旧配置与旧用例（钉住 `{"cron":"…"}` 原文）都不会因本键变化
    if (cfg.maxRuns && *cfg.maxRuns > 0) o.set(kMaxRunsKey, pj::makeInt(*cfg.maxRuns));
    auto text = pj::stringify(pj::JsonValue(std::move(o)));
    if (text.isFail()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "trigger_config 序列化失败: " + text.errorMessage());
    }
    return core::Result<std::string>::ok(text.value());
}

// 任务级「配置异常」判定（契约见 trigger_config.h）：判据必须与 Scheduler 的跳过分支
// 一一对应——多一条会误报正常任务，少一条会漏掉「UI 说没事、Scheduler 每跑必跳过」
std::string triggerConfigProblem(const std::string& triggerType,
                                 const std::string& triggerConfig) {
    // 第一道过滤与 Scheduler 同口径：只有 event / schedule 消费 trigger_config
    //（processEvent / tickNow 都是「enabled && triggerType == …」），manual / resident
    // 的该列是历史残留，不许挂标记
    if (triggerType != "event" && triggerType != "schedule") return {};
    auto cfg = decodeTriggerConfig(triggerConfig);
    if (cfg.isFail()) return "触发配置无法解析：" + cfg.errorMessage();
    if (triggerType == "schedule") {
        // decode 能过 ≠ 能触发：tickNow 在 decode 之后还有第二道 parseCron
        auto expr = parseCron(cfg.value().cron);
        if (expr.isFail()) return "cron 表达式非法：" + expr.errorMessage();
        return {};
    }
    if (cfg.value().events.empty()) {
        // 事件型但一个事件都没配（decode 放行 cron-only 配置，这是它的镜像）：processEvent
        // 逐项比对恒不命中 → 永不触发
        return "事件触发未配置任何事件（永不触发）";
    }
    return {};
}

core::Result<TriggerConfig> decodeTriggerConfig(const std::string& json) {
    if (json.empty()) {
        return core::Result<TriggerConfig>::fail(
            core::ErrorCode::ValidationError, "trigger_config 为空");
    }
    auto v = pj::parse(json);
    if (v.isFail() || !v.value().isObject()) {
        return core::Result<TriggerConfig>::fail(
            core::ErrorCode::ValidationError,
            v.isFail() ? "trigger_config 不是合法 JSON：" + v.errorMessage()
                       : "trigger_config 不是 JSON 对象");
    }
    const pj::JsonObject obj = v.value().asObject();
    TriggerConfig cfg;
    bool any = false;
    if (obj.has(kCronKey)) {
        auto cron = obj.get(kCronKey);
        if (!cron.isString() || cron.asString().empty()) {
            return core::Result<TriggerConfig>::fail(
                core::ErrorCode::ValidationError,
                "trigger_config 的 cron 字段不是非空字符串");
        }
        cfg.cron = cron.asString();
        any = true;
    }
    if (obj.has(kEventsKey)) {
        auto events = obj.get(kEventsKey);
        if (!events.isArray() || events.size() == 0) {
            return core::Result<TriggerConfig>::fail(
                core::ErrorCode::ValidationError,
                "trigger_config 的 events 字段须为非空数组");
        }
        for (int i = 0; i < events.size(); ++i) {
            auto ev = events[i];
            if (!ev.isString() || ev.asString().empty()) {
                return core::Result<TriggerConfig>::fail(
                    core::ErrorCode::ValidationError,
                    "trigger_config 的 events 数组含非字符串或空串元素");
            }
            cfg.events.push_back(ev.asString());
        }
        any = true;
    }
    if (obj.has(kCooldownKey)) {
        auto cd = obj.get(kCooldownKey);
        if (!cd.isNumber()) {
            return core::Result<TriggerConfig>::fail(
                core::ErrorCode::ValidationError,
                "trigger_config 的 cooldown_sec 字段不是数字");
        }
        cfg.cooldownSec = static_cast<int>(cd.asInt());
    }
    if (obj.has(kMaxRunsKey)) {
        auto mr = obj.get(kMaxRunsKey);
        // 严格：坏值一律 fail 而不是当「不限次数」——静默降级会让「只跑一次」的配置
        // 悄悄变回永久重复任务（本键存在的唯一目的就是别反复跑）。上限寄存器防手改库
        // 塞超大值：Scheduler 按上限做 listRuns(LIMIT maxRuns)，无上限则每次扫描拉爆内存
        if (!mr.isNumber() || mr.asInt() < 1 || mr.asInt() > kMaxRunsUpperBound) {
            return core::Result<TriggerConfig>::fail(
                core::ErrorCode::ValidationError,
                "trigger_config 的 max_runs 字段须为 1.." +
                    std::to_string(kMaxRunsUpperBound) + " 的整数");
        }
        cfg.maxRuns = static_cast<int>(mr.asInt());
    }
    if (!any) {
        return core::Result<TriggerConfig>::fail(
            core::ErrorCode::ValidationError,
            "trigger_config 须至少含非空 cron 或非空 events 之一");
    }
    return core::Result<TriggerConfig>::ok(std::move(cfg));
}

} // namespace module::agent
