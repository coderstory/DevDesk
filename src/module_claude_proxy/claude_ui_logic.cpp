// src/module_claude_proxy/claude_ui_logic.cpp
// 4 个纯函数实现（可无头单测）。
#include "claude_ui_logic.h"

#include "core/time.h"
#include "platform/json.h"
#include "provider_config.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace module::claude {

std::string modelsSummary(const Profile& p) {
    const ModelsConfig mc = effectiveModelsConfig(p.modelsConfigJson);
    int configured = 0;
    for (const auto& meta : modelSlotTable()) {
        if (!(mc.*meta.slot).model.empty()) ++configured;
    }
    const std::string total = std::to_string(kModelSlotCount);
    if (mc.def.model.empty()) {
        return configured > 0
            ? ("（默认未配）· 槽位已配 " + std::to_string(configured) + "/" + total)
            : std::string("-");
    }
    // 显示层剥 [1M]（1M 状态由复选框表达）；摘要走 deriveDisplayName，存储值原样
    return "默认 " + deriveDisplayName(mc.def.model) + " · 槽位已配 " +
           std::to_string(configured) + "/" + total;
}

std::string suggestImportName(const std::string& baseUrl) {
    size_t start = baseUrl.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = baseUrl.find('/', start);
    if (end == std::string::npos) end = baseUrl.size();
    std::string host = baseUrl.substr(start, end - start);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) host.resize(colon);
    if (!host.empty()) return "导入 " + host;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    core::time::localTime(t, &tm);
    // 缓冲区按最坏情况定尺（3×11 + 2 + 1 = 36），写小了 GCC -Wformat-truncation 判红
    char buf[36];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return "导入 " + std::string(buf);
}

std::string buildProfileExportJson(const Profile& p,
                                   const std::string& keyPlaceholder) {
    // env 域：与 runProfile 的临时 settings 同一条 env 管线，仅凭据换成占位值、
    // 不落盘只进剪贴板。
    const auto env = buildDirectEnv(p.baseUrl, keyPlaceholder, p.authField,
                                    p.modelsConfigJson, p.extraEnvJson);
    platform::json::JsonObject envObj;
    for (const auto& [k, v] : env) {
        envObj.set(k, platform::json::makeString(v));
    }
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(std::move(envObj)));
    auto text = platform::json::stringify(
        platform::json::JsonValue(std::move(root)), /*pretty=*/true);
    if (text.isFail()) {
        // 空串 = 失败信号（序列化失败理论不可达），调用方按失败走 toast
        return std::string();
    }
    return text.value();
}

} // namespace module::claude
