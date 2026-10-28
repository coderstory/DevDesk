// DevDesk 数据库当前完整 schema（单份、无版本、无历史包袱）
// 口径：不写版本化迁移链——没有 schema_version 表 / 顺序 ALTER 序列 / 新库守卫。
// applySchema 在一个事务里幂等执行全脚本。
// 改 schema 的唯一动作：直接改这份脚本，不追加版本、不写兼容分支。
// （master 侧 v1-v4 版本化升级中的真实 schema 变更已直接折进本 DDL：todo_tasks
//  的归档时间戳列一次建对；旧库表重建与桌宠配置数据清理只针对既有库，新库无需对应物。）
//
// **补列迁移**（addMissingAgentColumns）：CREATE IF NOT EXISTS 对已存在表跳过，
// 老库永远缺新列（2026-10-19 context_note 报障根因）。本函数在 applySchema 之后对
// agent 三表逐列 PRAGMA table_info 补缺，幂等——新库不炸、老库不丢数据。
#pragma once

#include "result.h"

namespace core::data { class SqliteDb; }

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

    -- kind 是列的语义标记（UI 行为靠它，不靠标题字符串）。UAT6.2 H20 起固定四列：
    --   'todo'    = 待办列（新建任务默认落列）          position 0
    --   'doing'   = 进行中列（H20 新增语义值）          position 1
    --   'done'    = 已完成列（完成任务 → 流转目标）     position 2
    --   'archive' = 已归档列（归档动作目标 / 只读）     position 3
    --   NULL      = 老库普通列（Database::migrate 启动路径的固定四列迁移会把它并入默认列）
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
        -- UAT6.2 H31：关联的 Markdown 笔记相对路径（POSIX 分隔）；空串 = 未关联。
        -- 只存路径键不存标题：改名后靠路径失联判定灰态，不自动解除关联。
        doc_path TEXT NOT NULL DEFAULT '',
        position INTEGER NOT NULL,
        completed_at INTEGER,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        archived_at INTEGER,
        FOREIGN KEY (column_id) REFERENCES todo_columns(id) ON DELETE CASCADE
    );

    -- Claude 配置切换器
    -- models_config / extra_env 均为 JSON 文本列，空串 = 未配置：
    --   - models_config      模型列表（多槽位），读侧空串时回落 default 槽位 = model
    --   - extra_env          env 装饰层（键值对，如 ANTHROPIC_SMALL_FAST_MODEL）
    -- auth_field 三态：'both'（默认，双发 x-api-key + Authorization: Bearer）/ 'bearer' /
    --   'api_key'——代理转发据此决定注入哪个鉴权头；未知值 / 空串读侧兜底 'both'。
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
        auth_field TEXT NOT NULL DEFAULT 'both'
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
        system_prompt TEXT NOT NULL DEFAULT '',
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

    -- JSON 配置文件的唯一落点。config.json / markdown_view_state.json
    -- / todo_view_state.json / claude_settings_snapshot.json 走本表；run-settings/*.json
    -- 走下面的 claude_run_settings。
    --
    -- **一张通用表，不按配置各建一张**：每份配置一个命名空间前缀（如
    -- `config.json:`），前缀之下是它的键。六张表 = 六个真相源，
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
    --   `first_sync_done` / `unsynced_notice` / `token`；value 为原样文本
    --   （sha/hash/标记）。`token` 行例外：value 是 credential::protect 后形态
    --   （明文平台 = 明文，靠 DB 文件 0600；Windows = DPAPI hex），凭据不进 dump
    --   正靠本表整体被剔除这一条。
    CREATE TABLE IF NOT EXISTS github_sync_state (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
    );

    -- ==================== 上线模块（Release）====================
    -- 9 张表与 release_types.h 的结构体一一对应。约定：
    --   - 枚举一律存文本（跨版本可读、避免数字漂移）；布尔存 0/1 INTEGER；
    --   - 时间列：分钟语义 = 当天 0..1439；epoch 语义 = 秒；「未完成」恒为 0（不用 NULL），
    --     0 是唯一的未完成判据，读侧无需区分「NULL / 空」；
    --   - 任务引用基础配置时全量快照（env_name / 时间窗 / 服务清单 / 类型 / 默认分支），
    --     基础配置后续改动不回灌已建任务。

    -- 基础配置：服务目录。kind = backend / pc_web / mobile_web
    CREATE TABLE IF NOT EXISTS release_service (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        kind TEXT NOT NULL DEFAULT 'backend',
        default_branch TEXT NOT NULL DEFAULT '',
        sort INTEGER NOT NULL DEFAULT 0
    );

    -- 基础配置：环境目录。end_minute <= start_minute 视为跨零点到次日
    CREATE TABLE IF NOT EXISTS release_env (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        start_minute INTEGER NOT NULL DEFAULT 0,
        end_minute INTEGER NOT NULL DEFAULT 0,
        sort INTEGER NOT NULL DEFAULT 0
    );

    -- 环境包含的服务（EnvDef.serviceIds 的落点；删环境/服务即级联清关联）
    CREATE TABLE IF NOT EXISTS release_env_service (
        env_id INTEGER NOT NULL,
        service_id INTEGER NOT NULL,
        PRIMARY KEY (env_id, service_id),
        FOREIGN KEY (env_id) REFERENCES release_env(id) ON DELETE CASCADE,
        FOREIGN KEY (service_id) REFERENCES release_service(id) ON DELETE CASCADE
    );

    -- 上线任务。一天一个按 plan_date 判；作废不占当天名额 → plan_date 不能加 UNIQUE。
    --   status = draft / pending / running / done / voided
    --   announce_sent_at = epoch 秒，0 = 未发布；其余 *_ok 为 0/1，0 = 未完成
    CREATE TABLE IF NOT EXISTS release_task (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        plan_date TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'draft',
        announce_text TEXT NOT NULL DEFAULT '',
        announce_sent_at INTEGER NOT NULL DEFAULT 0,
        pretest_no TEXT NOT NULL DEFAULT '',
        pretest_ok INTEGER NOT NULL DEFAULT 0,
        cab_no TEXT NOT NULL DEFAULT '',
        cab_ok INTEGER NOT NULL DEFAULT 0,
        ticket_nacos_no TEXT NOT NULL DEFAULT '',
        ticket_nacos_ok INTEGER NOT NULL DEFAULT 0,
        ticket_db_no TEXT NOT NULL DEFAULT '',
        ticket_db_ok INTEGER NOT NULL DEFAULT 0
    );

    -- 任务环境快照（时间窗是建任务时的拷贝）
    CREATE TABLE IF NOT EXISTS release_task_env (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_id INTEGER NOT NULL,
        env_name TEXT NOT NULL,
        start_minute INTEGER NOT NULL DEFAULT 0,
        end_minute INTEGER NOT NULL DEFAULT 0,
        removed INTEGER NOT NULL DEFAULT 0,
        FOREIGN KEY (task_id) REFERENCES release_task(id) ON DELETE CASCADE
    );

    -- 任务服务快照。服务级「跳过」= removed=1：不占进度、不提醒、不进执行清单
    CREATE TABLE IF NOT EXISTS release_task_service (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_env_id INTEGER NOT NULL,
        service_name TEXT NOT NULL,
        service_kind TEXT NOT NULL DEFAULT 'backend',
        default_branch TEXT NOT NULL DEFAULT '',
        branch TEXT NOT NULL DEFAULT '',
        removed INTEGER NOT NULL DEFAULT 0,
        FOREIGN KEY (task_env_id) REFERENCES release_task_env(id) ON DELETE CASCADE
    );

    -- 服务配置项（仅后端）。kind = nacos / db / cron / menu / gateway
    --   conf_order = 结构体 TaskConfig.order —— order 是 SQL 关键字，列名不能用它
    --   skipped = 1 表示本次没有（不校验、不占进度）；done_at = epoch 秒，0 = 未完成
    CREATE TABLE IF NOT EXISTS release_task_config (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_service_id INTEGER NOT NULL,
        kind TEXT NOT NULL DEFAULT 'nacos',
        content TEXT NOT NULL DEFAULT '',
        skipped INTEGER NOT NULL DEFAULT 0,
        conf_order INTEGER NOT NULL DEFAULT 0,
        done_at INTEGER NOT NULL DEFAULT 0,
        FOREIGN KEY (task_service_id) REFERENCES release_task_service(id) ON DELETE CASCADE
    );

    -- 服务级步骤。kind = branch / config / deploy / verify（branch = 合并分支）；
    -- done_at epoch 秒，0 = 未完成。配置完成以 release_task_config.done_at 记账，
    -- kind=config 行只是可选的聚合标记。
    CREATE TABLE IF NOT EXISTS release_task_step (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_service_id INTEGER NOT NULL,
        kind TEXT NOT NULL DEFAULT 'branch',
        done_at INTEGER NOT NULL DEFAULT 0,
        FOREIGN KEY (task_service_id) REFERENCES release_task_service(id) ON DELETE CASCADE
    );

    -- 操作留痕（action = confirm / revoke / submit / reorder；at = epoch 秒）
    CREATE TABLE IF NOT EXISTS release_task_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_id INTEGER NOT NULL,
        scope TEXT NOT NULL DEFAULT '',
        step TEXT NOT NULL DEFAULT '',
        action TEXT NOT NULL DEFAULT '',
        at INTEGER NOT NULL DEFAULT 0,
        note TEXT NOT NULL DEFAULT '',
        FOREIGN KEY (task_id) REFERENCES release_task(id) ON DELETE CASCADE
    );

    -- 唯一性兜底：去重守卫在先，唯一索引在后（与建表同脚本幂等执行）。
    --   去重守卫：这四条 DELETE 仅在建索引前清理约束上线前的历史重复行，每组保留
    --   最小 id；幂等、正常库零命中。早批次产物库若在无约束期写入过重复行，
    --   靠它避免 CREATE UNIQUE INDEX 直接失败拖垮启动链。
    --   task 先去重（外键 ON DELETE CASCADE，删重复 task 行会级联带走其子行，
    --   属预期丢弃），env / config / step 随后各自清残留重复。
    DELETE FROM release_task WHERE status <> 'voided' AND id NOT IN
        (SELECT MIN(id) FROM release_task WHERE status <> 'voided' GROUP BY plan_date);
    DELETE FROM release_task_env WHERE id NOT IN
        (SELECT MIN(id) FROM release_task_env GROUP BY task_id, env_name);
    DELETE FROM release_task_config WHERE id NOT IN
        (SELECT MIN(id) FROM release_task_config GROUP BY task_service_id, kind);
    DELETE FROM release_task_step WHERE id NOT IN
        (SELECT MIN(id) FROM release_task_step GROUP BY task_service_id, kind);

    -- 唯一索引防并发写穿透（应用层已拦为主，库层为兜底）：
    --   同日非作废任务唯一（作废不占当天名额，可共存）；
    --   环境 / 配置 / 步骤各按 (外键, 键) 唯一。
    CREATE UNIQUE INDEX IF NOT EXISTS idx_release_task_plan_date_active
        ON release_task(plan_date) WHERE status <> 'voided';
    CREATE UNIQUE INDEX IF NOT EXISTS idx_release_task_env_unique
        ON release_task_env(task_id, env_name);
    CREATE UNIQUE INDEX IF NOT EXISTS idx_release_task_config_unique
        ON release_task_config(task_service_id, kind);
    CREATE UNIQUE INDEX IF NOT EXISTS idx_release_task_step_unique
        ON release_task_step(task_service_id, kind);

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

    -- 按 status 过滤的全库只有两处，且都钉死 'running'：冷启动兜底轮询
    -- （AgentStore::hasRunningRun 的 SELECT 1 … LIMIT 1）与启动清扫（把上次异常退出
    -- 遗留的 running 改 failed）。部分索引只收 running 行（常态 0~1 条），agent_runs
    -- 一行一次运行、只增不删（仅随任务级联删），全列索引会跟着它无限长。
    -- 查询侧写 `status='running'` 才命中：planner 靠「WHERE 蕴含索引 WHERE」匹配，
    -- 写成 `status != 'x'` / 参数化 status 都退化成全表扫。
    CREATE INDEX IF NOT EXISTS idx_agent_runs_running
        ON agent_runs(status) WHERE status='running';

    -- 直连会话记录：按写入时刻倒序取最近（run_key 已有 UNIQUE 索引）
    CREATE INDEX IF NOT EXISTS idx_run_settings_written
        ON claude_run_settings(written_at DESC);

    -- 种子数据
    -- 首次建库插入 4 个固定 Todo 列，kind 直接带最终值（UAT6.2 H20：四列固定、标题固定）。
    -- 守卫 `WHERE NOT EXISTS (SELECT 1 FROM todo_columns)`：**表非空就不插**——这是
    -- 本脚本唯一的幂等守卫（DDL 靠 IF NOT EXISTS，种子靠这一句），连跑两次
    -- applySchema 仍是这 4 行。todo_columns.title 无 UNIQUE 约束，写不了
    -- ON CONFLICT，所以用这条整表守卫。
    -- 老库（种子已存在 / 进行中 kind 为 NULL / 有多余自定义列）由
    -- Database::migrate 启动路径的 migrateTodoFixedColumns() 一次性迁移（幂等、单事务、先备份）。
    INSERT INTO todo_columns (title, position, color, created_at, kind)
    SELECT title, position, color, CAST(strftime('%s','now') AS INTEGER), kind
    FROM (
        SELECT '待办'   AS title, 0 AS position, 'claude-clay' AS color, 'todo'    AS kind
        UNION ALL SELECT '进行中', 1,             'warm-amber',           'doing'
        UNION ALL SELECT '已完成', 2,             'muted-sage',           'done'
        UNION ALL SELECT '已归档', 3,             'warm-gray',            'archive'
    )
    WHERE NOT EXISTS (SELECT 1 FROM todo_columns);
)";

// ---- 迁移定义白名单（P2 票 11）----
// 背景：补列迁移的 DDL 此前是裸字符串直接拼进 SQL（表名/列名/列定义三段拼接），
// 拼错值只会在 SQLite 运行期炸。现口径：迁移涉及的表与列一律先定义成枚举，
// 拼 SQL 前必须经下方 constexpr 白名单查表取回标识符——白名单里没有的值
// （越界枚举 / 强转脏值）在查表处即显式报错（错误信息带非法值），绝不进入拼接。
// 白名单覆盖完备性由文件底部的 static_assert 在构建期钉死：漏条目 / 枚举与数组
// 顺序错位直接编译失败。

// 迁移 DDL 允许操作的表（补列迁移的目标三表）
enum class MigrationTable { AgentTasks, AgentRuns, AgentMessages, kCount };

struct MigrationTableEntry {
    MigrationTable id;
    const char* name;  // SQL 标识符（constexpr 白名单内，静态存储期）
};
inline constexpr MigrationTableEntry kMigrationTableWhitelist[] = {
    {MigrationTable::AgentTasks,    "agent_tasks"},
    {MigrationTable::AgentRuns,     "agent_runs"},
    {MigrationTable::AgentMessages, "agent_messages"},
};

// 迁移 DDL 允许新增的列（枚举值 → 列名 + ADD COLUMN 的列定义片段）。
// 三张表共用一张白名单：同名列（如 created_at）按表前缀区分枚举值。
// 枚举顺序 = 数组顺序 = 各表 DDL 的最终列序（查表按下标 O(1)，static_assert 钉死一致）。
enum class MigrationColumn {
    // agent_tasks
    TasksName, TasksGoal, TasksTriggerType, TasksTriggerConfig, TasksModelSlot,
    TasksMaxIterations, TasksAllowDestructive, TasksEnabled, TasksContextNote,
    TasksAutoNote, TasksSystemPrompt, TasksCreatedAt, TasksUpdatedAt,
    // agent_runs
    RunsTaskId, RunsStatus, RunsStartedAt, RunsEndedAt, RunsIterations,
    RunsInputTokens, RunsOutputTokens, RunsError,
    // agent_messages
    MessagesRunId, MessagesSeq, MessagesRole, MessagesContentJson,
    MessagesToolCallId, MessagesCreatedAt,
    kCount,
};

struct MigrationColumnEntry {
    MigrationColumn id;
    const char* name;  // 列名（SQL 标识符）
    const char* ddl;   // ALTER TABLE ADD COLUMN <列名> 之后的列定义片段
};
inline constexpr MigrationColumnEntry kMigrationColumnWhitelist[] = {
    // agent_tasks（与 kSql 中三表 DDL 的最终形态一致：漏列 / 改默认值即迁移漂移）
    {MigrationColumn::TasksName,             "name",              "TEXT NOT NULL"},
    {MigrationColumn::TasksGoal,             "goal",              "TEXT NOT NULL"},
    {MigrationColumn::TasksTriggerType,      "trigger_type",      "TEXT NOT NULL"},
    {MigrationColumn::TasksTriggerConfig,    "trigger_config",    "TEXT NOT NULL DEFAULT ''"},
    {MigrationColumn::TasksModelSlot,        "model_slot",        "TEXT NOT NULL DEFAULT 'default'"},
    {MigrationColumn::TasksMaxIterations,    "max_iterations",    "INTEGER NOT NULL DEFAULT 10"},
    {MigrationColumn::TasksAllowDestructive, "allow_destructive", "INTEGER NOT NULL DEFAULT 0"},
    {MigrationColumn::TasksEnabled,          "enabled",           "INTEGER NOT NULL DEFAULT 1"},
    {MigrationColumn::TasksContextNote,      "context_note",      "TEXT NOT NULL DEFAULT ''"},
    {MigrationColumn::TasksAutoNote,         "auto_note",         "INTEGER NOT NULL DEFAULT 1"},
    {MigrationColumn::TasksSystemPrompt,     "system_prompt",     "TEXT NOT NULL DEFAULT ''"},
    {MigrationColumn::TasksCreatedAt,        "created_at",        "INTEGER NOT NULL"},
    {MigrationColumn::TasksUpdatedAt,        "updated_at",        "INTEGER NOT NULL"},
    // agent_runs
    {MigrationColumn::RunsTaskId,            "task_id",           "INTEGER NOT NULL"},
    {MigrationColumn::RunsStatus,            "status",            "TEXT NOT NULL"},
    {MigrationColumn::RunsStartedAt,         "started_at",        "INTEGER NOT NULL"},
    {MigrationColumn::RunsEndedAt,           "ended_at",          "INTEGER"},
    {MigrationColumn::RunsIterations,        "iterations",        "INTEGER NOT NULL DEFAULT 0"},
    {MigrationColumn::RunsInputTokens,       "input_tokens",      "INTEGER"},
    {MigrationColumn::RunsOutputTokens,      "output_tokens",     "INTEGER"},
    {MigrationColumn::RunsError,             "error",             "TEXT"},
    // agent_messages
    {MigrationColumn::MessagesRunId,         "run_id",            "INTEGER NOT NULL"},
    {MigrationColumn::MessagesSeq,           "seq",               "INTEGER NOT NULL"},
    {MigrationColumn::MessagesRole,          "role",              "TEXT NOT NULL"},
    {MigrationColumn::MessagesContentJson,   "content_json",      "TEXT NOT NULL"},
    {MigrationColumn::MessagesToolCallId,    "tool_call_id",      "TEXT"},
    {MigrationColumn::MessagesCreatedAt,     "created_at",        "INTEGER NOT NULL"},
};

// 构建期校验：白名单必须与枚举一一对应（条目数 = kCount 且下标 i 的条目 id = 第 i 个
// 枚举值）。查表按下标直取的前提就是这条不变量。
inline constexpr bool migrationWhitelistsAligned() {
    constexpr size_t kTableEntries =
        sizeof(kMigrationTableWhitelist) / sizeof(kMigrationTableWhitelist[0]);
    static_assert(kTableEntries == static_cast<size_t>(MigrationTable::kCount),
                  "kMigrationTableWhitelist must cover every MigrationTable");
    for (size_t i = 0; i < kTableEntries; ++i) {
        if (static_cast<size_t>(kMigrationTableWhitelist[i].id) != i) return false;
    }
    constexpr size_t kColumnEntries =
        sizeof(kMigrationColumnWhitelist) / sizeof(kMigrationColumnWhitelist[0]);
    static_assert(kColumnEntries == static_cast<size_t>(MigrationColumn::kCount),
                  "kMigrationColumnWhitelist must cover every MigrationColumn");
    for (size_t i = 0; i < kColumnEntries; ++i) {
        if (static_cast<size_t>(kMigrationColumnWhitelist[i].id) != i) return false;
    }
    return true;
}
static_assert(migrationWhitelistsAligned(),
              "migration whitelists must be complete and ordered to match their enums");

// 白名单查询（迁移拼接的唯一取词口）：非法枚举值（越界 / 强转脏值）→ fail，
// 错误信息带非法值——加载期显式报错，不拼 SQL 不进 SQLite。
// 返回指针指向 constexpr 白名单条目，静态存储期恒有效。
core::Result<const char*> migrationTableName(MigrationTable table);
core::Result<const char*> migrationColumnName(MigrationColumn column);
core::Result<const char*> migrationColumnDdl(MigrationColumn column);

// 补列迁移：对 agent 三表的已有行逐列与当前 DDL 对齐，缺列补 ALTER TABLE ADD COLUMN。
// 幂等（列已存在则跳过）；表不存在则无操作（IF NOT EXISTS 已在 applySchema 里建全）。
// 为什么不能靠 CREATE IF NOT EXISTS：IF NOT EXISTS 对已存在表跳过，加列只影响新库（本 bug 根因）。
// 本函数同时是 Database::migrate 的「启动期修列」入口末尾，会串行调用
// migrateTodoFixedColumns（Todo 固定四列迁移）。
core::Result<void> addMissingAgentColumns(core::data::SqliteDb& db);

// Todo 固定四列迁移（UAT6.2 H20）——挂在 **DB 打开 / migrate 的启动路径**，与 Todo 模块
// 是否启用无关（模块关闭时首页逾期/待办口径仍正确）。每次进程启动随 Database::open 跑一次；
// 幂等（已迁移库零改动）、破坏性部分（删多余列）整段单事务、迁移前按预检自动备份库文件、
// 迁移后校验任务行数守恒（不等即 ROLLBACK）。
//   - 按 kind 确保 todo/doing/done/archive 四列存在，缺失按固定标题/色点重建；
//   - 历史「进行中」列（kind IS NULL 且 title='进行中'）补 kind='doing'；
//   - 多余自定义列：任务按 position 顺序追加到目标默认列（同 kind → 该 kind 列，
//     无对应 kind → 待办列），随后删列；**先 UPDATE column_id 再 DELETE**（外键 CASCADE）；
//   - 四列 position 归位 0..3。
core::Result<void> migrateTodoFixedColumns(core::data::SqliteDb& db);

// Todo 关联文档列迁移（UAT6.2 H31）：老库 todo_tasks 补 doc_path 列（空串默认）。
// 挂在启动迁移序列（Database::migrate 与整库导入共用）；幂等——列已存在零操作，
// 表不存在零操作（applySchema 会建全）。与 addMissingAgentColumns 同式：
// CREATE IF NOT EXISTS 对已存在表跳过，老库永远缺新列，必须显式补。
core::Result<void> migrateTodoDocLinkColumn(core::data::SqliteDb& db);

// 冷启动归档清扫（UAT6.2 H30）：done 列中 completed_at 早于「今日本地零点」的任务
// 移入归档列（archived_at = now）；completed_at 无值的存量 done 卡视为非本日一并归档，
// completed_at 原样保留（无值仍无值）。挂在启动迁移序列里（每进程一次，非轮询）；
// 单事务 + 幂等（今日零点前完成的归档后不再命中）。缺 done/archive 列时零操作
// （固定四列由前置迁移保证，缺列 = 无归档目标，不失败）。
core::Result<void> archiveDoneTasksBeforeToday(core::data::SqliteDb& db);

// 已摘除功能的遗留表清理：幂等 DROP TABLE IF EXISTS（表不在 = 零操作）。
// 表已不在 DDL 里，但老库启动会带着它们；无外键指向、无读方，删了安全。
core::Result<void> dropRemovedTables(core::data::SqliteDb& db);

// 完整迁移入口（Database::migrate 与整库导入共用）：applySchema → 补列/Todo 固定四列 →
// Todo 文档列 → 冷启动归档清扫 → 遗留表清理。幂等；失败时各步自身保事务原子性，
// 调用方不得继续把该库交给 UI。
core::Result<void> migrate(core::data::SqliteDb& db);

} // namespace core::schema
