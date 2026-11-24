// src/module_terminal/terminal_session_controller.h
// 多会话管理（need241 v2 设计 §4.1/§6）：每 TAB 一会话（独立 backend handle + vterm
// 帧 + scrollback 偏移记忆 + 三态状态机）。activeIndex 承载页面渲染焦点；关闭 =
// 右邻优先激活（无右邻则左邻）；会话序号进程内递增不复用。
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/terminal/terminal_backend.h"

namespace terminal {

enum class SessionState {
    Idle,     // 尚未启动（首个会话页面进入时自动 start）
    Starting, // 后台建会话中（bug406：ConPTY 创建已搬 worker 线程，页面先出骨架）
    Running,
    Exited,   // 子进程已退出，exitCode 有效；最后一帧内容保留展示
    Failed,    // spawn 失败，errorMessage 可达，可重试
};

// 出帧策略（bug249/bug305，纯函数供页面组帧与单测断言）：任一会话 Running 时必须
// 有主动唤醒某条路径——平台提供输出通知（reader 新输出/退出经 requestUiUpdate 唤醒）
// 时按需出帧，空闲零帧；没有通知路径的平台保留常驻 onFrame 泵兜底。
inline bool needsFramePump(bool anyRunning, bool outputNotifyAvailable) {
    return anyRunning && !outputNotifyAvailable;
}

// 低频自唤醒周期（bug306）：有输出通知路径时，Running 期间靠这一路 550ms 一帧推进
// 光标闪烁相位，并顺带巡检子进程退出（bug305：ConPTY 退出不断管道，只能靠进程句柄
// 巡检发现）。550ms ≈ 1.8fps（≤2fps 硬上限），与 1100ms 闪烁周期正好两拍一翻转。
// **不得回到 60fps 常驻出帧**（bug249 已验收口径）。
inline constexpr int kIdleFrameTickMs = 550;

// 是否需要挂低频自唤醒（纯函数）：有通知路径时 Running 才挂（退出后/空闲归零）；
// 无通知路径的平台已由 needsFramePump 常驻泵负责，不重复挂。
inline bool needsIdleTick(bool anyRunning, bool outputNotifyAvailable) {
    return anyRunning && outputNotifyAvailable;
}

// Starting 也要有帧（bug406）：后台建会话结果只在帧内 pollAll 收割，没帧会话就永卡
// Starting、失败态也看不见。但它只进**低频自唤醒档**（结果到达靠 worker 唤醒，低频
// 帧是兜底）——不能进常驻泵档，否则回到 bug249 的 60fps 空转。
inline bool needsStartingFrameTick(bool anyStarting, bool anyRunning,
                                  bool outputNotifyAvailable) {
    return anyStarting || needsIdleTick(anyRunning, outputNotifyAvailable);
}

// 单会话全部运行态（页面/网格只经 controller 间接读）
struct Session {
    std::unique_ptr<platform::TerminalSessionHandle> handle;
    SessionState state = SessionState::Idle;
    int exitCode = 0;
    std::string error;
    platform::TerminalFrame frame;
    int lastGeneration = -1;
    int rows = 0;
    int cols = 0;
    int scrollOffset = 0;      // scrollback 回滚偏移（每会话记忆，0=贴底）
    float scrollRemainder = 0.0f;
    int titleNumber = 0;       // 「终端 N」，进程内递增不复用
    std::string title;         // 网页改名后的展示名；空 = 用「终端 N」
};

class SessionController {
public:
    SessionController() = default;
    // join 后台建会话 worker；未收割结果里的句柄析构即终止子进程（不留孤儿）
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    void setBackend(platform::TerminalBackend* backend);

    // 输出/退出唤醒出口（bug249）：页面装 core::platform::requestUiUpdate —— reader
    // 线程产出新帧或子进程退出时调一次，只出这一帧。装后对既有会话立即生效。
    void setOutputWake(std::function<void()> wake);
    // 当前会话后端是否具备输出通知路径（页面据此决定是否需要常驻泵兜底）
    bool outputNotifyAvailable() const;

