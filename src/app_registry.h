// src/app_registry.h
// 应用装配登记表（一行 = 一个 Router 页面模块）。「页面模块元数据」的唯一登记处，消费方：
// main.cpp（extern "C" 工厂声明 + getRouter 按表装配）、app_assembly（门控遍历 + trayPageDisplayName
// 查短名）、测试（真实表遍历）。短名/图标两列与 Module 虚函数是同值双副本（tray 关闭态须无活实例
// 可查），一致性由装配测试锁死。C++20：图标列用 UTF-8 hex 转义（u8"" 是 char8_t[] 不可隐式转 char*）。
#pragma once

#include "core/module_interface.h"
#include "settings_config.h"

#include <optional>

// 页面模块工厂声明（extern "C"）；分组父项工厂按契约返回 nullptr
extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();
extern "C" ::core::Module* create_about_module();
extern "C" ::core::Module* create_agent_module();
// 平台能力不支持时返回 nullptr（装配层按空实例跳过，页面不注册）
extern "C" ::core::Module* create_wallpaper_module();
// 上线模块：分组父项工厂恒返回 nullptr（不可导航，不注册进 Router，只用于侧边栏树）；
// 两个子页各自独立实例，共用门控键 kModuleIdRelease。
extern "C" ::core::Module* create_release_group_module();
extern "C" ::core::Module* create_release_tasks_module();
extern "C" ::core::Module* create_release_config_module();

namespace app::registry {

// 一行 = 一个 Router 页面模块。顺序 = 注册顺序 = 侧边栏顺序 = PageId 枚举序（表序须一致）。
// gatingKey == nullptr = 恒注册（home/settings/about，语义同 moduleEnabled 恒 true）
struct ModuleEntry {
    ::core::PageId page;
    const char* gatingKey;   // kModuleId* 或 nullptr（恒注册）
    const char* shortName;   // 显示短名（托盘/关闭提示）；Module::shortName() 同值
    const char* iconGlyph;   // FA7 Free-Solid 码点 UTF-8；Module::iconGlyph() 同值
    ::core::Module* (*factory)();  // extern "C" create_*_module
    // 侧边栏二级菜单父级：空 = 顶级项（走既有平铺路径，渲染结果不变）；
    // 非空 = 该项挂在对应分组父项下（父项自身是本表一行，工厂返回 nullptr 不注册）。
    std::optional<::core::PageId> parent = std::nullopt;
};

inline constexpr ModuleEntry kModuleRegistry[] = {
    {::core::PageId::Home,        nullptr,                          "首页",   "\xEF\x80\x95", &create_home_module},      // U+F015 fa-house
    {::core::PageId::Markdown,    module::settings::kModuleIdMarkdown,     "笔记",   "\xEF\x81\x84", &create_markdown_module},  // U+F044 fa-pen-to-square
    {::core::PageId::Todo,        module::settings::kModuleIdTodo,         "待办",   "\xEF\x81\x98", &create_todo_module},      // U+F058 fa-circle-check
    {::core::PageId::ClaudeProxy, module::settings::kModuleIdClaudeProxy,  "Claude", "\xEF\x80\xA1", &create_claude_module},     // U+F021 fa-arrows-rotate
    {::core::PageId::Agent,       module::settings::kModuleIdAgent,        "智能体", "\xEF\x95\x84", &create_agent_module},      // U+F544 fa-robot
    {::core::PageId::Wallpaper,   module::settings::kModuleIdWallpaper,    "壁纸",   "\xEF\x80\xBE", &create_wallpaper_module},  // U+F03E fa-image
    // 上线：分组父项 + 两个可导航子页（父子三行同属 release 门控；父项工厂恒 nullptr）。
    // 整组排在 Settings 之前（设置/关于固定收尾），枚举序与表序同步。
    {::core::PageId::ReleaseGroup,  module::settings::kModuleIdRelease, "上线",     "\xEF\x84\xB5", &create_release_group_module},                                   // U+F135 fa-rocket
    {::core::PageId::ReleaseTasks,  module::settings::kModuleIdRelease, "任务",     "\xEF\x82\xAE", &create_release_tasks_module, ::core::PageId::ReleaseGroup},  // U+F0AE fa-tasks
    {::core::PageId::ReleaseConfig, module::settings::kModuleIdRelease, "基础配置", "\xEF\x80\x93", &create_release_config_module, ::core::PageId::ReleaseGroup}, // U+F013 fa-gear
    {::core::PageId::Settings,    nullptr,                          "设置",   "\xEF\x80\x93", &create_settings_module},    // U+F013 fa-gear
    {::core::PageId::About,       nullptr,                          "关于",   "\xEF\x81\x9A", &create_about_module},       // U+F05A fa-circle-info
};


// 分组父项判定（供装配层跳过无实例的父项行、侧边栏组织树）
inline bool isGroupPage(::core::PageId page) {
    for (const auto& e : kModuleRegistry) {
        if (e.parent.has_value() && *e.parent == page) return true;
    }
    return false;
}

} // namespace app::registry