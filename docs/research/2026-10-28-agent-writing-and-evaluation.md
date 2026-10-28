# 调研报告：如何写好 agent、如何评价评测 agent

日期：2026-10-28 · 调研方式：多轮外部检索（WebSearch / gh / webReader / WebFetch）· 结论已交叉验证

## 结论先行

1. **写好 agent 的核心是「最简可行 + 项目特定指令 + 把工具当接口设计」**：Anthropic 明确反对为炫技堆复杂度，通用建议价值低，具体领域步骤才有效；agent 的 description 是派发契约，要像给新员工 onboarding 一样写。
2. **评测 agent 的正确姿势是「任务 + 判分器」，且优先确定性判分**：代码级检查（测试通过、grep、git diff、状态检查）> LLM 评分（需人工校准）> 纯人工。评测「产出了什么」而不是「走了什么路径」。
3. **评测任务从真实失败中来，不从想象中来**（Hamel 错误分析优先 + Anthropic Step 1）：先把实际发生的失败记录归类，再转测试；20–50 个来自真实失败的任务就是很好的起点。通用指标（如「连贯性 3.72→4.2」）没有意义。
4. **能力评测与回归评测分开**：能力评测起始通过率应该低（有坡可爬）；回归评测应接近 100%（防倒退）。我们工作区的「已知平台不对称红清单」本质上就是回归评测的雏形。
5. **我们 6 个 agent 的主要缺口不是定义质量，是没有评测闭环**：没有任务集、没有「人工发现失败→转评测任务→改 agent 定义→复测」的迭代机制。本次已补（见第四节与 `agent-eval-tasks.md`）。

## 一、如何写好 agent

### 1.1 基本原则（Anthropic《Building effective agents》，精读）

- **能用最简单方案就不要用复杂方案**：workflow（预定义代码编排 LLM）先于 autonomous agent（LLM 动态自主循环）；只有当任务需要 LLM 在每一步基于环境反馈自主决策时才上真 agent。加复杂度必须可证明地改善结果。
- **透明**：让 agent 输出里展示规划步骤（plan → act），人能看清它在干什么。
- **ACI（Agent-Computer Interface）像 HCI 一样重要**：工具接口（名称、描述、参数、返回格式、报错文案）就是 agent 的 UX，要专门设计、专门评测、持续迭代。
- 指令写法：**领域特定的具体步骤 > 通用技巧**；保持 prompt 精简、及时更新（与模型版本共同演进）。

### 1.2 工具/接口设计（Anthropic《Writing effective tools for agents》，精读）

- 工具少而精：「更多工具不总带来更好结果」，合并多步操作为一个面向目标的工具（如 `schedule_event` 取代查人→查日程→创建三步）。
- 命名空间一致（前缀按服务/资源分组）；参数名无歧义（`user_id` 不是 `user`）；给分页/截断合理默认值。
- 返回值对 agent 友好：语义化字段（名字、类型），不是 UUID / mime type 之类密码；提供 `response_format: concise|detailed` 让 agent 控制开销（concise 约省 ⅔ token）。
- 报错要可行动：告诉 agent 下一步该怎么改，而不是抛错误码。
- 描述就是 prompt 工程的一部分，是最有效的改进杠杆之一。

### 1.3 子代理（subagent）定义

- **description 是路由触发器**：主代理把它当搜索 query 用；写清「用于什么 / 不用于什么 / 是否主动使用」；总描述量有上限（Claude Code 超 15k token 告警），细节放正文。
- 单一职责 + 显式边界 + 角色间互斥的转介规则（谁把活给谁）。
- 正文 = 该角色的系统提示词：角色、必读上下文、硬约束、工作流、输出格式、边界。参考 VoltAgent 161+ 角色集合的三段式骨架（分析→执行→质量收尾），但删掉其多代理编排仪式部分。

## 二、如何评价评测 agent

### 2.1 评测的结构（Anthropic《Demystifying evals for AI agents》2026-01，精读）

- **task（任务）** = 输入 + 成功标准；**trial** = 一次尝试（输出有随机性，需多次）；**grader（判分器）** = 对 transcript（轨迹）或 outcome（最终状态）打分的逻辑；**harness** = 跑评测的基础设施。
- 三类判分器组合使用：

| 类型 | 手段 | 优点 | 缺点 |
|---|---|---|---|
| 代码判分 | 字符串/正则、fail-to-pass 测试、静态分析、状态检查、工具调用校验 | 快、便宜、客观、可复现 | 对合理变体脆弱、缺细腻度 |
| 模型判分 | rubric 评分、自然语言断言、成对比较 | 灵活、可扩展、处理开放任务 | 非确定性、贵、必须与人工校准 |
| 人工判分 | SME 审查、抽样、A/B | 金标准、用于校准模型判分 | 贵、慢 |

