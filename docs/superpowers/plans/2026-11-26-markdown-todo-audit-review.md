# 笔记 / 待办模块审计复核与改进计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. 本计划只覆盖复核结论与第一批小修；大项另立计划。

**Goal:** 把桌面新增的两份审计报告（Markdown、待办）与本机真实代码/池台账对齐，剔除已失效结论，并给出可直接开工的第一批小修。

**Architecture:** 两份报告都产自另一条代码线（基线提交本仓不存在）且引用的是 Mac 池 id；本机 Windows 池同号异义。复核按「标题/关键词反查本机池 + 定点读代码」重做，不按 id 直采。

**Tech Stack:** C++20 / 自制迷你 doctest / SQLite / EUI-NEO DSL。

**Spec:**
- 原报告：桌面 `新建 文本文档 (3).txt`（Markdown 模块，257 行）、`新建 文本文档 (4).txt`（待办模块，281 行）
- 对照报告：`新建 文本文档 (2).txt`（MCP/智能体）
- 本机基线：`dev @ eba4b37c`（复核当日）

## 0. 结论速览

| 维度 | Markdown 报告 | 待办报告 |
|---|---|---|
| 基线 `5499e166` | **本仓不存在** | **本仓不存在** |
| 池 id | 全是 Mac 池 id；Windows 对应条目多数已关闭 | 同左；`#384 未开工`、`#345 挂起` 已过时 |
| 报告主要缺口 | 成立（查找替换缺失、预览不可交互、退出保存失败只写日志） | 成立（A-1/A-2/A-3/A-4/A-7/A-8 均代码复核） |
| 报告已失效结论 | 「5 条 P1 台账债待核销」→ Windows 侧已全部关闭 | 「到期通知未开工」→ 已交付；「#382 主动找我」→ 已否决 |
| 规模漂移 | 35 文件 / 11,222 行；测试 274 例（报告 10,821 / 272） | 15 文件 / 6,940 行；测试 144 例（报告 6,915 / 139，漏了 due_watcher 5 例） |

**一句话：** 两份报告的问题清单大体可信，但「台账动作」和「未来项目」两节已过期；本机真正该先做的是待办 A-1（完成时间被重写，数据语义错）和 A-4（保存落空报成功），Markdown 侧是退出保存失败无提示。

---

## 1. 实地核对

### 1.1 共性结论

1. 三份报告都写基线 `5499e166` / `6790eb35`，**两个提交在本仓都不存在**（`git rev-parse` 均失败）。说明报告产自另一台机/另一条线，引用行号只能作线索，不能当本机事实。
2. 三份报告的池号都是 **Mac 池口径**，Windows 池 `docs/pool/pool.db` 同号异义。核销动作应在 Mac 侧做，本机不能按 id 直关。
3. 报告自称「未入池、未改代码」属实；本次复核也未改代码。

### 1.2 Markdown 报告

| 报告结论 | 本机核实 | 证据 |
|---|---|---|
| 模块 35 文件 / 10,821 行 | 35 文件，**11,222 行**（+401 漂移） | 递归统计 `.cpp/.h` |
| 测试 272 例 | **274 例**（test_markdown.cpp 120 对报告 117；grep 得 276，含 2 行注释） | `git grep -c "REGISTER_TEST("` |
| 退出最终保存失败只写日志 | **属实**：3 次重试后仅 `LOG_ERROR`。本机 app 层已有 `setQuitGuard`（`main.cpp:631`，只覆盖退出前询问），但 `onStop` 没有「保存失败可阻断/上报」的通道——真正缺口是失败不可见，不是注释问题 | `markdown_module.cpp:169-177`、`main.cpp:631` |
| 编辑器内无查找/替换 | **属实**：`findBar/searchBar/replaceAll` 在模块内 0 命中 | `git grep -c` 空 |
| 预览不可交互 | **属实**：EUI `components/markdown.h` 的 `onClick` 0 命中；任务列表/图片/公式均为只读或占位 | `git grep -c onClick` 空 |
| CRLF 文本模式读、落盘 LF | **属实**：三处 `std::ifstream` 未开 binary；`md_file_open.cpp:21` 是正确范例 | `markdown_service.cpp:362/711/871` |
| 5 条 P1 台账债「在池」 | **本机已全部关闭/已验收**：#453→Win #369、#458→Win #382、#461→Win #402、#462→Win #403、#467→Win #410（标题一致，均 `关闭/已验收`） | `needs` 标题反查 |
| #292/#475/#264 待核销 | 本机对应 **#478/#398/#264**，均 `关闭/已验收`；本机同号 #292/#475 是无关条目（Git 路径超长 / memory 弹窗） | `needs` 标题反查 |
| 两份 spec 状态过期、`KNOWN_LIMITATIONS.md` 断链 | **属实**：状态仍写「设计稿（未实施）」「设计草稿（未实施）」；文件不存在 | spec 头 + `Test-Path` |

