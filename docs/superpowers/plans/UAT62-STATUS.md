# UAT6.2 唯一状态视图（2026-10-24）

> **唯一状态视图；台账仍是明细权威，二者冲突以本表更新日期较晚者为准并回写台账。**
> 台账明细：`plans/2026-10-21-UAT62-整改清单.md`；本表只做「同一件事三处状态不同步」的收口对齐，不替代台账的根因证据与执行协议。

- **更新时间**：2026-10-24
- **状态机**：⬜ 待开发 → 🔵 已开发·待核验 → ✅ 已核验·完成；⏸ = 暂停（等用户输入 或 已失效销账，不参与核验）
- **✅ 判定**：渲染/几何断言实跑过 / 用户真机目检确认 / 纯逻辑用例实跑过；纸面推导、代码审阅、「编译过了」一律不算。

| 条目ID | 来源 | 波次/批次 | 测试锚点 | 当前状态 | 剩余动作 | 证据 |
|---|---|---|---|---|---|---|
| H1–H14 | 台账·H表 | W3.5 修复轮 | 全量 1575/1531/35，非W4域零红 | 🔵 | 看图会话（切 bailian/qwen3.8-max）+ 用户真机目检转 ✅ | 台账 H1-H14 行；commit 清单见台账 W3.5 收口块 |
| H15 | 台账·H表 | W3.5 | 对比度门禁断言 | 🔵 | 用户真机目检（白卡半透 0.92） | `f352d024` 全站白卡半透 |
| H16 | 台账·H表 | W3.5 | todo 卡片三档色互异 + 底非 surface | 🔵 | 用户真机目检（优先级浅染 0.12） | `04d13e70` 浅染三档 88/88 绿 |
| H17 | 台账·H表 | W3.5 | `claude_switch_bottom_buttons_dual_tier` | 🔵 | 用户真机目检（最小窗底部按钮） | `07224abc` 双根因修复 |
| H18 | 台账·H表 | W3.5→换机续 | 逐页双档断言 | ⬜ | 按普查 md 拆 3 小批继续（残余写死 9 处三区块） | 普查 `b65a18fb` + `2026-10-23-UAT62-H18-弹性普查.md` |
| H18-批1 | 台账·H表 | W3.5 | home 双档具体值 + 无图表档 + 底部余量≤1 | 🔵 | test_ui_modern 图表柱高断言待更新（已派窄修） | `home_module.cpp:315/48/577` statH/chartH 公式 |
| H18-批2 | 台账·H表 | W3.5 | test_settings 双档 + test_github_sync_card 锚点 | 🔵 | 用户真机目检（640 档控件/进度次序重排） | `settings_ui_cards.cpp:524,550,567,589` |
| H18-批3 | 台账·H表 | W3.5 | test_agent_ui `agent_tools_height_dual_tier` | 🔵 | 编译实测确认（×0.3 校准） | `agent_module.cpp:203/1736/1932` |
| H18-MD | 台账·H表 | W3.5 | 双档断言侧栏宽 | 🔵 | 224 已落码+测试绿（接管会话发现，待用户追认；若拍 240 则回滚单点常量） | `markdown_module.cpp` kTreePanelWidth=224 + `md_module_sidebar224_split_dualres` 绿 |
| H18-Todo | 台账·H表 | W3.5 | — | ⏸ | 已转 H20 | 见 H20 行 |
| H19 | 台账·H表 | W3.5 | 双档断言（640 档=85% 基准、1000 档=用户档） | 🔵 | 用户真机目检（最小窗字号自动 85%） | `6f30fd5d` ≤660 窗高自动 85% |
| H20 | 台账·H表 | W3.5→换机续 | 双档宽度 + 动画中间帧 + 拖拽归档/恢复全链 | ⬜ | 3 条测试侧残留窄修（见下）+ 用户真机目检 | 原型 `docs/design/2026-10-23-todo-4col.html` SHA 938db369；构建 exit 0 |
| H20-数据 | 台账·H表 | W3.5 | 迁移幂等 + 任务守恒 + 启动即生效 | 🔵 | 迁移移至 DB 打开/schema apply 启动路径（数据批处理） | `home_service.cpp:174-175` kind 限定；`core/overdue.h` kWhereSql |
| H20-看板 | 台账·H表 | W3.5 | 双档宽度 + 动画中间帧 + 虚拟化 | 🔵 | 已编译+定向绿（2026-10-24 全子串复跑 Failed=0），待用户真机目检；旧测试引用列头菜单/✎ 由 D1 清理 | W=(contentW−3g)/4；archiveW=lerp(W×25%,W,archiveT) |
| H20-联动 | 台账·H表 | W3.5 | 交互全链 + 用户目检 | 🔵 | 已编译+定向绿（2026-10-24 全子串复跑 Failed=0），待用户真机目检；todo_dialogs.h NewColumn/Rename 死代码留收口简化审计 | 240/200ms ease-out；悬停 300ms 自动展开 |
| H21 | 台账·H表 | W3.5 | test_home 统计用例 + 编译实测 | 🔵 | 修复批 A kind-only 收紧已完成 | `home_service.cpp:174-175`；t5-t8、pendingTodos=4 |
| H22 | 台账·H表 | W3.5 | 文本探针：逐码点渲染核对 glyph index/advance | ⏸ | 暂缓（用户 2026-10-23 指示查不到可先忽略） | 字体 cmap 7 码点全在；疑似未注册加载 |
| H23 | 台账·H表 | 收口末 | diff 仅注释行 + 构建通过 + 抽查 10 文件 | ⏸ | 待最后环节（功能收敛后、出包前） | 注释治理：删历史叙述/批次号/hash/file:line |
| H24 | 台账·H表 | 全流程 | 逐任务净增行数 + 简化审计 + 构建/定向测试全绿 | ⏸ | 全流程执行（收口验收） | 最小代码量三禁；n 列推广/front 回落等清理项 |
| H25 | 台账·H表 | W3.5 | agent_ui/claude 定向测试全绿 | 🔵 | ccswitch 196/196 全绿收口，待用户目检 | agent_ui 71/72（1 平台 skip）、claude_ui 3/3、ccswitch 196/196 |
| H26 | 台账·H表 | W3.5 | 暗色下 Danger/语义色渲染断言 | ⏸ | 待用户拍板是否补映射（产品侧单开一批） | `componentTokensWeChat` 未映射 error/success/warning/info |
| A1 | 台账·A表 | W0 机制 | — | ⏸ | 已失效：S6.1 玻璃退役，drawGlassCardLayers 全仓零调用 | `todo_ui_board.cpp:460-489` 原绘制顺序 |
| A2 | 台账·A表 | W0 机制 | 标题首元素 Y 实测 + 用户目检 | 🔵 | 随 H8 待断言/目检（已由 H8 落地 kDialogInsetX=24） | EUI `dialog.h`；commit `64cf1ac`+`43f0d6d3` |
| A3 | 台账·A表 | W0 机制 | 用户目检（切页样板先行） | ⬜ | 定统一 header 规格：上边距砍/提示条可收起/状态入标题行/绿晕避头部 | 切换器页人工点评 6 组问题 |
| A4 | 台账·A表 | W0 机制 | 渲染断言 + 目检 | 🔵 | 待断言/目检（contentTop 76→58 已落码） | `todo_ui_dialogs.cpp:795` contentTop=58 |
| B1 | 台账·B表 | W3 白卡 | — | ⏸ | 已失效：随玻璃退役，看板卡由 H15/H16 接管 | `todo_ui_board.cpp:460+` |
| B2 | 台账·B表 | W3 白卡 | 渲染断言 + 目检 | 🔵 | 余项：未套 cardSurface 0.92 + Sm 阴影（H15 全站口径），待 H18-MD 回单后窄修 | md 6 处 `markdown_module.cpp:1241/1633/746/144/1133/1311` |
| B3 | 台账·B表 | W3 白卡 | 渲染断言 + 目检 | 🔵 | 余项同 B2（0.92/Sm） | `agent_module.cpp:1863` 工具卡 |
| B4 | 台账·B表 | W3 白卡 | 目检 | 🔵 | todo 列头钮随 H20 删除；cc/splash 已齐待目检 | `claude_ui_ccswitch.cpp:53`、`splash.cpp:222` |
| B5 | 台账·B表 | W3 白卡 | 渲染断言 + 目检 | 🔵 | 待断言/目检（三卡 0.92+Sm+border 已齐） | `settings_ui_cards.cpp:648/775`、`github_sync_card.cpp:150` |
| C1 | 台账·C表 | W4 弹窗 | 用户挑案 | 🔵 | 待用户目检确认后冻结为 W4 契约（竖向案补全稿已交付） | SHA `f7df48f1a24ebe63245d77d4b66b569b31b82be7130444f492d4807cf8a1a8fd` |
| C2 | 台账·C表 | W4 弹窗 | 渲染断言（全元素在框内）+ 用户目检 | ⬜ | 按选定 demo 逐像素对齐实现 | demo 即契约 |
| C3 | 台账·C表 | W4 弹窗 | 单测（纯逻辑）+ 详情弹窗渲染断言 | ⬜ | 字段合并落地：保 systemPrompt、五处清链 + 删两列 | `agent_module.cpp:2608/2758`、store、loop `:218/369`、详情 `:1470/1398`、`task_draft.cpp:213` |
| D1 | 台账·D表 | W3 md | 用户目检「看起来就是 demo」 | ⏸ | 卡点：docs/design 无 md A demo，需用户提供原稿或重出后再实施 | — |
| E1 | 台账·E表 | W3 切换器 | 用户目检 | ⏸ | 等用户线框（状态入标题行/提示条收起/操作分组/密度/APIKey 复制/tab 横滚/头部空白砍） | — |
| E2 | 台账·E表 | W3 全页 | 普查表 + 目检 | ⬜ | 其余 6 页 + 全弹窗 P1-P10 十模式普查 → 命中项套 A3 规格分批修 | 见 `2026-10-23-UAT62-E2-全页面弹窗普查.md` |
| F1 | 台账·F表 | W5 验证 | 测试执行本身 | ⬜ | 全弹窗「元素在框内」断言 + 步骤条门控单测 + 白卡清单固化为断言（防回潮） | — |
| F2 | 台账·F表 | W5 验证 | 实际跑过 | ✅ | 全量 1586 / Passed 1577 / Failed 0 / Skipped 9（2026-10-23 实跑）；9 条 skip 已由独立审计判定全为平台/工具链/环境条件 | 全量 ctest 实跑输出 |
| W3-P1 | 台账·W3前置 | W3 收口 | — | ⬜ | tests/ 95+ wechat.* 路径迁移（colors.wechat.* → colors.* 主槽位） | B 审计复查点名缺口 |
| W3-P2 | 台账·W3前置 | W3 收口 | — | ⬜ | 三源主题定义收敛（theme.cpp / component_theme.h / devdesk_theme.h 字面量并存 → shared constexpr 单源） | B 审计复查点名缺口 |
| W3-P3 | 台账·W3前置 | W3 收口 | — | ⬜ | DevDeskTheme 工厂消费对齐检查（所有消费方走统一工厂入口） | B 审计复查点名缺口 |
| W4-17条（总） | 用户 2026-10-23 拍板 | W4 | 见逐条行 | 🔵/⏸ | 17 = G1-G4（4 玻璃退役收尾）+ T1-T3（3 弹窗标题统一）+ C1-C10（10 代码注释治理），见下三行拆分；逐条状态以本表为准。**feat 现状**：G1-G3 零调用已提交、G4 🔵（`glass_card.h` 已删+防复活守卫，待合并复核）、T1-T3 🔵（待用户目检）；C1-C10 维持 ⏸ | 用户原话分拆 |
| W4-G1-G3 | 玻璃退役收尾 | W4 | — | 🔵 | feat 上 `drawGlassCardLayers` 全仓零调用（已由 S6.1 退役完成）；合并到 dev 时复核 dev 旧树残留自动消除 | grep `drawGlassCardLayers` feat=0 |
| W4-G4 | `glass_card.h` 归档/删除 | W4 | 构建 + smoke | 🔵 | 已提交待合并复核（不标 ✅：构建+smoke 已过但合并复核未做）：`glass_card.h` 已删 + 防玻璃复活守卫 | `a11cec47`；check-no-glass.sh 通过 |
| W4-T1-T3 | 弹窗标题统一（3 处） | W4 | 见 E2 普查 | 🔵 | 待用户目检：E2 #3/#4 全部证据点逐处达标（y 全 16、fontSize 全 20） | E2 普查表 |
| W4-C1-C10 | 代码注释治理（10 文件/注释块） | W4 | diff 仅注释行 + 构建 | ⏸ | 等标题批回单后串行（避免同文件多写者）：按 H23 盘点清单清理 10 个文件/注释块：保留契约/不变量/平台坑/why；删历史叙述/批次号/SHA/file:line/逐行复述/临时ID；普通注释块 ≤3 行 | H23 盘点 |
| W4-3处pin | 审计B·F8 | W4 | — | 🔵 | 随 T1-T3 目检：3 处已逐处裁决 24→16（agent_module.cpp），pin 注释清理 | `agent_module.cpp:2291/2954/3092` pin 已清，y 统一 16 |
| W4-在飞批收口（2026-10-24） | 本波收口 | W4 | 六子串定向 + probe | ✅ | fixer R1 七条修复 + 8 主题提交（9ce09044..a2e01da7）+ 复跑全绿（六子串 Failed=0 + probe 13/14；唯一红=agent_e2e DPAPI 换机环境项，留 W5 真凭据销案） | 六子串 ctest 实跑输出 |
| W3.5-A-L1 | 审计A | W3.5 | 探针视觉位移断言 | ⏸ | LOW 待后续 elapsed helper 一并处理：托盘隐藏期间相位继续走，恢复瞬间绿晕跳位 | `app_sidebar.cpp:188-193` fluidPhaseSeconds |
| W3.5-A-L3 | 审计A | W3.5 | — | ⏸ | LOW 同上：macOS 睡眠唤醒后 steady_clock 跳变（Windows 验证覆盖不到） | `app_sidebar.cpp:190-192` |
| W3.5-A-L4 | 审计A | W3.5 | — | ⏸ | LOW 增强项：detailW=56 硬编码，macOS/大字号档「查看详情」可能截/溢 | `github_sync_card.cpp:196` detailW=roundFs(56.0f) |
| W3.5-B-F11 | 审计B | W3.5 | 对比度门禁 | ⏸ | LOW 接受：OR 门使备选通道永不被强制（有描边即通过）；公式数学正确 | `test_github_sync_card.cpp` 对比度断言 |
| W3.5-B-F12 | 审计B | W3.5 | settings 双档 | ⏸ | LOW 增强项：H3 双档 +300 松界，建议精确断言增量 360 | `test_settings.cpp:779-812` settings_h3_elastic_heights |
| 架构债-elapsed | 台账·W3.5收口 | 跨波 | — | ⏸ | 统一 elapsed helper：fluid/splash 壁钟模式单点封装 + freeze gate + dt 钳 0.1s 防 macOS 睡眠跳变；待收口 | `app_sidebar.cpp` fluidPhaseSeconds；f06b687b 注释留痕 |
| 架构债-剪贴板 | 台账·W3.5收口 | 跨波 | — | ⏸ | EUI setClipboardText 返回 void 无法判失败（上游改进候选） | EUI-NEO `core/window/window_backend.h:26` |
| 架构债-DB两列 | 台账·换机交接 | W6 | — | ⏸ | DB 执行记忆两列删除（表结构只加列体系，删除有迁移风险，归 W6 出包前处理） | — |
| 测试残留-3条 | 台账·运行状态 | W3.5 收尾 | test_todo_ui / test_ui_modern / test_claude_ccswitch_ui | ✅ | 3 条残留断言全绿：test_todo_ui 65/65、test_ui_modern 17/17、test_claude_ccswitch_ui 196/196（2026-10-23 实跑） | 定向测试实跑输出 |
| 本波实测-F2 | 本波实测 | W5 | 全量 ctest | ✅ | 全量 1586 / Passed 1577 / Failed 0 / Skipped 9；9 条 skip 已由独立审计判定全部为平台/工具链/环境条件，无代码缺陷信号；逐条原文见 build/test-full-F2b.log 的 `[SKIP]` 行（日志有 18 行含解释性重复，Summary 计数为 Skipped: 9） | 2026-10-23 ctest 输出 + 独立审计报告 |
| 本波审计-内联 | 独立审计 | W5 | 5/5 CONFIRMED | ✅ | 内联修复独立审计对象 5 组、5/5 CONFIRMED：① menuAnchorX 逐列锚点公式；② test_ui_modern 锚点断言；③ test_agent_draft contextNote 移除（C3）；④ test_todo_tools 逾期收窄期望（H21）；⑤ F2 Summary + 9 skip | 审计报告（2026-10-23） |
| 本波审计-skip | 独立审计 | W5 | 9 skip 分类审计 | ✅ | 9 条 skip 经独立审计逐条判定：全属平台/工具链/环境条件，非产品代码缺陷 | 审计报告（2026-10-23） |

**⏸ 子类**：含两类——等用户输入（E1/D1/H22/H26 等）与已记录性遗留/审计 LOW 接受项（架构债、A-L1/L3/L4、B-F11/F12，不阻塞出包，留痕待后续 elapsed helper 收口或上游改进）。

**仍需人工补的缺口**：
1. **W4 17 条已定**——17 = G1-G4（4）+ T1-T3（3）+ C1-C10（10），由用户 2026-10-23 拍板；逐条状态见本表 W4-* 行。G1-G4 已收口（G4 🔵 待合并复核）、T1-T3 🔵 待用户目检；C1-C10 维持 ⏸。
2. **H1-H14 转✅**依赖看图会话（切 bailian/qwen3.8-max）+ 用户真机目检，glm 通道禁读图；待用户切模型。
3. **E1/D1** 阻塞于用户输入（切换器线框 / md A demo 原稿），未解除前 ⏸ 不推进。
