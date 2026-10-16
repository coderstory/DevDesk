# T5 清理票只读审计 — `worktree-agent-t5-cleanup`（基于 `2db7e38`，HEAD `b446ab9`）

- 日期：2026-10-16 ｜ 审计方式：只读复核 + 独立验证脚本（不复用实现方声明）
- 复跑：`DEVDESK_TEST_TMPDIR=/tmp/devdesk-t5-audit-tmp bash scripts/build-test-run.sh --no-run`
- 裁决：**再修** —— 1 项 🔴（`04161d5` 的修复未生效），其余 7 项放行

## 逐项结论

| # | 项 | 结论 | 证据 |
|---|---|---|---|
| 1 | `fa80dfa` 注释清扫零代码改动 | ✅ | 见下「双法验证」 |
| 2 | `63950b1` dirty 触发源 | ✅（附残余缺口） | 见下 |
| 3 | `a651015` autoPush | ✅（附建议） | 见下 |
| 4 | `c5d41dc` retry attempts | ✅ | 见下 |
| 5 | `8f39a9f` 首次启用校验 | ✅ | 见下 |
| 6 | `04161d5` pruneBackups 显形 | ❌ | 见下 |
| 7 | 全量复跑 + 守卫 | ✅ | `Total: 1426 Passed: 1420 Failed: 0`；UI probe 7/7；6 道守卫自检全过 |
| 8 | 提交卫生 | ✅ | 8 提交各只动声明文件；无 spec/backlog/CLAUDE.md 夹带 |

## 1 · `fa80dfa` 注释清扫 — ✅ 双法独立验证

**法 A（词法器）**：自写 C++ 注释剥离器（处理字符串/字符字面量/原始字符串/数字分隔符 `1'000'000`），
对 20 个文件逐一剥注释 + 去空行 + 逐行 `rstrip` 后比对父提交 → **20/20 文件代码体逐字节相同，0 差异**。

**法 B（行配对，不依赖词法器）**：直接解析 git 自身 diff，取 `-`/`+` 行各 105 条，
逐对比较 `line.split('//')[0].rstrip()` → **105 对，代码部分 0 处不符**。

**结构佐证**：98 个 hunk 全部 `+/-` 配平；20 个文件行数父/子完全一致 →
**没有任何注释被整行删除**，全部是 1:1 替换。

**必留项核查**（逐条读了全部 105 对）：删掉的全是纪律定义的「必删」三类 ——
出处与证据（`决策 #N` / `spec F#` / `审计 finding N` / `A#·C# 复审` / `T3 审计`）、
演变叙事（「原先只进日志」「修前本用例红」「T3 时代的旧记录」「C5 后不再逐篇 GET」）、
验证体系自解说（「test_sql_dump 的 sql_dump_import_rollback 钉死」）。
**规则/契约内容在每一对里都被保留或改写为直接陈述**，无一处删规则。
反重构警告与跨文件不变量仍健在（抽查：`app_assembly.h:2`「装配顺序（结构保证，勿改）」、
`proxy_runtime.h:3`「停机顺序不可变」、`test_sql_dump.cpp:6`「触发器若被挪回 INSERT 之前建…」、
`todo_dialogs.h:3`「元素 id/文案/几何/控制流是无头 UI 用例契约」）。
告警符号：105 对里无一处新增/删除 ⚠️🔴 类符号（原本就没有）。

## 2 · `63950b1` dirty 触发源 — ✅（声明范围全达成，另有残余缺口）

- **命名一致**：`kTodoUpdated` / `kTodoMoved` / `kTodoUncompleted` / `kTodoDeleted` / `kMdDeleted`
  与既有 `kTodoCreated` / `kMdSaved` 同风格；字符串值 `todo.*` / `md.*` 同风格（`src/core/event_bus.h:29-37`）。
  新增恰 5 个 ✅（与提交声明一致）。
- **订阅扩到 8 覆盖声明范围**：`src/module_settings/settings_module.cpp:180-189` 循环订阅 8 名，
  含 `kMdDeleted`（笔记删）+ `kTodoCreated/Updated/Moved/Completed/Uncompleted/Deleted`（待办增删改）✅。
  捕获的 `ev` 指向字符串字面量（静态存储期），无悬垂。
- **scheduler 不受影响 ✅ 核实**：`src/module_agent/agent_module.cpp:244-248` 的 `kEventNames` 仍是旧 3 名
  （`kTodoCreated`/`kTodoCompleted`/`kMdSaved`）；`EventBus::subscribe` 按名精确匹配（`event_bus.h:85`），
  新事件对它恒为 no-op。
- **todo 4 个写路径 emit 位置正确**（全在成功分支、失败路径先 return）：
  `todo_module.cpp:204`（move，198 失败即 return）/ `:227`（uncomplete，223 失败即 return）/
  `:268`（delete，262 失败即 return）/ `:281`（update，275 失败即 return）✅。
