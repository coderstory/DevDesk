// src/module_terminal/web/web_session_bridge.h
// 网页 ↔ 桌面会话桥（need#490 技术卡 v9 §3/§5）：网页与桌面共享同一 SessionController
// ——tab 列表/新建/关闭/激活同一份，激活 tab 共享；网页可改终端尺寸（20–200 列 /
// 8–200 行钳制，桌面路径不钳制、跟随最后写入者）；退出/失败态带 exitCode 与重开入口。
// 设置单一真源：writeSettings 唯一入口（校验→写内存→持久化→回灌→广播）。
// 本类不碰 HTTP/加密（W3 服务直接消费事件与快照）。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_settings.h"

namespace terminal::web {

// 网页侧尺寸钳制（PRD P-14 / 技术卡 §5.1）；桌面 resizeActive 不经本函数。
inline constexpr int kWebMinCols = 20;
inline constexpr int kWebMaxCols = 200;
inline constexpr int kWebMinRows = 8;
inline constexpr int kWebMaxRows = 200;

struct SizeClampResult {
    int rows;
    int cols;
    bool clamped;
};
SizeClampResult clampWebTerminalSize(int rows, int cols);

enum class WebEventType {
    Tabs,            // tab 增删/切换/新建/重开
    Exit,            // 会话退出（exitCode 有效）
    Failed,          // spawn 失败（reason=错误文案）
    Settings,        // 设置经 writeSettings 成功变更
    DesktopRestart,  // 桌面重启/无任何会话的启动态
};

struct WebEvent {
    WebEventType type = WebEventType::Tabs;
    int tabIndex = -1;
    int exitCode = 0;
    std::string reason;
    int rows = 0;
    int cols = 0;
};

struct WebTab {
    int index = 0;
    std::string title;   // 现状 = 会话序号；OSC 标题由 W3/W5 补
    bool active = false;
    std::string state;   // idle/starting/running/exited/failed
    int exitCode = 0;
};

class WebSessionBridge {
public:
    using SaveFn = std::function<core::Result<void>(const TerminalSettings&)>;
    using RepaintFn = std::function<void()>;
    using Listener = std::function<void(const WebEvent&)>;

    WebSessionBridge(SessionController& controller, TerminalSettings* settings, SaveFn save,
                     RepaintFn repaint = nullptr);

    void setListener(Listener listener) { listener_ = std::move(listener); }

    // ---- 共享会话 / tab ----
    std::vector<WebTab> tabs() const;
    int createTab();
    bool closeTab(int index);
    bool activateTab(int index);
    bool restartTab(int index);

    // 每帧调用：采样 Exited/Failed 变化，只发一次事件（W3 ticker 内调）。
    void pollStates();

    // 桌面启动态无任何会话 → 发 DesktopRestart（W4 完成判据 P1-A1）。
    void markDesktopRestart();

    // ---- 尺寸 ----
    // 网页 resize：钳制后写激活会话；首次网页接管前记录桌面尺寸。
    bool webResize(int rows, int cols, int* appliedRows = nullptr, int* appliedCols = nullptr);
    // 桌面 resize 前上报（module compose 调），最后网页订阅退出时恢复到最近桌面尺寸。
    void observeDesktopResize(int rows, int cols);
    // 网页订阅计数（W3 SSE 订阅增减调）；最后一个退出时恢复桌面尺寸。
    void addSubscriber();
    void removeSubscriber();
    int subscriberCount() const { return subscribers_; }

    // ---- 设置单一真源（网页 settings.set 与桌面面板「完成」同一入口） ----
    core::Result<void> writeSettings(const TerminalSettings& next);

private:
    void emit_(const WebEvent& ev);
    static std::string stateName_(SessionState state);

    SessionController& controller_;
    TerminalSettings* settings_ = nullptr;
    SaveFn save_;
    RepaintFn repaint_;
    Listener listener_;

    // 桌面尺寸记忆：网页接管前的最近桌面行列，最后一个订阅退出时恢复。
    bool desktopSizeValid_ = false;
    int desktopRows_ = 0;
    int desktopCols_ = 0;
    bool webSizeApplied_ = false;
    int subscribers_ = 0;

    // 退出/失败态变化采样：titleNumber 稳定（关闭会移 index，不用 index 当键）。
    std::map<int, SessionState> lastState_;
    std::map<int, int> lastExitCode_;
};

} // namespace terminal::web
