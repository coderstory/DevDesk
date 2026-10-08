# 组 B 独立审计（claude 详情：四页签锚定 / 行数按可用高推 / env 全量来源 / 片段区块钉底）

- **审计对象**：分支 `worktree-agent-a56855aa9dd3d85be`，唯一提交 `72b0c4a`（base `2839c99`，11 文件 +1081/−214），worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-a56855aa9dd3d85be`
- **审计者**：独立只读子任务（非实现者、未参与本轮改动）
- **审计日期**：2026-09-29
- **纪律声明（哪些做过 / 哪些没做）**：
  - 做过：`git show 72b0c4a` / `git diff 2839c99..72b0c4a` 通读；源码 + EUI-NEO 上游（components/scrollview.h、components/tabs.h、core/dsl.h、core/runtime/runtime_update.h）只读阅读与 grep；`git log -S` / `git blame` 归因复核；**实机产物（`/private/tmp` 下的截图 / 失败清单 / 日志）只读像素与集合分析**；`comm` 独立复算失败用例差集；变异验证为**静态推演**（手算 + 元素 id/几何推导），非实跑。
  - **没做**：没有构建、没有跑 `ctest`、没有跑 probe、没有启动 app、没有改任何实现代码或测试（故"变异后应转红"为静态推演，依据逐条写在各 finding 的判据里）。
  - 唯一写动作 = 本文件。

---

## 1. 六维度结论（每条一句话）

| 维度 | 结论 |
|---|---|
| 1 正确性 | **四条诉求全部达成，且经实机截图独立复核**：①四页签动作区钉同一条线（`final/*.png` 四张的「启用」按钮 bbox **完全相同** `(1384,1492)-(1600,1560)`）②「最近调用」按可用高推 13 行 + 「另有 17 条」（旧实现锁 10 行且无提示）③「环境变量」列 buildProxyEnv 合并结果 + 每键来源（图上 6 行「托管键」+「另有 10 项未显示」，总 16 键）④「+ 新建片段」钉在「+ 新建 Profile」行上方 12pt、块内滚动条可见。算式 `actY = bodyH − 85` 与绘制几何逐项对得上（内容框底→分隔线→按钮 = 20.25pt / 9.75pt，实测）。 |
| 2 回归面 | `recentCalls` 快照 3→30 **唯一消费点 = `drawDetailCallsPane`**（全仓 grep：`v.recent` 只此一处、`cachedRecent_` 只在 state 单元），无其他预览/内存/查询路径；`buildProxyEnv` 的 `sourcesOut` 是**默认 nullptr 的可选出参**，生产唯一既有调用点 `settings_sync.cpp:300` 传 5 参零改动、无"传了却忽略"的调用点；`actY` 兜底分支与 `sharedScrollOffset_` 见 F-03/F-09。**发现 1 条真死字段（F-01）**。 |
| 3 平台对称性 | **零平台宏、零 macOS 特有数值**：全部锚定式（`bodyH − kActReserve`、`sharedBlockBottomY(bodyH)`），新增常量（18/61/85/64/240…）都是从绘制几何量出的逻辑量，两平台同式；测试/probe 用显式逻辑尺寸（1262×801）与真窗解耦 → 平台无关。**已核，无问题**。 |
| 4 静默失败面 | 三个 `max(0,…)` 兜底齐全、除法分母恒 > 0（`kEnvRowPitch`/`kRowPitch`），极端小窗下 `floor` 结果恒 ≥ 0、不画越界行、有明文提示；滚动内容高为 0 时不产生 NaN（`scroll.h:68` 除数是 content 而非 viewport）。**但 0 行档破了"内容不越 contentBottom"这条自己立的规矩（F-02）**，且两处新守门是恒真的（F-06）。 |
| 5 测试有效性 | **非恒真，已静态复核两条变异**（F-03/F-06 反证 + §4）：①还原 `actY=paneBottom+20` → 四值 332/340+20/456/260 互不相等 → `claude_detail_actions_anchor_four_tabs` 必红；②还原 `clamp(…,3,10)` 或把快照退回 3 → 画出行数 10 / 3 ≠ 13 → `claude_calls_rows_by_available_height` 必红（该用例**同时**锁住数据侧上限与显示预算，判别力最强）；③`claude_shared_block_bottom_anchored` 在旧 clamp 几何下 blockBottom=752 ≠ btnY−12=531 → 必红。probe 三档断言无可恒真项（含 1 条按构造恒等的 `btnBottom==blockBottom`，见 F-02 判据与 §4-5）。 |
| 6 文档口径 | **发现 4 处旧描述未同步（F-10）**：probe/单测注释里"区块可向下溢出按钮区"、计划 §1 组 B 的 `contentBottom = min(paneBottom, actY−20)`、probes 设计文档的"粘性脚注"、`.h` 里 bodyH 的"内容框/行数预算唯一来源"（已死，F-01）。**新增文档本身与实现一致**（`kRecentCallsSnapshot` 的"不是显示上限"注释、P1 统一原则、EnvSource 分层除 F-04 一处写反）。 |

**findings 总数 11 条（Medium 2 / Low 9 / High 0）+ 1 条报告差异项（A-01，无严重度）**；**无阻塞合并项**（§5）。

---

## 2. findings（每条 = file:line + 问题 + 影响 + 建议 + 判据 + 严重度）

> `impact` 写维护成本 / 演进代价；`severity` 按"多难维护 / 多难演进"定。触发条件一律给出算术边界。

### F-01 [Medium] `DetailView::bodyH` 已成死字段，注释还宣称它是"行数预算唯一来源"

- **位置**：`src/module_claude_proxy/claude_ui_detail.h:55`（字段 + 注释"容器可用高（内容框/行数预算的唯一来源）"）、`src/module_claude_proxy/claude_module.cpp:147`（`dv.bodyH = bodyH;` 每帧写）
- **问题**：组 B 之后两个"行数预算型" pane 都改用 `contentBottom`（`claude_ui_detail.cpp:827` 由 bodyH 在 `drawDetailPanel` 内推出），`bodyH` 全仓**零读取**（`grep -rn "\.bodyH" src/module_claude_proxy/` 只回 1 处写入）。
- **影响**：后来者按字段注释拿 `v.bodyH` 算预算，会得到与真实裁剪下沿（`contentBottom = bodyH − 105`）差 105px 的另一个量 —— 这正是本次修的病根（"容器底与内容底不同源"）换了个马甲；结构体每帧多拷一个无用 float。
- **建议**：删字段（含 `claude_module.cpp:147` 那行），或保留但把注释改成"仅 claude_module 内部用；pane 侧预算请用 drawDetailPanel 传下的 contentBottom"。
- **判据**：`grep -rn "\.bodyH" src/module_claude_proxy/ | wc -l` → 期望 0（读取）或注释已改；现状 1（写）。
- **严重度**：Medium（注释主动误导，改错一处就退回原病）。

### F-02 [Medium] 0 行档：两串文字叠印在同一坐标，且"另有 N 条"/"查看全部"越出内容框

- **位置**：`src/module_claude_proxy/claude_ui_detail.cpp:479-487`（`shown==0` 画 `claude.calls.empty` 于 `(colTimeX, cellsY)`）、`:541-551`（`recent.size() > shown` 时画 `claude.calls.overflow` 于 `(colTimeX, cellsY + shown*kRowPitch)` = 同一坐标）、`viewY` 计算 `:562-564`
- **问题**：`shown == 0` 时 `callsOverflow` 仍为真（`recent.size() > 0`），两个 text 元素落在同一 y → 字符互相覆盖；且 0 行档的预留判据（`shown*kRowPitch + kOverflowH > rowsAvail` 的 while 循环）在 `shown == 0` 时**空转**，溢出提示行(18) 与「查看全部」带(8+24) 可整段落到 `contentBottom` 之下（算例：480 高 → bodyH=286 → rowsAvail=13 → 溢出行占 13..31、查看全部 39..63，contentBottom−cellsY 只有 13）。
- **影响**：这是本次唯一"越界"的残留（与 commit message 宣称的"不画越界行"相反——行没越界，**提示带**越了）。触发域：`bodyH < 297` ⟺ 客户区高 < 491，现最小窗（1330×845）不可达，但**新单测 `claude_calls_no_rows_when_too_short`（480 高）正落在此域**，而该用例用 `find`+`text` 断言，看不见叠印与越界 → 假绿面。日后任何"把最小窗调小/给此 pane 换尺寸"的改动会先撞到这里。
- **建议**：`shown == 0` 时 `callsOverflow` 置假（只留"内容区高度不足"一句，或把"另有 N 条"并进该句子）；两条溢出提示行的预留口径统一（env 用 20、calls 用 18，同一 18px 文本两处不一致）。
- **判据**：480 高下新增断言 `ui.find("claude.calls.overflow") == nullptr || overflow->frame.y > empty->frame.y + empty->frame.height`；以及 `viewall->frame.y + 24 <= contentbox->frame.y + contentbox->frame.height`（contentbox 元素已在树里，`claude.detail.contentbox`）。

### F-03 [Low] 两条 `max()` 兜底把"四页签 actY 全等"变成有条件等式（注释写成无条件）

- **位置**：`src/module_claude_proxy/claude_ui_detail.cpp:862`（`actY = max(paneBottom + kActTopGap, actYFloor)`）、`:62` / `:185`（basic / models pane `(void)contentBottom`）、`.h:20-30` 与 `:846-851` 注释"四个页签的 actY 相等"
- **问题**：基础信息 / 模型映射两个 pane 完全无视 `contentBottom`（固定 6 字段 / 6 槽位表行），其 `paneBottom` 分别恒为 312 / 340（`y0=112` + 38×6）。于是当 `paneBottom + 20 > actYFloor`（即 `bodyH < 445`，模型的约束更紧）时 `max()` 取左支，动作区随内容下移 → 四页签 actY 不再相等，且 `actY + 61` 可越面板底。
- **触发边界**：`bodyH ≥ 445` ⟺ 客户区高 ≥ 639。现最小窗 `kMinWindowLogicalHeight = 845`（`src/main.cpp:425`，`:544`）→ 客户区 ~801~805 → bodyH 607~611，**余量约 162px，当前不可达**。
- **影响**：注释把等式写成"不变量"（`.h:24-30`、`:846-851`），后来者若按它在小尺寸上写断言（probe/单测）或调小最小窗，会立刻红且**根因埋在 `(void)contentBottom` 里**（basic/models 是唯一两个"内容可以比容器高"的 pane）。属"注释承诺 > 实现保证"。
- **建议**：注释补条件（"bodyH ≥ 445 / 客户区 ≥ 639 时成立；更矮时 `max()` 分支接管，动作区随内容下移"）；或给 basic/models 也接 `contentBottom`（超框时用现有的"另有 N 项"口径表达）。
- **判据**：在 1000×480（bodyH=286）下断言四页签 actY 相等 → **现在必红**（基础 332 / 模型 360 / env 201 / calls 206），这条红即该 finding 的证明。

### F-04 [Low] `EnvSource` 的合并优先级注释写反（与规则 5 矛盾）

- **位置**：`src/module_claude_proxy/provider_config.h:214-216`
- **问题**：注释"优先级 片段 > profile 自身 > 托管键"把托管键放到**最低**；实际写入顺序（`provider_config.cpp:355-398`）是 profile → 片段 → 托管键，后写者赢，即 **托管键最高**（同文件 `:355` 注释"托管键随后写入 → 冲突时托管键优先（规则 5）"、`settings_sync.cpp:298`"托管键在最上层"）。括号里的"即后写的层赢"也与它列举的顺序不符。
- **影响**：改 env 合并 / 加新来源层的人（或写"来源优先级"用例的人）会照这条注释推出相反结论；本条不涉及代码行为（代码是对的），但它是**合并规则的唯一声明处之一**。
- **建议**：改成"托管键 > 片段 > profile 自身（写入顺序 profile → 片段 → 托管键，后写者赢）"。
- **判据**：`grep -n "优先级 片段" src/module_claude_proxy/provider_config.h`（现状 1 命中）；与 `grep -n "托管键优先" src/module_claude_proxy/provider_config.cpp`（1 命中）对照即矛盾。

### F-05 [Low] "生效 env" 语义被夸大：显示的是托管合并结果，不是 settings.json 的 env

- **位置**：`src/module_claude_proxy/claude_ui_detail.cpp:227-230`（"**= 启动代理接管时写进 settings.json 的那份 env**"）、`:249-253`（空态文案"无生效环境变量"）
- **问题**：`settings_sync::takeOver` 写入的是 `既有 env ∪ 托管键`（非托管键原样保留，`settings_sync.cpp:363-390` 的 kept 循环），且上层覆盖检测会**跳过被更高优先级文件压过的托管键**；代理未运行时 settings.json 里根本没有这份表（此时是"未接管"态，图上 `● 已停止`）。
- **影响**：排障时把这页当 settings.json 的 ground truth 会错——恰好在"未接管 / 有上层覆盖"这两个最需要查的场景错。
- **建议**：注释与空态文案改成"将写入的托管 env（接管时生效）"。
- **判据**：对照 `settings_sync.cpp:363-390`（保留非托管键）与同文件的托管键覆盖检测分支；UI 侧 `grep -n "生效环境变量" src/module_claude_proxy/claude_ui_detail.cpp`。

### F-06 [Low] 共享区块的"守门"是恒真断言（写成了身份式），真正的退化形态无人守

- **位置**：`src/module_claude_proxy/claude_ui_list.cpp:406-416`（`assert(sharedBlockTop + sharedH <= bodyH)` + 同条件的 `LOG_ERROR`）
- **问题**：`sharedBlockTop = sharedBlockBottomY(bodyH) − sharedH = bodyH − 76 − sharedH`（`:59-62`、`:410` 上一行），故 `sharedBlockTop + sharedH ≡ bodyH − 76`，断言**恒真、永不触发**（Release 编掉、Debug 也只在几何改写时才可能命中）。plan §5-B③ 要求的那条断言因此零保护力。区块真正的退化是 `sharedH < kSharedFixedH + kSharedInfoH = 94` 时标题行 / 空态行与「+ 新建片段」重叠（`sharedH = min(自然高, bodyH−240)` → 触发域 `bodyH < 334` ⟺ 客户区 < 528，现最小窗不可达）——没有守门。
- **影响**：读代码的人会以为"越界已有守卫"，从而在调区块高度/片段数上限时少一道自检；实际守的是恒等式。
- **建议**：断言换成有判别力的量，例如 `sharedBlockTop >= kListTopY + kSharedListMinH`（列表保底）或 `sharedH >= kSharedFixedH + kSharedInfoH || sharedConfigs.empty()`（块内不重叠）。
- **判据**：手算 `blockTop + sharedH ≡ bodyH − 76`（代入 `:59-62`）；或把 `sharedBlockMaxH` 改成返回 `bodyH`（制造越界）→ 断言仍绿 = 恒真的直接证明。

### F-07 [Low] `kBottomBtnRowH` 只落了一半：绘制侧仍写字面量 `bodyH - 64.0f`

- **位置**：`src/module_claude_proxy/claude_ui_list.cpp:51`（新常量）、`:59`（`bottomButtonRowY`）、`:575` / `:587`（两个底部按钮仍 `.position(20.0f, bodyH - 64.0f)`）
- **问题**：锚点（`sharedBlockBottomY` → `bottomButtonRowY`）走常量，被锚的对象（按钮行）走字面量 —— "高度只有一个来源"的原则只落了一半，两处可各自漂移。
- **影响**：改按钮行高（如 64→72）时区块锚点不动 → 区块底与按钮行间距静默变化（新用例会红，但改的人要先去撞红才发现该改两处）。
- **建议**：两处绘制改 `bottomButtonRowY(bodyH)`。
- **判据**：`grep -n "bodyH - 64.0f" src/module_claude_proxy/claude_ui_list.cpp` → 期望 0 命中（现状 2）。

### F-08 [Low] `drawSharedConfigBlock` 的 `y` 形参恒为 0，且文档说它是"绝对坐标"

- **位置**：`src/module_claude_proxy/claude_ui_list.h:84-88`（"y = 区块在面板内的起点（绝对坐标，块内元素相对块顶定位）"）、`claude_ui_list.cpp:613`（定义）、三个调用点 `:435` / `:454` / `:564` 全传 `0.0f`
- **问题**：调用方统一传 0，块的绝对位置由外层 `ui.stack("claude.shared.block").position(20.0f, sharedBlockTop)` 决定（`:433` 等）→ `y` 是**块内相对原点**，且恒 0；`.h` 的"绝对坐标"描述与实现相反。
- **影响**：后来者按注释传非 0（以为能挪块）→ 标题/按钮相对块偏移、块本身不动，排查成本高（且 `.position(0.0f, y)` 的 y 与块坐标叠加是隐性耦合）。
- **建议**：删掉形参（块内一律从 0 起算），`.h` 同步。
- **判据**：`grep -n "drawSharedConfigBlock(ui, 0.0f" src/module_claude_proxy/claude_ui_list.cpp` → 3 命中（证明恒 0）。

### F-09 [Low] `sharedScrollOffset_` 无复位路径（片段集变化 / 空态切换后残留）

- **位置**：`src/module_claude_proxy/claude_ui_state.h:97-100`（成员）、`claude_ui_list.cpp:666`（唯一写者：`onChange` 回写）
- **问题**：块内滚动偏移只有"用户滚动"一个写入口，没有"片段集变化 / 空态切换 / 内容变短"的复位；`contentKey`（`:672`）变化只触发重新测高、不重置偏移。渲染侧由 `ScrollViewBuilder` 每帧 `clamp(offset_,0,maxOffset)`（`EUI-NEO/components/scrollview.h:81`）兜住，故**不会画错**；但成员会停在旧值，片段改多后跳回旧偏移。
- **影响**：观感级缺陷 + 状态卫生问题（一个只在用户操作时收敛的状态，跨帧/跨数据集残留）；与 `cardsScrollOffset_`（`module_settings/settings_module.cpp:921`，另一个模块的成员）**无干扰**（已 grep 确认两者各只有本模块引用，`ProfileListState` 内也没有第二个 offset 与它混用）。
- **建议**：`scrollKey` 变化时把 `st.sharedScrollOffset_` 归零；或干脆不持久化（交给 `Ui::state` 的 scrollState 自持）。
- **判据**：`grep -rn "sharedScrollOffset_" src/` → 1 写 1 读、无复位分支；行为判据：滚到底 → 删到 1 个片段 → 再加回 8 个 → 断言渲染偏移为 0。

### F-10 [Low] 四处旧文档/注释与新行为矛盾（实现者只报了 0 处）

- **(a)** `tests/test_modules/test_claude_ui.cpp:1445-1448`："500 属极端矮窗…区块按既有口径可向下溢出按钮区" —— P4 起区块钉底不再溢出（该分支 `h>=720` 才断言，注释已失义）。
- **(b)** `docs/superpowers/plans/2026-09-28-macos-layout-unified-plan.md:70`：要求 `contentBottom = min(paneBottom, actY − 20)`；实现选择"固定框 = `actYFloor − 20`"（更有说服力——框恒定才能消灭"无法解释的空白"，见 commit message），但计划未标注偏离，读计划的人会以为框随内容收缩。
- **(c)** `docs/superpowers/specs/2026-09-28-devdesk-probes-design.md:152`：仍称共享区块为"粘性脚注"，且该处第 2 条断言（"滚到底"序列后末张卡 frame ⊆ 区块上方区域）**至今未实现**（本次只补了尺寸矩阵与锚定断言）。
- **(d)** `.h` 里 bodyH / 形参 y 的两处描述 → 见 F-01 / F-08。
- **影响**：后来者按旧文档推理"溢出 / 脚注滚动"会与现状相反；plan 是"未实施"档，但它是本轮唯一方案档。
- **建议**：(a) 改注释；(b) 在 plan 顶部"来源与状态"加一行"实施时 P1 内容框改为恒定框（偏离 §1 组 B 原文）"；(c) 同步措辞 + 把未实现断言登记为待办。
- **判据**：`grep -n "可向下溢出按钮区" tests/test_modules/test_claude_ui.cpp`；`grep -n "min(paneBottom" docs/superpowers/plans/2026-09-28-macos-layout-unified-plan.md`；`grep -n "粘性脚注" docs/superpowers/specs/2026-09-28-devdesk-probes-design.md`。

### F-11 [Low] 工作树残留未跟踪文件 `NUL`（Windows 风格重定向在 macOS 生成实体文件）

- **位置**：`<worktree>/NUL`（30 B，内容 `sh: mklink: command not found`，09:22）、`<worktree>/build/tests/NUL`（同款，02:06）
- **问题**：`> NUL` 在 macOS 不是黑洞而是文件名；`git status --porcelain` 因此非干净（`?? NUL`）。该残留同时是**方法可信度的直接反证**：基线比对流程里至少有一条 `mklink`（建硬链接副本？）命令**静默失败**——错误信息被写进 NUL 文件而没有出现在终端。
- **影响**：①`git add -A`（本项目禁止但人可能手滑）会提交垃圾；②说明相关脚本/一次性命令没有 `set -e` 级别的失败可见性。
- **建议**：删两个 NUL；命令改 `> /dev/null`；涉及"基线副本"的步骤改成显式 `cp` + `ls -l` 校验（见 §4-6）。
- **判据**：`git -C <worktree> status --porcelain`（本次实测输出 `?? NUL`）；`cat <worktree>/NUL`。

### A-01 [报告差异·无严重度] "三路证据（日志/单测/截图像素）"中"日志"一路在产物里不存在

- **事实**：实现者报告称四页签 actY 全等有"日志/单测/截图像素"三路证据。我复核：**单测**✓（用例断言四值相等）、**截图像素**✓（下 §3）。**日志**：保存下来的实机日志 `/private/tmp/groupb-shot/logs/devdesk.log`（11604 B，02:18 启动）里 `[layout]` **0 命中**——该行是 `LOG_DEBUG`（`claude_ui_detail.cpp:866-871`），默认级别 Info 不打印，而全 `/private/tmp/*.log` 亦无一条 `[layout]`（`grep -l "\[layout\]" /private/tmp/*.log` 空）。
- **影响**：这条证据链只能靠"事后把设置页日志级别切到 Debug 再复跑"补，当前无法从产物复核；对审计结论无影响（另两路已够）。
- **建议**：无（若要留痕，捕获一次 Debug 级实机日志或把该行降为 Info 一次）。**差异性质：夸大（声称有、产物无），非错报。**

---

## 3. 我独立做过的验证（证据 / 数字）

**A. 实机截图像素（`/private/tmp/groupb-shot/final/*.png`，2940×1912 = 1470×956 点 @2x，窗口最大化）**

| 检查 | 结果 |
|---|---|
| 四页签动作区同线 | 四张图（tab0/1/2/3 + full-shared-block-pinned）里「启用」主色按钮 bbox **完全相同** = `(1384,1492)-(1600,1560)` → 四页签锚同一条线（用户主诉①的像素级确认） |
| 内容框 / 分隔线 / 按钮三段间距 | 单列剖面：内容框底边线 1451-1452px → `claude.act.divider` 1472-1473px → 按钮顶 1492px = **20.25pt / 9.75pt**，与 `kActTopGap=20`、分隔线在 `actY−10` 逐项吻合 |
| 面板底 | 左右两栏底边线同在 1652-1654px；页面底 1732px → 根 column 子项总高**恰好**装下（`kGuideBarH` 修法的可见效果：此前多占 4px 会压缩底部 padding） |
| 「最近调用」行数 | 13 行 + 「另有 17 条（点「查看全部」）」 → 反解 `bodyH ∈ [603,627)`，与 `bodyH = 607 / actY = 522` 一致（行数对预算敏感：每 ±24pt 变一行） |
| 「环境变量」全量 + 来源 | 6 行 + 「另有 10 项未显示」（总 16 键），每行第二行首列来源标签（图上 6 行皆「托管键」）；行数与公式 `min(n, ⌊(contentBottom−y0)/54⌋)` 再让溢出位 = 6 一致 |
| P4 钉底 | 「+ 新建片段」紧贴「+ 新建 Profile / 导入当前配置」行上方（12pt 间距），碎片卡片区自带滚动条（片段 A..E + 滚动）→ 卡片可达、按钮恒可见 |
| 引导条状态 | 图上显示引导条且 `● 已停止` → 复算全部采用 `bodyH = screen.height − 194`（与 `claude_module.cpp:94-95` 一致） |

**B. 失败集差集（独立复算，`comm`）**：`/private/tmp/fail_base.txt`（68 行，02:16）vs `fail_mine2.txt`（64 行，09:22）：去重后 66 vs 62，**`only-in-mine = ∅`** ✓；`only-in-base = 4` = 3 条 `todo_ui` 拖拽/列滚动 + 1 条 `settings aboutVersionLine` —— 四条全部与组 B 无关（组 B 未触 todo/settings），且其中 `aboutVersionLine` 的修复（master `e0fb5df`）提交于 **09:18**，晚于 baseline 采样（02:16）→ 与"基线二进制≠base 提交"相容（见 §4-6）。
**C. 变异静态推演**：见 §1-5 与各 finding 判据（未实跑）。

---

## 4. 实现者主张独立复核清单

| # | 主张 | 复核方法 | 结论 |
|---|---|---|---|
| 1 | 四页签 actY 全 = 522 | 算式复算（`bodyH=607 → actY=522`，`:44/:826/:862`）+ 截图行数反解 bodyH∈[603,627) + 四页签按钮 bbox 相同 | **一致**（522 落在反解区间内；绝对像素因标题栏口径差 ±3pt，不影响不变量） |
| 2 | `85 = 61 + 24` 与源码动作块高一致 | 逐元素核 `drawDetailActions`：分隔线 actY−10 / 行 actY..+36 / 副提示 +40..+56 / 分隔线 +60..+61（`:595/:607/:731/:742`）；下行占用 61，+24 呼吸 = 85 | **一致**（另：audit-findings 的 145 其加数只和到 105，实现者的 85/105 拆法自洽） |
| 3 | 只修行数预算不修 `recentCalls(3)` 只拿得到 3 行 | grep 全仓消费点（唯一 `drawDetailCallsPane`）+ 算式 `shown=min(3,13)=3` 且不产生溢出提示 | **一致**（因果成立；新用例同时锁住数据侧上限） |
| 4 | `9811225` 是把区块改坏的提交 | `git show 9811225` 冲突消解原文（列表换 virtualList + 区块改绝对定位 + `listH=clamp(…,min(96,scrollH),…)`）；`git log -S "min(96.0f, scrollH)"` 仅命中本次 72b0c4a；对比 `a0f9c32`（区块在 scrollView content 尾部） | **一致（方向与提交号正确）**；**但"原本底部对齐"这层前提在代码里不成立**——pre-merge 形态是"滚动内容尾部"，只有列表填满视口并滚到底才在底部，空/错态分支更是写死 `cardY0+96` 的绝对坐标 → "钉底"是本次**新设计**，不是回退 |
| 5 | 新增用例在回退时会变红 | 静态推演三条（§1-5）；probe 断言逐条判是否有恒真项 → 无可恒真（唯一按构造恒等：`btnBottom==blockBottom`，因按钮就是块内定位；三档尺寸下另两条有判别力） | **一致**（均为静态推演，**未实跑**） |
| 6 | 零新增失败（与基线二进制逐条比对 only-in-mine = ∅） | `comm` 独立复算 = ∅ ✓。方法可信度：①口径本身有效（失败**用例名**集合差集）②**基线身份不可考**——产物里没有基线二进制、没有指向 base `2839c99` 的构建日志（`baseline-build.log` 实际指向 `/tmp/build-modkeys-verify`，属另一任务的目录），且 4 条 only-in-base 恰好是 master 侧后来修掉的用例 → 基线更像"当时的某个 master 二进制"而非 base 提交 ③**差集方法对"同一条用例失败原因变了"是盲的**（本例 64 条共红里仅 3 条 claude 相关：2 条 `claude_service` + 1 条 `claude: Ctrl+N…`；我读了后者断言，只查弹窗/表单/按键消费/代理状态，无详情几何断言 → 组 B 改其失败原因的风险可忽略）④另有 F-11 证据：比对流程里一条 `mklink` 静默失败。 | **与本报告一致（∅ 我复算为真）**；方法口径有效，但"基线=base 提交"不成立、"原因变化"盲区需知；**未独立复现**（我没跑测试） |
| 7 | `buildProxyEnv` 零改动既有调用方 | grep 全部调用点：`settings_sync.cpp:300`（5 参）+ 测试 4/5 参；`sourcesOut` 默认 nullptr，传指针处只有新 UI 调用且用了返回值 | **一致** |

---

## 5. 合并建议

- **无"必须修完才能合并"的项**：四条用户诉求全部达成（截图像素 + 行数/键数算式双重确认）；可达域（最小窗 1330×845 / 客户区 ~801）内无回归、无越界、无静默少画；失败集差集 only-in-mine = ∅（我独立复算）。
- **建议合并前顺手修（各 1 处，成本 < 10 行）**：**F-04**（注释把合并规则写反，是后来者最可能照抄的规则声明）、**F-01**（死字段 + 误导注释）、**F-11**（删两个 NUL，清 `git status`）。
- **建议合并后紧接着做**：F-02（0 行档叠印，与本次自己的用例同域，最容易在下次改尺寸时爆）、F-03（把"四页签相等"的无条件注释补上条件，否则半年后没人知道它只在 ≥639px 客户区高时成立）。
- **本审计的边界**：未构建、未跑测试/probe、未启 app；"变异转红"与"回归面无碍"均为静态推演 + 实机产物（截图/失败清单）复核，非我复现的运行结果。
