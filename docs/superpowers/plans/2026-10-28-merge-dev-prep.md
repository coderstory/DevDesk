# 2026-10-28 origin/dev → wt-release-core 合并预案

> 只读分析产物。dry-run 手段：`git merge-tree --write-tree HEAD origin/dev`（git 2.54，无写操作）。
> 结果树 `74a968a6`（内含冲突块文本），退出码 1 = 有冲突。

## 0. 基线

| 项 | 值 |
|---|---|
| ours | `wt-release-core` @ `8c6b27b2`（距 base 8 个提交：上线三项、重排枚举、折叠树、全局 ticker） |
| theirs | `origin/dev` @ `f42ea2c4`（距 base 120 个提交：MCP 模块、壁纸托盘、mpv 打包、壁纸开关收格等） |
| merge-base | `25e31b1d` |
| 文本冲突 | **15 个文件**（清单见 §1） |
| 双侧重叠文件 | 21 个；其中 6 个自动合并成功（含 main.cpp、schema.h、ticker×3 全等 blob） |

用户拍板的合并后侧边栏顺序：**首页 → 笔记 → 待办 → Claude → 智能体 → 壁纸 → MCP → 上线(父项) → 设置 → 关于**。

---

## 1. 冲突文件清单（15）

| # | 文件 | 冲突块 | 解法方向 |
|---|---|---|---|
| 1 | `CMakeLists.txt` | 3 | 三处均「双方都加」：add_subdirectory / 覆盖率库表 / 链接库表同时含 release + mcp |
| 2 | `src/core/CMakeLists.txt` | 1 | 源文件表同时保留 `ticker.cpp`（ours）与 `mcp_tool.cpp`（theirs） |
| 3 | `src/core/module_interface.h` | 2 | 枚举与 pageIdToString 均插入 Mcp（Wallpaper 之后）+ Release 三行（Mcp 之后），见 §3 |
| 4 | `src/app_registry.h` | 2 | 保留 ours 的 `#include <optional>`+parent 字段；工厂清单两边全保留；表序按 §3 |
| 5 | `src/app_assembly.cpp` | 1 | dispatchModuleToggle 两段插入**都要**：ours 的 matched/missingPages 声明 + theirs 的 MCP 服务启停块；循环体取 ours（多行遍历） |
| 6 | `src/module_settings/settings_config.cpp` | 2 | kKnownModuleIds 同时含 release+mcp；defaultEnabledModules 注释合并（return 行自动合并已含 release） |
| 7 | `src/module_settings/settings_config.h` | 2 | kModuleIdRelease 与 kModuleIdMcp 两条声明都留；save() 键数注释改为 19 键 |
| 8 | `src/module_settings/settings_ui_cards.cpp` | 2 | 标题带 MCP 开关取 theirs；网格循环**头取 theirs（cellCount/isWallpaper）**、kToggleable 取 ours（5 格含 release）；drawWallpaperCard 整函数按 theirs 删除（自动合并已删） |
| 9 | `src/module_settings/settings_ui_logic.h` | 1 | 取 theirs 两个常量名，但值都改 254（release 恒 3 行）；kWallpaperCardHeight 彻底删除 |
| 10 | `tests/CMakeLists.txt` | 3 | 三处「双方都加」：测试源表（release 5 文件 + mcp 2 文件 + mcp_service_bridge.cpp）、链接库、probe 特例注释 |
| 11 | `tests/probes/uat62_page_shots_probe.cpp` | 1 | kSlugs 12 项，Mcp 插在 wallpaper 与 release_group 之间 |
| 12 | `tests/probes/wallpaper_probe.cpp` | 1 | PageId switch 同时加 Mcp case 与 Release* 3 case（均 break 不接模块） |
| 13 | `tests/test_modules/test_app_assembly.cpp` | 8 | allOnConfig 加 mcp 键；两处 kEnumOrder/尺寸改 12 行；gating 下标全表重钉，见 §4-B |
| 14 | `tests/test_modules/test_settings.cpp` | 1 | 模块卡高 pin 取 ours（254 档，去 supported 条件），注释改为「release 恒 3 行」 |
| 15 | `tests/test_modules/test_wallpaper_module.cpp` | 1 | kEnumOrder/尺寸改 12 行（Mcp 在 Wallpaper 与 ReleaseGroup 之间） |

---

## 2. 逐文件预案

