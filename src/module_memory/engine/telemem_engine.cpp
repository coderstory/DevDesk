// src/module_memory/engine/telemem_engine.cpp
#include "telemem_engine.h"

#include "llama_host.h"
#include "python_locator.h"
#include "telemem_config.h"
#include "telemem_process.h"
#include "core/atomic_write.h"
#include "core/logging.h"
#include "platform/http_client.h"
#include "platform/process_host.h"
#include "platform/process_runner.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

namespace module::memory::engine {

namespace {

using platform::process_host::isAlive;
using platform::process_host::spawnDetached;
using platform::process_host::terminateTree;

// 拉起后探活窗口：telemem 冷启动（Python import + FAISS 加载）与 llama 载模型
// 都以十秒计；500ms 步进轮询，窗口内没就绪判 Failed（可一键重试）。
constexpr int kStartupPollMs = 500;
constexpr int kStartupWindowMs = 60000;
// 健康探测单次 HTTP 超时（探活不能拖住工作线程）
constexpr long kHealthTimeoutMs = 1500;
// pip 安装超时：telemem 依赖链 faiss+numpy+mem0ai，慢网按十分钟上限
constexpr long kPipTimeoutMs = 600000;

std::filesystem::path venvDir() { return memoryDataDir() / "venv"; }
std::filesystem::path configFilePath() {
    return memoryDataDir() / "telemem-config.yaml";
}

// telemem MCP 健康探测请求体：JSON-RPC initialize（MCP 流式 HTTP 只在 /mcp 挂路由，
// GET 会吃 405/404；initialize 不要求会话头，200 + body.result 即引擎活着）
constexpr const char* kTelememProbeBody =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"devdesk-health\",\"version\":\"1.0\"}}}";

// MCP 流式 HTTP 的 200 响应体是 SSE 信封（event:/data: 行），判据吃的是纯
// JSON——剥出首个 data: 行的载荷再进判据；纯 JSON 响应（非 SSE）原样通过。
std::string stripSseEnvelope(const std::string& body) {
    if (body.rfind("event:", 0) != 0) return body;
    static constexpr const char* kTag = "data: ";
    const size_t at = body.find(kTag);
    if (at == std::string::npos) return body;
    const size_t begin = at + std::strlen(kTag);
    const size_t end = body.find('\n', begin);
    return body.substr(begin,
                       end == std::string::npos ? std::string::npos
                                                : end - begin);
}

// 回环健康探测（判据 = judgeTelememHealth 的纯函数契约：错误嵌 result 体，
// 不能只看传输层 is_error）。jsonBody 非空 = POST JSON-RPC（telemem /mcp），
// 空 = 普通 GET（llama-server /health）。
bool probeHealthy(int port, const char* path, const char* jsonBody,
                  std::string* reasonOut) {
    platform::http::Request req;
    req.method = jsonBody != nullptr ? "POST" : "GET";
    req.url = "http://127.0.0.1:" + std::to_string(port) + path;
    if (jsonBody != nullptr) {
        req.body = jsonBody;
        req.headers["Content-Type"] = "application/json";
        // MCP SDK 硬校验：Accept 必须同时含 json 与 event-stream，缺一个 406
        req.headers["Accept"] = "application/json, text/event-stream";
    }
    req.timeoutMs = kHealthTimeoutMs;
    const auto r = platform::http::send(req);
    if (!r.isOk()) {
        if (reasonOut != nullptr) *reasonOut = r.errorMessage();
        return false;
    }
    const platform::http::Response& resp = r.value();
    const HealthVerdict v = judgeTelememHealth(resp.statusCode, resp.networkError,
                                               stripSseEnvelope(resp.body));
    if (reasonOut != nullptr) *reasonOut = v.reason;
    return v.healthy;
}

bool waitUntilHealthy(int port, const char* path, const char* jsonBody,
                      const std::atomic<bool>& stop, std::string* errOut) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kStartupWindowMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop.load()) return false;
        if (probeHealthy(port, path, jsonBody, errOut)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(kStartupPollMs));
    }
    return false;
}

