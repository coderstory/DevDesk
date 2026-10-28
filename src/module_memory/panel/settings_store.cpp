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
    return s;
}

void savePanelSettings(const MemoryPanelSettings& settings) {
    platform::json::JsonObject obj;
    obj.set("sanitizeEnabled", platform::json::JsonValue(settings.sanitizeEnabled));
    obj.set("recallThresholdPct",
            platform::json::JsonValue(settings.recallThresholdPct));
    obj.set("twoWordBaseline",
            platform::json::JsonValue(settings.twoWordBaseline));
    obj.set("llmImportedAtSec",
            platform::json::JsonValue(settings.llmImportedAtSec));
    obj.set("llmProfileName", platform::json::JsonValue(settings.llmProfileName));
    auto text = platform::json::stringify(
        platform::json::JsonValue(obj), true);
    if (text.isFail()) {
        LOG_WARN("记忆面板设置序列化失败: " + text.errorMessage());
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(engine::memoryDataDir(), ec);
    FILE* f = std::fopen(settingsFile().string().c_str(), "wb");
    if (f == nullptr) {
        LOG_WARN("记忆面板设置写入失败（目录不可写）");
        return;
    }
    std::fwrite(text.value().data(), 1, text.value().size(), f);
    std::fclose(f);
}

}  // namespace module::memory::panel
