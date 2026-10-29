// src/module_memory/engine/telemem_config.cpp
#include "telemem_config.h"

#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"

#include <fstream>
#include <system_error>

namespace module::memory::engine {

namespace {

constexpr const char* kProxyPlaceholder = "PROXY_MANAGED";  // DevDesk/cc-switch 托管标记

std::string forwardSlashes(const std::filesystem::path& p) {
    const std::string s = p.generic_string();
    return s.empty() ? std::string(".") : s;
}

} // namespace

std::filesystem::path memoryDataDir() {
    return ::core::paths::appDataDir() / "memory";
}

std::string generateTelememConfigYaml(const std::filesystem::path& dataDir) {
    const std::string base = forwardSlashes(dataDir);
    std::string yaml;
    yaml += "# DevDesk 生成（勿手改：每次引擎启动重写）。\n";
    yaml += "# 密钥红线：api_key 恒为空（空串走 mem0 的环境回退），真实 key 由拉起方\n";
    yaml += "# 经环境变量 ";
    yaml += kLlmKeyEnvName;
    yaml += " 注入，不落盘不进日志。\n";
    yaml += "llm:\n";
    yaml += std::string("  provider: ") + kLlmProvider + "\n";
    yaml += "  config:\n";
    yaml += std::string("    anthropic_base_url: ") + kLlmBaseUrl + "\n";
    yaml += std::string("    model: ") + kLlmModel + "\n";
    yaml += "    api_key: \"\"\n";
    yaml += "embedder:\n";
    yaml += "  provider: openai\n";
    yaml += "  config:\n";
    yaml += "    openai_base_url: http://127.0.0.1:" + std::to_string(kEmbedServerPort) +
            "/v1\n";
    yaml += std::string("    model: ") + kEmbedModel + "\n";
    yaml += "    api_key: \"local\"\n";
    yaml += std::string("    embedding_dims: ") + std::to_string(kEmbedDims) + "\n";
    yaml += "vector_store:\n";
    yaml += "  provider: faiss\n";
    yaml += "  config:\n";
    yaml += "    path: " + base + "/faiss_db\n";
    yaml += std::string("    embedding_model_dims: ") + std::to_string(kEmbedDims) +
            "\n";
    yaml += "history_db_path: " + base + "/history.db\n";
    return yaml;
}

std::string extractLlmKeyFromSettingsJson(const std::string& settingsJsonText) {
    auto parsed = platform::json::parse(settingsJsonText);
    if (!parsed.isOk()) return {};
    const platform::json::JsonValue root = parsed.value();
    if (!root.isObject()) return {};
    const platform::json::JsonObject obj = root.asObject();
    if (!obj.has("env")) return {};
    const platform::json::JsonValue env = obj.get("env");
    if (!env.isObject()) return {};
    const platform::json::JsonObject envObj = env.asObject();
    for (const char* key : {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_API_KEY"}) {
        if (!envObj.has(key)) continue;
        const platform::json::JsonValue v = envObj.get(key);
        if (!v.isString()) continue;
        const std::string value = v.asString();
        if (!value.empty() && value != kProxyPlaceholder) return value;
    }
    return {};
}

std::string readLlmKeyFromClaudeSettings() {
    std::error_code ec;
    const std::filesystem::path file = ::core::paths::claudeSettingsFile();
    if (!std::filesystem::exists(file, ec) || ec) return {};
    // ifstream 直收 fs::path：Windows 宽字符路径原生支持，无需 _wfopen_s 平台分叉
    std::ifstream f(file, std::ios::binary);
    if (!f.is_open()) return {};
    std::string text;
    char buf[4096];
    while (f.good()) {
        f.read(buf, sizeof(buf));
        text.append(buf, static_cast<size_t>(f.gcount()));
        if (text.size() > 4u * 1024u * 1024u) break;  // settings.json 不该这么大，防呆
    }
    return extractLlmKeyFromSettingsJson(text);
}

std::string redactSecretsForLog(const std::string& text,
                                const std::string& secret) {
    if (secret.empty()) return text;
    std::string out = text;
    size_t pos = 0;
    while ((pos = out.find(secret, pos)) != std::string::npos) {
        out.replace(pos, secret.size(), "<redacted>");
        pos += 11;  // strlen("<redacted>")
    }
    return out;
}

} // namespace module::memory::engine
