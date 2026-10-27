# 独立审计：上线模块地基（T01，commit 6f3997d1）

> **审计对象**：`D:/wt-rel-core` 分支 `wt-release-core`，commit `6f3997d1`
> **对照**：票据 `.scratch/release-module/issues/01-secondary-sidebar-module-skeleton.md`（主工作树 `D:/project/gui/DevDesk`）、`docs/superpowers/specs/2026-10-27-release-module-design.md`
> **审计方式**：只读静态审计——逐文件读 diff，与父提交 `6f3997d1^` 逐行对比渲染公式，全仓 grep 消费方。**未编译、未运行测试**；凡"测试会红"的结论均由常量/断言直接比对得出（非实测）。
> **严重度口径**：按「多难维护 / 多难演进」定，不按会不会崩。`blocker` = 阻断合并/验收。

## 结论摘要

- **blocker × 2，major × 4，minor × 7**。
- 侧边栏二级菜单主链路已接通；既有 8 个模块的平铺几何逐行等价（indent=0），顺序不变。
- 两个硬问题：**存量配置升级后「上线」默认不启用**（B1）；**三处既有测试断言未随本次常量更新，提交后套件必红**（B2）。
- 地基数据模型与 spec 冲突（M1），空骨架文件大面积无调用方（M2），折叠交互有可达状态击穿验收判据（M3），核心能力零测试覆盖（M4）。

---

## 一、正确性（逐条对照票据验收判据）

| # | 判据 | 结论 |
|---|------|------|
| 1 | 主侧边栏出现「上线」父项，带展开/收起箭头 | **已实现**。`src/app_sidebar.cpp:446-497` 的分组头渲染：展开态画 `chevron-up`（0xF077）/收起态 `chevron-down`（0xF078），码点在 EUI-NEO 内置 FA7 Free-Solid 字体中已被 `components/dropdown.h` 同码点使用，可用。折叠侧栏（72px）下父项只画居中图标、无箭头，属可接受的降级。 |
| 2 | 展开后出现「任务」「基础配置」两个缩进子项，点击可进入对应页面 | **已实现**。`src/app_registry.h:55-56` 两行 `parent=ReleaseGroup`；`src/app_sidebar.cpp:612` 以 `kChildIndent=16` 渲染；点击走 `renderNavItem` 原 `onClick → navigateTo`（`src/app_sidebar.cpp:519`）。两页为空态骨架，可渲染。 |
| 3 | 收起后子项隐藏，父项仍在 | **基本实现但有击穿点**：`g_collapsedGroups`（`src/app_sidebar.cpp:56,180-188`）收起时 `continue` 跳过子项，父项照常绘制；但当前页属于该分组时本帧**强制展开**（`src/app_sidebar.cpp:602-607`），见 M3。 |
| 4 | 既有 8 个模块侧边栏渲染与改动前完全一致（无箭头/无缩进/顺序与间距不变） | **已核，成立**。父提交的 `renderNavItem` 与现版在 `indent=0` 时逐项等价：hit `position(padX,y)/size(width,y)`、selectionBar、icon `padX+kItemInset`、label `padX+kItemInset+kIconW`、`y += itemH+gap` 全部不变（`src/app_sidebar.cpp:503-565`）；无 parent 行统一走 `renderNavItem(m, 0.0f)`（:616）。新行追加在登记表尾（`src/app_registry.h:54-56`），Router 按 PageId 序注册，既有 8 项序与 y 值不变。 |
| 5 | 设置页出现「上线」开关卡，默认开启；关闭后父项与子项一起消失 | **代码链路成立，但存量安装不满足「默认开启」**。默认清单含 release（`src/module_settings/settings_config.cpp:57-63`），模块卡第 5 格（`settings_ui_cards.cpp:417-419`），关闭时 `dispatchModuleToggle` 逐行注销两个子页（`src/app_assembly.cpp:255-298`），侧边栏父项由子行驱动、随之消失（`src/app_sidebar.cpp:596-616`）。**存量配置升级路径见 B1**；此条无自动化覆盖见 M4。 |

**结论**：票据功能在「全新配置」路径下成立；判据 3 与判据 5 各有一个可达/现实缺口（M3、B1）。

## 二、回归面

