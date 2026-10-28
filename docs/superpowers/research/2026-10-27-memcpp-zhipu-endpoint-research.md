# 智谱（Zhipu / Z.ai）端点调研：一 key 双用与 embeddings 可用性

日期：2026-10-27 ｜ 分支：feat/memcpp ｜ 来源：智谱中英文官方文档（docs.bigmodel.cn / docs.z.ai / code.claude.com），中英交叉核对。检索日均为 2026-10-27，引文为当日原文。

**结论先行**：记忆面板要的「一个智谱 coding plan 的 key 同时做对话提炼 + 向量化」，取决于 key 的签发日期。2026-10-16 起智谱把「标准 API key」和「套餐 key」分开管理、互不通用；只有此前签发、带「兼容」标签的历史 key，官方才明确支持同一把 key 走两个端点（套餐端点耗套餐额度、标准端点按量扣余额）。新建套餐 key 调不了 embeddings，向量化必须另建一把标准 API key（同账号即可，几乎零成本：0.5 元/百万 token）。

## 一、开放平台 API 的 OpenAI 兼容性

成立。标准 API base_url = `https://open.bigmodel.cn/api/paas/v4`，`/chat/completions` 与 `/embeddings` 都挂在这下面，认证就是 `Authorization: Bearer <API Key>`（API key 明文直填，现行文档不再要求旧的 JWT 换 token 流程）。

- 官方 OpenAI 兼容说明：「智谱提供与 OpenAI API 兼容的接口……只需要简单修改 API 密钥和基础 URL，就能无缝切换到智谱的模型服务」。
  https://docs.bigmodel.cn/cn/guide/develop/openai/introduction.md （官方文档，2026-10-27）
- embeddings 端点：`POST https://open.bigmodel.cn/api/paas/v4/embeddings`，请求体 `model` / `input` / `dimensions`，响应即 OpenAI 格式的 `data[].embedding` + `usage`。
  https://docs.bigmodel.cn/api-reference/模型-api/文本嵌入.md （官方文档，2026-10-27）
- 注意：coding plan 的 OpenAI 兼容端点是**另一个**地址 `https://open.bigmodel.cn/api/coding/paas/v4`，与标准 `/api/paas/v4` 不是同一条计费通道。
  https://docs.bigmodel.cn/cn/coding-plan/quick-start （官方文档，2026-10-27）

## 二、coding plan 的 key 能否调 embeddings（一 key 双用是否成立的关键）

官方已明示：**分开管理，默认不通用**。

- 「自 2026 年 10 月 16 日起，模型标准 API（按量计费／资源包）与 GLM Coding Plan 套餐的 API Key 分开管理」「标准 API Key、个人套餐 Key、团队套餐 Key 互不通用」「切换调用地址不会改变 Key 的计费用途」。
  https://docs.bigmodel.cn/cn/coding-plan/notice/api-key-management.md （官方文档，2026-10-27）
- 唯一例外是**历史兼容 key**（2026-10-16 前签发、账号曾购过套餐）：「对于历史兼容 Key，使用哪类额度取决于『调用端点』及『账号是否曾拥有过 Coding Plan 套餐』」；官方示例原句：「配置为 `https://open.bigmodel.cn/api/coding/paas/v4` 时使用套餐额度；改为 `https://open.bigmodel.cn/api/paas/v4` 后，则按标准 API 规则计费」。英文站同口径：标准端点调用「Uses your account balance or applicable usage bundles. Does not consume Coding Plan quota」。
  中文：https://docs.bigmodel.cn/cn/coding-plan/notice/api-key-management.md ；英文：https://docs.z.ai/devpack/notice/api-key-management.md （官方文档，2026-10-27）
- 新建套餐 key 能否调 embeddings：**官方未明示**（key 管理文档通篇未提 embeddings）。但两条已明示规则叠加后不可依赖：计费用途被 key 类型锁死 + 「套餐仅限在官方支持的指定工具与产品环境中使用」「在除规定工具外调用 API，不可享用 Coding 套餐的额度」（https://docs.bigmodel.cn/cn/coding-plan/faq.md ，2026-10-27）。结论：不要假设套餐 key 能调 embeddings。
- embeddings 官方建议路径：同账号在「标准 API Key」入口另建一把标准 key 按量计费（两类 key 可共存同一账号）。

## 三、embedding 模型现状

- 现行模型：`embedding-3`（推荐，接口枚举值），`embedding-2` 仍在线上（枚举与价目表均未下线，固定 1024 维）。响应结构 `{"object":"list","data":[{"index":0,"object":"embedding","embedding":[...]}],"usage":{...}}`，即 OpenAI `/v1/embeddings` 同构，OpenAI SDK 可直接调。
  https://docs.bigmodel.cn/api-reference/模型-api/文本嵌入.md （官方文档，2026-10-27）
