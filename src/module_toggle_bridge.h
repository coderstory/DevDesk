// 设置页 ↔ 应用层的模块开关即时生效桥（模式照 module_pet_bridge.h：中立层桥头
// + 链接期解析 + static_assert 哨兵）。实现 = 函数指针缝（默认 no-op + LOG_WARN）：
// app 装配段注入真实现，测试注入 recording 桩。
#pragma once
#include <string>
#include <type_traits>

namespace app::toggle {

using ToggleHandler = void (*)(const std::string& moduleId, bool on);
void setToggleHandler(ToggleHandler handler);   // nullptr = 恢复默认 no-op
void onModuleToggled(const std::string& moduleId, bool on);

// 第二桥：claude 惰性装配缝。会话内开启 claude 时经此缝补装配 ClaudeService；
// 真实现由 main.cpp 装配段注入（无捕获 → 可转函数指针）。
using ClaudeAssemblyHandler = void (*)(bool autoStart);
void setClaudeAssemblyHandler(ClaudeAssemblyHandler handler);  // nullptr = 恢复默认 no-op
void ensureClaudeServiceAssembled(bool autoStart);

}  // namespace app::toggle

static_assert(std::is_same_v<decltype(&app::toggle::onModuleToggled),
                             void (*)(const std::string&, bool)>,
              "module_toggle_bridge: onModuleToggled 签名漂移");
static_assert(std::is_same_v<decltype(&app::toggle::setToggleHandler),
                             void (*)(app::toggle::ToggleHandler)>,
              "module_toggle_bridge: setToggleHandler 签名漂移");
static_assert(std::is_same_v<decltype(&app::toggle::ensureClaudeServiceAssembled),
                             void (*)(bool)>,
              "module_toggle_bridge: ensureClaudeServiceAssembled 签名漂移");
static_assert(
    std::is_same_v<decltype(&app::toggle::setClaudeAssemblyHandler),
                   void (*)(app::toggle::ClaudeAssemblyHandler)>,
    "module_toggle_bridge: setClaudeAssemblyHandler 签名漂移");
