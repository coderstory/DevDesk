# R8 装配层收口：{PageId, 短名, 图标, 工厂, 门控键} 单表

> 状态：设计稿（主会话 review + 独立审计后再派开发）。只写 spec 不改代码。
> 前置：R1/R3 已合（ui_kit 基座 + metrics.h 就绪）。承接 PAB-02/PAB-03/PAB-04/THS-08（见 `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md` §0.3 row 7）。
> 已拍板：**D-R3**（2026-09-27 用户拍板，R spec §8.1）：`core::Module` 加 `shortName()`，带默认实现；列为不可回退项，**拆 2 commit** 落地（R spec §6 B 组）。
> 设计前置文档：`2026-09-27-god-class-refactor-design.md` §5.1（模块接入成本 PAB-02 目标态）；`2026-09-27-refactor-incremental-design.md` §4.2 尾行（5 列表驱动，main.cpp 687 → ≤550）。

## 1. 现状实证（2026-09-28 快照，证据 file:行号）

### 1.1 模块接入点分散 = 5 个文件 / 8+ 处（PAB-02）

| 位置 | 内容 |
|------|------|
| `src/main.cpp:42-46` | 5 个 `extern "C"` 工厂声明 |
| `src/main.cpp:142-146` | 5 个函数级 `static std::unique_ptr<Module>`（生命周期持有） |
| `src/main.cpp:154-158` | `ModuleSet{home.get(), markdown.get(), ...}` 结构字面量装配 |
| `src/app_assembly.h:39-45` | `ModuleSet` 定长 5 字段 —— **注册顺序 = 字段声明顺序，仅注释保证** |
| `src/app_assembly.cpp:74-90` | 三段复制粘贴式 `if/else` 门控（home/settings 恒注册分支 + 3 个 `moduleEnabled(cfg, kModuleIdX)`） |
| `src/module_settings/settings_config.h:64-69` | 门控键 `kModuleId*`（home/markdown/todo/claude_proxy/settings/pet） |
| `tests/test_modules/test_app_assembly.cpp:63,386,419,435` | `ModuleSet` 构造点 4 处（类型变更须同步改） |

新增一个页面模块要改 8+ 处，其中 `ModuleSet` 是公开头定长结构、门控是复制粘贴逻辑（god-class §5.1 已量化「9 处编辑 / 5 个文件」）。

### 1.2 Home 内另有一份硬编码清单（THS-08）

`src/module_home/home_module.cpp`：
- `:227-229` `claudeOn/mdOn/todoOn = moduleRegistered(page)` 三 bool；
- `:232-233` `statCount` 手写加权计数（claude 2 + md 1 + todo 2）；
- `:355-409` 统计卡 3 组 `if (xxxOn)` 重复判定；`:416-530` 最近编辑 `if (mdOn)`；`:534-535` `actCount` 再计数；`:547-594` 快捷按钮 3 组 `if (xxxOn)`。

门控判定在 3 区重复、计数靠手算——新增/关闭模块时宽度（statW/actW）漏改 = 卡片右缘错位（无编译期护栏）。home 不能 include app 层表（module_* 依赖方向不允许反向），THS-08 原案 = home 局地两张 static 表。

### 1.3 托盘映射：2 张平行表（PAB-03，口径已刷新）

- `src/app_assembly.cpp:173-182` `pageForTrayAction`（Action→PageId，**有测** `test_app_assembly.cpp:107/119`）—— 现无生产调用方（波 1 删 `trayNavAvailableFromConfig` 后 main.cpp 仍直接映射）；
- `src/main.cpp:345-367` `executeTrayAction` switch：`ShowMainWindow→requestShow`、4 个 Navigate\* → 硬编码 PageId + `navigateFromTray`、`Exit→requestExit`（**无测**）。→ 同一映射两处各写一遍（PAB-03 原「3 张表」经波 1 死代码清理后实测剩 2 张）。

