# MCP 服务骨架 · 重启前交接（2026-10-27）

> 一句话现状：spec 与台账已冻结，B1 源码已提交（`4bccde3c`）但**构建/测试未跑**；B1 按 30 分钟时限人工停止，B2-B5 未开始。
> 目标硬门禁：真实 Claude Code 连接真实 DevDesk 进程，握手 → `tools/list` → **真实调用 `web_fetch_markdown` 抓取真网页**且输出含目标网页正文标记；mock / 进程内假客户端 / 只握手不调工具都不算完成。不 push、不出安装包、不自动改 Claude Code 配置。

## 1. 现场状态

- worktree：`D:/wt-mcp-skeleton`；分支 `feat/mcp-server-skeleton`；HEAD 以 `git log -1` 为准（交接提交；其下源码快照 `4bccde3c`）；工作区干净（`build/` 被 gitignore）。
- 提交链：`5c952d7e`（dev 基线，已并入 origin/dev CI 修复）→ `dcd29b53`（spec 初稿）→ `c35a3bcf`（审计修订）→ `29e199a8`（复审收口）→ `f1ef5253`（B1-B5 台账）→ `4bccde3c`（B1 源码 WIP，21 文件 +2260 行）。
- 设计权威：`docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md`；可核销清单：`docs/superpowers/plans/2026-10-26-mcp-server-skeleton-plan.md`。

## 2. 已完成

- spec 三版 + B1-B5 冻结台账（提交见上）。
- B1 源码已提交：`src/core/mcp_port.h`、`src/core/mcp_tool.h/.cpp`；`src/module_mcp/`（CMake / session_store / protocol / http_handler / service / tools_builtin，内置工具仅 `devdesk_server_info`）；四个测试文件；root / core / tests 的 CMake 接线。
- **构建/测试未跑，不能视为通过**：worktree 首次 configure 成功（`D:/wt-mcp-skeleton/build` 已生成 `build.ninja`），冷构建进行中被重启打断；日志 `/tmp/b1-build.log`。台账 B1-01..B1-11 标 🔵，B1-G 仍 ⬜。

## 3. 重启后第一批命令（按序）

```bash
cd D:/wt-mcp-skeleton
git status                       # 应干净；HEAD 以 `git log -1` 为准（交接提交；其下源码快照 4bccde3c）
tasklist | grep -i devdesk       # 有实例先确认是否本 build 目录实例，勿误杀用户/其它会话
bash scripts/build-test-run.sh --filter mcp --no-run
# 若构建目录被重启损坏，再追加 --clean 重跑一次
```

- 绿：按台账 B1-G 勾 ✅ 并提交，证据写「命令原文 + 构建成功行 + 筛中用例名 + 退出码 + sha」。
- 红：按 spec §3/§5 修 B1，不扩到 B2-B5 范围；每批最多一次构建（见台账 §1）。

## 4. 剩余批次入口（细节见台账章节，不在此展开）

- B2 抓取与 HTML→MD：台账 §3（`html_to_markdown` + `web_fetch_markdown`）。
- B3 设置/桥/生命周期：台账 §4（设置卡、`mcp_service_bridge`、`main` / `app_assembly` 装配）。
- B4 live 与真实 Claude Code E2E：台账 §5（`mcp_live --check` + `--strict-mcp-config` E2E 硬门禁）。
- B5 审计/合并/dev 全量门禁/worktree 清理：台账 §6。
- 每批收尾显式 pathspec commit；定向 `--filter mcp`，B3 另跑一次 `--filter settings`（复用同一构建产物，不二次构建）。

## 5. 关键风险 / 注意

- SSRF：仅 http/https，字面量全表**每跳**校验；手动跟随 ≤5 跳、**总时限 10s**。
- JSON：所有字符串（URL/正文/错误信息）必须经 JSON 工厂转义；输出须良构 UTF-8。
- 编码：Content-Type/charset 白名单外报错、白名单内 sanitize；2 MiB 截断附注。
- 设置 UI：`contentKey` 追加 `(mcpOn ? "M" : "m")`；新增 `CardHeightKind::McpCard` 与 switch，不写死卡高。
- live：Windows/macOS 均可编译；B4 用独立 `if(DEVDESK_BUILD_LIVE_TOOLS)` 块（无 `AND UNIX`）。
- 真实 E2E：必须 `--strict-mcp-config` + 临时配置；不碰用户 `~/.claude*`。
- 不赶工：小步验证、每批一次构建；动 DevDesk 前先 `tasklist` 确认进程归属。
- EUI-NEO 在 `D:/project/gui/EUI-NEO`（构建脚本自动定位；必要时显式 `EUI_NEO_DIR=D:/project/gui/EUI-NEO`）。

## 6. 建议技能

- `superpowers:executing-plans`（或 `superpowers:subagent-driven-development`）
- `superpowers:test-driven-development`
- `superpowers:verification-before-completion`
- 交接本身可用 handoff 流程。

## 7. 敏感信息

无。本轮不得写入 token / key / 凭据。