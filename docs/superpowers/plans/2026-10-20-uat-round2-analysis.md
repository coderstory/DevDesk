# DevDesk UAT Round-2 分析文档

- 日期：2026-10-20
- dev：9d9e7c22（含 a3c53a34 sidebar 重叠+主题色选择器删除修复）
- 范围：真机 UAT 第二轮 16 组问题 + 废物代码清理 + 资产清理，全部已调研（R1-R7 子代理 + 主会话定位），待独立审计后统一处理。

## 两大跨切根因

1. **popup 容器未迁 wechat 令牌**（R4 统领）：所有 popup（contextMenu/dialog/dropdown/tooltip/styledMenu/toast）传 `.theme()` 的是 `componentTokens(t)`（旧暖橙），无 `componentTokensWeChat(t)`。W5-W10 只改了 popup 叶子元素，容器自身（底色/边框/hover/选中/文字/圆角）全留旧。修：各模块 `themeTokens/componentTheme/compTheme` 变量定义改 `componentTokensWeChat(t)`，~8 定义点一次切齐。
2. **dialog 按钮未迁 wechat**（R1/R2）：`ui_kit/dialog_form.cpp:75-126 buildDialogButton` 硬编码 core 令牌（Claude 橘），无 wechat 变体。修：加 `PrimaryWechat`/`SecondaryWechat` 枚举值 + 各弹窗 confirm/cancel 切换。

（两根因叠加 = 全弹窗/popup 令牌化，覆盖：右击菜单、新建笔记/片段/profile/编辑/导入弹窗、agent 弹窗、各下拉、tooltip、toast）

## 16 组问题（per-issue：根因 file:line + 修法 + 范围 + 冲突）

### 首页（home_module.cpp，W9）
1. **三按钮等宽**：L752-774（W9 1.5:1:1）→ 删 1.5 分支全 1.0。纯UI。冲突 §2/D-12「不等宽」（UAT 覆盖）。
2. **卡片字大**：L105/111/129（12/18/10px）→ 14/22/12。纯UI。冲突 §2。
3. **刷新按钮怪**：L365-379（textTertiary 透明底浮空，比次按钮浅一档）→ 对齐次按钮 textPrimary+secondaryButtonBg。纯UI。
4. **新建笔记弹窗旧**：markdown_module.cpp:2416-2481 + dialog_form.cpp:85-122 共享层 → 根治（根因2）。纯UI。blast radius 14 文件。

