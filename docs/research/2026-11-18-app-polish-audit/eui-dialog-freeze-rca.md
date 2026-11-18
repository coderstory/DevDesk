# EUI-NEO dialog 过渡动画「双壳同帧互冻」RCA（clue#307 框架线）

- 日期：2026-11-18
- 角色：researcher（只读调研；构建 POC 未跑——收尾令中止）
- 对象仓：`EUI-NEO/`（coderstory fork @ 9054d99）
- 触发：need#300 / clue#307（FINAL-REPORT.md:57,68）；应用侧延帧规避已落（`src/module_schedule/schedule_ui_list.cpp:513-518,756-760`）
- 结论分级口径：**已确认**（本轮实码核对）/ **待验证**（证据指向但未实跑）/ **推测**（合理推断无实证）

---

## 一、结论先行（≤5 条）

1. **已确认**：dialog 透明度过渡是「声明式目标 + 每帧 tick」结构：`components/dialog.h:89-91` 由 `open_` 算出目标（visible/scale/offsetY），`:105-124` 挂到 backdrop(rect) 与 panel(stack) 的 `.opacity().transition().animate()`；runtime 侧每帧 `setTarget+tick`（`core/runtime/runtime_update.h:762-777` panel 走 `updateLayoutElement`、`:821-846` backdrop 走 `updateRect`）。状态机本体 `core/animation.h:259-348`（`AnimatedValue<T>::setTarget/tick`）。
2. **已确认**：状态机存在一个「目标-值脱钩可冻死」的缺口：`setTarget` 在 `closeEnough(target_, target)` 时直接早退（`core/animation.h:282-283`），**不校验 current_ 是否已到目标**。一旦某帧 target 已写入但 tick 因 deltaSeconds=0 未推进（`tick` 的 `localTime <= 0` 早退，`core/animation.h:305-310`），后续帧 setTarget 永远早退、只能靠 tick 补推——若该元素的 update pass 此后被跳过，值与目标将永久脱钩。
3. **已确认（入口之一）**：compose 后的二次 update 用 **deltaSeconds=0.0f**（`core/app/dsl_window_runtime.h:120-131` `composeRequested` 分支 `runtime_.update(window, 0.0f, ...)`）。同帧状态翻转（点击回调改态 → composeRequested → 二次 compose+update(0)）时，A/B 两壳的新目标恰是在 **delta=0 pass** 上被 setTarget 消费的——当帧两壳都不动。
4. **待验证**：37 帧持续冻结的最后一块拼图未钉死。静态分析下唯一能造成「持续不推进」的门控是 `canReuseStaticSubtree`（`core/runtime/runtime_update.h:284-313`，满足 9 条件即整子树跳过 update，不 setTarget 不 tick）——但 dialog 子树因 `elementNeedsUpdate`（transition.enabled → subtreeNeedsUpdate=true，`core/dsl.h:1839-1868`）带防线，常规情况不会被跳过。**何种状态组合绕过该防线未实锤**（POC 未跑，见 §四）。
5. **已确认**：同帧对偶开合与延一帧错开的差异点在框架侧真实存在：同帧 = 一次 compose 内两个 shell 目标同时翻转、同帧经 delta=0 pass；延帧 = 两个目标分别落在两帧的正常 delta pass。这与「错开即全链正常」现象吻合（`src/module_schedule/schedule_ui_list.cpp:514` 注释同口径）。

---

## 二、机制链（file:line 级，全部实码）

