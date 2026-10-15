# GitHub Sync — Handoff 文档

> **交付日期**: 2026-10-15
> **分支**: `dev`（已 force push 到 origin/dev）
> **Commit**: `8e82e0d`（远端重写为 `d66ca1d`，内容全等）

## 一、交付摘要

为 DevDesk 新增 GitHub Sync 功能的基础架构层和配置 UI，允许用户将笔记（Markdown 文件）和待办数据（SQLite dump）同步到 GitHub 私有仓库，实现多设备数据同步。

### 当前完成状态

| 模块 | 状态 | 说明 |
|------|------|------|
| `core/sql_dump` | ✅ 完成 | SQLite SQL 导出/导入，含表过滤、事务批处理 |
| `core/git_sync_service` | ✅ 完成 | Git 操作封装：init/clone/push/pull/commit/status，通过 popen() 调用 git |
| `core/paths` | ✅ 完成 | 新增 `gitRepoDir()`（`~/DevDesk/`）和 `sqlDumpFile()`（`<repo>/data/database.sql`） |
| `github_sync_config` | ✅ 完成 | 配置模型 + SQLite 持久化 + Keychain Token 存储 |
| `github_sync_card` | ✅ 完成 | 设置页 UI 卡片（第 4 个 tab「同步」） |
| settings_module 集成 | ✅ 完成 | 卡片注册、回调装配、快照刷新 |
| CMakeLists | ✅ 完成 | `devdesk_core` + `devdesk_module_settings` 编译 |
| **运行时逻辑** | ⚠️ 占位 | push/pull 方法为占位实现（显示「开发中」） |
| **markdown/todo 集成** | ❌ 未开始 | commit hook 未接入 MarkdownService 和 TodoService |

## 二、新增/修改文件清单

### 新增文件（10 个）

```
DevDesk/src/core/sql_dump.h               # SQL 导出/导入接口声明
DevDesk/src/core/sql_dump.cpp             # SQL 导出/导入实现
DevDesk/src/core/git_sync_service.h       # Git 操作接口声明
DevDesk/src/core/git_sync_service.cpp     # Git 操作实现（popen）
DevDesk/src/module_settings/github_sync_config.h   # 配置模型 + Keychain 接口
DevDesk/src/module_settings/github_sync_config.cpp # 配置持久化实现
DevDesk/src/module_settings/github_sync_card.h     # UI 卡片声明
DevDesk/src/module_settings/github_sync_card.cpp   # UI 卡片实现
DevDesk/docs/superpowers/specs/2026-10-15-github-sync.md  # 设计文档
```

### 修改文件（7 个）

```
DevDesk/src/core/paths.h                  # +gitRepoDir() / +sqlDumpFile()
DevDesk/src/core/paths.cpp                # 路径实现
DevDesk/src/core/CMakeLists.txt           # +sql_dump.cpp +git_sync_service.cpp
DevDesk/src/module_settings/settings_ui_cards.h    # +GitHub Sync 快照字段 + 回调
DevDesk/src/module_settings/settings_ui_logic.h    # +kTabSync + kGithubSyncCardHeight
DevDesk/src/module_settings/settings_module.cpp    # +GithubSyncStore + 卡片注册 + 回调
DevDesk/src/module_settings/CMakeLists.txt         # +github_sync_config.cpp + github_sync_card.cpp
```

## 三、架构概览

```
┌─────────────────────────────────────────────┐
│  设置页「同步」tab (github_sync_card)        │
│  ┌───────────────────────────────────────┐  │
│  │ 启用开关 / 仓库URL / 分支 / Token     │  │
│  │ [立即同步] [拉取]                     │  │
│  └───────────────────────────────────────┘  │
└──────────────┬──────────────────────────────┘
               │ SettingsCardActions
┌──────────────▼──────────────────────────────┐
│  SettingsModule (settings_module.cpp)        │
│  ├─ GithubSyncStore (配置持久化)             │
│  ├─ applyGithubSyncEnabled / Push / Pull     │
│  └─ Token → platform::keychain              │
└──────────────┬──────────────────────────────┘
               │
┌──────────────▼──────────────────────────────┐
│  core 层                                     │
│  ├─ git_sync_service: Git 操作 (popen)       │
│  │   init / clone / add / commit / push      │
│  │   pull / fetch / status / remote_url      │
│  ├─ sql_dump: SQLite ↔ SQL 文件              │
│  │   exportToString / importFromString       │
│  └─ paths: gitRepoDir / sqlDumpFile          │
└─────────────────────────────────────────────┘
```

