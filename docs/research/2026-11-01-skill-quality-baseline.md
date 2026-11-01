# 判据基线 v2：ZCode 插件全类型面质量尺（devflow 审计⓪ 尺子）

- 日期：2026-11-01 ｜ 角色：researcher ｜ 服务对象：devflow 插件全面审计（6 项下游）与 skill 重写 ｜ 覆写 v1（pool:need:81 打回重做）
- **v1→v2 三缺陷修复**：① 权威重排——ZCode 官方（本地 zcode-guide/plugin-creator/skill-creator + 官方文档站 zcode.z.ai + CLI 0.17.0 实测）为第一权威，CC/Anthropic 文档全部降「CC 参照」；② 范围扩展——按 ZCode 官方定义立**全类型面**判据（manifest / marketplace / skill / command / hook / mcp / agent 七类 + 触发/写作/反模式/测试四横切 + Memory 熵），发现驱动，官方定义什么就立什么节；③ 元技能补课——精读 anthropics/skills 原版 skill-creator、mcp-builder、obra/superpowers writing-skills、Matt Pocock writing-for-agents（含 SKILL-MECHANICS），方法论入 C 组。
- 纪律：外部事实全部来自本轮检索/实测，逐条带来源编号；先验事实（任务书与 v1 已核实项）标注沿用不重查；devflow 打分为 frontmatter+行数+CLI 实测级预判，body 级留下游。

---

## 结论先行（≤5 条）

1. **尺子已按 ZCode 官方重建并扩到七类组件**：36+ 条判据全部「判据→来源→可检验表述」，硬约束层（A 组）几乎每条可脚本断言；`zcode plugins validate --source` 实测只吃**市场根**（要 marketplace.json），插件级机械校验应走 dev 市场 + validate 或 eval-run.sh 式 tripwire。
2. **ZCode 与 CC 有实质差异，v1 四条判据已失真并修正**：skill frontmatter 白名单仅 5 键（`name/description/when_to_use/license/metadata`），**`disable-model-invocation` 在官方白名单未列=疑被忽略**（devflow 3 个 skill 正写了它，行为实际靠 description 文字「AI 勿主动调用」补偿——补偿正确，字段本身待验证）；description 注入**截断 ~250 字符**且全体技能共享固定元数据预算、超限退化「仅名字」（本会话实测 104 个技能共存，预算压力是真实风险）；hooks 仅 7 事件且 `async` 无运行时效果。
3. **v1 两处事实错误已勘误**：devflow 测试基建**并非空白**——`tests/eval-run.sh` E1-E9 结构 tripwire（版本三处一致/引号配对/移植性零残留/锚点/门禁/池 MCP 13 工具冒烟）+ `docs/eval-tasks.md` 出生失效台账 + eval-log PASS/FAIL 流水，成熟度超出 v1 认知；真正缺的是**行为级**评测（触发率、WITH/WITHOUT 双臂、压力场景）。组件计数也非 18：11 skills + 7 agents + **1 command + hooks(2 文件 3 脚本) + 2 mcpServers + marketplace 交接**。
4. **官方源内部有 5 处分歧**（skill 发现优先级、agents 是否执行、hooks async、marketplace npm source、手动调技能语法），已列表给出裁决与待验证法；其中「agents 字段 recorded-not-executed」仅 diagnosing-plugins 孤例，官方站+configuration-guide+实况（本会话 gate 正拦 Agent 派发）三证 agents 是真实组件。
5. **devflow 预判（全类型面）**：结构层强（hooks 事件/字段/单位全对、mcpServers 模板变量用法正确、monitor dist 产物在、E1-E9 自测在跑）；高风险带集中在——agents 组铁律压 description + tools 白名单仅 1/7 且不含 Skill、command 兜底文案猜测 cache 路径、session-start 注入文本固定 token 成本偏大、plugin description/i18n 展示面 vs 官方精炼范式、bash command hook 的 Windows 行为待验证。

---

## 【用什么】来源清单（判据引用编号全表）

**权威分级**：ZCode 官方（Z/W/CLI）＞ ZCode 官方示例（E）＞ CC 参照（CC）＞ 社区元技能（M）。标「精读」= 本轮全文读；「扫描」= 结构取样；「实测」= 本轮 CLI/文件系统验证。

### ZCode 官方·本地（一手·精读，均本轮全文读）

| # | 来源 | 路径（`~/.zcode/cli/plugins/cache/zcode-plugins-official/` 下） |
|---|---|---|
| Z1 | diagnosing-commands 0.3.0 | `zcode-guide/0.3.0/skills/diagnosing-commands/SKILL.md` |
| Z2 | diagnosing-hooks 0.3.0 | `zcode-guide/0.3.0/skills/diagnosing-hooks/SKILL.md` |
| Z3 | diagnosing-mcp 0.3.0 | `zcode-guide/0.3.0/skills/diagnosing-mcp/SKILL.md` |
| Z4 | diagnosing-plugins 0.3.0 | `zcode-guide/0.3.0/skills/diagnosing-plugins/SKILL.md` |
| Z5 | diagnosing-skills 0.3.0 | `zcode-guide/0.3.0/skills/diagnosing-skills/SKILL.md` |
| Z6 | zcode-configuration-guide 0.3.0 | `zcode-guide/0.3.0/skills/zcode-configuration-guide/SKILL.md` |
| Z7 | plugin-creator 0.1.1 SKILL.md（6 步流程+完成判据+交付纪律范式） | `plugin-creator/0.1.1/skills/plugin-creator/SKILL.md` |
| Z8 | plugin-json-spec（ZCode manifest+marketplace 规范） | `plugin-creator/0.1.1/skills/plugin-creator/references/plugin-json-spec.md` |
| Z9 | installing-and-updating（手动市场交接全流程） | `plugin-creator/0.1.1/skills/plugin-creator/references/installing-and-updating.md` |
| Z10 | skill-creator 0.1.0（ZCode 版写作/迭代方法论） | `skill-creator/0.1.0/skills/skill-creator/SKILL.md` |

### ZCode 官方·网络（一手·精读）

| # | 来源 | URL |
|---|---|---|
| W1 | 官方文档站·Plugin（含 plugin.json/marketplace.json/命令/技能 frontmatter 字段速查、hooks.json、.mcp.json、模板变量、userConfig、官方市场类别） | https://zcode.z.ai/cn/docs/plugin |
| W2 | 官方文档站·Skill（元数据预算退化、250 字符注入截断、100KB body 截断、插件技能扁平布局、`$` 手动调用、导入/分发） | https://zcode.z.ai/en/docs/skill |

