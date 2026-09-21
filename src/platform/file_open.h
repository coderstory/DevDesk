// src/platform/file_open.h
// 平台无关接口：从进程命令行捕获「打开方式」传入的文件路径
// （EUI-NEO 提供 main，DevDesk 代码拿不到 argc/argv 形参，各平台用各自方式读取）
#pragma once

#include <string>

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过以 '-' 开头的 flag 参数）；无则返回空串
//   macOS:   _NSGetArgc/_NSGetArgv（Apple 稳定私有 API）
//   Windows: 标准 argc/argv（资源管理器「打开方式」/文件关联启动走 argv）
//   其他:    不支持，返回空
std::string openFileFromArgv();

} // namespace platform::file_open
