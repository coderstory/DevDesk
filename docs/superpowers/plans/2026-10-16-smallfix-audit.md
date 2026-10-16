# 审计：worktree-agent-smallfix 三提交（agent_runs 部分索引 / 手动首次同步补校验 / putFileOnBranch 委托）

- 对象：分支 `worktree-agent-smallfix`（worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-smallfix`）
  - `594d830` perf(agent): agent_runs 加 status 部分索引（schema.h + test_schema.cpp）
  - `b162e9c` fix(settings): 手动「立即同步」补跑首次同步三步校验（settings_module.cpp）
  - `4090e7a` refactor(platform): putFileOnBranch 委托 putFile（github_api.cpp + github_api.h）
- **基订正**：简报写「基 `cb71f74`」，实际分支从 **`586d195`** 切出（`cb71f74` 是 `586d195` 的后代、在 dev 上）。差 1 个提交。`git merge-tree --write-tree 589d8f5 HEAD` = 干净无冲突（dev 侧该区间唯一动到被审文件的 `68de02a` 改 `settings_module.cpp:14/129-141`，与本批 `b162e9c` 的 `:107/602/888+` 区域不相交）。
- 审计者：独立只读 agent（非实现者），2026-10-16
- 手段：三提交全 diff 精读 + 关联源码核对（schema 全索引清单 / agent_store 三处 running 查询 / settings 失败呈现与 publicRepo 语义 / github_api PUT 体组装）+ **sqlite3 查询计划实测**（`EXPLAIN QUERY PLAN`）+ worktree 复跑
- 复跑证据（`DEVDESK_TEST_TMPDIR` 必须为**绝对路径**，否则见 finding 1）：`DEVDESK_TEST_TMPDIR=$(pwd)/build/testtmp ./build/tests/devdesk_tests <filter>`
  - `github` → `Total: 64 / Passed: 64 / Failed: 0`；`agent` → `92/92/0`；`schema` → `16/16/0`；`settings` → `118/118/0`。四条与简报声称的 64/92/16/118 逐项吻合。

## Findings

| # | 位置 | severity | 问题 | 建议 |
|---|---|---|---|---|
| 1 | 复跑口径（`tests/doctest.h:126-141` + `scripts/build-test-run.sh:442-446`） | info | 直接跑二进制而不经脚本时，`DEVDESK_TEST_TMPDIR` 未设 → 临时基目录退化成**相对** `build/testtmp` → `DataRootEnv` 把 `DEVDESK_DATA_ROOT` 设成相对路径 → `test_claude_service.cpp:2991` 的 `settingsFile.is_absolute()` 红（`settings` filter 变 117/118）。经脚本（脚本导出绝对 `DEVDESK_TEST_TMPDIR`）即 118/118。**该用例与三提交无关，且在同一 dev `589d8f5` 上同样红**（已实测），非本批回归 | 复跑一律走 `scripts/build-test-run.sh`（CLAUDE.md 既有铁律）或手动导出绝对 `DEVDESK_TEST_TMPDIR`；本 finding 只提示口径，不改代码 |
| 2 | `schema.h:335-342`（注释） | info | 注释写「按 status 过滤的**全库**只有两处」。实为 **3 处**含 `status='running'`：`hasRunningRun`（:466）、启动清扫（:532）、**`startRunExclusive`（agent_store.cpp:191，`WHERE task_id=? AND status='running'`）**。第三处是 task 域过滤故不属「全库」，措辞成立，但 EQP 实测它**也命中**新索引（见下）——注释可顺带点一句「三处查询都受益」 | 非阻断；下次注释批次顺带补一句即可 |
| 3 | `settings_module.cpp:893-895`（manual FirstSync 路径） | info | 手动「立即同步」的 FirstSync 现走 `runGithubSyncCheckedFirstSync` → 也置 `publicRepo` → `githubSyncRepoPublic_`（**只增不减的闩**）→ 传给 UI（:1893）。**用户可见变化**：手动首次同步到 public 仓后，黄字「public 仓」提醒**现在会出现**（此前仅 Test 动作会置）。行为与 Test 动作一致、语义合理，但 commit message 只写了「补校验」未提这条 | 非阻断；如需可在 commit 说明补一句 |
| 4 | `settings_module.cpp:1150/1190`（out-of-scope） | info | `putFile` 新增公开 `branch` 参数，但同步推送的两处 `putFile` 调用**仍不传 branch** → PUT 落**仓库默认分支**，而同函数内 `getFile(?ref=branch)` / `listTree(branch)` / `createRef(branch)` 全按**配置分支**读。仅当 `cfg.branch`（默认 `"main"`，`github_sync_config.h:20`）≠ 仓库默认分支时才分叉。**纯属既有行为**（旧 `putFile` 本就无 branch 参数），本次 refactor 未引入也未修复 | 与三提交裁决无关；建议另开条目单独核（本审计不判其真伪，只登记） |

无 blocker、无 major。

## 核查点逐条

### 1. 索引（`594d830`）— 已核，通过

- **判据与查询匹配**：`hasRunningRun()`（`agent_store.cpp:464-475`）= `SELECT 1 FROM agent_runs WHERE status='running' LIMIT 1`；启动清扫 `sweepStaleRuns()`（`:529-534`）= `UPDATE agent_runs SET … WHERE status='running'`。二者 WHERE 均**蕴含**索引 `WHERE status='running'`（SQLite 部分索引的「查询 WHERE 蕴含索引 WHERE」规则），命中成立。
- **sqlite3 实测**（建旧表 → 跑 `CREATE INDEX IF NOT EXISTS … WHERE status='running'`）：
  - `EXPLAIN QUERY PLAN SELECT 1 … WHERE status='running' LIMIT 1` → `SEARCH agent_runs USING COVERING INDEX idx_agent_runs_running (status=?)`（**covering**，索引即含所需列，零回表）
  - `EXPLAIN QUERY PLAN UPDATE … WHERE status='running'` → `SEARCH agent_runs USING INDEX idx_agent_runs_running (status=?)`
  - 附：`… WHERE task_id=1 AND status='running' LIMIT 1`（startRunExclusive）**也** → `SEARCH … USING INDEX idx_agent_runs_running (status=?)`（见 finding 2）
- **命名/写法与既有风格一致**：`idx_<表>_<用途>`（同 `idx_agent_runs_task` / `idx_agent_messages_run`），`CREATE INDEX IF NOT EXISTS`，WHERE 部分索引写法与 `idx_tasks_due` 同款。✓
- **test_schema.cpp 索引清单同步属实**：`expectedIndexes` 真加了 `"idx_agent_runs_running"`（:107），非只改注释。计数注释 8→9 与实际 9 条一致。（既有风格只断言**存在**、不断言**数量/无多余**，故「9 个」是文档不是判据——沿用现状，可接受。）
- **无迁移政策下旧库行为**：**简报前提「不补索引」不成立、且实际更好**。`Database::migrate()`（`database.cpp:37-38`）每次启动调 `applySchema(kSql)`（`sqlite_db.cpp:408-421`，整脚本一个事务、DDL 全 `IF NOT EXISTS`）。`CREATE TABLE IF NOT EXISTS` 对已存在表整句跳过 → **旧库不加新列**；但 `CREATE INDEX IF NOT EXISTS` 独立于表 → **旧库会在下次启动补上新索引**（sqlite3 实测：旧表 + 数据 + 该语句 → 索引真被建出）。故性能修复对存量库**回溯生效**，可接受（且优于简报假设）。

### 2. 校验路径（`b162e9c`）— 已核，通过

- **真被两处共用、无遗留旧实现**：`runGithubSyncCheckedFirstSync` 仅一处定义（`settings_module.cpp:916-940`），仅一处调用（`:891-895`，`case ValidateFirstSync:` 与 `case FirstSync:` **fall-through 到同一次调用**）。原 `ValidateFirstSync` 的内联三步校验块已整段删除，无残留；`grep testConnection src/` 在 settings 仅剩两处——Test 动作（:864）与校验函数内（:923）。✓
- **失败呈现按声明不变**：`harvestGithubSync`（`:1647-1652`）`silentFailure = (LastAction_ == ValidateFirstSync)`，弹窗条件 `!= None && manual && !silentFailure`。两路都以 `manual=true` 发起（:591 启用开关 / :612 手动按钮）→ **FirstSync=手动弹窗、ValidateFirstSync=静默卡片红字**，与文件头注释（:110-115）一致。✓
- **「手动路径现在也判 public 仓」用户可见变化**：见 finding 3（闩置位，黄字提醒）。**校验闸本身**是声明内的意图修复——手动首次同步原**跳过**三步校验直接做方向判定（会覆盖本地库或上传全量），现与启用流程同一道闸；失败时 manual 路径弹窗、summary 即失败摘要。方向正确。
- **「无单测」声明属实**：`grep -rn "runGithubSyncAction|startGithubSyncAction|runGithubSyncCheckedFirstSync|runGithubSyncFirstSync" tests/` = 0 命中。`runGithubSyncCheckedFirstSync` 是 impl 类私有方法、不直接可测，也无集成用例驱动。声明属实（覆盖缺口，如实登记，非本批引入）。

### 3. 委托化（`4090e7a`）— 已核，通过

- **body 组装顺序等价**：新 `putFile`（`github_api.cpp:590-607`）顺序 = message → content → branch(非空才 set) → sha(非空才 set)；旧 `putFileOnBranch` = message → content → branch(**恒 set**) → sha(非空才 set)。键集/顺序一致。
- **覆盖语义等价**：四键互不相同，无同名覆盖；`JsonObject::set` 语义未变。
- **唯一行为差 = 空 branch**：旧 `putFileOnBranch` 恒发 `"branch": ""`，新 `putFile` 空 branch **省略该字段**。实测**无影响**：唯一 `putFileOnBranch` 调用点 `testConnection` 探针（:819）传**非空** branch；既有 `putFile` 调用点（settings push ×2，:1150/1190）传 4 参 → branch 缺省 `""` → 省略 → 与**旧 `putFile`** 逐字等价（旧 `putFile` 本就无 branch 字段）。故对**所有现存调用方**功能等价。✓
- **调用方全部更新**：`putFile` 的 2 处调用无需改（语义不变）；`putFileOnBranch` 改为委托、其唯一调用方未变。`grep putFileOnBranch` 仅 `github_api.cpp:658`（委托体）+ `:819`（探针）。头注释（`github_api.h:46-52/79-82`）与新语义一致。✓
- **64 例 github 用例复跑**：64/64 绿。✓

### 4. 提交卫生 — 已核，通过

三提交各只动声明文件：`594d830`={schema.h, test_schema.cpp}、`b162e9c`={settings_module.cpp}、`4090e7a`={github_api.cpp, github_api.h}。无越界、无夹带。

### 5. 复跑 — 见开头证据行（64/92/16/118 全绿，口径见 finding 1）

## 六维结论

1. **正确性 — 已核，通过**。索引判据与三处查询匹配（EQP 实测命中，SELECT 为 covering）；校验闸真被两处共用、无遗留旧实现；委托体与旧实现逐键等价。
2. **回归面 — 已核，通过**。64/92/16/118 实测全绿；`putFile`/`putFileOnBranch` 全部调用点核对无遗漏；merge-tree 对 dev 无冲突。
3. **平台对称性 — 已核，通过**。三提交纯逻辑：零平台分支、零新 include、零 CMake 触碰（SQL/文件系统/JSON 全走既有跨平台层）。
4. **静默失败面 — 已核，通过**。校验失败经 `summary` 上抛、manual 路径弹窗、启用路径卡片红字，无吞异常/无默认值兜底；索引新增无运行期失败路径。
5. **测试有效性 — 已核，通过（附覆盖缺口）**。`schema` 16/16 含索引清单断言（新索引名在列，删索引 → 红）；`github` 64/64 覆盖 putFile 编码/404/冲突等。**缺口**：settings 同步动作链（含新共用校验）**零单测**（已声明、属私有 impl 方法）——既有 `test_github_sync_card.cpp` 只测纯函数 `firstSyncChoice` 与 store，不驱动 `runGithubSyncAction`。
6. **文档口径 — 已核，通过（两条 info 级措辞）**。schema.h「两处」措辞见 finding 2；commit message 未提 publicRepo 副作用见 finding 3。均不改行为、不阻断。

## 行数/功能比

`594d830` +12/−1（9 行索引 + 注释 / 测试 1 行名 + 1 行注释改）；`b162e9c` +39/−31（净 +8，抽一个函数、两路合一）；`4090e7a` +13/−34（净 −21，**删重复 PUT 实现**）。三处均与「一个索引 / 一个共享函数 / 一次去重」一一对应，无超需求实现，符合「避免代码膨胀」口径。

## 裁决

**放行** — 三提交逐条核实成立：部分索引判据与 `hasRunningRun`/启动清扫（附 `startRunExclusive`）三处查询匹配且 EQP 实测命中（SELECT covering）、旧库经 `applySchema` 会**自动补索引**（简报「不补索引」前提不成立、实际更好）；`runGithubSyncCheckedFirstSync` 真被两处共用、无遗留旧实现、失败呈现按声明不变（手动弹窗 / 启用静默）；`putFile` 委托体与旧实现逐键等价、空 branch 差异对全部现存调用方无影响；三提交各只动声明文件。四条 findings **全 info 级**（复跑口径 / 注释措辞 / publicRepo 副作用未写入 commit / out-of-scope 的 push 分支写向），无需回修。
