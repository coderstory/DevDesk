# UAT 第 3 轮（2026-10-20）问题清单与处置

用户在 d5df6b63 构建真机 UAT 的实时反馈 + 第二波代码审计结论。起点 dev=8dc6a8aa（Act 3005bed4 + 同步双列 42504080 已合入）。

## 用户反馈 12 项

| # | 反馈 | 根因/证据 | 处置 |
|---|------|----------|------|
| 1 | todo 米黄 → **更正：md 编辑器内很多米黄** | markdown_module.cpp:985/:1054 `t.colors.surfaceElevated`（编辑/预览面板底）+ :349/:421 componentTokens 暖橙页面主题（Act 只迁移了 popup 作用域） | C2 批 |
| 2 | 目录树折叠按钮/效果不行 | 拍板 3 项全中：按钮样式/位置 + 折叠后 16px 细边形态奇怪 + 缺过渡动画（:369/:582/:674） | C2 批 |
| 3 | 半透透明度太高 | round-2 登记过（content 0.60 太透）；Act M-3 只修了降级门控未调值 | C1 批调高 |
| 4 | 半透/磨砂能否覆盖标题栏 | Windows CAPTION_COLOR 无 alpha；拍板：**自绘标题栏**（D-R3-1） | C1 批 |
| 5 | 关于页卡片没放大 | about kCardW=580 太小；另需窄窗 clamp（min 窗宽降到 1000 时 content≈712） | B 批（在飞，已追加 clamp 要求） |
| 6 | 设置 switch 显示好大 | settings_ui_cards.cpp:326 传 80×32，规范 44×24（agent_module.cpp:2361 同款） | B 批（在飞） |
| 7 | 运行按钮底色灰色没改对 | claude_ui_detail.cpp:879-895 透明底文字钮，用户看到灰框（疑 components::button 默认 border，todo board:132 有显式 .border(0) 先例）→ 改 WeChat Primary 绿主钮 | B 批（在飞） |
| 8 | **彻底删除供应商余量功能** | 用户拍板整功能删除（覆盖"不涉及功能变更"约束——用户 own scope） | 普查代理在飞 → D 批 |
| 9 | 侧边栏顶部 DevDesk 文本删掉 | app_sidebar.cpp:161 brand 行；品牌名改由自绘标题栏承载 | C1 批 |
| 10 | 「当前系统不支持窗口半透」提示文本删掉 | settings_config.cpp:215/:227 + window_effect_hint_{win,macos}.cpp + settings_module.cpp:326 提示行 | C1 批（删提示行，降级行为保留） |
| 11 | 窗体最小尺寸还是太大 | 拍板 **1000×640**（D-R3-2，当前 1280×800） | C1 批 |
| 12 | todo 归档列卡片文本用黑色 | todo_ui_board.cpp:616-618 归档态 textTertiary（灰）→ textPrimary | C3 微批 |

## 拍板决策

- **D-R3-1 自绘标题栏**（用户 2026-10-20 选「自绘标题栏（推荐）」）：undecorated 窗口 + 自绘拖拽/双击最大化/最小化/最大化/关闭 + 边缘 resize；半透/磨砂（DWM backdrop）随之铺满全窗。EUI-NEO 能力缺失优先修上游（CLAUDE.md 规则 2）。
- **D-R3-2 最小窗体 1000×640**（用户选「1000×640（推荐）」）：各页 1000 宽适配由 C1 推算核验（about 卡由 B 批 clamp 兜底）。
- **D-R3-3 折叠三处全修**（用户多选全中）：样式/位置重做 + 去掉 16px 细边（完全收起+浮动展开钮）+ lerp 过渡动画。文件夹行 46px 命中区**不动**（用户未选；wave-2 L-2 已记录整行方案会吞 selectDir）。

## 第二波代码审计（6f6201d7..d5df6b63，只读）

判定 **sound：0 HIGH / 2 MED / 5 LOW**。
- **W2-M1**（必修→B 批）：settings_ui_cards.cpp:493-494 代理卡「应用」按钮 core 橘残留 → wechat 绿主钮。
- **W2-M2**（必修→B 批）：todo_ui_dialogs.cpp:711/:727 截止按钮 #07C160 落 selectedBg #E5E5E5 = 1.90:1（12px）→ 新增 WeChatColors::primaryText（≥4.5:1 深绿）。
- L-1 dark-WeChat 文字侧债归 D-04 工单范围（原例外只描述 BG 面）；L-2 round-2 文档 :51 偏离**已回写**（见本文上方编辑）；L-3 tagY/listY 无 pin（记录不改）；L-4 formRow 14px 字面量（记录）；L-5 package 注释口径收窄（记录）。
- 存量不修（记录）：todo board:383「+ 添加任务」绿字同款对比度问题（先于 D 批存在，W2 审计在案）。

## 批次与状态

| 批 | 内容 | 状态 |
|----|------|------|
| Act（wave-1 必修 M-1/2/3 + L-2~5） | 3005bed4 已合入 | ✅ |
| 同步卡双列 | 42504080 已合入 | ✅ |
| 余量删除普查 | 只读调研 | 在飞 |
| B 批快修（运行按钮/应用按钮/switch/about/截止对比度） | fix/uat3-quick | 在飞 |
| C1 窗口壳（自绘标题栏/半透 alpha/1000×640/提示删除/sidebar 文本/sync retry 反馈） | fix/uat3-chrome | 派发 |
| C2 md 页（编辑器 wechat 化/折叠三处） | fix/uat3-md | 派发 |
| C3 微批（归档文本黑） | todo_ui_board.cpp | 待槽位 |
| D 批（余量整功能删除） | 按普查清单 | 待普查 |
| 收口 | 全量 ctest → wave-3 审计（round-3 全部改动）→ 修复 → 安装包 → 交付报告 → 真机 UAT | 待 |
