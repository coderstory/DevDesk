# macOS 布局统一处理方案（落盘存档）

> **来源与状态**（落盘元信息，非方案正文）
>
> - **来源**：4 个**只读**审计分片 + 1 次综合裁决（workflow `wf_9c7929b8-b63`，2026-09-28）。分片范围：① 布局尺度链 `src/ui_kit` + `src/core`（theme / Router / window_layout / ui_geometry）+ `src/main.cpp` + EUI-NEO compose 尺寸折算；② `module_claude_proxy`；③ `module_home` + `module_settings` + `module_about`；④ `module_todo` + `module_markdown`。四片 findings 原始 JSON（19 条，其中 8 条标 `needs_product_decision=true`）另存同目录 `2026-09-28-macos-layout-audit-findings.json`，可按 `file:line` 检索。
> - **状态：尚未实施。** 审计全程只读——未改 `src/`、未跑构建、未跑测试、未跑 app；正文中的「本机实测」数字来自审计期另一次 `EUI_FX_DEBUG=1` 一次性诊断，**macOS 侧 `ctest` 基线未验证**（正文 §2 步骤 0「基线取证」要求先补）。§2 依赖顺序的 0~6 步**一步未动**。
> - **待拍板：7 项（P1~P7）**，全部列在正文 §3，**用户未决**——不得并入自动修，须等用户逐项批复后再立项。
> - **正文完整性**：以下正文（自 `# macOS 布局统一处理方案（待审批）` 起至文末）为综合产物**原文照录，一字未改**（未改写、未精简、未调整措辞）。仅文件层面在顶部加了本段元信息、在文末补了一个换行符。
> - **实施偏离记录（2026-09-29 补，组 B 落地后回填）**：§1 组 B 正文要求 P1 的 pane 内容框底取 `contentBottom = min(paneBottom, actY − 20)`。实现的落点是
>   **恒定框**：`contentBottom = actYFloor − 20 = bodyH − 105`（仍按 `min` 兜底到 `paneBottom`），
>   即「内容框」四个页签同一块固定区域、不随内容收缩 —— 因为只有框恒定，下部空白才会读成
>   「这块是内容区、现在是空的」，而不是一块随内容大小变化的、无法解释的空白（用户主诉）。
>   正文 §1 组 B 的其余条款（动作区锚容器底、判定用真实裁剪下沿）逐字实现，未偏离。

---

# macOS 布局统一处理方案（待审批）

## 0. 一句话根因判断

**不是"一个"根因，也不是四个模块各自的 bug。是两条主线 + 一个放大因素。**

- **主线 A（共享层，真正 macOS 独有）**：窗口尺寸的"单位语义"没有平台化 —— 同一个常量在不同入口走了不同换算。`windowSize` 走了 `windowScaleForPoint` 换算，`minWindowSize` 漏了这一步；非客户区外框用的是 Windows 实测值；Router 页面拿到的 `Screen::scale` 恒 1.0。后果：**macOS 上最小窗高 845 点 > 工作区 833 点，"满足最小尺寸"与"窗口完整可见"结构上不可兼得**（Windows 上同一数值被 DPI 缩成 676 逻辑，反而没这事）。
- **主线 B（DevDesk 绘制层，两平台同式，但被 macOS 放大）**：「容器高度由常量推导」与「内容几何按数据量推导」是两个互不知情的来源 —— 详情面板的动作区跟着内容底走（`actY = paneBottom + 20`），面板底却是恒定的 `bodyH`；共享配置片段高度无上界；弹窗调用方按未钳制的 `h` 排绝对坐标。叠加 `clamp(...,3,10)` / `max(1,...)` 这类兜底，把"高度预算不足"静默转成"少画几行不报错"。
- **放大因素**：这批常量标定于 Windows 期（09-22 ~ 09-28），之后结构又被改过一次（「最近调用」从面板底部固定预览块抽成第 4 页签，2175c24，只搬内容没搬锚点）+ **用户数据恰好最短**（extra_env 只有 1 个键、调用记录 0 行）→ 空白被放到最大。

