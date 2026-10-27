# DevDesk 上线模块实施计划（含任务单）

> **依据**：`docs/superpowers/specs/2026-10-27-release-module-design.md`（含 2026-10-27 修订 D-27-01~06）
> **前置产物**：`docs/superpowers/specs/2026-10-27-release-module-tokens.json`（T0.1 产出）、`docs/superpowers/plans/2026-10-27-release-module-visual-acceptance.md`（T0.4 产出）
> **范围**：只做上线模块与侧边栏二级菜单这一项通用能力；不抽公共组件、不改既有数据表、不做未来扩展（GitLab / CI / 导出 / 多机同步）。
> **每单预算**：一个子任务 30 分钟内完成。超时不是加人，而是拆单；拆单后重新标依赖。
> **超时拆单示例**：T1.1 若 30 分钟内做不完，按文件边界拆成 T1.1a（接口 + 骨架 + 装配）与 T1.1b（既有契约测试更新），两单串行；不把两件事压成一张超时单。

---

## 0. 执行纪律（本计划的前置约束）

1. **主会话只编排**：本计划的任何代码 / 测试 / 配置 / 文档改动都派子任务；主会话只做拆分、派单、只读核查、合并与提交决策。
2. **涉及编译 / 测试的单必须 worktree 隔离**：子任务在独立 worktree 完成并原子 commit（显式 pathspec，禁 `git add -A`）；主工作树禁跑构建。
3. **构建槽唯一**：全项目同一时刻只允许 1 个子任务执行编译 / 测试；并行只用于「改码」，不用于「构建」。需要构建的单必须先在派单 prompt 里写明「当前占用构建槽」。
4. **只跑定向测试**：统一 `bash scripts/build-test-run.sh --no-run --filter release_<子串>`（Windows 额外确认 PATH 由脚本处理；按项目惯例先设本地 `TMP`，避 `%TEMP%` I/O 阻塞导致 DB open 挂死的已知环境坑）；全量套件只在 T5.6 里程碑由主会话跑一次。禁止启动 app；probe 例外，用探针自己的 `--screenshot` 跑完即退。
5. **每单报告**：分支 + commit sha + 改动文件清单 + 一句「改了什么、为什么」 + 定向测试命令与结果。缺一项视为未完成。
6. **先红后绿**：修复 / 行为类任务必须给出「回退该改动会红、恢复后绿」的证据，或等价的变异验证；恒真断言按假绿处理。
7. **防事故字段是硬要求**：每张单的「防什么事故」写的是**这条改动回退时会复发的具体事故**，不是覆盖率口号。

---

## 1. 文件白名单总原则

- 每张单只能动它列出的文件；跨单共用文件一律走「串行链」。
- 新模块只新增 `src/module_release/` 下的文件；动既有文件只做：枚举加一项 / 注册表加一行 / switch 加一个 case / CMake 加一行 / 设置页加一项 / 侧边栏加一条分支。
- `src/core/schema.h` 只追加 `CREATE TABLE IF NOT EXISTS` 与索引，不改既有表、不新增 `addMissing*` 迁移函数。
- `docs/superpowers/demo/release-module-demo.html` 在 T0.3 之后视为**冻结视觉基线**；后续只有 T5.4 的「改 demo」处置项可以再动它。

---

## 2. 串行链与并行矩阵

### 2.1 必须串行（同一文件 / 同一构建槽）

| 链 | 顺序 | 原因 |
|----|------|------|
| 装配 | T1.1 | 独占 `module_interface.h` / `app_registry.h` / `main.cpp` / 根 `CMakeLists.txt` / 既有契约测试 |
| 设置 | T1.1 → T1.2 | `settings_config.h` 先加常量，再加迁移字段 |
| 侧边栏 | T1.3 → T4.3 | 同一 `src/app_sidebar.cpp` |
| 模块骨架 | T1.1 → T1.4 → T4.2 | 同一 `release_module.cpp` |
| 测试登记 | T1.1 → T1.5 | 同一 `tests/CMakeLists.txt` |
| 数据仓储 | T2.3 → T2.4 → T2.5 | 同一 `release_store.h/.cpp` |
| 业务编排 | T2.8 → T2.9 → T2.10 | 同一 `release_service.h/.cpp` |
| 任务列表 UI | T3.2 → T3.3 | 同一 `release_ui_tasks.cpp` |
| 执行页 UI | T3.5 → T3.6 →（T3.9 ∥ T3.10） | T3.5/T3.6/T3.10 共用 `release_ui_exec.cpp`；T3.9 碰 `release_ui_exec_env.cpp` + dnd，T3.10 碰 exec/prereq，二者可在 T3.6 后并行 |
| 构建槽 | 所有需要编译 / 测试的单 | 全项目唯一，先到先得 |

### 2.2 可并行（不同文件；构建仍排队）

- T0.2 / T0.3 / T0.4（T0.3 依赖 T0.2）。
- T1.1 完成后：T1.2 ∥ T1.3 ∥ T2.1（schema 独立文件）；T1.4 依赖 T1.1；T1.5 依赖 T1.4。
- T2.1 + T2.2 完成后：T2.6 ∥ T2.7 ∥（T2.3→T2.4→T2.5 串行链）。
- T3.1 完成后：T3.4 ∥ T3.8 ∥（T3.2→T3.3）∥（T3.5→T3.6）；T3.7 在 T2.10 后可与 T3.5 并行。
- T4.1 在 T2.9 后可与 T3.x 并行；T4.2 依赖 T4.1 + T3.1；T4.3 依赖 T4.2 + T1.3（侧边栏串行链）。
- T5.x 按依赖顺序；审计（T5.7）必须等所有实现与验证单完成。

### 2.3 依赖图（简化）

```
T0.1 → T0.2 → T0.3 ┐
T0.1 → T0.4 ───────┤
                    ├→ T3.1 ─┬→ T3.2 → T3.3
T1.1 → T1.2         │        ├→ T3.4
T1.1 → T1.3         │        ├→ T3.8
T1.1 → T1.4 → T1.5  │        └→ T3.5 → T3.6 → T3.9
T1.1 → T2.1 ────────┤
T1.4 → T2.2 ─┬→ T2.6 ┤
             ├→ T2.7 ┤
             └→ T2.3 → T2.4 → T2.5
T2.4 + T2.7 → T2.8 → T2.9 → T2.10
T2.9 + T2.7 → T4.1 → T4.2 → T4.3
T3.6 + T2.6 + T2.10 → T3.10
T4.2 → T4.4
所有实现 → T5.1/T5.2/T5.3/T5.4/T5.5 → T5.6 → T5.7 → T5.8 → T5.9
```

---

# 阶段 T0：前置产物（只写文档/脚本，不写业务代码）

### T0.1 产出 token 清单（唯一数值源）

