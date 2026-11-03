#include "terminal_session_controller.h"

#include "core/logging.h"

namespace terminal {

void SessionController::setBackend(platform::TerminalBackend* backend) { backend_ = backend; }

void SessionController::startNew() {
    if (backend_ == nullptr) return;
    if (handle_) handle_->terminate();
    handle_.reset();
    state_ = SessionState::Idle;
    error_.clear();
    exitCode_ = 0;

    // shell 传空 = 后端默认解析（mac：决策 6 四段链；win：占位拒绝）
    auto result = backend_->spawn("", {sessionRows_ > 0 ? sessionRows_ : 24,
                                       sessionCols_ > 0 ? sessionCols_ : 80});
    if (result.isFail()) {
        state_ = SessionState::Failed;
        error_ = result.errorMessage();
        LOG_ERROR("Terminal: spawn 失败: " + error_);
        return;
    }
    handle_ = std::move(result.value());
    state_ = SessionState::Running;
    lastGeneration_ = -1;
}

void SessionController::stop() {
    if (handle_) handle_->terminate();
    handle_.reset();
    state_ = SessionState::Idle;
}

void SessionController::poll() {
    if (state_ != SessionState::Running || !handle_) return;
    const int gen = handle_->pendingGeneration();
    if (gen == lastGeneration_) return;
    lastGeneration_ = gen;
    frame_ = handle_->poll();

    int32_t code = 0;
    if (handle_->exited(&code)) {
        exitCode_ = code;
        state_ = SessionState::Exited;
        LOG_INFO("Terminal: 会话退出，exitCode=" + std::to_string(code));
    }
}

void SessionController::writeToSession(const uint8_t* bytes, int32_t count, bool paste) {
    if (state_ != SessionState::Running || !handle_) return;
    auto r = handle_->write(bytes, count, paste);
    if (r.isFail()) LOG_WARN("Terminal: write 失败: " + r.errorMessage());
}

void SessionController::resizeGrid(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    if (rows == sessionRows_ && cols == sessionCols_) return;
    sessionRows_ = rows;
    sessionCols_ = cols;
    if (handle_) {
        auto r = handle_->resize({rows, cols});
        if (r.isFail()) LOG_WARN("Terminal: resize 失败: " + r.errorMessage());
    }
}

int SessionController::scrollbackCount() {
    return handle_ ? handle_->scrollbackCount() : 0;
}

bool SessionController::scrollbackLine(int index, std::string& out) {
    return handle_ ? handle_->scrollbackLine(index, out) : false;
}

bool SessionController::selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                                      std::string& out) {
    return handle_ ? handle_->selectionText(rowStart, colStart, rowEnd, colEnd, out) : false;
}

} // namespace terminal
