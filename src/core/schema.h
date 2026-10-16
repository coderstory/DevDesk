// DevDesk 数据库当前完整 schema（单份、无版本、无历史包袱）
// 口径：不写迁移、不兼容旧数据——没有 schema_version 表 / ALTER 序列 / 新库守卫。
// applySchema 在一个事务里幂等执行全脚本；旧 DB 缺列不检测、不迁移、不兜底。
// 改 schema 的唯一动作：直接改这份脚本，不追加版本、不写兼容分支。
#pragma once

namespace core::schema {

// 完整 schema 脚本：可重复执行（幂等）；先全部 DDL，末尾一条种子 INSERT。
inline constexpr const char* kSql = R"(
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

    -- FTS5 全文索引：path 不入索引（只作行标识回查），title + content 建索引；
    -- remove_diacritics 2 = 忽略重音（中文/拉丁混排笔记检索）
    CREATE VIRTUAL TABLE IF NOT EXISTS markdown_fts USING fts5(
        path UNINDEXED,
        title,
        content,
        tokenize = 'unicode61 remove_diacritics 2'
    );

    -- kind 是列的语义标记（UI 行为靠它，不靠标题字符串）：
    --   'todo'    = 默认待办列
    --   'done'    = 已完成列（完成任务 → 流转目标）
    --   'archive' = 已归档列（空态文案 / 归档动作的目标列）
    --   NULL      = 普通列
    -- 不许退回按标题字符串匹配：用户一改名就静默失效。
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

    -- Claude 配置切换器
    -- models_config / extra_env / extra_settings_json 均为 JSON 文本列，空串 = 未配置：
    --   - models_config      模型列表（多槽位），读侧空串时回落 default 槽位 = model
    --   - extra_env          env 装饰层（键值对，如 ANTHROPIC_SMALL_FAST_MODEL）
    --   - extra_settings_json settings.json **除 env 外全部顶层属性**的原始 JSON
    --                        （model / permissions / statusLine / hooks / …，只存原文）
    -- auth_field 三态：'both'（默认，双发 x-api-key + Authorization: Bearer）/ 'bearer' /
    --   'api_key'——代理转发据此决定注入哪个鉴权头；未知值 / 空串读侧兜底 'both'。
    -- dialect_override 上游方言覆盖：空串 = 未覆盖 = 按 base_url 的 host 推断
    --   （'anthropic_default' / 'opencode_zen'）；自建网关 / 内网镜像的 host 后缀推断不出
    --   正确方言，只能显式覆盖。
    -- shared_config_id 绑定的共享片段 id：0 = 未绑定（0..1 绑定，不嵌套）。
    -- fallback_profile_id failover 备用供应商 id：0 = 无备用（0..1 单跳，不换第三家）。
    --
    -- 凭据三列（存储形态见 platform/credential_protection.h）：
    --   api_key_encrypted 凭据本体，存储形态分平台：
    --        macOS / POSIX = 明文，保护级别 = 文件权限 0600（core/file_perms.h 收紧
    --                        devdesk.db + -wal + -shm，每次打开校正）
    --        Windows       = DPAPI 密文的 hex 文本，保护级别 = 加密级
    --     空串 = 该 profile 没有密钥（「没有」与「有但解不开」由读侧错误码区分，
    --     不靠列内容区分）。Keychain 不是凭据来源，只剩启动时的一次性残留清理
    --     （claude:<id> 前缀之外的条目保留）。
    --   api_key_hint      UI 显示用（前 6 + "***" + 尾 4），脱敏形态，导出/复制链
    --                     径只允许用它，绝不用 api_key_encrypted。
    --   api_key_check     全量 SHA256 hex（64 字符）——作「密钥是否变更」判据。
    CREATE TABLE IF NOT EXISTS claude_profiles (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE NOT NULL,
        base_url TEXT NOT NULL,
        api_key_hint TEXT NOT NULL,
        api_key_check TEXT NOT NULL,
        api_key_encrypted TEXT NOT NULL DEFAULT '',
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
    -- 片段不含 base_url / 密钥：定位为纯装饰层（不进路由表、不碰凭据列）。
    -- 定位是「settings.json 非 env 属性的 JSON 预设」（extra_settings_json）。
    CREATE TABLE IF NOT EXISTS claude_shared_configs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        extra_settings_json TEXT NOT NULL DEFAULT ''
    );