- **目标**：从 `src/core/theme.cpp`（`lightTokens/darkTokens`）、`src/ui_kit/metrics.h`、`src/ui_kit/button.h`、`src/ui_kit/selection_bar.cpp`、`src/ui_kit/card.cpp`、`src/app_sidebar.cpp` 提取本模块用到的全部数值，写入 `docs/superpowers/specs/2026-10-27-release-module-tokens.json`。
- **改动文件白名单**：`docs/superpowers/specs/2026-10-27-release-module-tokens.json`（新建）。
- **验收判据**：
  - JSON 可被 `python -c "json.load(...)"` 解析；字段覆盖 spec §13.1 的 `colors/sizes/metrics/release/lineHeights/font`。
  - 每个值带 `source`（文件 + 常量名），可逐条回溯到代码；颜色同时收 light/dark。
  - `colors.release.reminderYellow = "#EAB308"` 存在并注明「theme 无黄档，release 专属」。
  - 不修改任何源码；`git diff` 只有这一个文件。
- **阻塞边**：无。
- **并行**：可与 T1.x 同时进行；不占构建槽。
- **防什么事故**：demo、C++、文档三处各写一套数字（页面标题 24/32、正文 14/15 的漂移已有先例）；后续实现凭记忆写字面量，改一处漏两处。

### T0.2 token 生成/校验脚本

- **目标**：`scripts/gen-release-tokens.py` 支持 `--write` 与 `--check`：从 T0.1 的 JSON 生成 `src/module_release/release_tokens.generated.h` 与 demo 的 `/* BEGIN GENERATED TOKENS */ … /* END GENERATED TOKENS */` 块；`--check` 只读比对，不一致退出码非 0。
- **改动文件白名单**：`scripts/gen-release-tokens.py`（新建）。
- **验收判据**：
  - `--write` 幂等（连续跑两次产物字节一致）；生成头带「禁止手改」与源文件名。
  - `--check` 在 demo 手工改一个值后必红（退出码 1 且打印文件名 + 键名 + 期望/实际）；恢复后绿。
  - Windows 控制台非 ASCII 不崩；文件读写显式 UTF-8。
  - 脚本不依赖第三方包（纯 stdlib）。
- **阻塞边**：T0.1。
- **并行**：T0.3 依赖本单；可与 T0.4 并行。
- **防什么事故**：校验脚本只打印 WARNING 导致「假绿」（本项目已有自定义 check 不 raise 的教训）；GBK 终端把中文路径/键名写坏。

### T0.3 demo 对齐代码与四闸基线

- **目标**：把 `release-module-demo.html` 改成可直接作为闸 3/闸 4 基线的原型：
  1. 插入 T0.2 的生成 token 块，删除手写重复变量；
  2. 侧边栏几何对齐代码：宽 208/72、水平 padding 16、顶部 32、项高 40、项间距 4、品牌块 y=32/h=56/logo 48/品牌→导航 28；子项缩进 24；上线排在内侧栏「待办」之后；
  3. 页面 padding 统一左/右/下 32、上 0；删除 `<1240px` 下把 padding 改 24、把后端步骤格改 3 列的媒体覆盖（契约固定 4 列）；
  4. 环境分组去掉折叠行为，改为常展开；删除 Accordion 相关 JS；
  5. 去掉 mono 字体（branch / 单号改正文字体）；
  6. 所有契约文本用显式 px 行高（32/26/24/22/18），不用无单位 `line-height`；
  7. 任务卡环境 chips 改为单行不换行（放不下显示「+N」），避免 chip 换行影响卡高。
- **改动文件白名单**：`docs/superpowers/demo/release-module-demo.html`。
- **验收判据**：
  - `python scripts/gen-release-tokens.py --check` 绿；
  - 浏览器打开无 JS 报错；四个页面（任务列表 / 执行 / 新建 / 基础配置）与提醒弹层仍可切换；
  - 任务列表首屏 10、滚动加载 10、底部「加载中…」仍可复现；
  - 侧栏二级菜单展开/收起可用；折叠态不显示子项；
  - 全文检索无 `Accordion`、无 `font-family:var(--mono)`、无 `font-size:32px`、无 `font-size:15px`。
- **阻塞边**：T0.2。
- **并行**：可与 T0.4 并行；不占构建槽。
- **防什么事故**：demo 与实现各说各话（预览好看、实现不对）；实现期才发现 Accordion 缺失而临时降级；浏览器字体度量与 FreeType 不同导致换行位置不同。

### T0.4 控件映射表 + 视觉契约表 + 验收单模板

- **目标**：把 spec §13.2 / §13.3 的表格落成可执行清单，并新建并排目检验收单模板。
- **改动文件白名单**：`docs/superpowers/specs/2026-10-27-release-module-design.md`（只补表格缺口，不改规则）、`docs/superpowers/plans/2026-10-27-release-module-visual-acceptance.md`（新建）。
- **验收判据**：
  - demo 中每个可见/可交互元素在映射表里都有落点；无「待定」「类似 XX」。
  - 缺口项均有一句话决定（不用 / 自绘 / 上游补），无「实现时再看」。
  - 验收单模板含列：序号 / 元素 / demo 坐标或尺寸 / probe 坐标或尺寸 / 差异 / 处置（改实现、改 demo、接受+理由）/ 责任人 / 状态。
  - 契约表至少覆盖 spec §13.3 的全部行。
- **阻塞边**：T0.1（数值不得另写一套）。
- **并行**：可与 T0.2/T0.3 并行；不占构建槽。
- **防什么事故**：映射表漏掉一个元素，实现期才发现没有组件，最后做成「看起来差不多」的临时件；验收单没有「处置」栏，差异被口头消化。

---

# 阶段 T1：地基（装配、设置、侧边栏、模块骨架）

### T1.1 装配地基（PageId / 注册表 / main / CMake / 既有契约测试）

- **目标**：让 `module_release` 以最小骨架出现在侧边栏并进入 Router，同时把 PageId 新序落进既有契约测试。
- **改动文件白名单**：
  - `src/core/module_interface.h`（PageId 加 `Release`；`pageIdToString`；§10.1 子项虚函数默认实现）
  - `src/app_registry.h`（工厂声明 + 注册行）
  - `src/main.cpp`（静态实例 + `moduleForPage` case）
  - `src/module_settings/settings_config.h`（只加 `kModuleIdRelease = "release"`）
  - `src/module_release/CMakeLists.txt`（新建，初始只列 `release_module.cpp`）
  - `src/module_release/release_module.h` / `release_module.cpp`（新建骨架：名称「上线」、短名「上线」、图标 `\xEF\x82\x93` = U+F093、空 compose）
  - 根 `CMakeLists.txt`（`add_subdirectory(src/module_release)` + app 链接 + 覆盖率插桩清单）
  - `tests/CMakeLists.txt`（`devdesk_module_release` 链接）
  - `tests/test_core/test_router.cpp`、`tests/test_modules/test_app_assembly.cpp`、`tests/test_modules/test_module_icons.cpp`、`tests/test_modules/test_claude.cpp`、`tests/test_modules/test_app_sidebar.cpp`
