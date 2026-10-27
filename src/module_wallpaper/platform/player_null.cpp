// src/module_wallpaper/platform/player_null.cpp
// Null 播放后端：只记录调用，不创建任何进程/窗口。P3 接入 mpv 前的占位实现。
#include "module_wallpaper/platform/player_backend.h"

#include <utility>

namespace module::wallpaper {

namespace {
void appendCall(std::vector<NullPlayerBackend::Call>* calls, NullPlayerBackend::Call call) {
    calls->push_back(std::move(call));
}
} // namespace

void NullPlayerBackend::start(std::uintptr_t windowId) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "start";
    call.number = static_cast<double>(windowId);
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "load";
    call.path = path;
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::pause(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "pause";
    call.flag = paused;
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::seek(float seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "seek";
    call.number = static_cast<double>(seconds);
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::setRate(float rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "setRate";
    call.number = static_cast<double>(rate);
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::setVolume(float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "setVolume";
    call.number = static_cast<double>(volume);
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::setMute(bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "setMute";
    call.flag = muted;
    appendCall(&calls_, std::move(call));
}

void NullPlayerBackend::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    Call call;
    call.op = "stop";
    appendCall(&calls_, std::move(call));
}

float NullPlayerBackend::positionSec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return position_;
}

std::vector<NullPlayerBackend::Call> NullPlayerBackend::calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

void NullPlayerBackend::clearCalls() {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.clear();
}

void NullPlayerBackend::setPositionForTest(float seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    position_ = seconds > 0.0f ? seconds : 0.0f;
}

} // namespace module::wallpaper