本机新增发现（报告未列）：
- `test_markdown.cpp` 用例数比报告多 3 条（报告可能用了旧口径）。

### 1.3 待办报告

| 报告结论 | 本机核实 | 证据 |
|---|---|---|
| 14 个源文件 6,915 行 | **15 文件 6,940 行**（报告前句把构建脚本单列、后句按总文件数计，口径不同，不是矛盾） | 递归统计 |
| 5 测试文件 139 例 | **6 文件 144 例**，漏了 `test_todo_due_watcher.cpp`（5 例） | `git grep -c` |
| A-1 详情保存重写完成时间 | **属实**：`collect()` 无视完成态是否变化，`completed=true` 一律写当前时刻；`save()` 不看是否 dirty | `todo_ui_dialogs.cpp:709-711`、`:720-721` |
| A-2 智能体标记完成不挪列 | **属实**：只调 `completeTask`，不搬列 | `todo_tools.cpp:405-408` |
| A-3 智能体能建进已完成/已归档 | **属实**：只校验列存在，不看列 kind | `todo_tools.cpp:236-250` |
| A-4 保存落空报成功 / 空标题能存 | **属实**：`updateTask` 不看影响行数；详情保存无标题校验 | `todo_service.cpp:567-571`、`todo_ui_dialogs.cpp:720-721` |
| A-7 工具写路径不发事件 | **属实**：`todo_tools.cpp` 中 `emit/EventBus` 0 命中 | `git grep -c` 空 |
| A-8 说明写 `todo.move` | **属实**：MCP 对外名已映射成 `todo_move`，说明未同步 | `todo_tools.cpp:277`、`mcp_tool_mirror.cpp:80-84` |
| 列管理接口零生产调用方 | **属实**：`createColumn/renameColumn/setColumnColor/deleteColumn/reorderColumns` + 取位次函数，生产代码无调用；测试/探针/live 工具仍用于造数 | `git grep` |
| 索引 `idx_tasks_column/idx_tasks_due` 存在 | **属实** | `core/schema.h:504-507` |
| **D-1 到期通知「没有任何提醒、未开工」** | **失效**：`module_schedule/todo_due_watcher.cpp` 已交付（扫描器 + 去重 + 渠道投递），5 条用例在跑；本机 #453（Mac #384）为 `关闭/已验收` | 代码 + `test_todo_due_watcher.cpp` + 池 |
| D-1 前置①通知不能跳页 | **属实**：`platform::notify::Notification` 无 action/click 字段 | `platform/notification.h:41-47` |
| D-1 前置②截止只到「天」 | 报告结论保留（未逐行复核） | `todo_ui_logic.h:353-361` |
| D-2 #382「主动找我」在池 P1 未开工 | **失效**：本机 #451（Mac #382）为 `拒绝/已否决` | `needs` 标题反查 |
| #345 四列折叠互斥「挂起」 | **失效**：本机 #463（Mac #345）已 `拒绝/已否决`；代码为每列独立折叠态 | `needs` + `todo_module.cpp:1380` |
| #404/#414/#464 转待验收 | 本机对应 #272/#368/#405 **均 `关闭/已验收`** | 标题反查 |

