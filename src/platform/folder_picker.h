// src/platform/folder_picker.h
// 平台无关目录选择器：弹出原生系统目录选择对话框，阻塞等待用户操作。
// 上游候选：若 EUI-NEO 未来提供原生目录对话框可上移至此层。
#pragma once

#include <string>

#include "core/result.h"

namespace platform::folderpicker {

// 目录选择回调类型（title, startDir）→ 所选绝对路径或 fail；用户取消 = fail
using PickFolderFn = core::Result<std::string> (*)(const std::string& title,
                                                    const std::string& startDir);

// 弹出原生目录选择对话框。title 为对话框标题栏文本（可为空串）；startDir 为初始定位目录
//（空串 = 系统默认，通常为"文档"/"此电脑"）。成功返回所选目录的绝对路径；用户取消返回 fail
//（错误信息「用户取消选择」）。
core::Result<std::string> pickFolder(const std::string& title,
                                     const std::string& startDir);

// 注入缝：默认 = 真实平台实现；测试经 setPickerForTest 替换（nullptr = 恢复默认）
inline PickFolderFn& pickerSlot() {
    static PickFolderFn slot = &pickFolder;
    return slot;
}

inline void setPickerForTest(PickFolderFn fn) {
    pickerSlot() = (fn ? fn : &pickFolder);
}

} // namespace platform::folderpicker