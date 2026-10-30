# engram 存储检索底座选型调研（池子系统技术方案输入）

- 日期：2026-10-29（本轮实测一手核实）
- 调研人：老学究（调研 Bot）
- 任务来源：需求分析群「池子系统」技术方案协作——老板娘指派重查 + schema 精读，结论落盘登记
- 结论先行：**engram 适合当 Agent 工作记忆层，不适合当池子业务实体的主存储**。池子的自定义实体（角色卡/素材，带评分/标签/排产状态/关系网）塞不进它的固定 schema；推荐「engram 当记忆+检索辅助层 + 自建 pool.db（SQLite+FTS5）当业务实体层」或纯自建，二选一待与业务方案合并后定。

## 一、项目事实（全部一手核实，2026-10-29）

| 项 | 事实 | 证据 |
|---|---|---|
| 仓库 | Gentleman-Programming/engram | github.com/Gentleman-Programming/engram |
| 星数 | **7,680★**，806 fork，2026-10-29 当天仍在更新 | GitHub API/搜索结果 |
| 许可 | **MIT**（LICENSE 原文实读：Copyright (c) 2026 Alan Buscaglia） | raw LICENSE |
| 形态 | Go 单二进制；SQLite（WAL）+ FTS5；MCP server（stdio，27 工具）+ HTTP API（默认端口 7437）+ CLI + TUI；Claude Code / OpenCode / Codex / Pi 插件 | DOCS.md 技术参考 |
| 语言 | Go | 仓库 language 字段 |

## 二、Schema 精读（权威 = `internal/store/store.go` 的 `Store.migrate`；本节译自 DOCS.md「Database Schema」段，2026-10-29 版）

固定表清单（业务不可自增表）：

- **sessions** — id/project/ownership_mode/directory/started_at/ended_at/summary/runtime_lease_expires_at
- **session_finalizations** — 本地 finalize 重试日志（不同步）
- **observations** — 核心记忆条目：id/sync_id/session_id(FK)/type/title/content/tool_name/project/scope/topic_key/normalized_hash/revision_count/duplicate_count/last_seen_at/pinned/review_after/expires_at/**embedding/embedding_model/embedding_created_at**/created_at/updated_at/deleted_at
- **observations_fts / prompts_fts** — FTS5 虚表（trigger 同步）
- **user_prompts / prompt_tombstones**
- **memory_relations** — 关系图：source_id/target_id/relation/judgment_status(pending|judged|orphaned|ignored)+溯源+时间戳（**挂在 observations 上，不是通用实体关系**）
- 同步族：sync_chunks / sync_state / sync_mutations / sync_delete_tombstones(+remote_floors) / sync_apply_deferred
- 项目族：project_aliases / sync_enrolled_projects / cloud_upgrade_state

SQLite 配置：WAL、busy_timeout 5000ms、synchronous NORMAL、外键 ON。数据目录禁止 NFS/SMB（启动会拒）。

## 三、与池子需求的适配判定

| 池子需求 | engram 现状 | 判定 |
|---|---|---|
| 自定义实体（角色卡/素材字段：名称/来源/评分/标签） | observations 无自由 metadata JSON 列；业务字段只能塞进 content 文本或 topic_key/scope 窄通道 | ❌ 勉强，结构化字段无家可归 |
| 自建表/扩展 schema | migrate 由上游独占管理，我们加表 = 改上游，升级即冲突 | ❌ 不开放 |
| 关系网 | memory_relations 有 source/target/relation + 判定状态，但两端必须是 observations | ⚠️ 能表达「素材间关系」，但实体只能先写成记忆条目 |
| 溯源链 | observations 带 project/scope/tool_name/normalized_hash/revision_count；另有 CAPTURE-PROJECT-PROVENANCE 文档（可选抓取证据）。够「来源 URL+抓取动作」两层，第三层（后续加工动作链）要自己用多条 observation 串 | ⚠️ 两层可用，第三层绕 |
| 排产视图 | 无任何排产/看板概念 | ❌ 完全没有 |
| 检索 | FTS5 + 可选 embedding（列在，语义检索成熟度待验证）+ timeline/context | ✅ 强项 |
| MCP 工具面 | 27 工具（mem_update=agent 档，mem_delete=admin 档），HTTP API 同能力 | ✅ 强项，零自建 |
| 多端同步 | 本地 chunk 导出 + 可选云同步 | ✅ 白送的 |

## 四、结论与建议（供决策文档合并）

1. **engram 的定位**：Agent 侧「干活记忆」（会话/观察/时间线检索）——若池子系统要配 Agent 工作记忆，engram 直接用，MCP 27 工具白拿。
2. **池子业务实体主存储**：不要塞 engram。两条路：
   - **A. 双库**：pool.db（自建 SQLite+FTS5，实体/关系/排产全自定义）+ engram（Agent 记忆+检索辅助）。代价 = 两套库。
   - **B. 纯自建**：pool.db 一套搞定实体+FTS5 检索，MCP 工具面参考 engram 的 27 工具形态自写薄层（DSH 已有 MCP 骨架，见 RESEARCH-INDEX 2026-10-27 MCP 三篇）。代价 = 检索/同步功能自己养。
3. **两道验证门已闭**：① schema 不可扩展（migrate 上游独占）→ 已确认；② 关系网可表达但绑定 observations → 已确认。
4. 选 A 还是 B 取决于业务方案里「Agent 记忆」是否在池子 scope 内——等惨品业务方案定实体后再拍。

## 五、来源

- https://github.com/Gentleman-Programming/engram （星数/形态/更新时间）
- https://raw.githubusercontent.com/Gentleman-Programming/engram/main/LICENSE （MIT 原文）
- https://raw.githubusercontent.com/Gentleman-Programming/engram/main/DOCS.md （schema/CLI/HTTP/MCP 边界，2026-10-29 版）
