# 任务弹窗 UI 重做 spec — 独立审计 (skeptic)

> 2026-10-19 | 审计者：独立 skeptic agent（只读，不碰 src/）
> 对象：`docs/superpowers/specs/2026-10-19-task-dialog-redesign.md`（14 条裁决，3 阶段，自称与 P2 零冲突）

---

## 1. 「砍 2」裁决 — **LOW**

砍了折叠面板（EUI-NEO 无 Collapse，§3.1 论证充分）和自适应输入高（input 无内容撑高模式，定高 120 替代 96）。两项都在用户原话的「可选」段，非主诉求。论证成立。

## 2. 「现状已满足 1」核实 — **CONFIRMED**

`saveTaskDialog()` 在 `agent_module.cpp:787-790`：`if (goal.empty()) { editError_ = "请先填写任务目标"; return; }`。spec 裁决 N 准确。

## 3. 直落 11 条技术可行性 — **LOW**

逐条对照 EUI-NEO-CAPABILITIES.md 与代码实证：tooltip ✅·多行 input ✅·toggleSwitch ✅·dropdown ✅·绝对定位 ✅·dialog ✅·Collapse ❌（已砍）·fontWeight 限制 ✅（已认知）。`SecondaryThemed` ✅（`dialog_form.h:78`）·`actionChip` 的 `primary` 参数 ✅（`agent_ui_shared.h:42-47`）·`t.colors.primaryText` ✅（`theme.h:29`，`#D97757`/`#A64F2E`）·`t.colors.error` ✅（50+ 处使用）。零幻觉 API。

## 4. 断言翻新计划 — **LOW**

spec §7 列出 18 处冻结断言 + 逐条映射新断言。实证验证：panel 500→540（4 处 :2053/:3051/:3706/:4562）+ draft probe 处；goal 96→120（:2086-2087）、≥90→≥114（:2063/:387）；metaRow.y=+224→拆为新分组断言；meta.hint 渲染→拆为 tooltip 断言（:5261-5265）。清单完整，旧→新每对都有对应。行号是约数但偏差 ≤3（实测对齐）。新增断言 7 条（区域标题/说明行/tooltip/破坏性独立行）+ P-B 2 条（chip 取消 primary 态/track 色 error）。probe 段判据全部不受影响（:173-262，交互逻辑不变）。

## 5. Windows 度量兼容 — **LOW**

spec §5.4 明确给出跨平台策略：`.wrap(true)+maxWidth`、`lineHeight ≥ max(平台默认,18)`、`clip(true)` 兜底、§5.1 所有 y 坐标以 `kDialogGap`/`kDialogRowH` 为基准推导不引入裸字号。策略针对原问题（12px lineHeight 截断）有效，但未给 Windows 显式验证步骤（可低优补一行：真机 Windows 上目检中文不截断不叠加）。

## 6. UI id 前缀 — **CONFIRMED**

spec §4.2 列出的 40+ 现状 id，全部与 `agent_module.cpp` 实际代码一致（逐条 grep 证伪）。新增 9 个 id（§4.1）命名在 `agent.dialog.task.*` 前缀下，不与现状冲突。可能删除的 `trigger.hint`/`meta.hint` 处置明确。

## 7. 冲突声明核实 — **MEDIUM**

spec 称与 P2 「零冲突」，但引用的 P2 worktree 路径 `wt-rp2` **不存在**（`git worktree list` 无此项）。P2 改动已合入 dev（commit `5de60c68` 等，agent_detail_dialog.cpp + 时间线/run 详情），spec 的 scope（`composeTaskDialog` in `agent_module.cpp`）与 P2 scope（`composeTaskDetailDialog` in `agent_detail_dialog.cpp`）确实文件/函数都不交叠。**但「零冲突」声明基于不存在的引用——应改为「P2 已合入 dev、本 spec 的实现面（agent_module.cpp composeTaskDialog）与 P2 改动区（agent_detail_dialog.cpp）不交叠，仍建议等 P2 稳定后或在其之上分支」。**

## 8. 三阶段可交付性 — **LOW**

P-A（分组+文字）：布局断言翻新 + `ctest -L probe` 全绿；P-B（按钮+tooltip+警示色）：新增 P-B 断言 + probe 通过；P-C 零工作量（全砍/已满足）。每阶段独立验收口径明确。建议 P-A 交付时附 `home_probes --screenshot` 任务弹窗截图作几何补证。

---

## 总评

spec 的技术基础扎实：所有引用 API 经实证存在、UI id 清单与代码完全一致、断言翻新为每条旧断言配了新对应项。两项砍裁在框架限制下有充分论证，不损害用户核心诉求。唯一实质性缺陷：P2 冲突分析引用了不存在的 worktree，需改为基于 dev HEAD 的实证声明。其余为低保真度的精益化建议。

**结论：修后可进入实现**（修正第 7 项的 P2 引用后即可，无需重写）。