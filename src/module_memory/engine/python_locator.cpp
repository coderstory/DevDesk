// src/module_memory/engine/python_locator.cpp
#include "python_locator.h"

#include "core/logging.h"
#include "platform/process_runner.h"

#include <cstdio>
#include <regex>

namespace module::memory::engine {

std::string parsePythonVersionOutput(const std::string& stdoutText) {
    // 首个形如 3.X[.Z] 的版本段；补丁位可缺
    static const std::regex kVersion(R"re(3\.\d{1,2}(\.\d+)?)re");
    std::smatch m;
    if (!std::regex_search(stdoutText, m, kVersion)) return {};
    return m.str();
}

bool pythonVersionQualified(const std::string& versionText) {
    if (versionText.empty()) return false;
    int major = 0, minor = 0;
    if (std::sscanf(versionText.c_str(), "%d.%d", &major, &minor) != 2) {
        return false;
    }
    if (major != 3) return false;
    // bug157（2026-11-03）：telemem 1.10.1 在 3.13.15 venv 实测兼容（依赖链
    // faiss/numpy/mem0ai/mcp 全装成功 + import 通过），版本闸放宽到 3.13。
    return minor >= 10 && minor <= 13;
}

VersionProbe systemVersionProbe() {
    return [](const std::string& exe) {
        const platform::ProcessRunner runner = platform::systemProcessRunner();
        if (!runner) return std::string();
        const core::Result<platform::ProcessResult> r =
            runner({exe, "-c", kVersionProbeScript}, "", "", 10000);
        if (!r.isOk() || r.value().exitCode != 0) return std::string();
        return r.value().stdoutText;
    };
}

std::optional<PythonInfo> findQualifiedPython(const VersionProbe& probe) {
    if (!probe) return std::nullopt;
    for (const std::string& exe : pythonCandidatePaths()) {
        if (exe.empty()) continue;
        const std::string out = probe(exe);
        const std::string version = parsePythonVersionOutput(out);
        if (!pythonVersionQualified(version)) continue;
        return PythonInfo{exe, version};
    }
    return std::nullopt;
}

} // namespace module::memory::engine
