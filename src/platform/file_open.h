// src/platform/file_open.h
// 平台无关接口：①从进程命令行捕获传入的文件路径（main 在 EUI-NEO，拿不到 argc/argv，各平台自读）；
// ②在系统文件管理器中打开目录；③接住不经 argv 的系统「打开文件」请求的进程内队列。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/result.h"

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过 '-' 开头的 flag）；无则空串。
// macOS: _NSGetArgc/_NSGetArgv；Windows: GetCommandLineW + CommandLineToArgvW，宽字符 → UTF-8；其他平台返回空。
std::string openFileFromArgv();

// 纯函数：从 UTF-8 argv 里选出打开文件参数——argv[1] 非空且不以 '-' 开头则返回之，否则空串。
std::string selectOpenFileArg(const std::vector<std::string>& utf8Args);

// 在系统文件管理器中打开目录（path 为 UTF-8；目录不存在返回失败，由调用方判断）。
// Windows ShellExecuteW → 资源管理器；macOS /usr/bin/open → Finder；其他 xdg-open。
core::Result<void> openDirectory(const std::string& path);

// ── 待打开文件队列（进程内 / 主线程 / 一次性取走）──
// 用途：接住不进 argv 的系统「打开文件」请求——macOS 的 Finder 双击 / 右键
// 「打开方式」走 LaunchServices 的 odoc Apple Event，需由 Cocoa 桥回调入队。
// 契约：入队与取走必须同在主线程（无锁）；空路径入队被忽略；
// takePendingOpenFiles 返回入队顺序的全部路径并在返回时清空队列（取走即清）。
// 定义在头内 inline：全进程单一队列实例，跨 TU 共享；不新增源文件、不改构建清单。
namespace detail {

// 队列唯一实例（inline 函数的局部静态在所有 TU 间共享同一对象）
inline std::vector<std::string>& pendingOpenFileQueue() {
    static std::vector<std::string> queue;
    return queue;
}

} // namespace detail

inline void enqueueOpenFile(std::string path) {
    if (path.empty()) return;
    detail::pendingOpenFileQueue().push_back(std::move(path));
}

inline std::vector<std::string> takePendingOpenFiles() {
    std::vector<std::string> taken;
    taken.swap(detail::pendingOpenFileQueue());
    return taken;
}

} // namespace platform::file_open
