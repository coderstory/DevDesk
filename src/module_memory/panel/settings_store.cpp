// src/module_memory/panel/settings_store.cpp
#include "settings_store.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "core/logging.h"
#include "module_memory/engine/telemem_config.h"
#include "platform/json.h"

namespace module::memory::panel {

namespace {

std::filesystem::path settingsFile() {
    return engine::memoryDataDir() / "panel_settings.json";
}

}  // namespace

MemoryPanelSettings loadPanelSettings() {
    MemoryPanelSettings s;
    FILE* f = std::fopen(settingsFile().string().c_str(), "rb");
    if (f == nullptr) return s;  // 首跑无文件 = 全默认
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    auto parsed = platform::json::parse(text);
    if (parsed.isFail() || !parsed.value().isObject()) return s;
    const auto obj = parsed.value().asObject();
    const auto getBool = [&obj](const char* k, bool dflt) {
        const auto v = obj.get(k);
        return v.isBool() ? v.asBool() : dflt;
    };
    const auto getInt = [&obj](const char* k, int dflt) {
        const auto v = obj.get(k);
        return v.isNumber() ? static_cast<int>(v.asInt()) : dflt;
    };
    // 需-9 token 阈值量级 10^5~10^7，超 int 保险域，走 int64 读
    const auto getInt64 = [&obj](const char* k, long long dflt) {
        const auto v = obj.get(k);
        return v.isNumber() ? static_cast<long long>(v.asInt()) : dflt;
    };
    const auto getStr = [&obj](const char* k) {
        const auto v = obj.get(k);
        return v.isString() ? v.asString() : std::string();
    };
    s.sanitizeEnabled = getBool("sanitizeEnabled", true);
    s.recallThresholdPct = getInt("recallThresholdPct", 85);
    s.twoWordBaseline = getInt("twoWordBaseline", 5);
    s.llmImportedAtSec = obj.get("llmImportedAtSec").isNumber()
                             ? obj.get("llmImportedAtSec").asInt()
                             : 0;
    s.llmProfileName = getStr("llmProfileName");
    s.mcpSearchEnabled = getBool("mcpSearchEnabled", true);
    // 需-9 用量告警（旧文件缺键 = 全默认：三档阈值常量 + 闸全武装零击发）
    s.usageDayAlertTokens = getInt64("usageDayAlertTokens", 500000);
    s.usageSoftBudgetTokens = getInt64("usageSoftBudgetTokens", 2000000);
    s.usageHardBudgetTokens = getInt64("usageHardBudgetTokens", 3000000);
    s.usageDayGate.armed = getBool("usageAlertDayArmed", true);
    s.usageDayGate.lastFireDay = getInt64("usageAlertDayLastDay", 0);
    s.usageSoftGate.armed = getBool("usageAlertSoftArmed", true);
    s.usageSoftGate.lastFireDay = getInt64("usageAlertSoftLastDay", 0);
    s.usageHardGate.armed = getBool("usageAlertHardArmed", true);
    s.usageHardGate.lastFireDay = getInt64("usageAlertHardLastDay", 0);
    return s;
}

core::Result<void> savePanelSettings(const MemoryPanelSettings& settings) {
    platform::json::JsonObject obj;
    obj.set("sanitizeEnabled", platform::json::JsonValue(settings.sanitizeEnabled));
    obj.set("recallThresholdPct",
            platform::json::JsonValue(settings.recallThresholdPct));
    obj.set("twoWordBaseline",
            platform::json::JsonValue(settings.twoWordBaseline));
    obj.set("llmImportedAtSec",
            platform::json::JsonValue(settings.llmImportedAtSec));
    obj.set("llmProfileName", platform::json::JsonValue(settings.llmProfileName));
    obj.set("mcpSearchEnabled",
            platform::json::JsonValue(settings.mcpSearchEnabled));
    obj.set("usageDayAlertTokens",
            platform::json::JsonValue(settings.usageDayAlertTokens));
    obj.set("usageSoftBudgetTokens",
            platform::json::JsonValue(settings.usageSoftBudgetTokens));
    obj.set("usageHardBudgetTokens",
            platform::json::JsonValue(settings.usageHardBudgetTokens));
    obj.set("usageAlertDayArmed",
            platform::json::JsonValue(settings.usageDayGate.armed));
    obj.set("usageAlertDayLastDay",
            platform::json::JsonValue(settings.usageDayGate.lastFireDay));
    obj.set("usageAlertSoftArmed",
            platform::json::JsonValue(settings.usageSoftGate.armed));
    obj.set("usageAlertSoftLastDay",
            platform::json::JsonValue(settings.usageSoftGate.lastFireDay));
    obj.set("usageAlertHardArmed",
            platform::json::JsonValue(settings.usageHardGate.armed));
    obj.set("usageAlertHardLastDay",
            platform::json::JsonValue(settings.usageHardGate.lastFireDay));
    auto text = platform::json::stringify(
        platform::json::JsonValue(obj), true);
    if (text.isFail()) {
        LOG_WARN("记忆面板设置序列化失败: " + text.errorMessage());
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        text.errorMessage());
    }
    std::error_code ec;
    std::filesystem::create_directories(engine::memoryDataDir(), ec);
    FILE* f = std::fopen(settingsFile().string().c_str(), "wb");
    if (f == nullptr) {
        LOG_WARN("记忆面板设置写入失败（目录不可写）");
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "目录不可写");
    }
    // 短写 / fclose 冲刷失败（磁盘满最常显形处）都必须报 fail：半截 JSON 下次
    // load 会整体落默认，mcpSearchEnabled=false 的工具会借机复活
    const std::string& payload = text.value();
    const std::size_t written =
        std::fwrite(payload.data(), 1, payload.size(), f);
    const bool flushed = std::fclose(f) == 0;
    if (written != payload.size() || !flushed) {
        LOG_WARN("记忆面板设置写入不完整（磁盘满/IO）");
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "写入不完整");
    }
    return core::Result<void>::ok();
}

}  // namespace module::memory::panel
