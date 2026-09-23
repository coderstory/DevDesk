# DevDesk

面向开发者的跨平台桌面工具集（C++20 原生 GUI，单 exe）。三个核心工具 + 仪表盘 + 设置 + 桌宠：

- **Markdown 笔记本** —— 笔记即普通 `.md` 文件：FTS5 全文搜索（支持中文）、目录树 / 标签 / 大纲、编辑·双栏·预览三态视图 + 键盘流（Ctrl+Shift+P 切视图、Ctrl+B/I/K 格式、Tab 缩进/表格跳格、右键插入表格）、400ms 自动保存
- **Todo 看板** —— 多列看板：拖拽跨列、优先级 / 截止日期 / 标签、搜索与逾期视图、右键列头重命名 / 移动 / 改色 / 删除列
- **Claude 切换器** —— 多套 API Profile 管理 + 本地流式代理（默认 8888 可配端口，Bearer 鉴权，SSE 透传；代理默认关、手动 / 随应用启动，启动即自动接管 Claude Code `settings.json`、停止自动还原）
- **首页仪表盘** —— 今日笔记 / 待办 / 逾期 / API 调用统计（每次进页自动重查 + 手动刷新）+ 最近编辑
- **设置** —— 六张卡：外观（主题 / 字号 / 窗口效果三档）/ 模块开关（重启生效）/ 桌宠 / 笔记目录（打开目录）/ Claude 代理（自启 · 端口 · Token 脱敏 · 接管状态）/ 日志保留（切换即时清理）
- **桌宠** —— 无边框置顶透明小窗宠物（module_pet，非页面模块）：拖拽 / 四态动画（待机·被戳·打字·睡觉）/ 气泡对话（含逾期任务数提醒）/ 右键菜单（穿透·换肤·位置重置·隐藏·退出 DevDesk 二段确认）/ 程序化与美术皮肤体系；默认关闭，经设置页「模块」卡启用（重启生效）

当前状态（2026-09-23，Windows 11）：**330/330 测试全绿；单 exe 零外部 DLL（Release 15.6 MiB）；10MB 流式端到端吞吐实测约 240–320 MB/s；60s soak 0% 丢失**。macOS 分支代码保留（接口隔离模式），自 2026-09-21 迁移后未重新验证。

