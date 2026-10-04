// src/module_agent/trigger_config.cpp
// trigger_config 编解码实现（契约见 trigger_config.h）
#include "module_agent/trigger_config.h"

#include <utility>

#include "platform/json.h"

namespace module::agent {
namespace {

namespace pj = platform::json;

constexpr const char* kCronKey = "cron";

} // namespace

core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg) {
    pj::JsonObject o;
    o.set(kCronKey, pj::makeString(cfg.cron));
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
    auto cron = v.value().asObject().get(kCronKey);
    if (!cron.isString() || cron.asString().empty()) {
        return core::Result<TriggerConfig>::fail(
            core::ErrorCode::ValidationError,
            "trigger_config 缺少非空的 cron 字段");
    }
    TriggerConfig cfg;
    cfg.cron = cron.asString();
    return core::Result<TriggerConfig>::ok(std::move(cfg));
}

} // namespace module::agent