**用户问"macOS 上为什么有这种问题"，诚实答案：不是 DPI 折算算错了。**这条工作假设已被实机数据证伪（`framebuffer 2940×1602 / contentScale 2.0 → 逻辑 1470×801`，macOS 逻辑高**等于**客户区点高，两平台都只除一次，无多除少除）。真正的原因按贡献排序：① 两个平台的逻辑空间不同源，macOS 更窄（1470 vs Windows 1536 逻辑宽，少 66）；② 最小窗下限在 macOS 是"点"，845 > 833 工作区；③ 常量标定于 Windows 期 + 09-28 那次结构调整；④ 你的数据恰好是最短的那一档。

**用户两个具体症状的定性（重要，别误修）**：
- **「env 只显示 1 条」= 数据本身就是 1 条**（DB 只读核对：`extra_env = {"CLAUDE_CODE_EFFORT_LEVEL":"max"}`，settings.json 的 11 个键全部正确分发到 base_url/api_key/4 个模型槽位/1 个 extra_env，没丢键）。高度算式要压到 1 行需 `bodyH<330`（≈客户区高 <490），本机最小窗内容高 845 都到不了。你看到的"不对"是**面板下部 60% 空白 + 四个页签的动作区位置互不一致（相差最大 171px）**——这是真 bug（见组 B）。
- **「最近调用显示不全」= 该页签 DB 0 行，本该是空态**；空白让它看起来"还有内容没显示"。但同时**确实有个真 bug**：该页签行数被硬上限锁死在 10 行（当前可容 14~21 行），且截断无任何提示（env 页签有"另有 N 条"）。

### 分片结论的矛盾点与裁决

| # | 矛盾 | 裁决 |
|---|---|---|
| 1 | 分片 2 曾猜"claude 的 env 行数退化更像模块内部 y0/预留常数不一致"，分片 1 说是"锚点漂移造成的空白" | **不矛盾，方向一致**：分片 2 猜对了"是模块内几何、不是屏幕尺寸"，分片 1 把点位钉死到 `paneBottom → actY`（拿到了算式与逐页签 y）。以分片 1 为准。 |
| 2 | `claude_module.cpp:86` 越界量：分片 1 算 **4px**（引导条实际高 18），分片 3 算 **16px**（按预留 30 算） | 取 **4px**：分片 1 引了源码行 `claude_ui_list.cpp:281 .size(contentW, 18.0f)`（实际渲染高），分片 3 用的是"预留值"（假设它等于实际）。**但修法两者完全相同**（收成共享常量 / 让 body 行 flexGrow），修完不需要区分。 |
| 3 | `main.cpp:424/:544` 是否要拍板：分片 1/2 标 Y，分片 4 标 N（给了"两全解"） | 拆开：**机械部分不拍板**（min 也走平台换算 + 与工作区取 min）；**要拍板的是"工作区小于设计下限时 UI 怎么办"**，见拍板项 P5。 |
| 4 | 本机客户区高：分片 2 推 805，分片 3/4 实测 801 | 取 **801**（`[layoutdiag] windowSize=(1470,801)` 实测），805 是 `833−28` 的推导。差 4 点，不影响任何阈值结论。 |
| 5 | Windows 侧逻辑客户区高：分片间给了 778 / 791 / 816 三个推导值 | **证据不足，不引用**。只保留定性结论：Windows 同一版式空白只差 15~45px，问题同样存在，只是 macOS 更明显。 |

---

## 1. 根因分组

### 组 A — 窗口尺寸语义未平台化（共享层，先决）

**涉及**：`/Users/coderstory/dev/gui/DevDesk/src/main.cpp:424-425`（常量）、`:438-462`（决策函数）、`:453-460`（日志）、`:544`（`.minWindowSize`）、`:550`（`.highDpi`）；`/Users/coderstory/dev/gui/DevDesk/src/core/window_layout.cpp:35`（`frameInsetsForScale`）；`/Users/coderstory/dev/gui/DevDesk/src/core/window_layout.h:4-11 / 25 / 34 / 41 / 70-88`（注释口径）；`/Users/coderstory/dev/gui/DevDesk/src/app_sidebar.cpp:273-276`（手搓 `Screen`，scale 恒 1.0）；`/Users/coderstory/dev/gui/DevDesk/CLAUDE.md:312`（把 Windows 口径写成跨平台事实）；`/Users/coderstory/dev/gui/EUI-NEO/core/app/glfw_app_main.cpp:82`（上游：`PointerEvent::scale` 从未赋值，恒 1.0）。

