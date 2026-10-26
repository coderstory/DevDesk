# DevDesk 壁纸模块最小集成设计（P1）

> 日期：2026-10-26 · 状态：P1 契约基线 · 需求来源：`WALLPAPER-MODULE-DEV.md`（交接文档）
> 适用基线：DevDesk `6cc99867`。与交接文档冲突时以交接文档为准；本 spec 记录的差异见 §7。

## 0. 一句话与术语

P1 只做一件事：把「壁纸」变成 DevDesk 的一个页面模块，**默认关闭**；用户在「设置 → 模块」里打开后，侧边栏出现「壁纸」、页面显示空态；关掉后页面消失并回到首页。P1 不播放壁纸、不碰桌面窗口与播放器、不写 macOS 平台实现。

术语（首次出现给一句人话）：

- **Router（路由）**：DevDesk 内部决定「窗口当前显示哪个页面」的调度器。页面模块注册进来才可达。
- **模块门控**：按设置里的开关决定模块是否注册。不注册 = 页面、侧边栏入口、生命周期回调都不可达。
- **模块 ID**：开关与注册表之间的稳定字符串键（壁纸 = `"wallpaper"`）。发布后不得改名，否则用户的开关状态会被当成未知项清掉。
- **工厂函数**：`create_wallpaper_module()`，装配层用它创建模块对象。返回空指针 = 当前平台没有这个模块。
- **WorkerW**：Windows 桌面图标后面的一层窗口；P3 才用，本阶段不碰。
- **mpv**：第三方视频播放器，随包分发；P3/P6 才用，本阶段不碰。

## 1. 范围与已锁定决策

### 1.1 P1 范围（本 spec 的全部内容）

1. `PageId` 新增壁纸页，插在 Agent 与 Settings 之间；注册表新增一行。
2. 新增模块 ID `"wallpaper"`，加入合法 ID 清单，**不加入**默认启用清单。
3. 新增平台能力查询：Windows 支持；macOS 与其它 POSIX 不支持。
4. 设置 → 模块 新增独立「壁纸」卡与开关；不支持的平台整卡不建。
5. 开启/关闭复用现有即时生效链路；关闭时若正停在壁纸页，回首页并给现有提示。
6. 模块骨架：`compose` 渲染空页；`onStart/onStop` 幂等空实现。
7. Windows 定向测试、空页截图、macOS 只保证构建通过。

### 1.2 已锁定决策（只列与 P1 相关的）

| 决策 | 结论 | 对 P1 的含义 |
|---|---|---|
| 宿主 | 壁纸是 DevDesk 的页面模块，不做独立应用 | 直接接入现有 Router |
| 默认状态 | **默认关闭** | 默认清单不含 wallpaper；首次启动无页面、无入口 |
| 平台顺序 | Windows 先交付；macOS 第二阶段用 Objective-C++，本阶段不实现 | 非 Windows 工厂返回空、能力为 false |
| 页面形态 | 侧边栏「壁纸」，页面内 Tab：播放 / 片库 | Tab 与页面内容属 P5；P1 只到空页 |
| 启用方式 | 设置 → 模块 的开关 | P1 唯一启用入口 |
| 转码 / 维护 / 删除 | 不做 | P1 不引入相关文件与依赖 |
| UI 效果 | 不做玻璃 / 半透 | P1 不改窗口效果接口 |
| 托盘 | 壁纸组属 P5 | P1 不改托盘菜单 |

## 2. 集成契约（P1 级）

### 2.1 PageId 与页面名

- `src/core/module_interface.h` 的 `PageId` 增加 `Wallpaper`，**插在 `Agent` 与 `Settings` 之间**。
- `pageIdToString(PageId::Wallpaper)` 返回 `"Wallpaper"`。
- 枚举序 = 侧边栏顺序 = 注册表行序，是现有跨模块契约；重排会静默改变导航顺序。
- PageId 只在本进程内使用；P1 没有新增任何依赖 PageId 数值的持久化格式。

### 2.2 注册表行

