# 组 A 独立审计（窗口尺寸语义平台化 + 侧边栏顶部留白）

- **审计对象**：分支 `feat/macos-layout-group-a`，唯一提交 `367c9b6`（base `2839c99`），worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-layoutA`
- **审计者**：独立只读子任务（非实现者、未参与本轮改动）
- **审计日期**：2026-09-29
- **纪律声明（哪些做过 / 哪些没做）**：
  - 做过：`git show 367c9b6` / `git diff 2839c99..367c9b6` 通读；源码与 EUI-NEO 上游（含 GLFW 3rd）只读阅读与 grep；测试文件逐条读断言；静态推演变异验证；`git merge-tree` 预演合并。
  - **没做**：没有构建、没有跑 `ctest`、没有启动 app、没有改实现代码（故"变异后应转红"为**静态推演**，非实跑；每条给出手算依据，见 §4）。
  - 唯一写动作 = 本文件。

---

## 1. 六维度结论（每条一句话）

| 维度 | 结论 |
|---|---|
| 1 正确性 | **对齐诉求**：macOS 最小窗 845 点 > 工作区 833 点的结构矛盾被消除（min 高 → 805 = 833 − 28 标题栏）；侧边栏顶部留白 24 → 40 落实"加一点上边距"。无阻塞缺陷，但"落实 Windows 845 设计下限"的表述与实测数不符（F-04）。 |
| 2 回归面 | **已核，全部同步**：`minWindowSizeForWorkArea` 唯一生产调用点 = `src/main.cpp:470`；`frameInsetsForScale` 生产调用点 = `src/main.cpp:460`（唯一）；三平台实现与 CMake 选编齐备；`pageScreen.scale`、`.minWindowSize` 替换、日志单位均已同步；无遗漏调用点。分支落后 master 8 提交（CLAUDE.md 区域 changed in both，但 0 冲突标记，可自动合并）。 |
| 3 平台对称性 | **Windows 未被改坏**：外框基准返回值与 base 常量逐字相同（14.4/37.6）；不变量用例 `{18,47}@{1.25}`、`{15,38}@{1.0}` 数值逐字保持（但改为显式传参 + 新增平台注入用例分别锁定，见 §4-1）；Windows 的实际变化只有两处预期内改动：最小窗 845 物理 → 973 物理、侧边栏观感 +16px（F-05）。 |
| 4 静默失败面 | **发现 1 条 Low（F-01）**：单轴返回 0 的"该方向不约束"在下游被放大为"两轴都不约束"（cocoa/win32 均为 all-or-nothing），且无任何日志。其余新增错误路径（工作区未知 / min=0 / 非法 scale）均为显式降级且测试覆盖。 |
| 5 测试有效性 | **非恒真，已静态复核两条变异**：留白 24 会红、钳制顺序反转会红（依据见 §4-9）；`test_ui_modern` 的 `Screen{1122,845}` → `{1330−208,805}` 是**收紧**（字面量变成被断言项），不是放松。发现 1 条测试与实现脱钩风险（F-06，未来常量变更），非当前假绿。 |
| 6 文档口径 | **发现 3 条 Low**：实现者自报 `about_module.cpp:362` 之外**漏报 TODO.md:58 / TODO.md:120**（F-02）；`app_sidebar.cpp:314` 的自证 grep 命令被本次提交自身作废（F-03）；`window_layout.h:19-20` 与提交信息对 Windows 下限的无条件表述（F-04）。`CLAUDE.md:312-313` 的 Windows 段逐字保留 + macOS 新段与实测一致，已核无矛盾。 |

**findings 总数 6 条（Medium 0 / Low 6 / High 0）+ 3 条信息项（I-01…I-03）**；**无阻塞合并项**（详见 §5）。

---

## 2. findings（每条 = file:line + 问题 + 影响 + 建议 + 判据 + 严重度）

### F-01 [Low] 单轴 0 在下游被放大成"两轴都不约束"，且不留日志

- **位置**：`src/core/window_layout.h:47`（"0 = 该方向不约束（透传给后端的 "no limit" 语义）"）、`src/core/window_layout.h:160-161`（规则 5）、`src/core/window_layout.cpp:44`（`available > 0 ? min(...) : 0`）、`tests/test_core/test_window_layout.cpp:266`（`DOCTEST_CHECK_EQ(tiny.height, 0)`）
- **问题**：返回值 0 在下游的语义不是"该方向不约束"，而是"两个方向一起退回无约束"：
  - `EUI-NEO/core/window/window_backend.cpp:866-867`：`minWidth > 0 ? minWidth : GLFW_DONT_CARE`（单轴映射本身正确）
  - `EUI-NEO/3rd/glfw/src/cocoa_window.m:1107-1110`：`if (minwidth == GLFW_DONT_CARE || minheight == GLFW_DONT_CARE) [setContentMinSize:NSMakeSize(0, 0)]` —— **或**关系，任一轴 0 → 两轴最小尺寸同时清零
  - `EUI-NEO/3rd/glfw/src/win32_window.c:1109-1113`：`if (minwidth != GLFW_DONT_CARE && minheight != GLFW_DONT_CARE)` —— **与**关系，任一轴 0 → `ptMinTrackSize` 整条不设
- **影响**：两条平台同形。触发条件 = 某轴 `工作区 − 外框 ≤ 0`（macOS 工作区高 ≤ 28 点 / Windows ≤ 47 物理 px）。现实显示器不可达，故定 Low；但一旦命中，最小尺寸保护**整条消失**（窗口可被拖到任意小），且日志只会打印 `最小窗 800x0`，读日志的人按 header 文档会误判为"高度不设限、宽度仍 800"。属"不崩也不报"。
- **建议（选一）**：(a) 把 `window_layout.h:47` / `:160-161` 改成"0 = 两轴一起退回后端无约束语义（GLFW_DONT_CARE 在 cocoa/win32 均按 all-or-nothing 处理）"；(b) 或 `clampMinToWorkArea` 在 `available <= 0` 时返回 1，保住另一轴约束；(c) 测试 `:266` 旁边补 `DOCTEST_CHECK_EQ(tiny.width, 800)`，把"另一轴仍为 800"钉住，避免后来者按"0 = 单轴不约束"误读。
- **判据**：`grep -n "GLFW_DONT_CARE" /Users/coderstory/dev/gui/EUI-NEO/core/window/window_backend.cpp`（866-867 行）；`sed -n '1107p' /Users/coderstory/dev/gui/EUI-NEO/3rd/glfw/src/cocoa_window.m`；`sed -n '1109,1114p' /Users/coderstory/dev/gui/EUI-NEO/3rd/glfw/src/win32_window.c`。
- **严重度**：Low（维护成本口径：文档语义与真实行为不符，后来者会据此做错判断）。

### F-02 [Low] 文档 stale 三处，实现者只报了 1 处（漏报 2 处）

- **位置**：`src/module_about/about_module.cpp:362`（实现者已报）、`TODO.md:58`、`TODO.md:120`（**漏报**）
- **问题**：三处仍写"最小窗 1330×845"。845 已失义：macOS 最小窗高 = 805（`main.cpp:470` → 833 − 28），Windows = 973 物理 = 778 逻辑。`TODO.md:58` 是"已完成"条目里对 about 页宽度契约的现状陈述，读起来是当前事实。
- **影响**：后续按 TODO 推断"最小窗内容高 845 可用"会与真实 805 差 40 点（设计 about/home 小屏兜底——即计划 P5 待拍板项——时正需要这个数）。
- **建议**：三处统一改写为"最小窗宽 1330；高受工作区钳制（macOS 805 / Windows 973 物理 = 778 逻辑），取值口径见 `core::window_layout::minWindowSizeForWorkArea`"，或直接引用函数名不再写数字。
- **判据**：`grep -rn "1330×845" src TODO.md docs`
- **严重度**：Low。

### F-03 [Low] 注释里的自证命令被本次提交自身作废

- **位置**：`src/app_sidebar.cpp:313-315`
- **问题**：注释写"今天全仓无消费点（grep `screen.scale` 零命中），零行为变化"。改动前该 grep 确实零命中（`git grep -n "\.scale" 2839c99 -- src` 输出为空），但同一提交在本文件新增 `:283`（读 `screen.scale`）与 `:316`（写 `pageScreen.scale`），现在同一条 grep 得 3 命中。读者复跑该命令会得到与注释相反的结论，进而怀疑整段"零行为变化"的判断。
- **影响**：证据链在提交那一刻起自相矛盾，降低后续审计对该注释的信任（本次已因此多花一轮 grep 交叉验证）。
- **建议**：改为"改动前 `git grep -n "\.scale" 2839c99 -- src` 零命中"。
- **判据**：`git grep -n "\.scale" 2839c99 -- src`（空）vs `grep -rn "screen\.scale" src`（3 命中：283/314/316）。
- **严重度**：Low。

### F-04 [Low] "Windows 落实 845 逻辑的设计下限"是无条件表述，与本机实测数矛盾

- **位置**：`src/core/window_layout.h:19-20`（"Windows 落实 845 逻辑的设计下限、macOS 保证最小窗仍收得进工作区"）；提交信息正文同句
- **问题**：同一句提交信息里既写"钳到 973 物理 = 778 逻辑"，又写"落实设计下限"，两者互斥。实测：`845 × 1.25 = 1056`，`1020 − 47 = 973`，钳后 `973 / 1.25 = 778.4` 逻辑 < 845。只有工作区足够的机器上才成立。
- **影响**：后续若按 header 断言"Windows 最小内容高 = 845 逻辑"（例如给 about/home 定小屏兜底阈值），会与真实 778 差 67 逻辑点。
- **建议**：改为"工作区足够时落实 845 逻辑下限；工作区不足时钳到 `工作区 − 外框`（本机 778 逻辑）"。
- **判据**：`tests/test_core/test_window_layout.cpp:236-237`（`min.height == 973`）+ 手算 `min(845×1.25, 1020−47)`。
- **严重度**：Low。

### F-05 [Low] 侧边栏留白同时改 Windows 观感（唯一"Windows 可见变化"）

- **位置**：`src/app_sidebar.cpp:122`（`topPad = t.sizes.spacingXXLarge`，共享算式、无平台分支）、`:152/:162/:171`（品牌 y）、`:181`（导航起点 `topPad + 32 + 28 = 100`）
- **问题**：用户诉求的原始场景是 macOS 真机反馈，但实现无平台分支 → Windows 上品牌 y 24 → 40、导航起点 84 → 100。项目口径"Windows 分支行为不变"在此是"行为不变、观感变化"。
- **影响**：无功能/几何破损（页面区几何、侧边栏容器、分隔线均未动，`sidebar_top_padding_stays_inside_sidebar` 已锁）；风险仅在用户知情度——若用户期望 Windows 观感逐字不变，则本条不满足。
- **建议**：判定**不修**（跨平台一致符合用户"共享算式"的一贯取舍；引入平台分支会把平台判定带进 UI 层，违反仓库「平台适配代码必须接口隔离、业务代码零平台宏」）。但**合并说明里必须写清"Windows 侧边栏也下移了 16px"**，让用户在阶段 6 知情验收。
- **判据**：`grep -n "spacingXXLarge" src/app_sidebar.cpp`（122 行，无 `#if`）；`tests/test_modules/test_app_sidebar.cpp:178,182`（本平台期望 40 / 100，两平台同值）。
- **严重度**：Low（观感类、已声明）。