（同站兄弟页 /docs/hooks、/docs/commands、/docs/mcp-services、/docs/subagents 本轮未逐页精读，列作下游补检入口；官方市场仓 https://github.com/zai-org/zcode-plugins 未精读，同上。）

### ZCode CLI 实测（一手·本轮实跑，zcode 0.17.0）

| # | 实测项 | 结果 |
|---|---|---|
| CLI | `zcode --help` / `zcode plugins --help` | 子命令全集：plugins discover/overview/marketplace list|add|remove|update/install|update|describe|**validate**|configure|restore；`doctor`；`commands list`；`skills list` |
| CLI | `zcode plugins validate --source <插件目录>` | **报错「Marketplace manifest not found」——只吃市场根**（要 marketplace.json），非插件目录 |
| CLI | `zcode plugins validate zcode-guide@zcode-plugins-official` | 因本机残留失效市场 `zcode-auto-resume-local` 报目录缺失（环境问题非规则问题） |
| CLI | `zcode commands list` | devflow `/auto-continue` 被发现，来源=插件 cache 路径 |
| CLI | `zcode skills list` | 104 技能共存；devflow 技能组 `user/plugin`、别名 `devflow:ask (alias ask)`；内置组 `system/plugin`；项目组 `project/agents`；description 全文展示 |

### ZCode 官方示例（一手·扫描/精读取样）

| # | 示例 | 取证点 |
|---|---|---|
| E1 | typst-workbench 0.2.0（**mcpServers 面样板**） | plugin.json：stdio+args 模板变量+cwd+timeoutMs+env（含 `${user_config.typstPath}`）+`userConfig`+`ui.surfaces`+`description_i18n`(en+zh-CN) 双语 |
| E2 | browser-use 0.5.1 control-browser（精读） | description「Use when…+Prefer over X+Main agent only」；`${ZCODE_PLUGIN_ROOT} ?? ${CLAUDE_PLUGIN_ROOT}` 且**禁止从 skill 自身路径推插件根**；工具全限定名 `mcp__node_repl__js` |
| E3 | computer-use 0.6.5（扫描） | description 一句负向路由（web 用 Browser Use）；「Success means the API accepted, not that the app acted」观察确认范式 |
| E4 | github 0.1.4 commit/setup（精读） | 正负边界双全（「not when they want to create a PR」）；`.claude-plugin`+`.zcode-plugin` 双 manifest 兼容布局 |
| E5 | visualize 0.1.1（扫描） | references/+assets/+scripts/+widgets/ 三层布局；skill 内 LICENSE/assets 结构 |
| E6 | mattpocock-skills 1.3.1 plugin.json（实测） | `skills` 为**显式路径数组** `["./skills/engineering/ask-matt",…]`——嵌套分组目录须数组声明，官方站「插件技能须扁平」适用于目录声明形态 |

### CC 参照（本轮未重查，沿用 v1 已核实；全部降级）

| # | 来源 | URL |
|---|---|---|
| CC1 | Claude Code Plugins reference | https://code.claude.com/docs/en/plugins-reference |
| CC2 | Skill authoring best practices | https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices |
| CC3 | Agent Skills 开放标准 | https://agentskills.io/specification.md |
| CC4 | Test plugins with evals | https://code.claude.com/docs/en/plugin-evals |
| CC5 | Subagents | https://code.claude.com/docs/en/subagents |

### 社区元技能（一手·精读，方法论供给）

| # | 来源 | 取证点 |
|---|---|---|
| M1 | anthropics/skills 原版 skill-creator（Z10 的母本） | evals/evals.json schema、WITH/WITHOUT 双臂同回合、assertions 后补、description improver、「compatibility」字段、Lack of Surprise、>300 行 reference 带 TOC |
| M2 | anthropics/skills mcp-builder | MCP 工具设计质量判据全集（命名前缀/描述/schema/错误信息/分页/annotations/工作流 vs API 覆盖权衡） |
| M3 | obra/superpowers writing-skills | **Iron Law：无失败测试先不写/不改 skill**；TDD 映射表；压力场景+合理化收集；四类 skill 分类测法；description 只写触发不写流程摘要；脚本经解释器调用免 exec 位丢失 |
| M4 | Matt Pocock writing-for-agents + SKILL-MECHANICS（本地 `mattpocock-skills/1.3.1`） | context pointer 理论、两载（context/cognitive load）、信息层级阶梯、分支披露测试、完成判据 clarity+demand、leading words、negation 失效模式、no-op 剪枝、环境即事实源、router skill、dmi 双语义 |
| M5 | 检索线索（未入判据） | https://github.com/anthropics/skills ；https://github.com/VoltAgent/awesome-agent-skills ；aihero.dev/skills-writing-for-agents |

### devflow 实证（一手·本轮扫描/实测）

| # | 内容 |
|---|---|
| D | `/Users/coderstory/dev/zcode-plugins/devflow`：plugin.json、hooks/hooks.json+3 脚本、commands/auto-continue.md、agents×7 frontmatter、skills×11 frontmatter 键位、pool-usage SKILL.md 全文、tools/pool_mcp.py 13 工具、tests/eval-run.sh+E 系列、README、`zcode skills/commands list` 实测输出 |

---

## 官方源分歧与 v1 勘误（审计前必读）

### 官方源内部分歧（5 处，均给裁决）

| # | 分歧 | 两边 | 裁决 | 待验证法 |
|---|---|---|---|---|
| X1 | skill 发现优先级 | Z10：项目>用户；Z5/Z6：用户>工作区、`.zcode`>` .agents`、深层>浅层 | **取 Z5/Z6**（0.3.0 更新且双 skill 一致+配置指南同表） | 同名技能在 `~/.zcode/skills` 与 `<repo>/.zcode/skills` 各放一版，`zcode skills list` 看加载者 |
| X2 | manifest `agents` 字段 | Z4：「recorded but not executed」；W1+Z6：agents 是真实组件（设置→子智能体「插件子智能体」只读组） | **取 W1+Z6**（双源+实况：devflow agents 派发被本会话 gate 实际拦截处理） | 断点验证：Settings→子智能体 看 devflow 7 角色是否在「插件子智能体」组 |
| X3 | hooks `async` 字段 | W1：command 钩子支持 async；Z2：「async 无运行时效果，钩子总是内联」 | **按 Z2 保守**：不依赖 async 做后台化 | 实测带 async 的钩子是否阻塞回合 |
| X4 | marketplace source `npm` | W1：source 类型含 npm；Z4：「npm/pip not supported」 | 冲突保留；devflow 不用 npm，无风险 | `validate` 一个 npm source 市场看结果 |
| X5 | 手动调技能语法 | W2：`$skill-name`；Z10：`/skill <name>`（`/` 菜单 Skills 组两者都提） | 两种入口并存的可能性大，**文档统一表述为「`$` 或 `/` 菜单选择」** | 输入框实测两语法 |

