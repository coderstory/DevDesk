# DevDesk 已知限制

> 如实记录，不做粉饰。基线：2026-09-23，Windows 11，330/330 测试全绿，单 exe 零外部 DLL。
> 分「Windows 已知限制」与「跨平台已知限制」两节；每条附来源 / 升级路径（如有）。
> 这些条目大多是各任务 notes 中登记的**范围外项**，不是未发现的缺陷。
> 已修复的历史条目移入文末「已解决」小节存档（含修复 commit），不静默删除。

## 一、Windows 已知限制

### 1. keychain 幂等删除后留下空目录

- **现象**：`deleteSecret` 删除 `<account>.key` 文件后，空的 `<service>` 目录留在磁盘上。
- **位置**：`src/platform/keychain_win.cpp`（只删文件，不清理父目录）。
- **影响**：无害，仅占一个空目录；与 macOS 实现的幂等语义对齐（不存在也返回 Ok）。

### 2. 发布形态：单 exe + fonts/assets 目录（资源未嵌入 exe）

- **现象**：分发时除 `DevDesk.exe` 外还需要旁挂 `fonts/`、`assets/` 目录（构建时 POST_BUILD 拷贝）。
- **边界**：v1 接受此形态；字体等资源未通过 windres 嵌入 exe，嵌入留待需要时。字体文件按 `.gitignore` 约定不入 git（`fonts/Inter-*.ttf`、`fonts/JetBrainsMono-*.ttf`），目录为空时应用以系统字体回退运行。
- **参考**：Release 构建 15.6 MiB（16,402,944 字节，2026-09-22 终验实测；T3 验收时点 15.4 MiB，W3 增码后 +250 KB），`objdump` 仅 16 个 Windows 系统 DLL。

### 3. 开发脚本 check-consistency.sh 在 Windows 不可用

- **现象**：`scripts/check-consistency.sh` 使用 macOS 的 `screencapture` / `open -a`，Windows 上无法运行。
- **定位**：开发工具，非运行时功能，暂不处理。

### 4. 主窗「最小化」期间桌宠动画暂停（Pet-Fix 2026-09-23 登记）

- **现象**：主窗口最小化（标题栏 ─）时桌宠帧动画/输入暂停，直到主窗还原；主窗点 X 隐藏到托盘（Pet-Fix 已修）则不受影响，桌宠照常运行。
- **位置**：EUI-NEO `core/app/glfw_app_main.cpp` iconified 分支（`glfwWaitEventsTimeout(0.25); continue;` 跳过子窗泵；与 hiddenToTray 分支同构）。
- **定位**：与桌宠设计 R5（宠物独立存活）同类，但最小化是暂态、用户还原即恢复，真机四症状范围外，登记待修。

## 二、跨平台已知限制

### UI / 交互

#### 1. Home 页右上角「⚙ 设置」按钮置灰 —— 已解决（W7 `7b115a6` 接线，本次文档核对清账）

- W5 侧边栏落地后设置入口已由侧边栏承担，Home 右上角「⚙ 设置」按钮一度未接线、置灰，且源码注释（"设置页 T11 落地前不可用"）过时。
- **已解决**：`home_module.cpp` 的 `home.btn.settings` 已挂 `navigateTo(core::PageId::Settings)`（2026-09-23 源码核实无 disabled 态），与侧边栏「设置」入口等效；本节此前条目描述滞后于 W7 实态。

#### 2. 运行中切换主题，窗口底色（clearColor）需重启才跟随 —— 已解决（磨砂 Phase B/D）

- **已解决（2026-09-22，磨砂设计 Phase B/D）**：EUI-NEO 上游新增 `app::setClearColor` 运行时 API（覆盖主窗口 + 全部存活子窗口 + requestFullPaint），DevDesk 侧 `main.cpp` 的 theme 回调与 `module::settings::applyWindowVisuals` 已接线——切主题/切窗口效果档，底色（含 alpha）下一帧即时生效，重启后一致。同一通路顺带支持窗口效果三档（关闭/半透/磨砂，见设置页「外观」卡）。

#### 3. Home 统计在 onStart 查询 + 手动刷新

