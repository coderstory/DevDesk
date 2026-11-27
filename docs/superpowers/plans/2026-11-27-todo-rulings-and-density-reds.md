# 待办裁决收口 + 密度几何红 + devflow 门禁装机 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把已写的 B-16/C-13 裁决改动验证收口、修掉远端密度提交造成的 9 条本机几何红，并在 DevDesk 常驻启用 devflow 门禁（T1/T3 + 提交归属+证据）。

**Architecture:** 代码改动只在测试侧（断言改读 `ui_kit::density`，保持精确相等）；产品代码不动。门禁装机是仓内配置 + 本地 git config + `.devflow/context.json`，不改产品行为。

**Tech Stack:** C++20 / doctest / `scripts/build-test-run.sh`（唯一构建入口）/ devflow 0.42.0 git hooks。

**Spec:** `C:\Users\e-Yunfei.Qian\dev\zcode-plugins\docs\superpowers\specs\2026-11-27-gate-flow-e2e-design.md`（§7 装机、§8 剩余开发）

## Global Constraints

- 构建/测试唯一入口：`bash scripts/with-build-lock.sh bash scripts/build-test-run.sh …`；禁直调 cmake/ctest（probe 的 `ctest -L probe` 例外）。
- 开发阶段只编码；修复阶段只跑定向 `--filter`；批次末一次全量（密度属平台底座，允许例外）。
- 断言必须精确（`CHECK_EQ`/`nearEq`），不得改容差/跳过用例掩盖。
- 不改 EUI-NEO、third_party gitlink、tmp/。
- 门禁启用后，DevDesk 代码提交必须带 `[pool:need:N]` 或 `[pool:bug:N]` + `Tests:` + `Evidence:`（证据文件存在、非空、不早于改动）。

## Review Focus

- 密度值在 100%/125% DPI 下是否都取整（4 的倍数）——断言不得依赖浮点巧合。
- `density_posix.cpp` 必须逐字保持改动前值（mac 零变化）。
- 门禁 hook 在 Windows Git for Windows 的 sh 下能否执行（路径含空格、CRLF）。
- 已绑定后 T1 不能再拦（防止每天第一次编辑被打断）。
- T3 证据时间戳比较在 Windows `stat -c %Y` 不可用时的回退。

---

### Task 1: 验证并提交 B-16 / C-13 裁决改动

**Files:**
- Modify: `src/module_todo/todo_module.cpp`（已改，未提交）
- Modify: `tests/test_modules/test_todo_ui.cpp`（已改，未提交）
- Modify: `docs/USER_GUIDE.md`（已改，未提交）

