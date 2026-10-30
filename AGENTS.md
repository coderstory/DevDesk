# AGENTS.md（DevDesk）

DevDesk 开发主仓库的 AI agent 入口：先读这份，再按需进子文档。2026-10-29 结构调整：原外层工作区壳已删除，本仓库自含全部 AI 适配件；EUI-NEO 由同级目录改为仓内 submodule。

## 一、仓库布局与 EUI-NEO submodule

本仓库 = **DevDesk** 应用（C++20 / CMake）。`EUI-NEO/` 是 git submodule = **EUI-NEO** GUI 框架（C++17：DSL + Runtime + 渲染 + 窗口），URL 指向 coderstory fork。

- 依赖方向只有一个：**DevDesk → EUI-NEO**，反向不存在。
- 接线方式：`add_subdirectory`，路径由 `EUI_NEO_DIR` 指定（默认即仓内 `EUI-NEO/`，脚本与 CMake 自动定位）。
- EUI-NEO 用的是 **coderstory fork**（远多于上游的本地提交：原生标题栏主题联动、窗口材质贯通、`setScrollOffset`、嵌套弹层 z 序逃逸、dropdown `openUp` 等）。**直接用上游 `sudoevolve/EUI-NEO` 会编译失败**。
- EUI-NEO 仓内 remote：`origin` = 上游 sudoevolve（**只读，永不 push**，2026-09-28 决策）、`fork` = coderstory（同步远端，双机与 CI 的构建源）。
- **跨仓耦合警告**：DevDesk 硬依赖 EUI-NEO 默认分支 HEAD、不锁版本。**先推 EUI-NEO → `fork`，再推 DevDesk → `origin`**；顺序反了 CI 会在 DevDesk 报一个指向错误文件的编译错误（真实发生过）。
- EUI-NEO 遇到能力缺失，优先在 `EUI-NEO/` 里修（fork），不要在 DevDesk 自造 workaround。EUI-NEO 内有多个历史 feature/fix 分支，动之前先确认所在分支。

## 二、双机同步协议（submodule 版）

主开发在 Windows 机，本机（Mac）可能落后。开工前先同步：

```sh
git fetch --all --prune && git pull --ff-only    # DevDesk ← origin/dev
git submodule update --init                       # EUI-NEO ← fork/main（.gitmodules 已指 fork）
```

用户口径：**以远端为准**；确要丢弃本地改动前，先把 `git diff` 备份成 patch 放到仓库外。同步就是同步，发现的问题只报告不动手。

## 三、双平台原生支持（硬约束）

macOS（arm64）+ Windows 11 **都必须是原生可用的一等公民**，不是「先在 macOS 跑通、Windows 以后再说」。

| 维度 | macOS arm64 | Windows 11 |
|---|---|---|
| 平台源码 | `*_macos.cpp` ×16 + `.mm` ×1（EUI-NEO 另有 macos 4 个，Cocoa/ObjC++） | `*_win.cpp` ×31（EUI-NEO 另有 17 个，DWM 原生 API） |
| 共用兜底 | `*_posix.cpp` | — |
| 窗口效果 | 三档原生材质（关闭/半透/磨砂） | DWM 原生 API |
| 数据根 | `~/DevDesk` | `%USERPROFILE%\DevDesk`（`DEVDESK_DATA_ROOT` 可覆盖） |
| 打包 | `.app` + DMG（`scripts/package-macos.sh`） | Inno Setup 单 exe 零外部 DLL（`scripts/package-windows.sh`） |
| 依赖 | Homebrew `openssl@3` | MSYS2 MinGW64，静态链 sqlite3/curl/ssl/crypto |

**平台差异一律拆文件 + CMake 按平台选源。业务代码里禁止 `#ifdef _WIN32` / `__APPLE__`。**
改任何平台相关代码，都要同时想两头；只按当前开发机验证等于没验证。