- **验收判据**：
  - PageId 新序固定：Home 0、Markdown 1、Todo 2、**Release 3**、ClaudeProxy 4、Agent 5、Wallpaper 6、Settings 7、About 8；`test_claude` 的 ClaudeProxy 期望值 3→4。
  - 注册表 9 行、行序 == 枚举序；`all-off` 仍只出 Home/Settings/About 3 条；`all-on` 9 条。
  - `--filter core_pageid_order`、`--filter assembly_`、`--filter module_icons`、`--filter app_sidebar` 全绿。
  - 无头 compose 断言侧边栏出现「上线」入口（节点 `app.sidebar.item.Release.hit`）；不启动 app。
- **阻塞边**：无（但必须在 T1.2 之前完成，`kModuleIdRelease` 由本单提供）。
- **并行**：可与 T2.1 并行；占用构建槽；与 T1.2/T1.3 共享构建槽，需排队。
- **防什么事故**：PageId 枚举序与注册表序漂移（`core_pageid_order` 注释明确：重排会静默改变默认导航顺序）；新增枚举漏 switch case 被 `-Wswitch -Werror` 拦下；新模块未链接导致侧边栏空白。

### T1.2 设置页接入 + 存量配置一次性纳入

- **目标**：上线模块可被设置页开关；存量配置升级后能直接看到新模块，用户手动关闭后不再被重新加回。
- **改动文件白名单**：`src/module_settings/settings_config.h`（`modulesVersion` 字段）、`src/module_settings/settings_config.cpp`、`src/module_settings/settings_ui_cards.cpp`、`src/module_settings/settings_ui_logic.h`、`tests/test_modules/test_settings.cpp`。
- **验收判据**：
  - `kKnownModuleIds` / `defaultEnabledModules()` 含 `release`。
  - 老配置（`enabledModules` 存在但无 `release`、无 `modulesVersion`）`load()` 后含 release 且内存版本 = 2。
  - 保存过版本 2 且用户关闭 release 的配置，下次 `load()` 不再自动加回。
  - 模块开关卡出现第 5 项「上线」；`kModulesCardHeight = 232`；85% / 100% / 130% 三档字号下卡内容不裁切（现有 roundFs 口径）。
  - `--filter settings` 绿。
- **阻塞边**：T1.1（`kModuleIdRelease` 已存在）。
- **并行**：可与 T1.3、T2.1 并行；占用构建槽。
- **防什么事故**：存量用户升级后侧边栏看不到新模块（等于功能不存在——与「修复合并不等于交付」同类）；卡高没随第 5 项增长导致开关被裁切；一次性迁移写成「每次加载都补」导致用户无法关闭模块。

### T1.3 侧边栏二级菜单渲染 + 回归断言

- **目标**：`app_sidebar.cpp` 增加一条「有子项」渲染分支；无子项模块的节点与几何保持原样。
- **改动文件白名单**：`src/app_sidebar.cpp`、`tests/test_modules/test_app_sidebar.cpp`。
- **验收判据**：
  - `Release` 父项右侧有 16×16 箭头；子项 `tasks` / `config` 缩进 24、宽 152、高 40、间距 4。
  - 父项点击展开/折叠；子项点击导航并调 `onChildNavSelected`；激活子项画 `selectionBar`。
  - 折叠态（72 宽）不渲染子项、不增加垂直空间；父项点击可导航。
  - 无子项模块的节点 id、单行几何（x / 宽 / 高 / 间距）、分组规则保持原版；整体 y 因新增「上线」一项而平移属预期，断言按新表序计算（回归断言逐项绿）。
  - `--filter app_sidebar` 绿；新增用例名含 `sidebar_child_nav_`。
- **阻塞边**：T1.1（接口与注册行）。
- **并行**：可与 T1.2、T2.1 并行；与 T4.3 共用 `app_sidebar.cpp`，必须串行在其前。
- **防什么事故**：二级菜单改动顺手改了所有导航项几何（无子项页面跟着位移/变样）；折叠态子项溢出遮挡底部开关；子项点击后父页 compose 不切换。

### T1.4 模块源码骨架 + 模块 CMake

- **目标**：按 spec §10.2 落地 `module_release` 全部源文件骨架（空实现可编译）与模块 CMake；实现 `release_module.cpp` 的子页路由（tasks/config）。
- **改动文件白名单**：`src/module_release/CMakeLists.txt`、`src/module_release/release_module.h/.cpp`，以及 §10.2 列出的其余 `.h/.cpp`（新建空实现，只有 include guard / namespace / 必要声明；**不含** `release_tokens.generated.h`，它由 T3.1 生成）。
- **验收判据**：
  - `devdesk_module_release` 目标可编译；全部新文件已被 CMake 显式列出。
  - `childNavItems()` 返回 tasks/config；`activeChildNavId()`、`onChildNavSelected()` 状态可切；compose 按子页分派到对应 UI 函数（骨架阶段可空）。
  - 关闭模块（onStop）不崩溃；子页状态回到默认 tasks。
- **阻塞边**：T1.1（子项接口已在 T1.1 声明）；与 T1.3 可并行（不同文件）。
- **并行**：与 T1.2/T1.3 改的是不同文件，可并行改码；构建排队。
- **防什么事故**：后续 UI 任务各自往 CMake 加文件导致同文件多写者；子页状态放在模块外导致切页丢失；模块停止后回调仍被调用。

### T1.5 测试脚手架与 tests/CMakeLists 登记

- **目标**：一次性建好本模块全部测试文件骨架并登记，后续单只改自己的测试文件，避免反复改 `tests/CMakeLists.txt`；同时把三支 release probe 名称预先加入 probe 的 app 层链接特例（文件尚不存在，GLOB 不会命中，等 T4.4/T5.2/T5.3 落地即自动继承正确链接面）。
- **改动文件白名单**：`tests/CMakeLists.txt`、`tests/test_modules/test_release_module.cpp`、`test_release_model.cpp`、`test_release_store.cpp`、`test_release_validate.cpp`、`test_release_time.cpp`、`test_release_service.cpp`、`test_release_reminder.cpp`、`test_release_ui_common.cpp`、`test_release_ui_tasks.cpp`、`test_release_ui_create.cpp`、`test_release_ui_exec.cpp`、`test_release_ui_config.cpp`、`test_release_ui_dialogs.cpp`、`test_release_ui_dnd.cpp`、`test_release_scenarios.cpp`（均新建，各含 1 条 trivial 用例）。
- **验收判据**：
  - `devdesk_tests` 可编译；`--filter release_` 能列出并跑通这些 trivial 用例。
  - `tests/CMakeLists.txt` 只改一次；probe 特例名已含 `release_ui_probe` / `release_ui_pages_probe` / `release_reminder_probe`；probe 的 `target_link_libraries` 已加 `devdesk_module_release`；后续单不再碰它。
  - 测试文件名与 tasks 单的验收命令一一对应。