**症状**：macOS 最小窗收不进屏（底沿压进 Dock 带）；临界屏上启动被无谓最大化（外框多算 15 宽/~10 高）；日志把"点"标成"物理px"且打印"缩放 1.000000"，把排查者引向错误前提；文档派生错误假设。

**统一修法**：
1. 抽一个**纯函数** `core::window_layout::minWindowSizeForWorkArea(request, workArea, scale, frameInsets)`：逻辑最小尺寸 → 走与 `windowSize` **同一条** `windowScaleForPoint` 换算 → 再与 `workArea − frameInsets` 取 min。
2. `frameInsetsForScale` 的基准值改为**平台注入**（core 保持纯计算，按仓库既有"平台适配接口隔离"惯例拆 `_macos` / `_win`）：macOS 返回 `{0, 28}`（真机实测回填），Windows 保留 `{14.4, 37.6}`@96dpi 基线（`window_layout_frame_insets_baseline` 用例锁的就是它）。
3. `app_sidebar.cpp:273` compose 里补 `pageScreen.scale = screen.scale;`（同一帧框架已传入真值）。
4. 日志单位按平台分支打印（macOS 标 `pt`），补打 `contentScale`。
5. `CLAUDE.md:312` 补平台限定（Windows 段逐字保留 + 新增 macOS 段附实测三行）。
6. 上游顺手项（低优先）：`glfw_app_main.cpp` 的 `consumePointerEvents` 补 `event.scale = dpiScale`。**当前 DevDesk 无消费点（grep 零命中），零行为变化，只是堵住契约空洞。**

**为什么一次覆盖**：这六处是**同一个语义缺口的不同出口**——"点 / 逻辑px / 物理px"三套单位没有单一权威定义，每加一个入口就漏一次换算。定完后：最小窗两边都对（macOS 收得进屏、Windows 真正落实 845 的设计意图）；外框只在 Windows 生效；`Screen.scale` 今后被误用会立刻现形而不是"macOS 碰巧对"。

---

### 组 B — 「容器底」与「内容底」不同源（用户主诉所在）

**涉及**：
- `/Users/coderstory/dev/gui/DevDesk/src/module_claude_proxy/claude_ui_detail.cpp:447`（`actY = paneBottom + 20`）、`:668-680`（`drawDetailPanel` 收 pane 底沿）、`:438-441`（历史注释）
- `/Users/coderstory/dev/gui/DevDesk/src/module_claude_proxy/claude_module.cpp:86`（引导条占位预留 30，实际 chrome 34）
- `/Users/coderstory/dev/gui/DevDesk/src/module_claude_proxy/claude_ui_list.cpp:412-414`（`sharedH` 无上界 + `listH` 96px 硬保底）、`:494-502`（区块画在列表下方）
- `/Users/coderstory/dev/gui/DevDesk/src/module_settings/settings_ui_logic.cpp:29-34` + `settings_module.cpp:615 / 631-635`（判定阈值比真实裁剪边界松 24px；框架实测 `viewportHeight` 被 `(void)` 丢弃改用推导值）
- `/Users/coderstory/dev/gui/DevDesk/src/module_todo/todo_ui_dialogs.cpp:452-466` / `:648`（调用方用未钳制的 `h` 排绝对坐标，框架 `components::dialog` 才钳成 `min(h, screenH−48)`）