### 1.4 短名双源（D8，D-R3 的根因）

| 来源 | 值 |
|------|-----|
| `app_assembly.cpp:184-193` `trayPageDisplayName`（首页/笔记/待办/Claude/设置，唯一消费点 `:207` 关闭模块提示，测试锁 `test_app_assembly.cpp:280-284`） | 短名 |
| 5 派生类 `displayName()`（home_module.cpp:163「首页」/ markdown_module.cpp:245「Markdown 笔记本」/ todo_module.cpp:43「Todo 看板」/ claude_module.cpp:50「Claude 切换器」/ settings_module.cpp:171「设置」） | 长名 |

同一页两个名字，靠注释「必然一致」担保。

### 1.5 启动顺序契约零护栏（PAB-04，专家 M2 未覆盖部分）

`main.cpp` 无测区：`:476-521` themeRestored（主题/字号/窗口效果接线）、`:527-528` initialWindow（尺寸决策）、`:601-639` initialized（argv 捕获 → DB → getRouter → **pet 开窗** `:622-636`）。顺序只由注释与语句位置保证，错了不编译失败、不挂测试（标题栏不跟随主题 / 日志丢行 / pet 早于 Router 就绪）。已可测部分（装配顺序/argv 捕获，`app_assembly` M2 已收）不回退。

### 1.6 R1/R3 可复用

- `src/ui_kit/metrics.h:35` `kNavItemH=40`（`app_sidebar.cpp:118` 已消费）、`:31` `kHeaderH=64`；
- `src/app_sidebar.h:19-20` `kSidebarWidthExpanded=208 / kSidebarWidthCollapsed=72`；
- 图标已模块自报：`Module::iconGlyph()`（module_interface.h:44）5 派生类已实现（0xF015 屋 / 0xF044 笔 / 0xF058 勾 / 0xF021 刷 / 0xF013 齿轮），`tests/test_modules/test_module_icons.cpp:58-99` 已锁码点 → **图标列可自报，一致性用例有先例**。

## 2. 方案

### 2.1 一张 {PageId, 短名, 图标, 工厂, 门控键} 表（app 层，header-only）

新型 `src/app_registry.h`（app 层公共头，零构建图变更）：

```cpp
namespace app::registry {
// 一行 = 一个 Router 页面模块。顺序 = 注册顺序 = 侧边栏顺序
//（= PageId 枚举序，契约 test_router core_pageid_order，表序须一致）。
// gatingKey == nullptr = 恒注册（home/settings；语义同 moduleEnabled 恒 true，
// 但显式表达不依赖谓词特殊逻辑）。
struct ModuleEntry {
    ::core::PageId page;
    const char* gatingKey;   // kModuleId* 或 nullptr（恒注册）
    const char* shortName;   // 显示短名（托盘/关闭提示）；Module::shortName() 同值
    const char* iconGlyph;   // FA7 Free-Solid 码点 UTF-8；Module::iconGlyph() 同值
    ::core::Module* (*factory)();  // extern "C" create_*_module
};
inline constexpr ModuleEntry kModuleRegistry[] = {
    {::core::PageId::Home,        nullptr,                          "首页",   "\xEF\x80\x95", &create_home_module},      // U+F015 fa-house
    {::core::PageId::Markdown,    module::settings::kModuleIdMarkdown,     "笔记",   "\xEF\x81\x84", &create_markdown_module},  // U+F044 fa-pen-to-square
    {::core::PageId::Todo,        module::settings::kModuleIdTodo,         "待办",   "\xEF\x81\x98", &create_todo_module},      // U+F058 fa-circle-check
    {::core::PageId::ClaudeProxy, module::settings::kModuleIdClaudeProxy,  "Claude", "\xEF\x80\xA1", &create_claude_module},     // U+F021 fa-arrows-rotate
    {::core::PageId::Settings,    nullptr,                          "设置",   "\xEF\x80\x93", &create_settings_module},    // U+F013 fa-gear
};
}
```

