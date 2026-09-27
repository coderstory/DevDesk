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
// 首次启动时插入 4 个默认列。todo_columns.title 无 UNIQUE 约束，SQL 里没有
// （也无从生效）ON CONFLICT；幂等性由 schema_version 版本号保证——runMigrations
// 只执行 version > 当前版本 的迁移，且每条迁移包裹在事务里，故 v2 的 INSERT 全程
// 只跑一次，不会重复插入。（与下方 v3/v4 注释同一口径。）
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

// Todo 列语义标记（version 4，W9 批次；与 v3 并行开发，集成时从 v3 重编号）
// 完成态流转（完成任务 → 已完成列）此前靠标题字符串「已完成」硬编码，
// 用户重命名列后行为静默失效。加 kind 列做语义标记：
//   'done' = 已完成列；'todo' = 默认待办列；NULL = 普通列
// 迁移时按种子标题回填一次，此后 kind 是唯一权威（重命名不再影响语义）。
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version
// 版本号保证只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kTodoColumnKind = {
    .version = 4,
    .description = "todo columns add kind semantic marker",
    .sql = R"(
        ALTER TABLE todo_columns ADD COLUMN kind TEXT;
        UPDATE todo_columns SET kind = 'todo' WHERE title = '待办';
        UPDATE todo_columns SET kind = 'done' WHERE title = '已完成';
    )"
};

// Claude 调用日志用量列（version 5，cc-switch 对照 §3.4）
//   - 流式转发在流末回填「本次流」的四类累计 token + 首事件耗时（proxy_router.cpp）
//   - 五列全部可空：老行 / 非流式行 / 上游没给 usage 的流 = NULL（未采集），
//     与真实的 0 区分开（0 是合法的 token 数，NULL 才是"没拿到"）
//   - 单位：四个 token 列 = token 数；first_event_ms = 流开始到首个上游 SSE chunk
//     的毫秒数
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeCallLogUsageColumns = {
    .version = 5,
    .description = "claude call logs add usage token columns",
    .sql = R"(
        ALTER TABLE claude_call_logs ADD COLUMN input_tokens INTEGER;
        ALTER TABLE claude_call_logs ADD COLUMN output_tokens INTEGER;
        ALTER TABLE claude_call_logs ADD COLUMN cache_read_tokens INTEGER;
        ALTER TABLE claude_call_logs ADD COLUMN cache_write_tokens INTEGER;
        ALTER TABLE claude_call_logs ADD COLUMN first_event_ms INTEGER;
    )"
};

// 共享配置片段（version 6，2026-09-27 shared-config-and-failover 设计 §2.1）
//   - claude_shared_configs：**独立表**（name UNIQUE + extra_env，与
//     claude_profiles.extra_env 同构同语义的 JSON 文本列，空串 = 未配置）
//   - claude_profiles.shared_config_id：0 = 未绑定（0..1 绑定、不嵌套）
// 为什么独立表而不是「特殊 profile」：claude_profiles 的每一行都被当成「可选路的
// 上游」（is_active 唯一索引 / listProfiles / 自动激活 / 复制命名 / 孤儿密钥清理 /
// 代理 getActiveProfile），塞一行没有 base_url/密钥的特殊行等于给这 6 条路径各加
// 一个 kind 判断，漏判一次就把片段当供应商转发。
// 为什么片段不含 base_url / 密钥：片段定位为纯 env 装饰层——不进路由、不进凭据链
//（base_url 不进路由表、Keychain 零交互、is_active 唯一性零影响）。
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeSharedConfigs = {
    .version = 6,
    .description = "claude shared config fragments (table + profile binding)",
    .sql = R"(
        CREATE TABLE IF NOT EXISTS claude_shared_configs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL,
            extra_env TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
        ALTER TABLE claude_profiles ADD COLUMN shared_config_id INTEGER NOT NULL DEFAULT 0;
    )"
};

// Profile 认证字段（version 7，2026-09-27 profile-auth-field 设计 §3）
//   - 三态：'both'（默认，双发 x-api-key + Authorization: Bearer）/ 'bearer' /
//     'api_key'——代理转发按 profile 的该字段决定注入哪个鉴权头
//   - 读侧对未知值 / 空串兜底为 'both'（auth_field_from_text）
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeProfileAuthField = {
    .version = 7,
    .description = "claude profiles add auth field (both/bearer/api_key)",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN auth_field TEXT NOT NULL DEFAULT 'both';
    )"
};

// 所有 migrations 的权威列表（顺序敏感，必须按 version 升序）
inline const std::vector<core::data::Migration>& all() {
    static const std::vector<core::data::Migration> kMigrations = {
        kCreateInitialTables,
        kSeedDefaultTodoColumns,
        kClaudeProviderConfigColumns,
        kTodoColumnKind,
        kClaudeCallLogUsageColumns,
        kClaudeSharedConfigs,
        kClaudeProfileAuthField,
    };
    return kMigrations;
}

} // namespace core::migrations
