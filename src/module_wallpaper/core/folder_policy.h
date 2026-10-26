// src/module_wallpaper/core/folder_policy.h
// 来源目录策略：路径规范化、可接受性、何时请求用户选目录。纯逻辑，不弹窗。
#pragma once

#include <string>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper::folder_policy {

// 空串（或纯空白）= 未配置。
bool isFolderConfigured(const std::string& folderPath);

// 规范化：去首尾空白 → 转绝对路径 → 去掉末尾分隔符（盘符根保留）。
// 空/纯空白输入返回空串（未配置），不做任何路径拼接。
std::string normalizeFolderPath(const std::string& rawPath);

// 选择结果是否可接受：必须是已存在的目录。路径为空/不存在/是文件 → false。
bool isAcceptableFolder(const std::string& normalizedPath);

// 是否应把「选目录」当主行动：未配置 / 目录不在 / 扫到 0 条都为 true；
// 只有 Playing 不需要（需求 §9.5）。
bool shouldRequestFolderSelection(const std::string& folderPath, Availability availability);

} // namespace module::wallpaper::folder_policy