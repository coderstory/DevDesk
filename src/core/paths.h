// src/core/paths.h
#pragma once

#include <filesystem>
#include <string>

namespace core::paths {

// 应用数据根目录
//   macOS:   ~/Library/Application Support/DevDesk/
//   Windows: %APPDATA%\DevDesk\
std::filesystem::path appDataDir();

// 配置文件（config.json）
std::filesystem::path configFile();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// Markdown 笔记根目录
std::filesystem::path markdownDir();

// 日志目录
std::filesystem::path logsDir();

// 主日志文件
std::filesystem::path logFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths