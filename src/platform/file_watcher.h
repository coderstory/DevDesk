// src/platform/file_watcher.h
// platform::FileWatcher —— 文件系统变化事件监听（接口隔离：*_win.cpp / *_macos.cpp
// 真实现，POSIX 之外无实现时 *_stub.cpp 兜底）
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
//   - macOS：FSEvents（FSEventStreamCreate + 专属后台线程的 CFRunLoop），
//     kFSEventStreamCreateFlagFileEvents 拿逐文件事件（而非目录级聚合），
//     天然递归整棵子树；事件同样按去抖窗口合并成批交付。位掩码另含
//     NoDefer（空闲后首事件立即投递）、WatchRoot（根被改名/删除时报
//     RootChanged → Unknown）。与 win 版语义一致（动作映射、错误码、
//     回调线程、回调内 stop 只请求停止）。**这里没有 win 版的启动武装
//     窗口**：start() 阻塞到 FSEventStreamStart 成功才返回，start() 返回
//     即监听已生效。
//   - 其余 POSIX：stub —— start 校验目录后返回 ok，但 active() 恒为
//     false、回调永不触发。消费方以 active()==false 判定降级为自身
//     mtime/size 轮询兜底（即维持接入 watcher 前的行为）。
//
// 线程约定：回调在 watcher 后台线程执行——消费方须自行保证线程安全
// （markdown_service 依赖 SqliteDb recursive_mutex + rescanMu_ 串行化）。
// 不抛异常（devdesk 约定），启动失败一律 Result 返回。
//
// 消费方契约（扩展名无关）：监听是目录级的——监听根下**一切**条目的变化都会
// 上报，不区分扩展名/文件类型（.md 笔记、数据库文件、编辑器临时文件、
// .DS_Store / Thumbs.db 一视同仁），消费方必须按自己的相关性过滤事件
// （markdown_service 只认 .md 路径 / Unknown / 目录或已不存在的路径，见
//   onWatchEvent 的 credit 过滤——完整口径以该函数注释为准）。
// 由此推论：**不要把高频落盘的伴生文件（数据库/状态文件）放进被监听目录**——
// 原生平台（Win/macOS）上它们每次写盘都会成为事件源；stub 平台无事件，
// 行为差异本身就是平台不对称（macOS 曾长期被误认为 stub，实为 FSEvents 真实现，
// 见 git 600081a——判断平台行为以本文件「平台实现」段与 CMake 平台分派为准，
// 不要信消费方代码里的转述注释）。消费方过滤的完整口径以 markdown_service
// 的 onWatchEvent 为准（.md 路径 / Unknown / 目录或已不存在的路径置 credit，
// 磁盘上存在的非 .md 普通文件不置）。
//
// 已知边界（Win）：
//   - 启动武装窗口（arming window）：start() 在**调用线程**打开目录句柄，而
//     首个 ReadDirectoryChangesW 由 worker 线程**异步**发出（线程创建 + 循环
//     首轮），两端之间存在长度取决于线程调度的窗口。Windows 只上报「首次读
//     请求发出之后」发生的改动——此窗口内发生的改动**不会被上报**，事后等待
//     也不会补发（与下条微秒空窗同源，属平台语义、非本实现缺陷）。
//     消费方指引：需要「零丢失」的调用方不应把 start() 返回当作监听已生效，
//     应在 start() 之后（例如等到确实收到过一次事件）才认为进入有效状态，
//     或依赖上层周期性兜底——本仓 markdown_service 有 5s tick 的 pollChanged
//     → rescan 兜底，属可接受依赖。（macOS 实现无此窗口，见「平台实现」。）
//   - 事件到达即同步，编辑器可能仍在写尾部字节 → 单次可能同步到半截内容；
//     写完后 mtime 推进会被下一次事件或消费方轮询修正（200ms 去抖已把
//     窗口压到编辑器批量 flush 级别）。
//   - 读请求完成与重发之间的微秒级间隙事件可能丢失，由消费方轮询兜底。
//   - 监听存活期间 watcher 持目录句柄；在回调线程里读被监听文件的消费方
//     （如 markdown_service 的 rescan）会瞬时持文件读句柄，而 MinGW 的
//     ifstream 句柄不带 FILE_SHARE_DELETE → 这段时间里外部删除该文件会失败
//     （fs::remove 返 false / remove_all 抛 filesystem_error）。清场方应先
//     stop()（或析构）并等回调返回再删，不要假设「删了就行」。（macOS 无
//     FILE_SHARE_DELETE 语义，删除不会被读句柄挡住；清场方仍应先 stop()。）
#pragma once

#include "core/result.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// 事件动作（Win 对齐 Win32 FILE_ACTION_*，macOS 对齐 FSEvents Item* 标志；
// stub 平台不产生任何事件）
enum class FileWatchAction {
    Created,   // 新增文件（FILE_ACTION_ADDED / ItemCreated）
    Deleted,   // 删除（FILE_ACTION_REMOVED / ItemRemoved；移入回收站同样报删除）
    Modified,  // 内容写入 / 时间戳变化（FILE_ACTION_MODIFIED / ItemModified 等）
    Renamed,   // 重命名（FILE_ACTION_RENAMED_* / ItemRenamed：新旧名各发一条，
               // 以磁盘存在性为准）
    Unknown,   // 缓冲溢出等无法逐项解析的兜底事件（relativePath 为空，消费方应全量重扫）
               // ——Win 的通知缓冲溢出、macOS 的 MustScanSubDirs/UserDropped/
               // KernelDropped/RootChanged 都走这里
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
    // 析构自动 stop()（join 后台线程）；对象必须活到回调返回——回调内析构
    // 属误用（detached/在跑的 worker 会摸已析构的 Impl，见 stop() 契约）
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 启动监听 dir（递归子树）。事件在 debounceMs 静默窗口后成批交给 onChange。
    //   - dir 为空 / 不是已存在的目录 / 回调为空 → fail（ValidationError / FileNotFound）
    //   - 原生监听建不起来（句柄/线程/事件流）→ fail（Internal / FileNotFound，带原因）
    //   - 平台无真实现（POSIX stub）→ 返回 ok 但 active()==false，
    //     消费方据此退回轮询兜底
    // 重复 start 先隐式 stop（支持换目录重启）。
    core::Result<void> start(const std::filesystem::path& dir,
                             FileWatchCallback onChange,
                             std::chrono::milliseconds debounceMs =
                                 std::chrono::milliseconds(200));

    // 停止监听并 join 后台线程；幂等。
    // 回调线程内调用 = 只请求停止（置 running=false + SetEvent；不 join、
    // 不关句柄），worker 收到请求后自行收尾退出；join 由之后非回调线程的
    // stop()/析构完成。故消费方必须保证对象活到回调返回（回调内析构 = 误用）。
    void stop();

    // 真监听是否在跑（Win：句柄/线程存活；macOS：FSEvents 流已武装且 worker
    // 存活；stub 平台恒 false）
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
