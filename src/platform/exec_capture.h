// src/platform/exec_capture.h
// 跨平台子进程执行接口：跑命令、拿退出码、不弹窗。
// 所有调用方必须在**后台线程**调用（阻塞式同步等待进程结束）。
#pragma once

#include <string>
#include <vector>

namespace platform {

// 检查指定可执行文件是否在 PATH 上（不启子进程，纯文件系统探测）。
// 平台实现：Windows = SearchPathW，POSIX = 逐 PATH 目录 stat。
bool executableOnPath(const std::string& name);

// 在指定工作目录下执行命令，阻塞等待进程结束，返回退出码。
// 不弹控制台窗口（Windows: CREATE_NO_WINDOW），标准输出/错误静默丢弃。
// 返回值：进程退出码（0 = 成功）。启动失败 → -1。
// 调用方负责保证 args 至少有一个元素（程序名）。
int execCapture(const std::string& workingDir,
                const std::vector<std::string>& args);

} // namespace platform