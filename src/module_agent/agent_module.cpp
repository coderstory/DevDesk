// src/module_agent/agent_module.cpp
// 智能体页模块空壳实现（agent M1 Task 11）。页面本体 Task 12 填实：
// compose 现只画一个占位文本，供装配/侧边栏/契约测试引用真实模块实例。
//
// 图标码点 U+F544（fa-robot）：已核对存在于 EUI-NEO 内置
// assets/Font Awesome 7 Free-Solid-900.otf 的 cmap（Task 11 离线解析）；
// eui::utf8(0xF544) 的 UTF-8 字节为 \xEF\x95\x84，与 app_registry.h
// 图标列同值（assembly_registry_matches_module_getters 锁一致性）。
#include "agent_module.h"

#include <eui/dsl.h>

namespace module::agent {

std::string AgentModule::iconGlyph() const {
    return ::eui::utf8(0xF544);  // fa-robot
}

void AgentModule::compose(::eui::Ui& ui, const ::eui::Screen& screen) {
    (void)screen;  // 空壳阶段单元素占位，无需读屏幕尺寸
    ui.text("agent.placeholder").text("M1 接线中").build();
}

} // namespace module::agent

extern "C" ::core::Module* create_agent_module() {
    return new module::agent::AgentModule();
}
