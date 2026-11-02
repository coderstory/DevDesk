# ponytail 研读报告——为 devflow 瘦身整改（need 103）与能力整合（need 104）铺路

- 日期：2026-11-02 ｜ 角色：researcher ｜ 简报：`[pool:need:102]`
- 对象：https://github.com/DietrichGebert/ponytail（外部一手，v5.3.3）＋ 本机痕迹（本地一手）
- 证据等级标注：【精读】=逐行读过源文件；【扫描】=仅 tree/API 尺寸；【本地】=本机路径实读

---

## 0. 结论先行（≤5 条）

1. **ponytail 是「一个 52 行 prompt 的生意」**：agent 可见面核心只有 `skills/ponytail/SKILL.md` 52 行（~900 token 常驻），其余 5 个命令 skill 全部按需加载、20 平台适配层全是这 52 行的派生副本（脚本生成 + 守卫校验），0 个 MCP server。182k stars / 10k forks / MIT / 月内 5 个 release（v5.3.3 2026-10-29）。
2. **「小而精」不是写得多省，而是单一事实源 + 派生**：prompt 只写一处，hook 注入（运行时读 SKILL.md 按档位过滤）、AGENTS.md/rules 副本（构建脚本生成 + `check-rule-copies.js` 校验）全是派生物。devflow 的 11 skills + 7 agents 行数其实同量级（537 行），差距在**派生机制与注入纪律**，不在文本长度。
3. **注入纪律是它跑赢的关键工程面**：SessionStart 一次注入 ~1.2KB 规则 + ≤2KB 代码库地图；UserPromptSubmit 对普通输入**零输出零注入**（只解析 `/ponytail` 命令写 flag file）；SubagentStart 单独补子代理传播（上游 issue #252）。所有 hook fail-open + 带超时预算。对照本项目已关停的 codegraph prompt-hook（每条消息 1–3KB 注入、跑偏率高，见 memory `context-hygiene-audit`），方向完全相反。
4. **能力 80% 核心 = 6 级 reuse-first 梯子 + never-cut 负面清单 + shortcut: 注释约定 + 「Not checked」结尾惯例**；band/board UI、gain 成绩卡、20 平台适配、i18n 全是场景特化，与 devflow 整合无关，明确排除。
5. **与 devflow 重叠面大（理念层），net-new 少且都是行级**：梯子/反过度设计/分级审查/pool 台账 devflow 都有等价物；真正 net-new 只有 4 件小事——shortcut: 注释约定、债务收割命令、审查发现补「不修的后果」段、description 触发词枚举写法。整合候选全部 ≤40 行级。建议**先瘦后整**。

---

## 1. 它是什么【精读 README + repo API】

| 维度 | 事实 | 来源 |
|---|---|---|
| 定位 | 「让 AI agent 像房间里最懒的资深程序员一样思考。最好的代码是你没写的代码」——YAGNI/reuse-first 行为插件，装上即全场生效 | README |
| 形态 | 一个 prompt（`skills/ponytail/SKILL.md`）+ 5 个命令型 skill + 3 个生命周期 hook；同一内容派生 ~20 个 agent 平台的适配副本 | README「The prompt」节 + git tree |
| 作者 | Dietrich Gebert（DietrichGebert），个人项目，sponsor=GreenPT；另有 npm 包 `@dietrichgebert/ponytail` | repo / npm |
| 活跃度 | created 2026-06-12，pushed 2026-10-29；182,232 stars、10,044 forks、148 open issues；releases v5.3.3(10-29)/v5.3.2/5.3.1/5.3.0(10-16)/v5.2.0(10-13)；npm 20 个版本 | GitHub API |
| 许可/度量 | MIT；自带可复现 benchmark：v5 较无 skill 基线 LOC 47%、输出 token 55%、成本 74%、耗时 59%；98% 高危逻辑自带测试（基线 68%）；39 任务×5 跑、Opus 5.5、真实 FastAPI+React 仓；6 个安全任务 30/30 通过；v5 重写自身代码 -53% | README + `benchmarks/results/2026-10-07-agentic.md`【扫描】 |

解决什么问题：AI agent 写码过度工程（装库/造日历/加抽象层）。手段不是「审查拦住」，而是**改写 agent 的默认决策序**（先问要不要存在，最后才写新代码）。

---

## 2. 组件面盘点【精读，除注明外】

### 2.1 agent 可见面（进上下文的部分）