### v1 勘误（打回缺陷②的范围缺口实证）

| # | v1 原判 | v2 修正 |
|---|---|---|
| V1 | 「devflow 18 组件」 | 实为 **11 skills + 7 agents + 1 command + hooks（hooks.json + session-start.sh + auto-continue.mjs + agent-gate.sh）+ 2 mcpServers + marketplace 交接 + eval 体系 + README 触发文档**；v1 预判表漏 command/hooks/marketplace 三面 |
| V2 | 「测试基建是空白，无任何触发率或增量价值度量」 | **前半错**：E1-E9 结构 tripwire + 出生失效台账 + eval-log 流水成熟（且 E8 已测「README 触发说法 vs frontmatter 一致性」）；**后半对**：行为级（触发率盲测、WITH/WITHOUT 双臂、压力场景）确实为零 |
| V3 | C1.7/C4.7 以 `disable-model-invocation` 为「ZCode 保留字段」展开 | W2/Z5 frontmatter 白名单未列该键，非白名单**被忽略**→ devflow 3 处 dmi 疑似死字段，行为全靠 description 补偿文字（补偿本身正确）；判据改为「行为不得依赖白名单外字段」 |
| V4 | 遗留2「ZCode 特有字段无公开权威页」 | **已解**：W1/W2 即权威字段表；hooks/mcp/commands/subagents 兄弟页待下游按需补读 |
| V5 | C4.6「子代理 description 总量 15k tokens 启动警告」 | 出处 CC5，降级为 CC 参照；ZCode 对应判据换为 W2 的「**共享固定元数据预算，超限退化仅名字**」（CLI 实测 104 技能同场，是现实约束非理论） |

---

## 【做什么】判据清单

格式：**判据 → 来源 → 可检验表述**。三档判定：达标 / 部分达标 / 不达标。A 组（类型面硬约束）优先机械判定；B-F 组按所附核验步骤判定。编号相对 v1 全量重排（v1 C1-C6 不再引用，映射见【怎么测-4】）。

### A. 类型面硬约束（ZCode 官方定义，发现驱动七类）

**A1 manifest（plugin.json）**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A1.1 | 清单位置按探测序 `.zcode-plugin/plugin.json` → `.claude-plugin/` → `.codex-plugin/`；发布物必须含其一 | W1,Z4 | 插件根三路径存在性检查 |
| A1.2 | `name` 匹配 `^[a-z0-9][a-z0-9._-]{0,127}$`，为稳定标识且=目录名；最小合法清单只需 name | W1,Z4,Z8 | 正则断言；目录名比对 |
| A1.3 | `version` 语义化（缺省 0.0.0）；发版时 plugin.json / marketplace.json / serverInfo 三处版本一致 | W1,Z7,D(E1) | 三处 `version` 字符串相等断言（devflow eval-run.sh E1 已自测，可直接复跑） |
| A1.4 | 只声明真实存在的资源；组件路径相对、不得绝对路径、不得逃逸插件根（宿主拒绝） | Z8,W1,Z4 | 遍历 manifest 每个声明路径：存在 ∧ 相对 ∧ `realpath` 在插件根内 |
| A1.5 | `hooks/hooks.json` 是自动发现的标准位置，manifest **不要重复指向同一文件** | W1 | manifest 无 `hooks` 键指向 `hooks/hooks.json` |
| A1.6 | `channels/lspServers/outputStyles/settings` 仅登记不执行（诊断提示），不得承载真实功能意图 | W1,Z4 | grep 这四键；有则核对是否只是无害登记 |
| A1.7 | `userConfig` 每项 type ∈ {string,number,boolean,directory,file}；`sensitive:true` 当前**界面无法填写也无法持久化**——用了就必须给 env 兜底 | W1,Z4 | userConfig 键位类型校验；sensitive 项必须存在对应 env 引用路径 |
| A1.8 | description 是市场展示文案：能力动词列举、精炼；官方范例普遍附 `description_i18n`(en+zh-CN) 双语 | W1,E1-E4 | 长度与风格核对（超长运行说明即偏航）；双语字段有无 |
| A1.9 | MCP args / hook command 引用插件内资源一律走 `${ZCODE_PLUGIN_ROOT}`（`${CLAUDE_PLUGIN_ROOT}` 等价），禁止硬编码安装缓存路径；脚本不得猜 cache 路径 | Z8,E2,W1 | grep manifest+hooks+脚本：插件内路径引用 100% 走变量；兜底**文案**里出现 cache 目录猜测也算部分达标 |

**A2 marketplace（分发面）**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A2.1 | 市场=`marketplace.json`（根级或 `.claude-plugin/`），`name` 正则同插件名，`plugins[]` 必填 | Z8,W1 | JSON schema 断言 |
| A2.2 | entry `source` ∈ 相对路径/directory/github/git/url/file；本地 dev 测试一律用相对路径或 directory | Z8,W1,Z9 | source kind 枚举校验 |
| A2.3 | 展示元数据（displayName/displayName_i18n/description_i18n/category/tags）**留在市场条目**，不塞插件 manifest；icon 用真实可达 URL，缺就不写不伪造 | Z8,W1 | manifest 与市场条目字段分布检查 |
| A2.4 | 市场条目 version/description 与插件 manifest 同步（版本对比口径以此为准，不同步=更新不生效） | W1,Z9 | 两处 version 相等断言 |
| A2.5 | `strict:true` 条目触发更严校验；`dependencies` 写 `name@market`（同市场可裸名），跨市场需 `allowCrossMarketplaceDependenciesOn` | W1,Z4 | 如用则字段齐备性校验 |
| A2.6 | 机械校验入口=`zcode plugins validate --source <市场根>`（**实测只吃市场根不吃插件目录**）；日常回归用 eval-run.sh 式 tripwire 兜底 | CLI | 对 dev 市场根跑 validate 退出码 0 |

