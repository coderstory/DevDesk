// src/module_agent/agent_module.h
// 智能体页模块（agent M1 Task 11）：**装配接线空壳**。
//
// 本文件只负责把 PageId::Agent 挂进应用装配层（enum/registry/settings 键/
// main 持有），页面本体（任务列表 / run 视图 / AgentLoop 接线）由 Task 12
// 填实——compose 现仅画一个占位元素，onStart/onStop 用基类空实现。
#pragma once

#include "core/module_interface.h"

namespace module::agent {

class AgentModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Agent; }
    std::string displayName() const override { return "智能体"; }
    std::string shortName() const override { return "智能体"; }
    std::string iconGlyph() const override;   // eui::utf8(0xF544)，cpp 内实现
    void compose(::eui::Ui& ui, const ::eui::Screen& screen) override;
};

} // namespace module::agent