**池号映射（本机口径，供后续核账用）**

| 报告（Mac 池） | 本机（Windows 池） | 本机状态 |
|---|---|---|
| md #453 / #458 / #461 / #462 / #467 | #369 / #382 / #402 / #403 / #410 | 全部关闭/已验收 |
| md #292 / #475 / #264 | #292 / #475 / #264 | 全部关闭/已验收 |
| todo #404 / #414 / #464 | #272 / #368 / #405 | 全部关闭/已验收 |
| todo #345 / #384 / #382 | #463（拒绝）/ #453（关闭）/ #451（拒绝） | 已裁决/已交付 |
| todo #386 / #371 / #297 | 同号为无关条目（池台账 HTML / 设置开关 / 顶栏卡片） | 不可按 id 直采 |

---

## 2. 仍然成立的缺口与优先级

| 级别 | 模块 | 项 | 依据 | 量级 |
|---|---|---|---|---|
| **P1** | 待办 | A-1 保存重写完成时间（数据语义错） | 报告 §3.1 + 代码复核 | S（5 行 + 30 行测试） |
| **P1** | 待办 | A-4 保存落空报成功、空标题可存 | 报告 §3.1 + 代码复核 | S（10 行 + 测试） |
| **P1** | Markdown | 退出最终保存失败无用户提示（丢稿） | 报告 §3.3 + 代码复核 | S–M（需先定退出交互） |
| P2 | 待办 | A-7 工具写不发事件（且需防互循环） | 报告 §3.1 | M，需裁决 |
| P2 | 待办 | A-2 / A-3 智能体行为与界面对齐 | 报告 §3.1 | S，需裁决 |
| P2 | 待办 | A-8 说明里的 `todo.move` 改名 | 1 行 | S |
| P2 | Markdown | 编辑器内查找/替换 | 报告 §3.1 | M（可参考 notepad 回流） |
| P3 | Markdown | 预览可交互（链接/复选框/图片） | 需 EUI-NEO 补命中能力 | L（框架级） |
| P3 | 待办 | B-4 查待办工具过滤/字段、归档上限 | 报告 §3.2 | S–M |
| 暂缓 | 两模块 | B/C 档其余 30+ 条 | 报告 §3.2/3.3 | 逐条另立 |

---

## 3. 第一批执行任务：待办数据正确性（S）

**五维预算（本批合计）**

| 维度 | 预算 |
|---|---|
| 代码量 | 生产 ≤ 20 行；测试 ≤ 120 行 |
| 注释量 | ≤ 6 行（只写 why） |
| 执行周期 | 0.5 天 |
| 内存 / CPU | 无新增常驻；`updateTask` 多一次递归锁（微秒级） |
| 证据物 | `evidence/20261126-todo-a1-a4/`（构建输出、`Total:`、修复前后用例红/绿日志） |

### Task 1: 详情保存不得重写已完成任务的完成时间

**Files:**
- Modify: `src/module_todo/todo_ui_dialogs.cpp:697-717`
- Test: `tests/test_modules/test_todo_ui.cpp`（仿 `todo_ui_detail_actions_save_form`，4051 行）

**Interfaces:**
- 不变更任何公开签名；`DetailDialog::collect` 内部行为改为「完成态未变 → 沿用 `orig->completedAt`」。

- [ ] **Step 1: 写失败用例**