**统一修法（一条原则）**：**容器可用高只有一个来源，且由框架/共享函数给出；所有"贴底元素"（动作区、按钮行、分隔线）锚容器底，内容视口 = 容器底 − 已知 chrome；所有"向下展开/是否越界"的判定必须用真实裁剪下沿，不用推导值。**
- claude 详情：动作区改 `actY = max(paneBottom + 20, bodyH − 动作块高)`（四个页签的分隔线 y 恒定），并给 pane 一个**显式内容框**（底 = `min(paneBottom, actY − 20)`，画背景/分隔线给出可见边界），空态画明确文案。
- claude_module：引导条高度与占位间距收成同一常量；更彻底的做法是让 body 行 `flexGrow` 吃掉剩余高，`bodyH` 由布局给出、不再手减 chrome。
- claude_ui_list：共享配置区块设可见上界或钉面板底，列表视口吃剩余高；最低限度 `listH` 的保底不得把区块推出 `bodyH`（加 debug 断言 `blockTop + sharedH <= bodyH`）。
- settings：把框架实测 `viewportHeight` 透传进 `drawCardRegion`，裁剪下沿与判定阈值统一用 `cardsTopY + viewportHeight`（`kWindowBottomMargin=16` 不变），`screenH` 参数改名 `clipBottomY`。
- todo 弹窗：`h` 与内容坐标**同源**——先 `h = min(h, screen.height − 48)` 再排内容（改一处覆盖 13 个对话框站点）。

**为什么一次覆盖**：这五处的失败形态是同一个——"面板/容器底"和"内容底"是两个没人对齐的量，差值就以空白、重叠、越界、静默裁切出现。CLAUDE.md 里没有一条规则要求二者同源；本次把它写成规则并落到五处。

---

### 组 C — 预算兜底静默化（clamp / max 吞掉"高度不足"）

**涉及**：`claude_ui_detail.cpp:204-206`（`envMaxRows = max(1, ...)`）、`:332-334`（`rowsCap = clamp(..., 3, 10)`）；`/Users/coderstory/dev/gui/DevDesk/src/module_markdown/markdown_module.cpp:651 / 1056 / 1452 / 951`（四处 `max(0, ...)`）；`/Users/coderstory/dev/gui/DevDesk/src/module_todo/todo_ui_board.cpp:395`（唯一未钳 0、直接传负值给 scrollView）；`claude_ui_list.cpp:412`（96px 保底，已并入组 B 的修法）。

**统一修法**：
1. 去掉与内容无关的**硬上限**：`rowsCap` 的 10 改为"纯按可用高推"（或 `min(可用行数, 分页大小)`，那属于拍板项 P2）。
2. 去掉会画出越界的**硬下限**：不足 1 行只画空态、不强行画 3 行。
3. **任何截断必须有可见表达**：`recent.size() > shown` 时补一行与 env 页签同款"另有 N 条（点「查看全部」）"。
4. 本机不可达的 `max(0,...)`（markdown 四处）**不删算式**，改成"钳 0 + 记一条 WARN / 加 debug 断言"——它们的价值不是解决今天的症状，而是把"静默归零"变成"可见归零"。
5. `todo_ui_board.cpp:395` 补 `max(0, ...)`。

**为什么一次覆盖**：这是"错误不报错"的家族。真正的修复不是把某个数字调对，而是**让"预算不足"在 UI 上有表达**——否则任何平台、任何根因算小了，症状永远是"少几行，没人知道"。

---

### 组 D — 独立 UI 债务（与前序无依赖，可并行）

| 位置 | 问题 | 修法 |
|---|---|---|
| `markdown_module.cpp:1356` | 状态栏左串（`文件名 ✓ 已保存 HH:MM`）在 macOS 上被 `.clip()` 硬裁（无省略号），可用宽 187 vs Windows 220，早触发 ~3 个 CJK 字 | 改为**中段省略**（保留尾部"✓ 已保存 …"），用实测字宽或 InputModel 测量接口；顺手给 `statusLeftW` 补 `max(0, ·)`（当前窗口下限不可达，但 `w < 268` 时为负） |
| `markdown_module.cpp:1431` | 预览列自带 `surface→surfaceElevated` 垂直渐变底板（`md.preview.bg`），浅色主题下"双层卡"观感 | **拍板项 P6** |