`src/app_registry.h` 增加工厂声明与一行（行序 = 枚举序）：

```cpp
extern "C" ::core::Module* create_wallpaper_module();

{::core::PageId::Wallpaper, module::settings::kModuleIdWallpaper, "壁纸",
 "\xEF\x80\xBE", &create_wallpaper_module},   // U+F03E fa-image
```

- 图标是 FontAwesome `fa-image` 码点 `0xF03E` 的 UTF-8 字节，写法沿用现有表（十六进制转义；C++20 下 `u8""` 是 `char8_t[]`，不能隐式转成 `char*`）。
- 表里的 `shortName` / `iconGlyph` 与模块虚函数是同值双副本；一致性由装配测试锁死（现有用例已覆盖）。

### 2.3 模块 ID、合法清单与默认值

- `src/module_settings/settings_config.h` 增加 `inline constexpr const char* kModuleIdWallpaper = "wallpaper";`。
- `settings_config.cpp` 的 `kKnownModuleIds`（合法模块 ID 清单，sanitized 用它过滤未知项）加入 `kModuleIdWallpaper`；不加会导致用户打开开关后，下次启动被当作未知项清掉。
- `defaultEnabledModules()` **不加入** wallpaper，保证首次启动与升级后默认关闭。
- `moduleEnabled(cfg, kModuleIdWallpaper)` 默认为 false；home/settings 恒 true 的现有规则不变。
- 开关状态持久化在 DevDesk 现有 `enabledModules`（SQLite `app_config`），**不写**壁纸自己的 `settings.json`。

### 2.4 平台能力接口

新增 `src/platform/wallpaper_capability.h`：

```cpp
namespace platform::wallpaper {
bool supported();   // true = 当前平台有壁纸能力
}
```

- 头文件与平台无关；实现按平台拆文件，由 CMake 选择编译（与 `autostart`、`folder_picker` 现有模式一致）。
- Windows：返回 true。macOS：返回 false（P1）。其它 POSIX：返回 false。
- 业务代码不得写 `#ifdef _WIN32` / `__APPLE__`；一律调用 `supported()`。
- macOS 第二阶段（P8）只替换 macOS 实现并新增 `.mm` 平台层；P1 不动。

### 2.5 工厂与装配

- `create_wallpaper_module()`：`supported()` 为 false 时返回 `nullptr`；true 时返回模块对象。
- `src/main.cpp` 只改两处（**不动托盘段**）：
  - `getRouter()` 增加 `static std::unique_ptr<::core::Module> wallpaper(create_wallpaper_module());`
  - `moduleForPage` 增加 `case ::core::PageId::Wallpaper: return wallpaper.get();`
- 复用现有容错：装配层遇到空实例跳过注册；`dispatchModuleToggle` 遇到空实例记 WARN 并 no-op。P1 不新增兜底代码。
- 默认关闭时 `buildGatedEntries` 不含该行 → 不注册 → 页面、侧边栏入口、`onStart` 都不可达。工厂对象是否创建不影响默认行为。

### 2.6 设置 → 模块 的「壁纸」卡

- 位置：现有「模块」Tab，在模块开关卡（2×2 格）下方新增一张**独立全宽卡**；不改现有 2×2 格的数量、尺寸与卡高预算。
- 卡内容：标题「壁纸」、一行说明、右侧 44×24 开关（沿用现有模块开关几何）。
- 说明文案：P1 取任务书示例「动态桌面壁纸（视频 / 图片），开启后随 DevDesk 启动」；最终文案 P5 定稿。
- 显示条件：`platform::wallpaper::supported()` 为 true 才建卡；false 时**整卡不建**（不创建任何元素、不占高度）。
- 卡表登记时门控列必须为 `nullptr`：该卡是「模块关闭时也要能打开」的开关，不能被模块开关自身隐藏。平台可见性用独立的可见性函数指针表达（`nullptr` = 恒显），不复用模块门控列。
- 开关初值 = `moduleEnabled(cfg_, kModuleIdWallpaper)`（默认 false）。
- 点击调用现有 `applyModuleEnabled(moduleId, on)`：写 `enabledModules` → 持久化 → 经 `module_toggle_bridge` 通知 app 层。**不新增开关链路**。
- 不在「通用」Tab 重复出现；不改开机自启卡。