### 2.1 顶层 `CMakeLists.txt`（3 块，全同模式）
- **两侧**：ours 加 `add_subdirectory(src/module_release)` + 覆盖率库表 `devdesk_module_release` + DevDesk 链接库 `devdesk_module_release`；theirs 同三处加 mcp 版。
- **合并目标**：三处各同时出现 release 与 mcp。
- **解法**：每块取 ours 侧为基，把 theirs 侧对应行并入（`add_subdirectory(src/module_mcp)` 紧跟 release 行；`devdesk_module_mcp` 紧跟 release 行）。

### 2.2 `src/core/CMakeLists.txt`（1 块）
- **两侧**：ours 在 `atomic_write.cpp` 后插 `ticker.cpp`；theirs 同位置插 `mcp_tool.cpp`。
- **合并目标**：源表同时含两文件。
- **解法**：两行都保留（ticker.cpp 一行 + mcp_tool.cpp 一行，注释各留）。

### 2.3 `src/core/module_interface.h`（2 块）
- **两侧**：ours 在 Wallpaper 与 Settings 之间插 ReleaseGroup/ReleaseTasks/ReleaseConfig（枚举 + pageIdToString 两处）；theirs 同位置插 Mcp。
- **合并目标**：`... Wallpaper, Mcp, ReleaseGroup, ReleaseTasks, ReleaseConfig, Settings, About`（用户定序：MCP 在上线上面）。
- **解法**：以 ours 侧为基，把 theirs 的 `Mcp,` 行插到 `Wallpaper,` 之后、`ReleaseGroup,` 之前；pageIdToString 同序加 `case PageId::Mcp: return "Mcp";`。

### 2.4 `src/app_registry.h`（2 块）
- **两侧**：ours 加 `#include <optional>`、ModuleEntry 加 `parent` 字段、3 行 release 注册行、3 个 release 工厂声明；theirs 加 `create_mcp_page_module` 声明 + 1 行 Mcp 注册行。
- **合并目标**：工厂声明 12 个全在（自动合并已含全部声明，冲突只在 include 注释区与注册表行）；注册表 12 行按 §3 表序；ModuleEntry 保留 parent 字段。
- **解法**：冲突块 1 取 ours（include + 注释）；冲突块 2 以 ours 三行为基，前面插 theirs 的 Mcp 行（无 parent 字段）。

### 2.5 `src/app_assembly.cpp`（1 块，最关键）
- **两侧**：ours 把 dispatchModuleToggle 改成多行遍历（`matched`/`missingPages` 聚合、isGroupPage 跳过、单键多页逐行拨）+ 循环前声明 `bool matched; std::string missingPages;`；theirs 在循环前插 MCP 特例块（先启停服务、不提前 return、落进下方遍历做页面注册/注销）。
- **合并目标**：MCP 特例块在前，ours 的声明与多行循环在后；冲突块之后共同尾部（`matched = true;` 起）引用两个声明——**漏 ours 侧直接编译失败**。
- **解法**：手拼 = theirs 的 MCP 特例块（原样）+ ours 的注释与两个声明（原样）+ 共同尾部。语义顺序：关 MCP 时先 stop 服务再注销页面（特例块在循环前，与 theirs 语义一致）。循环体用 ours 多行版：MCP 行按普通行走 register/unregister，release 三行同键逐行拨、父项行 isGroupPage 跳过。

### 2.6 `src/module_settings/settings_config.cpp`（2 块）
- **两侧**：ours 在 kKnownModuleIds 加 `kModuleIdRelease`、defaultEnabledModules 注释与 return 加 release、加 moduleDefaultsVersion 键+迁移表（1 版 release 默认启用）；theirs 加 `kModuleIdMcp`、mcpPort 键读写（load/save 自动合并均已含，verified）。
- **合并目标**：kKnownModuleIds 同时含 release 与 mcp（**sanitized 过滤未知名单——漏 release 会导致存量配置里的 release 被静默剔除、上线组消失**）；默认清单 6 项（return 行自动合并已含 release，无需手改）；mcpPort 与 moduleDefaultsVersion 两键并存（自动合并已达成，verified 472/542 行）。
- **解法**：冲突块 1 双方并（顺序按枚举：wallpaper, mcp, release）；冲突块 2 仅注释合并（release 默认启用 + mcp 默认关闭都写）。