- 仪表盘数据在模块 `onStart` 时查询一次，之后靠右上角「↻ 刷新」手动刷新；不会在每次切回首页时自动重查。
- 原因：`core::Router` 无 `onShow` 回调（只有 startAll / stopAll / navigateTo / compose），切页不触发数据刷新。

#### 4. 无剪贴板 API，token 无复制按钮

- EUI-NEO 的剪贴板能力仅用于 Input 组件内部粘贴（窗口层 `clipboardText/setClipboardText`），未暴露应用级"复制到剪贴板"的组件 API。
- 因此设置页 / 代理凭据的 token 没有「复制」按钮，需手动从 `%APPDATA%\DevDesk\proxy_token.txt` 抄录。

#### 5. Markdown 文件变化轮询是 mtime/size 秒级比对（W8）

- 「当前笔记被外部修改」检测基于 mtime（unix 秒）+ size 快照：外部编辑器若在同秒内改回等长内容，最快要等下一次 mtime 变化才被发现；目录级新文件/删除同样 5 秒内才反映（`MarkdownService::pollChanged`，`markdown_service.cpp`）。
- 外部删除当前笔记且本地有未保存编辑时只弹一次 toast（单实例限制，靠 `lastKnownMtime=-1` 标记节流），不弹选择框——本地编辑保存时会重建文件，无数据丢失风险。
- 升级路径：平台级 watcher（ReadDirectoryChangesW / FSEvent）在 EUI-NEO 有 onFileDrop/watcher 诉求时一并考虑上游 PR。

#### 6. 拖拽元素无 grab/grabbing 光标（EUI-NEO 上游缺口，审计 [30] 登记，2026-09-23）

- EUI-NEO `CursorShape` 只有 Arrow/Hand 两档（`core/input/input_types.h`），挂了 onClick/onPress 的元素自动 Hand——Todo 卡片悬停已有手型提示，但无法用抓取系光标区分「可点击」与「可拖动」，拖拽 affordance 只剩文案。
- 本轮已做的补偿：Todo 副标题补「拖动卡片跨列移动」、Markdown 副标题改静态快捷键说明（审计 [30]）。
- 升级路径：上游给 `CursorShape` 增加 `Grab/Grabbing`（GLFW `glfwSetCursorShape` 原生支持 HAND/ARROW 之外的形状需确认，可能要走 Win32 资源光标），DevDesk 在 onDrag 元素 `.cursor()` 处接线即可。

### 测试

#### 5. soak 300s 压测默认 SKIP

- 5 分钟长连接 soak 用例由环境变量 `DEVDESK_SOAK=1` 门控；未设置时打印 `[SKIP]` 直接通过，避免常规套件（约 86s）背上 5 分钟负担。
- 60s soak 常跑（实测 1200/1200 chunk，0% 丢失，seq 严格递增）；300s 版本需显式 `DEVDESK_SOAK=1` 才会执行，常规验收跑的是门控 SKIP 路径。

### Claude 供应商配置 / Claude Code 接管（provider-config，2026-09-22 Phase E 登记）

#### 10. `CLAUDE_CONFIG_DIR` 自定义目录一期不识别

- 接管通道只写默认 `~/.claude/settings.json`（`core::paths::claudeSettingsFile()`）。用户若设置了 `CLAUDE_CONFIG_DIR` 环境变量把 `.claude` 挪到别处，DevDesk 接管后 Claude Code 不会读到托管 env（它读的是自定义目录下的 settings.json）。
- 升级路径（设计 §9 开放问题 3）：设置页加一个 settings.json 路径输入框即可，有真实用户反馈再做。

#### 11. 接管期间手工改托管键会被下次同步覆盖

- 托管键（`ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` / `ANTHROPIC_MODEL` / `ANTHROPIC_DEFAULT_*_MODEL(_NAME)` / `CLAUDE_CODE_SUBAGENT_MODEL` / extra_env 中配置的键）语义归 DevDesk：接管生效期间用户手工编辑这些键，会在下一次代理启动 / 切 profile 同步时被覆盖。用户自有其他 env 键与非 env 字段完全不受影响。已接受的设计边界（设计 §5.3 如实记录）。

