# 04 · 执行记录呈现口径待复核（原口径与静态证据冲突）

Status: `needs-triage`
建议优先级: 低（**先复核，可能直接撤销**）
来源: handoff §3.C「C2」（原文：「`agent_messages` 表有工具级留痕但 UI 不呈现」）+ 2026-10-17 静态复核**未能复现**

## 现象

原始口径断言「工具级留痕落库了但 UI 不呈现」。静态复核**不支持**该断言 —— 渲染分支与落库链路都在。

## 证据（与原型口径相反）

- 落库：`src/core/schema.h:247` `agent_messages` 表存在；`src/module_agent/agent_store.cpp:306` INSERT 含 `tool_call_id`；`:339` 读回时 `:365-369` 解析可空 `tool_call_id`
- 解码：`src/module_agent/message_blocks.h:20-21` 定义 `ToolUse` / `ToolResult` 块类型；`:4`/`:16` 说明 tool 回喂与 input 原文都存在 `text` 里
- **渲染分支存在**：`src/module_agent/agent_module.cpp:1145 case MessageBlock::Type::ToolUse`、`:1157 case MessageBlock::Type::ToolResult`（另有 `:1129-1130` Text/Plain）
- 入口通路存在：`agent_module.cpp:925 store_->listRuns(task.id, kRunListLimit)` → 详情视图 `detailMessages_` → `:1061 decodeMessageBlocks(msg.contentJson)` → `:1061` 起的渲染循环
- 角色徽章也区分工具：`message_blocks.h:72`「含 tool_result 块（落库 role 是 user）= 工具」

## 待复核的两种可能

1. **原口径已过期**：渲染分支是后续补上的，审计矩阵取的是更早的快照 → **票可直接撤销**。
2. **口径指的不是「有没有渲染」而是「渲染得够不够用」**：例如
   - 工具块是否默认折叠、用户能否不点开就看到
   - `kRunListLimit` 是否把历史截断到用户看不到更早的 run
   - 是否只有「当前/最近」run 可点开
   - 工具 input 原文的摘要+截断是否让关键参数不可见（`message_blocks.h:16` 提到「渲染层自做摘要+截断」）

## 修复方向

**先复核再定**：静态只能查到「分支存在」，判断「用户视角够不够用」必须走真机 UI（handoff §3.D 走查计划）。
- 若属可能 1 → 撤销本票，并在矩阵里把 C2 改判 ✅ 并注明「原口径过期于 <提交>」
- 若属可能 2 → 收敛成具体条目（如「工具块默认折叠」）另开票

## 验收判据

给出复现或反证：真机走查后本票要么撤销，要么收敛为一条有复现步骤的具体缺口。