| # | 环节 | 锚点 | 内容 |
|---|---|---|---|
| 1 | dialog 声明 | `components/dialog.h:89-91` | `visible/panelScale/panelOffsetY` 全由 `open_` 单点算出 |
| 2 | 挂过渡 | `components/dialog.h:105-112`（backdrop）、`:117-124`（panel） | `.opacity(visible).transition(transition_).animate(Opacity[/Transform])`，默认 `Transition::make(0.16f, OutCubic)`（`:227`） |
| 3 | 常驻构建 | `components/dialog.h:80-130` | open(false) 照常建节点、仅 `.disabled(!open_)`——节点集不随开合变化 |
| 4 | 状态机 | `core/animation.h:274-330` | `setTarget`：未初始化→snap；目标等→早退；否则 `start_=current_; elapsed=0; active_=true`。`tick`：`localTime<=0` 早退不推进 |
| 5 | 每帧驱动 | `core/runtime/runtime_update.h:762-777`（stack/panel）、`:821-846`（rect/backdrop） | `setTarget(...)||changed` 后紧跟 `tick(deltaSeconds)`，`animating_` 汇总 `isLayoutAnimating/isRectAnimating` |
| 6 | 帧循环 | `core/runtime/runtime_lifecycle.h:97-155` | 每帧 `animating_=false` → 走树 → `previousFrameAnimating_=animating_`；**动画推进只发生在 update pass 内** |
| 7 | compose 后二次 update | `core/app/dsl_window_runtime.h:120-131` | `composeRequested` → `requestFullPaint + composeFrame + update(**0.0f**)` |
| 8 | 静态子树跳过 | `core/runtime/runtime_update.h:284-313` | 9 条件全满足→整子树跳过（无 setTarget/tick）；dialog 有 subtreeNeedsUpdate 防线（`core/dsl.h:1800-1820`） |
| 9 | app 侧触发 | `src/module_schedule/schedule_ui_list.cpp:513-518` | 点删除 → `editState_.open=false` +（修复前）同帧 `requestDeleteRule` 开确认壳 |

**机制一句话**：同帧对偶开合时两壳新目标在同一次 compose 内翻转，经 `dsl_window_runtime.h:126` 的 **delta=0 二次 update** 被 `setTarget` 消费（当帧不动）；此后「目标-值脱钩」能否冻死，取决于后续 update pass 是否到达这两个子树（第 4 条，未实锤）。

---

## 三、need#291「子树 max zIndex 排兄弟」是否同根？

**已确认：不同根。** zIndex 语义在 `core/dsl.h:1800-1822`（`rebuildOrderedChildren`：`subtreeMaxZIndex` 后序上界 + `stable_sort` 兄弟/根排序，`:264-266` 注释「弹层逃逸」），只影响**绘制/命中序**，不触碰 AnimatedValue 状态机。两壳 zIndex（deleteConfirm=1200，`schedule_ui_list.cpp:1267`）即便排序变化也不产生「透明度冻死」。clue#307 归因到 zIndex 排序线：**排除**（可从该线销案）。

---

## 四、未钉死点（POC 验证方法，移交框架线开发者）

**假说 H1（推测，静态最优贴合）**：同帧翻转后某壳满足 `canReuseStaticSubtree` 全部 9 条件被持续跳过。关键疑点在 `renderTransform.opacity==1.0f` 检查（`:311-312`）取的是**该元素自身**（stack root 无 opacity 属性，恒 1），而真正动画的 opacity 在**子元素**（backdrop rect / panel stack）上——root 级检查不感知子树动画。若某根缓存 paintBounds 的 `subtree.contains(pointer)` 为假（指针在两壳全屏 backdrop 之外/无效坐标），即可循环跳过。
**验证**：EUI `build-unit` 单测——compose A open/B closed 数帧→同帧翻转→固定 dt 连跑 40 帧→断言两壳 panel 实例 `opacity.value()` 递进；再分别注错 H1 条件（指针在子树外）与 H2（delta=0 二次 update）定位真凶。≤2 次构建可完成。

**假说 H2（推测）**：`delta=0` 二次 update（`dsl_window_runtime.h:126`）+ 主循环帧请求门控（`glfw_app_main.cpp:958-959` `anyAnimating` 决定帧节奏）形成「目标已写、tick 无粮」组合，后续帧 delta 恢复正常即应解冻——与 37 帧持续冻不符，单靠 H2 解释力不足，需与 H1 合并验证。

---

## 五、修复方案卡（2-3 选项，取舍与落地性）

### 方案 A（推荐）：过渡状态机失步自愈
- **改点**：`core/animation.h:274-283` `setTarget` 早退分支加失步检测：`target_==target && !active_ && !closeEnough(current_, target_)` → 视为脱钩，走重新激活路径（`start_=current_; elapsed=0; active_=true`）。
- **价值**：根治所有「target 已写但值未到」的冻死态，覆盖 H1/H2 两种触发；不改变正常路径行为（脱钩态在正常流中不存在）。
- **成本/风险**：约 5 行；风险=可能掩盖上游「为什么脱钩」的真 bug（需配日志或测试）。POC 未跑，状态机改动需单测覆盖。
- **三问门**：能否落地=改点明确 file:line 可直接改；100% 覆盖=覆盖两壳互冻与任何单壳脱钩；生产级参考=本仓 `core/animation.h` 即唯一实现，无外部依赖。**待验证：需 POC 证实 H1/H2 后确认此改点足以解冻。**