---

## 2. 依赖顺序

| 序 | 步骤 | 为什么必须在这个位置 |
|---|---|---|
| **0** | **基线取证**（不写代码）：macOS 上跑 `ctest` 记录当前通过数；跑一次 `EUI_FX_DEBUG=1` 启动拿实测客户区高与 `req.min`；grep 测试目录里对 `actY/845/1122/96.0f/clipBottom` 的既有断言 | 本会话审计**没跑过构建**，"646 全绿"是 Windows 侧数字，macOS 基线未验证；且 801 目前是实测但状态未知 |
| **1** | **组 A**（含日志单位） | 它决定"最小窗长什么样"，组 B 的标定必须在**最大化 801 点**与**最小窗**两档都成立；同时后续所有验证都靠日志 |
| **2** | **组 B** | 用户主诉在这里；改完立刻能用日志断言四页签锚点一致 |
| **3** | **组 C** | 行数预算的**正确公式依赖组 B 给出的内容框底**（`actY − 20`）；先改行数就是白改 |
| **4** | **组 D 的自动部分**（状态栏省略号） | 无依赖，可与 2/3 并行 |
| **5** | 拍板项落地（P1~P7） | 等用户批 |
| **6** | 回归：macOS `ctest` + probe 新增 1262×801 档；如条件允许，Windows 侧跑一遍单测 | — |

组 A 内部顺序：先定 `minWindowSizeForWorkArea` 纯函数 + 外框注入（新函数，不动老路径）→ 再切 `main.cpp:544` → 再改日志/文档 → 最后 `Screen.scale`（零风险，可随时）。

---

## 3. 需你拍板的项（不混进自动修）

> 判据：`needs_product_decision=true` 的 8 条已去重合并成 7 项。**P3 不在原始标记里，是我从数据侧提出的一个疑点，建议一并拍板。**

**P1. 详情面板下部结构**（`claude_ui_detail.cpp:447`，对应你的主诉）
- 现状：动作按钮组/副提示/分隔线的 y 完全由数据量决定，四个页签分隔线分别在 392/420/186/188，面板底恒在 611 → 下部 60% 空白，且页签之间观感不一致。
- 可选：(a) 动作区**钉面板底**，内容区给显式边界（推荐）；(b) 只在空白区画一句空态说明；(c) 只补"另有 N 条"，结构不动。
- 取舍：(a) 四个页签位置恒定、空白变成"列表的合理尾部"，代价是改绘制 + 行数预算；(b) 改动最小但空白感仍在；(c) 治不了"位置不一致"。

**P2. 「最近调用」行数上限与截断提示**（`claude_ui_detail.cpp:333`）
- 现状：硬上限 10 行（当前可容 14~21 行），超了**无提示**；可用高不足时下限 3 行会压进动作区。
- 可选：(a) 纯按可用高（去掉 10）；(b) 保留 10 但补"另有 N 条（点「查看全部」）"；(c) 上限提到一个更大值。
- 取舍：(a) 一次显示最多、与 env 一致；(b) 更克制但你要多点一次。

**P3. 「环境变量」页签到底该显示什么**（数据侧疑点，非 finding 标记）
- 现状：该页签只列 `extra_env`（本例 1 个键）；你 settings.json 的 11 个 env 键里 10 个被分到"基础信息/模型映射"，所以从这一页看不出全貌，容易被读成"丢键/少显示"。**我没有核实你"应该更多"的预期从哪来**，所以列为待你确认。
- 可选：(a) 保持只列额外键，加一行说明；(b) 列出全部生效 env（含分发后的键，标注来源）；(c) 加一个"导入/分发报告"入口。
- 取舍：(a) 最小改动；(b) 信息最全但页面变长变杂。

**P4. 共享配置片段溢出**（`claude_ui_list.cpp:412`）
- 现状：≥6 个片段时「+ 新建片段」被底部「导入当前配置」压住**点不到**；≥7 个落到面板外；≥8 个被整页 clip **完全不可见**（且无提示）。
- 可选：(a) 区块封顶 N 张 + "另有 N 个"（与 env 同款）；(b) 区块钉面板底，内部自己滚；(c) 整个面板改滚动页。
- 取舍：(a) 最小；(b) 最稳但列表变短；(c) 一次解决所有面板内溢出，改动面最大。

