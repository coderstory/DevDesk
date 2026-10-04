// src/module_settings/app_tools.h
// Settings 模块的智能体工具集：app.read_settings 只读工具，供 module_agent 经
// core::ToolRegistry 按名调用（模块隔离：只依赖 core/tool.h + 标准库）。
// 契约：输出是 SettingsConfig 白名单字段集（清单见 app_tools.cpp），非整份配置转储，无凭据键；
// 存储异常 → Result ok + isError + 中文说明；Result::fail 只留给不变量破坏。
#pragma once

#include <memory>

#include "core/sqlite_db.h"
#include "core/tool.h"

namespace module::settings {

// 工厂：产出 "app.read_settings" 只读工具。db = 应用共享连接（生产传
// ::core::appDatabase()，测试传临时库）。工具自持 SettingsStore，agent 工作线程调用天然安全。
std::shared_ptr<core::ITool> makeAppReadSettingsTool(core::data::SqliteDb* db);

} // namespace module::settings
