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

const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id) {
    for (const auto& sc : configs) {
        if (sc.id == id) return &sc;
    }
    return nullptr;
}

std::string buildProfileExportJson(const Profile& p,
                                   const std::string& fragmentJson,
                                   const std::string& keyPlaceholder) {
    // env 域：与 runProfile 的临时 settings 同一条 env 管线（claude_service.cpp
    // buildDirectEnv 消费点同款手法），仅凭据换成占位值、不落盘只进剪贴板。
    // 片段 env 叠加不再走 env 通道（advanced-json-editor §6：片段已重构为高级
    // 属性 JSON 预设；env 域片段管线随 §8.4 拆除）——恒传空串，坏片段 JSON 的
    // 防静默 WARN 由调用方 doCopyConfig 覆盖两域
    const auto env = buildDirectEnv(p.baseUrl, keyPlaceholder, p.authField,
                                    p.modelsConfigJson, p.extraEnvJson);
    platform::json::JsonObject envObj;
    for (const auto& [k, v] : env) {
        envObj.set(k, platform::json::makeString(v));
    }
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(std::move(envObj)));
    // 非 env 域：生效 JSON（片段叠加 + 片段赢）并入顶层（advanced-json-editor
    // §4.2/§6）——warnings 不再透出（坏 JSON 一侧按空处理的行为与接管链一致；
    // 用户可见的留痕由调用方在取数处做）
    applyExtraSettingsToSettingsDoc(
        root, effectiveExtraSettingsJson(p.extraSettingsJson, fragmentJson));
    auto text = platform::json::stringify(
        platform::json::JsonValue(std::move(root)), /*pretty=*/true);
    if (text.isFail()) {
        // 全字符串值的两段序列化，失败理论不可达；空串 = 失败信号，
        // 调用方按失败走 toast，不静默成功
        return std::string();
    }
    return text.value();
}

} // namespace module::claude