// 同步跑一条安装类命令（venv/pip/import 自检），返回退出码；-1 = 启动失败或超时
int runInstallStep(const std::vector<std::string>& argv, long timeoutMs) {
    const platform::ProcessRunner runner = platform::systemProcessRunner();
    if (!runner) return -1;
    const auto r = runner(argv, "", "", timeoutMs);
    if (!r.isOk()) return -1;
    if (r.value().timedOut) return -1;
    return r.value().exitCode;
}

} // namespace

TelememEngine& TelememEngine::global() {
    static TelememEngine instance;
    return instance;
}

TelememEngine::~TelememEngine() {
    stopRequested_.store(true);
    terminateTree(telememPid_.load());
    terminateTree(llamaPid_.load());
    if (worker_.joinable()) worker_.join();
}

TelememEngine::Snapshot TelememEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    Snapshot snap = snap_;
    // 契约：Running 中进程被外杀 → 面板显式提示（Failed），一键重启可拉回
    if (snap.state == State::Running) {
        const std::int64_t telememPid = telememPid_.load();
        if (telememPid != 0 && !isAlive(telememPid)) {
            snap.state = State::Failed;
            snap.detail = "引擎进程已退出（被外部终止或崩溃），可一键重启";
        }
    }
    return snap;
}

void TelememEngine::setState(State state, std::string detail) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.state = state;
    snap_.detail = std::move(detail);
}

void TelememEngine::startAsync() {
    if (workerActive_.load()) return;  // Preparing/Running 中：防重复拉起
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.llamaMissing = false;
    }
    stopRequested_.store(false);
    workerActive_.store(true);
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] {
        runPipeline();
        workerActive_.store(false);
    });
}

void TelememEngine::stopAsync() {
    stopRequested_.store(true);
    setState(State::Stopped, "引擎已停止（检索与新提炼均不可用，可一键重启）");
    terminateTree(telememPid_.exchange(0));
    terminateTree(llamaPid_.exchange(0));
    LOG_INFO("记忆引擎：已请求停止");
}

