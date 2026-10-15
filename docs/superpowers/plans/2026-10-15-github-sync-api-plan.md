# GitHub Sync（API 版）实施计划 — 票与阻塞边

> 依据：`docs/superpowers/specs/2026-10-15-github-sync-api-design.md`（含 §2.1 审计补正）
> 基线：`dev` @ `de456cb`（+ 本批文档提交）

## 票

| 票 | 内容 | 阻塞边（依赖） | 状态 |
|---|---|---|---|
| **T1** | `core/sql_dump` 补索引/触发器导出（F2）+ skip 表常量（F4）+ `tests/test_core/test_sql_dump.cpp` 回环断言 | 无 | 待派 |
| **T2** | `platform/github_api`（客户端 + 重试 + 环形日志 + testConnection 建临时分支 + `ErrorCode::Conflict`）+ `tests/test_platform/test_github_api.cpp` | 无 | **在飞** |
| **T3** | UI 层：`git_error_dialog`（含 409 变体）+ `github_sync_card`（测试连接/进度/错误行/详情/三键禁用）+ 卡片测试 | 无（接口按 spec 冻结） | **在飞** |
| **T4** | 同步编排（`settings_module`）：sha 状态机（F1）、5 分钟节流、首次方向、删除传播、备份 5 份（F5/F6）、本地 git 可选探测（F8）、暂停/重载（F3） | T1、T2、T3 | 待 T1–T3 |
| **T5** | 收尾：全量构建 + 全量测试 + `/code-review` 独立复查 + 装 `/Applications/DevDesk.app` | T4 | 待 T4 |

## 交付纪律（每票）

- worktree 隔离（跨文件不交叠的票可并行）；原子 commit、显式 pathspec、禁 `git add -A`
- 定向测试优先（`bash scripts/build-test-run.sh --filter <关键词>`）；T5 才跑全量
- 报告：sha + 改动文件与行数 + 定向 filter 的 Total/Passed/Failed + 已知未验项（≤12 行）
- 不合并、不 push（主会话串行合并 + 推送决策）

## 已知边界

- Windows 侧只到交叉编译守卫 + CI 编译级；**运行时未验**（无 Windows 机器）
- 真网络验收需用户提供私有仓 + PAT（本机可跑，凭据由用户提供）
- 冲突三栏合并弹窗、非 GitHub 托管不在本批（spec §9）