- **阻塞边**：T1.4（源码骨架存在）。
- **并行**：可与 T2.1 并行；构建排队。
- **防什么事故**：多张测试单同时改 `tests/CMakeLists.txt` 撞写者；测试未登记导致「本地跑不到」的假绿。

---

# 阶段 T2：数据层（schema、模型、仓储、纯逻辑、业务编排）

### T2.1 新增 release 表与索引

- **目标**：按 spec §9 在 `core/schema.h` 追加 9 张表与索引。
- **改动文件白名单**：`src/core/schema.h`、`tests/test_core/test_schema.cpp`。
- **验收判据**：
  - 新库 `applySchema` 后 9 张表 + 索引齐全。
  - **老库场景**：用 release 表不存在的旧库副本跑 `applySchema`，新表全部出现，既有表行数/内容不变。
  - `release_task_plan_date_active` 唯一索引：同一天两个非作废任务第二条失败；作废后可插入；恢复冲突由服务层拦截。
  - `--filter schema` 绿。
- **阻塞边**：无（独立于 T1）。
- **并行**：与 T1.x 并行；占用构建槽。
- **防什么事故**：老库永远缺新表（UAT 三连爆的「老库缺列」同类）；改既有 DDL 静默破坏历史数据；索引写成全表唯一导致作废后不能重建。

### T2.2 领域模型 / 枚举 / 快照结构

- **目标**：`release_model.h` 定义服务类型、状态、步骤类型、配置类型、任务/环境/服务/配置/日志结构；纯头文件，零 SQL、零 UI。
- **改动文件白名单**：`src/module_release/release_model.h`、`tests/test_modules/test_release_model.cpp`。
- **验收判据**：枚举值覆盖 spec §9 全部取值；快照字段与表列一一对应；默认值（空分支/空内容/未完成）明确；测试断言枚举字符串映射与默认值。
- **阻塞边**：T1.5（测试文件已登记）。
- **并行**：与 T2.1 并行（不同文件）；构建排队。
- **防什么事故**：状态字符串散落各文件导致拼写漂移；模型字段与表列不同名，读写时错位。

### T2.3 仓储 A：基础配置（服务 / 环境 / 环境-服务）

- **目标**：`release_store.h/.cpp` 实现基础配置 CRUD、事务、唯一名、级联删除。
- **改动文件白名单**：`src/module_release/release_store.h`、`src/module_release/release_store.cpp`、`tests/test_modules/test_release_store.cpp`。
- **验收判据**：服务/环境增删改查；重名返回可识别错误；删除服务自动级联删除 `release_env_service`；删除环境级联；SQL 全绑定参数；失败返回 `Result` 且不吞错；测试覆盖空库/重复名/级联。
- **阻塞边**：T2.1、T2.2。
- **并行**：与 T2.6/T2.7 并行（不同文件）；与 T2.4/T2.5 串行（同文件）。
- **防什么事故**：删除服务后环境里留悬空引用；SQL 拼接注入；写失败静默返回成功。

### T2.4 仓储 B：任务 / 环境快照 / 服务快照 / 分页

- **目标**：实现任务创建事务（任务 + 环境 + 服务 + 配置行一次写入）、按日查询、瀑布流分页查询（`ORDER BY plan_date DESC, id DESC LIMIT/OFFSET`）、环境/服务移除。
- **改动文件白名单**：`src/module_release/release_store.h/.cpp`、`tests/test_modules/test_release_store.cpp`。
- **验收判据**：同一天已有非作废任务时返回已有任务 id（不新建）；分页 10/10 无重复无遗漏；快照内容与当时基础配置一致，之后改基础配置不影响任务；移除服务后查询默认不返回；同一任务重复加入同一环境 / 同一环境重复加入同一服务被唯一约束拒绝；移除后重新加入同环境同服务 = 复用原行并恢复 `removed=0`（不撞唯一约束）；事务中途失败无半条任务。
- **阻塞边**：T2.3。
- **并行**：与 T2.6/T2.7 并行；与 T2.5 串行。
- **防什么事故**：快照引用主键导致改基础配置污染历史任务；分页 `OFFSET` 与排序不稳定导致重复/漏条；半写入任务导致详情页空列表。

### T2.5 仓储 C：配置项 / 步骤 / 日志 / 完成查询

- **目标**：实现配置项保存（含跳过）、步骤完成/撤回、日志追加、按任务汇总查询。
- **改动文件白名单**：`src/module_release/release_store.h/.cpp`、`tests/test_modules/test_release_store.cpp`。
- **验收判据**：配置内容**实际变化**时才清空 `done_at` 并写日志；内容未变（打开即保存）不撤回；步骤 `UNIQUE(task_service_id,kind)` 冲突可幂等处理；日志按时间升序（同秒按 id）可取；汇总查询返回每个服务的配置/步骤完成态；失败不吞错。
- **阻塞边**：T2.4。
- **并行**：与 T2.6/T2.7 并行；与 T2.8 串行（T2.8 依赖本单）。
- **防什么事故**：改内容后旧确认仍生效（spec §4.4 核心规则失效）；日志缺失导致撤回不可追溯；重复确认产生多行。

### T2.6 纯校验：分支 / 配置 / 单据号 / 完成判定

- **目标**：`release_validate.h/.cpp` 实现所有可单测的判定，不碰 DB/UI。
- **改动文件白名单**：`src/module_release/release_validate.h/.cpp`、`tests/test_modules/test_release_validate.cpp`。
- **验收判据**：
  - 未移除服务分支空 → 不合格；未跳过配置空 → 不合格；跳过项不报缺。
  - 空任务（无环境 / 无服务）不可提交。
  - 单据号：仅空白 → 锁定；非空 → 放行；只判非空，不判格式。
  - 环境完成 = 未移除服务全部完成；任务完成 = 所有环境完成 + 全部前置完成（nacos/DB 工单卡只在可见时计入）；环境服务全移除 → 完成。
  - 测试覆盖上述每条的正反例。
- **阻塞边**：T2.2。
- **并行**：与 T2.3/T2.4/T2.5/T2.7 并行；构建排队。
- **防什么事故**：完成判定「真空真」导致空任务一建即完成；单据号只靠 UI 禁用、程序守卫缺失；跳过项被误判为缺失。

### T2.7 纯时间：跨零点 / 到期 / 超时 / 状态时间线