**P5. 最小窗尺寸与"小屏兜底"**（`main.cpp:424-425 / :544`，同时涉及 `about_module.cpp:470`）
- 现状：`1330×845` 在 macOS 是**点**（> 本机工作区 833），在 Windows 是**物理px**（实际下限被缩到约 1064×676 逻辑，低于设计意图）。macOS 上"窗口满足最小尺寸"与"完整可见"不可兼得；about 页的"两卡并排"契约建立在这条下限上。
- **机械部分自动修**：min 走平台换算 + 与工作区取 min（注意：只做换算不做钳制，会把 macOS 的病复制到 Windows，因为 845 逻辑 = 1056 物理 > 1020 工作区）。
- 要你拍板的是"窗口可以小于 845 之后怎么办"：(a1) about/home 保持无滚动（小屏被切）；(a2) about + home 加**有界滚动**（推荐）；(a3) home 的 6 行固定块改成可压缩/换行。
- 附带子项（原 `about_module.cpp:470`，与 P5 同源）：footer 贴"画布底"还是"显示器可视底"？建议改可视底（用启动期实测工作区），代价是"贴底"的位置会随 Dock/缩放档变化；接受"只有一个底"则维持现状。

**P6. markdown 预览列渐变底板**（`markdown_module.cpp:1431`）
- 现状：`md.preview.bg` 自带渐变，浅色主题下"双层卡"，与你在 settings 侧反复要求删掉的 pattern 同族（那是最后一处）。
- 可选：(a) 删渐变（保留实色底或整块无底，交回主窗 clearColor）；(b) 维持。
- 取舍：(a) 与你此前诉求一致；注意 `md.tree` 有"bg 与 stack 尺寸精确重合"的既有耦合注释与用例锁，**只删渐变色值、不动 rect 尺寸**最稳，并补一条反断言。

**P7. `.highDpi(true)` 二选一**（`main.cpp:550`）
- 现状：GLFW 分支**不消费**这个开关，`GLFW_SCALE_TO_MONITOR` 全仓零设置；但 `DevDesk/CLAUDE.md` 声称"默认开启"。这个失实文档正是本次错误工作假设的源头之一。
- 可选：(a) 在 EUI-NEO GLFW 分支真正补上该 hint（会**改变 Windows 侧尺寸语义**，必须同步换算链，回归面大）；(b) 维持现状，删掉文档说法、把 `windowSize` 注释明确为"物理px（Windows）/ 点（macOS）"（**推荐，零风险**）。

---

## 4. 风险与不变量

**会不会动到 Windows 侧？**
- **组 A 会，且必须**：`minWindowSize` 走换算后，Windows 的最小窗会从"实际 676 逻辑"变回"设计意图 845 逻辑"，而 845 逻辑 = 1056 物理 > 本机 1020 工作区 → **若不同时做工作区钳制，Windows 会新出现"窗口收不进屏"**。这是本次最大的跨平台风险点，两条改动必须同一次提交。
- 组 A 的外框注入：Windows 分支返回现值 → 行为不变（`window_layout_frame_insets_baseline` 锁的 `{18,47}@{1.25}`、`{15,38}@{1.0}` 必须继续过）。
- 组 B/C/D：纯 DevDesk 绘制逻辑，两平台同式，修完两平台一起变好。**关键不变量：组 B 的"贴底"必须用锚定实现，不能写绝对 y** —— Windows 侧可用高与 macOS 不同（`bodyH` 差 ~15~45px），写死绝对值会在 Windows 上错位。
- 组 A 的 `Screen.scale` 补真值：今天**全仓无消费点**（grep 验证），零行为变化；修前再 grep 一次确认。
- 日志单位改动会改 `devdesk.log` 文本：需 grep 是否有测试/脚本依赖该行文案（目前未发现，`main.cpp:455` 是唯一打印点）。

