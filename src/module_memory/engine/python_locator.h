// src/module_memory/engine/python_locator.h
// Python 解释器探测（telemem 要求 3.10~3.13；本机 3.14 不合格——探测即拦截，
// 不放行到 pip 安装步才失败）。候选序与版本判定分离：候选清单按平台拆文件，
// 版本解析/合格判定是纯逻辑（单测钉死）。
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace module::memory::engine {

struct PythonInfo {
    std::string exePath;
    std::string versionText;  // "3.12.4" 形态（仅数字与点）
};

// 从 `<python> -c "import sys;print(...)"` 的 stdout 提取版本串（纯函数）。
// 取首个形如 X.Y.Z 的数字段；无匹配 → 空串。
std::string parsePythonVersionOutput(const std::string& stdoutText);

// 版本探针脚本（bug202 收拢共享）：`<exe> -c <脚本>` → stdout "3.12.4" 形态，
// 只吐主.次.补丁避免平台横幅混入。生产探测（python_locator.cpp）与 venv 快路径
//（telemem_engine.cpp）同源一处，解析统一走 parsePythonVersionOutput。
inline constexpr const char* kVersionProbeScript =
    "import sys;print('%d.%d.%d'%sys.version_info[:3])";

// 合格判定：3.10 ≤ 版本 ≤ 3.13（bug157 放宽：telemem 1.10.1 对 3.13 venv
// 实测兼容；3.14+ 与 3.9- 仍在探测步拦截）。空/残缺版本串 → false。
bool pythonVersionQualified(const std::string& versionText);

// 版本探针缝：给候选解释器路径，返回其版本 stdout。生产 = exec_capture 真跑
// `<exe> -c ...`；单测注入假探针（不启任何真进程）。返回空串 = 探测失败。
using VersionProbe = std::function<std::string(const std::string& exe)>;

// 平台候选清单（_win/_posix 实现；序即优先级）：
//   Windows：uv 管的 cpython-3.12（%LOCALAPPDATA%\uv\python\）→ 官方安装器目录 → PATH python/python3
//   POSIX：uv 管的 cpython-3.12（~/.local/share/uv/python/）→ Homebrew（/opt/homebrew/bin
//     与 /usr/local/bin 的 python3.13/3.12/3.11/python3，bug202）→ PATH python3.12/python3
std::vector<std::string> pythonCandidatePaths();

// 生产探针：真跑 `<exe> -c "import sys;print(...)"`（exec_capture 同步等待；
// 只允许在后台线程调用）。启动失败/超时 → 空串。
VersionProbe systemVersionProbe();

// 逐候选跑探针，返回首个 3.10~3.13 合格者；全不合格 → nullopt（面板走引导态）。
// 每个候选先 parse 再判合格；候选路径为空跳过。
std::optional<PythonInfo> findQualifiedPython(const VersionProbe& probe);

} // namespace module::memory::engine