- **目标**：`release_time.h/.cpp` 实现分钟与日期转换、跨零点窗口、到期时间（基准−1h / 基准 / 环境开始）、超时判定、按时间推导状态。
- **改动文件白名单**：`src/module_release/release_time.h/.cpp`、`tests/test_modules/test_release_time.cpp`。
- **验收判据**：23:30→00:30 的窗口结束在次日；结束 ≤ 开始按次日；基准取最早环境开始；基准−1h 跨日（如基准 00:30 → 前一日 23:30）正确；到期时间正确；已作废/已完成不参与；测试含边界分钟 0/1439、跨零点、无环境。
- **阻塞边**：T2.2。
- **并行**：与 T2.3-T2.6 并行；构建排队。
- **防什么事故**：跨零点被当成同日导致提前 24h 超时；到期时间算错导致提醒过早/过晚；本地日期与 epoch 混用。

### T2.8 业务编排 A：创建 / 同日复用 / 快照 / 配置行

- **目标**：`release_service.h/.cpp` 实现创建流程：计划日期解析、同日已有任务则返回既有、非常规日确认标记、快照写入、按服务类型生成配置行。
- **改动文件白名单**：`src/module_release/release_service.h/.cpp`、`tests/test_modules/test_release_service.cpp`。
- **验收判据**：周二/周四直接建；其他日必须带确认标记；同日已有非作废任务返回既有 id；后端 5 配置行、前端 0 行；默认分支带入；快照与基础配置一致。
- **阻塞边**：T2.4、T2.7。
- **并行**：与 T2.6 并行（不同文件）；与 T2.9/T2.10 串行。
- **防什么事故**：同日重复建任务撞唯一索引；非常规日绕过确认；前端服务被塞入配置项。

### T2.9 业务编排 B：提交 / 状态 / 完成度 / 作废与恢复

- **目标**：提交校验、状态机（configuring→pending→running→done）、超时标记、作废/恢复、恢复冲突拦截。
- **改动文件白名单**：`src/module_release/release_service.h/.cpp`、`tests/test_modules/test_release_service.cpp`。
- **验收判据**：校验不过不放行；提前勾前置不改状态；时间到才进 running；全部完成进 done；作废停止提醒并保留进度；恢复按当前进度与时间回到对应状态；同日已有非作废任务时恢复被拒绝；超时不入库。
- **阻塞边**：T2.5、T2.6、T2.8。
- **并行**：与 T3.x 无文件冲突，但 UI 依赖本单接口；与 T2.10 串行。
- **防什么事故**：状态提前跳变（勾前置即执行中）；作废任务仍提醒；恢复覆盖当天另一个任务。

### T2.10 业务编排 C：撤回 / 改内容自动撤回 / 只读锁定

- **目标**：任意步骤撤回（二次确认由 UI 提供）、改配置/分支自动撤回并写日志、已完成任务整单只读、配置顺序阶段锁。
- **改动文件白名单**：`src/module_release/release_service.h/.cpp`、`tests/test_modules/test_release_service.cpp`。
- **验收判据**：撤回清 `done_at` 并写 revoke 日志；内容/分支**实际变化**时清对应确认并写日志（值未变不撤回）；done / void 状态拒绝任何写操作（void 只能恢复后改）；running 后拒绝配置项重排；撤回后完成度重新计算。
- **阻塞边**：T2.9。
- **并行**：与 T3.x 无文件冲突；与 T3.10 有接口依赖。
- **防什么事故**：改内容后旧确认仍算数（安全语义失效）；已完成任务仍能被改；撤回只改 UI 不改库。

---

# 阶段 T3：UI（页面、卡片、弹窗、拖拽、校验呈现）

### T3.1 生成头 + UI 公共件 + token 门禁测试

- **目标**：运行 T0.2 脚本生成 `release_tokens.generated.h`；实现 `release_ui_common`：页头、区块标题、空状态、chip、进度条、状态文案。
- **改动文件白名单**：`src/module_release/release_tokens.generated.h`（生成）、`src/module_release/release_ui_common.h/.cpp`、`tests/test_modules/test_release_ui_common.cpp`。
- **验收判据**：
  - `python scripts/gen-release-tokens.py --check` 绿；生成头禁止手改（文件头注释 + 脚本可覆写）。
  - 单测 `release_tokens_match_theme`：`fontScale=1.0` 下 theme sizes 与生成常量逐项相等。
  - 页头几何：标题 x=32、y=0、24/700/32；页头高 40；正文起点 y=56。
  - chip 高 22、圆角 999、字号 11/600；进度条高 6、圆角 999。
  - 空状态复用 `ui_kit::emptyState`。
- **阻塞边**：T0.2、T1.4。
- **并行**：完成后 T3.2/T3.4/T3.5/T3.7/T3.8 可并行改码；构建排队。
- **防什么事故**：裸 hex 与字面量再次散落；demo 与代码数值不同步；页头几何各页各写一套。

### T3.2 任务卡

- **目标**：实现任务卡绘制（日期标题 18/600/24、状态 chip、进度条、环境 chips、摘要 3 行、作废置灰），几何与 token 一致。
- **改动文件白名单**：`src/module_release/release_ui_tasks.h/.cpp`、`tests/test_modules/test_release_ui_tasks.cpp`。
- **验收判据**：卡片宽由父级给定且 ≥320 时内容不溢出；摘要第 3 行后截断（节点存在且高度 = 3×22）；环境 chips 单行不换行、放不下显示「+N」；作废卡 opacity 0.68；点击回调带任务 id；节点 id `release.tasks.card.<id>.*`。
- **阻塞边**：T3.1、T2.4。
- **并行**：与 T3.4/T3.8 并行；与 T3.3 串行（同文件）。
- **防什么事故**：卡片高度随字体度量漂移（闸 3 字体差异）；摘要溢出压住后续卡片；作废卡看起来仍可操作。

### T3.3 任务列表瀑布流 + 分页 + 加载占位

- **目标**：`components::virtualmasonry` 接任务列表；首屏 10、底部再加载 10、加载占位、空状态、点击进详情。
- **改动文件白名单**：`src/module_release/release_ui_tasks.h/.cpp`、`tests/test_modules/test_release_ui_tasks.cpp`。
- **验收判据**：
  - 1600×1000 内容宽 1328 → 4 列 × 320；1000×640 内容宽 728 → 2 列 × 356。
  - 首次查询固定 `LIMIT 10`；12 条数据最终加载到 12 且无重复（不足一屏时按 spec §4.0 由 `virtualMasonry` 自动补载，不作为分页 bug）；加载中 `release.tasks.loading` 存在；加载中重复触底不重复追加。
  - 排序按计划日期降序；作废卡仍在列表。
  - 空列表显示空状态 + 「新建任务」按钮。
