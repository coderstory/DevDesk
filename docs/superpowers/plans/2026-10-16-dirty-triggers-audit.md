# dirty 触发源再补齐 只读审计 — `worktree-agent-dirty-triggers`（基于 `3fbc601`，HEAD `a5e60da`）

- 日期：2026-10-16 ｜ 审计方式：只读复核 + 独立复跑（不复用实现方声明）
- 复跑：`DEVDESK_TEST_TMPDIR=/tmp/devdesk-audit-dirty-triggers bash scripts/build-test-run.sh --no-run --filter todo_ui`（`Total 53 / Passed 53 / Failed 0`）
  与 `--filter markdown`（`Total 93 / Passed 93 / Failed 0`）；3 个相关用例再单跑直验（见 §7）
- 裁决：**放行** —— 功能正确、7 个 emit 点全在成功分支、3 用例全绿；附 1 项必补断言（`doSetColumnColor`，见 §5）+ 2 项文档口径提醒（§3、§6）

## 逐项结论

| # | 核查点 | 结论 | 证据 |
|---|---|---|---|
| 1 | 6/7 个新 emit 点位置 | ✅ | 全部在成功分支，失败先 return；CASCADE 语义成立 |
| 2 | 事件命名与语义 | ✅ | 与既有 `kXxx` / `"mod.verb"` 同风格；共用名歧义已在头文件声明 |
| 3 | R-4 隔离 | ✅（附口径提醒） | `kEventNames` 仍 3 名；subscribe 点**实为 3 处非 2 处**，但新增事件不影响第三方 |
| 4 | 订阅清单 11 ↔ emit 点 | ✅ | 8 旧 + 3 新，1:1 覆盖无遗漏/多余 |
| 5 | 测试有效性 | ✅（附 1 缺口） | 断言内容级 + 负对照；`doSetColumnColor` emit **无任何用例覆盖** |
| 6 | 提交卫生 / 主树还原 | ✅ | 7 文件只含声明改动；主树 `git status` 干净、停在 `3fbc601` |
| 7 | 复跑 | ✅ | 见下实数 |

## 1 · emit 位置正确性 — ✅

**数目**：任务书写「6 个新 emit 点」，实为 **7 个 emit 调用点**（`markdown_module.cpp` 2 处 + `todo_module.cpp` 5 处）对应 3 个新事件常量。以下逐点核。

**笔记（2 处，均 `service_` 成功后才 emit）**：
- `src/module_markdown/markdown_module.cpp:3137` `kMdCreated` —— 位于 `commitNewNote` 尾部。上游两处早退在 emit 之前：已存在名 → 直接 `openNote` 后 `return`（:3102-3108，**不发**）；`service_->create` 失败 → `return`（:3126-3130）。✅
- `src/module_markdown/markdown_module.cpp:3518` `kMdRenamed` —— 位于 `commitRenameNote` 尾部。空名 / 未就绪 / 非法路径 / `service_->rename` 失败四条早退均在 emit 前 `return`（:3480-3507）。✅

**待办列（5 处，全在 `r.isFail()` 早退之后）**：
- `todo_module.cpp:192` `doCreateColumn`、`:296` `doRenameColumn`、`:309` `doDeleteColumn`、`:321` `doSetColumnColor`、`:347` `doShiftColumn`。每个函数 `service_->*` 返回 `isFail()` 即 `return`/返回错误串，emit 在其后。✅
- `doShiftColumn`（:322-347）另有 no-op 早退：`idx>=size || j==idx` → `setStatus("已经在最左边/右边了")` + `return`（:333-336），**不发**。✅

**CASCADE 语义（删列）**：`doDeleteColumn` 的 emit 在 `service_->deleteColumn` 成功、`reloadAll()` 之后。`deleteColumn` 执行 `DELETE FROM todo_columns WHERE id=?`（`todo_service.cpp:326-341`，自动提交）；列下任务的连带删除由外键保证：`src/core/schema.h:70` `FOREIGN KEY (column_id) REFERENCES todo_columns(id) ON DELETE CASCADE` + `src/core/sqlite_db.cpp:272` `PRAGMA foreign_keys = ON;`。故 emit 覆盖的正是「列 + 其下任务」这一次真实数据变更。✅

**次要观察（非阻塞）**：`deleteColumn` **未**做 `sqlite3_changes()==0 → NotFound` 检查（对照 `renameColumn` `todo_service.cpp:300-302` 有该检查）。若传入不存在的 id，`deleteColumn` 仍返回 ok → `doDeleteColumn` 会发一次**空事件**（虚假 dirty）。UI 调用点（`todo_ui_dialogs.cpp:330`）始终传真实列 id，实际不可达；仅记录。

## 2 · 事件命名与语义 — ✅

- 常量名 `kTodoColumnChanged` / `kMdCreated` / `kMdRenamed` 与既有 `kTodoCreated` / `kMdSaved` / `kMdDeleted` 同风格；字符串值 `todo.column_changed` / `md.created` / `md.renamed` 与 `todo.created` / `db.import_begin` 同风格（下划线）。✅
- 5 条列路径共用一个名字：`src/core/event_bus.h:35-38` 常量注释已显式声明「删除列也是任务数据变更的触发源，防后续按名字推断覆盖面时漏掉」；`todo_ui` 用例注释亦复述「列操作共用 `todo.column_changed`」。**歧义风险已被文档钉住**——当前无订阅方需区分五种列改动，可接受。

## 3 · R-4 隔离 — ✅（附口径提醒）