| 组件 | 行数/大小 | 职责 | 风格要点 |
|---|---|---|---|
| `skills/ponytail/SKILL.md` | 52 行 / 3.5KB | 唯一常驻 prompt：人设 1 句 + Before-you-write 定界 + 6 级梯子 + 10 条操作规则 + never-cut 清单 + 3 档表 | frontmatter description = 触发词枚举（"ponytail"/"be lazy"/"simplest solution"/"yagni"/抱怨过度工程）+ 档位提示；正文全是祈使句，零叙事 |
| `AGENTS.md` | 33 行 / 2.7KB | 紧凑版 = SKILL.md 去 frontmatter/档位表，供 rules-file 类 agent 整文件复制 | 与 SKILL.md 内容同源（副本关系由守卫保证） |
| `skills/ponytail-review/SKILL.md` | 101 行 / 5.1KB | diff 审查：先读改动触及的代码（不只 diff）→ 按 bug/风险/规模/缺测试/速度/精简 6 轴找问题 → 四段式 finding → Must/Should/Nice 分级 + Verdict + `Lean: -N lines possible.` + `Not checked:` | 每条 finding 带 4 bullet：What this is / Problem / Fix / **If we skip it**；带一个完整示例 finding |
| `skills/ponytail-audit/SKILL.md` | 103 行 / 5.4KB | 全仓审计：先建图（入口/数据流/预期负载）→ 同 6 轴 → ranked 报告，≤20 条 | review 的全仓版，去重后共用「Look for」结构 |
| `skills/ponytail-debt/SKILL.md` | 44 行 / 1.8KB | 收割 `shortcut:` 注释成债务台账；给出可直接跑的 `git grep -nE` 命令；无 upgrade 触发器的标 `no-trigger`（会烂的那批） | 一条 grep 命令就是全部实现 |
| `skills/ponytail-gain/SKILL.md` | 56 行 / 2.2KB | 展示 benchmark 成绩卡（ASCII 柱状图）；「Honesty boundary」节禁止在真实仓编造每仓节省数字 | 纯展示，自我营销件 |
| `skills/ponytail-help/SKILL.md` | 76 行 / 3.2KB | 命令速查卡 + 档位/配置/更新说明 | 纯展示 |
| `commands/*.toml` ×6 | 每个仅 2 行（description 1 + prompt 1） | Claude Code 命令薄壳 | review 的单行 prompt ~450 词，是 review SKILL 的单行压缩版 |

**合计：agent 可见面 ~450 行 markdown；常驻部分仅核心 SKILL.md ~900 token（hook 还会按档位过滤掉非当前档的表格行）+ ≤2KB 代码库地图。普通用户消息注入 0。**

### 2.2 代码面（不进模型上下文）