- **md 删除**：`markdown_module.cpp:3466` 在 `remove()` 成功后才 emit（3443 失败已 return）；
  用例钉死「取消路径 0 次、确认成功恰 1 次」（`test_markdown.cpp`）✅。
- ⚠️ **残余缺口（超出本提交声明范围，建议转票）**：
  - 笔记**重命名** `commitRenameNote`（`markdown_module.cpp:3469-3517`）无任何 emit；
  - 笔记**新建** `commitNewNote`（`markdown_module.cpp:3073-3137`）无 emit；
  - 待办**列**写路径全部无 emit：`createColumn:186` / `renameColumn:289` / `deleteColumn:300` / `reorderColumns:337`
    —— 其中 `deleteColumn` 会级联删任务，是实打实的库变更却不置 dirty。
  这些改动都写进 `database.sql` / 远端笔记布局，漏置位 = 那类改动不会自动同步（正是本票要消灭的失败模式）。

## 3 · `a651015` autoPush — ✅（真值表正确；退出补推收口有可观测性缺口，给建议）

- **真值表**：`gsync::autoPushDue`（`github_sync_logic.cpp:50-54`）= `enabled && autoPush && dirty && throttleDue`。
  纯函数、无副作用、已单测（4 个假值分支 + 恰好 `kCommitThrottleSec` 的边界）✅。
- **手动路径不受限 ✅**：`githubSyncPush`（`settings_module.cpp:598`）/ `Test`（`:615`）都不查本闸。
- **退出补推同口径**（`settings_module.cpp:710-713`）：判断 —— **收口本身合理，但当前实现丢掉了 F5 的另一半**。
  - 合理面：退出补推不是用户显式动作，属自动路径；不闸的话「自动推送」开关就是句谎话。
  - 问题面：`setUnsyncedNotice()` **只在 `waitOrDetachSyncThread()` 里调用**（`settings_module.cpp:743-747`），
    而开关关闭时在 `:710` 就早退了，根本到不了那里。后果：用户关掉自动推送、带着 dirty 退出 →
    下次启动**既没推、也没有「有未同步改动」黄字**，F5 想给的可观测性信号一起没了。
  - **建议**（约 3 行）：早退前补一次 `if (enabled && !autoPush && dirty) setUnsyncedNotice();`，
    即「尊重用户关自动推送的选择，但仍告诉他还有未同步改动」。若团队认为 F5 的退出补推是不可关的安全网，
    则替代方案是只闸 `maybeAutoGithubSync`、退出补推不闸，并在配置项文案里写明该开关不覆盖退出补推。
    两者择一即可，**不建议维持现状**（关掉开关 = 静默丢信号）。

## 4 · `c5d41dc` retry — ✅

- 语义与实现对齐：`attempts` = 总请求数，默认 4（`github_api.h:25-27` 注释同步写明）。
  循环 `for (int i = 0; i < attempts; ++i)`（`github_api.cpp:362`），退避只在 `if (i + 1 < attempts)` 时执行
  （`:398`、`:412`）→ **4 次请求 / 3 次 sleep / 退避 1000·3000·9000 全部用上，无 off-by-one** ✅。
  `backoffFor` 用 `min(idx, size-1)` 复用末项，与注释一致（`:334-339`）。
- 新增的 `github_default_policy_three_retries` 用例补上了真正的缺口 ——
  旧用例覆写了 policy，只钉循环语义，默认值退回 3 也不会有用例红（这正是缺陷存活的原因）✅。
- spec 口径已回写：主树 `docs/superpowers/specs/2026-10-15-github-sync-api-design.md:90`
  现为 `attempts = 4`（主会话 `1352875`）✅。

## 5 · `8f39a9f` 首次启用 — ✅ 无死态

- `SyncAction::ValidateFirstSync`（`settings_module.cpp:877-902`）：先 `testConnection` 三步，
  `!report.ok` → `summary = report.summary; break;`（`ok` 保持 `:837` 的初值 false），
  过了才 `runGithubSyncFirstSync`，成功才 `ok = true` ✅。
- **失败后可重试，状态机不留死态** ✅：`setFirstSyncDone()` 只在成功路径调用（`:1606-1613`），
  失败后 `first_sync_done` 仍为假。三条重试路：
  ① 重拨启用开关 → `maybeStartFirstGithubSync`（`:586`）；
  ② 手动「立即同步」→ `githubSyncPush` 见 `!firstSyncDone` 改跑 `FirstSync`（`:601-608`）；
  ③ 卡片「详情」弹窗的「重试」→ `retryGithubSync` 按 `githubSyncLastAction_` 重跑 ValidateFirstSync（`:625-631`）。
