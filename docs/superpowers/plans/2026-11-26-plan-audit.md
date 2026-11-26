# 方案审计报告（2026-11-26）

**审计对象**
1. `docs/superpowers/plans/2026-11-26-mcp-agent-improvement-plan.md`（Task 1–6，待开发）
2. `docs/superpowers/plans/2026-11-26-markdown-todo-audit-review.md`（复核 + 小修计划）

**方法**：两名独立只读子代理分别审计（不共享主会话上下文），主会话按发现逐条修复计划；审计底稿在 `tmp/audit-mcp-plan.md`（342 行）、`tmp/audit-md-todo.md`（304 行）。

**总判定：PASS（修复后）**。两份计划在修复前分别为 `PASS_WITH_FIXES`：MCP 4 Critical / 11 Important / 13 Minor；笔记待办 2 Critical / 4 Important / 10 Minor。所有 Critical 与 Important 已写进计划并复验，Minor 中影响正确性的已一并修，纯口味项留执行时顺手处理。

---

## 1. MCP 计划修复（Task 1–6）

| 发现 | 后果 | 已落到计划 |
|---|---|---|
| **C-1** 测试用 `JsonValue::asArray().at()`（不存在） | Task 2 实现后仍编译失败 | 改为 `content.size()` + `content[0]` |
| **C-2** 运行中 `setAllowDestructive` 只改成员，不转发 live `protocol_` | 页头开关与 E2E 探针都会“显示已开但闸仍关” | 加 mutex 转发（与 `stop()` 搬移互斥） |
| **C-3** 既有超时用例 detach 的 worker 污染新并发用例；REQUIRE 失败 + joinable thread → terminate | 整进程被杀 | 用例开头 `waitInflightDrained`；加 `ThreadJoinGuard` / `BlockingReleaseGuard`（幂等 `releaseOnce`）；断言移到 join 后 |
| **C-4** 页头助手 `const std::function&` 被引用捕获 | compose 返回后悬垂，既有记忆行用例被 UB 波及 | 助手按值接收 `onToggle`，lambda `[onToggle = std::move(onToggle)]`；明文禁止 `&onToggle` |
| **I-1** `slotRelease` 捕获方式未写死 | detach 后悬垂/提前归还 | 明文按值捕获并给代码片段 |
| **I-2** 两测试失败路径泄漏/终止 | 槽位永久占用 | RAII 收束（同 C-3） |
| **I-3** 重启用例只验成员 | 假绿 | 改为真 HTTP 往返：默认拒 → 运行中开 → 放行 → stop/start → 仍放行 |
| **I-4** 运行中转发与 `stop()` 竞态 | use-after-free | C-2 的 mutex |
| **I-5** 助手无 icon/配色 | 破坏性行复用“大脑”图标 | 加 `iconCodepoint` + `iconColor`；破坏性行用 `0xF071` / `t.colors.warning` |
| **I-6** `readRow` 在 `loadChecked()` 不在 `load()` | 片段放错作用域编译失败；坏值静默 | 指定 `loadChecked()` 插入点；parse fail 原样上抛 |
| **I-7** `test_main` 未复位新桥槽 | 跨用例串桩 | 加入逐用例复位清单 |
| **I-8** 提交步骤缺 `git add`；收口 `git add -A` 扫入 submodule/tmp | 空提交或污染提交 | 每个任务显式 `git add`；收口先 `git status`，禁 `-A` |
| **I-9** 总开关一次放行 7 件（含删待办/笔记） | 产品面突变 | 开启时 app 层 toast 明示；USER_GUIDE 列 7 件与绑定关系 |
| **I-10** 8 个挂死 worker 永久占槽，重启不清零 | 服务假死 | spec §14.2 增补；`stop()` 有在途时 WARN（不重置计数） |
| **I-11** detach/restart 用例后置；probe 直跑 ctest | 违反先红后绿/唯一入口 | detach 用例并入 Task 3 Step 1；restart 用例注明实现前先落盘跑红；probe 明列为例外通道并保留脚本构建 |

Minor 处理：补 `<atomic>`/预取 future、拒绝分支 `LOG_INFO`、`MCP_MAX_CONCURRENT_CALLS` 写入文档、预算口径改 3 个 atomic、修 spec 链接路径、probe 给 `--shot` 命令与证据路径、“试一下”直调 `execute` 明文列为既定边界、测试点击前判空。

## 2. 笔记 / 待办计划修复

| 发现 | 已落到计划 |
|---|---|
| **C1** `test_todo_service.cpp` 的 `TestEnv` 只有带 tag 构造 | 改 `TestEnv env("update_missing_task");` |
| **C2** `sqlite3_changes` 缺锁，并发下 0 行判定不可靠 | `updateTask` 入口加 `std::lock_guard<std::recursive_mutex>`，锁覆盖 step+changes |
| **I1** 池映射 #292/#475 写成同号 | 改 `#292/#475/#264 → #478/#398/#264`，并注明同号异义 |
| **I2** Markdown 用例 276 含 2 行注释 | 改 274；`test_markdown.cpp` 120 对报告 117 |
| **I3** §5 把 Mac #382/#345 当本机号 | 改「#382(Mac)/#451、#345(Mac)/#463 已否决；本机同号为无关条目」 |
| **I4** `need263 待验收` 不成立 | 改「notepad 已有实现；Mac #263→#427、#285→#429 均拒绝」 |
| Minor | `main.cpp:631` 行号、列管理接口名、文件口径说明、删「详情红字通道」、`clue#365` 本机不存在、时间戳措辞、Frame 作用域拆分、补五维预算与证据路径、补可粘贴的 UI 用例 |

## 3. 未采纳 / 留到后续

- **M-9（MCP service 级并发 E2E）**：纯协议单测已覆盖配额与槽位；HTTP 面并发留到 C-2 调用日志批次一起做（避免本批引入长耗时 sleep 探针）。
- 纯文案/风格类 Minor：执行时顺手，不单独返工计划。
- 待办 A-2/A-3/A-7、Markdown 退出交互仍按计划 §4 等老板裁决，不在本批开工范围。

## 4. 剩余风险（已接受）

1. 测试顺序依赖：新用例靠 `waitInflightDrained(3s)` 排空前序 detach worker；若机器极慢仍可能假红，属可观测失败而非崩溃。
2. 槽位只随 worker 真正退出归还：极端挂死 8 条后 MCP 工具面会拒绝到重启应用；已在 spec/USER_GUIDE 写明。
3. 总开关粒度：7 件全开，按工具粒度留待后续裁决。
4. probe 通过 `ctest -L probe` 运行属项目既有例外，已在 Global Constraints 写明。

**结论：两份计划修复后通过审计，可进入开发。** 第一批按 MCP 计划 Task 1–6 执行，随后接笔记/待办小修。