（C++20 注意：不用 `u8""` 字面量——`u8"中文"` 是 `char8_t[]` 不可隐式转 `const char*`；图标列用 UTF-8 hex 转义与 `iconGlyph()` 返回值字节相同，装配测试比对 `table.iconGlyph == m->iconGlyph()`。）

- 5 个 `extern "C"` 工厂声明自 `main.cpp:42-46` 迁入本头（全局作用域，与现状同）。
- 短名/图标两列与模块虚函数是**同值双副本**，装配期由一致性用例锁（§2.3、§4），不产生第三份。

**消费方**：
1. `main.cpp` `getRouter`（`:142-158`）：遍历 `kModuleRegistry` 创建 `static` 实例数组（函数级 static、先于 armExitHook 构造的 LIFO 约定保持），`ModuleSet{...}` 结构字面量 → `GatedEntry` 列表（见 2.2）传给 `assembleAndStart`；
2. `app_assembly::assembleAndStart`（`:74-90`）：三段 if/else → **一行遍历**（key==nullptr 或 moduleEnabled → registerOne），home/settings 恒注册由 key=nullptr 表达；
3. `app_assembly::trayPageDisplayName`（`:184-193`）：body 改查 `kModuleRegistry`（关闭模块提示 `:207` 仍可用——表是静态元数据，不需要活实例），**返回值与现值逐字相同**；
4. `executeTrayAction`（PAB-03）：Navigate\* 分支删硬编码映射，改 `if (auto p = app::assembly::pageForTrayAction(action)) { app::assembly::navigateFromTray(getRouter(), *p); }`；只留 `ShowMainWindow`/`Exit` 两特例 → `pageForTrayAction` 恢复为唯一映射（有测）。

### 2.2 assembleAndStart 签名：ModuleSet → 一行表遍历

```cpp
// app_assembly.h（替 ModuleSet :39-45）
struct GatedEntry {
    const char* gatingKey;  // nullptr = 恒注册
    ::core::Module* module; // nullptr = 容错跳过（registerModule 同款）
};
std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const std::vector<GatedEntry>& modules, const PreStartHook& preStart);
```

- 注册顺序 = 传入列表顺序（消除「字段顺序即注册顺序」隐式不变量）；返回 registered 顺序保持（既有顺序断言用例继续有效）。
- app_assembly 保持可无头单测（M2 成果），测试用桩模块 + 自制 GatedEntry 列表。

### 2.3 Module::shortName()（D-R3 已拍板，不可回退项）

- `module_interface.h` 新增：
  ```cpp
  // 显示短名（托盘/关闭模块提示等「空间受限」场景）。默认回落 displayName()，
  // 现有模块各自显式 override。D-R3（2026-09-27 拍板）。
  virtual std::string shortName() const { return displayName(); }
  ```
- 5 页面派生类显式 override（= trayPageDisplayName 现值，短名从此单源）：
  Home→`"首页"`、Markdown→`"笔记"`、Todo→`"待办"`、ClaudeProxy→`"Claude"`、Settings→`"设置"`（home_module.cpp:162-164 / markdown_module.cpp:244-246 / todo_module.cpp:42-44 / claude_module.cpp:49-51 / settings_module.cpp:170-172 三连 override 处各加一行）。
- 一致性用例（仿 `test_module_icons.cpp` 先例）：`create_*_module()` 构造真实模块（纯 getter，无 DB）：断言 `表.shortName == m->shortName()`、`表.iconGlyph == m->iconGlyph()`、`表.page == m->pageId()`——表与虚函数双副本被测试绑死。

### 2.4 THS-08：Home 局地卡片/按钮表（不跨层）