- **`app_sidebar.cpp`**：无 parent 模块的几何/选中态/图标位置与改动前逐行等价（见上表 #4）；新增的 `registryEntry` 线性查表与 `drawnGroups` 集合不影响既有渲染。已核。
- **`app_assembly.cpp`**：单页门控键行为不变（命中首行即唯一行）；多页键改为逐行拨，是新增路径（无既有行为回归），但日志语义有瑕疵（m3）。已核。
- **`main.cpp`**：三个静态实例 + `moduleForPage` 三分支；`ReleaseGroup` 工厂恒 nullptr，`registerOne` 跳过空实例（`src/app_assembly.cpp:64-70`），不改变既有 8 页注册。已核。
- **`app_registry.h`**：既有 8 行字段零改动，新增可选尾字段 `parent` 默认 `nullopt`（:41），聚合初始化兼容。已核。
- **settings 四处**：默认清单/合法 ID/开关卡/卡高均同步；但卡高常量 198→254 牵动既有测试未同步（B2），且常量与开关数手工耦合（m4）。
- **两个被改 probe**：`uat62_page_shots_probe` 仅补 3 个 slug、`wallpaper_probe` 仅补 3 个 switch case——均为**补表行，未放宽任何断言**；`uat62` 的 null 工厂行按"平台能力不支持"跳过（该路径对 ReleaseGroup 的措辞失真，见 m2）。
- **两个被改既有测试**：`test_app_assembly` 全开 8→11、表形 11、getters 对分组行新增"工厂必须为 nullptr"断言（是收紧）；`test_module_gating` 仅 5→6 计数更新。**未放宽断言**。
- **但另有两处既有测试未适配**：`test_settings.cpp` 与 `test_wallpaper_module.cpp` 仍钉旧常量（B2）。

## 三、平台对称性

- **已核**：新增代码零 Windows/macOS 专属调用、零 `#ifdef` 泄漏；`module_release/*` 纯 `std` + `core` + `eui` DSL；`app_sidebar.cpp` 新增部分只用 `std::set`。原有 Windows 可见态查询块（`src/app_sidebar.cpp:37-47`）未被触碰。
- 箭头码点 0xF077/0xF078 与 EUI-NEO 自身 dropdown 同源（同一内置字体），两平台无差异。
- 测试红点（B2）为常量断言，与平台无关，两平台 CI 同样红。

## 四、静默失败面

- `release_store.cpp` 各方法显式返回 `ErrorCode::NotImplemented`——**不是**静默空值，已核合格。
- **但**：`release_reminder.h:10-11` 的契约写"幂等，重复调用不重复提醒"，实现只存 `lastTickUnix_` 后什么也不做（`release_reminder.cpp:6`），且全仓无调用方；后续接提醒时若信任该契约，会得到"不崩也不报"的空引擎。
- `release_list_ui/detail_ui/config_ui/dialogs` 五个函数为空实现且**从未被任何代码调用**；`release_module.cpp` 自己另画一套空态（M2）。
- 分组元数据有三条静默路径：`parent` 指向不存在的 PageId 时子项静默平铺为顶级项（`app_sidebar.cpp:599-616`，无日志）；`isGroupPage` 完全由子行反推（`app_registry.h:61-66`）；分组工厂 nullptr 与"平台不支持"同义（m2）。
- 已知键但实例全空时会连打两条日志、第二条内容错误（m3）。

## 五、测试有效性

- `test_release_basic` 能红的范围：分组工厂 nullptr、两页 pageId/displayName、空态文案、三行门控键、`isGroupPage`。这些若回退会红——**有效**。
- **不能红的核心**：侧边栏父项/箭头/缩进/收起/强制展开、关闭开关后父项+子项消失、设置卡第 5 格里 release 开关，**全部无断言**；`test_release_basic.cpp:56` 注释称"两个子页 parent 指向分组父项"，但循环体（:57-66）只断言 `gatingKey`，**未断言 parent 字段**（若把子项 parent 改错，用例仍绿）。
- 既有 `test_app_sidebar.cpp` 用桩模块（Home/Markdown）走的是无 parent 平铺路径，覆盖不到分组树。
- 结论：**票据核心 UI 能力目前没有能变红的测试**（M4）。