- **阻塞边**：T3.2、T2.4。
- **并行**：与 T3.4/T3.8 并行；与 T3.5 无文件冲突。
- **防什么事故**：`onEndReached` 同步查库造成无限回调/重复条目；加载占位永远看不见；列宽公式在最小窗溢出。

### T3.4 新建任务页

- **目标**：日期选择、周二/周四约束 + 非常规日二次确认、同日已有任务直接打开、环境选择与快照预览、创建按钮。
- **改动文件白名单**：`src/module_release/release_ui_create.h/.cpp`、`tests/test_modules/test_release_ui_create.cpp`。
- **验收判据**：选非常规日不确认不能创建；确认后可创建；同日已有任务时点创建跳到既有任务详情；默认分支出现在预览；空环境不可创建（与 T2.6 一致）。
- **阻塞边**：T3.1、T2.8。
- **并行**：与 T3.2/T3.3/T3.8 并行；构建排队。
- **防什么事故**：非常规日无确认直接落库；同日重复建任务；预览与实际快照不一致。

### T3.5 执行页 A：前置项 + 单据号门槛

- **目标**：执行页布局；任务级前置卡片（提测/CAB/nacos/DB/公告）；单据号输入；完成勾选与锁定态；公告时间展示。
- **改动文件白名单**：`src/module_release/release_ui_exec.h/.cpp`、`src/module_release/release_ui_exec_prereq.cpp`、`tests/test_modules/test_release_ui_exec.cpp`。
- **验收判据**：
  - 单据号空白 → 锁定态（灰色方框 + 「请先填写单据号」），点击不改变完成态；非空 → `components::checkbox` 可用。
  - 程序守卫测试：直接调用勾选接口且单据号空 → 拒绝 + 日志。
  - nacos/DB 工单卡按内容有无显示；公告卡无单据号。
  - 前置项完成写入日志。
- **阻塞边**：T3.1、T2.5、T2.9。
- **并行**：与 T3.2/T3.3/T3.4/T3.7/T3.8 并行；与 T3.6 串行（同文件）。
- **防什么事故**：复选框无 disabled 属性时做成「点了又弹回」的伪禁用；UI 禁用但程序守卫缺失；工单卡该出现时没出现。

### T3.6 执行页 B：环境 / 服务 / 步骤 / 撤回

- **目标**：环境分组常展开；服务行；后端 8 步 / 前端 3 步；配置内容展示；部署/验证勾选；移除确认；撤回二次确认。
- **改动文件白名单**：`src/module_release/release_ui_exec.cpp`、`src/module_release/release_ui_exec_env.cpp`、`tests/test_modules/test_release_ui_exec.cpp`。
- **验收判据**：分组常展开（无折叠箭头/无 Accordion 依赖）；步骤顺序与 spec §5.1 一致；撤回后该步 UI 立即回未完成且进度重算；移除服务/环境有二次确认；已移除项不出现在执行清单。
- **阻塞边**：T3.5、T2.9。
- **并行**：与其他页面并行；与 T3.9/T3.10 串行（同文件链）。
- **防什么事故**：环境分组做成折叠后发现组件缺失而临时降级；撤回只改 UI 不改库；已移除服务仍占进度。

### T3.7 配置内容全屏编辑

- **目标**：`DialogBuilder.screen(screen.width, screen.height)` 模态全屏编辑器；多行 + 滚动条；保存触发自动撤回与重新校验。
- **改动文件白名单**：`src/module_release/release_ui_dialogs.h/.cpp`、`tests/test_modules/test_release_ui_dialogs.cpp`。
- **验收判据**：弹层覆盖页面区、宽高 = screen 尺寸（留 24 外边距或按 DialogBuilder 现有口径）；输入多行可滚动；保存后内容落库且该步 `done_at` 清空 + 日志；取消不改库；IME 输入不丢字（至少文本注入测试）。
- **阻塞边**：T3.1、T2.10。
- **并行**：与 T3.2/T3.3/T3.4/T3.8 并行；构建排队。
- **防什么事故**：全屏弹窗尺寸算错导致输入区不可见；保存不撤回旧确认；取消却落库。

### T3.8 基础配置页（服务目录 / 环境定义）

- **目标**：服务 CRUD（名称/类型/默认分支）、环境 CRUD（名称/服务选择/时间窗）、删除二次确认、删除服务自动从环境摘除。
- **改动文件白名单**：`src/module_release/release_ui_config.h/.cpp`、`tests/test_modules/test_release_ui_config.cpp`。
- **验收判据**：服务类型切换展示固定配置项说明；环境时间窗允许跨零点（结束 ≤ 开始显示次日）；删除被环境引用的服务出现二次确认且确认后环境同步摘除；历史任务不受影响（用 T2.3 测试断言）；块面式 Tab 用 `ui_kit::wtab`。
- **阻塞边**：T3.1、T2.3。
- **并行**：与 T3.2/T3.3/T3.4 并行；与 T3.9 有接口依赖。
- **防什么事故**：删服务后环境悬空；跨零点时间窗保存后显示错误；用下划线式 `components::tabs` 破坏全局视觉。

### T3.9 拖拽排序（服务顺序 / 配置项顺序）

- **目标**：执行页服务顺序可拖；配置项顺序可拖；只在配置中/待执行阶段可调，进入执行后锁定；结果落库。
- **改动文件白名单**：`src/module_release/release_ui_dnd.h/.cpp`、`src/module_release/release_ui_exec_env.cpp`、`tests/test_modules/test_release_ui_dnd.cpp`。
- **验收判据**：拖动后顺序变化并持久化；重进页面顺序保持；running 状态拖动被拒绝（无视觉跟手 + 程序守卫）；拖拽不遮挡滚动。
- **阻塞边**：T3.6、T3.8。
- **并行**：与 T3.10 并行（不同文件，T3.10 用 exec.cpp + prereq）；构建排队。
- **防什么事故**：执行阶段仍可改顺序（spec §8 #6 失效）；拖完顺序未落库；拖拽与滚动/点击冲突。

### T3.10 提交 / 校验呈现 / 改动撤回反馈

- **目标**：「校验」按钮、提交时自动校验、不合格项标红 + 缺失清单、改内容自动重新校验与撤回提示、done 只读。
- **改动文件白名单**：`src/module_release/release_ui_exec.cpp`、`src/module_release/release_ui_exec_prereq.cpp`、`tests/test_modules/test_release_ui_exec.cpp`。
- **验收判据**：校验失败列清单（每条含服务/环境/步骤定位）；提交失败不改变状态；修正后清单实时消失；已完成任务隐藏/禁用所有写操作；改配置后弹出「已撤回」提示并写日志。
- **阻塞边**：T3.6、T2.6、T2.10。
- **并行**：与 T3.9 并行；构建排队。
- **防什么事故**：提交校验可被绕过；不合格项标红但清单缺失（用户不知道改哪）；已完成任务仍可编辑。

