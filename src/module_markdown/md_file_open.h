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

} // namespace module::markdown