    -- 逐 profile 一行配置 + 逐 profile 一行最近结果。
    --
    -- profile_quota_config：**provider_id 空串 = 未配置**（不是「配了但没选
    -- 供应商」）——「未配置」态判据就是这一列为空，别改成 NULL 表达。
    -- extra_json 存 fields() 里 **非 secret** 键的 JSON 对象原文；secrets_json
    -- 存 secret 键的同名结构（**明文入库前必过 platform::credential::protect**，
    -- 与 api_key_encrypted 复用同一对函数；POSIX 恒等 + 文件 0600，Windows
    -- DPAPI 密文 hex）。两者空串 = 该供应商不吃额外字段（沿用 profile 凭据）。
    -- auto_refresh / interval_minutes 只在 enabled=1 时被定时器读；interval 0 或
    -- 负 = 「到点即刷」（每次 tick 都算到期），便于测试与「手动刷新档」表达。
    --
    -- **ccswitch_provider_id**：用户选了 cc-switch 里某条
    -- Claude 供应商时**只存这个引用**，绝不把它那份 AK/SK 复制进本表
    -- （ccswitch_source.h 的「引用而非复制」纪律在此落库侧兑现）。查询时由
    -- CcSwitchQuotaCredentialSource 现读现取——用户在 cc-switch 改 key /
    -- 换 base_url，DevDesk 下一次查询立刻生效。空串 = 没走 cc-switch，
    -- 凭据来自 extra_json / secrets_json（手填）。
    CREATE TABLE IF NOT EXISTS profile_quota_config (
        profile_id INTEGER PRIMARY KEY,
        provider_id TEXT NOT NULL DEFAULT '',
        extra_json TEXT NOT NULL DEFAULT '',
        secrets_json TEXT NOT NULL DEFAULT '',
        ccswitch_provider_id TEXT NOT NULL DEFAULT '',
        enabled INTEGER NOT NULL DEFAULT 0,
        auto_refresh INTEGER NOT NULL DEFAULT 1,
        interval_minutes INTEGER NOT NULL DEFAULT 15
    );

    -- profile_quota_result：定时器 / 手动刷新落下的最近一次结果。
    --   kind              "tiered" / "balance"（QuotaKind 名）；空串 = 从未成功过
    --   windows_json      QuotaWindow 数组的 JSON 原文（编解码单点在
    --                      quota_store 的 encodeQuotaWindows / decodeQuotaWindows）
    --   last_error_kind   QuotaError 名（UI 据此选文案）；"none" = 当前无错
    --
    -- **fetched_at 与 last_success_at 是两列，不是一列**：
    --   fetched_at      = 最近一次**尝试**（成败都推进）。定时器用它判到期——
    --                     若用「上次成功」，一次持续失败会每个 tick 重打供应商
    --                     = 重试风暴。
    --   last_success_at = 最近一次**成功**（失败不推进）。UI 用它显示「数据时间」。
    -- 合成一列会让 UI 在一次 Transient 失败后显示自相矛盾的话：15 分钟前的
    -- 数字 + 「刚刚查询过」。
    --
    -- **「从未成功过」的可分辨性（本表最重要的一条不变量）**：失败落库时
    -- kind 保持空串、windows_json 保持空、balance_remaining 保持 0，绝不写
    -- 假的 0% ——UI 靠「kind 空 + last_error_kind 非 none」区分「失败且从未
    -- 成功」（只渲染错误行）与「有旧值 + 可能过期」。Transient 走另一条路：
    -- 保留上次成功值、只更新 last_error / last_error_kind。
    CREATE TABLE IF NOT EXISTS profile_quota_result (
        profile_id INTEGER PRIMARY KEY,
        kind TEXT NOT NULL DEFAULT '',
        windows_json TEXT NOT NULL DEFAULT '',
        balance_remaining REAL NOT NULL DEFAULT 0,
        currency TEXT NOT NULL DEFAULT '',
        plan_name TEXT NOT NULL DEFAULT '',
        fetched_at INTEGER NOT NULL DEFAULT 0,
        last_success_at INTEGER NOT NULL DEFAULT 0,
        last_error TEXT NOT NULL DEFAULT '',
        last_error_kind TEXT NOT NULL DEFAULT ''
    );

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
        context_note TEXT NOT NULL DEFAULT '',
        auto_note INTEGER NOT NULL DEFAULT 1,
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