---

# 阶段 T4：提醒

### T4.1 提醒收集 / 分级 / 抑制（纯逻辑）

- **目标**：`release_reminder` 纯逻辑：收集「到期未完成」项、三级分档、按环境分组、窗级取最高优先级、5 分钟抑制、配置中且未到基准不弹、done/void 不弹。
- **改动文件白名单**：`src/module_release/release_reminder.h/.cpp`、`tests/test_modules/test_release_reminder.cpp`。
- **验收判据**：
  - 前置项在基准−1h 到期 → 橙；公告到基准 → 黄；环境开始后部署未完成 → 红；任务已到期仍 configuring → 红。
  - 同窗多档 → 窗级 = 最高；条目保留各自色。
  - 关闭后 5 分钟内不再收集弹窗；到期未完成恢复。
  - 配置中且未到基准 → 空；done/void → 空。
  - 测试用固定时间源，不读真实时钟。
- **阻塞边**：T2.7、T2.9。
- **并行**：与 T3.x 并行；构建排队。
- **防什么事故**：时间重叠弹多个窗；同一项反复弹；应用重启后漏补弹；已作废任务仍提醒。

### T4.2 提醒全屏窗（打开 / 关闭 / 跳转 / 生命周期）

- **目标**：`app::openWindow` 打开工作区尺寸提醒窗；渲染分组列表；点条目关窗 + 唤起主窗 + 导航到任务/步骤；单窗约束；模块停止先关窗。
- **改动文件白名单**：`src/module_release/release_reminder_window.h/.cpp`、`src/module_release/release_reminder.cpp`、`src/module_release/release_module.cpp`、`tests/test_modules/test_release_reminder.cpp`。
- **验收判据**：
  - 窗尺寸 = 工作区物理尺寸（换算正确）；`windowCreatedHandler` 拿到句柄；关闭走 `requestWindowClose`。
  - 已开窗时再次触发不新开；点条目后跳转 payload/子页定位正确。
  - 模块 onStop 后窗口关闭、无悬垂句柄。
  - 窗内只读：无勾选入口。
- **阻塞边**：T4.1、T3.1、T1.4。
- **并行**：与 T3.x 并行（不同文件）；`release_module.cpp` 串行链在 T1.4 后。
- **防什么事故**：多窗叠弹；窗口尺寸单位错配（物理/逻辑）；模块停止后 compose 捕获悬垂 `this`；点条目只关窗不跳转。

### T4.3 主窗每帧提醒拉取 + 重开补弹

- **目标**：`app_sidebar.cpp` 每帧调用提醒 tick（与 `agent::pollRunEnd` 同模式），实现补弹与隐藏/最小化的边界。
- **改动文件白名单**：`src/app_sidebar.cpp`、`tests/test_modules/test_app_sidebar.cpp`、`tests/test_modules/test_release_reminder.cpp`。
- **验收判据**：当前页不是上线页也会 tick；到期扫描 1s 节流、内存快照 5s 或变更事件刷新（**不得每帧查库**）；主窗隐藏/最小化不弹，恢复可见补弹；单帧不重复开窗；无 DB 时只记日志不崩。
- **阻塞边**：T4.2、T1.3（同一 `app_sidebar.cpp`，必须排在 T1.3 后）。
- **并行**：与 T3.x 并行；与 T1.3 串行。
- **防什么事故**：提醒写在模块 compose 里导致只有上线页可见时才提醒（核心功能静默失效）；每帧开窗；无库/无模块时空指针崩溃。

### T4.4 提醒探针（真窗口 + 截图）

- **目标**：`tests/probes/release_reminder_probe.cpp`：真实窗口下注入三档提醒快照，断言窗级色、条目色、几何，并 `--screenshot` 落图。
- **改动文件白名单**：`tests/probes/release_reminder_probe.cpp`（新建；probes 目录 GLOB 自动登记）。
- **验收判据**：三种窗级各跑一次；条目色与档位一致；窗尺寸与工作区换算一致；截图 PNG 合法非全黑；无显示环境返回 77 跳过。
- **阻塞边**：T4.2。
- **并行**：可与 T5.2 的 UI probe 并行；构建排队。
- **防什么事故**：提醒只有逻辑绿、真窗口尺寸/颜色错；截图能力被误改回 screencapture（无权限）。

---

# 阶段 T5：验证与交付

### T5.1 token 门禁复核

- **目标**：复核闸 1：生成头 / demo / theme 三方一致；模块内无裸 hex。
- **改动文件白名单**：只读（若发现脚本缺陷，回到 T0.2 白名单修复）。
- **验收判据**：`python scripts/gen-release-tokens.py --check` 绿；`--filter release_tokens_match_theme` 绿；`grep -nE "#[0-9A-Fa-f]{6}" src/module_release/` 零命中（生成头除外）。
- **阻塞边**：T3.1。
- **并行**：可与 T5.2 并行；不占构建槽（脚本部分）。
- **防什么事故**：四闸只写在文档里没人执行；数值双源复发。

### T5.2 任务列表 / 卡片 probe 几何断言

- **目标**：`tests/probes/release_ui_probe.cpp`：真窗口 1600×1000 与 1000×640，侧栏展开/折叠，断言 spec §13.3 的列表/卡片/页头/loading 几何，并落两张 PNG。
- **改动文件白名单**：`tests/probes/release_ui_probe.cpp`（新建）。
- **验收判据**：spec §13.3 表逐行断言（至少列表/卡片/页头/loading/chips/进度条）；两尺寸列数与卡宽符合公式；侧栏折叠后 72 宽、子项不渲染；闸 4 截图时 `setCustomTitlebarMode(false)`（与 demo 同坐标系）；断言失败打印节点 id/期望/实际并非零退出；截图非全黑。
- **阻塞边**：T3.3、T1.3。
- **并行**：与 T5.1/T4.4 并行；与 T5.3 共享构建槽，可同批不同文件。
- **防什么事故**：几何只在无头单测里验、真窗口缩放出错；列数公式在最小窗失效；截图假成功（黑图）。

### T5.3 执行页 / 基础配置 / 弹窗 probe 几何 + 明暗色

- **目标**：新增 `release_ui_pages_probe.cpp`，覆盖执行页分组/步骤格、基础配置、全屏编辑器、明暗两主题。
- **改动文件白名单**：`tests/probes/release_ui_pages_probe.cpp`（新建；**不改** T5.2 的 `release_ui_probe.cpp`，避免同文件冲突）。
- **验收判据**：spec §13.3 剩余行全部断言；light/dark 各跑一次；颜色取自 `theme::current()`；弹窗尺寸与遮罩色断言；全部 probe `ctest -L probe -R release` 绿。
- **阻塞边**：T3.6、T3.7、T3.8、T3.10。
- **并行**：与 T5.1/T5.2 部分并行；构建排队。
- **防什么事故**：只验任务列表、其余页面视觉退化没人发现；暗色模式下颜色写死 Light 值。

