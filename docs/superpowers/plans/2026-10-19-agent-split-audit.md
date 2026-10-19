# 智能体拆分审计（独立审计）— 2026-10-19

- **审计对象**：`ccda23c5`（草稿 Job → task_draft_job）+ `a619418d`（详情弹窗 → agent_detail_dialog）
- **基线**：F1 审计架构文档 `docs/superpowers/plans/2026-10-19-agent-batch-audit-arch.md`
- **口径**：只读、纯静态审读 + grep、零构建；每条 finding = `file:line` + 问题 + 影响 + 建议，severity 按演进代价

---

## 1. 正确性

### 1.1 HIGH — 6 个常量搬走后 agent_module.cpp 留死代码

- **位置**：`src/module_agent/agent_module.cpp:235,239-241,250-251`
- **问题**：`kDetailDialogH`、`kStopChipW`、`kStopChipH`、`kStopChipY`、`kMsgAreaMinH`、`kMsgAreaMaxH` 六个 constexpr 仅被 composeTaskDetailDialog 使用，该函数已迁出 → 当前全仓无消费方（grep 确认零非定义行命中）。它们是编译器认可的 dead code（能被编译器优化掉，不破构建），但留着一个来源不明的常量集会增加维护混淆——未来改动者可能误认为还有代码在用而追空引用。
- **影响**：低功能风险（编译通过），高维护混淆——AI 后续改动可能被这些"看起来在用的常量"误导，或误改后以为在修改某条实际已经不走的路径。
- **建议**：删除这六行 constexpr（保留 `kDetailRunListLimit`，它在行 1249 的 compose 函数里仍被消费）。

### 1.2 MEDIUM — 五个函数 + 三个常量跨 TU 双副本，漂移风险

- **位置**：
  - `statusDotColor`：`agent_module.cpp:104-111` / `agent_detail_dialog.cpp:51-58`
  - `actionChip`：`agent_module.cpp:115-149` / `agent_detail_dialog.cpp:61-94`
  - `navigateToClaudePage`：`agent_module.cpp:151-155` / `agent_detail_dialog.cpp:98-105`
  - `cronTextOf`：`agent_module.cpp:278-296` / `agent_detail_dialog.cpp:108-117`
  - `runLimitText`：`agent_module.cpp:298-301` / `agent_detail_dialog.cpp:119-121`
  - `kDialogGap` / `kDialogLabelW` / `kDialogRowH`：`agent_module.cpp:218-220` / `agent_detail_dialog.cpp:46-48`
- **问题**：均为匿名命名空间内 static 内链接（两 TU 各自一份，不造成 ODR/符号冲突，实现者描述"可接受"成立）。但**同值常量并存**意味着改任意一个常量/函数逻辑时必须手工双改——漏一处 → 主页面（compose/composeTaskDialog）与详情弹窗（composeTaskDetailDialog）的行为不一致。当前它们完全相同（git show 逐段对照搬前/搬后一致），但这是**纯人工约定、无编译器/测试强制**。其中 `cronTextOf` 和 `runLimitText` 在 detail dialog 侧已有一个独立副本（agent_detail_dialog.cpp:108-121），agent_module.cpp 侧的原件仍被 composeTaskDialog 消费——未来修改文案或逻辑时会自然只改一处。
- **影响**：漂移风险——改一处漏一处 → 主页面与详情页行为分裂（如 chip 颜色、标签宽高、cron 显示格式不一致）；当前无 test 覆盖双副本一致性（测试只测单一调用路径）。
- **建议**：将五个函数 + 三个共享常量提取到 `task_draft_job.h`（已有 trigger 常量先例）或新建 `agent_ui_shared.h`，两边 include 复用，消除双真源。actionChip 和 statusDotColor 的泛用性支持这一提取——它们不是"恰好相同"而是"同一份逻辑刻了两遍"。若不想扩 task_draft_job.h 的职责域，可另起 `agent_ui_shared.h`（头文件 inline，零链接成本）。

### 1.3 已核 — composeTaskDetailDialog 函数体完整搬运