### 方案 B：同帧合并调度——二次 update 不吃真实目标变更
- **改点**：`core/app/dsl_window_runtime.h:126` 的 `runtime_.update(window, 0.0f, ...)` 改为透传真实 delta（或 compose 前先 update 消费目标、compose 后仅 repaint）。
- **价值**：消除「目标在 delta=0 pass 被消费」的当帧冻结；改动 1 行。
- **成本/风险**：语义风险高——delta=0 二次 update 是有意的（避免状态文本重复消费时间），透传 delta 可能让其它动画双倍推进；需要审计所有依赖 update(0) 的路径。
- **三问门**：落地性存疑（影响面未审计）→ 不推荐独立采纳。

### 方案 C：文档化契约 + 应用侧延帧（现状）
- **内容**：把「同帧对偶开合会互冻」写进 `EUI-NEO/docs/` 组件契约与 DevDesk `docs/EUI-NEO-CAPABILITIES.md`；应用侧维持延帧规避（schedule 已落，`schedule_ui_list.cpp:514-518`）。
- **价值**：零框架风险；**成本**：每个模块手工规避（claude_proxy/todo/mcp/agent/release 等 dialog 使用方共 10+ 处，`grep components::dialog` 见 §六），漏一处即复发。
- **三问门**：能落地、覆盖需逐模块补、无生产级框架修复——作为过渡方案保留，不作终态。

**推荐**：A 为主修复（框架级根治），C 作为 A 落地前的既有护栏保留；B 不采纳。

---

## 六、同型面扫描（open-close 过渡组件，静态列举）

**已确认使用同款「声明式目标+AnimatedValue tick」机制的组件面**（风险=同帧对偶开合可复现同款冻结）：

| 组件 | 文件 | 备注 |
|---|---|---|
| dialog | `components/dialog.h` | 本缺陷主面（backdrop+panel 双动画元素） |
| dropdown | `components/dropdown.h` | 未精读其 open 过渡实现，风险待扫 |
| contextmenu | `components/contextmenu.h` | 同上 |
| styledmenu | `components/styledmenu.h` | 同上 |
| toast | `components/toast.h` | 同上 |
| tooltip | `components/tooltip.h` | 同上 |
| sidebar / navbar / carousel | `components/sidebar.h` 等 | 同上 |

- **已确认的 app 侧同构场景**：`components::dialog` 使用方 10+ 处（`src/ui_kit/dialog_shell.cpp:81`、`src/ui_kit/dialog_form.cpp:12`、`src/module_mcp/mcp_page_module.cpp:795`、`src/module_agent/agent_detail_dialog.cpp:143`、`src/module_agent/agent_module_dialogs.cpp:521`、`src/module_release/release_detail_draw.cpp:2432,2533,2691`、`src/module_todo/todo_ui_dialogs.cpp:211,848`、`src/module_notepad/notepad_window.cpp:878`、`src/module_claude_proxy/claude_ui_form.cpp:589`）——凡「关 A 开 B 同帧」的链路都可能踩同款；schedule 已延帧，其余未排查。
- **诚实声明**：以上组件的 open-close 过渡实现**未逐个精读**（预算耗尽），仅确认 dialog 本体与 AnimatedValue 为共享机制；同型面精确计数待框架线按 §四 POC 方法逐组件复测。已知确认计数：**1 个主面（dialog）+ 6 类待扫面 + 11 处 app 使用点**。

---

## 七、遗留

1. 37 帧冻死的最后触发点未实锤（H1/H2 假说，验证方法见 §四）——移交框架线 developer 跑 POC。
2. 同型面 6 类组件逐个精读未做。
3. 方案 A 的状态机改动需单测（建议入 `tests/unit/`，参照 `signal.cpp:43` 的 dialog 测试基建）。