#### 12. Claude 模块关闭期间不执行崩溃补还原（module-gating，2026-09-22 登记）

- 代理崩溃补还原（`recoverPendingSettingsRestore`）挂在 ClaudeService 创建路径上；按插拔方案 A 的语义（设计 §5.2），claude 模块关闭时 service 整体下线，补还原也随之不执行。若上次退出时处于接管状态（异常退出没走到还原）、且本次以关闭 claude 模块启动，`settings.json` 会保持托管值直至重新启用模块。
- 缓解：重新启用 claude 模块并启动代理，下次正常退出即还原；升级路径是把补还原提升为 app 级启动步骤（不依赖模块开关），有真实踩坑再做。

#### 13. env 热切换未做真机验证（留给用户人工点验）

- 官方 settings-reference 文档背书 env 键在「saved change alters the merged env」时运行中会话即重载（设计 §9 问题 1 已关闭），但「真实 Claude Code 连本地代理跑一次对话 + 热切换后立即生效」的真机验证尚未执行——属用户人工点验项（见 CHANGELOG [Provider-Config] 的人工验证清单）。

#### 14. 导入的 `*_MODEL_DESCRIPTION` 变体不识别（范围外登记）

- 官方还存在 `ANTHROPIC_DEFAULT_*_MODEL_DESCRIPTION`（模型描述文案）env 变体；`parseImportedEnv()` 未收录，导入时该键会落入高级 env 表原样透传（不丢失，仅不做槽位语义识别）。设计 §9 问题 2 的可选增强，无真实诉求不做。

#### 15. 表单高级 env 页可见行数上限 5

- 760×600 弹窗高度约束，键值编辑器只渲染前 5 行（导入超过 5 个键时全量保存在状态里、保存时全量写入，界面显示「另有 N 项已保存」提示）；编辑第 6 行及以后需先删行腾位。升级路径：引入 scrollView 或加高弹窗。

#### 16. settings.json 备份轮换不实现（W2 终验降级取舍，2026-09-22）

- 设计 §5.3 曾写「写入前复制 `.devdesk.bak.<n>` 保留 10 份（学 cc-switch）」，实现时未做且决定**不做**：原子写（tmp+rename，不产生半写文件）+ 首份快照 sidecar（接管前原 env 完整保存、停止/退出/崩溃补还原三个还原路径）已覆盖数据安全面；轮换备份的增量收益仅是「接管期间用户手改托管键被覆盖后的历史找回」（该场景语义上托管键本就归 DevDesk 管理，§5.3 已接受的边界）。文档两处（CHANGELOG / 设计 §5.3、§7.3）已同步改为实际行为。升级路径：若未来出现真实诉求，在 `settings_sync.cpp` 写入前复制并做 ≤10 份轮换清理。

### 平台验证

#### 6. macOS 侧自 2026-09-21 迁移后未重新构建验证

- P0–P8 曾在 macOS 上 66 测试通过（含 .app bundle 与文件关联）。
- T2–T11 的全部改动只在 Windows 上编译、测试过；macOS 分支代码（`*_macos.cpp`、`.app` target、Security.framework 链接）保留且按接口隔离模式组织，但**未验证**。回到 macOS 开发时需先全量构建 + 跑测试。

### EUI-NEO 上游 PR 候选（修上游，不在 DevDesk hack）

#### 7. GCC 16.2 `-Os` 对 constexpr std::string 的 C4 mangle bug

- GCC 16.2 以 `-Os` 编译时，对 C++20 起 constexpr 化的 `std::string` 移动构造发出新式 C4 引用，而 MSYS2 `libstdc++.a` 只有 C1/C2 定义 → Release 链接 undefined reference（`-O3` 不触发）。
- DevDesk 侧已 workaround 并注释：MinGW 下从 app target 摘除 EUI-NEO 强加的 `-Os`（回到 CMake 默认 `-O3`），见顶层 `CMakeLists.txt`。
- 根治应走上游：EUI 不应给消费者 app target 强加优化级别，或工具链补 C4 别名。

#### 8. EUI-NEO platform.cpp macOS 辅助函数缺 `__APPLE__` 守卫

