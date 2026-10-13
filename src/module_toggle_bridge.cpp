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

}  // namespace app::toggle