### F-06 [Low] 测试把"平台档输入"再写一份 → 未来常量变更时与实现静默脱钩

- **位置**：`tests/test_modules/test_ui_modern.cpp:941-947`（`minWindowForMacWorkArea()` 内写死 1600/1000/1330/845、1470×833、`FrameInsets{0,28}`）、`:1024-1026`
- **问题**：计划 §4 原文要求"P5 后改为经同一函数推导，否则会变成'测试和实现各写一份 845'"。实现已改为经同一函数推导（合格），但**把函数的平台档输入在测试里又写了一份**——`main.cpp:428-431` 的 `kInitialWindowLogical*` / `kMinWindowLogical*` 在匿名 namespace，测试拿不到，只能重抄。
- **影响**：不是当前假绿（函数逻辑回退仍会被 `:1025-1026` 的 `==1330 / ==805` 抓住）；但 `main.cpp` 常量一旦改动（如 1330 → 1200），本测试仍算出 805 并**继续绿**，about 页"最小窗不溢出"的 UI 契约断言与实现静默脱钩。
- **建议**：把 4 个常量提到可共享的头（`core/window_layout.h` 或 app 层公共头），`main.cpp` 与测试同源消费；退一步至少在 `minWindowForMacWorkArea()` 注释里写明"与 `main.cpp:428-431` 手工同步"。
- **判据**：`grep -n "1330\|845" tests/test_modules/test_ui_modern.cpp`（字面量 3 处）与 `main.cpp:428-431` 之间无任何引用关系（`grep -rn "kMinWindowLogical" src tests` 只命中 main.cpp）。
- **严重度**：Low（按"多难维护"口径可视为中低边界，本次判 Low）。

