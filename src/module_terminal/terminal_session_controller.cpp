// src/module_terminal/terminal_session_controller.cpp
#include "terminal_session_controller.h"

#include <algorithm>
#include <utility>

#include "core/logging.h"
#include "terminal_settings.h"

namespace terminal {

void SessionController::setBackend(platform::TerminalBackend* backend) { backend_ = backend; }

void SessionController::setOutputWake(std::function<void()> wake) {
    outputWake_ = std::move(wake);
    for (Session& s : sessions_) {
        if (s.handle) s.handle->setOutputNotify(outputWake_);
    }
}

bool SessionController::outputNotifyAvailable() const {
    for (const Session& s : sessions_) {
        if (s.handle && s.handle->supportsOutputNotify()) return true;
    }
    return false;
}

core::Result<std::unique_ptr<platform::TerminalSessionHandle>> SessionController::spawnHandle_(
    const std::string& shell, const std::string& workDirIn, int rows, int cols) {
    using HandleResult = core::Result<std::unique_ptr<platform::TerminalSessionHandle>>;
    // 可用性前置检查（need:127 技术卡 v2 决策 6）：OS<1809 无 ConPTY → 失败文案进
    // Failed 覆盖卡（复用既有 UI 形态）
    if (backend_ == nullptr) {
        return HandleResult::fail(core::ErrorCode::Internal, "终端后端未初始化");
    }
    if (!backend_->available()) {
        return HandleResult::fail(core::ErrorCode::NotImplemented,
                                  backend_->unavailableReason() != nullptr
                                      ? backend_->unavailableReason()
                                      : "终端不可用");
    }
    // shell/workDir 空 = 后端默认（mac：决策 6 四段链；win：pwsh→powershell→COMSPEC）；
    // 非空 = 设置面板/网页选值。PRD P-14：非空不可用目录 → 启动失败（不再静默回退
    // 主目录，用户须能看见「启动目录无效」而不是悄悄跑在别处）。
    const std::string& workDir = workDirIn;
    if (!workDir.empty() && !workDirUsableForSpawn(workDir)) {
        return HandleResult::fail(core::ErrorCode::ValidationError,
                                  "启动目录不存在、不是目录或不可进入: " + workDir);
    }
    return backend_->spawn(shell, {rows > 0 ? rows : 24, cols > 0 ? cols : 80}, workDir);
}

void SessionController::installHandle_(Session& s,
                                       std::unique_ptr<platform::TerminalSessionHandle> handle,
                                       int scrollback) {
    s.handle = std::move(handle);
    s.handle->setScrollbackLimit(scrollback); // need250：新会话按设置分配回滚
    // bug249：输出/退出唤醒接线（reader 线程 → requestUiUpdate，非 UI 线程安全路径）
    if (outputWake_) s.handle->setOutputNotify(outputWake_);
    s.state = SessionState::Running;
    s.lastGeneration = -1;
    s.error.clear();
    s.exitCode = 0;
    LOG_INFO("Terminal: 新会话已启动（终端 " + std::to_string(s.titleNumber) + "，" +
             std::to_string(s.rows > 0 ? s.rows : 24) + "x" +
             std::to_string(s.cols > 0 ? s.cols : 80) + "）");
}

int SessionController::spawnInto_(Session& s) {
    // 同步路径（restartSession / 新建 TAB）；进页走 startSessionAsync 不占 UI 帧
    auto result = spawnHandle_(newSessionShell_, newSessionWorkDir_, s.rows, s.cols);
    if (result.isFail()) {
        s.state = SessionState::Failed;
        s.error = result.errorMessage();
        LOG_ERROR("Terminal: spawn 失败: " + s.error);
        return -1;
    }
    installHandle_(s, std::move(result.value()), newSessionScrollback_);
    return 0;
}

int SessionController::startSession() {
    Session s;
    s.titleNumber = nextTitleNumber_++;
    s.rows = count() > 0 ? sessions_.back().rows : 0;
    s.cols = count() > 0 ? sessions_.back().cols : 0;
    spawnInto_(s);
    sessions_.push_back(std::move(s));
    activeIndex_ = static_cast<int>(sessions_.size()) - 1;
    return activeIndex_;
}

SessionController::~SessionController() {
    {
        std::lock_guard<std::mutex> lock(spawnMu_);
        spawnStop_ = true;
        spawnQueue_.clear(); // 未执行的建会话请求丢弃（不 spawn 子进程去陪葬）
    }
    spawnCv_.notify_all();
    if (spawnThread_.joinable()) spawnThread_.join();
    spawnOutcomes_.clear(); // 已 spawn 未接入的句柄：析构即 terminate
}

int SessionController::startSessionAsync() { return startSessionAsyncIn(std::string()); }

int SessionController::startSessionAsyncIn(const std::string& workDir) {
    Session s;
    s.titleNumber = nextTitleNumber_++;
    s.rows = count() > 0 ? sessions_.back().rows : 0;
    s.cols = count() > 0 ? sessions_.back().cols : 0;
    s.state = SessionState::Starting;
    SpawnRequest req;
    req.titleNumber = s.titleNumber;
    req.shell = newSessionShell_;
    req.workDir = workDir.empty() ? newSessionWorkDir_ : workDir;
    req.rows = s.rows;
    req.cols = s.cols;
    // 先占位再派活：empty() 对 Starting 成立，ensureBackend 幂等保持
    sessions_.push_back(std::move(s));
    activeIndex_ = static_cast<int>(sessions_.size()) - 1;
    postSpawn_(std::move(req));
    return activeIndex_;
}

void SessionController::postSpawn_(SpawnRequest req) {
    {
        std::lock_guard<std::mutex> lock(spawnMu_);
        spawnQueue_.push_back(std::move(req));
        if (!spawnThread_.joinable()) {
            spawnThread_ = std::thread([this] { runSpawnLoop_(); });
        }
    }
    spawnCv_.notify_one();
}

void SessionController::runSpawnLoop_() {
    for (;;) {
        SpawnRequest req;
        {
            std::unique_lock<std::mutex> lock(spawnMu_);
            spawnCv_.wait(lock, [this] { return spawnStop_ || !spawnQueue_.empty(); });
            if (spawnStop_) return;
            req = std::move(spawnQueue_.front());
            spawnQueue_.erase(spawnQueue_.begin());
        }
        SpawnOutcome out;
        out.titleNumber = req.titleNumber;
        auto result = spawnHandle_(req.shell, req.workDir, req.rows, req.cols);
        if (result.isFail()) {
            out.error = result.errorMessage();
        } else {
            out.handle = std::move(result.value());
        }
        {
            std::lock_guard<std::mutex> lock(spawnMu_);
            spawnOutcomes_.push_back(std::move(out));
        }
        // 非 UI 线程 → 请求一帧（bug249 同通道），UI 线程 pollAll 才能收到就绪/失败
        if (outputWake_) outputWake_();
    }
}

void SessionController::drainSpawnOutcomes_() {
    std::vector<SpawnOutcome> ready;
    {
        std::lock_guard<std::mutex> lock(spawnMu_);
        ready.swap(spawnOutcomes_);
    }
    for (SpawnOutcome& out : ready) {
        Session* s = nullptr;
        for (Session& cand : sessions_) {
            if (cand.titleNumber == out.titleNumber) {
                s = &cand;
                break;
            }
        }
        // 建会话期间已关闭：结果直接丢弃（out.handle 析构 → terminate，不留孤儿进程）
        if (s == nullptr) continue;
        if (!out.handle) {
            s->state = SessionState::Failed;
            s->error = out.error.empty() ? "终端启动失败" : out.error;
            LOG_ERROR("Terminal: 后台建会话失败: " + s->error);
            continue;
        }
        installHandle_(*s, std::move(out.handle), newSessionScrollback_);
        // 骨架期页面已把视口行列写进 Session（无句柄只赋值），此处补一次对齐，
        // 避免句柄停在 spawn 时的默认 24x80
        if (s->rows > 0 && s->cols > 0) {
            auto r = s->handle->resize({s->rows, s->cols});
            if (r.isFail()) LOG_WARN("Terminal: 首次 resize 失败: " + r.errorMessage());
        }
    }
}

void SessionController::closeSession(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    const int oldActive = activeIndex_;
    Session& s = sessions_[index];
    if (s.handle) s.handle->terminate();
    s.handle.reset();
    sessions_.erase(sessions_.begin() + index);
    if (sessions_.empty()) {
        activeIndex_ = -1;
    } else if (index < oldActive) {
        // 关非激活左邻（P2-2）：焦点会话不变，仅索引随删左移一位
        activeIndex_ = oldActive - 1;
    } else if (index == oldActive) {
        // 关激活：右邻优先激活（关闭位之后左移一位 → min(index, newCount-1)）
        activeIndex_ = std::min(index, static_cast<int>(sessions_.size()) - 1);
    }
    // index > oldActive：关非激活右邻，焦点不动
}

void SessionController::setActive(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    activeIndex_ = index;
}

void SessionController::setSessionTitle(int index, const std::string& title) {
    Session* s = at(index);
    if (s == nullptr || s->title == title) return;
    s->title = title;
    ++titleRevision_;
}

void SessionController::moveSession(int from, int to) {
    const int n = static_cast<int>(sessions_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    Session moved = std::move(sessions_[static_cast<size_t>(from)]);
    sessions_.erase(sessions_.begin() + from);
    sessions_.insert(sessions_.begin() + to, std::move(moved));
    if (activeIndex_ == from) {
        activeIndex_ = to;
    } else if (from < activeIndex_ && activeIndex_ <= to) {
        --activeIndex_;
    } else if (to <= activeIndex_ && activeIndex_ < from) {
        ++activeIndex_;
    }
    ++titleRevision_; // 顺序变化 → 页面标题缓存重建
}

void SessionController::restartSession(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    Session& s = sessions_[index];
    if (s.handle) s.handle->terminate();
    s.handle.reset();
    s.frame = platform::TerminalFrame{};
    s.scrollOffset = 0;      // P3-1：重启=全新输出流，回滚偏移归零
    s.scrollRemainder = 0.0f;
    spawnInto_(s);
}

// need250：shell/目录存成员（仅新会话消费）；回滚即时改既有会话上限（裁最旧）。
// handle 默认空实现（mac 桥）安全无害。
void SessionController::setNewSessionOptions(const std::string& shell,
                                             const std::string& workDir,
                                             int scrollbackLimit) {
    newSessionShell_ = shell;
    newSessionWorkDir_ = workDir;
    newSessionScrollback_ = scrollbackLimit > 0 ? scrollbackLimit : 1000;
    for (Session& s : sessions_) {
        if (s.handle) s.handle->setScrollbackLimit(newSessionScrollback_);
    }
}

Session* SessionController::active() { return at(activeIndex_); }
const Session* SessionController::active() const { return at(activeIndex_); }
Session* SessionController::at(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return nullptr;
    return &sessions_[index];
}
const Session* SessionController::at(int index) const {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return nullptr;
    return &sessions_[index];
}

void SessionController::pollAll() {
    drainSpawnOutcomes_(); // bug406：后台建会话结果先接入，本帧即可渲染新会话
    for (Session& s : sessions_) {
        if (s.state != SessionState::Running || !s.handle) continue;
        const int gen = s.handle->pendingGeneration();
        if (gen == s.lastGeneration) {
            // 已退出但帧未收割的面（generate 不再变的退出）也要补收割
            int32_t code = 0;
            if (s.handle->exited(&code)) {
                s.exitCode = code;
                s.state = SessionState::Exited;
                LOG_INFO("Terminal: 终端 " + std::to_string(s.titleNumber) +
                         " 退出，exitCode=" + std::to_string(code));
            }
            continue;
        }
        s.lastGeneration = gen;
        s.frame = s.handle->poll();
        int32_t code = 0;
        if (s.handle->exited(&code)) {
            s.exitCode = code;
            s.state = SessionState::Exited;
            LOG_INFO("Terminal: 终端 " + std::to_string(s.titleNumber) +
                     " 退出，exitCode=" + std::to_string(code));
        }
    }
}

void SessionController::writeToActive(const uint8_t* bytes, int32_t count, bool paste) {
    Session* s = active();
    if (s == nullptr || s->state != SessionState::Running || !s->handle) return;
    auto r = s->handle->write(bytes, count, paste);
    if (r.isFail()) LOG_WARN("Terminal: write 失败: " + r.errorMessage());
}

void SessionController::resizeActive(int rows, int cols) {
    Session* s = active();
    if (s == nullptr || rows <= 0 || cols <= 0) return;
    if (rows == s->rows && cols == s->cols) return;
    s->rows = rows;
    s->cols = cols;
    if (s->handle) {
        auto r = s->handle->resize({rows, cols});
        if (r.isFail()) LOG_WARN("Terminal: resize 失败: " + r.errorMessage());
    }
}

int SessionController::activeScrollbackCount() {
    Session* s = active();
    return s && s->handle ? s->handle->scrollbackCount() : 0;
}

bool SessionController::activeScrollbackLine(int index, std::string& out) {
    Session* s = active();
    return s && s->handle ? s->handle->scrollbackLine(index, out) : false;
}

bool SessionController::activeSelectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                                            std::string& out) {
    Session* s = active();
    return s && s->handle ? s->handle->selectionText(rowStart, colStart, rowEnd, colEnd, out)
                          : false;
}

} // namespace terminal
