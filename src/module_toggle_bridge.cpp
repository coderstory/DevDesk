// src/module_toggle_bridge.cpp —— 模块开关即时生效桥的实现缝。
// 独立 TU（不落 app_assembly.cpp）的原因：probe 可执行只链接 module_* 库、
// 不链接 app_assembly.cpp，settings_module.cpp 引用 app::toggle 符号后 probe
// 链接会 undefined symbol。缝本体零 app/EUI 依赖（只 core/logging.h），放
// src 根与头成对（中立层桥头 = 头 + 缝），DevDesk exe / devdesk_tests /
// probes 三个消费方各自显式列入源表。真 handler 由 main.cpp 装配段注入
//（活门控任务 3）；为空时 no-op + LOG_WARN——启动早期 / 测试环境拨开关
// 不崩，写盘（settings 侧已 persist）不受影响。
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

}  // namespace app::toggle