### 2.7 `src/module_settings/settings_config.h`（2 块）
- **两侧**：ours 声明 `kModuleIdRelease`；theirs 声明 `kModuleIdMcp`。
- **合并目标**：两条声明都保留；save() 键数注释由「18 个」改为「19 个」（mcpPort + moduleDefaultsVersion 并存）。
- **解法**：冲突块 1 双方并；冲突块 2 合并注释改口径。`kModuleDefaultsVersion` 保持 1（theirs 未动，自动合并即 ours 版）。

### 2.8 `src/module_settings/settings_ui_cards.cpp`（2 块）
- **两侧**：ours 把 release 加进 kToggleable（第 5 格）+ 循环头 `for i < kToggleCount`（无壁纸逻辑）+ 注释；theirs 加标题带 MCP 服务开关、壁纸收进网格（kWallpaperToggle + cellCount + isWallpaper 循环头）、删除 drawWallpaperCard 整函数。
- **合并目标**：kToggleable = 5 项（含 release，自动合并已达成）；循环头**必须取 theirs 的 cellCount/isWallpaper 版**——冲突块之后的共同尾部（cellId/suffix/text/sw 全部 isWallpaper 条件式）是 theirs 写的，取 ours 循环头则 `isWallpaper` 未声明、编译失败；壁纸格成为第 6 格（release 占第 5 格），行仍是第 3 行（i/2=2，几何不变）。
- **解法**：冲突块 1 取 theirs（MCP 开关块原样保留）；冲突块 2 取 theirs 循环头；kToggleable 表与 kWallpaperToggle 表自动合并已都对，仅注释把「壁纸第 5 格」改「第 6 格」。drawWallpaperCard 已按 theirs 自动删除，不复活。

### 2.9 `src/module_settings/settings_ui_logic.h`（1 块）
- **两侧**：ours `kModulesCardHeight=254`（release 3 行）+ `kWallpaperCardHeight=128`（独立壁纸卡）；theirs `kModulesCardHeight=198` + `kModulesCardHeightWallpaper=254`（壁纸第 5 格），kWallpaperCardHeight 已删。
- **合并目标**：保留 theirs 的两个符号（`settings_module.cpp` 与 `test_wallpaper_module.cpp:397` 引用 `kModulesCardHeightWallpaper`），但 release 使网格恒 5 格 → 恒 3 行 → **两值都设 254**（198 档已不存在，不支持平台也 3 行）；kWallpaperCardHeight 删除（合并树中已无任何消费方）。
- **解法**：取 theirs 侧两行，值都改 254，注释改为「release 加入后恒 3 行：68+roundFs(56)×2+roundFs(36)=216，+38 → 254」。

### 2.10 `tests/CMakeLists.txt`（3 块）
- **两侧**：ours 加 5 个 release 测试源 + 链接 `devdesk_module_release` + release_probe 特例；theirs 加 2 个 mcp 测试源 + `mcp_service_bridge.cpp` 源 + 链接 `devdesk_module_mcp` + mcp_page_module.cpp 特例说明。
- **合并目标**：双方条目全保留；TIN-10 自检靠磁盘 GLOB，release 测试文件随合并进树即通过。
- **解法**：三块均双方并（块 3 是注释，两段注释都留）。

### 2.11 `tests/probes/uat62_page_shots_probe.cpp`（1 块）
- **两侧**：ours kSlugs 11 项（含 release 三 slug）；theirs 9 项（含 mcp）。
- **合并目标**：12 项 = `"home","markdown","todo","claude_proxy","agent","wallpaper","mcp","release_group","release_tasks","release_config","settings","about"`。static_assert 与注册表行数自动对齐；探针按工厂 nullptr 跳过 ReleaseGroup（slug 落 skipped 名单），Mcp 正常截图（07-mcp.png）。
- **解法**：以 ours 为基插 mcp。

### 2.12 `tests/probes/wallpaper_probe.cpp`（1 块）
- **两侧**：ours 在 §1 switch 加 Release* 3 case；theirs 加 Mcp case。
- **合并目标**：switch 含全部 4 个新 case（Mcp 与 Release* 均 `break` 不接模块）。§1 用默认 cfg——合并后默认清单含 release，gated 列表会多出 release 三行（module=nullptr，assembleAndStart 按 ours 逻辑跳过）；`defaultGatingHasWallpaper` 语义不变。其余断言无表尺寸/下标钉值，不动。
- **解法**：双方 case 并排。

