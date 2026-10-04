// src/platform/file_watcher_stub.cpp
// POSIX 兜底实现（Windows/macOS 之外的构建，如 Linux）：校验参数后返回 ok，但永不监听、回调永不触发。
// 消费方以 active()==false 判定并退回自身轮询；start/stop/active 语义与错误码同两端，消费方无需分支。
#include "platform/file_watcher.h"

#include "core/logging.h"

#include <system_error>

namespace platform {

struct FileWatcher::Impl {
    // stub 无后台线程、无句柄；仅保留对象寿命
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() = default;

core::Result<void> FileWatcher::start(const std::filesystem::path& dir,
                                      FileWatchCallback onChange,
                                      std::chrono::milliseconds) {
    if (dir.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "FileWatcher: 监听路径为空");
    }
    if (!onChange) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "FileWatcher: 回调为空");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return core::Result<void>::fail(core::ErrorCode::FileNotFound,
                                        "FileWatcher: 目录不存在: " + dir.string());
    }
    LOG_INFO("FileWatcher: 当前平台无原生 watcher 实现，退回轮询兜底: " +
             dir.string());
    return core::Result<void>::ok();  // 成功但 active()==false，消费方自行降级
}

void FileWatcher::stop() {}

bool FileWatcher::active() const { return false; }

} // namespace platform
