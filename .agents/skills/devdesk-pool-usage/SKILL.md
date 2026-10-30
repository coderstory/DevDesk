---
name: devdesk-pool-usage
description: 三池工具指南：折入 §6.5 表结构规范与软删纪律
---

# devdesk-pool-usage：三池工具面使用指南（所有 Bot 挂载）

> **ZCode 侧挂载现状（2026-10-31）**：devdesk-pool MCP server 已注册于全局 `~/.zcode/cli/config.json` 的 `mcp.servers`（stdio 跑 `tools/.pool-mcp-venv/bin/python tools/pool_mcp.py`），新会话自动挂载；ZCode 会话内工具名为 `mcp__devdesk-pool__pool_add` 等带前缀形态（下文速查表里的短名 `pool_add` 即对应它）。dsh 侧 capability 同名共存，两边打同一个 `docs/pool/pool.db`。

## 表结构规范（§6.5，所有表强制）
- **审计四件套**：id（自增）/ created_by（创建人=入池 owner）/ updated_by（每次写自动刷，`pool_update` 传 `by=你的Bot名`）/ deleted（软删标记 0/1）——全部 MCP 工具层自动维护，人肉不管。
- **软删纪律**：删除走 `pool_soft_delete(id, by=自己)`，默认所有读路径不再返回，数据保留可审计回溯；**禁止直写 SQL**（绕工具=绕状态机+审计+快照，属违规；例外须报备，直写后必须跑 `python3 tools/pool_ops.py sync` 重生成报告/快照）。

## 何时用
- 「需求/bug/待办」要**入池、捞取、更新状态、查重、溯源、查自己的账、按需求查 BUG**时，一律走 devdesk-pool MCP 工具。
- 开工前查重（铁律）：先 pool_search 关键词，命中同类不重复入池。
- 派单/排产从池捞取：pool_fetch（默认按 P 级,id 排序）。

## 个人 TODO（todo_set / todo_get）
- 写：`todo_set(owner=自己的Bot名, content="今晚要做的事", status="进行中")` —— upsert，每人一条（未删行间互斥，软删后可重写）。
- 读：`todo_get(owner=自己)` / `todo_get()` 全池。

## 身份绑定（条件式）
**只有用户意图明确是「查自己名下」时才加 `owner=自己`**（"我还剩几个 bug"/"我的在池条目"）；查全池/查他人/排产捞取不加，防过度过滤。无歧义默认不加。

## 按需求查 BUG（need_id 外键，与身份绑定独立）
- 全部 BUG：`pool_fetch(need_id=X)`；未处理完：`+ status="在池"`；全链：`pool_trace(X)`；挂 BUG：`pool_update(id=X, bug="...")`。

## 工具速查（MCP server 名：devdesk-pool）
- pool_add(pool: need|bug|debt, title, priority, owner: 自己, detail?) → {id}
- pool_fetch(priority?, status?, pool?, owner?, need_id?)；status 全集：在池/排产/进行中/待验收/关闭/挂起/拒绝
- pool_update(id, status?, priority?, owner?, blocker?, note?, bug?, evidence?, reason?, detail?, bug_resolved?, by=自己)
  状态机：在池→排产|挂起|拒绝；排产→进行中|挂起；进行中→待验收|挂起；待验收→关闭|进行中；挂起→在池。
  关闭须带 evidence 且子 BUG 全部处理完（bug_resolved 标记）；拒绝必填 reason；挂起必填 blocker。
- pool_search(keyword) / pool_stats(owner?, pool?) / pool_trace(id) / pool_soft_delete(id, by)
- todo_set(owner, content, status?) / todo_get(owner?)

## 典型流程
1. 派单前 pool_search 查重 → pool_add（owner=自己）。
2. 开工 status=进行中；进展 note；发现 BUG bug="..."。
3. 交付 status=待验收 → QA 过关 status=关闭 + evidence。
4. 卡住 status=挂起+blocker；解除 status=在池。
5. 每天开工 todo_set 写一条今晚目标；用户问自己时 pool_stats(owner=自己)。

## 环境与兜底
- MCP 不可用：报派单方/老板娘挂接入；紧急只读 `sqlite3 docs/pool/pool.db "..."`（写禁）。
- venv 坏了：`python3 tools/pool_ops.py reinstall-venv`。
- 直写 SQL 报备后：必跑 `python3 tools/pool_ops.py sync`（裸 python 可跑，自动注入 mcp 桩）。
- 跨机部署/备份还原：tools/README.md。
