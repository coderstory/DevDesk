// src/module_wallpaper/wallpaper_ui.h
// 壁纸页纯 UI 组装层：吃 WallpaperService::snapshot() 的快照，吐 EUI 元素树。
//
// 契约：
//   - 本层不碰 platform / 线程 / 服务；所有交互经 UiActions 回调交给装配层。
//   - 文案只消费快照里的 statusTitle/statusSubtitle/modeLabel/countdownLabel 与
//     presentation::*（空态/模式/间隔/速度/音量/让路原因），不在此拼状态文案；
//     模式说明、卡片标题、空态支持格式行属于界面固定文案。
//   - 不显示文件名；不出现删除/维护/转码/队列入口。
//   - 卡片实色（ui_kit::cardFrame），无玻璃/半透；只使用现有组件与主题 token。
#pragma once

#include <functional>
#include <string>

#include <eui/dsl.h>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

struct WallpaperSnapshot;

// 页面交互出口。缺省（空）回调 = 该交互不生效，测试可只填关心的几项。
struct UiActions {
    std::function<void(WallpaperKind)> setKind;
    std::function<void(PlayMode)> setMode;
    std::function<void(int)> setInterval;  // 秒；只允许 kRotationChoiceSeconds
    std::function<void(float)> setRate;
    std::function<void(float)> setVolume;
    std::function<void(bool)> setMuted;
    std::function<void(bool)> setPauseOnBattery;
    std::function<void(int)> setImageMinPixels;
    std::function<void(ImageFit)> setImageFit;
    std::function<void()> selectFolder;  // 弹出目录选择并写入服务
    std::function<void()> rescan;
    std::function<void()> togglePause;
    std::function<void()> next;
};

// 会话内 UI 状态：不持久化，随模块实例存活；重启恢复默认（折叠全收起、播放页）。
struct UiState {
    int tab = 0;                  // 0 = 播放，1 = 片库
    bool effectsExpanded = false; // 播放速度 + 音频共用的折叠组
    bool holdsExpanded = false;   // 让路规则折叠组
    bool intervalOpen = false;    // 间隔下拉弹层
    // 来源草稿：点击来源分段的当帧就按新来源布局，不等服务下一帧快照。
    bool kindDraftValid = false;
    WallpaperKind kindDraft = WallpaperKind::Video;
    float scrollOffset = 0.0f;    // 折叠展开后可滚动；会话内保留位置
};

// 组装整页。节点 id 契约见 .cpp 文件头。
void composeWallpaperUi(eui::Ui& ui, const eui::Screen& screen,
                        const WallpaperSnapshot& snapshot,
                        const UiActions& actions, UiState& state);

} // namespace module::wallpaper