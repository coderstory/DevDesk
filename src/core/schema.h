// src/core/schema.h
// DevDesk 数据库**当前**完整 schema（单份、无版本、无历史包袱）
//
// 口径（2026-09-30 用户决策）：项目不写迁移 / 不兼容旧数据——程序尚未开发完毕，
// 不存在需要升级的真实旧库。因此这里**没有** schema_version 表、没有 ALTER
// 序列、没有「数据库比二进制新」守卫：只有一份「程序现在期望的最终形态」。
//
// 语义：
//   - SqliteDb::applySchema(kSql) 把整份脚本在**一个事务**里幂等执行一遍
//     （全部 CREATE ... IF NOT EXISTS + 一条带 NOT EXISTS 守卫的种子 INSERT）。
//   - 已存在的表/索引不报错、不改动；空库与半成品库都被补成同一形态。
//   - **不升级旧库**：旧 DB 文件缺列就是缺列，本程序不检测、不迁移、不兜底。
//     想要当前 schema 就删掉旧 db 文件重来。
//
// 改动 schema 时的唯一动作：**直接改这份脚本**（改列 / 改默认值 / 改索引），
// 不追加新版本、不写兼容分支。改完跑 tests/test_core/test_schema.cpp 的
// 「逐表逐列」用例核对最终形态。
#pragma once

namespace core::schema {

// 完整 schema 脚本：可重复执行（幂等）。
// 顺序：先全部 DDL（表 → 虚拟表 → 索引），末尾一条种子 INSERT。
inline constexpr const char* kSql = R"(
    -- ==================== Markdown 笔记本 ====================
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

    -- FTS5 全文索引：path 不入索引（只作行标识回查），title + content 建索引；
    -- remove_diacritics 2 = 忽略重音（中文/拉丁混排笔记检索）
    CREATE VIRTUAL TABLE IF NOT EXISTS markdown_fts USING fts5(
        path UNINDEXED,
        title,
        content,
        tokenize = 'unicode61 remove_diacritics 2'
    );

    -- ==================== Todo 看板 ====================
    -- kind 是列的**语义标记**（UI 行为靠它，不靠标题字符串）：
    --   'todo'    = 默认待办列
    --   'done'    = 已完成列（完成任务 → 流转目标）
    --   'archive' = 已归档列（空态文案 / 归档动作的目标列）
    --   NULL      = 普通列
    -- 加这列的原因：完成态流转此前硬编码标题「已完成」，用户一改名就静默失效。
    CREATE TABLE IF NOT EXISTS todo_columns (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        title TEXT NOT NULL,
        position INTEGER NOT NULL,
        color TEXT,
        created_at INTEGER NOT NULL,
        kind TEXT
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

    -- ==================== Claude 配置切换器 ====================
    -- models_config / extra_env / extra_settings_json 均为 JSON 文本列，空串 = 未配置：
    --   - models_config      模型列表（多槽位），读侧空串时回落 default 槽位 = model
    --   - extra_env          env 装饰层（键值对，如 ANTHROPIC_SMALL_FAST_MODEL）
    --   - extra_settings_json settings.json **除 env 外全部顶层属性**的原始 JSON
    --                        （model / permissions / statusLine / hooks / …，设计 D-1 只存原文）
    -- auth_field 三态：'both'（默认，双发 x-api-key + Authorization: Bearer）/ 'bearer' /
    --   'api_key'——代理转发据此决定注入哪个鉴权头；未知值 / 空串读侧兜底 'both'。
    -- dialect_override 上游方言覆盖：空串 = 未覆盖 = 按 base_url 的 host 推断
    --   （'anthropic_default' / 'opencode_zen'）；自建网关 / 内网镜像的 host 后缀推断不出
    --   正确方言，只能显式覆盖。
    -- shared_config_id 绑定的共享片段 id：0 = 未绑定（0..1 绑定，不嵌套）。
    -- fallback_profile_id failover 备用供应商 id：0 = 无备用（0..1 单跳，不换第三家）。
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
        updated_at INTEGER NOT NULL,
        models_config TEXT NOT NULL DEFAULT '',
        extra_env TEXT NOT NULL DEFAULT '',
        shared_config_id INTEGER NOT NULL DEFAULT 0,
        auth_field TEXT NOT NULL DEFAULT 'both',
        dialect_override TEXT NOT NULL DEFAULT '',
        fallback_profile_id INTEGER NOT NULL DEFAULT 0,
        extra_settings_json TEXT NOT NULL DEFAULT ''
    );