### 2.13 `tests/test_modules/test_app_assembly.cpp`（8 块）——细节见 §4-B
### 2.14 `tests/test_modules/test_settings.cpp`（1 块）——细节见 §4-I
### 2.15 `tests/test_modules/test_wallpaper_module.cpp`（1 块）——细节见 §4-C

---

## 3. 枚举序 / 注册表 / switch 定稿

### 3.1 `PageId`（12 项，下标即枚举值）

```
0 Home, 1 Markdown, 2 Todo, 3 ClaudeProxy, 4 Agent, 5 Wallpaper,
6 Mcp, 7 ReleaseGroup, 8 ReleaseTasks, 9 ReleaseConfig, 10 Settings, 11 About
```

### 3.2 `kModuleRegistry`（12 行，行序 = 上表）

| i | page | gatingKey | 短名 | 图标 | 工厂 | parent |
|---|---|---|---|---|---|---|
| 0 | Home | nullptr | 首页 | U+F015 | create_home_module | — |
| 1 | Markdown | markdown | 笔记 | U+F044 | create_markdown_module | — |
| 2 | Todo | todo | 待办 | U+F058 | create_todo_module | — |
| 3 | ClaudeProxy | claude_proxy | Claude | U+F021 | create_claude_module | — |
| 4 | Agent | agent | 智能体 | U+F544 | create_agent_module | — |
| 5 | Wallpaper | wallpaper | 壁纸 | U+F03E | create_wallpaper_module | — |
| 6 | Mcp | mcp | MCP | U+F1E6 | create_mcp_page_module | — |
| 7 | ReleaseGroup | release | 上线 | U+F135 | create_release_group_module | — |
| 8 | ReleaseTasks | release | 任务 | U+F0AE | create_release_tasks_module | ReleaseGroup |
| 9 | ReleaseConfig | release | 基础配置 | U+F013 | create_release_config_module | ReleaseGroup |
| 10 | Settings | nullptr | 设置 | U+F013 | create_settings_module | — |
| 11 | About | nullptr | 关于 | U+F05A | create_about_module | — |

### 3.3 `moduleForPage`（main.cpp，自动合并已验证含全部 12 case，无需手改）
顺序无关（switch），核对点：`Mcp → mcpPage.get()`、`ReleaseGroup/Tasks/Config → 对应实例` 与既有 9 case 并存（merged main.cpp:301-312 已确认）。

### 3.4 `pageIdToString` 12 case（同枚举序，含 Mcp 与 Release*）。

---

## 4. 测试断言合并口径

### A. `tests/test_core/test_router.cpp`（**无文本冲突，但必须手改**，见 §5.7）
- `core_pageid_order`（合并树取 ours 全序数组版）：kExpectedOrder 改 12 项 = §3.1 全序；`kExpectedCount == 12`；测试名串同步加 Mcp。
- `core_pageid_existing_eight_order_shifted`（ours 版）：语义升级为「既有 9 项」（Mcp 加入既有项），过滤 release 三行后剩 9 项逐位比对；枚举钉值：前 6 项 0..5 不变、`Mcp==6`、`Settings==10`、`About==11`。

### B. `tests/test_modules/test_app_assembly.cpp`（8 块冲突）
- `allOnConfig()`：enabledModules 9 键 = home, markdown, todo, claude_proxy, settings, agent, wallpaper, **mcp, release**（两冲突块双方并）。
- `assembly_registry_table_shape`：**12 行**；kEnumOrder = §3.1 全序；gating 钉值：`[0]=nullptr`、`[1]=markdown`、`[2]=todo`、`[3]=claude_proxy`、`[4]=agent`、`[5]=wallpaper`、`[6]=mcp`、`[7]=[8]=[9]=release`、`[10]=[11]=nullptr`。测试名 11 行 → 12 行。
- `assembly_build_gated_entries_real_table`：all-off 仍 3 条（Home/Settings/About）；all-on **12 条**按表序（ReleaseGroup 行在内、module==nullptr）；kEnumOrder 同 §3.1。
- 其余用例（stub 清单、preStart 钩子等）无表尺寸依赖，不动。

### C. `tests/test_modules/test_wallpaper_module.cpp`（1 块冲突）
- `wallpaper_registry_row`：kEnumOrder = §3.1 全序 12 项；`kRegistrySize == 12`。
- `wallpaper_enabled_gating`（theirs 新增，自动合并、**不改仍绿**）：其 allOn 手搓清单无 release 键 → release 三行被门控滤除，gated 仍恰 9 条且 `[4]=Agent,[5]=Wallpaper,[6]=Mcp,[7]=Settings` 全部成立。
- `wallpaper_pageid_order`：Agent < Wallpaper < Settings 在合并枚举下仍成立，不动。