**Interfaces:**
- Consumes: `doUncompleteTask` → `finishCompletionMove(taskId, "doing")`；`onKeyEvent` 新增 ⌘/Ctrl+N/D/]/[。
- Produces: 无新接口；行为对后续任务无依赖。

- [ ] **Step 1: 跑定向用例**

Run: `bash scripts/with-build-lock.sh bash scripts/build-test-run.sh --dev --filter uncomplete_and_shortcuts`
Expected: `Total: 1 Passed: 1 Failed: 0`（用例名 `todo_ui_uncomplete_and_shortcuts`）

- [ ] **Step 2: 跑待办回归**

Run: `bash scripts/with-build-lock.sh bash scripts/build-test-run.sh --dev --filter todo`
Expected: `Total: 186 Passed: 186 Failed: 0`

- [ ] **Step 3: 提交（门禁启用前，用 wip 检查点）**

```bash
git add src/module_todo/todo_module.cpp tests/test_modules/test_todo_ui.cpp docs/USER_GUIDE.md
git commit -m "feat: 待办裁决落地（取消完成回进行中 / 归档可拖出 / 页内快捷键）"
```

---

### Task 2: 修 9 条密度几何红（测试侧读 density）

**Files:**
- Modify: `tests/test_modules/test_markdown.cpp`（硬编码 32/40 的几何断言）
- Modify: `tests/test_modules/test_settings.cpp`（`510.0f` 与列宽基准）

**Interfaces:**
- Consumes: `ui_kit::density::pageInsetV()`（Win 24 / posix 32）、`ui_kit::density::headerH()`（Win 36 / posix 40）、`ui_kit::density::cardGap()`。
- Produces: 无。

- [ ] **Step 1: 复现并记录全部失败断言**

Run: `bash scripts/with-build-lock.sh bash scripts/build-test-run.sh --dev --filter file:test_markdown.cpp,file:test_settings.cpp`
Expected: 9 条 FAIL，逐条抄下 `assert:` 行与期望/实得值到任务记录。

- [ ] **Step 2: 把测试里的页内边距与页头常量换成 density 调用**

规则（只改期望值来源，不改公式与容差）：
- `32.0f` 作为页内边距（pad）→ `ui_kit::density::pageInsetV()`；
- `40.0f` 作为页头高 → `ui_kit::density::headerH()`；
- 由 pad 派生的 `contentW` / `midW` / 列宽期望沿用现有公式，只把 pad 变量换源；
- 明确不是页内边距的 `32.0f`（字号/坐标/内距）保持不变——逐处看上下文再改。

- [ ] **Step 3: 跑定向用例**

Run: `bash scripts/with-build-lock.sh bash scripts/build-test-run.sh --dev --filter file:test_markdown.cpp,file:test_settings.cpp`
Expected: 0 FAIL；`Total` 与改前用例数一致。

- [ ] **Step 4: 全仓 std include 守卫**

Run: `bash scripts/check-std-includes.sh`
Expected: `全部 … 个文件 … 都有显式 include`，exit 0。

- [ ] **Step 5: 提交（待门禁启用后按 T3 口径；若此时未启用则普通提交）**

```bash
git add tests/test_modules/test_markdown.cpp tests/test_modules/test_settings.cpp
git commit -m "fix: 密度几何断言改读平台密度档（本机 125% 不再假红）"
```

---

### Task 3: 在 DevDesk 常驻启用 devflow 门禁

**Files:**
- Modify: `.claude/settings.json`（`enabledPlugins` 显式加 devflow）
- Modify: `.gitignore`（`.devflow/` 本地状态）
- Create: `.devflow/context.json`（由 `task_bind.py` 生成，不入库）

**Interfaces:**
- Consumes: 插件仓 `devflow/hooks/edit-context-gate.sh`、`devflow/scripts/git-hooks/*`、`devflow/tools/task_bind.py`。
- Produces: `core.hooksPath` 指向插件仓 hooks；`.devflow/context.json` 存在即 T1 放行。

- [ ] **Step 1: 显式启用插件（项目作用域）**

在 `.claude/settings.json` 的 `enabledPlugins` 加 `"devflow@dev-zcode-plugins": true`。

- [ ] **Step 2: 绑定当前工作（先建池条目）**

Run（在 DevDesk 根）:
```bash
python "$USERPROFILE/dev/zcode-plugins/devflow/tools/pool_add_cli.py" --pool need --title "待办裁决收口 + 密度几何红修复" --priority P1
python "$USERPROFILE/dev/zcode-plugins/devflow/tools/task_bind.py" --kind need --id <上一步返回的 id>
```
Expected: `.devflow/context.json` 生成，内容含 kind/id。

- [ ] **Step 3: 启用 git 钩子（仓内本地配置）**

```bash
git config core.hooksPath "$USERPROFILE/dev/zcode-plugins/devflow/scripts/git-hooks"
git config --get core.hooksPath
```
Expected: 输出插件仓 git-hooks 的绝对路径。

- [ ] **Step 4: 冒烟——T1 拦截与放行**

用 hook 载荷直接验证（不经模型）：
- 无 context：临时移走 `.devflow/context.json`，feed `{"tool_input":{"file_path":"src/core/result.h"}}` → 期望 exit 2 且 stderr 含「改代码前先确认上下文」。
- 有 context：恢复后同载荷 → 期望 exit 0。

Run:
```bash
printf '%s' '{"tool_input":{"file_path":"src/core/result.h"}}' | bash "$USERPROFILE/dev/zcode-plugins/devflow/hooks/edit-context-gate.sh"; echo "exit=$?"
```
Expected: 移走时 `exit=2`；恢复后 `exit=0`。

- [ ] **Step 5: 冒烟——T3 提交门**

在临时分支上造一个空代码改动：
- 无 tag 提交 → 期望被拒（「代码提交须带归属」）；
- 带 `[pool:need:N]` 但缺 `Tests:`/`Evidence:` → 期望被拒；
- 补齐且证据文件存在 → 期望通过。

Run:
```bash
git checkout -b gate-smoke && echo "// x" >> src/core/result.h && git add src/core/result.h
git commit -m "chore: smoke"            # 期望被拒（chore 豁免，改用 feat）
git commit -m "feat: smoke"             # 期望被拒（缺归属）
git checkout -- src/core/result.h && git checkout dev && git branch -D gate-smoke
```
Expected: 两次提交均非 0；工作树无残留改动。

- [ ] **Step 6: 记录装机结果**

把 hooksPath、绑定 id、两次冒烟的退出码写进 `evidence/20261127-devflow-install/gate-smoke.log`。

---

### Task 4: 按门禁口径提交本批

- [x] **Step 1: 跑全量**

Run: `bash scripts/build-test-run.sh --no-run`

实际结果（2026-11-27 23:2x，macOS arm64，本机补跑）：

- 第一轮：`Total 2757 / Passed 2748 / Failed 1 / Skipped 8` —— 红在
  `todo_ui_drag_edge_autoscroll`（B-13 用例平台不对称）。
- 第二轮（修用例后）：测试全绿，但守卫链红在 `test_todo_service.cpp:1664` 的
  `std::milli` 缺 `<ratio>`（B-18 实测用例引入）。
- 第三轮（补 include 后）：`Total 2757 / Passed 2749 / Failed 0 / Skipped 8`，
  probe 72 个 100% passed（6 个设计内跳过），守卫链 6 道自检全过，Windows 交叉编译
  526/526，std include 1030 文件全过。证据：`evidence/20261127-batch-fullrun/full-green.log`。

两处红都不在产品代码，属「Windows 跑绿、mac/平台无关门禁会红」的遗漏：

| 缺口 | 根因 | 修法 |
|---|---|---|
| B-13 用例红 | 用例把板底写死为 `screen.height − 30`，只在 Windows 密度档（pad 24）落在 28px 边缘带内；mac pad 32 落点已越出板底 | 落点改由 `ui_kit::density::pageInsetV()` 派生 |
| std include 红 | B-18 用例用 `std::chrono::duration<double, std::milli>` 未 include `<ratio>`，靠标准库传递包含 | 补 `#include <ratio>` |

- [ ] **Step 2: 证据落盘并通过 T3 提交**

```bash
mkdir -p evidence/20261127-devflow-install
cp build/gate-summary.json evidence/20261127-devflow-install/
git add tests/test_modules/test_markdown.cpp tests/test_modules/test_settings.cpp .claude/settings.json .gitignore evidence/20261127-devflow-install
git commit -m "fix: 密度断言改读平台密度档 + devflow 门禁常驻 [pool:need:<id>]

Tests: bash scripts/build-test-run.sh --no-run（Total … / Failed 0）
Evidence: evidence/20261127-devflow-install/gate-summary.json"
```

---

### Task 5: 继续剩余待办（本批之后逐条）

- [x] C-17 补单测（`todo_ui_archive_auto_announce`，2026-11-27）
- [x] C-3/C-9 对账：按 `docs/research/2026-11-26-todo-module-research.md` P3-1 修正用户指南
      （4 张出厂模板、关闭模块仍跑后台迁移/同步、归档列内不支持手动排序）；D-1/D-2 经审计复核
      为「已交付 / 已否决」，无需开发。
- [x] B-9 = P2-15：`EventBus` 增 `subscribeWithPayload` / `emit(name,payload)`；工具写事件带
      `{"taskId","op","source":"tool"}`（不触发事件型智能体任务，防互循环）。
- [x] B-11 = P2-16：视图状态迁到本机专属 `ui_view_state` 表，并加入 `defaultSkipTables`
      （GitHub Sync 整库 dump 不再跨机搬运；旧 app_config 行一次性迁移并删除）。
- [x] B-18 = P1-5/Q5：`todo_service_reload_cost_200_tasks` 实测 220 任务整板重读（先测后定，
      未超 50ms 预算则不做增量刷新）。
- [x] C-2 = P2-8：系统「减少动态效果」下待办卡果冻静止（`jellyFrame(..., reducedMotion)` +
      平台 override 测试缝）。
- [x] B-13 = P2-7：拖拽边缘自动滚动（M；本批实现）。代码 `dragEdgeAutoScroll`
      （`todo_module.cpp:732`，调用点 `:1322`）+ 用例 `todo_ui_drag_edge_autoscroll`；
      2026-11-27 mac 全量补跑时修掉该用例的平台不对称落点。
- [ ] D-1/D-2 PRD：审计复核已交付/否决，仅在用户要 PRD 时另立文档。