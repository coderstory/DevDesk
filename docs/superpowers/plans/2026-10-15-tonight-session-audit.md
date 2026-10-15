# 今晚批次审计 — GitHub Sync T1 合并 + include 修复 + spec 回写（de456cb..9e1682f）

- 审计对象：主仓 dev，任务下发时 HEAD = `9e1682f`（固定口径）；审计进行中 dev 两次前移（`fc7a248` = +T2/T3 合并，`1e7a4ad` = backlog docs），本文对两个快照分开下结论，末尾给 live dev 可构建性
- 双 lens：C++20 系统专家 + 软件架构师；本审计除本文件外零写动作
- 不重复 T1 六维审计（`2026-10-15-github-sync-t1-audit.md` 已放行），聚焦合并结果与跨提交一致性；T1 正确性只做抽样复验
- 验证手段（全部隔离环境，主树未跑构建）：临时 worktree `/Users/coderstory/dev/gui/wt-audit-tonight` @ `fc7a248`（用完已删），冷构建 405/405 exit 0；`check-std-includes.sh` 对 `de456cb` / `9e1682f` / `fc7a248` 三快照各扫一遍（git archive 导出，判据同脚本）；SqlDump 用例本审计亲跑。手敲 cmake 而非 build-test-run.sh 的理由：主树构建断且禁跑、审计不许启动 app（脚本默认行为），属 CLAUDE.md 明示的诊断/验证例外

## 一、必查项结论

### 1. 合并完整性（2570a96）— 已核，无丢失

- 四个 T1 文件（`sql_dump.cpp/.h`、`tests/CMakeLists.txt`、`test_sql_dump.cpp`）在合并结果中与 `47fe68a` **逐字节一致**（`git diff --quiet 2570a96 47fe68a -- <file>` 四个全部 quiet）；对 dev 侧父提交 `9eb0e17` 的差异恰好等于 T1 引入的内容，dev 侧当晚改动（spec 回写 + include 修复）原样保留。两侧文件集不相交，无冲突解决动作可错
- `<set>`：T1 的 `47fe68a` 自己就带（pickaxe `-S'#include <set>'` 唯一命中 + 合并快照 `sql_dump.cpp:7` 在位）。T2 分支的 `05cc0bc` 修的是**同一文件的旧版**（其父 `de456cb` 无 T1 重写），属同一缺陷的平行重复修复，随 `80d45e3` 合入时无冲突、无丢失。dev@9e1682f 不因缺 `<set>` 而编不过
- `tests/CMakeLists.txt`：`test_sql_dump.cpp` 注册在位（`tests/CMakeLists.txt:13`）

### 2. include 修复（9eb0e17）— 已核，修对位置，无第三处遗留

- `git_sync_service.h`：头文件自身接口用 `std::filesystem::path`（:52/:103/:113），`<filesystem>` 加在头文件本体 = 正确落位；`agent_loop.cpp`：`std::snprintf`（:371）配 `<cstdio>` = 正确
- 基线 `de456cb` 守卫扫出**三处**违规：上两处 + `sql_dump.cpp:117` 的 `std::set`。第三处不经 9eb0e17，而是 T1 重写顺手带入 `<set>` 解决——「补欠账两处」对该 commit 口径准确，当晚全部欠账确已清零：`9e1682f` 扫描 392 文件 0 违规、`fc7a248` 扫描 404 文件 0 违规
- 插曲：9eb0e17 修的 `git_sync_service.h` 是决策#9 标记待删的文件——树在删除落地前必须能编，修了没错，仅记录

### 3. spec 回写与代码一致性 — F2/F4 逐条对上；#4/#6 在固定快照超前于 dev（审计期间自愈）

