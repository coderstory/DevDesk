// trigger_config 编解码实现（契约见 trigger_config.h）
#include "module_agent/trigger_config.h"

#include <utility>

#include "platform/json.h"

namespace module::agent {
namespace {

namespace pj = platform::json;

constexpr const char* kCronKey = "cron";
constexpr const char* kEventsKey = "events";
constexpr const char* kCooldownKey = "cooldown_sec";

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
    auto text = pj::stringify(pj::JsonValue(std::move(o)));
    if (text.isFail()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "trigger_config 序列化失败: " + text.errorMessage());
    }
    return core::Result<std::string>::ok(text.value());
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
    if (!any) {
        return core::Result<TriggerConfig>::fail(
            core::ErrorCode::ValidationError,
            "trigger_config 须至少含非空 cron 或非空 events 之一");
    }
    return core::Result<TriggerConfig>::ok(std::move(cfg));
}

} // namespace module::agent
