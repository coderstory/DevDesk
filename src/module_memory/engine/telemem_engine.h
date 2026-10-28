// src/module_memory/engine/telemem_engine.h
// telemem + llama-server 托管编排器（S2 批次②④）。真副作用（进程/网络/装包）全部
// 收敛在单一后台工作线程；compose 侧只读 snapshot()（锁内拷贝，非阻塞）。
// 状态迁移契约（UI 依赖，勿改语义）：
//   Idle →(startAsync)→ Preparing →(全绿)→ Running
//   Preparing 途中任何步失败 → Failed（detail 面板可读，重试 = 再按启动）
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

    struct Snapshot {
        State state = State::Idle;
        std::string detail;        // 当前步/失败原因（面板可读；已过 redactSecretsForLog）
        std::string pythonVersion; // 合格解释器版本（如 "3.12.4"），未找到为空
        bool pythonFound = false;
        bool llamaMissing = false; // llama-server 定位失败（embedding 档不可用的引导态）
    };

    // 模块级单例：MemoryModule 与后续 hook 壳共用同一托管实例
    static TelememEngine& global();

    ~TelememEngine();

    TelememEngine(const TelememEngine&) = delete;
    TelememEngine& operator=(const TelememEngine&) = delete;

    Snapshot snapshot() const;  // 任意线程/每帧可调

    void startAsync();  // Preparing/Running 中为 no-op（防重复拉起）
    void stopAsync();   // 幂等；杀进程树，不等待工作线程（UI 线程禁阻塞）

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

} // namespace module::memory::engine
