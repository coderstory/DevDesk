// src/app_registry.h
// 应用装配登记表（R8：装配层收口，一行 = 一个 Router 页面模块）。
//
// 这是「页面模块元数据」的唯一登记处，三个消费方都从这里取：
//   - main.cpp：extern "C" 工厂声明（历史自本文件迁来）+ getRouter 按表装配；
//   - app_assembly：buildGatedEntries 门控遍历（moduleEnabled 全项目唯一
//     调用点）+ trayPageDisplayName 查短名（托盘/关闭模块提示文案）；
//   - 测试：真实表遍历（门控键列填错 / Home 漏 override 测试层必挂）。
//
// 短名/图标两列与 Module 虚函数（shortName()/iconGlyph()）是**同值双副本**
//（tray 关闭态必须无活实例可查，静态列不可省），一致性由装配测试
// （test_app_assembly 的 registry 用例）锁死，不产生第三份。
//
// C++20 注意：不用 u8"" 字面量——u8"中文" 是 char8_t[] 不可隐式转
// const char*；图标列用 UTF-8 hex 转义，与各模块 iconGlyph() 返回字节相同。
#pragma once

#include "core/module_interface.h"
#include "settings_config.h"  // kModuleId*（门控键）

// 6 个模块工厂声明（extern "C"，自 main.cpp 迁入；全局作用域与迁移前同）
extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();
extern "C" ::core::Module* create_about_module();

namespace app::registry {

// 一行 = 一个 Router 页面模块。顺序 = 注册顺序 = 侧边栏顺序
//（= PageId 枚举序，契约 test_router core_pageid_order，表序须一致）。
// gatingKey == nullptr = 恒注册（home/settings；语义同 moduleEnabled 恒 true，
// 但显式表达不依赖谓词特殊逻辑——T3 拍板）。
struct ModuleEntry {
    ::core::PageId page;
    const char* gatingKey;   // kModuleId* 或 nullptr（恒注册）
    const char* shortName;   // 显示短名（托盘/关闭提示）；Module::shortName() 同值
    const char* iconGlyph;   // FA7 Free-Solid 码点 UTF-8；Module::iconGlyph() 同值
    ::core::Module* (*factory)();  // extern "C" create_*_module
};

inline constexpr ModuleEntry kModuleRegistry[] = {
    {::core::PageId::Home,        nullptr,                          "首页",   "\xEF\x80\x95", &create_home_module},      // U+F015 fa-house
    {::core::PageId::Markdown,    module::settings::kModuleIdMarkdown,     "笔记",   "\xEF\x81\x84", &create_markdown_module},  // U+F044 fa-pen-to-square
    {::core::PageId::Todo,        module::settings::kModuleIdTodo,         "待办",   "\xEF\x81\x98", &create_todo_module},      // U+F058 fa-circle-check
    {::core::PageId::ClaudeProxy, module::settings::kModuleIdClaudeProxy,  "Claude", "\xEF\x80\xA1", &create_claude_module},     // U+F021 fa-arrows-rotate
    {::core::PageId::Settings,    nullptr,                          "设置",   "\xEF\x80\x93", &create_settings_module},    // U+F013 fa-gear
    {::core::PageId::About,       nullptr,                          "关于",   "\xEF\x81\x9A", &create_about_module},       // U+F05A fa-circle-info
};

} // namespace app::registry