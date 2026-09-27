// src/core/atomic_write.h
// 文件原子写（唯一实现）。此前仓内散着 3 份同模式拷贝（module_claude_proxy
// settings_sync::atomicWrite / module_markdown atomicWriteFile / module_settings
// atomicWriteConfig），第 4 处调用点（module_todo 的视图状态落盘）漏改仍用
// ofstream(trunc) 原地写——2026-09-27 审计 THS-03 据此下沉 core，供 module_*
// 复用（module_* 互不依赖，公共 util 只能落在 core）。
#pragma once

#include <filesystem>
#include <string>

#include "result.h"

namespace core {

// 原子写：把 content 逐字节写入 target。
// 实现 = 同目录临时文件 <target>.devdesk-tmp → remove(target) → rename(tmp, target)
// （MinGW 的 rename 不覆盖已存在目标，故先删）。成功后目标内容与 content 完全
// 一致、无临时文件残留；失败时目标保持原有内容（不会被截断成半截），临时文件
// 已清理。父目录需调用方自行保证存在（与本工具的前身拷贝语义一致）。
// label 仅用于错误信息前缀（如 "配置文件" → "配置文件写入失败: ..."）。
// 不抛异常（项目铁律），全部走 std::error_code 重载。
core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label = "文件");

} // namespace core