void TelememEngine::runPipeline() {
    const std::filesystem::path dataDir = memoryDataDir();
    std::error_code ec;

    // ① Python 3.10~3.12 探测（不合格在此拦截，不放行到 pip）
    setState(State::Preparing, "探测本机 Python（需要 3.10~3.12）…");
    const auto py = findQualifiedPython(systemVersionProbe());
    if (!py.has_value()) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            snap_.pythonFound = false;
            snap_.pythonVersion.clear();
        }  // 出锁再 setState：非递归锁，持锁重入必死锁
        setState(State::Failed,
                 "未找到 Python 3.10~3.12：请安装后点「启动引擎」重试"
                 "（3.13 及以上版本暂不支持）");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.pythonFound = true;
        snap_.pythonVersion = py->versionText;
    }

    // ② venv 建在用户数据目录（随 DEVDESK_DATA_ROOT 隔离）
    std::filesystem::create_directories(dataDir, ec);
    const std::filesystem::path venv = venvDir();
    if (!std::filesystem::exists(venvPythonExe(venv), ec)) {
        setState(State::Preparing, "创建虚拟环境（venv）…");
        const int rc = runInstallStep(buildVenvArgs(py->exePath, venv), kPipTimeoutMs);
        if (rc != 0) {
            setState(State::Failed, "创建 venv 失败（退出码 " + std::to_string(rc) +
                                        "），可点「启动引擎」重试");
            return;
        }
    }

    // ③ telemem 首跑安装（缺才装；进度口径 = 阶段文本，失败可重试）
    setState(State::Preparing, "检查 telemem 安装状态…");
    const int importRc =
        runInstallStep({venvPythonExe(venv).string(), "-c", "import telemem"},
                       30000);
    if (importRc != 0) {
        setState(State::Preparing, "安装 telemem（首次约几分钟，取决于网速）…");
        const int rc = runInstallStep(buildPipInstallArgs(venv), kPipTimeoutMs);
        if (rc != 0) {
            setState(State::Failed, "pip 安装 telemem 失败（退出码 " +
                                        std::to_string(rc) + "），可重试");
            return;
        }
    }

    // ④ TELEMEM_CONFIG 生成（密钥不落盘；key 只进子进程环境）
    std::filesystem::create_directories(dataDir / "faiss_db", ec);
    const auto written = core::atomicWriteFile(
        configFilePath(), generateTelememConfigYaml(dataDir), "telemem 配置");
    if (!written.isOk()) {
        setState(State::Failed, "写配置失败: " + written.errorMessage());
        return;
    }

    // ⑤ llama-server：定位 → 模型下载 → 拉起 → 健康。失败不拦 telemem：
    // embedding 缺席时引擎可运行，add/search 报工具级错误，面板另行引导。
    const std::filesystem::path llamaExe = locateLlamaServer(dataDir);
    if (llamaExe.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.llamaMissing = true;
    } else {
        setState(State::Preparing, "准备 embedding 模型（首次需下载）…");
        const auto dl = ensureEmbedModelDownloaded(dataDir);
        if (dl.isOk() && !stopRequested_.load()) {
            setState(State::Preparing, "启动本机 embedding 服务…");
            const auto spawn = spawnDetached(
                buildLlamaServerArgs(llamaExe, llamaModelPath(dataDir),
                                     kEmbedServerPort),
                {}, (dataDir / "models").string());
            if (spawn.isOk()) {
                llamaPid_.store(spawn.value());
                std::string healthErr;
                if (!waitUntilHealthy(kEmbedServerPort, "/health", nullptr,
                                      stopRequested_, &healthErr) &&
                    !stopRequested_.load()) {
                    terminateTree(llamaPid_.exchange(0));
                    std::lock_guard<std::mutex> lock(mu_);
                    snap_.llamaMissing = true;
                    LOG_WARN("记忆引擎：embedding 服务健康检查超时——" + healthErr);
                }
            } else {
                std::lock_guard<std::mutex> lock(mu_);
                snap_.llamaMissing = true;
                LOG_WARN("记忆引擎：llama-server 拉起失败——" + spawn.errorMessage());
            }
        } else if (!dl.isOk()) {
            std::lock_guard<std::mutex> lock(mu_);
            snap_.llamaMissing = true;
            LOG_WARN("记忆引擎：模型下载失败——" + dl.errorMessage());
        }
    }
    if (stopRequested_.load()) {
        setState(State::Stopped, "引擎已停止");
        return;
    }

    // ⑥ telemem 拉起（TELEMEM_CONFIG + key 环境注入）→ 回环 8421 健康
    setState(State::Preparing, "启动记忆引擎…");
    const std::string llmKey = readLlmKeyFromClaudeSettings();
    if (llmKey.empty()) {
        setState(State::Failed,
                 "未读取到智谱 API key：请确认 Claude 设置（settings.json）中已配置");
        return;
    }
    const auto spawn = spawnDetached(
        buildTelememServerArgs(venv, kTelememPort),
        buildTelememEnv(configFilePath(), llmKey), dataDir.string());
    if (!spawn.isOk()) {
        setState(State::Failed, "引擎进程拉起失败: " + spawn.errorMessage());
        return;
    }
    telememPid_.store(spawn.value());

    std::string healthErr;
    if (!waitUntilHealthy(kTelememPort, "/mcp", kTelememProbeBody,
                          stopRequested_, &healthErr)) {
        terminateTree(telememPid_.exchange(0));
        terminateTree(llamaPid_.exchange(0));
        if (stopRequested_.load()) {
            setState(State::Stopped, "引擎已停止");
        } else {
            setState(State::Failed,
                     "引擎健康检查超时（" + healthErr + "），可一键重启");
        }
        return;
    }
    // 日志红线：进日志的任何文本先脱敏（当前 detail 不含 key，防御性兜底）
    const std::string runningDetail =
        "运行中（127.0.0.1:" + std::to_string(kTelememPort) + "）";
    setState(State::Running, runningDetail);
    LOG_INFO("记忆引擎：已就绪 " + redactSecretsForLog(runningDetail, llmKey));
}

} // namespace module::memory::engine
