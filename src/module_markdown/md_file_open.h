// src/module_markdown/md_file_open.h
// 「用 DevDesk 打开文件」的路径解析（审计 2026-09-23 [5]）
//
// 把操作系统传入的绝对路径换算成**笔记目录内的 POSIX 相对路径**，供
// main.cpp 以 Router 负载 "open:<相对路径>" 分发给 Markdown 模块
// （consumeNavPayload 的既有通道，Home 最近编辑同款）：
//   - 文件在笔记目录内        → 直接相对化（含嵌套子目录）
//   - 文件在笔记目录外        → 复制进笔记目录根（强制 .md 后缀、撞名
//                              自动「 2」编号；Obsidian「捕获进 vault」
//                              语义），编辑/索引/树全链路立即可用
// 纯文件系统操作、不碰 UI/DB，单独成文件便于无头单测。
// 失败（文件不存在 / 目录 / 复制出错）→ Result::fail，调用方降级为仅切页。
#pragma once

#include "core/result.h"

#include <filesystem>
#include <string>

namespace module::markdown {

core::Result<std::string> captureFileIntoNotesDir(
    const std::filesystem::path& notesDir,
    const std::string& absPath);

} // namespace module::markdown
