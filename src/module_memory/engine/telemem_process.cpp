// src/module_memory/engine/telemem_process.cpp
#include "telemem_process.h"

#include "telemem_config.h"
#include "platform/json.h"

#include <filesystem>

namespace module::memory::engine {

namespace fs = std::filesystem;

std::vector<std::string> buildVenvArgs(const std::string& pythonExe,
                                       const fs::path& venvDir) {
    return {pythonExe, "-m", "venv", venvDir.string()};
}

std::vector<std::string> buildPipInstallArgs(const fs::path& venvDir) {
    return {venvPythonExe(venvDir).string(), "-m", "pip", "install",
            "--disable-pip-version-check", "telemem"};
}

std::vector<std::string> buildTelememServerArgs(const fs::path& venvDir,
                                                int port) {
    return {venvPythonExe(venvDir).string(),
            "-m",
            "telemem.mcp",
            "--transport",
            "streamable-http",
            "--host",
            "127.0.0.1",
            "--port",
            std::to_string(port)};
}

std::vector<std::string> buildTelememEnv(const fs::path& configPath,
                                         const std::string& llmKey) {
    std::vector<std::string> env;
    env.push_back("TELEMEM_CONFIG=" + configPath.string());
    if (!llmKey.empty()) {
        env.push_back(std::string(kLlmKeyEnvName) + "=" + llmKey);
    }
    return env;
}

HealthVerdict judgeTelememHealth(int httpStatus, bool networkError,
                                 const std::string& body) {
    if (networkError) return {false, "连接失败（引擎未启动或端口不通）"};
    if (httpStatus != 200) {
        return {false, "HTTP " + std::to_string(httpStatus)};
    }
    const auto parsed = platform::json::parse(body);
    if (!parsed.isOk() || !parsed.value().isObject()) {
        return {false, "响应体非 JSON（端口被其他进程占用）"};
    }
    const platform::json::JsonValue root = parsed.value();
    // 契约：错误嵌 result 体。两层都要查，缺一层就把「引擎活着但报错」误判成健康。
    const platform::json::JsonObject obj = root.asObject();
    if (obj.has("error")) {
        return {false, "引擎返回错误（嵌顶层 error）"};
    }
    if (!obj.has("result")) return {true, {}};
    const platform::json::JsonValue result = obj.get("result");
    if (result.isObject() && result.asObject().has("error")) {
        return {false, "引擎返回错误（嵌 result.error）"};
    }
    return {true, {}};
}

} // namespace module::memory::engine