    // 新建会话追加到序列尾并激活，返回新会话 index。backend 不可用 / spawn 失败
    // 不抛不崩：会话以 Failed 态占位（TAB 圆点红 + 覆盖卡错误详情）。
    int startSession();
    // bug406：后台建会话——立即返回（会话以 Starting 占位），spawn 在 worker 线程跑，
    // 结果由 pollAll 收割（成功转 Running，失败转 Failed 带 error）。进页帧不被
    // ConPTY 创建阻塞（实测 14.4ms）。
    int startSessionAsync();
    // 关闭第 i 个会话（终止进程，无确认；语义=终止）。激活右邻优先，无右邻则左邻；
    // 关最后一个 → activeIndex=-1（空态）。
    void closeSession(int index);
    void setActive(int index);
    // 网页 tab 改名（P1-4）：写入会话展示名，桌面 tab 与网页显示同一名字。
    void setSessionTitle(int index, const std::string& title);
    // 标题代数：终端页缓存据此判断是否需要重建（改名不改变会话数）。
    int titleRevision() const { return titleRevision_; }
    int activeIndex() const { return activeIndex_; }
    int count() const { return static_cast<int>(sessions_.size()); }
    bool empty() const { return sessions_.empty(); }

    Session* active();
    const Session* active() const;
    Session* at(int index);
    const Session* at(int index) const;

    // 每帧调用：全会话拉新快照（generation 变化才拷贝）+ 退出收割——后台 TAB
    // 的状态点/覆盖卡也要随会话态变化。
    void pollAll();

    // 激活会话的输入/resize/选区路由（无激活会话时静默丢弃）。
    // 滚动偏移不在此路由：折算需要 cell 真高（page 持 metrics），直接写 Session.scrollOffset。
    void writeToActive(const uint8_t* bytes, int32_t count, bool paste = false);
    void resizeActive(int rows, int cols);
    int activeScrollbackCount();
    bool activeScrollbackLine(int index, std::string& out);
    bool activeSelectionText(int rowStart, int colStart, int rowEnd, int colEnd, std::string& out);

    // 重试/重新开始：把第 i 个会话回到 Running（Exited/Failed 复位重建）
    void restartSession(int index);

    // need250 设置面板：新会话参数（shell/目录仅新会话生效）；回滚上限即时作用于
    // 全部既有会话（超限裁最旧）。shell/workDir 空 = 平台默认/主目录语义。
    void setNewSessionOptions(const std::string& shell, const std::string& workDir,
                              int scrollbackLimit);

private:
    struct SpawnRequest {
        int titleNumber = 0;
        std::string shell;
        std::string workDir;
        int rows = 0;
        int cols = 0;
    };
    struct SpawnOutcome {
        int titleNumber = 0;
        std::unique_ptr<platform::TerminalSessionHandle> handle;
        std::string error;
    };

    int spawnInto_(Session& s);
    // 慢段（可用性检查 + spawn）：不碰 Session，UI 线程与建会话 worker 都可调
    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawnHandle_(
        const std::string& shell, const std::string& workDir, int rows, int cols);
    // 快段：已 spawn 的句柄接入会话（唤醒/回滚上限/状态迁移）。仅 UI 线程调
    void installHandle_(Session& s, std::unique_ptr<platform::TerminalSessionHandle> handle,
                        int scrollback);
    void postSpawn_(SpawnRequest req);
    void drainSpawnOutcomes_();  // UI 线程（pollAll 首步）
    void runSpawnLoop_();        // worker 线程

    platform::TerminalBackend* backend_ = nullptr;
    std::vector<Session> sessions_;
    int activeIndex_ = -1;
    int nextTitleNumber_ = 1;
    int titleRevision_ = 0;  // 改名代数（终端页标题缓存失效信号）
    // 新会话参数（need250 面板写、spawnInto_ 消费）
    std::string newSessionShell_;
    std::string newSessionWorkDir_;
    int newSessionScrollback_ = 1000;
    std::function<void()> outputWake_;  // 空=未装（无唤醒出口，页面走常驻泵兜底）

    // bug406 后台建会话：单 worker 串行队列；结果经 spawnMu_ 与 UI 线程交接（句柄与
    // 错误只在 UI 线程接入 Session，worker 不改 sessions_）
    std::thread spawnThread_;
    std::mutex spawnMu_;
    std::condition_variable spawnCv_;
    std::vector<SpawnRequest> spawnQueue_;
    std::vector<SpawnOutcome> spawnOutcomes_;
    bool spawnStop_ = false;
};

} // namespace terminal