- 与既有 FirstSync 状态清理兼容：`clearGithubSyncFirstSyncFlag` 仍在 repoUrl/branch 变更时调用（`:1805`、`:1811`）✅。
- harvest 接线完整：`pushedToRemote`（`:1583-1587`）与 `setFirstSyncDone`（`:1606-1607`）都补了 ValidateFirstSync ✅。
- 失败不弹窗（`silentFailure`，`:1629-1633`）符合「拨开关瞬间不打断」✅。
- ⚠️ 小提示（非阻塞）：路径 ② 会**跳过校验**直奔 FirstSync（`githubSyncPush:601-608`）。
  属用户显式动作、且 FirstSync 自身仍会因坏 token 失败，可接受；若要严格「先校验后动数据」，
  可让 `githubSyncPush` 在 `!firstSyncDone` 时也走 `ValidateFirstSync`。

## 6 · `04161d5` pruneBackups — ❌ 修复未生效（ec 仍被吞）

**结论：新增的失败分支是死代码，提交声称的「显形」没有达成。**

`src/module_settings/github_sync_logic.cpp:108-116`：

```cpp
const auto& victim = found[static_cast<size_t>(i)];
if (std::filesystem::remove_all(victim, rmEc) > 0) {
    ++removed;
} else if (rmEc) {
    LOG_WARN("GithubSync: 滚动删除备份失败 " + victim.string() + ": " + rmEc.message());
}
```

`std::filesystem::remove_all` 的错误返回是 `static_cast<uintmax_t>(-1)`（即 18446744073709551615），
**它 > 0 恒为真** → `else if (rmEc)` 永远不可达，WARN 永不打印；同时 `++removed` 把失败也计成成功。

**实证（本机 macOS/libc++ = 本项目主平台，独立小程序复现真实场景）**：

```
[全失败] base 只读、删 victim/  → remove_all returned=18446744073709551615 (= -1)  ec=13 Permission denied  → n > 0 为 TRUE
[部分失败] victim/locked 只读、删 victim/ → remove_all returned=-1  ec=66 Directory not empty（a.txt 其实已被删）
```

两种失败都返回 -1，两种都落进 `> 0` 分支。即：本提交把 `remove_all` 的 ec 从「丢弃」改成了
「用一个恒真的比较挡在前面」，ec 依然没有被观察到。

**修法**（`src/core/db_backup.cpp:170-175` 的 core 版就是这个正确写法，可直接照抄判据顺序）：

```cpp
const std::uintmax_t n = std::filesystem::remove_all(victim, rmEc);
if (rmEc) {
    LOG_WARN("GithubSync: 滚动删除备份失败 " + victim.string() + ": " + rmEc.message());
} else if (n > 0) {
    ++removed;
}
```

**无测试兜住**：两个 prune 用例（`test_modules/test_github_sync_logic.cpp:87`、
`test_modules/test_github_sync_card.cpp:543`）只覆盖快乐路径（7−5=2 / 8−5=3），
失败分支零覆盖 —— 这也是死代码能存活的原因。修完建议补一条「把待删目录置为不可删 → 断言 removed 不涨 + 日志有 WARN」。

**「未委托 core」的理由 ✅ 成立**：`core::dbmaint` 版 `pruneFilesWithPrefix`
（`src/core/db_backup.cpp:139-179`）用 `fs::remove`（**删不了目录**，而笔记备份恰恰是目录）、
首错即 `fail` 返回 `Result`（vs gsync 的「返回删除数 + 继续」）。语义确实不同，委托会改掉笔记备份的清理契约。

**次要**：同提交补的「扫描中断 WARN」对**循环中途**失败有效（`it.increment(ec)` 后下一轮顶部检查），
但 `directory_iterator it(dir, ec)` **构造失败**时循环体一次都不进（`it == end`），仍无 WARN。
此时 `found` 为空 → 不删任何东西 → 备份超保留数增长，仍是静默面。可顺手补。

## 7 · 全量复跑 — ✅

```
Total:  1426 Passed: 1420 Failed: 0      ✅ 测试全绿
UI probe: 100% tests passed out of 7     ✅
6 道守卫自检全过（好树绿 / 坏树各红 —— 判据非恒真）✅
```

`Total − Passed = 6` = 按平台 `DOCTEST_SKIP`，非失败。与实现方声明一致。
新增 5 条用例（`github_sync_auto_push_due` / `github_default_policy_three_retries` /
`settings_github_sync_autopush_switch_gates_auto_path` / `settings_github_sync_dirty_from_write_events` /
`todo_ui_event_wiring_write_paths`）与提交一一对应 ✅。

## 8 · 提交卫生 — ✅

8 个提交逐个 `git show --name-only`，全部只动声明范围内的文件；
`fa80dfa` 恰 20 文件（与声明一致）；无 spec / backlog / CLAUDE.md / 构建脚本夹带 ✅。

## 处置建议

1. **必改**：`04161d5` 的 `remove_all` 判据顺序（先查 `rmEc`）—— 现版本等于没修，且无测试兜住。建议连带补失败路径用例。
2. **建议改**：`runExitSyncPush` 早退前补 `setUnsyncedNotice()`（保住 F5 的「有未同步改动」信号）。
3. **转票**：dirty 触发源残余（笔记重命名/新建、待办列写路径）；prune 的 `directory_iterator` 构造失败静默面。