`home_module.cpp` 内两张 static 表 + 单一门控判定：
- 统计卡表 `{PageId, 标题, 副标题取数 lambda, 点击负载}`（现 :355-409 的 5 卡，claude 2 卡 = 2 行）；`statCount = 表行数（按 moduleRegistered 过滤）`；
- 快捷按钮表 `{PageId, 文案, 图标, 点击负载}`（现 :547-594 的 3 钮）；`actCount` 同上派生；
- `claudeOn/mdOn/todoOn` 三 bool 收成一次 `moduleRegistered(page)` 判定（保留 `:153 moduleRegistered` 助手，语义不变）。
- 不 include app 层头：home 是 module 层，`/app_registry.h` 在 app 层之上。**宽度现算公式（statW/actW）保持不变**，只换「按表行数派生」。

### 2.5 PAB-04 收窄版：pet 开窗进可测入口

- `main.cpp:622-636` pet 门控开窗抽为 `app::assembly::maybeStartPet(cfg, startFn)`（`startFn = std::function<void()>` 生产传 `module::pet::startPetWindow` 闭包；app_assembly.cpp include `module_pet/pet_window.h` 与 main.cpp 同级依赖，不分层破坏）；门控谓词 `isModuleEnabled(kModuleIdPet)` 不变。
- 新增 1 条无头用例：pet 开启/关闭 → 回调是否被调（顺序断言「装配后」由测试调用顺序显式锁）。
- **不做**：themeRestored/initialWindow 两段 static 仍留 main.cpp（EUI 生命周期深度耦合，抽 `app_boot` 另议——PAB-04 的核心缺口是 pet 时序与 argv/DB 顺序，前者本条收口，后者已有测试缝）。

### 2.6 明确不做

- `pageIdToString`（module_interface.h:22-31）保留：core 在用（router.cpp:29/39-40）、作元素 ID 序列化（app_sidebar.cpp:178、home_module.cpp:611），是 ID 非显示名，不并入短名表（专家 L3 的「由表派生」仅适用于显示语义 switch）。
- 侧边栏摄入逻辑不动（app_sidebar.cpp:172 遍历 `router->modules()` 拿 pageId/displayName/iconGlyph——表序 = 注册序 = modules() 序，行为零变化）。
- pet 不进表（非 Router 页面模块，`main.cpp:622-636` 语义保持现状）。
- `moduleEnabled` 谓词（settings_config.h:144）与 `kModuleId*` 常量不动。

## 2.x 待拍板（每条含推荐；未拍板前开发不落地该项默认值之外的结构）

| # | 问题 | 选项 | 推荐 |
|---|------|------|------|
| T1 | `shortName()` 默认实现 vs 纯虚 | A 带默认实现回落 displayName()（D-R3 原案）／ B 纯虚 | **A（D-R3 已定）**；风险点：漏 override 的模块在关闭提示会显示长名——§4 验收「grep shortName = 7 命中」兜底 |
| T2 | 表放哪 | A 新 `src/app_registry.h`（header-only）／ B `main.cpp` 匿名命名空间（app_assembly 查不到）／ C `app_assembly.h`（app_assembly 与表同居，main 仍持有工厂） | **A**：三个消费方（main 工厂 / assembly 门控 / tray 短名）都能见；零构建图变更。B 否决（trayPageDisplayName 在别的 TU）；C 可使（表与装配同居），但 app_assembly 本已写信——A 最符合单一写者 |
| T3 | 门控键口径 | A `gatingKey=nullptr` = 恒注册（PAB-02 原案「恒启用项留空」）／ B 恒注册也填 kModuleId*（依赖 moduleEnabled 对 home/settings 恒 true） | **A**：显式、不依赖谓词特殊逻辑；遍历统一 `key==nullptr \|\| moduleEnabled(cfg,key)`；`kModuleIdPet` 不出现（pet 非页面模块） |
| T4 | 表内图标列 | A 存静态串 + 一致性用例锁（5 列齐全，R spec §4.2 原文）／ B 不存列，侧边栏继续经虚函数取 | **A**：登记表自文档化；与虚函数一致性由 §2.3 用例锁（no third copy）。B 让表缺一列、文档价值减半 |
| T5 | 表 短名/图标 与虚函数双副本可接受？ | A 接受 + 一致性用例绑死（§2.3）／ B 表不存短名，关闭提示改经「保存的实例快照」（无实例时无法查 → 否决） | **A**：tray 关闭态必须无实例可查（app_assembly.cpp:207），静态列不可省；一致性用例是双副本唯一缝合点 |
| T6 | PAB-04 范围 | A 收窄版（本 spec §2.5：pet 进可测入口 + 1 用例）／ B 全量 `app_boot.{h,cpp}` 抽三段 static | **A**：themeRestored/initialWindow 与 EUI 生命周期深度耦合，硬抽收益低风险高；B 留待后续线（本 R8 不含） |