GUI 框架为 [EUI-NEO](https://sudoevolve.github.io/EUI-NEO/)（源码同级目录集成，非 submodule）。

## 三分钟上手（Windows 11 + MSYS2 MinGW64）

**前置**：

1. [EUI-NEO](../EUI-NEO) 源码在 DevDesk **同级目录**（`../EUI-NEO`；也可 `-DEUI_NEO_DIR=<路径>` 覆盖）。**EUI-NEO 需保持本地 main 最新**——当前依赖 rev `b92abb7`（含 onWindowDestroyed/requestFocus/trayMenu/windowEffect/lineStyles/getMonitorWorkAreas/setSystemPowerHandler/styledMenu/onTrayMenu/windowIsFocused 等新 API；这批提交尚未 push 到 origin，从 GitHub 新克隆拿到的是旧 v0.6.0、会编译失败）。顶层 CMake configure 时打印 EUI-NEO git rev 并在版本不符时软警告（不 fail）；EUI-NEO push 后将启用硬版本检查。
2. MSYS2 MinGW64 环境：`mingw-w64-x86_64-gcc`、`mingw-w64-x86_64-sqlite3`、`mingw-w64-x86_64-curl`、`mingw-w64-x86_64-openssl`（pacman 安装），以及 CMake ≥ 3.14 与 Ninja。
   本机工具链路径（CMake 4.3.3 / Ninja / MinGW64 GCC 16.2）见 [CLAUDE.md](CLAUDE.md)。

**构建**（Git Bash；裸 `cmake` configure 即可，零 `-D` workaround，静态链接 `DEVDESK_STATIC` 默认 ON）：

```bash
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

cd /d/project/gui/DevDesk        # 换成你的 clone 位置
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/DevDesk.exe
```

Release 构建（单 exe 15.6 MiB = 16,402,944 字节，仅依赖 Windows 系统 DLL；T3 验收时点为 15.4 MiB，W3 增设置页 / Home 服务后 +250 KB）：

```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

> 换平台后（如从 macOS 迁来）必须删掉旧 `build/` 重建——缓存里锁着旧平台的产物。
> 用户数据在 `%APPDATA%\DevDesk`，与仓库无关；使用说明见 [docs/USER_GUIDE.md](docs/USER_GUIDE.md)。

## 跑测试

```bash
cd build
ctest --output-on-failure                  # 全量 330 例（不含默认 SKIP 的 300s soak）
```

测试二进制支持**按名子串过滤**与调试日志（自制轻量框架，没有 `--list-test-cases` 之类参数，子串即过滤）：

```bash
./tests/devdesk_tests.exe stress           # 只跑名字含 "stress" 的用例
./tests/devdesk_tests.exe markdown         # 只跑 markdown 相关用例
./tests/devdesk_tests.exe --log            # 打开 Debug 级控制台日志
```

**soak 300s 压测默认 SKIP**（常规套件不背 5 分钟），需要时显式打开门控：

```bash
DEVDESK_SOAK=1 ./tests/devdesk_tests.exe soak
```

## 架构一图流

4 层模块化 + 平台接口隔离（与 EUI-NEO 后端选择同一模式）：

```
main.cpp ── 装配：paths / logging / SQLite / Claude 代理 / Router + 常驻侧边栏（桌宠窗直连 startPetWindow）
   │
   ▼
module_home / module_markdown / module_todo / module_claude_proxy / module_settings / module_pet
   │   可依赖 core + platform；模块之间禁止互相依赖（跨模块只走 core::appRouter 导航；
   │   home/markdown/todo/claude_proxy/settings 是 Router 页面模块，module_pet 为独立子窗模块）
   ▼
core ── Result / paths / sqlite_db / database / logging / theme / router / time
   │   禁止依赖 platform 与 EUI-NEO（错误处理统一 Result<T>，不抛异常）
   ▼
platform ── http_client / http_server / keychain / json / file_open
   │   接口隔离：头文件平台无关，实现按平台拆文件，CMake target_sources 按平台选择
   ▼
EUI-NEO ── GUI 框架（OpenGL 后端），../EUI-NEO 以 add_subdirectory 集成
```

平台接口隔离的拆法（以 keychain 为例，paths / time / file_open 同）：

```
src/platform/keychain.h          平台无关接口
src/platform/keychain_win.cpp    Windows：DPAPI + 文件存储
src/platform/keychain_macos.cpp  macOS：Security.framework
src/platform/keychain_posix.cpp  Linux 兜底
```

业务代码零 `#ifdef _WIN32` / `__APPLE__`（验收口径：grep `src/` 全部 *.cpp/*.h 零命中）。

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/superpowers/specs/2026-09-20-devdesk-design.md](docs/superpowers/specs/2026-09-20-devdesk-design.md) | 唯一权威设计 spec（架构 / API / 设计 token / §16 里程碑） |
| [docs/superpowers/plans/2026-09-21-windows-recovery-plan.md](docs/superpowers/plans/2026-09-21-windows-recovery-plan.md) | macOS→Windows 迁移恢复计划（T1–T12 任务与实测数字） |
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md) | 用户指南（六模块用法 / 代理配置 / 数据位置） |
| [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) | 已知限制（Windows / 跨平台，如实记录） |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构详解（4 层 / 依赖规则 / OOP 模式 / ADR） |
| [docs/EUI-NEO-CAPABILITIES.md](docs/EUI-NEO-CAPABILITIES.md) | EUI-NEO 能力盘点（遇到 UI 问题先查这里） |
| [docs/EUI-NEO-PR-PLAN.md](docs/EUI-NEO-PR-PLAN.md) | EUI-NEO 上游 PR 方案 |
| [docs/MAINTENANCE-EVALUATION.md](docs/MAINTENANCE-EVALUATION.md) | P16 长期维护评估（Metal 后端 / Markdown 编辑器） |
| [CHANGELOG.md](CHANGELOG.md) | 变更记录（a1a106a 起） |

开发约定、工具链路径、EUI-NEO 上游贡献政策见 [CLAUDE.md](CLAUDE.md)。
