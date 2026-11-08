# devflow 审计①：角色定义与流程自洽（qa 面）

- 日期：2026-11-02 ｜ 角色：qa-tester ｜ 池条目：pool:need:75
- 对象：`/Users/coderstory/dev/zcode-plugins/devflow` 下 agents/*.md ×7、skills/{ask,audit,consolidate,req-analysis,tech-solution,pool-usage}/SKILL.md、hooks/session-start.sh（只读审计，对象零改动）
- 尺子：`docs/research/2026-11-01-skill-quality-baseline.md`（主判据 A7 全组、A3.2/A3.5/A3.7/A3.8/A3.10、B 组、C.1-C.7、D.1/D.2/D.7）+ 官方 diagnosing-skills / zcode-configuration-guide 0.3.0
- 下文文件路径均相对插件根 `devflow/`。

## 总判定

**流程骨架自洽（打回机制、串行保证、门禁链均有明文），主要问题集中在角色边界的两处实义矛盾（P1）与跨文件重复定义已发生措辞漂移（P2）。** 无 P0。

---

## 轴① 角色符合性

**[P1] agents/product-manager.md:14 — PM 验收职责同句自相矛盾，且链上无排期，属悬空职责 — 证据：同一条边界行先写「执行：新功能取舍、M/L 级 PRD 与矩阵、spec 修订、优先级、用户视角验收」，同句又写「不执行：……验收 → qa-tester」。两处「验收」无区分定义（用户视角验收 vs 实操验收）；ask/SKILL.md:21 的 L 级链「researcher → product-manager → ui-ux-designer → developer → code-reviewer → qa-tester → consolidate」中 PM 之后无任何验收槽位，「用户视角验收」在全流程无处执行 — 建议：二选一——删除 PM 的「用户视角验收」，或明文定义其与 qa 实操验收的分工（如「PRD 目标达意性判定，非实操」）并在 ask 链上给排期。**

**[P1] agents/product-manager.md:14 × skills/ask/SKILL.md:20 × skills/req-analysis/SKILL.md:3 — M 级 PRD 归属三方矛盾 — 证据：PM 正文宣称「执行：M/L 级 PRD 与矩阵」；ask 定级表 M 行却写「`req-analysis` 出 PRD 骨架 + 矩阵 → ui-ux-designer（UI 类）→ developer」，全程无 PM；req-analysis description 自称「M 级直用，L 级当草稿」。三处无调和条款：M 级需求的 90% 取舍决策实际由主会话经 req-analysis 做出，PM 的「需求端 Owner」名号（product-manager.md:8）与其实际管辖范围（仅 L）不符 — 建议：PM 边界改为「L 级 PRD 与矩阵（M 级由主会话 req-analysis 代产，PM 免）」，或 ask M 行加「PM 复核取舍」一步。**

**[P2] agents/*.md（7/7）— description 压铁律/流程细节，A7.3 方向系统性违例，且同一规则 description+正文双份 — 证据（description 位置均 ：3）：bug-fixer「铁律：复现优先（写不出变红的命令禁止推理根因）……」与其正文 :21 铁律 1 逐字重复；developer「方案卡必出（F-ID）、先找现成……」、qa-tester「测试清单……先于验收存在；验收硬标准 = ……全绿不构成完成」、researcher「外部事实必须来自本轮检索」、code-reviewer 双轴清单、PM「默认只做 90%」、ui-ux「绿色微信风……设计决策不绑实现技术」均为正文规则的复制 — 建议：devflow 派发走 agent-gate 白名单显式指名，description 的路由价值在「职责+何时派」一句话即可（正例：product-manager/ui-ux 的 use proactively+触发问句，A7.3 全组最佳）；规则文本下沉正文，7 条 description 合计约 745 汉字对 A7.4 共享元数据预算（同场 104 技能）是纯负债。**

**[P2] agents/code-reviewer.md:7 — tools 白名单不含 Skill（A3.10），正文未声明「本角色不用技能」；白名单范式 7 角色仅 1/7 — 证据：`tools: [Read, Bash]`（简报已验证：该代理内不能调技能）；正文（:9-26）无「本角色不调技能/无池写权限」声明，其开工门禁因此只能是「确认简报含标记」而非其余 6 角色的 `pool_update` 置进行中，回流仅靠汇报文本——这一差异是有意设计但未写明 — 建议：正文补一句「本角色只读（tools 白名单无 Skill/MCP 写工具），开工登记与回流由主会话代执行」，同时消除 1/7 白名单不一致对 A7.5 路由唯一性的弱化。**

**[P2] 重复工作点与双真相源漂移清单（C.9 同一含义单一真相源违例）— 证据：**
1. **PRD 产出两遍**：PM 流程 5/自查（product-manager.md:21,24）与 req-analysis 步骤 4/5（SKILL.md:10,11）近全文复制，且已漂移——PM 自查有「每个操作：成败各见什么？**可撤销？**」，req-analysis 自查无「可撤销？」；
2. **方案卡 schema 两遍**：developer.md:21 与 tech-solution/SKILL.md:11 的落盘结构「F-ID / 方案与参照 / 涉及文件（含另一平台）/ POC 结论 / 取舍」逐字重复；
3. **POC 规则三遍且措辞已漂移**：researcher.md:15「结论入报告后**删除或注明**」vs developer.md:20 与 tech-solution/SKILL.md:10「用完**删除或转正**」——researcher 可留 POC 残留在报告里，另两处必须清理，口径不一；
4. **bug 终态三层查**（qa:31 跟踪 / ask:53 收口门禁 / consolidate:20 审计）与**成本纪律四处维护**（developer.md:23、qa-tester.md:33、session-start.sh:8，外加工作区 AGENTS.md 第四份跨仓副本）——前者属有意门禁冗余可接受；后者跨两仓四处同步，漂移风险最高。
— 建议：每项定一个权威源（如 POC 口径归 developer/bug-fixer 的执行角色定义，researcher 引用之），其余位置改条件式指向；consolidate 校准收敛五信号（consolidate/SKILL.md:14）应把「跨文件重复」的扫描范围明确覆盖插件内 skill/agent 互拷文本。

轴①其余检查项未发现：description 与正文职责方向一致（无角色挂羊头卖狗肉）；qa/developer 测试维度分工（qa:25 vs developer:23/24）、PM 取舍 vs researcher 调研、bug-fixer vs developer 修复/开发边界均互斥且与 ask 定级表咬合。

## 轴② 流程衔接

**[P2] skills/ask/SKILL.md:38 × agents/ui-ux-designer.md:50 — 「UI 先验验证硬门禁」的验收主体悬空 — 证据：ask「UI 开发前置 = 设计文档+demo 验收通过（矩阵硬检查点）——设计验证不通过不得进开发」未写谁判「通过」；ui-ux-designer 流程 6 写「用户/PM 过目 → developer 照设计文档实现」——用户不在场时 PM 是否有裁决权、「过目」是否等于门禁的「验收通过」均无定义。链上检查点存在但判定者缺位，属潜在「上游未完成下游已开动」口子 — 建议：明文指定主体（建议：主会话按 ui-ux 产出的先验验证清单核对，用户否决权保留），并写进矩阵检查点的操作者。**

**[P2] skills/consolidate/SKILL.md:3 × skills/ask/SKILL.md:20,53 — consolidate 触发口径与 M 级收尾方式不一致 — 证据：consolidate 触发词「功能矩阵全终态、L 级收尾」——M 级走 req-analysis 也产矩阵，矩阵全终态字面满足 consolidate 触发条件；但 ask M 行规定 M 级收尾是「简版沉淀」（:53「M 做简版」），不派 consolidate 会 — 建议：consolidate 触发词改「L 级收尾、或矩阵全终态**且用户点名**」，与 ask 的 M/L 分档对齐。**

轴②其余未发现：串行保证机制在位（ask:43-49「每个子代理返回后物理验证，不信汇报原文」+ 对照表派下一棒；并行仅 audit:25 显式允许「同一条消息内多个子任务调用 = 并行」）；「需求分析未落台账禁止派开发」（ask:35 排期制）+「UI 先验硬门禁」（ask:38）+「方案卡覆盖核对」（ask:49）三道防上游缺位门禁闭合；批次执行护栏（批内超 S 级升级、编排留痕，ask:25-27）闭合；双登记/状态机两步（ask:32）与 pool-usage 状态机（SKILL.md:22）一致。

## 轴③ 打回机制

**[P2] agents/developer.md:11,14 — 「方案卡不可实现」无一般打回条款 — 证据：仅两特例明文——UI 偏离「偏离即回 ui-ux-designer」（:14）、简报事实矛盾「停用该依赖并上报主会话」（:11）；通用「POC 失败/方案不可行 → 打回 PM/tech-solution 重做方案卡」无明文，只能靠输出格式「决策」栏兜底，主会话是否据此打回取决于读报告的自由发挥 — 建议：developer 边界补一句「方案卡经 POC 判不可实现 → 停工上报主会话打回方案产出一方，不得自行改方案实施」。**

**[P2] skills/audit/SKILL.md:34,39 — 两处缺上限/缺出口 — 证据：①「两面结论矛盾 → 并列呈示双方证据，标「待裁决」，不擅自压掉任何一方」未指定裁决人与时限，也未定义存在未决「待裁决」时总判定（可发布/需修改后重审，:36）如何取值；②「修复后重审只派原发现面（增量复审，不全员重跑）」无次数上限——缓解因素：audit 仅限插件自检或用户显式点名（:8），用户即事实出口，但 skill 未写明 — 建议：「待裁决」条目计入总判定强制项（有未决待裁决 → 判「需修改后重审」）；增量复审写明上限（如同 ask:46 的 ≤2 次口径）。**

轴③其余未发现：主机制明文、可执行、有上限、有裁决出口——ask:46「同一子任务打回 ≤2 次，超限升级用户裁决，不无限循环；不代做」、:47 假证据必打回（要求同批全部证据复核）、:48 补卡必复审、:49 方案卡覆盖核对打回计入同一上限；7 角色开工门禁「简报无 `[pool:need/bug:<id>]` 标记 = 简报缺陷，打回主会话，禁开工」条款齐备（bug-fixer:11 / code-reviewer:12 / developer:11 / product-manager:11 / qa-tester:11 / researcher:11 / ui-ux-designer:11）；bug-fixer 不可复现出口（:14「要不到明说『无法定位』，禁止盲改」）与紧急通道（:17-18，紧急只重排顺序不豁免质量）闭合。

## 轴④ 渐进式披露与 token 经济

**[P2] agents/ui-ux-designer.md:13-37 — 风格基线（约 25 行：14 值色板表/组件家族/动效规范）为「出设计时才用」的参考型内容内联 agent 正文，A3.8/C.7 该拆未拆 — 证据：`## 风格基线（绿色微信风）`起 ：13 至动效规范 ：37；门控跳过分支（:46「初析后判微调/纯文案级 → 跳过设计阶段」）也要全量载入这 25 行却分文不用——凡涉 UI 的微调/文案任务每次白付 — 建议：拆 `agents/references/ui-style-baseline.md`（或 skill 形态），正文留条件式指向「进入设计产出前先读风格基线」；正文可从 55 行降至约 30 行，门控跳过派发的恒在成本同步下降。**

**[P2] hooks/session-start.sh:8 — 注入文本末句与插件正文重复，恒在层复述可查内容（C.6/C.9）— 证据：「高效纪律：检索先 CodeGraph、简报带全已知内容、测试只跑受影响域。」分别与 ask:28「简报带全已知内容」、developer.md:23/qa-tester.md:33 定向用例条款重复，且紧接着的下句已声明「流程权威在插件 skills；项目约束以工作区 AGENTS.md 为准」——既已声明权威在 skills，纪律细节应按需查而非每会话重付 — 建议：注入保留四要素：流程同意最高原则、ask 路由一句、7 角色白名单、完成=证据物一句；删「高效纪律」三联句。注入主体（约 230 汉字/会话）量级本身可接受。**

**[P2] 全部 6 个流程 skill — B.4 触发可靠性实测记录缺失 — 证据：无任何 skill 有触发命中率/相邻误触记录；基线 V2 已判行为层为零，本轮逐 skill 复核仍为零（eval-run.sh E 系列均为结构 tripwire）。另 audit description 尾段「提供：按对象选角色视角并发隔离审计 → 去重归一 + 误报复验 → 唯一结论」为流程摘要入 description（D.2 方向），因其为手动专用 skill（显式加载必读正文）风险低，随本项一并观察 — 建议：按基线【怎么测-2】建 10 条盲测（5 目标+3 相邻+2 隐含）覆盖 ask/consolidate/req-analysis 三个高频 skill 先行。**

轴④其余未发现（正面结论）：6 个 skill description 全部在前 250 字符内完成「做什么+何时用」且触发词前置（A3.5/B.1 达标）；负向边界齐备——ask「纯问答与 git 操作不用」、audit「默认禁止自动调用，AI 勿主动调用」+ frontmatter 白名单内无死字段依赖（`disable-model-invocation` 为已验证死字段，其 description 补偿文字正确，属 B.6 正确姿势）、req-analysis/tech-solution「深度竞品/选型全报告 → researcher」、pool-usage「devflow 流程禁用全局池，防串账」；正文行数全部达标（A3.7：最大 ask 53 行 < 500），无 >300 行 reference 无 TOC 问题（D.5 未发现，全插件 skills 无 references 目录，深嵌套引用不存在）；consolidate 预算表（SKILL.md:14「agent ≤70」）下 ui-ux 55 行、qa 35 行均在预算内。A3.2 白名单外键：6 skill 仅 name/description，无白名单外键（agents 的 thoughtLevel/model/tools 为简报已验证的运行时命中键，不作缺陷计）。

---

## 计数

| 级别 | 数量 | 条目 |
|---|---|---|
| P0 | 0 | — |
| P1 | 2 | PM 验收职责自相矛盾+悬空；M 级 PRD 归属三方矛盾 |
| P2 | 10 | ①description 压铁律 7/7；②code-reviewer 白名单声明缺失；③重复定义漂移清单；④UI 先验门禁验收主体悬空；⑤consolidate 触发口径与 M 级不符；⑥developer 方案不可实现无打回条款；⑦audit 待裁决无出口+增量复审无上限；⑧ui-ux 风格基线未拆 references；⑨session-start 注入复述正文；⑩B.4 触发实测缺失 |

## 审计自报

- 实际工具调用：21 次（预算 ≤35，未超）。读对象 14 文件 + 尺子 1 + 官方手册 2 + 目录/行数清点 1 + 开工 pool_update 1（含 ToolSearch 装载 1）+ 本报告落盘 1。
- 对象零改动（只读审计），唯一写入为本报告文件。