### 2.7 开关执行语义（复用现有即时生效）

- 打开：`Router::registerModule` → `onStart()`；侧边栏与页面在下一帧出现。
- 关闭：`onStop()` → `Router::unregisterModule`；若当前页正是壁纸页，则 `navigateTo(Home)` + 现有 `showAppNotice("模块已关闭", "<短名> 已关闭，已返回首页")`。
- 重复开关：设置层同值短路（不写盘）；Router 注册/注销幂等。P1 不新增去重代码。
- P1 的 `onStart/onStop` 是幂等空实现，必须支持 `onStart → onStop → onStart` 循环（现有活门控允许会话内反复开关）。

### 2.8 页面与引擎

- P1 的 `compose()` 只渲染空页：页面标题「壁纸」+ 说明「壁纸模块已启用，片库与播放将在后续版本接入」；正式空态与视觉 P5 定稿。
- P1 没有 WallpaperService、没有线程、没有 mpv、没有 WorkerW 宿主、没有托盘壁纸组。
- 验收里的「引擎未启」在 P1 用结构性判据：模块未注册（`Router::isRegistered(Wallpaper)==false`）= 页面与生命周期不可达。真实引擎从 P3 开始出现。

## 3. P1 验收（可实测）

| # | 场景 | 操作 | 预期（同时用于回归） | 防的事故 |
|---|---|---|---|---|
| 1 | 首次启动 / 升级后 | 不碰开关，启动 DevDesk | 侧边栏无「壁纸」；`Router::isRegistered(Wallpaper)==false`，壁纸页不可导航 | 默认被打开、默认清单误加 wallpaper |
| 2 | 设置页平台差异 | Windows 打开「设置 → 模块」 | 有独立「壁纸」卡，开关默认关；2×2 模块卡布局不变 | 开关初始值错、挤坏现有卡 |
| 3 | 非 Windows 设置页 | macOS 打开「设置 → 模块」 | 不建壁纸卡（找不到该卡任何元素） | 在不支持平台露出可点开关 |
| 4 | 打开开关 | 拨开壁纸开关 | 侧边栏出现「壁纸」；`isRegistered==true`；进入后渲染空页 | 注册/门控链路断、页面空白无标题 |
| 5 | 重复打开 | 再次拨到同一值 | 注册态不变、不重复写盘、页面不重复出现 | 幂等缺口 |
| 6 | 关闭开关且停在壁纸页 | 拨关 | 页面消失；自动回首页；出现「模块已关闭」提示；`isRegistered==false` 且导航不可达 | 注销后停在空页、提示缺失 |
| 7 | 关闭开关且不在壁纸页 | 先切到首页，再拨关 | 页面消失；不强制导航 | 误把用户拽回首页 |
| 8 | 重启保持意图 | 开关打开后重启 DevDesk | 壁纸页仍在侧栏，开关仍为开 | sanitized 把 wallpaper 当未知项清掉 |
| 9 | Windows 定向测试 | `bash scripts/build-test-run.sh --filter wallpaper`，再跑 `gating` / `assembly`（及受影响的 `sidebar`） | 定向用例全绿（门控 / 装配 / 模块契约 / 平台能力） | 只改代码不验行为 |
| 10 | macOS 构建 | CI macOS job | 构建通过；`supported()==false`；工厂返回空、不注册；不做界面 UAT | 非 Windows 编译断裂、误报 macOS 可用 |
| 11 | 空页截图 | 运行 `wallpaper_probe --screenshot D:/Data/desktop/wallpaper-p1-empty.png` | 截图文件存在且非全黑/非纯色；侧栏项与空页可见 | 只有断言、没有真实渲染 |

补充口径（DevDesk 规则）：

