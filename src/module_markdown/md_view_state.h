// src/module_markdown/md_view_state.h
// 审计 [23]：markdown 视图状态跨会话持久化 —— **SQLite app_config 表**（键
// `markdown_view_state`）。原 appDataDir()/markdown_view_state.json 已于
// 2026-10-03（批次 E2，用户第 17 条「所有 app 配置全部进 SQLite，不要 JSON
// 文件了」）退役：不读、不写、不迁移。
// 模块隔离：module_* 互不依赖，视图状态不进 module_settings 的 SettingsConfig
//（方案 B），也不进别的模块 —— 它落在**中立层** app_config 表里，谁都不依赖谁。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::markdown {

// 默认 app_config 键名（供测试与工具层拼键名；实现与 save/load 同一来源）
inline constexpr const char* kMarkdownViewStateKey = "markdown_view_state";

// 用户重启后最想恢复的四项（审计 [23] evidence：选中笔记 / 四态视图 / 收起的目录；
// 2026-09-29 追加：目录树选中目录）
struct MarkdownViewState {
    std::string lastPath;                    // 打开的笔记（POSIX 相对路径，空 = 无）
    // ViewMode：0=Edit 1=Split 2=Preview 3=Live（越界 → 0）。
    // 前三值是旧落盘形态的既有语义，Live 只做追加（免迁移）
    int mode = 0;
    std::vector<std::string> collapsedDirs;  // 已收起的目录（POSIX 相对路径，各层同表）
    // 目录树的**当前选中目录**（POSIX 相对路径；"" = 根目录行 —— 这是合法值，
    // 不是「无选中」）。新建笔记 / 新建文件夹的父目录由它派生，故必须与
    // collapsedDirs 一样跨会话记住：否则重启后新条目又散回根目录（本次修复的
    // 用户现象之一）。
    std::string selectedDir;
};

// markdown 视图状态读写（不抛异常；键缺失 / 值损坏一律回退默认值）。
// db：应用共享连接（生产传 ::core::appDatabase()，测试传临时库句柄）。允许为
// nullptr，但那不是「没配置」而是「数据库没就绪」：save() 返回 fail Result，
// load() 回退默认值并打 LOG_ERROR。
class MarkdownViewStateStore {
public:
    explicit MarkdownViewStateStore(core::data::SqliteDb* db);

    MarkdownViewState load() const;

    // 与 load() 同源，把「库未就绪 / 表不存在 / 值损坏」如实报出来
    core::Result<MarkdownViewState> loadChecked() const;

    core::Result<void> save(const MarkdownViewState& st) const;

    // 净化：mode 钳进合法枚举域；含 ".." 的路径丢弃（防穿越笔记根目录）
    static MarkdownViewState sanitized(const MarkdownViewState& st);

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::markdown
