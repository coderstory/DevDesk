// src/module_memory/engine/telemem_process.h
// telemem 进程参数构造与健康判据（纯逻辑，零平台宏；单测注入假执行体、不启真引擎）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace module::memory::engine {

// venv 创建：python -m venv <dir>
std::vector<std::string> buildVenvArgs(const std::string& pythonExe,
                                       const std::filesystem::path& venvDir);

// venv 内解释器（平台布局差异：Windows Scripts\python.exe / POSIX bin\python3；
// 实现按平台拆 telemem_process_win/_posix.cpp，业务零平台宏）
std::filesystem::path venvPythonExe(const std::filesystem::path& venvDir);

// 首跑安装：venvPython -m pip install --disable-pip-version-check telemem
//（pip 进度对 C++ 侧不可流式读，进度口径 = 阶段文本；失败由面板重试入口重跑）
std::vector<std::string> buildPipInstallArgs(
    const std::filesystem::path& venvDir);

// telemem 常驻进程 argv：venvPython -m telemem serve --http --host 127.0.0.1 --port <port>
//（只绑回环：外部机器连不上，端口校验单测钉死）
std::vector<std::string> buildTelememServerArgs(
    const std::filesystem::path& venvDir, int port);

// 拉起环境：TELEMEM_CONFIG=<configPath>（telemem 官方配置注入口）+
// kLlmKeyEnvName=<key>（密钥唯一落点 = 子进程环境；config 文件里恒无 key）。
// key 为空时该变量不注入（不喂空串占位）。
std::vector<std::string> buildTelememEnv(const std::filesystem::path& configPath,
                                         const std::string& llmKey);

// 健康判据（纯函数）。契约：错误嵌 result 体——telemem 的结构化错误长在 HTTP 200 的
// body 里（body.error 或 body.result.error），绝不能只看传输层是否报错。
//   networkError            → 不健康（网络层不通/连接拒绝）
//   httpStatus != 200       → 不健康
//   body 非法 JSON          → 不健康（不是我们的引擎占着端口）
//   body.error / body.result.error 存在 → 不健康（引擎活着但报错；进程不误杀）
//   其余                    → 健康
struct HealthVerdict {
    bool healthy = false;
    std::string reason;  // 不健康时的人类可读原因（可进面板/日志）
};
HealthVerdict judgeTelememHealth(int httpStatus, bool networkError,
                                 const std::string& body);

} // namespace module::memory::engine
