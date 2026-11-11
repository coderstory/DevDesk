// src/platform/open_settings.h
// 打开系统「默认应用」设置页（F-10 失败引导，PRD D-5=B 平台分支）。
// 平台差异在本层收敛（*_win/_macos/_posix 拆文件，业务代码零平台宏，与
// autostart / file_open 同一模式）。
#pragma once

namespace platform::open_settings {

// 当前平台是否有「一键直达系统默认应用设置页」能力：Windows = true
//（ms-settings:defaultapps）；macOS 无按文件类型默认页的程序化直达
//（PRD D-5：官方唯一批量入口 = Finder「显示简介→打开方式→全部更改」，
// 如实呈现文字指引）→ 恒 false。业务层按此分流按钮/文字指引。
bool oneClickAvailable();

// 拉起系统默认应用设置页。仅 oneClickAvailable()==true 的平台可能成功；
// 失败/不支持返回 false（UI 层有文字指引兜底，调用方不打断用户）。
bool openDefaultAppsSettings();

} // namespace platform::open_settings
