// src/module_terminal/web/web_session_bridge.cpp
#include "web_session_bridge.h"

#include <algorithm>
#include <utility>

#include "core/logging.h"

namespace terminal::web {

SizeClampResult clampWebTerminalSize(int rows, int cols) {
    SizeClampResult out;
    out.rows = std::clamp(rows, kWebMinRows, kWebMaxRows);
    out.cols = std::clamp(cols, kWebMinCols, kWebMaxCols);
    out.clamped = out.rows != rows || out.cols != cols;
    return out;
}

WebSessionBridge::WebSessionBridge(SessionController& controller, TerminalSettings* settings,
                                   SaveFn save, RepaintFn repaint)
    : controller_(controller),
      settings_(settings),
      save_(std::move(save)),
      repaint_(std::move(repaint)) {}

std::string WebSessionBridge::stateName_(SessionState state) {
    switch (state) {
        case SessionState::Idle: return "idle";
        case SessionState::Starting: return "starting";
        case SessionState::Running: return "running";
        case SessionState::Exited: return "exited";
        case SessionState::Failed: return "failed";
    }
    return "idle";
}

void WebSessionBridge::emit_(const WebEvent& ev) {
    if (listener_) listener_(ev);
}

std::vector<WebTab> WebSessionBridge::tabs() const {
    std::vector<WebTab> out;
    const int active = controller_.activeIndex();
    for (int i = 0; i < controller_.count(); i++) {
        const Session* s = controller_.at(i);
        if (s == nullptr) continue;
        WebTab t;
        t.index = i;
        // 标题：OSC 0/2 标题由 W3 服务补充；此层先给稳定可辨识的「终端 N」。
        t.title = "终端 " + std::to_string(s->titleNumber);
        t.active = i == active;
        t.state = stateName_(s->state);
        t.exitCode = s->exitCode;
        out.push_back(std::move(t));
    }
    return out;
}

int WebSessionBridge::createTab() {
    const int index = controller_.startSession();
    WebEvent ev;
    ev.type = WebEventType::Tabs;
    ev.tabIndex = index;
    emit_(ev);
    return index;
}

bool WebSessionBridge::closeTab(int index) {
    if (index < 0 || index >= controller_.count()) return false;
    controller_.closeSession(index);
    WebEvent ev;
    ev.type = WebEventType::Tabs;
    ev.tabIndex = controller_.activeIndex();
    emit_(ev);
    return true;
}

bool WebSessionBridge::activateTab(int index) {
    if (index < 0 || index >= controller_.count()) return false;
    controller_.setActive(index);
    WebEvent ev;
    ev.type = WebEventType::Tabs;
    ev.tabIndex = index;
    emit_(ev);
    return true;
}

bool WebSessionBridge::restartTab(int index) {
    if (index < 0 || index >= controller_.count()) return false;
    controller_.restartSession(index);
    const Session* s = controller_.at(index);
    WebEvent ev;
    ev.type = WebEventType::Tabs;
    ev.tabIndex = index;
    ev.reason = s != nullptr ? stateName_(s->state) : std::string();
    emit_(ev);
    return true;
}

void WebSessionBridge::pollStates() {
    std::map<int, SessionState> seen;
    for (int i = 0; i < controller_.count(); i++) {
        const Session* s = controller_.at(i);
        if (s == nullptr) continue;
        seen[s->titleNumber] = s->state;
        auto prev = lastState_.find(s->titleNumber);
        const bool changed = prev == lastState_.end() || prev->second != s->state;
        if (!changed) continue;
        if (s->state == SessionState::Exited) {
            WebEvent ev;
            ev.type = WebEventType::Exit;
            ev.tabIndex = i;
            ev.exitCode = s->exitCode;
            lastExitCode_[s->titleNumber] = s->exitCode;
            emit_(ev);
        } else if (s->state == SessionState::Failed) {
            WebEvent ev;
            ev.type = WebEventType::Failed;
            ev.tabIndex = i;
            ev.reason = s->error;
            emit_(ev);
        }
    }
    // 已关闭会话的状态记录清理（titleNumber 不复用，不会误判复活）。
    for (auto it = lastState_.begin(); it != lastState_.end();) {
        if (seen.find(it->first) == seen.end()) {
            lastExitCode_.erase(it->first);
            it = lastState_.erase(it);
        } else {
            ++it;
        }
    }
    lastState_ = std::move(seen);
}

void WebSessionBridge::markDesktopRestart() {
    WebEvent ev;
    ev.type = WebEventType::DesktopRestart;
    ev.reason = "desktop_restart";
    emit_(ev);
}

bool WebSessionBridge::webResize(int rows, int cols, int* appliedRows, int* appliedCols) {
    const SizeClampResult clamp = clampWebTerminalSize(rows, cols);
    if (!webSizeApplied_) {
        if (desktopSizeValid_) {
            webSizeApplied_ = true;
        } else {
            const Session* active = controller_.active();
            if (active != nullptr) {
                desktopRows_ = active->rows;
                desktopCols_ = active->cols;
                desktopSizeValid_ = desktopRows_ > 0 && desktopCols_ > 0;
                webSizeApplied_ = true;
            }
        }
    }
    const Session* active = controller_.active();
    if (active == nullptr) return false;
    controller_.resizeActive(clamp.rows, clamp.cols);
    if (appliedRows != nullptr) *appliedRows = clamp.rows;
    if (appliedCols != nullptr) *appliedCols = clamp.cols;
    return true;
}

void WebSessionBridge::observeDesktopResize(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    desktopRows_ = rows;
    desktopCols_ = cols;
    desktopSizeValid_ = true;
    // 桌面也改尺寸 = 最后写入者，网页退出时恢复到该尺寸。
    webSizeApplied_ = false;
}

void WebSessionBridge::addSubscriber() { subscribers_++; }

void WebSessionBridge::removeSubscriber() {
    if (subscribers_ > 0) subscribers_--;
    if (subscribers_ != 0) return;
    if (!webSizeApplied_ || !desktopSizeValid_) return;
    controller_.resizeActive(desktopRows_, desktopCols_);
    webSizeApplied_ = false;
}

core::Result<void> WebSessionBridge::writeSettings(const TerminalSettings& nextRaw) {
    if (settings_ == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "终端设置真源未接线");
    }
    if (!workDirUsableForSpawn(nextRaw.startupDir)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "启动目录不存在、不是目录或不可进入: " +
                                            nextRaw.startupDir);
    }
    const TerminalSettings previous = *settings_;
    const TerminalSettings sanitized = sanitizedTerminalSettings(nextRaw);
    *settings_ = sanitized;  // 单一真源：内存先回灌，桌面/网页控件下一帧读同一值
    if (save_) {
        auto saved = save_(sanitized);
        if (saved.isFail()) {
            *settings_ = previous;  // 持久化失败回滚（HTTP 层回 500）
            return saved;
        }
    }
    controller_.setNewSessionOptions(sanitized.shell, sanitized.startupDir, kScrollbackDefault);
    WebEvent ev;
    ev.type = WebEventType::Settings;
    emit_(ev);
    if (repaint_) repaint_();
    return core::Result<void>::ok();
}

} // namespace terminal::web