已知平台不对称红（非回归，别当 bug 修）：macOS FileWatcher 轮询兜底 ×2、keychain 空账号语义 ×1。

## 四、代码检索纪律（codegraph 优先）

查代码先走 codegraph（仓库根 `codegraph.json`，按符号/调用关系检索），grep 只作兜底全文扫。索引排除 `**/build/**`、`**/wt-*/**` 与 EUI-NEO；新增临时目录要同步进 exclude，别把产物灌进索引。

## 五、构建与测试纪律

> **一律 `bash scripts/build-test-run.sh`（仓库根）。禁止手敲 `cmake -B …` / `cmake --build …`（有门禁硬阻断）。**

脚本封装了手敲必漏的约定：EUI-NEO submodule 自动定位、ccache 两个 launcher 参数、`OPENSSL_ROOT_DIR` 平台差异、发布形态守卫（Windows 单 exe/零外部 DLL）、模块隔离守卫、并行度封顶 `DEVDESK_JOBS=3`。

| 开关 | 作用 |
|---|---|
| `--clean` | 全量重建（默认增量） |
| `--release` | Release，构建目录切到 `build-release/`（与 Debug 互不覆盖） |
| `--no-test` / `--no-run` | 跳过测试 / 不启动 app |
| `--install` | 仅 macOS：重组 `.app` 装到 `/Applications`，装完校验 sha256 |
| `--filter X` | 只跑描述含 X 的用例 |

其它脚本：
- `scripts/check-module-isolation.sh` — 模块隔离静态守卫（退出码 0 通过 / 1 违规 / 2 环境错，不静默绿）
- `scripts/check-consistency.sh` — 一致性检查（macOS 走 `screencapture`，需终端「屏幕录制」TCC 权限，已授予）

接线变更 / 换平台后必须 `rm -rf build build-release` 重建：CMake 缓存里锁着旧路径/旧平台产物。

测试基线：**用例总数以运行输出 `Total:` 为准，不要在文档写死**。

## 六、架构约束（DevDesk 4 层）

```
main → module_*（Router 页面模块 + 独立子窗）
           ↓
       core（Result / paths / sqlite_db / logging / theme / router）
           ↓
       platform（http_client / http_server / keychain / json）
           ↓
       EUI-NEO（submodule）
```

- `module_*` 之间 ❌ 不可互相依赖
- `core` ❌ 不依赖 `platform`，❌ 不调用 EUI-NEO 运行时行为（header-only 类型桥接例外：`eui::Color` 值类型 / `Ui`/`Screen` 签名透传 / `component_theme.h` 纯映射）
- `platform` ✅ 可依赖 `core`
- **不抛 C++ 异常**，错误统一走 `Result<T>`（`src/core/result.h`）
- Router 页面模块在 `src/app_registry.h` 一行登记，顺序 = 注册顺序 = 侧边栏顺序；**枚举以 `kModuleRegistry` 为准**（当前：home / markdown / todo / schedule / claude_proxy / agent / memory / wallpaper（平台能力不支持时不注册）/ mcp / release 分组（任务 + 基础配置两子页）/ settings / about）。

## 七、编辑边界

**仓库外归档，不要在仓库内重建**：
- 原 `/Users/coderstory/dev/gui` 工作区壳的产物/归档（claude_proxy 安全审计、EUI-NEO PR 文案、splash 交付、zpatch 拷贝等）已于 2026-10-29 清理，备份在 `/Users/coderstory/dev/gui-archive-backup-20261029/`（目录 + 同名 .tar.gz）。

**可写**：本仓库全部（含 submodule `EUI-NEO/`，其在 main 分支）。

## 八、协作方式

