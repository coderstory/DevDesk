# ADR 0008：MCP 全协议决策反转——从「不做」到自建 server

- **日期**：2026-10-30
- **状态**：Accepted（追记：决策反转发生于 2026-10-29 module_mcp 交付；本 ADR 为补录，史密斯专员决策交叉核对 2026-10-30 发现路线图未更新，见 `docs/plans/decision-crosscheck-2026-10-30.md` #5）
- **决策方**：三池 MCP 立项拍板（`docs/superpowers/specs/2026-10-29-三池mcp决策文档.md`）
- **相关**：`docs/strategy/05-技术路线图.md` §四（原「明确不做」行已标记反转）

## 背景

路线图「明确不做」曾列「MCP 全协议（自己实现完整的 MCP 协议栈）」，理由：当时工具约 21 个，不需要 MCP 协议的动态发现和协商层。该判断在成文时点成立。

## 决策反转

2026-10-29 module_mcp 完整交付（`src/module_mcp/mcp_protocol.cpp` + `mcp_http_handler.cpp` 自建 MCP server，`src/core/mcp_tool.cpp` 工具缝），反转的驱动因素：

1. **需求侧变化**：三池（记忆/会话资产/收集箱）立项后，外部智能体（DeepSeek CLI 等 MCP 客户端）直连 DevDesk 数据成为硬需求——没有标准协议层，每个外部方都要定制对接。
2. **成本侧变化**：协议栈以「成熟方案搬运」落地（协议解析 + HTTP handler 自建但格式对齐官方 spec），维护面远小于当初「自己发明一套」的担忧。

## 影响

- 路线图该行标记反转并指向本 ADR；「明确不做」清单其余各行不受影响。
- 后续 MCP 相关能力（新工具暴露、三池接入）按 module_mcp 既有缝扩展，不再重新讨论「做不做协议」。