**A3 skill**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A3.1 | 技能=目录+`SKILL.md`（大小写敏感）；manifest 用**目录声明**时技能必须扁平（分组子目录不被拾取），嵌套布局须显式路径数组 | W2,E6 | `skills` 字段形态 × 目录层级交叉断言 |
| A3.2 | frontmatter 白名单仅 `name/description/when_to_use/license/metadata`，非白名单键**被忽略**——任何功能语义不得依赖白名单外字段（见 V3 的 dmi 教训） | W2,Z5 | 提取每 skill 全部 frontmatter 键 → 对照白名单 → 白名单外键逐个审「行为是否依赖它」 |
| A3.3 | `name` kebab-case 1-64 字符且=目录名 | Z5,W2,Z10 | 正则+比对（devflow E2 已自测，可复跑） |
| A3.4 | `description` ≤1024 字符（**超限=整个技能被丢弃**，非截断）；完全无 frontmatter → 装载但空描述、几乎不触发 | Z5,W2 | 长度计数；frontmatter 存在性 |
| A3.5 | 注入给模型的只是 ~250 字符摘录——**触发词必须前置在 description 前 250 字符内** | Z5,W2 | 每条 description 前 250 字符含触发措辞断言 |
| A3.6 | `when_to_use` 用作补充触发时机；能用它承载「何时用」就不挤占 description | W2,Z5 | 有触发面冗余的 skill 检查是否用了该键 |
| A3.7 | body <500 行；>100KB 加载截断 | W2,Z10 | `wc -l`+`du` |
| A3.8 | 大块细节拆 references/scripts/assets 且 body 写**条件式指向**（「若 X 先读 references/y.md」）；>300 行 reference 顶部带 TOC | Z10,M1 | 引用指向条件句存在性；长 reference 头 30 行 TOC |
| A3.9 | 同名技能按发现序遮蔽（用户>工作区、`.zcode>` `.agents`、深层>浅层，按 X1 裁决）；发布技能须查与既存技能同名冲突 | Z5,Z6 | 对每个技能名全根查重 |
| A3.10 | 子代理 tools 白名单可能把 Skill 工具排除→白名单子代理内**无法调用任何技能**；给子代理配白名单时必须显式含 `Skill` 才能用技能 | W2 | 每个带 tools 的 agent：白名单 ∋ Skill 或明确「本角色不用技能」 |

**A4 command（斜杠命令）**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A4.1 | 文件名=命令名，匹配 `^[a-z0-9][a-z0-9_:-]{0,63}$`（违规=静默丢弃）；嵌套子目录以 `:` 连接（`review/code.md`→`/review:code`，**非** `/review/code`） | Z1,W1 | 文件名正则批量断言 |
| A4.2 | frontmatter 认 6 个连字符键：`description/argument-hint/allowed-tools/model/skills/disable-noninteractive`；未知键**静默忽略**（拼写错误无告警） | Z1,W1 | 键名白名单比对 |
| A4.3 | description 或非空 body 至少有其一，否则命令被丢弃；description 缺省时取正文首个非空行 | Z1 | 两要素存在性 |
| A4.4 | 参数占位只用精确 token `$ARGUMENTS`/`$1`/`$2`；无占位符时参数自动追加「User arguments:」节（写文档时应声明该行为） | Z1 | 占位符 token 精确匹配 |
| A4.5 | 禁内联动态 shell（`` !`cmd` ``/`!` 代码块会被拒绝）；`skills` 字段自动挂载技能 | Z1 | grep 拒绝形态 |
| A4.6 | 命令名不得撞内置 slash 命令/`compress`（交互菜单被过滤）；同名命令先到先得，插件命令优先级最低（本地方件永远盖插件） | Z1 | 与内置名单对照；全根查重 |

**A5 hook**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A5.1 | 事件**仅 7 个**：SessionStart/UserPromptSubmit/PreToolUse/PermissionRequest/PostToolUse/PostToolUseFailure/Stop；`Notification/SubagentStop/PreCompact` 等不认（静默不触发） | Z2,W1 | 事件名枚举断言 |
| A5.2 | matcher 是**区分大小写**的正则，对工具名匹配（`"bash"` 匹不到 `Bash`）；别名 `Task`↔`Agent`、`Write/Edit`←`ApplyPatch`；无效正则**静默永不匹配**；省略 matcher=全匹配 | Z2 | 逐条 matcher：正则合法性+目标名大小写+别名覆盖 |
| A5.3 | `type:"command"`：command/shell/timeout(秒)/timeoutMs/statusMessage；`type:"process"`：command/args[]/timeoutMs/statusMessage（argv 无 shell，最可移植）；**字段混用=钩子被丢弃** | Z2,W1 | 逐条钩子按 type 做字段集校验 |
| A5.4 | 超时单位：command 的 `timeout` 是**秒**、process 与 `timeoutMs` 是毫秒（`timeoutMs` 优先；缺省 60000ms）——`timeout:500`=500 秒 | Z2 | 数值×单位语义核对 |
| A5.5 | 模板变量：`${ZCODE_PLUGIN_ROOT}`（**仅插件钩子**）、`${ZCODE_PROJECT_DIR}`、`${CLAUDE_SESSION_ID}`；skill 目录变量在钩子中**报错** | Z2 | 变量白名单 grep |
| A5.6 | 输出协议：stdout 严格 JSON（多余键=校验失败丢弃）或退出码（0 过/2 阻断/其他=错误）；`additionalContext` 注入会话；PreToolUse 可返回 allow/ask/deny；Stop 请求续跑上限 3 次 | Z2 | 手跑样例输入核退出码与 JSON 键集 |
| A5.7 | 配置文件钩子默认禁用（须 `hooks.enabled:true`）；**插件贡献钩子时 runner 自动启用**——插件钩子是激活钩子系统的主要形态 | Z2,Z6 | hooks.json 结构断言+文档如实说明 |
| A5.8 | 钩子仅在插件启用后的**新会话**生效；安装/升级/改钩子的交接文案必须写明「开新会话」 | W1,Z9 | README/交接文档含该句 |
| A5.9 | 跨平台：command 钩子经 shell，POSIX 语法 Windows 风险高——**优先 process 钩子**；shell 钩子脚本保持无扩展名 polyglot 或经解释器调用 | Z2 | 钩子类型分布；shell 钩子的目标平台跑通证据 |
| A5.10 | 脚本一律经解释器调用（`bash "${ZCODE_PLUGIN_ROOT}/x.sh"`）而非裸路径执行（打包器可能剥 exec 位）；不依赖 `async` 字段做后台（按 X3 裁决） | M3,Z2 | grep 钩子命令形态 |

