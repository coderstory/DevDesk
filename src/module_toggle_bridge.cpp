// module_toggle_bridge 的实现缝。独立 TU：probe 只链 module_* 库，settings 引用
// app::toggle 符号后 probe 链接会 undefined symbol。缝本体零 app/EUI 依赖。
#include "module_toggle_bridge.h"

#include "core/logging.h"

namespace app::toggle {

namespace { ToggleHandler g_handler = nullptr; }

void setToggleHandler(ToggleHandler handler) { g_handler = handler; }

void onModuleToggled(const std::string& moduleId, bool on) {
    if (g_handler == nullptr) {
        LOG_WARN("模块开关桥未装配（启动早期/测试环境），仅写盘: " + moduleId);
        return;
    }
    g_handler(moduleId, on);
}

// 第二桥：claude 惰性装配缝，形态与第一桥同款（默认 no-op + LOG_WARN）。
namespace { ClaudeAssemblyHandler g_claudeHandler = nullptr; }

void setClaudeAssemblyHandler(ClaudeAssemblyHandler handler) {
    g_claudeHandler = handler;
}

void ensureClaudeServiceAssembled(bool autoStart) {
    if (g_claudeHandler == nullptr) {
        LOG_WARN("claude 惰性装配桥未装配（启动早期/测试环境），跳过");
        return;
    }
    g_claudeHandler(autoStart);
}

// 第三桥：release 提醒弹窗总开关缝，形态与第二桥同款（默认 no-op + LOG_WARN）。
namespace { ReminderPopupHandler g_reminderPopupHandler = nullptr; }

void setReminderPopupHandler(ReminderPopupHandler handler) {
    g_reminderPopupHandler = handler;
}

void notifyReminderPopupSetting(bool on) {
    if (g_reminderPopupHandler == nullptr) {
        LOG_WARN("提醒弹窗桥未装配（启动早期/测试环境），跳过");
        return;
    }
    g_reminderPopupHandler(on);
}

// 第四桥：调度全屏弹窗总开关缝，形态与第三桥同款。
namespace { FullscreenPopupHandler g_fullscreenPopupHandler = nullptr; }

void setFullscreenPopupHandler(FullscreenPopupHandler handler) {
    g_fullscreenPopupHandler = handler;
}

void notifyFullscreenPopupSetting(bool on) {
    if (g_fullscreenPopupHandler == nullptr) {
        LOG_WARN("全屏弹窗桥未装配（启动早期/测试环境），跳过");
        return;
    }
    g_fullscreenPopupHandler(on);
}

}  // namespace app::toggle

// 动态背景桥（UAT6 主窗活体流质）：settings_module.cpp 前置声明 ::app::setFluidBackgroundEnabled，
// 主程序真实现见 app_sidebar.cpp（全局态 + compose 门控），探针与单测缺该符号时由此 weak stub
// 兜底（默认无副作用 no-op）。
#ifdef __GNUC__
#define DEVDESK_WEAK __attribute__((weak))
#elif defined(_MSC_VER)
#define DEVDESK_WEAK __declspec(selectany)
#else
#define DEVDESK_WEAK
#endif

namespace app {
DEVDESK_WEAK void setFluidBackgroundEnabled(bool /*enabled*/) {
    // 弱符号默认 no-op：探针/单测环境直接吞掉，不做动画泵/全局态变更。
}
}  // namespace app
