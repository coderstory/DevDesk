// src/module_terminal/terminal_session_controller.cpp
#include "terminal_session_controller.h"

#include <algorithm>
#include <utility>

#include "core/logging.h"
#include "terminal_settings.h"

namespace terminal {

void SessionController::setBackend(platform::TerminalBackend* backend) { backend_ = backend; }

int SessionController::spawnInto_(Session& s) {
    // 可用性前置检查（need:127 技术卡 v2 决策 6）：OS<1809 无 ConPTY → Failed 态 +
    // unavailableReason 明确文案（复用失败覆盖卡，零新增 UI 形态）
    if (backend_ == nullptr) {
        s.state = SessionState::Failed;
        s.error = "终端后端未初始化";
        return -1;
    }
    if (!backend_->available()) {
        s.state = SessionState::Failed;
        s.error = backend_->unavailableReason() != nullptr ? backend_->unavailableReason()
                                                           : "终端不可用";
        LOG_ERROR("Terminal: 后端不可用: " + s.error);
        return -1;
    }
    // shell/workDir 空 = 后端默认（mac：决策 6 四段链；win：pwsh→powershell→COMSPEC）；
    // 非空 = 设置面板选值（need250）。目录无效静默回退主目录（设计 §4.2⑤ 容错）
    std::string workDir = newSessionWorkDir_;
    if (!workDir.empty() && !workDirUsableForSpawn(workDir)) {
        LOG_WARN("Terminal: 启动目录不存在，回退用户主目录: " + workDir);
        workDir.clear();
    }
    auto result = backend_->spawn(newSessionShell_,
                                  {s.rows > 0 ? s.rows : 24, s.cols > 0 ? s.cols : 80},
                                  workDir);
    if (result.isFail()) {
        s.state = SessionState::Failed;
        s.error = result.errorMessage();
        LOG_ERROR("Terminal: spawn 失败: " + s.error);
        return -1;
    }
    s.handle = std::move(result.value());
    s.handle->setScrollbackLimit(newSessionScrollback_); // need250：新会话按设置分配回滚
    s.state = SessionState::Running;
    s.lastGeneration = -1;
    s.error.clear();
    s.exitCode = 0;
    LOG_INFO("Terminal: 新会话已启动（终端 " + std::to_string(s.titleNumber) + "，" +
             std::to_string(s.rows > 0 ? s.rows : 24) + "x" +
             std::to_string(s.cols > 0 ? s.cols : 80) + "）");
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
