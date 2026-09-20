# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

**DevDesk** 是面向开发者的跨平台桌面工具集（C++17 GUI 应用），包含三大工具：

- **Markdown 笔记本**：本地文件存储 + SQLite FTS5 全文搜索
- **Todo 看板**：SQLite 存储，多列卡片管理
- **Claude 配置切换器**：多套 API profile 管理 + 本地 HTTP 代理（带流式转发）

目标平台：**macOS 27+**（主线承诺）和 **Windows 11**（实验性支持）。

## 🚨 项目核心约束（最重要的几条）

1. **用户不熟悉 C++，纯 AI 开发** — 代码对用户不可见，所有反馈通过：文档 / 中文日志 / 截屏 / 测试结果 / mockup
2. **完整设计文档**：所有架构 / API / 设计 token 决策都写在 `docs/superpowers/specs/2026-09-20-devdesk-design.md`，**实现前先读 spec**
3. **每个 P 阶段完成后必须**：跑测试 + 启动应用 + 截屏 + 更新文档 + git commit（详见 spec §16.2）
4. **配色方案已定为 B（温暖专业 / Claude.ai 风格）**，完整 token 写在 spec §6.5.1 和 §6.5.2

## 🛠️ 常用命令（规划中，P0 阶段实施）

```bash
# 一键构建 + 运行（macOS）
./scripts/dev.sh

# 跑全部测试
./scripts/test.sh

# 自动截屏（当前页面）
./scripts/screenshot.sh <page-name>

# 重置数据库
./scripts/reset-db.sh
```

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`3rd/EUI-NEO/`（git submodule）。

## 🏗️ 架构要点（4 层模块化）

```
main → 4 个 module_* (markdown / todo / claude_proxy / home)
            ↓
       core (Result / paths / sqlite_db / logging / theme / router / drag_controller)
            ↓
       platform (http_client / http_server / keychain / json)
            ↓
         EUI-NEO
```

**关键依赖规则**（违反会导致循环依赖）：
- `core` ❌ 不能依赖 `platform` 或 `EUI-NEO`
- `module_*` 之间 ❌ 不可互相依赖
- `platform` ✅ 可以依赖 `core`（用 `Result`）
- `module_*` ✅ 可以依赖 `core` 和 `platform`

## ⚠️ EUI-NEO 真实能力盘点（已审计源码，2026-09-20）

实现时**只使用以下能力**，不要假设有未列出的 API：

| ✅ 支持 | ❌ 不支持（必须 hack 或放弃） |
|---------|------------------------------|
| button / input (多行+IME+选区) / checkbox / switch / dropdown / datepicker / slider / progress / tabs / navbar / sidebar / dialog模态 / toast / contextMenu / tooltip / dataTable / charts / markdown / virtualList / scrollView | **OS 文件拖入** — 没有 onFileDrop |
| 任意 hex 颜色 + 透明度 + 圆角 + 阴影 + 渐变（仅2色+水平/垂直）+ transform 2D/3D | **drop target API** — 必须自实现 DragController |
| FreeType 字体加载 + 系统字体 fallback + HiDPI 自动 | **tree 组件** — 必须自实现 MarkdownTreeView |
| FontAwesome 图标 + Unicode Emoji + SVG（栅格化） | **macOS 27 Liquid Glass / vibrancy** — 用实色 + 阴影 |
| onTap / onPress / onDrag / onKeyEvent / onScroll / onContextMenu / clipboard / IME / 撤销重做 | **全局 OS hotkey**（窗口失焦时收不到键盘事件）|
| `transition + animate` (8 种 easing + 位掩码属性) | **fontWeight runtime 切换**（只打包 Bold 一份字重，SemiBold/Medium 无效）|
| 多窗口（`app::openWindow`） | **自定义托盘菜单**（只硬编码 Show/Exit）|
| | **Toast 队列**（单实例）|
| | **Collapse / Accordion 组件** |
| | **skeleton loader**（loading 占位需自画）|
| | **Tab 焦点循环**（InputKey::Tab 不自动调度）|
| | **onResize / onMove 回调**（靠 `screen` 参数变化触发自适应）|

完整盘点报告见 spec §16（实施里程碑）章节的依赖章节。

## 🎨 视觉规范（"大气"原则）

- **默认窗口**：1600 × 1000，最小 1200 × 720
- **HiDPI**：默认开启（GLFW_SCALE_TO_MONITOR）
- **主品牌色**：Claude Clay `#D97757`
- **配色**：方案 B（warm professional，暖墨底 + Claude Clay 橘）
- **正文**：15 px（不是 14）
- **页面标题**：32 px
- **统计数字**：48 px
- **主按钮高度**：40-44 px（不是 32）
- **间距**：基于 8 px 网格，主区 padding 40 px
- **圆角**：按钮 10 px / 卡片 12-14 px / chip 999 px（pill）

完整 token 见 spec §6.5.1 和 §6.5.2。

## 🚦 当前阶段

按 spec §16 实施。当前在 **P0（项目骨架）之前**：先调 spec + 出 HTML 原型，**还没有 C++ 代码**。

按 superpowers 流程：
1. 调 spec（采纳审计结果）— **进行中**
2. 出 HTML 交互原型（用支持组件模拟 EUI-NEO 能力）
3. 用户 review 原型
4. 写实现计划（writing-plans skill）
5. 按 P0-P15 实施

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（已确定方案 B）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `../EUI-NEO/` | 上游 GUI 框架（git submodule），实现时直接 `add_subdirectory(3rd/EUI-NEO)` |

## 🧪 验证原则

- **未运行测试 = 未完成**
- **未截屏 = 未完成**
- **未读 spec = 未开始**
- **未 commit = 未持久化**
- **不要假设 EUI-NEO 能力** — 不确定时去 `../EUI-NEO/` 源码 grep