- **判产出不判路径**：检查 agent 是否按特定顺序调特定工具过于脆弱——agent 常找到设计者没预见的合法解。评它最终交付了什么。
- **能力评测 vs 回归评测**：前者起始低通过率（爬坡用），后者应 ~100%（防倒退）；能力评测满分后「毕业」为回归评测。
- **非确定性**：pass@k（k 次至少 1 次成功）适合「成功一次就行」；pass^k（k 次全成功）适合「每次都得行」。LLM 判分器本身不稳定——同输出三次打分可能三个结果，需重复运行/多数投票，并给「Unknown」出口防幻觉。
- **八步路线图**：尽早开始（20–50 个真实失败任务即可）→ 从手工检查和 bug 库取任务 → 任务无歧义（两个领域专家独立得出同一 verdict；要有参考解证明可解；多次 0% 通过通常是任务坏了不是 agent 不行）→ 双向平衡（该触发和**不该触发**都要测，防单侧优化）→ 稳定隔离的评测环境 → 精心设计判分器 → **读 transcript**（失败要「看起来公平」；分数不高时先怀疑评测）→ 监控评测饱和（100% 后无改进信号）并持续维护。
- **瑞士奶酪模型**：自动评测 + 生产监控 + A/B + 用户反馈 + 人工读轨迹 + 系统化人评，多层叠加，没有单层能拦住所有问题。
- 框架只是加速器：Harbor、Braintrust、LangSmith、Langfuse、Arize Phoenix 等；「框架的好坏取决于跑在里面的评测任务」，简单脚本起步完全可行。

### 2.2 错误分析优先（Hamel Husain，精读）

- 先看真实数据（对话/轨迹/产出），别先写测试或选指标——「测容易测的，而不是测对用户重要的」是通病。
- 具体做法：电子表格记录（ID / 主要问题 / 分类 / 备注）→ 让模式自己浮现（不求全）→ 只针对**观察到的失败模式**写测试 → 每类先生成 1–2 个用例，小步扩。
- 反对现成通用指标：「3.72 涨到 4.2 能说明系统变好了吗？不知道。」

### 2.3 厂商同构与社区实践（OpenAI 精读 + 社区扫描）

- OpenAI《Evaluate agent workflows》（精读）：两阶段模型——**先用 trace grading 调试行为**（trace = 一次运行的模型/工具/交接完整记录，配 grader 检查「是否选对工具、路由是否改善」），**「知道什么是好之后」再沉淀可重复的 datasets 与 eval runs**。与 Anthropic 的「读 transcript → 任务集」路线同构，交叉验证成立。
- 轨迹评测三支柱：代码检查 + LLM-as-judge + 轨迹分析（Zep、Medium 实操文，扫描）。
- LLM 判分器要与人工标注校准；判分 prompt 本身可被系统优化（TR Labs 用 GEPA/ACE 三倍判分准确率，扫描）。

## 三、对照审计：本工作区 6 个 agent

**已与最佳实践一致的**（不重复建设）：
- 项目特定硬约束写进每个角色正文（Anthropic：具体指令 > 通用建议）✓
- description 写成路由触发器、角色互斥、S 级反派发约束 ✓
- 完成 = outcome 实操实证（判产出不判路径）✓；测试会红性自检（等价于变异测试验证测试有效性）✓
- 已知平台不对称红清单 = 回归评测雏形 ✓；S/M/L 成本分级 = 最简可行原则 ✓

**本次补上的缺口**：
- 无评测闭环 → 新增 `agent-eval-tasks.md` 任务集 v0：把 2026-10-28 用户指出的真实失败逐条转成带判分方式的评测任务（错误分析优先：全部来自真实失败，零想象任务；当日复核后为 16 条）。
- 无复盘习惯 → AGENTS.md「八」新增评测与迭代小节：L 级任务收尾读子代理完整汇报（= 读 transcript），失败要能归因；agent 定义是活文档，靠实战失败驱动迭代。
- 派发双向平衡 → 任务集显式包含「应派发」与「不应派发」两类（防过度派发与漏派发，Anthropic under/over-trigger 教训）。

**待实战验证的**（不预先过度建设）：
- 判分器中 LLM rubric 部分（过度设计识别、spec 完整性）需在首批实战中与用户判断对齐校准后再依赖。
- pass^k 一致性（同一任务多次运行结果稳定）暂不机制化，等出现「时好时坏」的实际信号再加。

## 四、参考来源

精读一手：
- Anthropic《Building effective agents》 https://www.anthropic.com/research/building-effective-agents
- Anthropic《Demystifying evals for AI agents》(2026-01) https://www.anthropic.com/engineering/demystifying-evals-for-ai-agents
- Anthropic《Writing effective tools for agents》 https://www.anthropic.com/engineering/writing-tools-for-agents
- Hamel Husain《Error analysis before writing tests》 https://hamel.dev/notes/llm/officehours/erroranalysis.html
- OpenAI《Evaluate agent workflows》 https://developers.openai.com/api/docs/guides/agent-evals
- VoltAgent/awesome-claude-code-subagents（161+ 角色集合与模板） https://github.com/VoltAgent/awesome-claude-code-subagents

扫描线索（未全文精读）：
- Zep《LLM Evaluation Framework》(2026-05) — 判分器校准 https://www.getzep.com/ai-agents/llm-evaluation-framework
- LinkedIn（Maxime）— LLM 判分器非确定性问题
- Medium（hungvtm）— 轨迹评测三支柱实操
- Hamel《It's Hard to Eval Is a Product Smell》 https://hamelhusain.substack.com/p/its-hard-to-eval-is-a-product-smell