## 六、文档口径

- spec §10（`2026-10-27-release-module-design.md:236-247`）仍写"改 8 处""侧边栏自动接管，无需改动"——与实现不符：本次实际改了 `app_sidebar.cpp/h`、`app_assembly.cpp`、`settings_ui_logic.h`、两个 probe，且 `create_release_module()` 拆成三个工厂。后续按 §10 施工会漏掉分组机制（m5）。
- spec §9 数据表 / §2 领域模型 / §7 状态机 与 `release_types.h` 定义**结构性冲突**（M1）。
- probe/测试注释仍写"8 行 / 8 个真模块"（`uat62_page_shots_probe.cpp:10,28,131`；`test_app_assembly.cpp:528` 用例描述）（m5）。
- spec §11 的「折叠组件缺失」仍属实；侧边栏自绘折叠已落地但未回写 spec（并入 m5）。

---

## Findings

### B1（blocker）存量配置升级后「上线」不启用，验收判据 5 在真实安装上不成立

- **位置**：`src/module_settings/settings_config.cpp:57-63`（默认清单）、`:460-476`（enabledModules 存储分支）、`:512-513`（save 全量回写该数组）；`src/main.cpp:611-624`（退出时 load-modify-save）
- **问题**：`release` 只加进了 `defaultEnabledModules()`；一旦配置里已有 `enabledModules` 行（任何旧版本用户：设置改动或正常退出写窗体记忆都会走 `save()` 全量回写），`loadChecked()` 用存储数组整体替换默认值，`sanitized()` 只强制补 home/settings，**不会补 release**。升级后旧配置得到 5 项清单 → `moduleEnabled(cfg,"release")==false` → 两个子页不注册 → 侧边栏没有父项。
- **影响**：所有存量安装（含本机）升级后看不到「上线」，与票据「默认开启」相反；这正是项目交付标准里的「存量数据升级路径」缺口。后续每加一个默认开启模块都会复发。
- **建议**：加一次性迁移标记（如 `moduleDefaultsVersion`），在 load 时把新默认 ID 合并进存储清单并回写；或改为存"禁用清单"语义（新模块默认开）。二选一都需补一条"旧配置 5 项 → 升级后 release 开"的用例。

### B2（blocker）三处既有测试断言未随本次常量更新，套件必红

- **位置**：`tests/test_modules/test_settings.cpp:2282`（`modules 卡高 at(168,198,257)`）、`:3606`（`kModulesCardHeight == 198.0f`）；`tests/test_modules/test_wallpaper_module.cpp:153`（默认清单 `enabledModules.size()==5`）
- **问题**：源码已改为 254（三档 roundFs = 216/254/330）、默认清单 6 项，三处断言仍是旧值。`test_settings` 的卡高断言与 `test_wallpaper_module` 的默认项数断言必然失败。
- **影响**：提交后 CI/门禁直接红；也说明本次提交前未跑过受影响用例。`test_settings.cpp:2588-2606` 的"卡高包住内容"用例取的是同一新常量，能过，进一步说明只是旧钉值漏改。
- **建议**：把三处期望更新为 254 / 216 / 330（或改用 `roundFs(kModulesCardHeight)` 派生），默认项数改 6 并显式断言含 `kModuleIdRelease`；跑 `test_settings` + `test_wallpaper_module` 定向用例收口。

### M1（major）地基数据模型与 spec 结构性冲突，后续任务要推倒重建

- **位置**：`src/module_release/release_types.h:11-69`、`src/module_release/release_logic.cpp:8-39`；对照 spec `2026-10-27-release-module-design.md:28-42,90-98,189-197,214-230`
- **问题**：
  - `ReleaseTask` 只有单个 `serviceId`/`environmentId`（:63-64），spec 是一个任务含多环境、每环境多服务、且任务引用时做快照；
  - `TaskStatus` 为 `Pending/Running/Done/Failed/Cancelled`（:51-57），spec 是「配置中/待执行/执行中/已完成/已作废」，无 Failed/Cancelled；
  - `Service`/`Environment`/`ConfigItem` 缺 spec 的 `kind/default_branch/sort`、`start_minute/end_minute/env_service` 与 5 类配置项、`skipped/conf_order`；
  - `validateTask()` 校验标题/服务/环境（:8-22），spec §4.3 校验的是分支、配置内容、跳过标记；
  - `progressPercent()` 把全部 Step 计入（:32-39），spec §4.3/5.4 明确跳过项不计进度；
  - `daysUntilExpiry()`（:24-30）凭空引入 spec 不存在的"到期天数"，且"无记录"与"已过期"都返回 0，两态不可区分。
  - 文件头注释（:1-2）声称这是"跨文件契约、不得私自定义同义结构"——等于把错模型锁定为契约。