**A6 mcpServers**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A6.1 | 插件 MCP 声明于 manifest `mcpServers` 或插件根 `.mcp.json`；服务命名空间 `plugin:<插件>:<服务>`，工具全名形如 `mcp__plugin_<插件>_<服务>__<工具>`（本会话实测印证） | Z3,W1 | 命名与全限定名核对 |
| A6.2 | `${...}` 模板变量**仅插件 MCP 展开**（含 `${user_config.键}`）；配置文件 MCP 不展开（那里必须绝对路径） | Z3,W1 | 变量出现位置核对 |
| A6.3 | 配置 schema **严格**：未知键=整个服务被静默丢弃；type 省略时 command→stdio、url→http 推断 | Z3 | 逐 server 键集白名单校验 |
| A6.4 | stdio 必有 `command`（字符串）+`args`（字符串数组，禁 OpenCode 式数组塞进 command）；http/sse 必有 `url`；环境变量键名是 `env`（非 `environment`，desktop 路径不迁移旧键） | Z3,W1 | 字段类型+键名断言 |
| A6.5 | 默认超时 30000ms；慢启动服务显式加 `timeoutMs` | Z3 | timeoutMs 存在性×启动耗时 |
| A6.6 | 敏感值只进 `env`/`headers`，不进 `command`/`url`；优先 `${user_config.键}` 而非明文 | W1,Z3 | grep 敏感字面量 |
| A6.7 | 工具设计质量（M2 全集）：动作式命名+一致前缀；描述含功能+参数+返回说明；inputSchema 带约束与字段示例；尽量 outputSchema/structuredContent；错误信息**可操作**（含下一步建议）；结果可分页/截断防上下文爆仓 | M2 | 逐工具：命名/描述/schema/错误样本/分页五查 |
| A6.8 | 尽量标注 annotations（readOnly/destructive/idempotent/openWorld hint）供宿主与模型判断安全性 | M2 | 逐工具 annotations 存在性 |
| A6.9 | stdio 错误流会进 ZCode 日志；排障法=终端直接复跑 `command+args`；交付前完成一次握手冒烟（initialize+tools/list） | Z3,D(E9) | 冒烟记录存在（devflow E9 已是范本，可复跑） |

**A7 agent（子智能体）**

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| A7.1 | `agents/*.md` 随插件注册（设置→子智能体「插件子智能体」只读组可见）；按 X2 裁决不采信「仅登记」说 | W1,Z6 | 客户端可见性核验 |
| A7.2 | frontmatter：`name/description` 必备；`model/color/thoughtLevel/tools` 等扩展键属平台行为，ZCode 官方字段表未列——**用了即标注「平台未证实」并给回退语义** | W1,D | 非 W1 字段清单逐个标注 |
| A7.3 | description 驱动自动委托：职责+触发时机（何时派）齐备；鼓励主动用「use proactively」措辞；实现细节/铁律不进 description 进 body | CC5(参照),Z10 类推 | 逐 agent 两要素+细节位置检查 |
| A7.4 | 全体技能+子代理 description 共享固定元数据预算，超限**退化为仅名字**（触发率骤降）——description 总量是系统级约束，不是单条文案问题 | W2,CLI | 统计全插件 description 总字符数；结合同场技能总数（实测 104）评估预算占用 |
| A7.5 | 派发路由清晰：主入口 router（skill 或主会话）与各 agent 分工唯一，无双重入口歧义 | M4(router skill 范式) | 画派发图核对单入口 |

### B. 触发工程

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| B.1 | description = 做什么 + 何时用，缺一即部分达标 | Z10,W2 | 两要素逐条核对 |
| B.2 | 含负向边界（何时不用/勿主动调用），高频误触入口必须有 | E2,E4,D | 排除句存在性 |
| B.3 | 略带推销对抗欠触发（「even if they don't explicitly say X」式覆盖隐含措辞） | Z10,M1 | 隐含场景覆盖检查 |
| B.4 | 触发可靠性必须实测：目标命中率/相邻误触率有记录；ZCode 无并行评测→逐条单跑新会话 | Z10,Z5 | 测试记录存在性 |
| B.5 | 触发文档一致性：README/说明文字与 frontmatter 能力（含手动/自动）一致，矛盾即不达标 | D(E8 范本) | 速查表逐行对照 frontmatter |
| B.6 | 行为不得依赖白名单外 frontmatter 字段（dmi 类）；手动专用技能靠 description 文字+文档声明兜底 | W2,V3 | 白名单外键×行为依赖交叉审 |

### C. 写作结构与风格（元技能方法论提炼）

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| C.1 | 每步有**完成判据**，且双性达标：clarity（能判 done/not-done）+ demand（要求量足）；防范 premature completion（后续步骤可见诱发赶工，必要时按序切分隐藏） | M4,M1,Z7 | 各主要步骤末尾完成条件句抽查+模糊度评级 |
| C.2 | 复杂流程编号步骤+可复制 checklist | Z10,CC2 | 编号段存在性 |
| C.3 | 示例胜规则：结构化输出给字面样例；指定工具写出调用形式；**一个精例胜多个平庸例**，不给多语言重复例/填空模板 | M3,Z10,CC2 | 每个「输出格式」要求旁有无 literal example |
| C.4 | 解释 why；全大写 MUST/NEVER 堆砌=规则解释不足信号 | Z10,M3 | 强调词密度×理由句 |
| C.5 | 指令用祈使句 | Z10,CC2 | 抽句核对 |
| C.6 | **两载**意识：context load（每回合恒在的 token）vs cognitive load（人要记住的东西）——恒在层（description/注入文本）按「每字都永久付费」修剪；能下放 references 的不下放 context | M4 | 恒在层字符预算核算 |
| C.7 | 信息层级：分支作披露测试（所有分支都用的内联、部分分支用的下放指针）；同概念定义/规则/注意事项 co-location 不散落；防 sprawl | M4 | 指针条件句+散落检查 |
| C.8 | leading words：用模型先验里已有的紧凑概念词锚定行为；**正表述优先**（禁令把禁止行为带进上下文，反而更可得——「别想大象」）；禁令仅留不可正表述的硬护栏且配正向目标 | M4 | 禁令句改写检查；概念词一致性 |
| C.9 | 剪枝三查：逐句 no-op 测试（不改默认行为即删全句）；环境即事实源（复述 `package.json`/目录布局/`--help` 的内容=cache，会腐烂，只写查不到的约定与缘由）；同一含义单一真相源 | M4 | 逐句抽查+重复含义 grep |
| C.10 | 反复出现的手工步骤沉淀 `scripts/`（写一次勿重造）；脚本在正文中经解释器调用（`bash scripts/x.sh`）；流程权威内置 feedback loop（校验→修→复跑） | M3,Z10,CC2 | scripts 引用形态+循环存在性 |