**需要同步改的既有断言（已定位）**
| 断言位置 | 为什么要改 |
|---|---|
| `tests/test_modules/test_ui_modern.cpp:898-925` `uiModernMdPreviewGradient`（`:923` `DOCTEST_CHECK(bg->gradient.enabled)`） | **直接锁住了预览列渐变必须开启**。P6 拍板删渐变后必须反断言（写法照 `uiModernSettingsBodyGradientRemoved`，`test_ui_modern.cpp:862-886`） |
| `tests/test_modules/test_ui_modern.cpp:934 / 1006-1009`（`Screen{1122.0f, 845.0f}`） | 手写字面量镜像最小窗常量。P5 后改为"经同一函数推导"，否则会变成"测试和实现各写一份 845" |
| `tests/test_modules/test_ui_modern.cpp:1000-1003`（footer 底边 == `screen.height − spacingXXLarge`） | 若采纳 P5 子项（footer 改贴"可视底"），这条断言要改成传工作区 |
| `tests/test_core/test_window_layout.cpp:108`（决策不小于最小尺寸 1200×720）、`:135-146`（外框 125%/100% 基线） | 新增 `minWindowSizeForWorkArea` 后，老用例覆盖"请求 ≥ 最小值"路径，新用例覆盖"工作区 < 最小值"路径；外框用例补平台参数 |
| `tests/test_core/test_router.cpp:389-401`（`frame.scale == 1.0f` 注释"未填 → 1.0 退化值"） | **不需要改**（它测 `windowToCompose`，不经过 app_sidebar compose）；但那条注释描述的正是组 A 要消灭的"手搓 Screen"模式，注释建议同步更新 |
| 组 C 相关（行数上限/`listH` 保底 96/`clipBottom`） | 修前 grep `tests/` 里对 `10` / `96.0f` / `cardsH` 的引用；目前**未发现**直接断言这四个常量的用例，但不能排除间接断言（如"画了多少行"的计数） |

**代价与未测边界**：组 B 的 todo 弹窗改动面小但**本机不可达**（触发需客户区高 < 636/688，本机 801）；settings 的 24px 判定缺口本机也不触发。这两项修完**无法在本机现窗口态验证"修好了"**，只能验证"算式与真实裁剪底一致"（见下）。

---

## 5. verify 口径（不依赖截图）

**先说限制**：本机 `screencapture` 被 TCC「屏幕录制」挡死（多片已确认），**全程无像素级证据**；probe harness 把 `pointerScale=dpiScale=1.0` 写死（`tests/probes/probe_harness.h:96`），**Retina 路径零自动化覆盖**。好在本次所有缺陷都在**逻辑空间**（已证 DPI 折算正确），所以下面的判据都能成立。**【2026-09-28 晚更新】上面「被 TCC 挡死 → 无像素级证据」是登记时状态，现已不成立**：屏幕录制权限用户已授予，`screencapture -x` **实测可用**（2940×1912 PNG）；现存限制仅为「**对 OpenGL 窗口可能抓到导航前的旧帧**」（移动鼠标触发重绘后拍第二张、取后一张）。实施本方案时按 `CLAUDE.md`「功能开发生命周期」阶段 4 启动 app + 截图自验（独立 `DEVDESK_DATA_ROOT`）；probe harness 的 `pointerScale=dpiScale=1.0` 限制不变。

**每组的客观证据**