- `git show a619418d` 的删除块（1298 行）与 `agent_detail_dialog.cpp:132-1418`（1287 行）行数差 = 11（含匿名命名空间头/尾、include 块等 redistributed boilerplate，非函数体缺失）。逐段对照搬运前（`a619418d^:agent_module.cpp` 的 composeTaskDetailDialog 段）与搬运后（`agent_detail_dialog.cpp:132-1418`）的 ui id、回调、分支结构：全部一致。
- 常数 `kDetailRunListLimit` 仍在 agent_module.cpp:247 且被 compose 函数（行 1249）消费，**不是死代码**——它被两份代码共用（主页面 compose 的 run 列表查询 + 详情弹窗的 run 列表渲染），单独一份保留的原因成立。

### 1.4 已核 — DraftJob / DraftPhase / 草稿 Job 三函数完整搬迁

- `DraftJob` 结构体（9 字段 + 8 行契约注释）从 `agent_module.h` → `task_draft_job.h:24-33`，逐字段一致。
- `DraftPhase` 枚举（4 值 + 注释）从 `agent_module.h` → `task_draft_job.h:37`，值序一致。
- `startDraftGeneration` / `reapDraftJob` / `applyDraftPrefill` 三函数体从 `agent_module.cpp` → `task_draft_job.cpp:22-170`，锁纪律（draftMu_）、epoch 失效逻辑、线程 join 顺序、LOG_ERROR 调用均完整保留。
- 触发类型常量（`kTriggerKeys` 等 6 组 + `triggerIndexOf` / `triggerLabelOf` / `joinCnList`）从 `agent_module.cpp` 匿名命名空间 → `task_draft_job.h:44-101`，`inline constexpr` 消除原 TU 边界，agent_module.cpp 零残留定义（grep 空）。

---

## 2. 回归面

### 2.1 LOW — agent_module.cpp 残留 detail dialog 专用常量（见 1.1）

- 六常量既无消费亦无守卫，属冷残留——不影响功能但增加维护混淆。不算回归，算清理遗漏。

### 2.2 已核 — 所有对搬走符号的引用点已更新

| 符号 | 原位置 | 现定义 | 引用方 |
|------|--------|--------|--------|
| `DraftJob` | `agent_module.h` 内部 struct | `task_draft_job.h:24` | `agent_module.h:29` include → `agent_module.cpp` 成员 `draftJob_`（行 431 等） |
| `DraftPhase` | `agent_module.h` 内部 enum | `task_draft_job.h:37` | `agent_module.cpp` 大量引用（行 2454/2551/2552/2564 等）——均通过 include 链可达 |
| `startDraftGeneration` | `agent_module.cpp` | `task_draft_job.cpp:22` | `agent_module.cpp:2514` 调用——经 agent_module.h:224 声明链接 |
| `reapDraftJob` | `agent_module.cpp` | `task_draft_job.cpp:102` | `agent_module.cpp:1624` compose 每帧调用 |
| `applyDraftPrefill` | `agent_module.cpp` | `task_draft_job.cpp:130` | `task_draft_job.cpp:124` reapDraftJob 内调用——同 TU |
| `composeTaskDetailDialog` | `agent_module.cpp` | `agent_detail_dialog.cpp:132` | `agent_module.cpp:2043` compose 调用——经 agent_module.h:205 声明链接 |
| `kTriggerKeys` 等常量 | `agent_module.cpp` 匿名空间 | `task_draft_job.h:44-101` | `agent_module.cpp` 多处（行 603/769/815/848/905/2061-2076/2157-2166/2181）——经 `agent_module.h → task_draft_job.h` include 链 |

### 2.3 已核 — CMakeLists 登记

- `src/module_agent/CMakeLists.txt:16-17` 新增 `task_draft_job.cpp` 和 `agent_detail_dialog.cpp`，在 `agent_module.cpp` 之前，顺序不影响链接。

### 2.4 已核 — include 链

- `agent_module.h:29` 新增 `#include "module_agent/task_draft_job.h"`——是唯一的 task_draft_job.h 引用点。无其他文件直接 include task_draft_job.h。
- `agent_detail_dialog.cpp:6` include `agent_module.h`——这是编译 composeTaskDetailDialog 成员函数定义的必要前提（需要 AgentModule 类定义）。

