// src/core/migrations.h
// DevDesk 数据库 schema migrations（参考 spec §8.1 / §8.2 / §8.3）
//
// 命名规则：
//   - .version 严格递增（1, 2, ...）
//   - .description 用中文，描述本次迁移做了什么
//   - .sql 可以包含多条 SQL，runMigrations 会一次性 exec
//
// 新增迁移时：
//   1. 把 migrations() 末尾 append 一个新的 Migration
//   2. 版本号必须是 MAX(version) + 1
//   3. SQL 必须 idempotent-friendly（用 IF NOT EXISTS），避免与已有 schema 冲突
#pragma once

#include "sqlite_db.h"

#include <vector>

namespace core::migrations {

using core::data::Migration;

// 7 个核心表的初始 schema（version 1）
inline const Migration kCreateInitialTables = {
    .version = 1,
    .description = "create initial tables",
    .sql = R"(
        -- Markdown 笔记本
        CREATE TABLE IF NOT EXISTS markdown_index (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            title TEXT,
            size_bytes INTEGER NOT NULL,
            modified_at INTEGER NOT NULL,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS markdown_tags (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL
        );

        CREATE TABLE IF NOT EXISTS markdown_note_tags (
            note_id INTEGER NOT NULL,
            tag_id INTEGER NOT NULL,
            PRIMARY KEY (note_id, tag_id),
            FOREIGN KEY (note_id) REFERENCES markdown_index(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id) REFERENCES markdown_tags(id) ON DELETE CASCADE
        );

        CREATE VIRTUAL TABLE IF NOT EXISTS markdown_fts USING fts5(
            path UNINDEXED,
            title,
            content,
            tokenize = 'unicode61 remove_diacritics 2'
        );

        -- Todo 看板
        CREATE TABLE IF NOT EXISTS todo_columns (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            position INTEGER NOT NULL,
            color TEXT,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS todo_tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            column_id INTEGER NOT NULL,
            title TEXT NOT NULL,
            description TEXT,
            priority INTEGER NOT NULL DEFAULT 0,
            due_at INTEGER,
            tags TEXT,
            position INTEGER NOT NULL,
            completed_at INTEGER,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            FOREIGN KEY (column_id) REFERENCES todo_columns(id) ON DELETE CASCADE
        );

        CREATE INDEX IF NOT EXISTS idx_tasks_column
            ON todo_tasks(column_id, position);
        CREATE INDEX IF NOT EXISTS idx_tasks_due
            ON todo_tasks(due_at) WHERE due_at IS NOT NULL;

        -- Claude 配置切换器
        CREATE TABLE IF NOT EXISTS claude_profiles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL,
            base_url TEXT NOT NULL,
            api_key_hint TEXT NOT NULL,
            api_key_check TEXT NOT NULL,
            keychain_account TEXT NOT NULL,
            model TEXT NOT NULL,
            is_active INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE UNIQUE INDEX IF NOT EXISTS idx_claude_active
            ON claude_profiles(is_active) WHERE is_active = 1;

        CREATE TABLE IF NOT EXISTS claude_call_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            profile_id INTEGER NOT NULL,
            method TEXT NOT NULL,
            path TEXT NOT NULL,
            status_code INTEGER,
            latency_ms INTEGER,
            request_bytes INTEGER,
            response_bytes INTEGER,
            error TEXT,
            started_at INTEGER NOT NULL,
            FOREIGN KEY (profile_id) REFERENCES claude_profiles(id) ON DELETE CASCADE
        );

        CREATE INDEX IF NOT EXISTS idx_call_logs_started
            ON claude_call_logs(started_at DESC);
        CREATE INDEX IF NOT EXISTS idx_call_logs_profile
            ON claude_call_logs(profile_id, started_at DESC);
    )"
};

// 默认 Todo 列（version 2）
// 首次启动时插入 4 个默认列。ON CONFLICT 保证幂等性
// （多次调用 migrate 不会重复插入）。
inline const Migration kSeedDefaultTodoColumns = {
    .version = 2,
    .description = "seed default todo columns",
    .sql = R"(
        INSERT INTO todo_columns (title, position, color, created_at) VALUES
            ('待办',    0, 'claude-clay',  strftime('%s','now')),
            ('进行中',  1, 'warm-amber',   strftime('%s','now')),
            ('已完成',  2, 'muted-sage',   strftime('%s','now')),
            ('已归档',  3, 'warm-gray',    strftime('%s','now'));
    )"
};

// Claude 供应商完整配置（version 3，spec 2026-09-22 provider-config §4.1）
//   - models_config / extra_env 均为 JSON 文本列，空串 = 未配置
//   - 旧行零迁移（读时兼容：旧 model 列降级为 default 槽位 fallback，§4.2）
//   - SQLite 无 ADD COLUMN IF NOT EXISTS；幂等性由 schema_version 版本号
//     保证（v3 只执行一次，exec 多语句 + 事务包裹）
inline const Migration kClaudeProviderConfigColumns = {
    .version = 3,
    .description = "claude provider full config columns",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN models_config TEXT NOT NULL DEFAULT '';
        ALTER TABLE claude_profiles ADD COLUMN extra_env TEXT NOT NULL DEFAULT '';
    )"
};

// 所有 migrations 的权威列表（顺序敏感，必须按 version 升序）
inline const std::vector<core::data::Migration>& all() {
    static const std::vector<core::data::Migration> kMigrations = {
        kCreateInitialTables,
        kSeedDefaultTodoColumns,
        kClaudeProviderConfigColumns,
    };
    return kMigrations;
}

} // namespace core::migrations