- 用户**不熟悉 C++**，纯 AI 开发。代码对用户不可见，反馈走：文档 / 中文日志 / 截屏 / 测试结果。
- 文档驱动：权威 spec 在 `docs/superpowers/specs/`，实现前先读。
- 待办与剩余决策：`docs/superpowers/plans/2026-09-22-backlog.md`、`TODO.md`。
- commit 前缀：`feat:` / `fix:` / `refactor:` / `docs:`。
- **不要阶段性汇报**：完成一个小阶段就停下来汇报会被用户明确否定（说过两次）；把整个任务做完、测试跑绿再一次性汇报。汇报极简（编号+一句话结论），细节与证据写进清单卡文件并给路径。

## 九、文档入口

| 文件 | 内容 |
|---|---|
| `CLAUDE.md` | DevDesk 完整开发指南（工具链、构建约定、架构约束、EUI-NEO 能力盘点） |
| `docs/superpowers/specs/` | 设计与需求 spec（权威） |
| `docs/EUI-NEO-CAPABILITIES.md` | EUI-NEO 能力盘点，遇 UI 问题先查 |
| `docs/USER_GUIDE.md` | 使用说明 |
| `EUI-NEO/docs/` | 框架侧文档（DSL / 组件 / 布局 / 事件 / 动画 / 渲染后端 / 平台能力 / 集成指南） |
| `EUI-NEO/CONTRIBUTING.md` | 框架提交规范 |
| `DOCS-CONVENTIONS.md`（仓库根） | 文档目录地图；通用命名 / 骨架 / 写作规范权威在 devflow 插件模板 |
| `.zcode/` | 本项目 AI 适配件（见 §十）；角色与流程 skill 由 devflow 插件供给 |

## 十、AI 开发作业纪律（流程权威在 devflow 插件，激活由其 SessionStart hook 自动注入）

> 通用规则（S/M/L 分级与裁决、卡片链、完成定义 = 证据物实操实证、检索先行与 POC、代码负资产与最小 90%、注释与测试纪律、六角色白名单）的权威定义在 devflow 插件（`ask` / `consolidate` / 6 角色），每个会话由插件 hook 注入激活提示，本节不复制。以下仅**本项目特有**约定：

- **检查点提交**：长任务（>3 文件）每完成一个环节，把本次任务相关文件以 `wip:` 临时 commit，收尾 squash 为单个 `feat:` / `fix:` / `docs:` commit；`git add` 前先 `git status` 确认不含他人未提交改动。
- **Agent 评测任务集**：`docs/research/agent-eval-tasks.md`——真实失败当次入库、连续 5 次全过毕业为回归任务；复盘时读子代理完整汇报（transcript）。
- **文档规范**：通用权威在插件 `devflow-setup` 的 docs-conventions 模板；本项目路径见 `DOCS-CONVENTIONS.md`。

## 十一、本项目 AI 适配件（仓库自含；通用能力全部在 devflow 插件，零副本）

| 适配件 | 内容 |
|---|---|
| `.zcode/config.json` | 项目门禁（PreToolUse）：手敲 cmake ❌、写产物目录 ❌；脚本在 `.zcode/armory/gates/pre-tool-use.sh`；六角色白名单与 SessionStart 激活由插件 hook 供给 |
| `.zcode/armory/evidence-capture.sh` | 薄委托插件采集器；证据归档 `evidence/`（已 git-ignore），路径写回测试清单卡 |
| `.zcode/cheatsheets/platform-file-map.md` | 平台拆分文件映射（含 ⚠ 无 posix 兜底族），改平台代码前先查 |
| `.zcode/lessons/<role>.md` | 角色经验沉淀：一事一行带日期与来源，与既有规则重复不记 |
| `.claude/settings.json` | Claude Code 侧适配。⚠ 既有双机现状：`ledger-guard.py` / `uat62-whip.py` 两个 hook 写死 Windows 主机绝对路径（`D:/project/gui/DevDesk/...`），Mac 上静默失败（fail-open），属已知不对称、非 bug |

插件源码 `~/dev/zcode-plugins/devflow`（另机 clone `coderstory/zcode-plugins`）；新项目初始化用插件的 `devflow-setup` skill。**改流程改插件，改约束改本文件 §一~§七。**