| 组件 | 大小 | 职责 | 工程要点 |
|---|---|---|---|
| `hooks/claude-codex-hooks.json` | 40 行 | 仅 3 个生命周期钩子：SessionStart / UserPromptSubmit / SubagentStart | 每条带 timeout:15 + statusMessage |
| `hooks/ponytail-instructions.js` | 98 行 | **单一事实源构建器**：运行时读 SKILL.md → 剥 frontmatter → 按档位过滤表格行/示例行；读不到才回退内嵌 AGENTS.md 副本 | 过滤规则有防误伤注释（普通 bullet 开头恰好像是档位词的场景） |
| `hooks/ponytail-activate.js` | 9.3KB | SessionStart：写 flag file `~/.claude/.ponytail-active` + 发规则 + 附代码库地图（3s 超时，fail-open，#1079/#1157）+ statusline 缺失提示 | Cursor 有 rule 时只发一行通知不双注入（#817） |
| `hooks/ponytail-mode-tracker.js` | 9.9KB | UserPromptSubmit：只解析 `/ponytail …` 与自然语言关停语 → 写 flag file；**普通输入静默退出**；`/ponytail default` 是唯一写 config 的路径（review 不可作默认，#377；`/ponytail-review` 不得锁档，#736） | 模式状态存 flag file，不靠每消息重注入 |
| `hooks/ponytail-subagent.js` | 94 行 | SubagentStart 传播（#252：SessionStart 上下文不达子代理）；`PONYTAIL_SUBAGENT_MATCHER` 正则可选圈定 agent 类型；bad regex/stdin 异常/超时一律 fail-open 注入 | Windows stdin 卡死修复注释带 issue 号（#443/#658/#790） |
| `hooks/ponytail-map.js` | 111 行 | 代码库地图：`git ls-files` + 每语言 1 个正则，每目录一行、共享目录（util/lib/service/component）优先、2000 字符预算、4000 文件上限 | 自注 `ponytail: regex, not a parser`——插件自己 dogfood shortcut 约定 |
| `hooks/ponytail-{runtime,config}.js` | 7.7KB+5.9KB | 跨平台（Claude/Codex/Copilot/Cursor/CodeBuddy/Qoder）输出写法与档位归一 | — |
| `hooks/ponytail-band-import.js` | 14KB | 历史会话导入 board【扫描】 | — |
| band UI（5.3 新增） | register.tsx 22KB + board.ts 45KB + metrics.ts 14.7KB + logo.ts 49.9KB | Claude Code ≥2.1.290 提示条上方状态带 + 本地统计页（`~/.config/ponytail/band/`）【扫描】 | 纯 UI，不进模型上下文 |
| 适配层 ~20 平台 | rules 副本 2623B ×7（.agents/.clinerules/.cursor/.qoder/.windsurf/.kiro/copilot-instructions）+ 8 个 plugin.json/yaml + openclaw skills（`scripts/build-openclaw-skills.js` 生成）+ opencode 命令/插件 + pi-extension + `__init__.py` 8.9KB（Hermes Python 适配） | 全是同一 prompt 的派生物 | `check-rule-copies.js` + `check-versions.js` 守卫副本一致；根 `plugin.json` 仅 1 行 `{"name":"ponytail"}` |
| 测试/CI | tests/ 24 文件 ~250KB（hooks.test.js 48KB）+ test.yml/publish.yml | 副本一致性、hook 行为、跨平台插件清单全测【扫描】 | — |
| 文档 | INSTALL.md 15.7KB + agent-portability.md 12.8KB + cursor-hooks.md 13.5KB + examples/ 12 案例 ~36KB + i18n README ×4 ~68KB【扫描】 | 接入指南 + before/after 案例（案例不进 prompt） | — |

**总体量**：仓库（去 assets/i18n/benchmarks）~150 文件；agent 可见面 ~450 行；代码侧 hooks 核心 ~1600 行 JS、band ~2900 行、测试 ~3000 行、适配层 ~1500 行。**一个 182k-star 的插件，常驻上下文成本 ≈ 1K token。**

### 2.3 本机痕迹【本地】

