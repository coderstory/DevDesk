# 调研成果索引（RESEARCH-INDEX）

> 单一登记源：每份落盘调研文档在此登记一行（日期 | 类型 | 主题 | 一句话结论 | 路径）。查重先查这里。

| 日期 | 类型 | 主题 | 一句话结论 | 路径 |
|---|---|---|---|---|
| 2026-10-27 | 实现复审 | MCP 骨架 B1+B2 自研「睁眼看世界」四维对照（html_to_markdown / SSRF / Streamable HTTP / 超时截断） | 站得住不打回：协议面 MUST 全对齐、超时截断最优；3 个 major 待收口批（IP 编码绕过+::/96、无会话头请求未 400、markdown 转义与嵌套有序列表缩进），自研方向在零依赖约束下成立但缺 turndown 转义表/缩进规则 | docs/superpowers/research/2026-10-27-mcp-implementation-review.md |
| 2026-10-27 | 实现复审·增量 | MCP 第二轮:B3 装配/排他绑定/安全收口权威参照/SSRF 第三遍/架构五维 | B3 装配与业界生命周期同构无 major;EXCLUSIVEADDRUSE 即微软 MUST、TIME_WAIT 代价业界常规;给收口批权威口径(标点转义合法超集、4 空格缩进、尾点 fail-closed)+新发现尾点绕过(1 行修);SSRF 防线除尾点外闭合;架构五维=精简优/高效优/可插拔合格/独立性合格/声明式欠佳(B5 演进路径已给,不阻塞 B4) | docs/superpowers/research/2026-10-27-mcp-round2-review.md |