```cpp
REGISTER_TEST(todo_ui_detail_save_keeps_completed_at,
              "todo_ui: 已完成任务在详情直接保存后完成时间不变（A-1）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    const int64_t todoCol = cols.value()[0].id;
    const int64_t doneCol = cols.value()[2].id;
    auto made = env.svc->createTask(todoCol, "已完成任务");
    DOCTEST_REQUIRE(made.isOk());
    // 造一条「历史完成（早于今天零点）」的任务：先搬进已完成列，再把 completedAt 改成 1_700_000_000
    DOCTEST_REQUIRE(env.svc->moveTask(made.value().id, doneCol, 0).isOk());
    auto listed = env.svc->listTasks(doneCol);
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_REQUIRE_EQ(listed.value().size(), 1u);
    module::todo::Task orig = listed.value()[0];
    orig.completedAt = std::optional<int64_t>(1700000000);
    DOCTEST_REQUIRE(env.svc->updateTask(orig).isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.2.card.0.bg");  // 已完成列
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame f2(mod.get());  // f1 先销毁再重绘（test_todo_ui.cpp:806-819 既有范式）
        const auto* save = f2.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        save->onClick();  // 不改任何字段，直接保存
    }
    auto got = env.svc->listAllTasks();
    DOCTEST_REQUIRE(got.isOk());
    bool found = false;
    for (const auto& tk : got.value()) {
        if (tk.id == made.value().id) {
            found = true;
            DOCTEST_CHECK_EQ(tk.completedAt.value_or(-1), 1700000000);
        }
    }
    DOCTEST_CHECK(found);
    mod->onStop();
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter todo_ui_detail_save_keeps_completed_at`
Expected: FAIL（完成时间被改写成当前时刻）。

- [ ] **Step 3: 改 `collect()`**

```cpp
    const bool wasCompleted = orig != nullptr && orig->completedAt.has_value();
    if (orig != nullptr && wasCompleted == completed) {
        updated.completedAt = orig->completedAt;  // 完成态没变：完成时间是事实，不得重写
    } else {
        updated.completedAt = completed
            ? std::optional<int64_t>(static_cast<int64_t>(std::time(nullptr)))
            : std::nullopt;
    }
```

- [ ] **Step 4: 跑绿 + 回归**

Run: `bash scripts/build-test-run.sh --dev --filter todo_ui`
Expected: 新用例 PASS，既有 74 条 UI 用例全 PASS。

- [ ] **Step 5: 提交**

```bash
git commit -m "fix: 待办详情保存不再重写已完成任务的完成时间"
```

### Task 2: 保存落空要报错；详情标题非空校验

**Files:**
- Modify: `src/module_todo/todo_service.cpp:531-571`
- Modify: `src/module_todo/todo_ui_dialogs.cpp:720-721`
- Test: `tests/test_modules/test_todo_service.cpp`、`tests/test_modules/test_todo_ui.cpp`

- [ ] **Step 1: 写失败用例**

服务层：

```cpp
REGISTER_TEST(todo_service_update_missing_task_fails,
              "todo_service: updateTask 目标不存在 → fail(NotFound) 而非静默成功", {
    TestEnv env("update_missing_task");  // test_todo_service.cpp 的夹具必须带 tag
    DOCTEST_REQUIRE(env.ready);
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto made = env.svc->createTask(cols.value()[0].id, "将被删");
    DOCTEST_REQUIRE(made.isOk());
    DOCTEST_REQUIRE(env.svc->deleteTask(made.value().id).isOk());
    module::todo::Task ghost;
    ghost.id = made.value().id;
    ghost.title = "ghost";
    auto r = env.svc->updateTask(ghost);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NotFound);
    return true;
})
```

UI 层（确定性路径：直接造一条空白标题任务，不依赖改输入框）：

```cpp
REGISTER_TEST(todo_ui_detail_blank_title_rejected,
              "todo_ui: 详情空白标题保存被拒，弹窗保留、库值不变（A-4）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto made = env.svc->createTask(cols.value()[0].id, "   ");  // service 不校验
    DOCTEST_REQUIRE(made.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onClick();
    }
    {
        Frame f2(mod.get());
        const auto* save = f2.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        save->onClick();
    }
    {
        Frame f3(mod.get());
        DOCTEST_CHECK(f3.ui.find("todo.dialog.detail.save.bg") != nullptr);  // 弹窗没关
    }
    auto got = env.svc->listAllTasks();
    DOCTEST_REQUIRE(got.isOk());
    bool found = false;
    for (const auto& tk : got.value()) {
        if (tk.id == made.value().id) {
            found = true;
            DOCTEST_CHECK_EQ(tk.title, std::string("   "));
        }
    }
    DOCTEST_CHECK(found);
    mod->onStop();
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter todo_service_update_missing`
Run: `bash scripts/build-test-run.sh --dev --filter todo_ui_detail_blank_title_rejected`
Expected: 两条均 FAIL。

