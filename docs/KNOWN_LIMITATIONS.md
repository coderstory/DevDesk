# DevDesk 已知限制

> 如实记录，不做粉饰。基线：2026-09-22，Windows 11，137/137 测试全绿，单 exe 零外部 DLL。
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

## 二、跨平台已知限制

### UI / 交互

#### 1. Home 页右上角「⚙ 设置」按钮仍置灰（残留装饰按钮）

- W5 侧边栏落地后设置入口已由侧边栏承担（原浮动齿轮桥接已删），但 Home 页右上角的「⚙ 设置」按钮**没有随之一并接线或移除**，仍是 disabled 置灰状态，且源码注释（"设置页 T11 落地前不可用"）已过时。
- **影响**：纯视觉残留，不影响任何功能（侧边栏常驻可进设置）；属下次 UI 清理顺手项（改为 navigateTo(Settings) 或直接删掉按钮）。

#### 2. 运行中切换主题，窗口底色（clearColor）需重启才跟随

- W5 起 `clearColor` 在启动时从持久化主题快照（`main.cpp` 的 `dslAppConfig()`），深浅主题启动即正确。
- 但 EUI-NEO 无运行时改 clearColor 的 API（`DslWindowRuntime::request_` 是 initialize 时的快照且无私有 setter），**运行中**在设置页切主题时卡片/文字色即时生效、窗口底色不跟随，重启后一致。根治需上游提供运行时 clearColor API。

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

### 测试

#### 5. soak 300s 压测默认 SKIP

- 5 分钟长连接 soak 用例由环境变量 `DEVDESK_SOAK=1` 门控；未设置时打印 `[SKIP]` 直接通过，避免常规套件（约 86s）背上 5 分钟负担。
- 60s soak 常跑（实测 1200/1200 chunk，0% 丢失，seq 严格递增）；300s 版本需显式 `DEVDESK_SOAK=1` 才会执行，常规验收跑的是门控 SKIP 路径。

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
3. **无侧边栏导航（浮动齿轮桥接）**（原跨平台 UI-2）——**已于 W5 修复（`90d5792`）**：main.cpp 自绘常驻侧边栏（260px，row(侧边栏, 页面内容)，含设置共 5 个模块入口，当前页高亮，点击 navigateTo）；浮动齿轮桥接已删。残留：Home 页右上角「⚙ 设置」按钮本身仍是置灰状态（见上文跨平台 UI-1，纯视觉残留）。
4. **Home 快捷按钮降级为导航；最近编辑点击仅导航**（原跨平台 UI-3）——**已于 W5 修复（`c3901a8`）**：Router 增加跨模块跳转负载（`navigateTo(page, payload)` + `consumePendingPayload` 一次性消费语义）；Home「+ 新建笔记 / + 新建任务」带 `action:*` 负载在目标页触发新建弹窗，「最近编辑」行点击带 `open:<路径>` 负载直接打开那篇笔记。
5. **Claude 切换器页面的操作按钮未接线**（原跨平台 UI-4）——**已于 W5 修复（`843cb2c`）**：claude_module.cpp 从 0 个 onClick 到全量真接 `ClaudeService`——新建/编辑（dialog 表单，编辑预填、Key 留空=不换）、启用（setActiveProfile）、删除（确认 dialog + Keychain 清理）、测试连通（testConnection + toast）、代理开关（startProxy/stopProxy），失败 toast 显示 Result 中文错误。
6. **深色主题下除设置页外页面背景固定浅色**（原跨平台 UI-6）——**已于 W5 修复（`90d5792`）**：`clearColor` 改为构造时从 `core::theme::current().colors.background` 快照，跟随持久化主题；EUI-NEO 无运行时改 clearColor 的 API，运行中切换主题仍需重启生效（运行中切换的小尾巴见上文跨平台 UI-2）。

---

**对应改进去向**：上游类（7–9）按 [EUI-NEO-PR-PLAN.md](EUI-NEO-PR-PLAN.md) 的优先级推进（已选定 P0 `fontWeight` 真切换、P1 全局 hotkey 两个 PR 候选）；其余为低优先级收尾项，随下次 UI 清理顺手处理。
