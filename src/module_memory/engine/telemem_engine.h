// src/module_memory/engine/telemem_engine.h
// telemem + llama-server 托管编排器（S2 批次②④）。真副作用（进程/网络/装包）全部
// 收敛在单一后台工作线程；compose 侧只读 snapshot()（锁内拷贝，非阻塞）。
// 状态迁移契约（UI 依赖，勿改语义）：
//   Idle →(startAsync)→ Preparing →(全绿)→ Running
//   Preparing 途中任何步失败 → Failed（detail 面板可读，重试 = 再按启动）
//   bug182（2026-11-07）：Running 前置 = embedding 链就绪（llama-server + 模型）——
//   embedding 缺席时置 Failed + 引导文案，不再「半装好也 Running」
//   Running 中进程被外杀 → snapshot 探活发现 → Failed（一键重启 = startAsync）
//   stopAsync → Stopped（幂等；Preparing 中调用 = 中止请求，工作线程自检后落 Stopped）
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace module::memory::engine {

class TelememEngine {
public:
    enum class State { Idle, Preparing, Running, Stopped, Failed };

    // embedding 本机包就位态（need:202 F-04/F-05；设计 §4 四字段映射）。
    // UI 只读这些字段渲染，不自行探测文件系统。
    enum class EmbedSource { None, Pkg, DataRoot };
    enum class EmbedState {
        Unknown,      // 未探测（管线未跑、未开过向导）
        NeedDownload, // llama 在场但模型缺（idle 引导态，可走 HF 下载）
        Downloading,  // 管线下载中（detail 带进度）
        Ready,        // 就位（embedSource 区分随包/数据根）
        Failed        // 定位/下载/拉起/健康失败（embedDetail 可诊断，可重试）
    };

    struct Snapshot {
        State state = State::Idle;
        std::string detail;        // 当前步/失败原因（面板可读；已过 redactSecretsForLog）
        std::string pythonVersion; // 合格解释器版本（如 "3.12.4"），未找到为空
        bool pythonFound = false;
        bool llamaMissing = false; // llama-server 定位失败（embedding 档不可用的引导态）
        // ── embedding 本机包四字段（向导③步与主控台就绪口径共用）──
        bool embedReady = false;
        EmbedSource embedSource = EmbedSource::None;
        EmbedState embedState = EmbedState::Unknown;
        std::string embedDetail; // 进度/失败原因/就位来源（可诊断）
    };

    // 模块级单例：MemoryModule 与后续 hook 壳共用同一托管实例
    static TelememEngine& global();

    ~TelememEngine();

    TelememEngine(const TelememEngine&) = delete;
    TelememEngine& operator=(const TelememEngine&) = delete;

    Snapshot snapshot() const;  // 任意线程/每帧可调

    void startAsync();  // Preparing/Running 中为 no-op（防重复拉起）
    void stopAsync();   // 幂等；杀进程树，不等待工作线程（UI 线程禁阻塞）

    // 向导③步即时检查（need:202 F-04）：本地文件探测一次（llama 定位 + 模型定位），
    // 只在 Idle/Stopped 下覆写 embed* 字段——Preparing/Running/Failed 下字段由管线
    // 独占，调用是 no-op。UI 线程可调（纯 stat，几毫秒）；打开向导时触发一次。
    void refreshEmbedProbe();

    // 测试缝（memory_probe 组 E 用；与 MemoryModule::setActiveViewForTest 同族）：
    // 锁内整表覆写快照供 UI 断言，不触碰 worker/进程句柄。仅测试进程调用。
    void setSnapshotForTest(const Snapshot& snap);

    // 测试缝（同族）：worker 线程是否已退出（startAsync 可再拉起）。端态 setState
    // 之后 worker 收尾清 workerActive_ 才算真正空闲——「端态可见」≠「可重启」，
    // 连跑两条管线测试时需要后者。
    bool workerIdleForTest() const { return !workerActive_.load(); }

private:
    TelememEngine() = default;
    void runPipeline();
    void setState(State state, std::string detail);

    // 反重构警告：pipeline 内禁止在持有 mu_ 时调用 setState（非递归锁必死锁）——
    // 先在锁内改 snap_ 字段并出作用域，再 setState。
    mutable std::mutex mu_;
    Snapshot snap_;
    std::thread worker_;
    std::atomic<bool> workerActive_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<std::int64_t> telememPid_{0};
    std::atomic<std::int64_t> llamaPid_{0};
};

// 回环探活（need 219 F-04 memory.health 工具取数口）：复用引擎管线同款 probe
// 判据，对 kTelememPort /mcp 发 initialize 探测；reasonOut 非空时给中文失败
// 原因。引擎未运行时连接拒绝即失败返回 false，任意线程可调。
bool probeEngineHealthy(std::string* reasonOut);

} // namespace module::memory::engine