- **影响**：T02（服务目录）/T03（环境）/T04（任务快照）/T08（执行清单）几乎每个 ticket 都要先改这层再写业务，返工量大；若后续实现者照着现有结构长，会与 spec 和用户口径整体错位。
- **建议**：本票只保留 spec 能直接支撑的类型（或直接删除 types/logic 骨架），按 §9 表结构重建 `Service(kind/default_branch/sort)`、`Environment(start/end/服务集)`、任务快照三表模型与五态状态机；未对齐前不要实现"像真的一样"的校验/进度函数（避免被误用），并且这些纯函数要么补单测、要么不落实现。

### M2（major）空骨架文件大面积无调用方，后续必然被遗忘或重复实现

- **位置**：`release_list_ui.cpp:6-9`、`release_detail_ui.cpp:6-11`、`release_config_ui.cpp:6-9`、`release_dialogs.cpp:6-8`、`release_reminder.cpp:6`、`release_store.cpp:15-51`、`release_view_state.cpp:7-24`；`release_module.cpp:17-41`（自绘空态）
- **问题**：`composeReleaseListPage/composeReleaseDetailPage/composeReleaseConfigPage/三个 draw*Dialog` 全为空函数且全仓无调用方；`ReleaseStore`/`ReleaseReminder`/`ReleaseViewState` 三个类无任何实例化；两个页面由 `release_module.cpp` 内部的 `composeEmptyPage` 直接绘制，完全绕过这些"页面入口"。这些文件被编译进静态库，但没有链接/调用关系。
- **影响**：后续实现者（AI）最可能的行为是"在 release_list_ui.cpp 里把页面画出来"，然后发现模块 compose 根本没调用它；或者另起一套写入 `release_module.cpp`，两份实现并存。骨架的目的是把落点固化下来，现在落点与实际调用链是断开的。
- **建议**：现在就打通一次——`ReleaseTasksModule::compose` 调 `composeReleaseListPage`、`ReleaseConfigModule::compose` 调 `composeReleaseConfigPage`、详情页入口留空态但不建从未被调用的文件；`ReleaseStore/Reminder/ViewState` 要么在此票实例化并给最小可判定行为（store 断言 db 非空并返回 NotImplemented），要么移到后续 ticket 再加。至少加一条单测/编译期引用保证"文件被链接"。

### M3（major）当前页属于分组时强制展开，收起操作在可达状态下无效

- **位置**：`src/app_sidebar.cpp:602-607`（强制 `expanded=true`）、`:184-188`（`toggleSidebarGroup` 无条件翻转）、`:456`（父项点击）
- **问题**：用户停在「任务」或「基础配置」页时点父项，`g_collapsedGroups` 实际被写入"收起"，但本帧渲染仍强制展开、箭头仍显示向上；再次点击才翻回去。离开该分组后（如点首页）分组突然收起。
- **影响**：票据判据 3「收起后子项隐藏」在该状态不成立；用户感知为"箭头点不动"，状态与画面不一致，也制造了"折叠态只在下次导航突然生效"的怪行为。后续实现者读注释会以为这是有意设计而不敢改。
- **建议**：二选一并写清规则——(a) 当前页在组内时，点击父项先 `navigateTo(Home)` 再收起（或直接拒绝并 toast"先切走"）；(b) 去掉强制展开，允许隐藏当前页但保持选中语义。无论哪种，`toggleSidebarGroup` 与渲染必须对同一状态判定。

### M4（major）票据核心能力无自动化覆盖；新测试的注释断言比实际断言多

