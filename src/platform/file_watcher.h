// src/platform/file_watcher.h
// platform::FileWatcher —— 文件系统变化事件监听（接口隔离：*_win.cpp 真实现 / *_stub.cpp 兜底）
//
// 诞生背景（KNOWN_LIMITATIONS #5 根治）：markdown 的外部改动检测原基于
// mtime（unix 秒）+ size 轮询，同秒改回等长内容存在永久漏检窗口。
// 本类把「变更」变成推事件：消费方收到事件即刻按事件同步，不再依赖
// 秒级粒度比对。
//
// 平台实现：
//   - Windows：ReadDirectoryChangesW + OVERLAPPED + 自带后台线程，
//     递归监听整个子树（FILE_NOTIFY_CHANGE_FILE_NAME / DIR_NAME /
//     LAST_WRITE / SIZE）。事件按去抖窗口合并成批交付。
//   - POSIX / macOS：stub —— start 校验目录后返回 ok，但 active() 恒为
//     false、回调永不触发。消费方以 active()==false 判定降级为自身
//     mtime/size 轮询兜底（即维持接入 watcher 前的行为）。
//
// 线程约定：回调在 watcher 后台线程执行——消费方须自行保证线程安全
// （markdown_service 依赖 SqliteDb recursive_mutex + rescanMu_ 串行化）。
// 不抛异常（devdesk 约定），启动失败一律 Result 返回。
//
// 已知边界（Win）：
//   - 事件到达即同步，编辑器可能仍在写尾部字节 → 单次可能同步到半截内容；
//     写完后 mtime 推进会被下一次事件或消费方轮询修正（200ms 去抖已把
//     窗口压到编辑器批量 flush 级别）。
//   - 读请求完成与重发之间的微秒级间隙事件可能丢失，由消费方轮询兜底。
#pragma once

#include "core/result.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// 事件动作（与 Win32 FILE_ACTION_* 对齐；stub 平台不产生任何事件）
enum class FileWatchAction {
    Created,   // 新增文件（FILE_ACTION_ADDED）
    Deleted,   // 删除（FILE_ACTION_REMOVED；移入回收站同样报删除）
    Modified,  // 内容写入 / 时间戳变化（FILE_ACTION_MODIFIED）
    Renamed,   // 重命名（FILE_ACTION_RENAMED_*：新旧名各发一条，以磁盘存在性为准）
    Unknown,   // 缓冲溢出等无法逐项解析的兜底事件（relativePath 为空，消费方应全量重扫）
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
    ~FileWatcher();  // 自动 stop()：join 后台线程，回调保证不再触碰消费方

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 启动监听 dir（递归子树）。事件在 debounceMs 静默窗口后成批交给 onChange。
    //   - dir 为空 / 不是已存在的目录 / 回调为空 → fail（ValidationError / FileNotFound）
    //   - 平台无真实现（POSIX/macOS stub）→ 返回 ok 但 active()==false，
    //     消费方据此退回轮询兜底
    // 重复 start 先隐式 stop（支持换目录重启）。
    core::Result<void> start(const std::filesystem::path& dir,
                             FileWatchCallback onChange,
                             std::chrono::milliseconds debounceMs =
                                 std::chrono::milliseconds(200));

    // 停止监听并 join 后台线程；幂等；回调线程内调用安全（跳过自 join）。
    void stop();

    // 真监听是否在跑（Win 且目录句柄/线程存活；stub 平台恒 false）
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
