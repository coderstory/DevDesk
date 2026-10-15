# T1 独立审计 — `core/sql_dump` 索引/触发器导出 + 导入修复（47fe68a）

- 审计对象：`worktree-agent-sqldump` @ `47fe68a`（worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-sqldump`）
- 依据：plan T1 节 / spec §2.1 F2（dev `1d58d76` 修订版）+ F4 / `git show 47fe68a`
- 审计者与实现者无关联；本审计除本文件外零写动作
- 复跑证据：worktree 内已编好的 `build/tests/devdesk_tests SqlDump` → `Total: 5 / Passed: 5 / Failed: 0`（实测；二进制 mtime 早于 commit 3 分钟，但 `git status` 确认 sql_dump 源无未提交改动，唯一未提交文件 `github_sync_card.cpp` 与实现者披露的解锁补丁一致、不触及本票代码）

## Findings

| # | 位置 | severity | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| 1 | `docs/superpowers/specs/2026-10-15-github-sync-api-design.md` §2.1 F4 ｜ low ｜ spec 写「常量放 `github_sync_logic`」，实现落在 `core::sql_dump::defaultSkipTables()`，spec 该行未像 F2 一样回写 | 文档口径漂移：后续按 spec 找常量会找错层；core 落位本身更优（编排层不必为清单依赖 settings 模块） | 像 F2 一样在 spec 补一句回写，认可 core 落位 |
| 2 | `src/core/sql_dump.cpp:365-383`（importFromString） ｜ low ｜ `PRAGMA foreign_keys = OFF` 与恢复两条 `db.exec` 返回值被丢弃、无日志 | 静默失败面：若关闭/恢复失败，导入将以 FK 报错（响亮）或残留 FK=OFF 状态（静默）；实际 PRAGMA 在无事务连接上几乎不可能失败 | 收一下 Result，失败打 LOG_WARN，3 行内可解 |
| 3 | `src/core/sql_dump.cpp` listVirtualTables ｜ low ｜ 虚表判定用 `sql LIKE 'CREATE VIRTUAL TABLE%'`，大小写敏感；小写手写的 `create virtual table` 会漏判，影子表按普通表导出 | 边界：当前 schema 全部经迁移建表、大写，不会触发；触发时回环可能错乱 | 可改 `UPPER(sql) LIKE 'CREATE VIRTUAL TABLE%'`，一行；或记录为已知边界 |
| 4 | `src/core/sql_dump.cpp` listSchemaObjects / listVirtualTables ｜ info ｜ prepare 失败静默返回空集 → dump 缺索引/触发器且无任何报错 | 理论静默失败；两条 SQL 均为静态字面量，实际不可能 prepare 失败 | 可不修；如顺手可打日志 |
| 5 | `tests/test_core/test_sql_dump.cpp` ｜ info ｜ 实现者未提供「先红后绿」变异证据（仅声称测试钉死顺序） | 断言非恒真系静态推得：回环逐字节比对 + `audit_log=3` 行数断言 + 段位置断言，回归任一（触发器前移/影子表误判/自动索引导出）必然变红 | 主会话合并前如需硬证据可做一次变异复跑；不阻断 |

未发现 medium 及以上问题。有意偏离与边界均属实并已核实：触发器在 INSERT 后（spec 已回写 `1d58d76`）；FTS 结构判定/autoindex 排除/剥事务外壳/FK 临时关闭与代码一致；视图与 INSTEAD OF 触发器不处理、skip 表不进 dump 均在头文件与 commit message 写明。

## 六维度结论

1. **正确性**：已核。F2 = 索引/触发器导出（`DROP IF EXISTS` 幂等、autoindex 因 `sql` 为 NULL 正确排除、触发器在 INSERT 后）+ 回环测试逐字节比对，真解决 schema 静默退化；F4 = `defaultSkipTables()` + 用例锁死清单。剥事务外壳只剥首 BEGIN/末 COMMIT、尾巴保留执行，符合「不静默截断」声明。
2. **回归面**：已核。`sql_dump` 在 src/ 下零既有调用方（仅自头文件/实现 + 新测试引用），签名与默认参数未变，`tests/CMakeLists.txt` 仅加一行；无既有迁移/备份路径可破坏。
3. **平台对称性**：已核。全部走标准 SQLite C API 与 `foreign_keys`/`exec`/`prepare`，无平台分支、无新增编译期依赖；dump/导入语义跨平台一致（同库同行为），Windows MSYS2 sqlite 无差异风险点。
4. **静默失败面**：基本已核。导入失败路径返回 `Result::fail` + 整体回滚，不吞错；仅 finding 2/4 两处低危静默点（PRAGMA exec 丢弃、prepare 失败返空），实际不可达。
5. **测试有效性**：已核（复跑 5/5 绿）。五个用例断言均为内容/顺序/行数级，无恒真假绿；`triggerPos > firstInsert` 的 npos 真值风险已被前置 presence 断言覆盖。变异级「先红后绿」未实测（finding 5）。
6. **文档口径**：已核（除 finding 1）。头文件契约注释（导出四段序、导入前置条件「无打开事务」、skip 表不进 dump、autoindex/影子表排除理由）与实现逐条一致；触发器顺序注释与 spec 修订版一致。

## 总裁决

**放行合并**。零 medium+ findings；finding 1（spec F4 回写）建议合并时顺手处理，2/3 可作为后续小修或不修。