## 3. 改动清单

| 文件 | 改动（commit） | 行为 |
|------|----------------|------|
| `src/core/module_interface.h` | 加 `virtual std::string shortName() const`（默认回落 displayName），注释引 D-R3（**C1**） | 接口新增，零既有编译影响（有默认实现） |
| 5 派生类：`home_module.cpp:162-164` 等 5 处三连 override 区 | 各加 `shortName() override`（首页/笔记/待办/Claude/设置）（**C1**） | 短名单源化，现值 = trayPageDisplayName |
| `src/app_registry.h`（新，app 层 header-only） | `ModuleEntry` + `kModuleRegistry[5]` + extern"C" 工厂声明（自 main.cpp:42-46 迁入）（**C2**） | 表 = 装配元数据唯一登记处 |
| `src/main.cpp` | 删 `:42-46` extern 声明（改 include app_registry.h）；`getRouter` `:142-158` 改按表遍历创建 static 实例 + 传 GatedEntry 列表；`executeTrayAction` `:345-367` Navigate\* 改调 `pageForTrayAction` + 两特例；pet 开窗 `:622-636` 改调 `app::assembly::maybeStartPet`（**C2**） | main.cpp 647 → ≈540（R spec 目标 ≤550）；托盘映射单点 |
| `src/app_assembly.h` | `ModuleSet`（:39-45）→ `GatedEntry`；`assembleAndStart` 签名改 `const std::vector<GatedEntry>&`；新增 `maybeStartPet` 声明；`trayPageDisplayName` 注释更新（**C2**） | 公开结构表驱动 |
| `src/app_assembly.cpp` | `assembleAndStart`（:74-90）→ 一行遍历；`trayPageDisplayName`（:184-193）→ 查 `kModuleRegistry`；新增 `maybeStartPet` 实现（include `module_pet/pet_window.h` + `settings_config.h` 已有）（**C2**） | 门控逻辑单点；短名单源 |
| `src/module_home/home_module.cpp` | THS-08 收口：统计卡/快捷按钮两张 static 表 + `statCount/actCount` 从表长派生 + 单门控判定（**C2**） | 门控判定一处；宽度派生不落字面量 |
| `tests/test_modules/test_app_assembly.cpp` | ModuleSet 4 处（:63/:386/:419/:435）→ GatedEntry 构造；新增：表一致性用例（5 行 × shortName/iconGlyph/page vs 真实模块 getter，仿 test_module_icons）、恒注册用例（key=nullptr 全关配置下 home/settings 仍注册，既有 :165 语义）、短名表派生断言（保持 :280-284 逐字值）；新增 maybeStartPet 开/关用例（**C2**） | 装配层护栏不降级 |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | **无**（header-only 表；tests 已 include src/） | — |

## 4. 验收