- 维度：embedding-3 默认 2048，可选 256 / 512 / 1024 / 2048——**没有 768 档**（telemem 本地档 nomic-embed-text 是 768 维，切云端必须显式配维度并重建索引）。
- 限制：单条输入 ≤3072 token、数组一次最多 64 条、上下文 8K。
- 计费：0.5 元/百万 token（Batch API 半价 0.25 元）；计费页未列免费额度，按量扣账户余额。
  https://docs.bigmodel.cn/cn/guide/start/pricing.md （官方文档，2026-10-27）

## 四、Claude Code 官方接入口径

- 智谱中文站：`ANTHROPIC_BASE_URL=https://open.bigmodel.cn/api/anthropic`，key 用智谱平台 API key（「替换您访问的 base_url 为 https://open.bigmodel.cn/api/anthropic」）。 https://docs.bigmodel.cn/cn/guide/develop/claude/introduction （官方文档，2026-10-27）
- Z.ai 英文站：settings.json 里配 `ANTHROPIC_BASE_URL="https://api.z.ai/api/anthropic"`、`ANTHROPIC_AUTH_TOKEN="your_zai_api_key"`。 https://docs.z.ai/devpack/tool/claude （官方文档，2026-10-27）
- 两个环境变量是 Claude Code 官方机制：「ANTHROPIC_AUTH_TOKEN: Custom value for the Authorization header (the value you set here will be prefixed with Bearer)」「ANTHROPIC_BASE_URL: Override the API endpoint」。 https://code.claude.com/docs/en/env-vars （官方文档，2026-10-27）
- 「同一个 key 既走 Anthropic 端点又走 paas/v4」：只有历史兼容 key 官方支持（见第二节引文：套餐端点耗套餐额度、标准端点按量扣余额，同一把 key 换 base_url 即切换）；新建套餐 key 官方明确「切换调用地址不会改变 Key 的计费用途」，即双端点双计费不成立。用户可到控制台 API Keys 页看 key 是否带「兼容」标签自行确认。

## 五、对常驻记忆提炼的限制定价

- 套餐额度机制：「每 5 小时限额 + 每周限额」双窗口，积分制（非按 token）：Lite 2,000/5小时 + 10,000/周；Pro 12,000 + 60,000；Max 28,000 + 140,000；耗尽后「需要等待下一个 5 小时周期恢复额度，系统不会继续消耗您的其他资源包/账户余额」；工作日 14:00–18:00（UTC+8）以外时段积分按 5 折扣。 https://docs.bigmodel.cn/cn/coding-plan/overview.md ；https://docs.z.ai/devpack/overview.md （官方文档，2026-10-27）
- 超量行为：官方只写「等下一周期」，未承诺降智/限速细则（2026 年 2 月官方曾因扩容不足公开致歉，属历史事件非成文规则）。并发数官方未给具体数字，仅「Max > Pro > Lite」动态调整。 https://docs.bigmodel.cn/cn/coding-plan/usage-notes.md （官方文档，2026-10-27）
- 关键风险：后台常驻记忆提炼**不属于**「官方支持的指定工具」——「用于非支持工具将被限制权益」，违规「触发风控，会被进行限流、冻结等处置。3 次以上违规可能会被封禁账号」（usage-notes 原文）。所以 telemem 的常驻小请求**不能走套餐端点**，必须走标准 API 按量计费。
- 按量成本可忽略：提炼用 `glm-5.3-flash` 0.8 元/百万输入 token（输出 2.8 元）+ embedding-3 0.5 元/百万 token；记忆面板每天百万 token 量级也才几毛到几元。 https://docs.bigmodel.cn/cn/guide/start/pricing.md （2026-10-27）

## 接入方案建议

1. 一 key 双用：**有条件成立**——仅限 2026-10-16 前签发、控制台显示「兼容」标签的 key（同一 key：/api/anthropic 或 /api/coding/paas/v4 耗套餐额度，/api/paas/v4/embeddings 按量扣余额，官方原文背书）；2026-10-16 后新建的套餐 key 双用不成立，需同账号另建一把标准 API key 专门做 embeddings（并保证账户有少量余额）。
2. DevDesk profile 三字段（base_url+key+模型名）**不够**：需要 base_url、api_key、chat_model、embed_model 至少四个字段；若要覆盖「套餐对话 + 按量向量化」双通道，profile 应支持两套 base_url+key（anthropic 兼容通道 / OpenAI 兼容标准通道）。
3. telemem 配置：provider 选 OpenAI 兼容，base_url=`https://open.bigmodel.cn/api/paas/v4`，chat 模型 `glm-5.3-flash`，embeddings 模型 `embedding-3` 且必须显式设 `dimensions`（建议 1024；无 768 档），建索引前定死维度，换档需全量重嵌。
