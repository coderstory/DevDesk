# 调研成果索引（RESEARCH-INDEX）

> 单一登记源：每份落盘调研文档在此登记一行（日期 | 类型 | 主题 | 一句话结论 | 路径）。查重先查这里。

| 日期 | 类型 | 主题 | 一句话结论 | 路径 |
|---|---|---|---|---|
| 2026-10-27 | 实现复审 | MCP 骨架 B1+B2 自研「睁眼看世界」四维对照（html_to_markdown / SSRF / Streamable HTTP / 超时截断） | 站得住不打回：协议面 MUST 全对齐、超时截断最优；3 个 major 待收口批（IP 编码绕过+::/96、无会话头请求未 400、markdown 转义与嵌套有序列表缩进），自研方向在零依赖约束下成立但缺 turndown 转义表/缩进规则 | docs/superpowers/research/2026-10-27-mcp-implementation-review.md |
| 2026-10-27 | 实现复审·增量 | MCP 第二轮:B3 装配/排他绑定/安全收口权威参照/SSRF 第三遍/架构五维 | B3 装配与业界生命周期同构无 major;EXCLUSIVEADDRUSE 即微软 MUST、TIME_WAIT 代价业界常规;给收口批权威口径(标点转义合法超集、4 空格缩进、尾点 fail-closed)+新发现尾点绕过(1 行修);SSRF 防线除尾点外闭合;架构五维=精简优/高效优/可插拔合格/独立性合格/声明式欠佳(B5 演进路径已给,不阻塞 B4) | docs/superpowers/research/2026-10-27-mcp-round2-review.md |
| 2026-10-27 | 审计 | MCP 服务骨架 B1+B2+B3 提交段（4bccde3c..a028fb1e） | 有条件通过：2 major（SSRF 字面校验可被 userinfo/短拼 IPv4/v6 zone-id 绕过；台账 §3/§4 未回填且 B2 提交点门禁实红）+ 3 minor | docs/superpowers/plans/2026-10-27-mcp-b1b2b3-audit.md（feat/mcp-server-skeleton worktree） |
| 2026-10-27 | 调研 | MCP 网页正文主干提取（boilerplate removal）最小规则集 | 业界收敛三类信号（语义标签/类名关键词、链接密度、块文本量），单趟扫描器逐标签逐块即可移植；规则集 R1+R1b+R2 必做、R3 主容器锚定建议、R4 无障碍可选，阈值取保守端，规则全不命中时输出与现状完全一致 | docs/superpowers/research/2026-10-27-mcp-content-extraction-review.md |
| 2026-10-28 | 审计 | MCP 代理参数批+CA 批（eab2c496/cf990da3）六维独立审计 | 有条件通过：主路径与测试断言站得住；最重 = tcpReachable 未钉扎 env 代理致探测失真(http_client.cpp:154)+CA 接线零断言假绿风险+sendStreaming 忽略 Request.proxy，计 7 findings+2 informational | docs/superpowers/plans/2026-10-27-mcp-proxy-ca-audit.md |
| 2026-10-28 | 实证·终验 | MCP 菜单页 UI 终验（门控/侧栏/菜单页/弹窗试一下/三路关闭/开关联动，真窗口真操作+外部 curl 探活） | 可交付：场景 6/6、两轮 37 断言 0 FAIL；GET /mcp=405、关=连接拒绝、再开恢复 405、tools/list 与卡片集合一致；附 1 非阻塞瑕疵（弹窗 schema 文本溢出压「运行结果」标题） | docs/superpowers/plans/2026-10-28-mcp-menu-ui-verify.md |