| 回写 | 声称的实现事实 | dev@9e1682f | live dev（fc7a248） |
|---|---|---|---|
| `1d58d76` F2 触发器序 | 段序①建表②索引③INSERT④触发器 + 双 DROP IF EXISTS + 回环钉序 | ✅ `sql_dump.cpp:247-305` 完全一致（四段注释 + 实现序），测试 `test_sql_dump.cpp:125-128` 断言 `triggerPos > firstInsert` | 同左 |
| `8c40d05` F4 常量落位 | `core::sql_dump::defaultSkipTables()` 存在 + 单测锁死 + T4 编排引用 | ✅ 函数在（`sql_dump.h:18`/`sql_dump.cpp:23`）、用例 `sql_dump_skip_defaults` 在；T4 引用是前瞻指令 | ⚠ 见 finding 1：T3 已抄了第二份清单 |
| `95eb875` 决策#4 403 分态 | 「T2 实现修订」：限流态 403 按 429 重试 | ❌ **超前于 dev**——`github_api.cpp` 在 9e1682f 不存在 | ✅ `github_api.cpp:102-108`（`x-ratelimit-remaining:0` 才重试）+ Retry-After 上限 60s（:36/:110）与 spec 一致 |
| `be7633c` 决策#6 bodyTail B | 「T2 实现」的响应体尾部暴露面 | ❌ **超前于 dev**——同上 | ✅ `bodyTail()` 在（`github_api.cpp:81`，:407 消费） |

- 回写先于所引代码上 dev 落盘（22:35/22:37 vs T2 合入 22:46），期间 spec 引用了 dev 上不存在的实现；T2/T3 合入后口径归一。**流程注**：回写应与其引用的合并同批或其后落，避免窗口期 spec 指向空气

### 4. 文档口径矛盾 — 未发现硬矛盾，两条待办口径已亲验为真

- backlog M4（`9e1682f`）「dev 无 migration 体系」：实核 `schema.h` 无版本无 migrations（单份 DDL），`migrations v1-v4` 属 master 线——commit 的核实结论与代码一致，非拍脑袋
- 决策#9「删 git_sync_service」与 9eb0e17 修其 include 不构成矛盾（见上）；F4 前瞻指令与 T3 现状见 finding 1

### 5. 过程纪律（诚实记录）

- **主会话直接提交文档**：reflog 显示 bfe3a67 / 458568f / 1d58d76 / 8c40d05 / 95eb875 / be7633c / 9e1682f（以及固定快照后的 5c6b953 / fc7a248 / 1e7a4ad）全部为 dev 上的**直连 commit**（无 worktree 合并记录）——「文档也派子任务」的派单纪律今晚未执行。内容本身经本审计抽查为真（M4 核实、F2/F4 回写），但既写又审的第二方视角缺失是实打实的
- **inclfix 口径与 reflog 不符**：任务下发称「inclfix 分支被自动合并未经显式审计关卡」，reflog 里**没有** inclfix 分支或合并——`9eb0e17` 是 22:24:59 的 dev 直连 commit，无独立审计留痕（本审计已补验其正确性）。记录分歧，以 reflog 为准
- **审计目标漂移**：dev 在本审计进行中两次前移（22:46 T2 合入、22:47 T3 合入、22:56 backlog）；本文对固定快照与 live dev 分开下结论，未混判
- dev@9e1682f **构建断属实**：`github_sync_card.cpp:91/:175` 调 `components::switch_`，该符号在 DevDesk 全仓与本地 EUI-NEO（HEAD `9cdfa64`）均不存在（真实 API 是 `components/switch.h` 的 `SwitchBuilder`/`toggleSwitch`，自 07-13 就没有过 `switch_`）——来源 `8e82e0d`，与任务下发口径一致；live dev 已由 T3 重写（`38115d0`）消除该调用而痊愈

### 6. 架构 lens（sql_dump 扩展后 + skip 常量落位）

- **单一职责保持**：导出/导入/回环同属「SQLite ↔ SQL 文本」一个概念，4 个公开函数 + 1 个常量，helper 全私有，接口深度没有膨胀；导入侧 FK 开关、事务壳剥离、段序约束都是 dump 域内语义，没有越界
- **`defaultSkipTables()` 落 core 不破坏依赖规则**：core 仍不依赖 platform/module；相反若清单落 `github_sync_logic`，反向会让 core 语义被 module_settings 持有。落位本身更优（T1 审计 finding 1 同判）
- 措辞小瑕：F4 回写理由「备份与同步两路共用」——`db_backup`（dbmaint）是文件级 `sqlite3_backup`，不消费该清单；现实消费方只有同步 dump 一路 + 未来 T4。不影响落位结论

## 二、Findings

