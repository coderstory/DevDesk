// src/module_settings/app_tools.h
// Settings 模块的智能体工具集（agent M2 Task 9）：app.read_settings 只读工具，
// 供 module_agent 经 core::ToolRegistry 按名调用。
// 模块隔离：本文件只依赖 core/tool.h + 标准库，不依赖 module_agent；工具归
// 本模块（与 MarkdownTools 归 module_markdown 同款分工——见计划 Global
// Constraints「各工具集归各自模块」）。
//
// 安全面（M2 Task 9 约束——只读 + 钳制 + 不泄密）：
//   · 输出是 **SettingsConfig 的白名单字段集**（9 键，清单钉在 app_tools.cpp
//     的字段白名单注释处，新增字段必须同步该注释与本头），不是整份
//     config.json 的转储：配置文件里将来若混入敏感键也不会外泄；
//   · 凭据本体不在 config.json——API Key 存在 SQLite 的
//     claude_profiles.api_key_encrypted 列（2026-10-02 存储统一前在
//     platform::keychain），本工具的字段白名单里没有任何凭据键，故无凭据面；
//   · 值取自 SettingsStore::load()，其返回值已经 sanitized（越界钳制 / 未知
//     枚举收敛 / 模块 ID 过滤），agent 拿到的就是「真正会生效的值」。
//
// 副作用说明（load() 既有行为，本工具不新增写盘路径）：配置文件**可读但 JSON
// 解析失败**时，load() 会把原始内容 copy 一份到 `<config>.json.bak` 再回落全
// 默认值（settings_config.cpp:344-353）。本工具**不重做一遍 parse**、故不把这种
// 「坏 JSON」判成 isError：真源只有 load() 一处，在工具里再解析一次等于把配置
// 格式契约复制成两份、日后必然漂移；而坏 JSON 已有 .bak + LOG_WARN 的既有恢复
// 路径（不属「静默无痕」）。工具只在**打不开 / 是目录 / 路径不可访问**这类
// 「load() 会静默当成默认值」的磁盘异常上报 isError。
//
// 错误语义（spec §10 M1 统一）：配置不可读（磁盘异常）→ execute 返回
// Result ok + ToolResult.isError=true + 中文说明（错误回喂 LLM）；
// Result::fail 只留给编程不变量破坏。
#pragma once

#include <filesystem>
#include <memory>

#include "core/tool.h"

namespace module::settings {

// 工厂：产出 "app.read_settings" 只读工具。configPath = 配置文件完整路径
//（生产传 ::core::paths::configFile()，测试传临时文件）。工具自持
// SettingsStore 实例、不摸模块成员——agent 工作线程调用天然安全，不需要锁。
std::shared_ptr<core::ITool> makeAppReadSettingsTool(
    const std::filesystem::path& configPath);

} // namespace module::settings