---

## 3. 平台对称性

- **已核**：`task_draft_job.cpp`、`task_draft_job.h`、`agent_detail_dialog.cpp` 三个新文件零平台宏（`_WIN32`/`__APPLE__`/`#ifdef` 均零命中）。业务代码继续遵守平台隔离铁律。

---

## 4. 静默失败面

### 4.1 已核 — 日志与错误分支完整保留

- `task_draft_job.cpp:96` `LOG_ERROR` 线程创建失败日志 → 原样保留。
- `task_draft_job.cpp:30-34` 空输入 → `DraftPhase::Failed` + error 文案 → 原样保留。
- `task_draft_job.cpp:37-43` LLM 不可用 → `DraftPhase::Failed` + error 文案 → 原样保留。
- `task_draft_job.cpp:49-52` 重复生成防护 → `DraftPhase::Failed` + error 文案 → 原样保留。
- `task_draft_job.cpp:62-63` `draftJob_` 占位防护 → mutex 锁纪律原样保留。
- `task_draft_job.cpp:74-84` LLM 请求失败 / parse 失败 → errorText 写入 + done release → 原样保留。
- `agent_detail_dialog.cpp` 内 LOG_WARN 调用（行 112-114）→ 原样保留。
- **结论**：无吞异常、无消日志、无丢断言。

---

## 5. 测试有效性

### 5.1 MEDIUM — 测试均经 AgentModule 接口，不直接验证拆分 TU

- **位置**：`tests/test_modules/test_agent_ui.cpp`（76 例）、`tests/test_modules/test_agent_draft.cpp`（27 例）等
- **问题**：全部测试通过 AgentModule 公开方法调用（openTaskDetail/startDraftGeneration 等），无测试直接 import `agent_detail_dialog.cpp` 或 `task_draft_job.cpp` 的内部符号。140/140 全绿证明**接口契约未破**（拆分前后 AgentModule 的 public 行为等价），但**不能检测**：
  - 1.1 死常量（不产生测试可观测行为差异）
  - 1.2 双副本漂移（编译通过、单元行为一致时不可见）
  - 未来只改一处副本时，测试仅覆盖一个调用路径
- **影响**：拆分本身不创造功能回归（已验证），但测试架构无法为后续双副本漂移提供安全网。
- **建议**：提取共享 helper（见 1.2）后，测试即可单一真源覆盖所有调用路径。在此之前，人工 code review 是唯一防漂移手段。

### 5.2 已核 — 拆分后测试不测"旧位置"

- agent_module.cpp 不再包含 composeTaskDetailDialog 定义 → 旧位置已无法编译。测试 140/140/0 = 证明**新位置编译通过且行为等价**，不是"测试还跑旧代码"的假绿。

---

## 6. 文档口径

### 6.1 LOW — agent_module.cpp 匿名命名空间注释描述已搬走内容

- **位置**：`agent_module.cpp:95-96`
- **内容**：`// 时长/状态/时间文案 helper 已迁 module_agent/agent_time_text.{h,cpp}（详情弹窗 P2，`——此注释准确描述了 agent_time_text 的搬迁，但未提及**本批**新增的两个拆分（task_draft_job / agent_detail_dialog）以及伴随拆分的常量迁移。
- **影响**：低——不影响正确性，但后续 AI 读此处注释时不会知道已有 5 个 helper 函数跨 TU 双副本存在。
- **建议**：在匿名命名空间开始处（行 98 前）加一句"本匿名命名空间的 statusDotColor/actionChip/navigateToClaudePage/cronTextOf/runLimitText 与 agent_detail_dialog.cpp 的匿名命名空间同值双副本；改任一处必须双改——未提取共享头文件是因为 X"，或在实施 1.2 建议（提取共享 helper）后自然消除双副本，注释不再需要。

### 6.2 已核 — 搬走的契约注释随代码走