- [ ] **Step 3: 实现**

`updateTask` 函数入口（`prepare` 之前）加锁；`handle()` 要求调用方自持 `mutex()`，且锁必须覆盖 `step + sqlite3_changes`（范式见 `renameColumn`，`todo_service.cpp:305-321`）：

```cpp
core::Result<void> TodoService::updateTask(const Task& task) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    // ...原有 prepare/bind...
    auto stepResult = stmt.step();
    if (stepResult.isFail()) return core::propagate<void>(stepResult);
    if (sqlite3_changes(db_.handle()) == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound, "任务不存在");
    }
    return core::Result<void>::ok();
}
```

详情保存前：`title = core::str_util::trimAscii(title)` 后为空 → `host.warn("标题不能为空")` 并 return false，不落库；`todo_ui_dialogs.cpp` 补 `#include "core/str_util.h"`。

- [ ] **Step 4: 跑绿**

Run: `bash scripts/build-test-run.sh --dev --filter todo_service`
Run: `bash scripts/build-test-run.sh --dev --filter todo_ui`
Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git commit -m "fix: 待办保存落空报错 + 详情标题非空校验"
```

### Task 3: A-8 工具说明改名（1 行）

- [ ] **Step 1: 改 `todo_tools.cpp:277`**：`请使用移动任务工具`（不带具体工具名，避免与对外下划线名再次漂移）。
- [ ] **Step 2: 跑 `bash scripts/build-test-run.sh --dev --filter todo_tools`，PASS 后提交**：

```bash
git commit -m "fix: 待办工具说明不再引用会漂移的 todo.move 名"
```

---

## 4. 需老板裁决（先定后开工）

1. **待办 A-2 / A-3**：智能体工具行为是否与界面对齐（完成即搬列；只能建进待办/进行中）？对齐后智能体完成的任务次日会被自动归档。推荐「对齐」。
2. **待办 A-7**：工具写路径是否补发事件？补则必须带 `source=tool` 标记防「智能体写 → 事件 → 起运行 → 再写」循环。推荐先只补「看板/首页刷新」所需的最小事件，不接事件型任务。
3. **Markdown 退出保存失败**：选哪种交互？① 退出前预保存，失败弹「重试 / 放弃并退出」并阻止本次退出（推荐）；② 仅通知后照常退出（仍是丢稿）。选①属 M 级，需要 `setQuitGuard` 增加保存结果通道。
4. **Markdown 查找/替换**：是否从 notepad 回流？notepad 已有实现（`src/module_notepad/notepad_find.{h,cpp}`）；对应 Mac #263→Win #427、Mac #285→Win #429 均为 `拒绝`，不要按「待验收」计。推荐先复用交互，不重写引擎。

## 5. 本轮不做

- Markdown 预览可交互（链接/复选框/图片/公式）：需 EUI-NEO `components::markdown` 新增命中与指针事件 API，属框架级 L，不在本批次。
- 待办 B/C 档 30+ 条：逐条另立计划；其中 B-1 选中态、B-2 逾期滚动、B-5 菜单对称、B-6 草稿守卫可合成一个 S 批。
- 标签全删：Mac clue#365 已裁决但无池条目；本机 `clues` 最大 id 346、无此条，裁决出处需另补本机文档/池条目，开工前再入池。
- **#382（Mac）/ 本机 #451、#345（Mac）/ 本机 #463**：两份均 `拒绝/已否决`；本机同号 #382/#345 是无关条目（md 预览性能 / notepad 复制），不得按同号引用，不重开。
- 所有已关闭项的「核销」：Windows 池已是关闭态，动作在 Mac 池，本机不重复做。