### D. 反模式（出现即扣分）

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| D.1 | busywork（绕圈/重读/一次性脚本）归因到具体指令→**删规则不加规则**；过拟合窄规则堆积让 skill 越改越差 | Z10,M3 | trace 审计归因 |
| D.2 | description 写成流程摘要（agent 读了摘要就照摘要办、不再读正文）——description 只写触发条件与症状 | M3 | description 与 body 内容重叠度 |
| D.3 | 时效内容硬编码（版本号/日期/「最新」）；机器特定残留（绝对路径/语言参数） | CC2,D(E3 范本) | grep 年份/版本/`/Users/…` |
| D.4 | 泛化空洞描述（「Helps with X」无场景无触发词） | CC2,Z5 | description 名词化程度 |
| D.5 | 深嵌套引用（reference 里再指 reference，链深>1）；Windows 反斜杠路径字面量 | CC2,CC3 | 引用链深度断言 |
| D.6 | 文件存在≠功能可用；未跑验收不得称成功——交付必须区分「已验证/待用户动作/待验证」三态 | Z7,Z9 | 交付段三态措辞检查 |
| D.7 | **Iron Law：没有先跑出失败基线，不写新 skill 也不改旧 skill**（含「只是加一节」）；写前先记录无技能时的违规/合理化话术 | M3 | 每次改动有无失败基线记录 |
| D.8 | 不给无判定条件的自由分支；不假设工具/包已安装（外部命令先交代可用性） | CC2 | 分支条件句+安装交代 |

### E. 测试与评测

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| E.1 | 结构 tripwire 层（机器判分）：版本/计数一致性、frontmatter 形态、移植性残留、跨文件锚点、门禁变异测试、MCP 握手冒烟——devflow E1-E4/E7-E9 是现成范本 | D | eval-run.sh 退出码 0；断言清单覆盖上述六类 |
| E.2 | **行为触发层**（目前缺）：新会话盲测 10 条=5 目标措辞+3 相邻措辞+2 隐含措辞；指标目标命中 ≥80%、相邻误触 ≤20%；手动技能显式加载后核对行为自洽 | Z5,Z10 | 盲测记录存在（无记录=不达标） |
| E.3 | **增量价值层**：同回合跑 WITH/WITHOUT 双臂（M1；CC4 的 evals 机制为参照）；ZCode 无并行评测→顺序单跑逐条看；断言草拟与运行并行 | M1,CC4(参照) | 双臂对比记录 |
| E.4 | 压力场景按 skill 类型选测法：纪律型（多压叠加+合理化收集→加显式反制）、技艺型（变体/缺信息场景）、模式型（反例识别）、参考型（检索/应用/盲区） | M3 | 每类型 skill 有对应测法记录 |
| E.5 | 评测集结构化：`{skill_name, evals:[{id,prompt,expected_output,files,assertions}]}`，prompt 用真实用户措辞（带路径/口语/错别字） | M1 | evals 文件 schema 断言 |
| E.6 | 出生失效台账：真实失败当次入库、每条任务附出生与失效条件、连续 N 次全过标毕业 | D(范本),项目惯例 | 台账字段齐备性 |

### F. Memory / 池熵治理（参照判据，服务下游审计⑥；ZCode 无官方 memory 治理页，本组以 devflow 池机制+项目惯例为源，级别最低、措辞为「参照」）

| # | 判据 | 来源 | 可检验表述 |
|---|---|---|---|
| F.1 | **入池查重铁律**：入池前必 `pool_search`，命中同类不重复入池——重复条目是池熵的第一形态 | D(pool-usage) | 抽样 N 条 open 条目两两相似度抽查；pool_search 关键词命中同主题多条=熵 |
| F.2 | **状态机终态收敛**：每条目必须处于状态机合法态（在池/排产/进行中/待验收/关闭/挂起/拒绝），关闭必带 evidence、拒绝必填 reason、挂起必填 blocker；长期滞留非终态=熵 | D(pool-usage+pool_mcp.py 状态机) | pool_stats 状态分布；非终态且超期（如 >2 迭代）条目清单 |
| F.3 | **软删除可回溯**：删除一律走 `pool_soft_delete(id,by)`（记操作者），禁直写 SQL（绕状态机+审计+快照=违规）；快照/报告与 DB 可对账 | D(pool-usage) | 审计直写痕迹（sqlite timeline vs 快照 diff）；soft_delete 记录 by 字段非空 |
| F.4 | **文档台账单真相源**：流程卡/PRD/方案卡入 doc 台账（doc_save 同 slug 升版入 history，不产散落 md）；同主题多版本散落=熵 | D(pool-usage+项目 docs/pool 约定) | doc_list 同主题版本链 vs 仓内散落同主题 .md 计数 |
| F.5 | **记录新鲜度自描述**：条目/经验/评测任务必须带日期与出生失效条件（过时可判）；无日期条目=熵 | D(eval-tasks 范本),项目惯例 | 无日期/无失效条件条目计数 |

---

## 【怎么做】方法与 devflow 全类型面预判

### 方法（本轮实际执行序）

本地官方 10 文件精读定基调（Z1-Z10）→ 官方文档站 2 页精读交叉（W1/W2）→ CLI 5 项实测（validate 语义/技能发现/命令发现）→ 官方示例 6 处取样（E1-E6）→ 元技能 4 个精读提炼（M1-M4）→ devflow 全类型面取证（D）→ 分歧裁决（X1-X5）→ 判据落盘。两源交叉规则：硬约束判据 ≥1 个 ZCode 官方源；与 CC 参照冲突时以 ZCode 为准；官方源互斥时按更新度+源数+实况裁决并标 X 编号。

### devflow 预判表（扩全类型面；证据=frontmatter+行数+CLI 实测；body 级 `?` 待下游复核；√ 达标 / ◐ 部分达标 / ? 待核 / ✗ 不达标）

**plugin.json + marketplace 交接**：

| 面 | 判定 | 证据与差距 |
|---|---|---|
| manifest 位置/名称/版本 | √ | `.zcode-plugin/plugin.json`；name 合法；E1 三处版本一致自测在跑 |
| 组件路径 | √ | agents/skills 目录存在；monitor/dist/server.mjs+panel.html+THIRD_PARTY_NOTICES 三产物在（A1.4 发布面 ✓，比 v1「待核」进一步）；hooks 未在 manifest 重复指向（A1.5 ✓） |
| description 展示面 | ◐ | 一句超长中文运行说明 vs 官方精炼动词列举（E1-E4）；无 `description_i18n` 双语（E1 有）；组件计数文案有 E1 防回退锚（好） |
| 模板变量 | √ | mcpServers 全走 `${ZCODE_PLUGIN_ROOT}`，pool env 用 `${ZCODE_PROJECT_DIR}`（A6.2 插件 MCP 展开 ✓） |
| dependencies/userConfig/ui | √(空) | 未用即无违规；如需用户配置（如 monitor 端口）可评估 userConfig |

