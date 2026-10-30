
# RESEARCH-INDEX 调研与分析成果索引

> **单一登记源**：researcher / investigator / auditor 落盘的每份文档必须在此登记一行——不登记 = 未归档。
> **查重先行**：一切调研 / 分析类派单前先查本册，已覆盖的课题不重复调研、不重复联网。
> **分工**：战略基线级调研报告（`docs/strategy/` 01–13）由 [`00-总览索引.md`](strategy/00-总览索引.md) 管，本册不重复登记；本册管任务级成果。
> **时效**：条目对应文档超 30 天未核对 → 文档移入 `docs/archive/` 并从本册摘除（沿 CLAUDE.md「状态文档时效约定」）。

**登记格式**（一行一条，最新在上）：

```
- YYYY-MM-DD | 类型(调研/调查/审计) | 主题 — 一句话结论 | 路径
```

---

## 登记簿

- 2026-10-30 | 调研 | 需求场景支撑（老板 10-30 场景化材料整理）：记忆 4 / MCP 3 / 智能体 4 共 11 场景按「现状可用/差哪块」分层 | 三缺口收口：G-1 infer 真调（记忆原文直存，提炼模型从未真调）/ G-2 笔记 MCP（业务数据零暴露）/ G-3 cron 模板（= 需-8 S 级，第四出厂模板）；随行需-9 用量告警 S 级；附一天时间线；两份 spec 草案（需-8/需-9）同批落 docs/specs/ | docs/superpowers/research/2026-10-30-需求场景支撑.md
- 2026-10-29 | 调研 | engram 存储检索底座选型（池子系统输入） | 一手核实 7.68k★/MIT/SQLite+FTS5/MCP 27 工具；schema 精读判定：migrate 上游独占不可扩表、无自由 metadata 列、关系网绑定 observations——engram 适合作 Agent 记忆层，池子业务实体主存储应自建 pool.db（双库或纯自建待业务方案定） | docs/research/2026-10-29-engram-storage-selection.md
- 2026-10-28 | 调查 | memcpp S2 记忆模块 live 层验证 — venv/引擎对账、llama 联动、三视图截图、key 红线；4 处修复 1331450c/90cf043f/473204ff/00ac9931 | docs/superpowers/plans/2026-10-28-memcpp-s2-live-verify.md
- 2026-10-28 | 调查 | memcpp 端到端联调验收 — S3+S4 遗留 live 对账，5 处缺陷修复（MCP 会话缝/update 形参/delete 遗忘映射/UUID id 回填/融合段解析），Recall@5=100%，hook 真跑+ledger 闭环+fail-open | docs/superpowers/plans/2026-10-28-memcpp-e2e-verify.md

## 存量补登欠账

- `docs/superpowers/plans/` 存量约 157 份文档（audit / research 混杂）先于本册存在、未经索引，补登为待派任务。**补登完成前，查重除本册外必须辅以 `grep` / 目录扫描 `docs/superpowers/plans/`**，不得只查本册就判定"未调研过"。
| 日期 | 类型 | 主题 | 一句话结论 | 路径 |
|---|---|---|---|---|
| 2026-10-27 | 实现复审 | MCP 骨架 B1+B2 自研「睁眼看世界」四维对照（html_to_markdown / SSRF / Streamable HTTP / 超时截断） | 站得住不打回：协议面 MUST 全对齐、超时截断最优；3 个 major 待收口批（IP 编码绕过+::/96、无会话头请求未 400、markdown 转义与嵌套有序列表缩进），自研方向在零依赖约束下成立但缺 turndown 转义表/缩进规则 | docs/superpowers/research/2026-10-27-mcp-implementation-review.md |
| 2026-10-27 | 实现复审·增量 | MCP 第二轮:B3 装配/排他绑定/安全收口权威参照/SSRF 第三遍/架构五维 | B3 装配与业界生命周期同构无 major;EXCLUSIVEADDRUSE 即微软 MUST、TIME_WAIT 代价业界常规;给收口批权威口径(标点转义合法超集、4 空格缩进、尾点 fail-closed)+新发现尾点绕过(1 行修);SSRF 防线除尾点外闭合;架构五维=精简优/高效优/可插拔合格/独立性合格/声明式欠佳(B5 演进路径已给,不阻塞 B4) | docs/superpowers/research/2026-10-27-mcp-round2-review.md |
| 2026-10-27 | 审计 | MCP 服务骨架 B1+B2+B3 提交段（4bccde3c..a028fb1e） | 有条件通过：2 major（SSRF 字面校验可被 userinfo/短拼 IPv4/v6 zone-id 绕过；台账 §3/§4 未回填且 B2 提交点门禁实红）+ 3 minor | docs/superpowers/plans/2026-10-27-mcp-b1b2b3-audit.md（feat/mcp-server-skeleton worktree） |
| 2026-10-27 | 调研 | MCP 网页正文主干提取（boilerplate removal）最小规则集 | 业界收敛三类信号（语义标签/类名关键词、链接密度、块文本量），单趟扫描器逐标签逐块即可移植；规则集 R1+R1b+R2 必做、R3 主容器锚定建议、R4 无障碍可选，阈值取保守端，规则全不命中时输出与现状完全一致 | docs/superpowers/research/2026-10-27-mcp-content-extraction-review.md |
| 2026-10-28 | 审计 | MCP 代理参数批+CA 批（eab2c496/cf990da3）六维独立审计 | 有条件通过：主路径与测试断言站得住；最重 = tcpReachable 未钉扎 env 代理致探测失真(http_client.cpp:154)+CA 接线零断言假绿风险+sendStreaming 忽略 Request.proxy，计 7 findings+2 informational | docs/superpowers/plans/2026-10-27-mcp-proxy-ca-audit.md |
| 2026-10-28 | 实证·终验 | MCP 菜单页 UI 终验（门控/侧栏/菜单页/弹窗试一下/三路关闭/开关联动，真窗口真操作+外部 curl 探活） | 可交付：场景 6/6、两轮 37 断言 0 FAIL；GET /mcp=405、关=连接拒绝、再开恢复 405、tools/list 与卡片集合一致；附 1 非阻塞瑕疵（弹窗 schema 文本溢出压「运行结果」标题） | docs/superpowers/plans/2026-10-28-mcp-menu-ui-verify.md |

