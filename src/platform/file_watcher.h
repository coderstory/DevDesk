// src/platform/file_watcher.h
// 文件系统变化事件监听。平台实现：*_win.cpp（ReadDirectoryChangesW + 后台线程）/ *_macos.cpp
//（FSEvents + 专属 CFRunLoop）/ *_stub.cpp（start 返回 ok 但 active() 恒 false，消费方降级轮询）。
// 契约：目录级监听（不按扩展名过滤，消费方自滤）；回调在 watcher 后台线程执行，消费方自负线程安全；
// 回调内 stop() 只请求停止不 join，join 由之后非回调线程的 stop()/析构完成。不抛异常，失败走 Result。

#pragma once

#include "core/result.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// 事件动作（Win 对齐 Win32 FILE_ACTION_*，macOS 对齐 FSEvents Item*；stub 平台不产生事件）
enum class FileWatchAction {
    Created,   // 新增（FILE_ACTION_ADDED / ItemCreated）
    Deleted,   // 删除（FILE_ACTION_REMOVED / ItemRemoved；移入回收站同样报删除）
    Modified,  // 内容写入 / 时间戳变化（FILE_ACTION_MODIFIED / ItemModified 等）
    Renamed,   // 重命名（FILE_ACTION_RENAMED_* / ItemRenamed：新旧名各发一条，以磁盘存在性为准）
    Unknown,   // 缓冲溢出等无法逐项解析的兜底（relativePath 空，消费方应全量重扫；
               // Win 通知缓冲溢出与 macOS 丢事件标志都走这里）
};

struct FileWatchEvent {
    FileWatchAction action = FileWatchAction::Unknown;
    // 相对 watch 根路径，POSIX 分隔符（如 "inbox/a.md"）；空串 = 无法定位具体文件
    std::string relativePath;
};

// 去抖批量回调：在 watch 后台线程上执行，一个静默窗口内的突发合并为一次交付
using FileWatchCallback = std::function<void(std::vector<FileWatchEvent>)>;

class FileWatcher {
public:
    FileWatcher();
    // 析构自动 stop()（join 后台线程）；对象必须活到回调返回——回调内析构属误用
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 启动监听 dir（递归子树），事件在 debounceMs 静默窗口后成批交给 onChange。
    //   dir 空 / 非已存在目录 / 回调为空 → fail(Validation)；原生监听建不起来 → fail(Internal/FileNotFound)；
    //   平台无真实现（POSIX stub）→ 返回 ok 但 active()==false，消费方退回轮询。重复 start 先隐式 stop。
    core::Result<void> start(const std::filesystem::path& dir,
                             FileWatchCallback onChange,
                             std::chrono::milliseconds debounceMs =
                                 std::chrono::milliseconds(200));

    // 停止监听并 join 后台线程；幂等。回调线程内调用 = 只请求停止（置 running=false + SetEvent，
    // 不 join、不关句柄），worker 自行收尾退出；join 由之后非回调线程的 stop()/析构完成。
    void stop();

    // 真监听是否在跑（Win：句柄/线程存活；macOS：FSEvents 流已武装且 worker 存活；stub 恒 false）
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