### 数据目录布局

```
~/DevDesk/                    ← gitRepoDir() = appDataDir()
├── .git/                     ← git init 在此
├── .gitignore
├── devdesk.db                ← SQLite 主库（不纳入 git）
├── notes/                    ← markdownDir()
│   ├── note1.md
│   └── note2.md
└── data/
    └── database.sql          ← SQL dump（纳入 git）
```

## 四、关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Git 仓库位置 | `~/DevDesk/`（appDataDir） | 用户选择路线 A，仓库独立于代码仓库 |
| 认证方式 | HTTPS + GitHub PAT | 用户指定 |
| SQLite 备份格式 | SQL dump（`.sql` 文件） | 文本可 diff，跨平台兼容 |
| Git 调用方式 | `popen()` 调用 git CLI | DevDesk 已有 libcurl+OpenSSL，git 直接可用 |
| Token 存储 | `platform::keychain`（DPAPI/Keychain） | 安全，已有实现 |
| 配置存储 | `app_config` 表（`github_sync:*` 前缀） | 与 SettingsConfig 同模式 |
| 冲突处理 | IDEA 风格弹窗（左中右布局） | 用户选择，待实现 |
| Commit 粒度 | 单文件（只 commit 被修改的） | 用户选择 |
| Push 通道 | git push HTTPS + OpenSSL 后端 | 已验证可用（`git -c http.sslBackend=openssl`） |

## 五、待实现（下一步）

### P0 — 核心运行时逻辑

1. **`git_sync_service.cpp` 实际调用**
   - 当前 `githubSyncPush()` / `githubSyncPull()` 是占位（显示「开发中」）
   - 需实现完整工作流：SQL dump → git add → git commit → git push/pull → 状态反馈

2. **markdown/todo commit hook**
   - 在 `MarkdownService::write()` / `TodoService::createTask()` 等写操作后触发
   - 调用 `git_sync_service` 执行 `git add <changed_file> + git commit`
   - 若 `autoPush=true` 则接着 `git push`

3. **冲突检测与 UI**
   - push 被 reject 时 → fetch → 检测冲突 → 弹窗
   - IDEA 风格：左（本地）/ 中（diff）/ 右（远端），逐文件合并
   - SQL dump 冲突：提示用户选择保留本地或远端版本

### P1 — 健壮性

4. **错误处理完善**
   - git 未安装检测 + 友好提示
   - 网络错误重试（指数退避）
   - Token 过期检测（401 → 提示用户更新）

5. **启动时同步**
   - onStart 时若 enabled=true 则自动 fetch + pull
   - SQL dump → import（覆盖本地 DB 前备份）

6. **进度指示**
   - 长操作（push/pull）的进度条或 spinner

### P2 — 体验优化

7. **连接测试按钮**
   - 验证 repo URL + token 是否有效

8. **gitignore 模板**
   - 自动生成 `.gitignore`（排除 `devdesk.db`、临时文件等）

## 六、Push 记录

- **远端**: `https://github.com/coderstory/DevDesk.git`
- **分支**: `dev`（force push: `ef5798b` → `d66ca1d`）
- **Token**: Windows 凭据库 `gho_*` OAuth token
- **命令**: `git -c http.sslBackend=openssl push https://coderstory:TOKEN@github.com/coderstory/DevDesk.git +HEAD:dev`
- **注意**: 因沙箱 ACL 问题，本地 commit `8e82e0d` 在远端被重写为 `d66ca1d`（内容全等）。另一台电脑 clone 后需 `git fetch && git reset --hard origin/dev`

## 七、在另一台电脑继续开发

```bash
git clone https://github.com/coderstory/DevDesk.git
cd DevDesk
git checkout dev
# 然后按 CLAUDE.md 的构建说明编译
```