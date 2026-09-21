# DevDesk 已知限制

> 如实记录，不做粉饰。基线：2026-09-22，Windows 11，124/124 测试全绿，单 exe 零外部 DLL。
> 分「Windows 已知限制」与「跨平台已知限制」两节；每条附来源 / 升级路径（如有）。
> 这些条目大多是各任务 notes 中登记的**范围外项**，不是未发现的缺陷。

## 一、Windows 已知限制

### 1. 非 ASCII 文件路径可能乱码（argv 打开文件）

- **现象**：通过文件关联 / 命令行参数打开文件时，路径含非 ASCII 字符（如中文文件名）可能乱码。
- **原因**：`src/platform/file_open_win.cpp` 使用 MinGW CRT 的 ANSI `__argv`。
- **升级路径**：`GetCommandLineW + CommandLineToArgvW` 再转 UTF-8，完整做法已注释在源文件头部，留待后续任务。

### 2. keychain 幂等删除后留下空目录

- **现象**：`deleteSecret` 删除 `<account>.key` 文件后，空的 `<service>` 目录留在磁盘上。
- **位置**：`src/platform/keychain_win.cpp`（只删文件，不清理父目录）。
- **影响**：无害，仅占一个空目录；与 macOS 实现的幂等语义对齐（不存在也返回 Ok）。

### 3. 发布形态：单 exe + fonts/assets 目录（资源未嵌入 exe）

- **现象**：分发时除 `DevDesk.exe` 外还需要旁挂 `fonts/`、`assets/` 目录（构建时 POST_BUILD 拷贝）。
- **边界**：v1 接受此形态；字体等资源未通过 windres 嵌入 exe，嵌入留待需要时。字体文件按 `.gitignore` 约定不入 git（`fonts/Inter-*.ttf`、`fonts/JetBrainsMono-*.ttf`），目录为空时应用以系统字体回退运行。
- **参考**：Release 构建 15.6 MiB（16,402,944 字节，2026-09-22 终验实测；T3 验收时点 15.4 MiB，W3 增码后 +250 KB），`objdump` 仅 16 个 Windows 系统 DLL。

### 4. 开发脚本 check-consistency.sh 在 Windows 不可用

- **现象**：`scripts/check-consistency.sh` 使用 macOS 的 `screencapture` / `open -a`，Windows 上无法运行。
- **定位**：开发工具，非运行时功能，暂不处理。

## 二、跨平台已知限制

### UI / 交互

#### 1. 字号缩放仅作用于设置页自身

- 设置页的字号缩放（85%–130%，正文基准 15px）只影响**设置页自己的文字**。
- 原因：`core::theme` 的 SizeTokens 无运行时缩放入口（`setDark/setLight` 用硬编码 token 重建全局），且各模块存在硬编码字号。
- 现状：配置已持久化到 `config.json`（`fontScalePercent`），全局接线留后续（需在 `core::theme` 加缩放 API + 清理各模块硬编码字号）。

#### 2. 无侧边栏导航

- Router 只渲染**当前模块**，没有 spec §8.5 设想的常驻侧边栏。
- 设置入口是 main.cpp 在 compose 顶层画的**浮动齿轮按钮**（右下角，zIndex 100）：非设置页点它进设置，设置页变返回箭头回首页。正式侧边栏落地后可移除该桥接。
- 连带影响：首页右上角的「⚙ 设置」按钮当前为置灰 disabled 状态；从 Markdown / Todo / Claude 页回首页需经设置页中转。

#### 3. Home 快捷按钮降级为导航；最近编辑点击仅导航

- 「+ 新建笔记 / + 新建任务 / 切换 Claude Profile」三个快捷按钮当前**只跳转到对应模块**，不做跨模块弹窗（模块间禁止互相依赖，无跨模块"开新建对话框"机制），新建 / 切换动作需在目标页面完成。
- 「最近编辑」列表整行点击**仅跳转 Markdown 页**，不会打开点击的那一篇笔记。

#### 4. Claude 切换器页面的操作按钮未接线

- 页面数据全部真实（Profile 列表 / 当前 Profile 详情 / 最近调用 / 代理运行状态，均读 `ClaudeService`），但操作按钮——`+ 新建 Profile`、`启用`、`测试连通`、`编辑`、`删除`、`⏸ 停止代理`——是展示样式，**未接点击事件**（该文件 0 个 onClick）。
- 底层能力完整：Profile CRUD / 启用 / 切换 / 代理启停在 `ClaudeService` 已实现并有单元测试覆盖（claude_service / proxy_router / streaming 共 30+ 用例），缺的只是 UI 交互接线。