- 模块生命周期回调（`onStart/onStop`）的真实调用次数不作验收判据；验收看 Router 注册态与页面可达性，避免把空实现的日志/计数当契约。
- 不报 `Total / Passed` 数字当交付证明；按「哪些场景实际跑过 / 结果 / 哪些没跑到」三行报告。
- P1 涉及可见 UI，实现完成后必须真窗口跑 1、2、4、6 并截图；截图只作观感补刀，几何与行为以断言为准。
- 「macOS 仅保证编译」不等于 macOS 可用；交接文档要求 UAT 未过前不得宣称 macOS 交付。

## 4. 文件白名单（P1 实际改动）

路径均相对 DevDesk 仓库根，按基线 `6cc99867` 核对。**白名单外文件一律不动**；实现中发现必须新增，先停下回报，不自行扩范围。Task 1/2/3 与 `.superpowers/sdd/WALLPAPER-MODULE-DEV/task-{1,2,3}-brief.md` 的文件清单一一对应，并按独立审计裁决补齐 F1-F5 项。

### Task 1 — 平台能力 + 模块骨架 + 注册表接入（页面能被装配）

| 文件 | 动作 | 目的 |
|---|---|---|
| `src/platform/wallpaper_capability.h` | 新增 | 平台无关的能力查询声明 `platform::wallpaper::supported()` |
| `src/platform/wallpaper_capability_win.cpp` | 新增 | Windows 实现：恒 true |
| `src/platform/wallpaper_capability_macos.cpp` | 新增 | macOS 实现：恒 false（P1） |
| `src/platform/wallpaper_capability_posix.cpp` | 新增 | 其它 POSIX 实现：恒 false |
| `src/module_wallpaper/CMakeLists.txt` | 新增 | 新静态库 `devdesk_module_wallpaper`（P1 只含模块骨架；链接 core/platform/ui_kit，零兄弟模块依赖） |
| `src/module_wallpaper/wallpaper_module.h` | 新增 | 模块类声明（实现 `core::Module`） |
| `src/module_wallpaper/wallpaper_module.cpp` | 新增 | 工厂（不支持平台返回空）、页面名/短名/图标、空页 `compose`、幂等空 `onStart/onStop` |
| `tests/test_modules/test_wallpaper_module.cpp` | 新增 | 六个场景：枚举序、注册表行、默认关、门控条数、持久化、dispatch 开关；描述含 ASCII `wallpaper`，保证 `--filter wallpaper` 可定向跑到 |
| `src/core/module_interface.h` | 修改 | `PageId::Wallpaper`（Agent 与 Settings 之间）+ `pageIdToString` |
| `src/app_registry.h` | 修改 | 工厂声明 + 注册表一行（Agent 行与 Settings 行之间）+ 头注释「7 个模块工厂声明」改 8 |
| `src/main.cpp` | 修改 | 模块实例 + `moduleForPage` 分支（托盘段不动） |
| `src/module_settings/settings_config.h` | 修改 | `kModuleIdWallpaper` |
| `src/module_settings/settings_config.cpp` | 修改 | `kKnownModuleIds` 加入 wallpaper；`defaultEnabledModules()` 不加 |
| `src/platform/CMakeLists.txt` | 修改 | 三个平台分支各登记对应能力实现（附录 A 未列，P1 必改） |
| `CMakeLists.txt` | 修改 | `add_subdirectory`、主程序链接、include、覆盖率清单（四处同源维护） |
| `tests/CMakeLists.txt` | 修改 | 登记测试文件；测试目标链接 `devdesk_module_wallpaper`（Task 3 还会改 probe 段） |
| `tests/test_modules/test_app_assembly.cpp` | 修改 | 表形状 7→8 行、枚举序插入位、门控键索引、all-on 条数、仅 wallpaper 行允许空工厂 |
| `tests/test_modules/test_module_gating.cpp` | 修改 | 默认清单仍 5 项、wallpaper 默认关、sanitized 保留但不自动补 |
| `tests/test_modules/test_app_sidebar.cpp` | 修改（按需） | 若侧栏条目数/顺序断言因第 8 行必然变化，只改合法期望 |
| `tests/test_core/test_router.cpp` | 修改 | `core_pageid_order` 枚举序断言插入 Wallpaper（wallpaper=5、settings=6、about=7）并更新描述串 |
| `tests/probes/uat62_page_shots_probe.cpp` | 修改 | `kSlugs` 7→8（加 wallpaper）、注释 7 行→8 行；非 Windows 空工厂跳过注册与截图（审计裁决纳入 Task 1 冻结清单） |
| `CLAUDE.md` | 修改 | 模块口径由「7 个 Router 页面 / 4 个可插拔」改为「8 个 / 5 个」 |