- `src/module_agent/agent_module.cpp:244-246` `kEventNames` 仍为 3 名（`kTodoCreated` / `kTodoCompleted` / `kMdSaved`），未随新增事件扩张 → 事件型 agent 任务不会被新事件意外触发。✅
- `EventBus::subscribe` 按名精确匹配（`event_bus.h`），新事件对既有订阅方恒为 no-op。✅
- ⚠️ **口径提醒**：提交信息称「全仓 `bus().subscribe` 只有 settings_module 与 agent_module 两处」——**实为 3 处生产订阅点**：`src/module_settings/settings_module.cpp:186`、`src/module_agent/agent_module.cpp:476`、**`src/module_claude_proxy/claude_module.cpp:313` 与 `:319`**。第三方（claude_module）只订 `kDbImportBegin` / `kDbImportEnd`，**不受新增事件影响**，故结论（新事件不惊动既有订阅方）仍成立；仅「两处」这一措辞不准。其余 subscribe 命中全在 `tests/`。

## 4 · 订阅清单 ↔ emit 点 — ✅

`src/module_settings/settings_module.cpp:179-185` 循环订阅 **11 个**事件：4 个笔记（`kMdSaved`/`kMdCreated`/`kMdRenamed`/`kMdDeleted`）+ 6 个待办任务（`kTodoCreated`/`Updated`/`Moved`/`Completed`/`Uncompleted`/`Deleted`）+ 1 个列（`kTodoColumnChanged`）= 11。与本次新增 3 个 emit 事件名**一一对应，无遗漏、无多余**。token 入 `githubSyncSubs_`（:2119），`onStop` 于 :227 `clear()` 退订。✅

## 5 · 测试有效性 — ✅（附 1 缺口）

- **非恒真（内容级断言 + 负对照）**：`test_settings.cpp:3971-3983` 逐个事件名验证「置 dirty」，并以 `kDbImported` 作**无关事件负对照**（`!dirtyAfter(kDbImported)`）；`test_markdown.cpp` 新用例以「已存在名直接打开」作**不发 `md.created`** 的负对照（`DOCTEST_CHECK_EQ(gotCreated, 0)`）再断言真新建恰 1 次；`test_todo_ui.cpp` 新用例每步断言计数 `1/2/3/4` 且校验列数、列名、CASCADE 后任务数=0。均依赖 emit，emit 回退即红。✅
- **先红后绿可信度**：回退 emit 会使 3 个用例红——`md_module_create_rename_event_wiring`（`gotCreated`/`gotRenamed` 恒 0）、`todo_ui_event_wiring_columns`（`gotColumn` 恒 0）、`settings_github_sync_dirty_from_write_events`（新事件不置 dirty）。与实现方「3 处红」声明一致，逻辑可信（本审计为只读，未实际回退复跑）。
- ⚠️ **缺口：`doSetColumnColor` 的 emit 无任何用例覆盖**。新 `todo_ui` 用例只走了 建列 / 改列名 / 移列 / 删列 **4 条**路径，未覆盖第 5 条 `doSetColumnColor`（`todo_module.cpp:321`）。settings 侧用例只验证「事件名 → dirty」，不验证「改色操作 → 发事件」。按项目纪律「新增/修改的行为必须有断言，没有断言的改动视为未完成」，此项**应补一条改色断言**（右键列头 → 改色子选 → 断言 `gotColumn` 再 +1）。因 emit 代码本身正确、且事件名已被端到端覆盖，判为非阻塞、**放行后必补**。
- 另：`doShiftColumn` 的 no-op 分支（下标 0 左移不发事件）仅以注释声明，无用例钉死——同属可补的次要项。

## 6 · 提交卫生 / 主树还原 — ✅

- `git diff 3fbc601 a5e60da --stat` = **7 文件 +301/−7**，与声明一致：`event_bus.h`、`markdown_module.cpp`、`settings_module.cpp`、`todo_module.cpp`、`test_markdown.cpp`、`test_settings.cpp`、`test_todo_ui.cpp`。逐 hunk 读毕，**无夹带**（无 spec/backlog/CLAUDE.md、无无关重构、无格式噪声）。worktree `git status --short` 干净。
- **主树还原**：`/Users/coderstory/dev/gui/DevDesk` 在分支 `dev`、HEAD `3fbc601`；审计开始时 `git status --short` **空** → 隔离改动**未泄漏**进主树。`git merge-base a5e60da 3fbc601` = `3fbc601`（基线正确）。实现方所称「隔离未生效、手动迁出」的最终状态已干净，无可复核的残留。
- **旁注（与本提交无关）**：审计期间主树 `scripts/build-test-run.sh` 出现一处**外部并发改动**（mtime 2026-10-16 13:04，给 `DEVDESK_TEST_TMPDIR` 加默认导出），**不属 `a5e60da`**（该提交不触此文件），worktree 内副本无此改动。记录以免误判为主树残留。

## 7 · 复跑实数 — ✅

| 命令 | 结果 |
|---|---|
| `build-test-run.sh --no-run --filter todo_ui` | `Total: 53 Passed: 53 Failed: 0` ✅ |
| `build-test-run.sh --no-run --filter markdown` | `Total: 93 Passed: 93 Failed: 0` ✅ |
| `devdesk_tests "column_changed"`（新列用例） | `[PASS] todo_ui: 建列/改列名/移列/删列→todo.column_changed（删列级联任务也置位）` ✅ |
| `devdesk_tests "md.created"`（新笔记用例） | `[PASS] markdown_module: 新建→md.created、重命名→md.renamed；已存在名直接打开不发（headless）` ✅ |
| `devdesk_tests "各写路径事件"`（settings 消费侧） | `[PASS] settings: 笔记/待办各写路径事件 → GitHub Sync 置 dirty（带无关事件对照）` ✅ |

（过滤器为**描述子串**匹配，非通配符；`todo_ui` / `markdown` 分别命中 53 / 93 例，全绿。）
