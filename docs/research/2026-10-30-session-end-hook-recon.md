# 会话结束 hook 摸底（需-10 核实项）——Claude Code SessionEnd 事件与 payload

- 日期：2026-10-30。走查人：jQuery（前端）。
- 一手来源：官方 Hooks Reference（code.claude.com/docs/en/hooks，已取全文 markdown 存档核对；本文件结论全部可回溯到该页对应锚点）。
- 用途：会话资产库的采集入口选型依据。

## 结论（三条，先行）

1. **事件名 = `SessionEnd`**，会话终止时触发，专为「会话收尾归档」设计；matcher 可按退出原因过滤。
2. **payload（stdin JSON）**：公共字段 `session_id` / `transcript_path` / `cwd` / `permission_mode` / `hook_event_name` + 专有字段 `reason`。`transcript_path` 直指该会话的完整 `.jsonl` 转写文件——**会话资产库要的原文就在这个路径里，hook 只需搬运不需要拼装**。
3. **三条硬约束**：① 无决策控制（不能阻止退出，输出 JSON 字段被丢弃，只有 stderr 会显示给用户）；② 默认超时 **1.5 秒**（退出/`/clear`/`/resume` 时同步等它），资产库做重活必须 `async: true` 或调 `CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS`（上限 60s）；③ matcher 词表 = `clear` / `resume` / `logout` / `prompt_input_exit` / `other`（`bypass_permissions_disabled` 已于 v2.1.234 移除，配了也不再发）。

## 事件名与时机（资产库相关全集）

| 事件 | 时机 | 对资产库的意义 |
|---|---|---|
| `SessionEnd` | 会话终止（clear/resume/logout/输入框退出/其他） | **主采集点**：transcript 落盘终态 + 原因 |
| `Stop` | Claude 每轮答完（每轮都发，非会话级） | 轻量增量采集点（不等同会话结束，别混用） |
| `SessionStart` | 会话开始/恢复（matcher：startup/resume/clear/compact/fork） | 资产库「会话开始」记账 + 恢复场景补挂 |
| `PreCompact` / `PostCompact` | 压缩前后（matcher：manual/auto） | 压缩是 transcript 内容分水岭，归档需记 compact 事件 |

## SessionEnd payload 实样（官方原文）

```json
{
  "session_id": "abc123",
  "transcript_path": "/Users/.../.claude/projects/.../00893aaf-....jsonl",
  "cwd": "/Users/...",
  "hook_event_name": "SessionEnd",
  "reason": "other"
}
```

matcher 过滤的就是 `reason`：`clear`（/clear 清空）、`resume`（/resume 切换）、`logout`、`prompt_input_exit`（输入框可见时退出）、`other`。

## 会话资产库的落地建议

1. **采集**：配置 `SessionEnd` command hook（`matcher` 不限或按需），handler 读 stdin 拿 `transcript_path` + `reason`，把 jsonl 拷/索引进资产库；`reason` 入库做会话分类维度。
2. **超时**：搬运小文件 1.5s 默认够；要解析/上传/检索入库就 `async: true`（后台执行不阻塞退出）——文档明确 async hook 不受退出阻塞。
3. **不可靠面**：进程被强杀（kill -9 / 断电）SessionEnd 不发 → 资产库需容忍漏采（可在下次 `SessionStart` 时对 `~/.claude/projects/**` 做增量对账补漏）。
4. **版本注意**：本机 Claude Code 版本需 ≥ v2.1.268 才能让 `CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS` 同时抬高单个无 timeout hook 的时限（旧行为只抬总预算）。

## 本机现状核实

- 用户级 `~/.claude/settings.json`：**hooks 为空对象**，无既有 SessionEnd 配置，资产库可独占该事件无冲突。
- DevDesk 仓 `.claude/settings.json`：只有 PreToolUse/PostToolUse 两条 discipline hook，无 SessionEnd——同样无冲突。