**11 skills**（frontmatter 键位实测：全部 name/description；audit、codegraph-setup、devflow-setup 额外写 `disable-model-invocation`）：

| 组件 | 判定 | 一行差距预判（编号=判据） |
|---|---|---|
| ask | √/◐ | router 范式（M4）好例；description 担流程细节，疑应下放 body（A3.5/B.1 前置性待测）；负边界「纯问答与 git 不用」√ |
| audit | ◐ | dmi 字段死代码风险（B.6/V3）；description「默认禁止自动调用，AI 勿主动调用」补偿文字正确；body 39 行并发审计流程疑缺 checklist（C.2）? |
| codegraph-setup | ◐ | 同 dmi 风险；安装类 skill 应含验证步骤与失败分支（C.1/C.10）? |
| consolidate | ◐ | 20 行装 4 件事，步骤/完成判据待核（C.1/C.2）? |
| daily-wrapup-discipline | ◐ | 多职责挤短 body（C.7 sprawl 反向）? |
| devflow-setup | ◐ | dmi 风险；description 已含「仅用户明说、AI 勿主动调用」人工向文案（B.6 正确姿势）；产物清单齐备性待核（A1.4 同理）? |
| pool-usage | √ | 全插件触发工程最佳：双池并存**禁用全局池**负边界（B.2）+身份绑定条件式（B.1）+纪律段禁直写 SQL（F.3）；「速查」形态如增长应 references 化（A3.8） |
| req-analysis | ◐ | 14 行出 PRD+矩阵，疑缺步骤/示例/完成判据（C.1/C.3），重写第一优先 |
| tech-solution | ◐ | 同 req-analysis |
| ui-probe-shot | √ | description 5 场景编号+触发面广（B.1/B.3 正例，可反向输出为判据样例） |
| zcode-dispatch | ◐ | 实现细节（双 env 入口）进 description（A3.5/C.6 违规方向），瘦身候选 |

**7 agents**（frontmatter 实测：全部 name/description/color/model/thoughtLevel:max；仅 code-reviewer 有 `tools:[Read,Bash]`）：

| 组件 | 判定 | 差距预判 |
|---|---|---|
| 全组 | ◐ | **铁律压 description**（A7.3 违规方向，7/7）；`thoughtLevel/model` 为 W1 未列键→标注平台未证实（A7.2）；description 合计字符量对共享预算的贡献待测（A7.4） |
| code-reviewer | ◐ | tools 白名单**不含 Skill**→该代理内无法调用技能（A3.10）——若属有意（纯只读）应在 body 写明；白名单范式仅 1/7 不一致（A7.5） |
| product-manager / ui-ux-designer | √ | 「use proactively+触发问句样例」全组最佳（A7.3 正例） |
| bug-fixer/developer/qa-tester/researcher | ◐ | 无 proactive 措辞；靠 gate 派单则属设计意图，待核设计文档 |

**1 command（auto-continue）**：文件名正则 √；frontmatter description+argument-hint √；`$ARGUMENTS` √；**兜底文案让 AI 猜测插件 cache 目录**（「通常位于 ~/.zcode/cli/plugins/cache/ 下」）——脚本未硬编码故非 A1.9 直违，但与 Z8「loader 管 token、勿猜 cache 路径」精神冲突，◐（建议改走 `${ZCODE_PLUGIN_ROOT}` 由钩子侧传入或 statusMessage 提示）。

**hooks（hooks.json + session-start.sh + agent-gate.sh + auto-continue.mjs）**：

- 事件名 4 处全合法（A5.1 √）；matcher `Agent|Task` 覆盖别名（A5.2 √）、`startup|resume` 合法值 √；command/process 字段各归各（A5.3 √）；timeoutMs 全毫秒（A5.4 √）；statusMessage 全配 √；gate 退出码 2=阻断语义正确（A5.6 √）；auto-continue 的 Stop block 续跑受 ZCode 3 次上限约束（A5.6，重试计数设计与之咬合 √）。
- ◐ 两处：① `bash "${ZCODE_PLUGIN_ROOT}/gates/agent-gate.sh"` 是 **command 钩子经 shell**——Windows 宿主行为待验证（A5.9，DevDesk 双机硬约束下必核；monitor/auto-continue 已用 process 型 √）；② session-start 注入文本 ~370 汉字每会话恒在（C.6 两载：粗估 >700 tokens/会话 × 每会话，预算压力真实）。
- √ 好例：gate fail-open 自检（session-start 顺带拦 general-purpose 验证门禁活着，正是「hook 静默失效无外部信号」的对策）；gate 白名单+池标记双重拦截与 diagnosing-hooks 排障路径兼容。

**2 mcpServers**：pool（python3 裸解释器调 √ A5.10/A6.4；13 工具与 E9 冒烟断言一致 √ A6.9；工具描述带状态机语义 √ A6.7 部分）｜monitor（node+dist 产物在 √；无 timeoutMs 默认 30s，面板 server 启动耗时待测 ? A6.5）。

**eval 体系**：E1-E9 结构 tripwire+出生失效台账+PASS/FAIL 流水 √（E.1/E.6 达标，且为全插件最佳实践来源）；行为触发层（E.2）与双臂增量层（E.3）✗ 缺失——下游审计的量化主战场。

**README**：能力速查表=触发文档与 frontmatter 一致性有 E8 锚 √（B.5 达标范本）。

---

## 【怎么测】判据可验证性方案

### 1. 机械校验入口（本轮实测结论）

- 插件级：`zcode plugins validate` **只吃市场根**——把插件放进 dev 布局 `<workspace>/plugins/{marketplace.json, devflow/}` 后 `validate --source <workspace>/plugins`（与 Z8/Z9 的 dev 市场布局一致）；日常回归用 `sh tests/eval-run.sh`（E1-E9 已覆盖 A1/A3 大半硬断言）。
- 技能发现/遮蔽：`zcode skills list`（组别+别名+路径直接可见）；命令：`zcode commands list`；配置体检：`zcode doctor`。
- A3.2 白名单外键、A4 键位、A5 事件/字段、A6 schema：均可用 eval-run.sh 增设 tripwire（一次性脚本即可机械判定）。

