// src/module_toggle_bridge.h —— 设置页 ↔ 应用层的模块开关即时生效桥。
// 模式照 module_pet_bridge.h（中立层桥头 + 链接期解析 + static_assert 哨兵）：
// settings 不 include 任何 app/兄弟模块头；符号在可执行文件链接期解析。
// 实现 = 函数指针缝（默认 no-op + LOG_WARN）：app 装配段注入真实现（main.cpp
// 装配段），实现落 module_toggle_bridge.cpp（与头成对的 src 根 TU；因 probe
// 只链 module_* 库，app_assembly.cpp 不在其链接面——2026-10-01 评审裁定），
// 测试注入 recording 桩。handler 为空时
// onModuleToggled 只记日志不崩——启动早期/测试环境拨开关，写盘（settings
// 侧已 persist）不受影响。
#pragma once
#include <string>
#include <type_traits>

namespace app::toggle {

using ToggleHandler = void (*)(const std::string& moduleId, bool on);
void setToggleHandler(ToggleHandler handler);   // nullptr = 恢复默认 no-op
void onModuleToggled(const std::string& moduleId, bool on);

// ---- 第二桥（2026-10-01 惰性装配计划 T1）：claude 惰性装配缝 ----
// settings 页会话内开启 claude 模块 → dispatchModuleToggle on 路径（app_assembly）
// 经此缝补装配 ClaudeService（启动期门控关 = 服务未创建）。真实现由 main.cpp
// 文件级静态函数注入（无捕获可转函数指针）；nullptr = 恢复默认 no-op +
// LOG_WARN（启动早期/测试环境不崩）。
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