| 路径 | 事实 |
|---|---|
| `/Users/coderstory/dev/DevDesk/.claude/settings.json` | `enabledPlugins` 含 `"ponytail@ponytail": true`（Windows 机时代配置，hooks 写死 `C:/PROGRA~1/…`、`D:/project/gui/…`，Mac fail-open） |
| `/Users/coderstory/dev/DevDesk/.claude/settings.local.json` | `"ponytail@ponytail": false` + `"code-modernization@claude-plugins-official": false`——**简报所说启用/禁用对冲记录实证，现态=双双禁用** |
| `~/.zcode/cli/plugins/cache/` | 无 ponytail marketplace/本体（仅 claude-plugins-official / dev-zcode-plugins / local-auto-resume / zcode-auto-resume-local / zcode-plugins-official 五目录）→ **Mac 本机无源码缓存，本报告正文以 GitHub main（v5.3.3）为底本** |
| `~/.zcode/cli/config.json` | plugins/mcp 段均无 ponytail（mcp.servers 仅 codegraph） |
| `~/.zcode/cli/memories/projects/devdesk-7acb05266951a89c/memory/context-hygiene-audit.md` | 2026-11-02 审计已把此对冲列为待老板拍板 4 项之三 |
| 待验证 | Windows 机 `%USERPROFILE%\.claude\plugins\cache\` 下是否有 ponytail 本体（Mac 无法查；不影响本报告结论） |

---

## 3. 能力清单（核心/边缘 × 通用/场景特化）

| # | 能力 | 定性 | 整合判断 |
|---|---|---|---|
| 1 | 6 级 reuse-first 梯子（不需要存在→仓内已有→标准库/平台→已装依赖→一行→最小可用） | **核心 × 通用** | 整合候选 A |
| 2 | never-cut 负面清单（信任边界校验/防数据损错误处理/安全/无障碍/用户点名项，永不裁） | **核心 × 通用** | devflow 已有等价（developer 硬约束 + qa 面） |
| 3 | `shortcut: <上限>, <升级触发>` 注释约定 | **核心 × 通用** | 整合候选 B |
| 4 | 回复结尾 1–2 行「skipped / not checked / 风险」惯例 | **核心 × 通用** | 整合候选 C（devflow 已有「未验证项」，补齐口径即可） |
| 5 | 四段式审查 finding（What this is / Problem / Fix / **If we skip it**）+ Must/Should/Nice 分级 + `Verdict` + `Lean: -N lines possible.` | **核心 × 通用** | 整合候选 D |
| 6 | review 先读改动触及的代码与调用方、判定预期负载再下结论 | **核心 × 通用** | devflow code-reviewer 已有「追到调用方」 |
| 7 | `/ponytail-debt` 债务收割（一条 git grep + 台账行格式 + no-trigger 标记） | 边缘 × 通用 | 整合候选 E（价值在约定 #3，命令本身薄） |
| 8 | `/ponytail-audit` 全仓 ranked 审计（先建图、≤20 条、给修复顺序） | 边缘 × 通用 | devflow code-reviewer 指定对象=全仓即可覆盖，不新增组件 |
| 9 | 代码库地图（2000 字符符号索引，SessionStart 附带） | 边缘 × 通用 | DevDesk 已有 codegraph MCP，**不整合**（见 §5 判据 7） |
| 10 | 三档模式 lite/full/ultra + flag file + 静默模式跟踪 | 边缘 × 场景特化 | 排除（devflow 单档；机制可借鉴见 §5 判据 5） |
| 11 | band/board 状态带 UI（Claude Code ≥2.1.290 专属） | 边缘 × 场景特化 | 排除 |
| 12 | `/ponytail-gain` 成绩卡（自指营销） | 边缘 × 场景特化 | 排除 |
| 13 | `/ponytail-help` 速查卡 | 边缘 × 场景特化 | 排除（AGENTS.md 已覆盖） |
| 14 | 20 平台适配副本层 + i18n README ×4 | 边缘 × 场景特化 | 排除（devflow 只需 ZCode/Claude Code 一族）；其**派生机制**留作判据 |

80% 核心+通用 = #1–#6；给 need 104 的整合对象就是 #1、#3、#4、#5 四件 + #7 一件薄命令。

---

## 4. 与 devflow 重叠面

devflow 现状（简报已验证 + 本次实测 `/Users/coderstory/dev/zcode-plugins/devflow`）：7 agents（200 行）+ 11 skills（337 行）+ hooks（auto-continue.mjs 691 行 + hooks.json 63 行 + session-start.sh 10 行）+ 1 command（12 行）+ pool_mcp.py 1316 行（14 工具）+ tests/evals（21 用例）。

| ponytail 能力 | devflow 等价物（文件） | 判定 |
|---|---|---|
| 梯子 reuse-first | `agents/developer.md:19-20`（关联扫描+「先找现成」派 researcher/提案组件）+ `:28` 反过度设计 | **理念已有，形态不同**：devflow 是散点原则，ponytail 是有序决策序（第 1 级「要不要存在」devflow 缺） |
| never-cut 清单 | `agents/developer.md:28` 硬约束 + `agents/qa-tester.md` 验证面 | 已覆盖 |
| 审查分级清单 | `agents/code-reviewer.md:26` `[P0|P1|P2] file:line — 问题 — 为什么 — 建议` | **高度重叠**；ponytail 多「If we skip it」段与「Lean -N 行」汇总，devflow 多「可合入/需重审」总判定 |
| 审计 | `skills/audit/SKILL.md`（48 行，多角色并发审计） | 对象不同：devflow audit 审**流程产物/插件自身**（默认禁自动），ponytail-audit 审**代码仓**；代码仓审计实际由 code-reviewer 承担 → 不缺能力 |
| 债务台账 | `docs/pool/` 池库 + pool_add/fetch/update | devflow 台账**更强**（卡链/状态机/追溯）；缺的只是「从代码注释自动收割」入口 |
| SessionStart 注入 | `hooks/session-start.sh`（10 行）+ hooks.json | 已有 |
| 子代理规则传播（SubagentStart hook） | 不需要：devflow 走 Task 委派，子代理自带 `agents/*.md` 人设（本次子代理 metadata 实证 profileSnapshot 随派发携带） | 无需求（ponytail #252 是其单 prompt 架构特有问题） |
| 模式开关/help 卡 | AGENTS.md + devflow-setup | 已覆盖 |
| **net-new 清单** | ① shortcut: 注释约定（全无）② If we skip it 段（无）③ Lean -N 行汇总（无）④ description 触发词枚举写法（devflow 的 description 是「职责+触发」混合叙述，无显式触发词枚举）⑤ 0-MCP 低成本思路（devflow 14 个 MCP 工具 schema 常驻） | ①–④ 是 need 104 的真整合面；⑤ 是 need 103 的判据面 |

---

## 5. 瘦身判据（供 need 103，每条可检验）

1. **单一事实源**：prompt 只写一处，hook 注入/平台副本全部运行时或构建期派生，副本一致性有脚本守卫（`check-rule-copies.js`）。→ devflow 检验：skills/agents/rules 之间零手抄重复段（现状已较好，baseline 尺子固化即可）。
2. **manifest 最小**：Claude Code 插件清单 = 1 行 `{"name":"ponytail"}`，其余靠约定路径发现。→ devflow 检验：plugin.json 字段 ≤ 必要集，无自造字段。
3. **命令=单行 prompt**：TOML 仅 description + prompt 两行，无参数框架无 YAML 大纲。→ devflow 检验：commands/ 下文件 ≤12 行（现状 12 行，达标）。
4. **披露只两层**：常驻 1 个核心 + 按需命令层，无 references/ 多级引用链。→ devflow 检验：每个 SKILL.md 外链文件 ≤1 层；超层的（若 consolidate/tech-solution 引文档台账 schema）压成一行指针。
5. **UserPromptSubmit 静默纪律**：普通输入零注入，只在显式命令时出声；会话级状态存 flag file 而非每消息重注。→ devflow 检验：hooks 中不存在「每条消息无条件注入」的路径（本项目 codegraph prompt-hook 已关即是同一教训，memory `context-hygiene-audit` 第 1 项）。
6. **hook 全部 fail-open + 超时预算**：地图 3s / 钩子 15s / stdin 兜底 1s，失败不阻塞会话，注释带 issue 号可溯源。→ devflow 检验：hooks.json 每条有 timeout；auto-continue.mjs（691 行，本次未读，need 103 现场读）逐 hook 核对失败路径。
7. **0-MCP 思路**：能文件约定 + 正则解决的不上 MCP server（ponytail 14 工具的对应物是 0 个 server + 1 条 grep）。→ devflow 检验：pool_mcp.py 14 工具逐个对照实际调用频率（可查 pool_trace/会话记录），低频工具考虑合并/降级为 skill 文档指引。
8. **案例外置**：examples/ 12 个 before-after 不进任何 prompt，README 链接即可。→ devflow 检验：skill 正文无长案例段（agent-eval-tasks.md 是仓库文档非 prompt，合规）。
9. **自我基准化**：改版以可复现 benchmark 数字验收（v5 -53% code 后才加新功能——先删后加的顺序本身是判据）。→ devflow 检验：need 103 产出必须带「瘦身前后注入 token 量/组件数」对照，判据尺子（`/Users/coderstory/dev/DevDesk/docs/research/2026-11-01-skill-quality-baseline.md`）可加「常驻注入量」维度。
10. **文案纪律**：每条规则 = 祈使句 + 边界 + 例外，零叙事零哲学铺陈；description 三件套（干什么 / Use for 触发词枚举 / 边界如 One-shot changes nothing）。→ devflow 检验：11 skills description 逐个对照三件套（多数已达标，补触发词枚举）。

**devflow 可直接套用的整改点预判**（供 need 103 排期参考，非结论）：pool_mcp 14 工具 × 常驻 schema 是 devflow 与 ponytail 最大的结构性差异（1K vs 14 工具 schema）；auto-continue.mjs 691 行单文件是代码面最大疑点；文档面（AGENTS.md 66KB 瘦身）已有独立审计在案，不重复。

---

## 6. 整合候选清单（供 need 104，四元组）

| # | 能力 | devflow 落点 | 最小改法 | 风险 |
|---|---|---|---|---|
| A | 6 级梯子（有序 reuse-first 决策序） | `agents/developer.md` §流程 第 2 步 | 把「先找现成」改写为梯子序 6 问（需要存在吗→仓内→标准库/平台→已装依赖→一行→最小可用），~8 行 | 与接地铁律「现成方案优先」、researcher 口径重复——须一处定义一处引用，防两把尺子 |
| B | `shortcut: <上限>, <升级>` 注释约定 | `agents/developer.md` 硬约束 +1 行 | 1–2 行约定（含「不得用于已知要修的事」边界） | 注释垃圾化；必须配套候选 E 收割，否则烂账 |
| C | 审查 finding 补「不修的后果」+ 末行「可省 N 行」 | `agents/code-reviewer.md:26` 输出格式段 | +3~4 行（每发现加一段；末行加 Lean 汇总） | 低；注意与 P0/P1/P2 分级并存不冲突 |
| D | description 触发词枚举写法 | `docs/research/2026-11-01-skill-quality-baseline.md` 判据 +1 条；11 个 skill frontmatter 校对 | 判据 2 行 + 每 skill ~1 行 | 几乎零 |
| E | debt 收割（shortcut: 注释 → pool 台账） | 并入 `skills/consolidate/SKILL.md`（收尾场景天然契合），收割后逐条 pool_add | +10~15 行（grep 命令 + 台账行格式 + no-trigger 标记），不建新 skill、不加 MCP 工具 | 低频闲置风险 → 靠并入现有收尾流程化解；勿学 ponytail 单开命令 |
| F | 全仓 ranked 审计报告 | 不新增组件：`agents/code-reviewer.md` 简报支持对象=全仓即可 | 0~2 行 | 无（能力已有，只是从未这么派发过） |
| G | hook fail-open/timeout 审计 | `hooks/hooks.json` 逐条 | 0~3 行 | 无 |
| — | 代码库地图（map.js 思路） | **不整合** | DevDesk 已有 codegraph MCP 覆盖同一需求且更准（AST 级 vs 正则） | 引入即与 codegraph 双头 |

**先瘦后整 vs 边瘦边整：先瘦后整。** 理由：① need 103 裁掉的面（尤其 pool_mcp 工具合并、auto-continue 精简）会改变 E/G 的落点地形，先整会返工；② 候选 A–G 全部 ≤15 行级，瘦完再整成本几乎不变；③ ponytail 自身示范了这个顺序——v5 先删 53% 代码，然后才加 band。

---

## 7. 遗留 / 待验证

- `hooks/auto-continue.mjs`（691 行）行为未读——need 103 现场读，本报告只给「单文件最大疑点」定位。
- ponytail 本体从未出现在 Mac 的插件缓存（`~/.zcode/cli/plugins/cache/` 无 ponytail marketplace）：推断仅 Windows 机装过；待验证（Windows 机查 `%USERPROFILE%\.claude\plugins\cache\`）。DevDesk 项目 settings 对冲清理可循 memory audit 第 3 项一并处理。
- benchmark 数字采信作者自报（README 自述方法与结果文件，未第三方复核；方法脚本 `benchmarks/agentic/run.py` 31KB 未精读）。
- band 需 Claude Code ≥2.1.290；ZCode 侧对应能力未知——band 已列入排除，不影响结论。
- `/ponytail-gain` 成绩卡里的「Hidden checks passed 97%」与 README 正文「97%/96%」数字一致，但与 hero 图「98% risky logic」非同一指标（不同分母），引用时注意口径。

## 8. 来源

**精读**：README.md（webReader 整页）；`skills/ponytail/SKILL.md`(52 行全)、`ponytail-review`(101)、`ponytail-audit`(103)、`ponytail-debt`(44)、`ponytail-gain`(56)、`ponytail-help`(76)；`AGENTS.md`(33)；`plugin.json`/`plugin.yaml`/`skills/plugin.json`；`commands/ponytail.toml`、`ponytail-review.toml`；`hooks/claude-codex-hooks.json`(40 全)、`ponytail-instructions.js`(98 全)、`ponytail-subagent.js`(94 全)、`ponytail-map.js`(111 全)、`ponytail-activate.js`(前 70)、`ponytail-mode-tracker.js`(前 70)、`__init__.py`(前 70)。均经 `gh api repos/DietrichGebert/ponytail/contents/*`（main 分支 = v5.3.3）。

**扫描**：git tree 全量（文件尺寸）、repo stats / releases / npm versions API、benchmarks 目录、tests/、band UI、INSTALL/agent-portability/cursor-hooks、opencode/openclaw/pi-extension 适配层。

**本地**：`/Users/coderstory/dev/DevDesk/.claude/settings.json`、`settings.local.json`；`~/.zcode/cli/config.json`；`~/.zcode/cli/plugins/cache/`（ls）；`~/.zcode/cli/memories/projects/devdesk-7acb05266951a89c/memory/context-hygiene-audit.md`；devflow `/Users/coderstory/dev/zcode-plugins/devflow`（skills/agents/hooks wc -l + developer/code-reviewer/audit 三文件全文）。

**工具调用数自报：23 次**（简报预算 ≤40，未超）。