1. **行数**：`main.cpp` 647 → ≤550（`wc -l src/main.cpp`）。
2. **PAB-02**：`grep -rn "ModuleSet" src/` 零命中；`app_assembly.cpp` 内 `moduleEnabled` 恰 1 处（一行遍历内）。
3. **PAB-03**：`main.cpp` 内无 TrayMenuAction→PageId 直接映射 switch（executeTrayAction 只剩 2 特例 + 委托）；`pageForTrayAction` 有生产调用方（`grep -rn "pageForTrayAction" src/main.cpp` 命中）。
4. **THS-08**：`home_module.cpp` 内 `claudeOn/mdOn/todoOn` 布尔组 ≤1 处；`statCount/actCount` 由表长度派生。
5. **PAB-04**：`maybeStartPet` 无头用例通过（回调开/关两态）。
6. **shortName()**：`grep -rn "shortName" src/` = 7 命中（接口 1 + 5 页面派生类 override + app_registry 表头注释 1）；一致性用例绿（pet 非 `Module` 派生类，不参与）。
7. **零回归**：定向 `./build/tests/devdesk_tests.exe app_assembly`（含 :280-284 短名断言原值不变）+ `module_gating` + `router`（core_pageid_order）+ `home` + `sidebar`；里程碑收尾全量 `Total: 646 / Passed: 645 / Failed: 0`（以运行输出为准）；**肉眼**：侧边栏 5 项 + 折叠 tooltip + 托盘菜单 + 关闭模块后托盘点导航的提示文案逐字不变。
8. 每次合并后真编译一次（merge-tree 无冲突 ≠ 能编译）。

## 5. 风险

| # | 风险 | 验证 / 处置 |
|---|------|-------------|
| 1 | **shortName() 不可回退**（R spec §6）：合入即成为接口一部分，回退须同删接口 + 全部 override | 拆 2 commit（C1 接口+实现，C2 消费）：C1 单独闭环（接口加法行为零变化、全量编译过）后再 C2；C2 失败仅 revert C2 |
| 2 | 门控键口径写反（key=nullptr 判错）→ home/settings 变可关 | 恒注册语义用例（all-off 配置下 home/settings 仍注册，语义同 `test_app_assembly.cpp:165`）；review 时对遍历单行逻辑逐字校验 |
| 3 | 表短名/图标与虚函数双副本 drift（新增模块漏 override） | §2.3 一致性用例（真实模块 getter 对比）在装配测试里锁死；§4 验收 grep 守 5 处 override |
| 4 | `trayPageDisplayName` 查表后文案漂移 | 测试 `:280-284` 断言逐字保持；实现为纯查表（无手写 switch） |
| 5 | Home 卡片表收口改坏宽度 | 宽度现算公式（statW/actW）逐字保持，只换计数来源；`test_home` 全量过 + 肉眼首页卡片不右缘错位 |
| 6 | pet 收口改坏开窗时序 | maybeStartPet 门控/注入回调与现状逐字等价（谓词同一份 `moduleEnabled`）；无头用例开/关两态 |
| 7 | `assembleAndStart` 签名变更波及外部（无外部调用方，仅 main.cpp + 测试） | 编译期全量暴露；main.cpp 与测试同 commit 改造 |
| 8 | 表序与 PageId 枚举序漂移（重排 = 侧边栏序变） | 表序 = 枚举序由既有 `core_pageid_order` 契约 + 新表序断言（表第 i 行 .page == 枚举第 i 个）锁定 |

## 6. 流程

**2 commit（C1 → C2，串行）**：
- **C1 = 加虚函数 + 5 派生类实现**（module_interface.h + 5 页面派生类 + 一致性用例基础设施；pet 非 `Module` 派生类不参与）：接口加法，行为零变化，全量编译 + 定向测试独立闭环；
- **C2 = 合表消费**（app_registry.h 新文件 + main.cpp + app_assembly.* + home_module.cpp + 测试改造）：表落地，PAB-02/03/04/THS-08 收口。

派单约束：worktree 隔离开发；定向测试（`test_app_assembly` + `test_module_gating` + `test_router` + `test_home` + `test_app_sidebar` + `test_module_icons`）；禁全量重建/全量套件；每 commit 显式 pathspec；合并 master 后主会话真编译一次；肉眼验收项（§4-7）在合并后由用户/主会话执行。