- **位置**：`tests/test_modules/test_release_basic.cpp:56-66`；`tests/test_modules/test_settings.cpp:1705-1745`；全仓无侧边栏分组用例/探针
- **问题**：(1) 没有任何用例/探针断言分组父项、chevron、子项缩进、收起后子项消失/父项保留、强制展开、开关关闭后父项+子项一起消失；(2) `test_release_basic.cpp:56` 注释说"两个子页 parent 指向分组父项"，循环体只断言 `gatingKey`，**parent 字段没断言**；(3) 设置卡用例只查到 `sw.3`，release 的 `sw.4` 存在性与默认勾选未覆盖。
- **影响**：侧边栏重写、`parent` 列改成别的组织方式、`dispatchModuleToggle` 回退成"首行即返回"，测试都保持绿色——正是"功能回退时不变红"的假绿形态。`test_app_sidebar` 的桩 Router 只走平铺路径，帮不上忙。
- **建议**：补一组无头用例（用 `ScopedAppRouter` + 桩模块覆盖 ReleaseTasks/ReleaseConfig）：断言 `app.sidebar.item.ReleaseGroup.hit/.chevron` 存在、子项 label 的 x 比顶级项大 16、收起后子项 hit 消失而父项 hit 仍在、当前页在组内时强展行为按 M3 结论锁定；补 `settings.card.modules.sw.4` 默认 checked；补"release 关闭 → 父项与两个子项均不在 DOM"的装配级断言。再加一张展开/收起态 probe 截图。

### m1（minor）折叠侧栏下缩进仍作用于命中区与选中条，注释与代码矛盾

- **位置**：`src/app_sidebar.cpp:501-502`（注释"仅展开态生效"）vs `:513-514`、`:527-528`（无条件加 indent）vs `:536`（折叠态图标居中）
- **问题**：72px 折叠态下，子项的 hit/selectionBar 仍右移 16px 并收窄 16px，而图标按 `(kWidth-kIconW)/2` 居中；图标左侧 10px 落在命中区外，选中底色比图标窄且右偏。
- **影响**：折叠侧栏是用户可长期使用的模式；该状态视觉/点击不一致，且注释会误导后续维护者以为已按态区分。改动面小但属于"注释说的不是代码做的"。
- **建议**：`indent` 只在 `!collapsed` 时参与 hit/sel 计算（折叠态一律 0），或把命中区改为覆盖整行、视觉条另算。

### m2（minor）分组身份完全隐式 + nullptr 双语义，元数据无校验

- **位置**：`src/app_registry.h:41,61-66`；`src/module_release/release_module.cpp:71-72`；`src/app_assembly.cpp:262-264`；`tests/probes/uat62_page_shots_probe.cpp:183-188`
- **问题**：`isGroupPage()` 靠扫描"是否有子行 parent 指向它"反推分组身份；父行 `factory` 返回 nullptr 这一信号同时表示"不可导航分组"和"平台不支持"（壁纸）。三条后果：子行被移除/改 parent 后父项静默不再是分组；`parent` 指向不存在的 PageId 时子项静默平铺为顶级项（`app_sidebar.cpp:599-616` 无日志）；`uat62_page_shots_probe` 把跳过的 `release_group` 打印成"平台能力不支持（工厂返回 nullptr）"，误导排障。
- **影响**：分组能力要扩展到第二个模块时，每个消费方都得记得这套隐式约定；错误元数据不会报错只会"看起来不对"。
- **建议**：在 `ModuleEntry` 加显式 `kind`（Top / GroupHeader / Child）或在 `isGroupPage` 处加静态校验（编译期/单测断言 parent 必须指向表中存在的分组行、分组行 factory 必须为 nullptr、分组必须至少一个子行）；probe 跳过文案区分"分组父项"与"平台不支持"。

### m3（minor）已知门控键但实例全空时，日志给出"未知或恒开模块"

- **位置**：`src/app_assembly.cpp:265-269,296-297`
- **问题**：`matched` 只在 `mod != nullptr` 时置位。壁纸在非支持平台、或未来 release 子页工厂临时返回 nullptr 时，循环先打"无实例，跳过即时生效"，再打"未知或恒开模块 xxx，忽略"——第二条是错误诊断。
- **影响**：排障时会被引向"键名写错"，而真实原因是实例缺失；多页键让这条路径从"单行 return"变成"可叠加两条日志"，更容易出现。
- **建议**：`matched` 改为"键命中即置位"，无实例时只保留一条准确日志（区分"键未知"与"键已知但无实例"）。