### 信息项

- **I-01**：计划 §5 组 A verify 口径 ③（"把窗口拖到最小 → grep `[layoutdiag] frame windowSize=` → 断言客户区高与工作区关系"）**未做**，以 AX `setSize({1000,400})` 钳到 1330×805 替代。替代证据的强度评估见 §4-8。
- **I-02**：`window_layout_posix.cpp` 返回 `{0,28}` / `"pt"` 属**无实测的保守估计**（文件内已自述）。已核与框架一致：`EUI-NEO/core/window/window_backend.cpp:71-77` 非 Windows 分支 `windowScaleForPoint` 亦恒返 1.0。仓库无 Linux CI/构建，属"已声明的未测边界"，非缺陷。
- **I-03**：实现者申报的 3 条既有失败（`test_theme_hierarchy` md 面板底 1 条、`test_settings` 端口草稿 2 条，对应用例 `settings_port_esc_discards_draft` 等）**本次未独立复跑**（只读纪律不构建不跑测）。可复现命令：`cd <worktree>/build && ./tests/devdesk_tests <子串>` 与 `git stash`/`git checkout 2839c99` 对照。另注：`CLAUDE.md`「视觉规范」段"最小 1200 × 720"与 `spec 2026-09-20-devdesk-design.md:647/:1664`（"最小窗口 1200 × 720"）与代码早已不符（1330×845），**先于组 A**，不属本次范围。

