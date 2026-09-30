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

// Todo 归档列语义标记回填（version 8，波 1 THS-14）
//   - 种子列「已归档」此前没有 kind，UI 的空态文案靠标题字符串判断
//     （todo_module.cpp 曾写 col.title == "已归档"），用户重命名即静默失效
//   - kind 列由 v4 建立（'todo' / 'done'），本次只按标题回填 'archive' 一次，
//     此后 kind 是唯一权威（重命名不再影响语义）
//   - 已知限制（与 v4 同病，不引入新机制）：升级前已改过归档列名的库，
//     按 title 回填会落空
// 版本号 8：合并 master 时由 v7 顺延——master 的 kClaudeProfileAuthField 已先占
// v7 且可能已落库，改号会让 v7 旧库重跑 ALTER；本迁移此前只存在于本分支，顺延零副作用。
inline const Migration kTodoArchiveColumnKind = {
    .version = 8,
    .description = "todo columns backfill 'archive' kind",
    .sql = R"(
        UPDATE todo_columns SET kind = 'archive' WHERE title = '已归档';
    )"
};

// Profile 上游方言覆盖（version 9，审计 CPX-13）
//   - 空串 = 未覆盖 = 按 claude_profiles.base_url 的 host 推断
//     （UpstreamDialect::forBaseUrl 的缺省行为，存量行零迁移）
//   - 值域：'anthropic_default' / 'opencode_zen'（未知 / 空串读侧回落 host 推断，
//     写侧规范化为空串——同 v7 auth_field 的两侧兜底口径）
//   - 为什么需要：自建网关 / 内网镜像的 host 后缀推断不出正确方言（zen 镜像被判
//     成 opencodeZen → 多注入 x-opencode-session；或本地反代被当成默认 → 模型名
//     的 [1M] 标记不剥，上游 400），此前只能改代码
// 版本号 9：v1..v8 已落库（v8 = 波 1 归档列 kind 回填），顺延为 v9
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeProfileDialectOverride = {
    .version = 9,
    .description = "claude profiles add dialect override",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN dialect_override TEXT NOT NULL DEFAULT '';
    )"
};

// Profile failover 备用列（version 10，2026-09-27 shared-config-and-failover
// 设计 §3.7/§3.8）
//   - claude_profiles.fallback_profile_id：0 = 无备用（0..1 单跳，不换第三家）。
//     读路径 = ClaudeService::resolveFallbackProfile（getProfile + loadApiKey，
//     任一失败 / id==0 → nullopt → 原样回错）；写路径校验拒自引用（== 自身）；
//     删除 profile 时同事务解绑「备用指向它」的行（不留悬空 id）；
//     复制 profile 时一并携带（duplicateProfile 入参）。
// 为什么只有一列：设计 §3.7 的另一列 claude_call_logs.failover_from 服务于
//「调用记录落库可审计」（B9，批 3-UI 的表单/日志/状态栏面），本批（批 3 failover
// 服务层 + 表单绑定）不读写它，先不加（加了 = 永远 DEFAULT 0 的死列）。
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeFailoverColumns = {
    .version = 10,
    .description = "claude profiles add fallback profile id (failover)",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN fallback_profile_id INTEGER NOT NULL DEFAULT 0;
    )"
};

// Profile 高级设置 JSON + 片段重构（version 11，2026-09-30 advanced-json-editor
// 设计 §3.2/§8.3）
//   - claude_profiles.extra_settings_json：settings.json **除 env 外全部顶层属性**
//     （model / permissions / statusLine / hooks / …）整体作为一份原始 JSON 存储，
//     空串 = 未配置（设计 D-1：只存原文，不拆字段）
//   - claude_shared_configs.extra_env → extra_settings_json：共享片段从「env 装饰
//     层」重构为「高级属性 JSON 预设」（同设计 §8）。旧片段 env 数据**弃用不转换**
//     （设计 §8.3）：env 键值对（如 ANTHROPIC_BASE_URL）不是合法的非 env 属性
//     JSON——把它当 settings.json 顶层属性并入，会把回环地址/占位 token 顶到
//     root 层写进 live 文件，是注入垃圾不是迁移；「尽力转换」同样做不了（env 键
//     没有 settings.json 对映规则）。按项目「No migration, no back-compat」口径
//     直接 DROP COLUMN，用户按新语义重录
//   - DROP COLUMN 需 SQLite ≥3.35（3.35.0 才加入；3.25 是 RENAME COLUMN，不可
//     混写）——与项目既有 INSERT … RETURNING 要求同一条下限
//    （profile_repository.cpp createProfile 注释），两端平台 SQLite 均 ≥3.45。
//     extra_env 列无索引/约束/触发器（v6 建表核），可安全删除
// 版本号 11：v1..v10 已落库，顺延为 v11
// 注意：ALTER TABLE ADD/DROP COLUMN 无 IF NOT EXISTS，靠 schema_version
// 版本号保证只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeExtraSettingsJson = {
    .version = 11,
    .description = "claude profiles add extra settings json; shared configs "
                   "rework to non-env settings json (old fragment env data discarded)",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT '';
        ALTER TABLE claude_shared_configs ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT '';
        ALTER TABLE claude_shared_configs DROP COLUMN extra_env;
    )"
};

// 智能体子系统三表（spec 2026-09-30-agent-design §6，R4：v11 已被
// kClaudeExtraSettingsJson 占用，顺延 v12）
inline const Migration kAgentTables = {
    .version = 12,
    .description = "建智能体子系统三表 agent_tasks / agent_runs / agent_messages",
    .sql = R"(
        CREATE TABLE IF NOT EXISTS agent_tasks (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT NOT NULL,
          goal TEXT NOT NULL,
          trigger_type TEXT NOT NULL,
          trigger_config TEXT NOT NULL DEFAULT '',
          model_slot TEXT NOT NULL DEFAULT 'default',
          max_iterations INTEGER NOT NULL DEFAULT 10,
          allow_destructive INTEGER NOT NULL DEFAULT 0,
          enabled INTEGER NOT NULL DEFAULT 1,
          created_at INTEGER NOT NULL,
          updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS agent_runs (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          task_id INTEGER NOT NULL,
          status TEXT NOT NULL,
          started_at INTEGER NOT NULL,
          ended_at INTEGER,
          iterations INTEGER NOT NULL DEFAULT 0,
          input_tokens INTEGER,
          output_tokens INTEGER,
          error TEXT,
          FOREIGN KEY (task_id) REFERENCES agent_tasks(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_agent_runs_task ON agent_runs(task_id, started_at DESC);
        CREATE TABLE IF NOT EXISTS agent_messages (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          run_id INTEGER NOT NULL,
          seq INTEGER NOT NULL,
          role TEXT NOT NULL,
          content_json TEXT NOT NULL,
          tool_call_id TEXT,
          created_at INTEGER NOT NULL,
          FOREIGN KEY (run_id) REFERENCES agent_runs(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_agent_messages_run ON agent_messages(run_id, seq);
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
        kTodoArchiveColumnKind,
        kClaudeProfileDialectOverride,
        kClaudeFailoverColumns,
        kClaudeExtraSettingsJson,
        kAgentTables,
    };
    return kMigrations;
}

} // namespace core::migrations