### todo（todo_ui_board.cpp + todo_ui_dialogs.cpp，W6）
5. **归档灰看不清**：board:485(0.7)+:614(textTertiary#999)+:573(chip) → 去 0.7、标题色 textSecondary。纯UI。冲突 §4「归档灰化」（UAT 覆盖）。
6. **todo 米黄底**：board:478-480/624/686（surface）+ dialogs 8 处 surface/surfaceElevated + 2 橘 → 批量 surface→wechat.cardBg/dividerSecondary/selectedBg/secondaryButtonBg（~13 处）。纯UI。
7. **优先级配色难看/没绿/灰蒙蒙**：todo_ui_logic.h:281-291（priorityColor 映射 textTertiary/info/warning/error，无绿）+ :299 tint {0.18,0.30,0.38,0.42} 太低 → WeChat 板：None 中性/Low=wechat.primary 绿 #07C160/Medium=wechat.linkBlue #576B95/High=wechat.warningRed #FA5151 + tint 提到 {0.20,0.40,0.50,0.55}。**哪个优先级=绿待用户拍板**（建议 Low=绿）。纯UI。

### 切换器（claude_ui_list.cpp + claude_ui_detail.cpp，W7）
8. **运行按钮怪**：detail:879-895（Text 级透明底，比刷新键还弱）→ 升 Secondary 级或 outlined + fa-play icon。纯UI。冲突 W7 §5「Text 级」（UAT 覆盖）。
9. **4 弹窗未改**（新建片段/profile/编辑/导入）：claude_ui_form.cpp + claude_ui_ccswitch.cpp → 根治（根因2）。纯UI。
10. **「已停止」间距大+应右对齐**：list:286-338（status 在 title row 左侧，text 盒 200px 太宽）→ status 点+文字+help 挪出 title row、右对齐到 header 右缘、text 盒缩到内容宽。纯UI。

### 智能体（agent_module.cpp + agent_detail_dialog.cpp，W5）
11. **弹窗未重新设计**：detail_dialog 全文零 wechat + 按钮走 drawDialogButtons（core 橘）→ 根治（根因2）+ 详情摘要卡底/停止 chip/展开收起切 wechat 令牌。纯UI。

### 设置（settings_module.cpp + settings_ui_cards.cpp + github_sync_card.cpp，W4）
12. **tab 布局乱**：W4 只迁 appearance 卡到 FormRow，其余 7 卡用 W4 前布局（3 label 宽 110/84/64/104 + 2 标题位 + 裸定位）→ 统一 formRow(110)+标题 16/22+desc y=40/h=16+内容起 y=68。纯UI。**W4 未完成迁移（回归）**。
13. **同步失败弹窗按钮没反应**：git_error_dialog.cpp（retry:248/copy:276/close:295）。接线正确。根因：① retry 静默失败（startGithubSyncAction:735 joinable 静默 return）= **功能 bug**；② close 也"没反应"→ 疑 hit-test（dialog scale/translate transform 矩阵）。修：retry 静默前加 toast 反馈 + hit-test 写 probe 诊断。**功能 bug + 待 probe**。非 W4 回归。

### 关于（about_module.cpp，W10）
14. **卡片大**：L125-145（kCardW 520 等）→ 520→580/padX 24→32/padTop/Bottom 32→40/icon 48→56/rowH 36→40。纯UI。

### 跨切
15. **splash 底色没绿**：splash.cpp:72（wc.pageBg #F7F7F7 灰）→ 换 wc.sidebarBgTinted #E8F0EC 微绿。纯UI。1 行。
16. **右击菜单未改** → 根因 1（popup 容器 .theme(componentTokens)→componentTokensWeChat）。纯UI。归属 W6/W8/W7/main。

### 窗口效果（settings_config.cpp + app_sidebar.cpp + about + markdown 子面板）
17. **半透透明度太高**：settings_config:180/187（content 0.60/标题栏 0.90）→ 截图确认"太透 vs 太实"或折中 content 0.80/标题栏 0.85。纯UI。
18. **磨砂不生效 sidebar/titlebar/about + 生效范围乱**：sidebar(app_sidebar:118-120 #E8F0EC opaque)+titlebar(settings_config:240 customColor, CAPTION_COLOR 无 alpha)+about(about_module:118-121 pageBg opaque)+markdown 子面板+各白卡都 opaque 盖 backdrop。规律：只 clearColor-transparent 的 content 透（home/markdown主/todo/claude/settings 五页）。修：sidebar/about/markdown 子面板/卡片带 alpha（纯UI）；titlebar frosted 档不设 customColor（**需 Win11 真机验证 caption 贯通，不通则走 EUI 上游 DwmExtendFrameIntoClientArea = 超 UI 范围**）。

### md 目录树（markdown_module.cpp，W8）
19. **大气+放大+可折叠**：中栏 220→240/行高 32→36/字 13→14（专用常量不改 fontSizeSmall token）/图标 16-20→18-22/搜索框 36→40/gap 0→4/padding 12→16。文件夹折叠**已实现+持久化**但命中区只 26px（"不好用"根因）→ 扩大整行；面板折叠**未实现**→ 新增 treePanelCollapsed_ state+按钮（纯 UI state）。纯UI。

### 窗体（main.cpp）
20. **最小宽高太大**：kMinWindowLogical 1330×845 → 1080p@125% ≈ 1663×1056 几乎全屏 → 调小（如 1200×720 逻辑）+ 复验各页小窗不溢出。纯UI。

## 废物代码清理（R7）

- **真死可清（4）**：`about_anim.h` 空壳文件（删文件）、`markdown_module.cpp:194` kDefaultEditorFontSize float 副本（删 2 行）、`home_module.cpp:39-44` statNumberFont（删 6 行）、`home_module.cpp:46-52` digitCount（删 7 行）。grep 证零消费。
- **存疑待拍板（3）**：accentTheme config 链（a3c53a34 已删选择器→整链成废，可清）、segvHandler（建议留零成本诊断）、演变叙事注释 5 处（CLAUDE.md 纪律要删）。
- **不死归 UAT 迁移（2）**：旧暖橙令牌（15+ 消费）、componentTokens()（20+ 消费）随 popup 根治迁移。

## 资产清理（打包）

- **DevDesk 只用 2 字体**：JingNanJunJunTi（UI 文本，EUI text.cpp:37 默认）+ Font Awesome（图标，:38）。
- **未用资产（打包 Exclude）**：music/ 1.5M（零音频代码）、shaders/shadertoy/ 225K（W2a 删 splash 后废弃）、YouSheBiaoTiHei.ttf（EUI clock demo 用、DevDesk 不用）、NotoSansSC-VF.ttf（CLAUDE.md 说排除但仍塞）、mona-loading-default.gif/Hard drive-rafiki.svg（疑 demo）。
- 修：`installer/windows/devdesk.iss` / `scripts/package-windows.sh` 加 Excludes 排除未用资产，瘦身安装包 ~2M+。

## 范围分类

- **纯 UI（多数）**：1-12, 14-16, 19, 20 + splash + 右击菜单 + 窗口 alpha/sidebar/about/markdown 带 alpha
- **功能 bug（1）**：13 retry 静默失败（加反馈）
- **待 probe（1）**：13 hit-test 诊断
- **超 UI 范围需平台/EUI（1）**：18 titlebar caption 磨砂贯通（Win11 不自动→EUI 上游 DwmExtendFrameIntoClientArea）
- **待用户拍板**：7 优先级哪个=绿、13 retry 反馈文案、17 半透"太透 vs 太实"截图、废物 3 存疑

## 统一处理顺序建议

1. **popup/弹窗根治**（根因 1+2）：dialog_form 加 wechat 变体 + 各模块 themeTokens→componentTokensWeChat。一次覆盖全弹窗/popup。blast radius 最大但根因最集中。
2. **todo 米黄+优先级配色**（6,7）：批量 surface→wechat + priorityColor 重做。
3. **各页 per-issue**（1-5, 8-12, 14-16, 19, 20）。
4. **窗口效果**（17,18）：alpha 调值 + sidebar/about/markdown 带 alpha + titlebar frosted 不设 customColor（+真机验证）。
5. **废物代码清理**（R7 4 真死 + 拍板 3 存疑）。
6. **资产清理**（打包 Exclude 未用）。
7. **同步失败功能 bug**（13）：retry 静默加反馈 + hit-test probe。