---

## 3. 逐维度"已核"说明（为什么判定无问题）

1. **正确性**：诉求二条逐条核对——① "macOS 最小窗 845 点 > 工作区 833 点 → 结构性收不进屏"：`main.cpp:470` 取 `minWindowSizeForWorkArea` → 换算（scale=1.0 恒等）+ 与 `工作区 − 外框` 取 min → 805；窗口外框高 = 805 + 28 = 833 = 工作区高，**"存在一个完整可见的落位"这一结构性条件成立**（钳制只约束 size、不约束 position，用户仍可把窗拖出屏，这是 macOS 常态、非本缺陷）。② "侧边栏顶部要有呼吸感"：40 = 页面主区 padding token（`theme.cpp:40/93` `spacingXXLarge = 40.0f`），与 about 页 `topPad = spacingXXLarge + spacingXLarge` 同族。
2. **回归面**：`grep -rn "minWindowSizeForWorkArea\|frameInsetsForScale\|windowUnitName" src tests` 逐个确认调用点（生产 2 处、测试 2 文件）；`src/core/CMakeLists.txt:16-37` 三平台选编互斥且 `window_layout.cpp` 在公共源；三平台实现文件均存在且只 include 自己的头（零 EUI-NEO 依赖，符合 core 分层）；`pageScreen.scale` 全仓唯一手搓 `Screen` 点已补（`app_sidebar.cpp:307-316`）。
3. **平台对称性**：见 §4-1/2/3/6；`window_layout_win.cpp:16` 返回值 `{14.4f, 37.6f}` 与 base 的 `kFrameBaseWidthAt96Dpi/kFrameBaseHeightAt96Dpi` 逐字相同；测试用 `#if` 在函数体内（避开 `-Wembedded-directive`），且仓库测试已有同型先例（`tests/test_platform/test_terminal_launch.cpp:327/367`）。
4. **静默失败面**：除 F-01 外，新增路径均为显式降级且被 `window_layout_min_window_below_work_area_paths` 的 6 个分支覆盖（钳制 / 工作区未知 / 工作区小于外框 / min=0 / 非法 scale / 顺序）。新增日志（`main.cpp:473-481`）补打最小窗与单位名，把原先不可见的量变成可 grep 的：`最小窗 1330x805`。
5. **测试有效性**：`tests/test_app_sidebar.cpp:178/182/235/237` 与 `tests/test_core/test_window_layout.cpp:216-291` 的期望值都直接来自被测代码路径（`Element::y` = `.position` 入参，见 `EUI-NEO/core/dsl.h:112-113`，非布局结果），硬等值、非容差漂移。`test_ui_modern` 的改动是**收紧**（新增 `==1330` / `==805` 两条字面量断言）。无恒真断言。
6. **文档口径**：`CLAUDE.md:312`（Windows 段）逐字保留、`:313`（macOS 新段）三条实测与代码/日志一致（`windowScaleForPoint` 恒 1.0 → `window_backend.cpp:71-77`；`framebuffer 2940×1602 / 2.0 → 1470×801`；工作区 833 − 28 = 805）。矛盾项见 F-02/F-03/F-04。

