// 「用 DevDesk 打开文件」的路径解析：把绝对路径换算成笔记目录内的 POSIX
// 相对路径，供 main.cpp 以 Router 负载 "open:<相对路径>" 分发。
//   目录内 → 直接相对化；目录外 → 复制进根目录（强制 .md、撞名编号）。
// 纯文件系统操作，不碰 UI/DB。失败（不存在/目录/复制出错）→ Result::fail。
#pragma once

#include "core/result.h"

#include <filesystem>
#include <string>

namespace module::markdown {

core::Result<std::string> captureFileIntoNotesDir(
    const std::filesystem::path& notesDir,
    const std::string& absPath);

// 查看/装载场景的 UTF-8 绝对路径文件读取（二进制原样字节）。
// Windows 纪律：std::ifstream 的 std::string 重载走窄字符 ANSI fopen，非 ASCII
// 路径必败（BUG-1 根因）；路径须经 fs::path 让 libstdc++ 走 UTF-16 宽打开。
// 不存在 → FileNotFound；打开失败 → FileReadFailed（查看器错误占位两态区分）。
core::Result<std::string> readFileUtf8(const std::string& absPath);

} // namespace module::markdown