### Task 2 — 设置 → 模块「壁纸」开关卡（用户能开关）

| 文件 | 动作 | 目的 |
|---|---|---|
| `src/module_settings/settings_ui_logic.h` | 修改 | 新增壁纸卡高常量（任务书建议 126–132，按现有 `roundFs` 派生；130% 字号不裁切） |
| `src/module_settings/settings_ui_cards.h` | 修改 | 声明 `drawWallpaperCard` |
| `src/module_settings/settings_ui_cards.cpp` | 修改 | 白卡 + 标题「壁纸」+ 说明 + 右侧 44×24 开关；元素根 `settings.card.wallpaper`；开关走 `applyModuleEnabled` |
| `src/module_settings/settings_module.cpp` | 修改 | `CardSpec` 增加平台可见性函数指针（现有行补 nullptr）；「模块」Tab 登记壁纸卡；不可见时跳过绘制、不占高度 |
| `tests/test_modules/test_wallpaper_module.cpp` | 修改 | 平台双分支：Windows 断言设置→模块卡与开关存在、开关默认 false；非 Windows 断言 `supported()==false`、工厂 nullptr、`ui.find("settings.card.wallpaper")==nullptr`。另加开关拨动后的持久化往返 |
| `tests/test_modules/test_settings.cpp` | 修改 | 把 `settings.card.wallpaper`（title/desc/sw.track）加入逐卡池，在 85%/100%/130% 三档断言行盒不重叠、行盒 ⊆ 卡 frame、派生钉值；若改写在新测试，必须给等价数值断言 |
| `tests/probes/module_toggle_sidebar_probe.cpp` | 修改（按需） | 仅当既有断言因新卡必然变化时更新，不改判据强度 |

### Task 3 — 真窗口 probe + 空页截图

| 文件 | 动作 | 目的 |
|---|---|---|
| `tests/probes/wallpaper_probe.cpp` | 新增 | 真窗口场景：默认关无侧栏项 / 打开后注册且空页渲染 / 导航「设置→模块」断言壁纸卡存在并截图 / 关闭后注销并回 Home；不启动真实引擎 |
| `tests/CMakeLists.txt` | 修改 | probes 段把 `wallpaper_probe` 加入 app 层特例名单、链接 `devdesk_module_wallpaper`、加模块 include 路径 |
| `tests/probes/module_toggle_sidebar_probe.cpp` | 修改（按需） | 若 Task 2 未改且此处断言因新模块必然变化，只改合法期望 |

### 4.1 明确不在 P1 的文件与能力

- **托盘**：`src/core/tray_menu_spec.h` 与 `src/main.cpp` 的托盘构建/分发段 → P5。
- **mpv 与打包**：`src/module_wallpaper/platform/**`、`scripts/package-windows.sh`、`installer/**`、`THIRD-PARTY-NOTICES` → P3/P6。
- **真实播放/桌面宿主**：WorkerW、视频/图片子窗口、图片解码、watchers、全屏检测、WallpaperService → P3/P4。
- **纯逻辑层**：`src/module_wallpaper/core/**`（轮换/让路/片库/几何/文案/设置）→ P2。
- **完整 UI**：播放页/片库页、折叠行组、空态三变体、页头来源开关 → P5。
- **macOS 平台实现**：任何 `.mm` 文件 → P8；P1 只有返回 false 的能力实现。
- **壁纸设置文件**：`wallpaper/settings.json` 的读写 → P2/P4。
- **EUI-NEO 改动**：P1 不需要。
- **安装包与许可**：P6。