- `core/platform/platform.cpp` 中 `appleScriptString` / `appleFileDialogCommand` 等 macOS 专用辅助函数在所有平台编译（Windows/Linux 上是死代码）。
- 上游补 `#if defined(__APPLE__)` 守卫即可，属小改动 PR。

#### 9. 能力表中的长期限制（EUI-NEO 现状）

以下为 EUI-NEO 0.6.0 的已审计边界（完整清单见 [EUI-NEO-CAPABILITIES.md](EUI-NEO-CAPABILITIES.md)），短期内不构成 DevDesk 的阻塞，但属于上游长期限制：

- **无 OS 文件拖入**（无 onFileDrop）——DevDesk 的 Markdown 导入只能走目录扫描；
- **单字重**（只打包 Bold 一份字重，`fontWeight` 运行时切换无效，SemiBold/Medium 不生效）；
- **无全局 OS hotkey**（窗口失焦时收不到键盘事件）；
- 其他：无 tree 组件、无自定义托盘菜单（只硬编码 Show/Exit）、Toast 单实例、无 Collapse/Accordion、无 skeleton loader、无 Tab 焦点循环、无 onResize/onMove 回调。

---

## 三、已解决（历史条目存档，W5 遗留项清零，2026-09-22）

以下条目曾列于本文件，W5 已修复，移入此处存档（不删除历史）：

1. **非 ASCII 文件路径可能乱码（argv 打开文件）**（原 Windows-1）——**已于 W5 修复（`90d5792`）**：`file_open_win.cpp` 改 `GetCommandLineW + CommandLineToArgvW + WideCharToMultiByte(CP_UTF8)`，宽字符 argv 转 UTF-8，非 ASCII 路径不再乱码。
2. **字号缩放仅作用于设置页自身**（原跨平台 UI-1）——**已于 W5 修复（`90d5792`）**：`core::theme` 增加 `setFontScale/fontScale`（默认 1.0，钳制 [0.5, 2.0]），在 token 读取层统一施加（setDark/setLight 重建后重放，不叠加）；设置页字号档位改写全局缩放，全模块生效。
3. **无侧边栏导航（浮动齿轮桥接）**（原跨平台 UI-2）——**已于 W5 修复（`90d5792`）**：main.cpp 自绘常驻侧边栏（260px，row(侧边栏, 页面内容)，含设置共 5 个模块入口，当前页高亮，点击 navigateTo）；浮动齿轮桥接已删。曾记的残留（Home ⚙ 置灰按钮）亦已接线（见上文跨平台 UI-1）。
4. **Home 快捷按钮降级为导航；最近编辑点击仅导航**（原跨平台 UI-3）——**已于 W5 修复（`c3901a8`）**：Router 增加跨模块跳转负载（`navigateTo(page, payload)` + `consumePendingPayload` 一次性消费语义）；Home「+ 新建笔记 / + 新建任务」带 `action:*` 负载在目标页触发新建弹窗，「最近编辑」行点击带 `open:<路径>` 负载直接打开那篇笔记。
5. **Claude 切换器页面的操作按钮未接线**（原跨平台 UI-4）——**已于 W5 修复（`843cb2c`）**：claude_module.cpp 从 0 个 onClick 到全量真接 `ClaudeService`——新建/编辑（dialog 表单，编辑预填、Key 留空=不换）、启用（setActiveProfile）、删除（确认 dialog + Keychain 清理）、测试连通（testConnection + toast）、代理开关（startProxy/stopProxy），失败 toast 显示 Result 中文错误。
6. **深色主题下除设置页外页面背景固定浅色**（原跨平台 UI-6）——**已于 W5 修复（`90d5792`）**：`clearColor` 改为构造时从 `core::theme::current().colors.background` 快照，跟随持久化主题；EUI-NEO 无运行时改 clearColor 的 API，运行中切换主题仍需重启生效（运行中切换的小尾巴见上文跨平台 UI-2）。

---

**对应改进去向**：上游类（7–9）按 [EUI-NEO-PR-PLAN.md](EUI-NEO-PR-PLAN.md) 的优先级推进（已选定 P0 `fontWeight` 真切换、P1 全局 hotkey 两个 PR 候选）；其余为低优先级收尾项，随下次 UI 清理顺手处理。