    -- JSON 配置文件的唯一落点。config.json / pet_config.json / markdown_view_state.json
    -- / todo_view_state.json / claude_settings_snapshot.json 走本表；run-settings/*.json
    -- 走下面的 claude_run_settings。
    --
    -- **一张通用表，不按配置各建一张**：每份配置一个命名空间前缀（如
    -- `config.json:` / `pet_config`），前缀之下是它的键。六张表 = 六个真相源，
    -- 备份/导出/审计都得逐张拼，漏一张丢一份。
    --   value 恒为 TEXT 且**存得住任意字节**：中文 / 空串 / `{}` / `[]` / 含换行 /
    --   超长串都按原样进出（每份配置用自己的结构编码成字符串，见 core/app_config.h）。
    --   空串是合法配置值，与「键不存在」严格区分（读侧 NotFound vs 命中空串）。
    -- 本表**不含密钥**：凭据走 claude_profiles.api_key_encrypted（分平台保护）；
    -- run-settings 的临时 settings 含明文 key，由 claude_run_settings
    -- 自己那道保护兜（见该表注释）。
    CREATE TABLE IF NOT EXISTS app_config (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL,
        updated_at INTEGER NOT NULL
    );

    -- Claude 直连会话的临时 settings 记录
    -- run-settings/*.json **不是配置文档**，是「每次运行落一份、之后要读回来对比/
    -- 追踪」的记录：单键会被后一次覆盖，所以单独建表，一行一次运行。
    --
    -- 为什么还留着一个文件：`claude --settings <path>` 是**外部进程契约**，
    -- 被启动的 claude CLI 只会按路径读文件，DevDesk 无法改成从库里读。
    -- 因此口径是：**DB 行是记录与追踪的真值，文件是喂给外部进程的临时载体**。
    -- 写入顺序 = 先落库（记录先成立）→ 再落文件 → 再 chmod 0600 → 再启动会话；
    -- 中途失败按 runProfile 的既有补偿删文件并按失败返回。
    --
    -- settings_json **含明文 API key**（buildDirectEnv 的密钥键三态之一），
    -- 故经 platform/credential_protection 的分平台保护入库
    -- （macOS/POSIX = 明文 + 库文件权限 0600；Windows = DPAPI 密文 hex），
    -- **不新写一套加密**。空串合法（「没有 env」）。
    -- written_at 供 24h 清扫与排序；file_path 记当时物化出来的文件路径
    -- （会话退出自删 + 24h 清扫后该路径已不存在，属正常）。
    CREATE TABLE IF NOT EXISTS claude_run_settings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        run_key TEXT NOT NULL UNIQUE,
        profile_id INTEGER NOT NULL,
        profile_name TEXT NOT NULL DEFAULT '',
        written_at INTEGER NOT NULL,
        file_path TEXT NOT NULL DEFAULT '',
        settings_json TEXT NOT NULL DEFAULT ''
    );

    -- GitHub Sync 同步基线（本机专属状态）。在 core::sql_dump::defaultSkipTables()
    -- 里被排除 → **SQL dump 不含本表**（dump 是跨设备传输面，基线不得随它走）。
    -- 本地备份是整库文件拷贝（db_backup），含本表且应当含：备份回滚要连基线一起
    -- 回到备份时刻，否则恢复后全部笔记被误判为本地新增。
    --   为什么必须独立于 app_config：dump 会整体跨设备转移，基线（上次同步
    --   成功后的远端 sha + 本地内容 hash、首次方向标记、未同步提示）若随 dump
    --   落到新设备，新设备会拿**别人写的** sha/hash 做比对——判「未变」跳过下载，
    --   再把「登记有而本地无」判成本地删除 → DELETE 清空远端笔记。独立成表才能
    --   从 dump 里整表剔除（app_config 无法只剔前缀）。
    --   key 形如 `sha:notes/<名>` / `hash:notes/<名>` / `sha:data/database.sql` /
    --   `first_sync_done` / `unsynced_notice`；value 为原样文本（sha/hash/标记）。
    CREATE TABLE IF NOT EXISTS github_sync_state (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
    );

    -- 索引
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

    -- 直连会话记录：按写入时刻倒序取最近（run_key 已有 UNIQUE 索引）
    CREATE INDEX IF NOT EXISTS idx_run_settings_written
        ON claude_run_settings(written_at DESC);

    -- 种子数据
    -- 首次建库插入 4 个默认 Todo 列，kind 直接带最终值。
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