---

## 4. 实现者主张独立复核（核了哪几条 / 是否一致）

| # | 实现者主张 | 独立复核方式与结果 | 一致？ |
|---|---|---|---|
| 1 | 不变量用例逐字保持（`{18,47}@{1.25}`、`{15,38}@{1.0}`） | 读 `tests/test_core/test_window_layout.cpp:143-155`：断言数值逐字未变；**但基准改为显式传参**（不再走单参入口），平台绑定另由 `:189 window_layout_frame_insets_platform` 用 `expectedPlatformBaseline()`（`_WIN32` → `14.4/37.6`）锁定。两条合计 ≡ 原不变量，无弱化 | 是（附一条口径说明） |
| 2 | `window_layout_win.cpp` 返回值就是原常量、无语义漂移 | `git show 2839c99:src/core/window_layout.cpp` 原为 `14.4f / 37.6f`；新文件 `:16` 同为 `{14.4f, 37.6f}`；`frameInsetsForScale` 的 ceil 逻辑未动（`:57-60`） | 是 |
| 3 | "Windows 分支行为不变" | 外框：不变。最小窗：1330×845（物理）→ **1663×973（物理）**（`tests:236-237`，即计划 §4 声明的预期变化）；侧边栏观感 +16px（F-05）。结论：**无回归，但非"逐字不变"** | 部分（实现者已自行声明，仅措辞需注意） |
| 4 | `EUI-NEO/core/runtime/runtime_lifecycle.h:105-107` 已填 `event.scale = dpiScale` → 组 A 第 6 条无需 patch | 读该文件：`:106 event.scale = dpiScale;`、`:107 event.space = PointerSpace::Physical`；`:115` 传参即 `effectiveScale = dpiScale × uiScale`（`core/app/dsl_window_runtime.h:75`）。且该行由 `d281378` 引入，`git merge-base --is-ancestor d281378 7164d55` = **YES**（文档写的最低依赖 rev 也已含它）。原计划指向的 `glfw_app_main.cpp:82` 已不存在该消费函数（`grep consumePointerEvents` 零命中） | 是（比主张更强：最低 rev 也满足） |
| 5 | `pageScreen.scale` 全仓无消费点、零行为变化 | `git grep -n "\.scale" 2839c99 -- src` 输出为空（改动前连 `.scale` 都没有）；改动后 `src` 内 `.scale` 仅 `app_sidebar.cpp` 3 处（283 读 / 316 写 / 314 注释） | 是（"今天"措辞见 F-03） |
| 6 | 组 A 提交不含 EUI-NEO 任何改动 | `git show --name-only --format="" 367c9b6` = 12 文件，全在 DevDesk 仓库内；`grep -i "eui\|\.\./"` 零命中 | 是 |
| 7 | `test_ui_modern` 的 `{1122,845}` → `{1330,805}` 是换推导、非放松 | 读 `:941-947 / :1024-1027`：`minScreen = {minWin.width − 208, minWin.height}`，并新增 `DOCTEST_CHECK_EQ(minWin.width,1330)` / `(minWin.height,805)`——字面量从"隐含输入"变成"被断言项" | 是（实际是收紧） |
| 8 | AX 真机：设 `{1000,400}` → 系统钳到 `1330×805` | **强度评估**：强证据，链条完整——AX setSize → AppKit → `setContentMinSize(NSSize(1330,805))`（`cocoa_window.m:1107-1110`，由 `window_backend.cpp:866-877` 下发，值来自 `minWindowSizeForWorkArea`）。它证明：① 新值确实到达后端；② 系统在该窗口上强制该下限。**不足以**证明：① 手动拖动边角路径——但两条路径共享同一 `contentMinSize` 约束源，故为高置信推断而非实测；② 窗口**位置**必定留在工作区内（钳制只约束 size）。实现者自列"未验证"属实，属诚实申报 | 大体一致（边界已列清） |
| 9 | 变异验证：留白改回 24 → sidebar 用例 3 条转红；钳制顺序反转 → 红 | **静态推演**（未实跑，见纪律声明）：① `app_sidebar.cpp:152/162/171` 的 `y` 由 `.position(0/padX, topPad)` 直给，`Element::y` 即该入参（`core/dsl.h:112-113`），故 `tests:178/235/237` 在 24 档下必红；② `tests:236` 期望 973 = `min(845×1.25=1056, 1020−47=973)`，若先钳后乘得 `min(845,973)×1.25 = 1056 ≠ 973` → 必红。测试非恒真 | 是 |
| 10 | 报告中的用例数（window_layout 15/15、sidebar 8/8） | 文件内 `REGISTER_TEST` 计数：`test_window_layout.cpp` = 15、`test_app_sidebar.cpp` = 7；以子串 filter 计（`test_main` 按名字子串过滤）：`sidebar` → 8（含 `tests/test_font_em_layout.cpp:104 "font-em: sidebar labels fit item height/width"`） | 是（数字自洽） |
| 11 | 既有失败 3 条非本次引入 | **未独立复核**（只读纪律、不跑测试）；已定位对应用例存在（`test_settings.cpp:1546` 端口草稿 Esc 丢弃等），可复现命令见 I-03 | 未核（申报合理，待阶段 4 复核） |
| 12 | `EUI-NEO` 侧 32 点透明带（`ead954c`）与组 A 无关 | `git show --name-only 367c9b6` 零 EUI-NEO 路径 + 组 A 未改任何窗口效果/compose 透明相关代码（仅 `app_sidebar.cpp` 侧边栏子树内部 y 值）→ 与组 A 无因果关系 | 是 |

