// src/platform/file_trash.h
// 平台能力：把文件移入系统废纸篓（need356 F-1「删除当前壁纸素材」的唯一删除动作）。
// 不做直删：撤销路径只有系统废纸篓一条，误删可回收是产品前提。
#pragma once

#include <string>
#include <type_traits>

namespace platform::file_trash {

// 失败五档：调用方据此分派面向用户的文案（NotFound/PermissionDenied/TrashUnavailable
// 三档提示 + Busy 提示；None = 成功）。三平台实现必须映射到同一组语义，不得新增档位。
enum class TrashFailure {
    None,
    NotFound,          // 源文件/路径已不存在
    PermissionDenied,  // 无权限（含需提权）
    TrashUnavailable,  // 系统拒收（回收站禁用 / 单文件超上限 / 路径过长）
    Busy,              // 被占用 / IO 冲突 / 操作被中断
};

struct TrashOutcome {
    TrashFailure failure = TrashFailure::None;
    bool ok() const { return failure == TrashFailure::None; }
};

// path = UTF-8 绝对路径。阻塞调用。
// 线程契约：禁止在 UI 主线程调用（移废纸篓是文件系统 IO；Windows 内部还要自建 STA 线程）。
// 实现内部自管线程/COM 模型，调用方无需 CoInitialize。
TrashOutcome moveToTrash(const std::string& path);

}  // namespace platform::file_trash

static_assert(
    std::is_same_v<decltype(&platform::file_trash::moveToTrash),
                   platform::file_trash::TrashOutcome (*)(const std::string&)>,
    "file_trash: moveToTrash 签名漂移");
