// src/module_claude_proxy/claude_ui_logic.cpp
// R4（P-B1）：4 个纯函数实现 —— 逐字自 claude_module.cpp 匿名命名空间搬运
//（含注释），只改归属；行为零变化由 tests/test_claude_ui.cpp 的等价断言护栏。
#include "claude_ui_logic.h"

#include "core/time.h"
#include "platform/json.h"
#include "provider_config.h"

#include <cstdio>
#include <ctime>

namespace module::claude {

std::string formatLatency(int64_t ms) {
    if (ms <= 0) return "-";
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return std::string(buf);
}

std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

std::string modelsSummary(const Profile& p) {
    const ModelsConfig mc = effectiveModelsConfig(p.modelsConfigJson, p.model);
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
    return "默认 " + mc.def.model + " · 槽位已配 " +
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
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return "导入 " + std::string(buf);
}

std::vector<std::string> sharedFrozenKeys(const SharedConfig& sc) {
    std::vector<std::string> keys;
    if (sc.extraSettingsJson.empty()) return keys;
    auto parsed = parseExtraEnv(sc.extraSettingsJson);
    if (parsed.isOk()) {
        const auto mapped = applyEnvAliases(parsed.value()).env;
        keys.reserve(mapped.size());
        for (const auto& [k, v] : mapped) keys.push_back(k);
    }
    // JSON 非法 → 空集（读路径容错：宁可少挡几个键，不让整次切换失败，
    // 与服务层 sharedEnvKeysFor 同语义）
    return keys;
}

std::string normalizedEnvKey(const std::string& k) {
    if (k.empty()) return k;
    const auto mapped =
        applyEnvAliases(std::map<std::string, std::string>{{k, ""}}).env;
    for (const auto& [nk, v] : mapped) return nk;   // 单键映射：首键即归一结果
    return k;   // 理论上到不了（applyEnvAliases 不透传删除键），兜底原名
}

const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id) {
    for (const auto& sc : configs) {
        if (sc.id == id) return &sc;
    }
    return nullptr;
}

bool keyIn(const std::vector<std::string>& keys, const std::string& k) {
    for (const auto& x : keys) {
        if (x == k) return true;
    }
    return false;
}

std::string buildProfileExportJson(const Profile& p,
                                   const std::string& sharedExtraEnvJson,
                                   const std::string& keyPlaceholder) {
    // 与 runProfile 的临时 settings 同一条 env 管线（claude_service.cpp
    // buildDirectEnv 消费点同款手法），仅凭据换成占位值、不落盘只进剪贴板
    const auto env = buildDirectEnv(p.baseUrl, keyPlaceholder, p.authField,
                                    p.modelsConfigJson, p.model,
                                    p.extraEnvJson, sharedExtraEnvJson);
    platform::json::JsonObject envObj;
    for (const auto& [k, v] : env) {
        envObj.set(k, platform::json::makeString(v));
    }
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(std::move(envObj)));
    auto text =
        platform::json::stringify(platform::json::JsonValue(std::move(root)));
    if (text.isFail()) {
        // 全字符串值的两段序列化，失败理论不可达；空串 = 失败信号，
        // 调用方按失败走 toast，不静默成功
        return std::string();
    }
    return text.value();
}

} // namespace module::claude