| 2026-10-29 | 调研 | 现成美观流体效果 C++/GLSL 轮子（背景美化选型） | 一句话结论：无「拿来即用」的 C++ 成品；最优路=直接搬 PavelDoGreat/WebGL-Fluid-Simulation（16.7k★，MIT）的 GLSL shader 文件进 EUI-NEO shadertoy 缝；备选 bassicali/inkbox（59★，MIT，GPU Gems 稳定流体，C++/现代 OpenGL，染料墨水观感，shader 可拆）；kamindustries/touchFluid（238★，MIT）为 TouchDesigner 变体仅参考；tunabrain/gpu-fluid 上轮已封存不重复。Shadertoy 站单文件高赞流体未能二源核实具体条目，不列。 | docs/superpowers/research/2026-10-29-fluid-glsl-wheels.md |
| 2026-10-30 | 审计 | docs/ 全仓文档时效审计（341 份按 30 天口径） | 3 条已确认失实（EUI-NEO-CAPABILITIES.md:43/355-366/628 托盘菜单「不可行」——app.h:108 已有 trayMenu API）+CLAUDE.md 同款失实 1 条；待验证：fontWeight 切 face；归档候选仅 EUI-NEO-PR-PLAN.md（09-21）；其余顶层/strategy/specs 均未过期或有保留理由 | docs/superpowers/research/2026-10-30-docs-freshness-audit.md |
| 2026-10-30 | 摸底 | EUI-NEO 全局快捷键能力现状（需-12 收集箱核实项） | EUI-NEO 现状无全局 OS hotkey：键盘事件唯一入口=GLFW key callback（window_input_backend.cpp:238，仅聚焦投递），全仓无 RegisterHotKey/CGEventTap/addGlobalMonitor 实现，能力盘点 ❌ 记载仍准确。落地两条：EUI-NEO fork 加 hotkey 缝反哺（推荐，约 1 天，Win RegisterHotKey+WM_HOTKEY / mac addGlobalMonitor 需辅助功能权限）或 DevDesk platform 层自建；无三方依赖 | docs/superpowers/research/2026-10-30-global-hotkey-capability.md |
| 2026-10-30 | 调研 | 会话结束 hook 摸底（需-10）：Claude Code SessionEnd 事件与 payload | 事件名 SessionEnd、matcher=退出原因五值；payload=公共字段+reason，transcript_path 直指会话 jsonl 原文；硬约束=无决策控制+默认 1.5s 超时（async/环境变量可放宽）、强杀不发事件需对账补漏；本机用户级与仓级 hooks 均无 SessionEnd 配置零冲突 | docs/research/2026-10-30-session-end-hook-recon.md |
| 2026-10-30 | 调研 | 需求↔BUG 关联与状态联动最佳实践（Jira/Linear/GitHub 一手文档） | 已覆盖：需求↔BUG 父子外键≈sub-issues、按需求聚合查询、status_log 溯源；有差距 2 点（BUG 全关→父可关的 server 校验提示、兄弟条目间 blocked-by 依赖）；不必抄：多层嵌套/Jira 17 种 link 类型矩阵 | docs/superpowers/research/2026-10-30-issue-relation-practices.md |
| 2026-10-30 | 调研 | zcode bridge 两需求技术分析（需-14/15） | 三病灶在 fork 现源逐一实锤（app-server.js:1397 懒 stop/1391 reasoning 无尾缓冲/1429 收尾不同源），存量 patch git apply --check 通过、真会话三场景回归后半天级；结论=fork 自修不等 upstream；需-14 半天级，先做作需-15 验证前置 | docs/superpowers/research/2026-10-30-zcode-bridge-技术分析.md |
| 2026-10-30 | 调研 | 需-16 供应商自动切换技术面 | 切换点实锤=zcode 层 ~/.zcode/v2/provider_config.json 的 defaultModelSelection 一个字段（免费 start-plan↔付费套餐），DSH/bridge 不用动；探测=最小真实回合+失败报文判定+重试防误切；零新依赖半天级；权限授权仍前置卡点 | docs/superpowers/research/2026-10-30-provider-auto-switch-技术面.md |
| 2026-10-30 | spec | 需-16 供应商自动切换合成 spec（实现任务书） | 授权来源=老板娘依在案授权代批全自动版；切换点 provider_config.json 单字段+最小回合探测+重试防抖+原子写回+告警兜底；V-01~V-05 五条验收含真实切换两向验证 | docs/specs/2026-10-30-需16-供应商自动切换-spec.md |
| 2026-10-30 | 调研 | 看板 v2 数据层现成轮子（拿来主义先行） | Datasette（simonw/datasette，11,528★，Apache-2.0，10-23 在更）开箱即给 SQLite 只读 JSON API+任意列筛选，可顶替手搓只读端点或作其对照；未本地安装（pip 一条）；成本对比与取舍见群内结论，采纳再落 spec | （群内快答，未落独立文档）|
