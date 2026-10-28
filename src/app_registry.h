// src/app_registry.h
// 应用装配登记表（一行 = 一个 Router 页面模块）。「页面模块元数据」的唯一登记处，消费方：
// main.cpp（extern "C" 工厂声明 + getRouter 按表装配）、app_assembly（门控遍历 + trayPageDisplayName
// 查短名）、测试（真实表遍历）。短名/图标两列与 Module 虚函数是同值双副本（tray 关闭态须无活实例
// 可查），一致性由装配测试锁死。C++20：图标列用 UTF-8 hex 转义（u8"" 是 char8_t[] 不可隐式转 char*）。
#pragma once

#include "core/module_interface.h"
#include "settings_config.h"

// 9 个模块工厂声明（extern "C"）
extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();
extern "C" ::core::Module* create_about_module();
extern "C" ::core::Module* create_agent_module();
extern "C" ::core::Module* create_mcp_page_module();
// 平台能力不支持时返回 nullptr（装配层按空实例跳过，页面不注册）
extern "C" ::core::Module* create_wallpaper_module();

namespace app::registry {

// 一行 = 一个 Router 页面模块。顺序 = 注册顺序 = 侧边栏顺序 = PageId 枚举序（表序须一致）。
// gatingKey == nullptr = 恒注册（home/settings/about，语义同 moduleEnabled 恒 true）
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
    {::core::PageId::Agent,       module::settings::kModuleIdAgent,        "智能体", "\xEF\x95\x84", &create_agent_module},      // U+F544 fa-robot
    {::core::PageId::Wallpaper,   module::settings::kModuleIdWallpaper,    "壁纸",   "\xEF\x80\xBE", &create_wallpaper_module},  // U+F03E fa-image
    {::core::PageId::Mcp,         module::settings::kModuleIdMcp,          "MCP",    "\xEF\x87\xA6", &create_mcp_page_module},   // U+F1E6 fa-plug
    {::core::PageId::Settings,    nullptr,                          "设置",   "\xEF\x80\x93", &create_settings_module},    // U+F013 fa-gear
    {::core::PageId::About,       nullptr,                          "关于",   "\xEF\x81\x9A", &create_about_module},       // U+F05A fa-circle-info
};

} // namespace app::registry