    -- 调用日志（可审计）：五列 usage 全部可空——NULL = 未采集（老行 / 非流式 / 上游没给
    -- usage），与真实的 0 区分开（0 是合法 token 数，NULL 才是「没拿到」）。
    -- 单位：四个 token 列 = token 数；first_event_ms = 流开始到首个上游 SSE chunk 的毫秒数。
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
        input_tokens INTEGER,
        output_tokens INTEGER,
        cache_read_tokens INTEGER,
        cache_write_tokens INTEGER,
        first_event_ms INTEGER,
        FOREIGN KEY (profile_id) REFERENCES claude_profiles(id) ON DELETE CASCADE
    );

    -- 共享配置片段：**独立表**（不是「特殊 profile」）。claude_profiles 的每一行都被
    -- 当成「可选路上游」（is_active 唯一索引 / listProfiles / 自动激活 / 复制命名 /
    -- 孤儿密钥清理 / 代理 getActiveProfile），塞一行没有 base_url/密钥的特殊行等于给
    -- 这 6 条路径各加一个 kind 判断，漏判一次就把片段当供应商转发。
    -- 片段不含 base_url / 密钥：定位为纯装饰层（不进路由表、Keychain 零交互）。
    -- 定位是「settings.json 非 env 属性的 JSON 预设」（extra_settings_json）。
    CREATE TABLE IF NOT EXISTS claude_shared_configs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        extra_settings_json TEXT NOT NULL DEFAULT ''
    );

    -- ==================== 智能体（agent M1，2026-09-30）====================
    -- 手动触发的 agent run 三表：任务定义 / 运行实例 / 消息审计。
    -- FK ON DELETE CASCADE：删任务清 runs，删 run 清 messages（审计链随宿主走，
    -- 不留孤儿运行）。content_json 存 Anthropic content blocks 数组原文。
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

    -- ==================== 索引 ====================
    CREATE INDEX IF NOT EXISTS idx_tasks_column
        ON todo_tasks(column_id, position);
    CREATE INDEX IF NOT EXISTS idx_tasks_due
        ON todo_tasks(due_at) WHERE due_at IS NOT NULL;

    -- 同一时刻至多一个激活 profile（部分唯一索引）
    CREATE UNIQUE INDEX IF NOT EXISTS idx_claude_active
        ON claude_profiles(is_active) WHERE is_active = 1;

    CREATE INDEX IF NOT EXISTS idx_call_logs_started
        ON claude_call_logs(started_at DESC);
    CREATE INDEX IF NOT EXISTS idx_call_logs_profile
        ON claude_call_logs(profile_id, started_at DESC);

    -- agent：run 按任务倒序取最近 / 消息按 run 内序回放
    CREATE INDEX IF NOT EXISTS idx_agent_runs_task
        ON agent_runs(task_id, started_at DESC);
    CREATE INDEX IF NOT EXISTS idx_agent_messages_run
        ON agent_messages(run_id, seq);

    -- ==================== 种子数据 ====================
    -- 首次建库插入 4 个默认 Todo 列，kind 直接带最终值（不再「插完再按标题回填」）。
    -- 守卫 `WHERE NOT EXISTS (SELECT 1 FROM todo_columns)`：**表非空就不插**——这是
    -- 本脚本唯一的幂等守卫（DDL 靠 IF NOT EXISTS，种子靠这一句），连跑两次
    -- applySchema 仍是这 4 行。todo_columns.title 无 UNIQUE 约束，写不了
    -- ON CONFLICT，所以用这条整表守卫。
    INSERT INTO todo_columns (title, position, color, created_at, kind)
    SELECT title, position, color, CAST(strftime('%s','now') AS INTEGER), kind
    FROM (
        SELECT '待办'   AS title, 0 AS position, 'claude-clay' AS color, 'todo'    AS kind
        UNION ALL SELECT '进行中', 1,             'warm-amber',           NULL
        UNION ALL SELECT '已完成', 2,             'muted-sage',           'done'
        UNION ALL SELECT '已归档', 3,             'warm-gray',            'archive'
    )
    WHERE NOT EXISTS (SELECT 1 FROM todo_columns);
)";

} // namespace core::schema
