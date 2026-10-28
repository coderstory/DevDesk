// src/module_memory/engine/python_locator.cpp
#include "python_locator.h"

#include "core/logging.h"
#include "platform/process_runner.h"

#include <cstdio>
#include <regex>

namespace module::memory::engine {

namespace {
// 版本探针小脚本：只吐主.次.补丁，避免平台横幅混入（stderr 另行捕获不拼进结果）
constexpr const char* kVersionScript = "import sys;print('%d.%d.%d'%sys.version_info[:3])";
} // namespace

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
    return minor >= 10 && minor <= 12;
}

VersionProbe systemVersionProbe() {
    return [](const std::string& exe) {
        const platform::ProcessRunner runner = platform::systemProcessRunner();
        if (!runner) return std::string();
        const core::Result<platform::ProcessResult> r =
            runner({exe, "-c", kVersionScript}, "", "", 10000);
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