| # | 位置 | lens | severity | 问题 | 建议 |
|---|---|---|---|---|---|
| 1 | `src/module_settings/settings_module.cpp:108-109`（消费 `:817`；随 T3 `38115d0` 进 dev，晚于固定快照） | 架构 | **medium** | `kGithubSyncSkipTables` 手抄了 `defaultSkipTables()` 的第二份清单，注释自承「与 sql_dump.h 的示例口径一致」——恰好违反 sql_dump.h 头注与 F4 回写明文的「不得另抄清单（两份必漂移）」；现有单测只锁 core 清单，没有任何机制把这份拷贝跟 core 绑住，core 加一张表时同步路静默漏改 | T4 硬门禁兑现改引（T3 审计已登记），或现在就小改一行 `= core::sql_dump::defaultSkipTables()`；另可加一条断言两清单相等的单测防复发 |
| 2 | `95eb875` / `be7633c`（spec）↔ dev 时序 | 文档/过程 | low | 两处决策回写引用的 T2 代码在落盘时点不在 dev 上（窗口 22:35→22:46），期间 spec 指向不存在的实现；本审计期间 T2 合入后已自愈、逐条对上 | 回写与其引用的 merge 同批或其后落盘；跨票回写在票面标「依赖 Tn 合入」 |
| 3 | 主会话直连 commit ×7+（reflog 22:09-22:56） | 过程 | low | 「文档也派子任务」纪律今晚成批未执行；文档内容虽经抽查为真，但写审同体 | 后续文档变更同样走子任务；主会话保留裁决与提交 |
| 4 | `9eb0e17`（inclfix） | 过程 | low | 直连 dev、无独立审计关卡即合入（任务下发所称「分支自动合并」与 reflog 不符，实为直连 commit）；所幸 2 行机械改动，本审计已补验正确 | 修复类改动即便机械也留最小审计痕（审计文件一行带过即可） |
| 5 | `platform/macos/build-app.sh`（git mode 100644）+ `CMakeLists.txt:327` 直呼 | 基建 | low（**先于今晚存在**，末次触碰 10-02 `937c2f4`） | `ninja devdesk_app` / `cmake --build --target devdesk_app` 在任何 fresh clone/worktree 上 `Permission denied`（exit 126，本审计实测）；显式 `bash build-app.sh` 则成功出包——非今晚回归，但「devdesk_app 目标不在 all」+ exec 位缺失叠加，fresh 环境一行命令出不了 .app | `git update-index --chmod=+x platform/macos/build-app.sh` 或 CMakeLists 改经 bash 调用 |
| 6 | （信息项，无需行动） | 系统 | info | dev@9e1682f 构建断（`switch_` 不存在，见 §5），已被 T3 重写痊愈；live dev 构建全绿（见下） | — |

## 三、可构建性结论

- **live dev（fc7a248；其上 `1e7a4ad` 仅 backlog docs 2 行，不影响）**：隔离 worktree 冷构建 **405/405 exit 0**（含 EUI-NEO、全部模块、`devdesk_tests`、4 个 probe）；`build/tests/devdesk_tests SqlDump` 亲跑 **5/5 全绿**（回环比对 / 重复导入幂等 / 失败回滚 / 默认 skip 清单）；`bash platform/macos/build-app.sh` 出包成功。**结论：dev 当前可构建、可测，今晚 T1-T3 无一阻断**
- **固定快照 dev@9e1682f**：按静态证据（`switch_` 符号全仓不存在 + 使用点在位）判定**不可构建**，与任务下发口径一致；该断点未进今晚任何审计对象票（属 `8e82e0d` 旧账），由 T3 合并顺带修复
- 唯一 blocker 性质事项：无。finding 1（skip 清单双份）是唯一建议尽快兑现的 medium，其余均 low/过程类

## 四、总评

今晚批次整体质量好：合并零丢失、spec 回写与代码逐字对得上（F2 段序/F4 落位经代码实证）、include 欠账当晚清零且守卫三快照全绿、backlog M4 核实结论经代码复核为真、T1 审计放行判断与本审计抽样复验一致。扣分集中在过程面：主会话成批直连改文档、决策回写跑到所引代码前面、include 修复无审计痕；以及一条 T3 带进来的实 medium（skip 清单双份）。dev 现在可构建可测，无阻塞项。