## 5. 明确不做（跨阶段边界）

- 托盘壁纸组（P5）。
- mpv 打包与许可文件（P6）。
- 真实播放：mpv、宿主窗口、图片层、系统让路（P3/P4）。
- macOS 平台层与真机能力（P8）。
- 永久不做（交接文档 §1.3）：转码/降帧、维护/删除当前壁纸、多显示器壁纸、玻璃/半透效果、独立应用与安装器。

## 6. 后续里程碑（摘要，细节以交接文档 §10 为准）

| 阶段 | 内容 | 与本 spec 的关系 |
|---|---|---|
| P2 | 纯逻辑层 + 单测（轮换/让路/片库/几何/文案/设置） | 跨平台可跑，不碰 UI |
| P3 | Windows 平台引擎（WorkerW、mpv、图片层、watchers、全屏） | 真机探针 |
| P4 | WallpaperService 装配（扫描/轮换/让路/续播/live apply） | 模块 `onStart/onStop` 接入真实引擎 |
| P5 | UI 完成（播放/片库、折叠、空态、托盘菜单组） | 定稿本 spec 的待定项（卡高/空态视觉） |
| P6 | mpv 打包与许可 | 不改 git 提交 mpv |
| P7 | 真机 UAT（交接文档 §11.3 场景） | 用户验收 |
| P8 | macOS ObjC++ 平台层（代码先行、CI 编译；UAT 未过不报可用） | 本阶段不实现 |

## 7. 与交接文档的差异与待定项

差异（均不改产品决策，只补实施细节）：

1. **附录 A 未列 `src/platform/CMakeLists.txt`**：新增 `wallpaper_capability_*.cpp` 必须在该文件按平台登记，否则不参与编译。Task 1 任务书已补入，P1 必改。
2. **`tests/probes/uat62_page_shots_probe.cpp` 未见于附录 A 与 Task 1/3 任务书**：注册表增加一行会触发它的 `static_assert(kSlugs 行数 == 注册表行数)`。审计裁决将其纳入 Task 1 冻结清单（§4）：`kSlugs` 7→8、注释 7 行→8 行、非 Windows 空工厂跳过注册与截图。
3. **附录 A 把 `src/core/tray_menu_spec.h` 与 main.cpp 托盘段列为修改项**：那属于 P5 的托盘组；P1 明确不做，main.cpp 只加模块实例与 `moduleForPage` 分支。
4. **交接文档 §3.2 允许「隐藏卡片或显示仅 Windows」二选一**：P1 按任务书取「整卡不建」。
5. **交接文档 §10 P1 要求「probe 截图空页面」**：P1 按 Task 3 任务书新增 `tests/probes/wallpaper_probe.cpp`（附录 A 允许 `wallpaper_*_probe.cpp`），截图证明空页渲染。
6. **`tests/test_modules/test_module_icons.cpp`**：它逐项列出模块工厂，补壁纸图标只是增加覆盖；不加不影响 P1 验收，故不列入白名单。
7. **权威设计 spec `docs/superpowers/specs/2026-09-20-devdesk-design.md` 的 PageId 旧枚举本次不同步**：延后到 P5 文档阶段，负责人 = 主会话台账。
8. **`CLAUDE.md` 模块口径与 `src/app_registry.h` 头注释同步纳入 P1**：见 Task 1 白名单（7 页/4 可插拔 → 8 页/5 可插拔；7 个工厂 → 8 个）。

待定（不阻塞 P1，P5 定稿）：

- 壁纸卡高常量与内边距的最终数值（任务书建议 126–132，按现有 `roundFs` 几何派生）。
- 壁纸页空态的正式结构与视觉（P1 只需标题 + 说明可见）。

冲突裁决：以上与交接文档不一致时，以交接文档为准；涉及 P1 范围收窄的条目按本 spec 执行。