### T5.4 并排目检与验收单

- **目标**：按闸 4 完成 demo vs probe 并排比对，差异逐条进验收单并处置。
- **改动文件白名单**：`docs/superpowers/plans/2026-10-27-release-module-visual-acceptance.md`（只填单；若需改 demo，先回 T0.3 白名单，不在本单直接改实现）。
- **验收判据**：1600×1000 与 1000×640 各一组图；每条差异有处置；无「待定」；「接受」项写明理由；未处置差异为 0。
- **阻塞边**：T5.2、T5.3、T0.3。
- **并行**：不占构建槽；必须等两组 probe 完成。
- **防什么事故**：并排目检变成「看一眼差不多」；字体/间距差异被口头放过；demo 与实现的偏差没有回归入口。

### T5.5 业务场景矩阵（含老库升级）

- **目标**：`test_release_scenarios.cpp` 固化跨层场景，覆盖：
  1. 老库（无 release 表）启动升级；
  2. 同一天唯一 / 作废后可建 / 恢复冲突；
  3. 跨零点时间窗与超时；
  4. 首屏 10 + 加载 10 + 无重复；
  5. 单据号空/空白/非空三态 + 程序守卫；
  6. 提醒三档 + 5 分钟抑制 + 重开补弹 + configuring/done/void 不弹；
  7. 快照不随基础配置变化；
  8. 删服务被环境引用 → 确认 + 自动摘除；
  9. 改配置/分支自动撤回 + 日志；
  10. 环境服务全移除 → 环境完成；
  11. 前端服务只有 3 步；
  12. running 后配置顺序锁定。
- **改动文件白名单**：`tests/test_modules/test_release_scenarios.cpp`。
- **验收判据**：12 条场景每条一个用例、注明「防什么事故」；全绿；老库场景用真实旧库副本或等价 schema 前置。
- **阻塞边**：T2.10、T3.10、T4.3。
- **并行**：需等待实现完成；与 T5.2/T5.3 可并行改码，但构建排队。
- **防什么事故**：单测全绿但存量库/跨零点/补弹等真实场景未跑（UAT 三连爆教训）。

### T5.6 里程碑全量门禁（主会话执行）

- **目标**：在 `dev` 集成分支最新 tip（各任务已按「完成即合 dev」规则汇入）上跑一次全量测试 + 全部 probe + 模块隔离脚本 + token check。
- **改动文件白名单**：无（只跑命令；失败回对应任务单修）。
- **验收判据**：`bash scripts/build-test-run.sh --no-run` 全绿；`ctest -L probe` 全绿；`bash scripts/check-module-isolation.sh` 绿；`python scripts/gen-release-tokens.py --check` 绿；macOS 兼容门禁六维清单已过（无平台分支也要写明「已核」）；报告贴 `Total / Passed / Failed` 行（不向用户报数字，只用于门禁）。
- **阻塞边**：T5.1-T5.5。
- **并行**：独占构建槽。
- **防什么事故**：分支绿 ≠ 合并绿（f274f74 事故）；worktree 审计闸不覆盖合并结果。

### T5.7 独立审计（六维）

- **目标**：另派只读子任务按 CLAUDE.md 功能生命周期阶段 2 的六维审计：正确性 / 回归面 / 平台对称性 / 静默失败面 / 测试有效性 / 文档口径。
- **改动文件白名单**：`docs/superpowers/plans/2026-10-27-release-module-audit.md`（新建；只写审计文件）。
- **验收判据**：六维逐条结论（「未发现问题」也要写「已核」）；每条 finding 带 `file:line` + 影响 + 建议；审计者不是实现者。
- **阻塞边**：T5.6。
- **并行**：只读，不占构建槽。
- **防什么事故**：实现者自审失去第二视角；审计无 file:line 无法处置；范围外顺手改（审计只读）。

### T5.8 按审计修 + 复验

- **目标**：逐条处置 findings（修 / 不修 + 理由），复跑受影响定向测试与 probe。
- **改动文件白名单**：由 findings 决定，逐条列出；只允许需求交付物缺陷，历史遗留与范围外项不修。
- **验收判据**：findings 文件每条有处置结论；修复项有先红后绿或变异验证；复查通过。
- **阻塞边**：T5.7。
- **并行**：构建槽独占；可与不同文件的修复并行改码。
- **防什么事故**：审计发现被「以后再说」吞掉；借审计扩大改动范围；修完不复验。

### T5.9 合并 / 安装 / 真机 UAT 交付

- **目标**：主会话把审计后的最后一批修复串行合并进 `dev`，在 `dev` 重建、打包、覆盖安装到用户桌面，交用户真机点验。
- **改动文件白名单**：无（git merge / 构建 / 打包命令）。
- **验收判据**：`dev` 上有 T5.8 修复的 merge commit；合并后重建 + 全量门禁通过；新包已放 `D:\Data\desktop` 并由用户确认安装；用户真机 UAT 通过。
- **阻塞边**：T5.8。
- **并行**：串行；一次只合一条。
- **防什么事故**：合并了没重建（用户跑旧代码）；修复合并不等于交付（0.4.3 事故）；分支合并顺序错导致 `index.lock` 冲突。

---

## 3. 交付前三问（写在 T5.9 报告里，不许省略）

1. **存量数据升级路径验过吗**：老库副本 → 新表创建 + 既有数据不变（T2.1/T5.5 证据）。
2. **真实外网 / 网关验过吗**：本期不接外部系统，无网关调用；明确写「不适用」而不是留空。
3. **目标平台真机验过吗**：probe 真窗口（Windows 11 本机）+ 用户真机 UAT；macOS 未验则写「未验证」。

---

## 4. 交付报告口径（对用户）

- 不报 Total/Passed 数字；报三行：
  1. 哪些业务场景实际跑起来了（真窗口 probe / 真库 / 真机）；
  2. 结果如何（通过 / 有问题）；
  3. 哪些没跑到（明写「未验证」）。
- 任务单完成 ≠ 交付完成：T5.9 的用户真机 UAT 通过才算完。

---

## 5. 完成定义（全部满足才算完成）

- 四闸全绿：token `--check`、控件映射无缺口、probe 几何逐条断言、并排目检差异清零（「接受」项有理由）。
- 42 张任务单全部有 commit sha 与处置结论；审计 findings 全部关闭。
- `dev` 上合并后的树通过 T5.6 全量门禁；新包已到用户桌面且真机 UAT 通过。
- 报告按「三行场景口径」写，不报测试总数；未跑场景明写「未验证」。