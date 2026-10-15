# GitHub Sync — PRD & 技术方案

> DevDesk 数据跨设备同步功能，通过 GitHub Private 仓库同步笔记、待办与数据库。

## 需求概述

用户配置 GitHub 仓库地址 + PAT 后，DevDesk 自动将 `~/DevDesk/` 数据目录下的笔记（`notes/*.md`）和数据库 SQL dump 同步到远程仓库，实现多设备数据同步。

## 已定决策（Grill Round 2-3）

| # | 决策 | 结论 |
|---|------|------|
| 1 | 仓库位置 | 直接在 `~/DevDesk/` 下 `git init`，不新建子目录 |
| 2 | 认证方式 | HTTPS + PAT（Personal Access Token），存入 Windows Credential Store / macOS Keychain |
| 3 | SQL 备份格式 | SQL dump（`database.sql`），commit 前实时导出 |
| 4 | 冲突处理 | 弹窗 IDEA 风格（左中右布局），先冲突列表→再合并弹窗 |
| 5 | Commit 粒度 | 单文件粒度，只 commit 被修改的文件（不 `git add -A`） |
| 6 | 仓库独立性 | 数据仓库独立，不和 DevDesk 代码仓库混用 |
| 7 | Push 通道 | 默认 `git push`（libgit2 / 系统 git），含 `-c http.sslBackend=openssl` 兼容 Windows schannel |
| 8 | 分支 | 用户可自定义远程分支名，默认 `main` |
| 9 | 备份触发 | 绑定 md 的 commit 机制，commit 时顺手备份；也支持手动触发 |
| 10 | 目录结构 | `notes/` 子目录放 md 文件，`database.sql` 放根目录 |
| 11 | Token 存储 | `platform::keychain`：service=`DevDesk/github`，account=`token` |

## 架构设计

### 新增文件

```
src/core/git_sync_service.h        — GitSyncService 接口（core 层，不依赖 platform）
src/core/git_sync_service.cpp      — 实现：git init/clone、add、commit、push、pull、status
src/core/sql_dump.h                — SQLite → SQL 文本导出（纯函数，复用 schema.h）
src/core/sql_dump.cpp
src/module_settings/github_sync_card.h    — 设置页「GitHub Sync」卡片
src/module_settings/github_sync_card.cpp
src/module_settings/github_sync_config.h  — GitHub Sync 配置模型 + 持久化
src/module_settings/github_sync_config.cpp
src/module_settings/github_sync_dialog.h  — 冲突解决弹窗（三栏）
src/module_settings/github_sync_dialog.cpp
```

### 修改文件

```
src/core/CMakeLists.txt            — 添加 git_sync_service、sql_dump
src/core/paths.h / paths.cpp       — 添加 gitRepoDir()、sqlDumpFile()
src/module_settings/CMakeLists.txt — 添加新文件
src/module_settings/settings_module.cpp — 集成 GitHub Sync 卡片 + 触发
src/module_settings/settings_config.h   — 添加 GitHubSyncConfig 到 SettingsConfig
src/module_settings/settings_ui_cards.h — 注册新卡片
src/module_settings/settings_ui_logic.h — 添加卡片常量
src/module_markdown/markdown_service.cpp — write() 后触发 auto-commit
src/module_todo/todo_service.cpp         — 变更后触发 auto-commit
```

### 数据流

```
用户编辑笔记/待办
    → MarkdownService::write() / TodoService::save()
    → 置 dirty flag
    → 下一次 idle timer（2s 防抖）
    → sql_dump::exportToFile(db, sqlDumpFile)   // 实时导出 SQL
    → git add <changed-file>                     // 单文件粒度
    → git commit -m "auto: <file>"
    → git push origin <branch>                   // 异步，不阻塞 UI
    → 失败 → 队列重试 / 用户手动触发
```

### 启动同步

```
onStart / 用户点击「同步」
    → git_sync_service::pull()
    → 成功 → sql_dump::importFromFile(db, sqlDumpFile) → 重建 DB 状态
    → 冲突 → 弹窗让用户选（保留本地 / 保留远程 / 手动合并）
```

### 配置存储

沿用 `app_config` 表，键前缀 `github_sync:`：

| 键 | 类型 | 说明 |
|----|------|------|
| `github_sync:enabled` | bool | 是否启用 |
| `github_sync:repo_url` | string | `https://github.com/user/repo.git` |
| `github_sync:branch` | string | 远程分支名，默认 `main` |
| `github_sync:auto_push` | bool | 是否自动 push（默认 true） |

Token 不存 app_config，走 `platform::keychain`：
- service = `DevDesk/github`
- account = `token`

### Git 操作实现策略

DevDesk 不链接 libgit2（减少依赖）。通过 `popen()` 调系统 `git` 命令：

```
git -C <repoDir> status --porcelain
git -C <repoDir> add <file>
git -C <repoDir> commit -m "msg"
git -C <repoDir> -c http.sslBackend=openssl push <url> <branch>
git -C <repoDir> -c http.sslBackend=openssl pull <url> <branch>
```

URL 由 `repo_url` + PAT 拼接：`https://<token>@github.com/user/repo.git`

### 冲突检测与处理

`git pull --no-rebase` 后检查 `git status --porcelain`：
- 无冲突 → 自动合并完成
- 有冲突（`UU` 状态文件）→ 弹出冲突解决对话框

冲突对话框三栏布局：
- 左栏：冲突文件列表
- 中栏：差异对比（本地 vs 远程）
- 右栏：操作按钮（保留本地 / 保留远程 / 打开文件手动合并）

### SQL Dump 格式

```
-- DevDesk SQLite dump (auto-generated)
-- Generated at: 2026-10-15T12:00:00Z

BEGIN TRANSACTION;

-- Table: todo_columns
CREATE TABLE IF NOT EXISTS todo_columns (...);
INSERT INTO todo_columns VALUES (...);

-- Table: todo_tasks
...

COMMIT;
```

使用 `sqlite3_db_dump()` 或自写 SQL 遍历 `SELECT * FROM <table>` 生成 INSERT 语句。排除 `claude_call_logs`（日志量大、跨设备无意义）、`claude_run_settings`（临时文件记录）。

### 安全考量

- PAT 只存 Keychain/Credential Store，不出现在日志和 SQL dump 中
- `.gitignore` 确保 `devdesk.db` 及其 WAL/SHM 不进入仓库
- Git push URL 包含 PAT 时，命令字符串不在日志中打印完整 URL

## 实现步骤

1. `sql_dump.h/cpp` — SQL 导出/导入
2. `git_sync_service.h/cpp` — Git 操作封装
3. `paths` 扩展 — `gitRepoDir()`, `sqlDumpFile()`
4. `github_sync_config` — 配置模型
5. `github_sync_card` — 设置页 UI 卡片
6. `github_sync_dialog` — 冲突解决弹窗
7. 集成到 `settings_module` + `markdown_service` + `todo_service`
8. 测试 + push 到 dev 分支