### m4（minor）模块卡高度是手工常量，与开关数量耦合

- **位置**：`src/module_settings/settings_ui_logic.h:103`（198→254）；`settings_ui_cards.cpp:418,455`（行数由 `kToggleCount` 派生，但卡高不是）
- **问题**：行数已按 `kToggleCount/2` 派生，卡高却手写 254；每加一个开关都要在另一文件改常量，并同步三处测试钉值——本次 B2 就是这样产生的。
- **影响**：第 7 个模块开关会在无告警的情况下溢出卡底（内容画到卡外），只能靠肉眼/后续测试发现；改常量的连锁面跨 3 个文件。
- **建议**：提供单一派生函数（如 `modulesCardHeightFor(fontScale)` = 68 + ceil(count/2)*roundFs(56) + roundFs(36) + 余量），布局与测试共用；`settings_ui_cards.cpp` 已引 `kToggleCount`，顺手让卡高同源。

### m5（minor）文档与注释口径过期，会误导后续实现

- **位置**：`docs/superpowers/specs/2026-10-27-release-module-design.md:236-247`；`tests/probes/uat62_page_shots_probe.cpp:10,28,131`；`tests/test_modules/test_app_assembly.cpp:528`
- **问题**：spec §10 仍写"改 8 处"与"侧边栏自动接管，无需改动"，而本次实际改动还需 `app_sidebar.cpp/h`、`app_assembly.cpp`、`settings_ui_logic.h`、两个 probe，工厂也从单个 `create_release_module()` 变为三个；probe 与测试注释仍称"8 行/8 个真模块"。
- **影响**：后续按 spec §10 接入模块/分组会漏掉侧边栏机制与装配层多页键逻辑；"自动接管"的陈述让实现者以为不用读 `app_sidebar.cpp` 的分组约定。
- **建议**：spec §10 增补"分组二级菜单"一节（parent 列、分组父项 nullptr 契约、多页一键门控、装配层逐行拨、需要同步的 11 个落点），并更新 §11（自绘折叠已落地）；probe/测试注释同步到 11 行。

### m6（minor）两个新/改文件缺行尾换行

- **位置**：`src/app_registry.h` 文件尾（`git diff` 的 `\ No newline at end of file`）；`tests/test_modules/test_release_basic.cpp` 文件尾
- **问题**：末行无 `\n`。
- **影响**：纯一致性问题；部分工具/后续 diff 会产生无意义的末行变更噪声，个别 git 配置下 diff 显示难读。
- **建议**：补行尾换行。

### m7（minor）分组在侧栏的位置由登记表尾随顺序隐式决定，落在 system 组之后且无组间分隔

- **位置**：`src/app_registry.h:54-56`（追加在 About 之后）；`src/app_sidebar.cpp:570-573,584-594`（`isSystemGroup` 只认 Settings/About，divider 只画一次）
- **问题**：「上线」分组渲染在「设置/关于」之下，`isSystem=false` 但 divider 已画过，视觉上归属 system 区且无所属分隔；若后续再往表尾加分组，会继续叠在 system 区。
- **影响**：当前视觉可接受，但"工作区/系统区分组"和"分组归属"都没有显式元数据；第二个分组出现时要重做排序/分节逻辑（可能改动分组头绘制顺序与 divider 算法）。
- **建议**：给 `ModuleEntry` 增显式 `section`（work/system）或分组行索引，侧边栏按 section 过滤后再画 divider；若产品上就是要把上线放最底部，至少在 spec 里写明这是约定而非顺序巧合。

---

## 附：未验证项（诚实声明）

- 审计全程**未编译、未运行 `devdesk_tests`/probe**；B2 的"必红"来自常量与断言的字面比对，M4 的"不变红"来自测试覆盖面的静态盘点——两者都未用实际运行复核。
- 未做真窗口渲染验证：侧边栏分组展开/收起、折叠侧栏下的视觉偏移（m1）只有代码几何推导，没有截图证据。
- 未验证 macOS 行为（本审计在 Windows 工作树静态阅读；新代码无平台分支，风险低）。