### D. `tests/test_modules/test_release_basic.cpp`（ours 独有，无冲突）
- 无表尺寸/下标钉值；三行 gatingKey==release、子页 parent==ReleaseGroup、isGroupPage 断言在合并表下全部成立。**不动**。

### E. `tests/probes/uat62_page_shots_probe.cpp`（1 块冲突）
- kSlugs 12 项（§2.11 顺序）。截图编号 01..12 自动适配（twoDigit(i+1)）。

### F. `tests/probes/module_toggle_sidebar_probe.cpp`（无冲突，自动合并）
- 探针 cfg 只启用 markdown/todo/claude_proxy：release（默认启用但无实例被门控跳过渲染）与 mcp（默认关）都不出现在侧边栏；`Settings.y == Agent.y + 49`（work/system 分隔线）断言不受影响。**唯一过时点**：注释「Settings(9)」应为「Settings(10)」（合并枚举），可选修。

### G. `tests/probes/release_probe.cpp`（ours 独有，无冲突）
- `kExistingSidebarPages` 8 项存在性 + y 单调 + 缩进断言：合并后 Mcp 默认关不入树，8 项相对顺序不变，**全绿不动**。`cards.size()==10` 是上线任务列表分页（kReleaseTaskPageSize），与合并无关。

### H. `tests/probes/wallpaper_probe.cpp`（1 块冲突）
- switch 补 4 case（Mcp + Release*）。§3b「壁纸格（插拔卡内第 5 格）」注释过时（合并后第 6 格），断言仅存在性检查，值不动。

### I. `tests/test_modules/test_settings.cpp`（1 块冲突）
- 冲突块（模块卡高 pin）：取 ours 无条件版 `at(216.0f, 254.0f, 330.0f)`，删除 theirs 的 `supported() ? ... : ...` 条件（release 恒 3 行，198 档已不存在）；注释改为「release 恒 5 格 → 3 行；壁纸再加 1 格仍 3 行」。
- 合并树其余壁纸钉值已全部是 theirs 网格版（壁纸格行距 2×rowPitch=112、开关行顶 y+6=186 等）——合并后壁纸仍是第 3 行（index 5，i/2=2），**几何不变、全绿**。
- 高度契约测试（合并树取 ours 版）`kModulesCardHeight == 254.0f` 与上述解法一致，不动。
- 卡清单断言 `{"settings.card.modules"}`（模块 tab 恒单卡）为 theirs 版，独立壁纸卡已删，成立。

### J. 其他
- `tests/test_core` 与 `tests/test_modules/test_ticker.cpp`：ticker.h/cpp/test_ticker.cpp 三文件 ours 与 theirs **blob 全等**（add/add 零冲突），无任何处理。
- theirs 独有新测试（test_mcp_*、test_wallpaper_core/platform/ui/tray、test_tray_dynamic）：`mcp_page: Wallpaper < Mcp < Settings` 在合并枚举下成立；`test_wallpaper_module.cpp:397` 引用的 `kModulesCardHeightWallpaper` 必须保留（=254）。

---

## 5. 非冲突但语义相撞的核对点（自动合并成功 ≠ 语义安全）