#### 5. Home 统计在 onStart 查询 + 手动刷新

- 仪表盘数据在模块 `onStart` 时查询一次，之后靠右上角「↻ 刷新」手动刷新；不会在每次切回首页时自动重查。
- 原因：`core::Router` 无 `onShow` 回调（只有 startAll / stopAll / navigateTo / compose），切页不触发数据刷新。

#### 6. 深色主题下除设置页外，页面背景固定浅色

- `main.cpp` 的 `clearColor` 固定为浅色 `#FAF9F5`，且除设置页（自绘满屏背景）外其他页面不铺满屏背景。
- 深色主题下 Markdown / Todo / Claude / Home 页的卡片区域跟随主题变深，但窗口底色仍是浅色，观感割裂。

#### 7. 无剪贴板 API，token 无复制按钮

- EUI-NEO 的剪贴板能力仅用于 Input 组件内部粘贴（窗口层 `clipboardText/setClipboardText`），未暴露应用级"复制到剪贴板"的组件 API。
- 因此设置页 / 代理凭据的 token 没有「复制」按钮，需手动从 `%APPDATA%\DevDesk\proxy_token.txt` 抄录。

### 测试

#### 8. soak 300s 压测默认 SKIP

- 5 分钟长连接 soak 用例由环境变量 `DEVDESK_SOAK=1` 门控；未设置时打印 `[SKIP]` 直接通过，避免常规套件（约 86s）背上 5 分钟负担。
- 60s soak 常跑（实测 1200/1200 chunk，0% 丢失，seq 严格递增）；300s 版本需显式 `DEVDESK_SOAK=1` 才会执行，常规验收跑的是门控 SKIP 路径。

### 平台验证

#### 9. macOS 侧自 2026-09-21 迁移后未重新构建验证

- P0–P8 曾在 macOS 上 66 测试通过（含 .app bundle 与文件关联）。
- T2–T11 的全部改动只在 Windows 上编译、测试过；macOS 分支代码（`*_macos.cpp`、`.app` target、Security.framework 链接）保留且按接口隔离模式组织，但**未验证**。回到 macOS 开发时需先全量构建 + 跑测试。

### EUI-NEO 上游 PR 候选（修上游，不在 DevDesk hack）

#### 10. GCC 16.2 `-Os` 对 constexpr std::string 的 C4 mangle bug

- GCC 16.2 以 `-Os` 编译时，对 C++20 起 constexpr 化的 `std::string` 移动构造发出新式 C4 引用，而 MSYS2 `libstdc++.a` 只有 C1/C2 定义 → Release 链接 undefined reference（`-O3` 不触发）。
- DevDesk 侧已 workaround 并注释：MinGW 下从 app target 摘除 EUI-NEO 强加的 `-Os`（回到 CMake 默认 `-O3`），见顶层 `CMakeLists.txt`。
- 根治应走上游：EUI 不应给消费者 app target 强加优化级别，或工具链补 C4 别名。

#### 11. EUI-NEO platform.cpp macOS 辅助函数缺 `__APPLE__` 守卫

- `core/platform/platform.cpp` 中 `appleScriptString` / `appleFileDialogCommand` 等 macOS 专用辅助函数在所有平台编译（Windows/Linux 上是死代码）。
- 上游补 `#if defined(__APPLE__)` 守卫即可，属小改动 PR。

#### 12. 能力表中的长期限制（EUI-NEO 现状）

以下为 EUI-NEO 0.6.0 的已审计边界（完整清单见 [EUI-NEO-CAPABILITIES.md](EUI-NEO-CAPABILITIES.md)），短期内不构成 DevDesk 的阻塞，但属于上游长期限制：

- **无 OS 文件拖入**（无 onFileDrop）——DevDesk 的 Markdown 导入只能走目录扫描；
- **单字重**（只打包 Bold 一份字重，`fontWeight` 运行时切换无效，SemiBold/Medium 不生效）；
- **无全局 OS hotkey**（窗口失焦时收不到键盘事件）；
- 其他：无 tree 组件、无自定义托盘菜单（只硬编码 Show/Exit）、Toast 单实例、无 Collapse/Accordion、无 skeleton loader、无 Tab 焦点循环、无 onResize/onMove 回调。

---

**对应改进去向**：UI 接线类（本节 1–7）属恢复计划后续任务范畴；上游类（10–12）按 [EUI-NEO-PR-PLAN.md](EUI-NEO-PR-PLAN.md) 的优先级推进（已选定 P0 `fontWeight` 真切换、P1 全局 hotkey 两个 PR 候选）。