### 2. 行为触发盲测（B.4/E.2，当前为零，须新建）

10 条 prompt=5 目标措辞+3 相邻措辞+2 隐含措辞；每条新开会话逐条发（ZCode 无并行评测）；记录命中/误触；手动技能（audit/codegraph-setup/devflow-setup）显式加载（`$` 或 `/` 菜单）后核对行为自洽（B.6）。

### 3. A/B 双臂与压力场景（E.3/E.4）

- 重写任一 skill 时同回合跑 WITH/WITHOUT（M1：runs 在后台执行时草拟 assertions；`evals/evals.json` schema 落盘）；ZCode 顺序单跑。
- 纪律型 skill（devflow 大半是）用压力场景+合理化收集：多压叠加（时限+沉没成本）看是否违规、收集合理化话术→写显式反制（M3）。
- 失败基线先行（D.7）：改 skill 前先跑无技能版记录违规证据。

### 4. 下游 6 项审计的判据映射（含「每项调哪个 diagnosing skill」）

| 下游审计 | 主判据 | 调用 ZCode 官方 diagnosing |
|---|---|---|
| ① 角色流程（7 agents + ask/consolidate 等流程 skill） | A7 全组 + C.1-C.5 + D.1/D.7 + B.4；tools 白名单×Skill 可用性（A3.10） | diagnosing-skills（触发面）+ zcode-configuration-guide（路由入口） |
| ② MCP 质量（pool / monitor） | A6.1-A6.9 全组 + E.1(复跑 E9) | **diagnosing-mcp** |
| ③ SQLite 健康（pool.db 状态机/快照对账） | F.2/F.3 + A6.7/A6.9（错误面）+ E9 断言复跑 | diagnosing-mcp（连接/排障面）+ sqlite 直查对账 |
| ④ 文件实效（台账/产物/证据物/时效） | C.9（环境即事实源/cache）+ F.4/F.5 + D.3 + A1.4（声明=存在） | **diagnosing-plugins**（组件路径/资源存在性） |
| ⑤ hooks / 门禁 / 注入面 | A5 全组 + C.6（注入文本两载）+ A5.9 Windows 待验证 | **diagnosing-hooks** |
| ⑥ Memory 熵 + 触发面全量复审 | F.1-F.5 + B 组全量 + A3.4/A3.5/A7.4 预算核算 | **diagnosing-skills** + **diagnosing-commands**（/auto-continue 面）+ diagnosing-plugins（组件启停对技能池的影响） |
| 附：通用迁移（跨机/跨市场，DevDesk 双机） | A2 全组 + E3 残留扫描（devflow 已有）+ A5.9/A6.4 跨平台 | **diagnosing-plugins**（市场/安装/依赖） |

### 5. 触发失败修复顺序（固定）

先改 description（250 字符前置重排+负边界）→ 再 `when_to_use` → 最后才考虑加 body；每轮只改一处→复跑同批 prompt（Z10 循环）。

---

## 遗留

1. **W 站兄弟页未精读**：`/docs/hooks`、`/docs/mcp-services`、`/docs/commands`、`/docs/subagents`、官方市场仓 zai-org/zcode-plugins——A5/A6/A7 判据已由本地 Z1-Z4 覆盖主干，兄弟页可作下游审计②⑤的补充引证。
2. **X1-X5 五处官方源分歧**已给裁决与验证法，其中 X1（技能优先级）与 X2（agents 执行性）建议审计前用 5 分钟 POC 实测定案。
3. **`disable-model-invocation` 在 ZCode 的实际行为**未实测（白名单未列，疑被忽略）：删除该字段是对的做法候选，但需先确认 ZCode 是否有未文档化的保留键（查 release note 或实装行为）。
4. **agent frontmatter 扩展键**（model/color/thoughtLevel/tools）无官方字段表：A7.2 按「平台未证实」弱表述，devflow 已在用且工作，风险主要是升级兼容。
5. **command 钩子的 Windows shell 行为**（A5.9）与 **monitor server 30s 默认超时**（A6.5）需在 Windows 机实测——本机 macOS 无法代验。
6. 元技能 M5 线索（VoltAgent 精选库、aihero 原文）未精读；anthropics/skills 生产实例（docx/pdf/xlsx）可在重写期逐篇补样例范式。
7. 中文 description 的触发权衡仍无官方数据（W2 全英文语料），devflow 全中文描述的欠触发程度只能靠【怎么测-2】实测回答。

## 来源汇总（分级复述）

- **精读·ZCode 官方·本地**：Z1-Z10（路径见上表）。
- **精读·ZCode 官方·网络**：W1 https://zcode.z.ai/cn/docs/plugin ；W2 https://zcode.z.ai/en/docs/skill 。
- **实测·ZCode CLI 0.17.0**：`--help`、`plugins validate`（--source 语义）、`commands list`、`skills list`。
- **精读/扫描·ZCode 官方示例**：E1 typst-workbench 0.2.0；E2 browser-use 0.5.1；E3 computer-use 0.6.5；E4 github 0.1.4；E5 visualize 0.1.1；E6 mattpocock-skills 1.3.1（均 `~/.zcode/cli/plugins/cache/` 本地）。
- **精读·社区元技能**：M1 https://github.com/anthropics/skills （skills/skill-creator）；M2 同仓 skills/mcp-builder；M3 https://github.com/obra/superpowers （skills/writing-skills）；M4 mattpocock-skills 1.3.1 writing-for-agents + SKILL-MECHANICS（本地）。
- **CC 参照（沿用 v1，本轮未重查）**：CC1 https://code.claude.com/docs/en/plugins-reference ；CC2 https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices ；CC3 https://agentskills.io/specification.md ；CC4 https://code.claude.com/docs/en/plugin-evals ；CC5 https://code.claude.com/docs/en/subagents 。
- **扫描·devflow 实证**：D `/Users/coderstory/dev/zcode-plugins/devflow`（plugin.json / hooks / commands / agents / skills / tools/pool_mcp.py / tests / docs/eval-* / README + CLI 实测输出）。
- **检索线索（扫描）**：https://github.com/zai-org/zcode-plugins ；https://github.com/VoltAgent/awesome-agent-skills ；https://aiweekly.co/alerts/zais-zcode-adds-plugin-management-to-its-glm-52-agent 。

（本报告为任务书指定唯一落盘文件，v2 覆写；工具调用共 27 次，预算 50，未超。禁做项未触碰：除本文件外未改任何东西，未执行审计本身。）