---

## 5. 必须修完才能合并的项

**无阻塞项（0 条 High / 0 条 Medium）。** 理由：Windows 分支无功能回归；macOS 目标缺陷已由实机数值（min=1330×805）证实闭环；测试非恒真且已锁定新值。

**建议同分支顺手修（各 ≤ 5 行，可与阶段 3 一起做）**：

1. **F-04**（`window_layout.h:19-20` + 提交信息口径）——最便宜，且是唯一会让后来者用错数字做设计决策的一条。
2. **F-02**（`TODO.md:58` / `TODO.md:120` / `about_module.cpp:362` 三处 845 → 805 口径）——同一类 stale 一次性清干净，避免下次又被当成新 finding。
3. **F-03**（`app_sidebar.cpp:314` 注释改成"改动前 git grep 零命中"）。
4. **F-01**（口径 a：文档补一句"0 在两平台都是 all-or-nothing"；可选 (c) 补 `tiny.width == 800` 断言）——触发条件在现实显示器不可达，可降级到 backlog。

**合并前的机械动作（非 finding）**：分支落后 master 8 提交（`git log --oneline 2839c99..master | wc -l` = 8）；`git merge-tree 2839c99 367c9b6 master` 对 `CLAUDE.md` 报 `changed in both`，但**0 个冲突标记**（可自动合并）。阶段 5 合并前先同步 master；注意本分支的 `CLAUDE.md` 尚不含 master 新落的「功能开发生命周期」六阶段一节，故本审计文件按派单给定名 `2026-09-29-audit-group-a.md` 落盘（非模板的 `2026-09-28-<主题>-audit.md`）。

---

## 6. 未验证 / 边界（审计者自述）

1. **没跑过任何测试**：全部测试有效性结论为静态推演 + 代码因果链，非实跑红/绿。
2. **Windows 侧无编译/运行证据**：`window_layout_win.cpp` 的正确性只到"逐字等于原常量 + 测试用 `_WIN32` 分支给期望值"这一层；Windows 上的实际编译与用例通过与否未验证（无 Windows 机器）。
3. **手动拖动边角路径未实测**（实现者已列，本次未补测）。
4. **最小窗在 macOS 的可用性**：805 高下 about/home 页是否有内容被切，属计划 P5 待用户拍板项（组 A 只把"可缩到 805"变为事实）；本次未做 805 档的观感判定，仅确认 `uiAboutLayout` 的几何断言在 805 档仍成立（测试内已改为此档）。
5. `docs/` 内对 845 的其他历史表述（plan 2026-09-28 原文、spec `:647/:1664` 的 1200×720）属历史/先存偏差，未纳入本次 findings 范围（仅 I-03 记名）。