| # | 位置 | 核对点 | 状态 |
|---|---|---|---|
| 1 | `src/main.cpp` moduleForPage | 12 case 全（Mcp + Release* + 既有 9） | ✅ 合并树 301-312 已含 |
| 2 | `src/main.cpp` 装配段 | ticker::pump（dslAppConfig 顶）、MCP handlers、壁纸托盘桥三者并存 | ✅ 759 / 391-392 / 354 行已含 |
| 3 | `src/core/schema.h` | release 9 张表（kSql 内）与 theirs 的 todo doc_path + H30/H31 迁移链并存；migrate() 顺序注释合并 | ✅ 自动合并已含双方 |
| 4 | `settings_config` 合法 ID 清单 | kKnownModuleIds 必须同时含 release 与 mcp——**漏 release：sanitized 把存量配置里的 release 剔除 → 上线组静默消失（高风险）** | ⚠️ 冲突块 1，按 §2.6 双方并 |
| 5 | `settings_config` 键数 | load/save 同时读写 mcpPort（472/476 行）与 moduleDefaultsVersion（542/543 行）两键；save()「18 键」注释改 19 | ✅ 键已并存，注释改 |
| 6 | 默认清单 + 迁移 | 合并默认清单 6 项含 release；`kModuleDefaultsVersion=1` 迁移把版本 <1 的存量配置并入 release（默认启用上线组，设计内） | ✅ 自动合并 |
| 7 | `settings_module.cpp` 卡注册表 | 合并树 = theirs：壁纸独立卡行已删、MCP 配置卡已加、模块卡高走 `supported() ? kModulesCardHeightWallpaper : kModulesCardHeight`——两常量都必须存在且 =254，否则不支持平台卡高 198 配 3 行网格裁切 | ⚠️ 依赖 §2.9 解法 |
| 8 | `settings_ui_cards.cpp` 循环头尾一致性 | 冲突块外尾部已按 theirs 写成 isWallpaper 条件式——循环头必须取 theirs，取 ours 即编译失败 | ⚠️ 冲突块 2，按 §2.8 |
| 9 | `app_assembly.cpp` dispatchModuleToggle | MCP 服务启停特例 + 多行遍历并存；关 MCP 时序 = 先 stop 服务再注销页面（特例块在循环前） | ⚠️ 冲突块，按 §2.5 |
| 10 | `src/app_sidebar.cpp` | theirs 未动此文件；合并表多出的 Mcp 行 parent=nullopt 走顶级平铺路径，release 折叠树不受影响 | ✅ |
| 11 | **dev 侧既有红灯** | `origin/dev` 的 `test_router.cpp` core_pageid_order 钉 `settings==6/about==7`，但 dev 枚举 Mcp=6 使 Settings=7——**dev 侧该测试本身是红的**。合并树取 ours 版（无此钉值）+ §4-A 手改，不继承该红灯 | ⚠️ 见 §4-A |
| 12 | worktree 卫生 | 本 worktree 有 untracked `.demo-data/`；合并执行者禁止 `git add -A`，收尾提交用显式 pathspec | — |

---

## 6. 风险 Top3（按后果排序）

1. **`app_assembly.cpp` dispatchModuleToggle 单块双语义**：冲突块两侧是「互不可缺」的插入——漏 ours 侧 = `matched`/`missingPages` 未声明编译失败；漏 theirs 侧 = 编译照过但 MCP 开关只剩页面注册、**服务永不启停**（隐蔽功能缺陷，单测可能全绿）。
2. **`settings_ui_cards.cpp` 循环头与尾部跨块耦合**：冲突只标在循环头，尾部 isWallpaper 条件式按「无冲突」静默归 theirs——若解冲突时习惯性取 ours 循环头（kToggleCount + 无 isWallpaper），尾部全部变量未声明，且错误形态是编译错误（容易发现但返工面大）；同时卡高两常量若只改一个，不支持平台出现 198 高卡装 3 行网格的裁切。
3. **枚举/表序在 7 处钉值的涟漪**：用户定序「MCP 在 上线 上面」必须同时落到 module_interface.h 枚举、app_registry.h 表、test_router ×2、test_app_assembly ×2、test_wallpaper_module、uat62 kSlugs。任一处沿用旧序，要么静态断言/逐位比对红（好），要么侧边栏序静默漂移（Router 按枚举序分发 vs 侧边栏按表序渲染，错序无任何运行时告警）。

---

## 7. 建议执行顺序

1. `git merge origin/dev`（conflicts 按 §1 表逐项手解；worktree 禁止 add -A）。
2. 解完 15 个文件后，再手改 2 个无冲突文件：`tests/test_core/test_router.cpp`（§4-A）、`tests/probes/module_toggle_sidebar_probe.cpp` 注释（可选）。
3. 全表 grep 核对：`git grep -n "static_cast<size_t>(11)\|static_cast<size_t>(9)\|Mcp" src/ tests/` 确认无残留旧尺寸与旧序。
4. 定向编译（禁全量套件）：`cmake --build build --target devdesk_tests` + 定向跑 §4 所列用例（test_router/test_app_assembly/test_wallpaper_module/test_settings/test_release_basic/test_ticker/test_mcp_settings 按 `--filter` 子串），探针留到真窗口 probe。
5. 提交用显式 pathspec 列清单（§1 的 15 文件 + test_router.cpp），不 push，交主会话收口。