| 契约注释 | 搬前 | 搬后 |
|----------|------|------|
| DraftJob 字段与线程契约 | `agent_module.h:158-173` | `task_draft_job.h:1-9`（文件头）+ `:24-33`（字段注释） |
| DraftPhase 主线程约束 | `agent_module.h:175-177` | `task_draft_job.h:35-37` |
| startDraftGeneration「生成中重复点击被挡」 | `agent_module.cpp` 原位置 | `task_draft_job.cpp:20-21` |
| reapDraftJob「compose 每帧调用」 | `agent_module.cpp` 原位置 | `task_draft_job.cpp:100-101` |
| applyDraftPrefill「只填可信字段、永不落库」 | `agent_module.cpp` 原位置 | `task_draft_job.cpp:128-129` |

- **原处无孤儿注释**：`agent_module.h` 不再有 DraftJob/DraftPhase 的定义或注释——它们被完整移除，未残留"此处原有 X"类注释。

### 6.3 已核 — agent_module.h 成员声明序契约保持

- `agent_module.h:237-238`：`scheduler_` 仍在 `loop_`（:235）/ `llm_`（:234）/ `store_`（:233）之后，注释"逆序析构下 Scheduler 先亡，其 run 线程 join 期间被引用的 loop_/store_ 仍存活"完整保留。

---

## 专项重点核验

### 副本形态

- `agent_detail_dialog.cpp` 的 5 个函数（statusDotColor/actionChip/navigateToClaudePage/cronTextOf/runLimitText）+ 若干常量（kDialogGap/kDialogLabelW/kDialogRowH/kDetailRunListLimit）使用匿名命名空间 static 内链接——**两 TU 各自一份，不造成外部链接重复/ODR 违规**。实现者"可接受"的判断在编译正确性上成立。
- **漂移风险**：见 1.2——同值常量/同逻辑函数两份并存，改一侧漏另一侧会主页面与详情页行为分歧。当前完全相同（git show 逐段对照确认），但无自动防漂移机制。

### 硬契约

- `scheduler_` 声明序在 `loop_/llm_/store_` 之后：`agent_module.h:233-238`，已核。
- ui id 前缀 `agent.dialog.*` 零变化：`agent_detail_dialog.cpp` 全量使用 `agent.dialog.detail.*`（:173/183/185/202/206/337/340/345/353/356/361/391/395/401/410/425/436/464/468/477/489/502/510/528/539/692/759/762/776/781），与拆分前 composeTaskDetailDialog 一致。常量断言守卫未被改动（无守卫存在于本批 scope 内）。
- 常量断言守卫：本批未碰 agent_module.cpp 或新文件中的任何 `static_assert` / `DOCTEST_CHECK`。

### 未拆块未误碰

- `composeTaskDialog`：`git diff a619418d^..a619418d -- agent_module.cpp` 零行命中 composeTaskDialog/composeDraftDialog/composeDeleteConfirm——确认仅移动 composeTaskDetailDialog，其余弹窗未触碰。
- `composeDraftDialog`：同上，零 diff 命中。
- `composeDeleteConfirmDialog`：同上，零 diff 命中。

---

## 汇总

| 严重度 | 数量 | 摘要 |
|--------|------|------|
| HIGH | 1 | agent_module.cpp 残留 6 个死常量（kDetailDialogH/kStopChipW/H/Y/kMsgAreaMinH/kMsgAreaMaxH） |
| MEDIUM | 2 | 5 函数+3 常量跨 TU 双副本漂移风险；测试不直接验证拆分 TU |
| LOW | 2 | 匿名命名空间注释未提及本批新双副本；kDetailRunListLimit 单独保留原因未写明 |

**专项重点三项结论**：
1. 副本形态：双 TU 匿名命名空间 = static 内链接，无 ODR 违规，但存在漂移风险（当前行为等价，无自动防护）。
2. 硬契约：scheduler_ 声明序 / ui id 前缀 / 常量断言守卫 → 全部已核，零违规。
3. 未拆块未误碰：composeTaskDialog / composeDraftDialog / composeDeleteConfirmDialog 零 diff 命中，确认未触碰。

审计文件路径：`D:/project/gui/wt-agent-split-20261019/docs/superpowers/plans/2026-10-19-agent-split-audit.md`