| 组 | 判据（可 grep / 可断言） |
|---|---|
| **A** | ① 新纯函数单测：macOS 参数档 `{workArea 1470×833 点, insets {0,28}}` → 断言 `minHeight ≤ 805`；Windows 档 `{1920×1020 物理, scale 1.25, insets {18,47}}` → 断言结果 == 设计下限或按工作区钳，且**与改动前的物理值对照不回归**。② 实机 `EUI_FX_DEBUG=1` 启动 → grep `[layoutdiag] createWindow ... req.min=(...)`，断言 min 高 ≤ 工作区高 − 标题栏。③ 把窗口拖到最小 → grep `[layoutdiag] frame windowSize=`，断言客户区高与工作区的关系成立（必要时新增一行"窗口是否越出工作区"诊断）。④ grep 日志：macOS 分支不再出现"物理px"标注。 |
| **B** | ① **单测（最强杠杆）**：`eui::Ui` 里按真实页面尺寸 `Screen{1262.0f, 801.0f}` 驱动 claude 详情，断言 `ui.find("claude.detail.actions")->frame.y` 在**四个页签下完全相等**，且 `actions.y + actions.height ≤ claude.detail.frame.y + frame.height`。现有 `tests/test_modules/test_claude_ui.cpp:206 claude_module_compose_with_data` 与 `:429`（已用 `claude.detail.tabs.hit.1`）就是现成骨架。② 新增一行诊断日志 `[layout] pane=<id> paneBottom=.. actY=.. contentBottom=.. rows=../..` → 实机 grep，四个页签 `actY` 必须相等（这正是用户"高度不对"的客观化）。③ debug 断言 `blockTop + sharedH <= bodyH`。 |
| **C** | ① 单测：造 30 条调用记录 + 固定 Screen → 断言"画出的行数 == floor(可用高/24)"（而不是 10），且存在"另有 N 条"文本 widget。② 造 `bodyH` 不足一行的极端 Screen → 断言画 0 行 + 空态文案，**不画越界行**。③ 本机不可达的 markdown 四处 `max(0,...)`：断言"归零时产生一条 WARN（或 debug 断言触发）"，证明不再静默。 |
| **D** | ① 单测：可用宽 187 + 超长中文文件名 → 断言结果串含省略号且尾部"✓ 已保存 HH:MM"完整（可与 `tests/probes/markdown_probes.cpp:368` 已登记的 P3 合并，直接把它从 TODO 变成断言）。② 渐变：反断言 `ui.find("md.preview.bg")->gradient.enabled == false`，同时 `md_tree_column_layout_semantics`（`tests/test_modules/test_markdown.cpp:1015`）必须继续过（证明 rect 尺寸未动）。 |
| **全局** | ① macOS 上 `ctest` 全绿（**第一步先记基线**；当前 macOS 基线未验证，不确定是否 646/646）。② `tests/probes/claude_probes.cpp:15` 的尺寸矩阵从 `{1200×720, 1200×1000}` 扩到含 **`1262×801`**（真实 macOS 页面尺寸）——现有 probe 完全测不到"真实窗口尺寸下的区域比例"，这是这类问题只能靠人眼发现的机制性原因。③ 若采纳 P5(a2)，补 about/home 在 `Screen{960×613}`（"更大文字"缩放档）下不溢出/可滚的断言。 |

**证据不足、需要补的东西（我不知道，得取）**
1. **最小窗在 macOS 拖动缩放时的实际钳制行为**：结论基于 GLFW cocoa 源码 + `845 > 833` 的数值关系，未实测。
2. **修改后 Windows 侧的实际观感**：本次无法在 Windows 机器上跑（除非你提供），只能用单测的双平台参数化用例兜。
3. **字体实宽**（PingFang vs 微软雅黑）：状态栏"多少字被省略"需实机确认，或直接改成按实测宽度算，绕开这个未知。
4. **用户预期来源**（P3）：你为什么会觉得 env 该有更多条 —— 是看到空白，还是知道自己有 11 个键？这决定 P3 选 (a) 还是 (b)。

---

**一句话给用户**：你看到的核心症状（面板下方高度不对）是**真的 bug**，但它不是"macOS 折算算错"，而是"动作区跟着数据量跑、面板底却固定"造成的 60% 空白（09-28 把"最近调用"改成第 4 页签时只搬了内容没搬锚点）；macOS 独有并真正会伤人的只有一条 —— **最小窗 845 点比工作区 833 点还高，窗口结构性收不进屏幕**。修的顺序是：先定共享层单位语义 → 再修面板锚点 → 再收拾会吞错误的 clamp → 最后 UI 债务。7 项拍板在上面第 3 节。
