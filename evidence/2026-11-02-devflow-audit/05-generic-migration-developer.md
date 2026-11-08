# devflow 审计⑤：通用能力迁移边界核对（developer 面）

- 日期：2026-11-02 ｜ 角色：developer 面 ｜ 对象：A 侧 /Users/coderstory/dev/DevDesk（AGENTS.md / CLAUDE.md / DOCS-CONVENTIONS.md / .zcode/ / .agents/skills 42 个）⇄ B 侧 /Users/coderstory/dev/zcode-plugins/devflow（skills×11 / agents×7 / setup 模板×5 / README / pool-mcp-acceptance.py）
- 判据：skill-quality-baseline v2 之 C.9（环境即事实源）、C.7（co-location/防 sprawl）、D.3（机器/项目特定残留）、A3.9（同名遮蔽）、F.4（单真相源）、A3.5/A7.4（description 共享预算，104 技能同场）
- 只读审计；唯一写动作 = 本文件。工具调用共 26 次（预算 35）。

---

## 轴① 正向迁移（项目 → 插件）

### 已覆盖项（项目侧仅为实例化，维持现状）

| 项目侧内容 | 已覆盖于（插件文件） |
|---|---|
| AGENTS.md §五 成本纪律全文（定向不全量 / 阶段档 / 同环节不重复构建 / 断言与证据物） | developer.md 流程 6、qa-tester.md 流程 5、ask 收口节（同句「验证的价值在断言与证据物，不在跑的次数」已入插件） |
| AGENTS.md §四 检索纪律（codegraph 优先 / grep 非代码兜底 / 先 init） | developer.md 流程 1、bug-fixer.md 铁律 1、researcher.md 边界 |
| CLAUDE.md 测试纪律（三问 / 会红 / 格子明账） | qa-tester.md 价值观 + 流程 0 + 反模式速查 |
| AGENTS.md §八 commit 前缀（机制部分） | ask 收口节（wip: 前缀 / 禁裸 add -A）；前缀词表本身=项目事实留原地 |

**[P2] AGENTS.md §五成本纪律段 — 插件已载规则全文，项目侧又整段复述，双处维护有漂移面 — 证据：AGENTS.md §五「全量 doctest+probe+守卫链只在阶段节点跑…」与 developer.md 流程 6、qa-tester.md 流程 5 逐句同义 — 建议：项目侧压缩为「档位映射 + 指针」（--dev=开发内循环、无 filter 纯构建等脚本事实保留，纪律句指向插件）**

### 未覆盖的通用项（三元组：内容 → 目标文件 → 迁移形态）

**[P1] AGENTS.md §十需求全流程纪律（2026-10-31 老板令） — 「需求产物与技术方案卡各自都要过一次独立审计（不承担该需求开发的角色评审），过了才进下一环节」只写在项目侧；插件 ask 定级表 M 链仅「reviewer / qa 抽查」、L 链无 PRD/tech 卡审计门，audit skill 有载体但 ask 未强制 —— 只按插件流程走的会话会跳过产物审计，且两处口径分叉（F.4）— 证据：AGENTS.md §十「每条需求走完整流程 = 需求分析 → 调研 → 技术方案 → 开发 → 测试 → 审计；需求产物与技术方案卡各自都要过一次独立审计」；ask SKILL.md 定级表 M 行无审计环节 — 建议 → `skills/ask/SKILL.md` 定级表/收口节 → 规则句（M/L 流程插入「PRD 与 tech 卡各过一次独立审计」硬检查点，复用 audit skill §1 选面表）**

**[P2] AGENTS.md §八「不要阶段性汇报：完成一个小阶段就停下汇报会被用户明确否定；把整个任务做完、测试跑绿再一次性汇报」— 通用协作规则，插件只约束了报告形态（结论先行 / 极简）未约束汇报时机 — 证据：AGENTS.md §八原文；ask 过程管控节无对应句 — 建议 → `skills/ask/SKILL.md` 过程管控节 → 规则句一句**

**[P2] CLAUDE.md「交付前三问」（存量数据升级路径验过吗 / 真实外网网关验过吗 / 目标平台真机验过吗，答不上写「未验证」不许沉默）— 通用交付纪律，qa-tester 已有「存量数据迁移」单点但未成套 — 证据：CLAUDE.md 阶段 4 交付前三问段（1632 全绿当天真机三连爆的教训成型）— 建议 → `agents/qa-tester.md` 流程 0 或输出节 → 规则句三问**

**裁决留原地（项目特有，明说不迁移）**：AGENTS.md §一/§二（submodule 布局与双机同步）、§三（平台硬约束与 #ifdef 禁令）、§四排除清单、§六架构 4 层、§七编辑边界、§九文档入口表；CLAUDE.md 构建/工具链/EUI-NEO 政策/已踩过的坑；cheatsheets/platform-file-map.md；lessons 的代码级坑（json asInt、fwrite 短写、bundle 陈旧）；armory 门禁项目实例；DOCS-CONVENTIONS 目录地图本体。AGENTS.md §八双机同步、§十检查点提交（模板 agents-md-section.md 有同款槽位，项目填充即实例）均属项目事实。

### 项目侧不该再持有（回收类）

**[P1] CLAUDE.md「派单与并发」— 六类 devdesk-* 子代理指向不存在的 `.zcode/agents/`（现 .zcode 仅 armory/cheatsheets/config.json/lessons），且与同文件尾部「角色词汇以 devflow 插件六角色为准」内部矛盾；「交付流水线四律」「功能开发生命周期六阶段」「审计与决策口径」与插件 ask/developer/qa 流程构成双真相源（F.4/C.7 sprawl）— 证据：CLAUDE.md:302「子任务一律用项目根定义的六类子智能体创建（位于 `.zcode/agents/`…devdesk-builder…）」 vs CLAUDE.md:429「角色词汇以 devflow 插件六角色为准」；.zcode 实际目录清单无 agents/ — 建议：CLAUDE.md 协作纪律段整体回收为指针（流程权威 = devflow 插件 + AGENTS.md §十），仅保留项目环境事实（构建槽独占、worktree 纪律、DEVDESK_JOBS、pkill 红线等）**

**[P1] CLAUDE.md「视觉规范」— 与插件 ui-ux-designer 设计语言方向相反，双权威冲突：项目侧仍载「主品牌色 Claude Clay #D97757、配色方案 B」并引 spec §6.5.1/6.5.2；插件侧载「绿色微信风、旧橙色系风格已废弃列为禁止项」且实现档 #36D399 来自 DevDesk theme.h 现行 token — 证据：CLAUDE.md「主品牌色：Claude Clay `#D97757`」 vs agents/ui-ux-designer.md:23「旧橙色系风格已废弃，列为禁止项」 — 建议：二选一定权威——若绿色微信风是 DevDesk 真口径，回收 CLAUDE.md 视觉段为指针（权威 = ui-ux-designer + 项目 UI spec）；若 Claude Clay 仍是品牌口径，则插件「旧橙禁令」须撤（对 DevDesk 构成误伤）。报告不代裁，须用户拍板**

**[P2] AGENTS.md §十 Agent 评测任务集与插件 consolidate 步骤 4 — 同一套「失败入库 / 连续 5 次毕业」规则两套载体（项目 docs/research/agent-eval-tasks.md vs 插件 docs/eval-tasks.md + docs/eval-log.md），分工未写明 — 证据：AGENTS.md §十「真实失败当次入库、连续 5 次全过毕业」 vs consolidate「毕业 = 连续 5 条 PASS…载体 docs/eval-log.md」 — 建议：项目侧一行写明分工（插件 skill 评测归插件 eval 体系；项目 agent 行为评测归 docs/research/agent-eval-tasks.md），不合并**

**[P2] .zcode/lessons/{code-reviewer,developer,qa-tester}.md 各含一条 2026-10-31 测试纪律行 — 与插件 qa-tester.md 价值观近乎逐字重复，违反 lessons 自身「与既有规则重复不记」头部规则 — 证据：lessons/developer.md「测试禁形式主义/覆盖率导向——每条用例必须防『特定功能×特定场景×特定边界』…会红性铁律=删被测代码必红」 ≈ qa-tester.md 价值观段 — 建议：三行删除或改一句引用（保留日期与来源的指针行）**

**[P2] .zcode/armory/evidence-capture.sh — 硬编码本机插件 clone 绝对路径，双机必断（Windows 机 clone 路径不同）；且缺模板后加的断链自检 — 证据：`exec bash /Users/coderstory/dev/zcode-plugins/devflow/scripts/evidence-capture.sh "$@"` — 建议：双机各自跑 devflow-setup 幂等重生成（顺带回灌 D3 的自检块），AGENTS.md §十一补一句双机口径**

---

## 轴② 反向泄漏（插件 → 项目专有内容）

**[P1] agents/qa-tester.md 价值观节（约:23）— 写死台账 slug `2026-10-31-testing-discipline-policy` 为「上位依据（老板口径优先）」，实测 `doc_get(slug=…)` 返回「文档不存在」，doc_search『测试纪律』亦无变体（DevDesk 真权威是仓库文件 docs/TESTING-DISCIPLINE.md，非台账卡）——死引用 + 项目专有 slug 进通用 agent — 证据：qa-tester.md「**上位依据**：doc_get(slug=2026-10-31-testing-discipline-policy)（老板口径优先，冲突以此为准）」；实测 error「文档不存在（slug=2026-10-31-testing-discipline-policy）」 — 建议：改为「上位依据 = 工作区 AGENTS.md 及其指定的项目测试纪律文档」，删具体 slug；行为有内联规则兜底故定 P1 不升 P0**

**[P1] agents/ui-ux-designer.md:23 — 项目名与项目设计史硬编码进通用 agent：「仅 gui 现行深色皮肤沿用」（gui=DevDesk 旧工作区壳名，机器/项目特定残留 D.3）、「旧橙色系风格已废弃，列为禁止项」（DevDesk 专属设计决策，且与 DevDesk CLAUDE.md 现行文字冲突，见轴①）— 证据：原文摘录见轴① A2 — 建议：「深底档语境」与「旧橙禁令」移项目侧（AGENTS.md 或 UI spec），插件句改为中性「深色档仅项目明确沿用暗色皮肤时使用；设计语言冲突以工作区 AGENTS.md 为准」**

**[P1] skills/ui-probe-shot/SKILL.md（插件版）— 把 DevDesk 构建事实当通用事实复述（C.9 cache 会腐烂）：「项目 tests/probes/ 已有底座直接用（探针文件放目录即被 glob 收编、零注册，运行名=文件名去 .cpp）」是 DevDesk CMake GLOB+LABELS 行为；parseProbeArgs/pollOnce/compose/assertDrawnAbove/assertNoEqualZOverlap 等 API 名全为 DevDesk/EUI-NEO 专有，无「示例」标注 — 证据：SKILL.md:7 与:16 原文；另一项目按此找 glob 收编会落空 — 建议：「glob 收编/零注册」改条件句「若项目探针目录支持零注册 glob 直接用；否则按项目构建体系注册」，API 名前加「示例取自既有项目 harness，以项目现成探针为准」（后半句已有，补前半）**

**[P2] skills/zcode-dispatch/SKILL.md（插件版）— 个人通道 skill 带本机事实：/Applications/ZCode.app 绝对路径 ×2、~/.dsh、~/.claude.json、夜间窗口 23:00-09:00 套餐口径 — 证据：入口节原文「node /Applications/ZCode.app/Contents/Resources/glm/zcode.cjs」 — 建议：插件定位即个人插件（README 明示私有），可留；但机器路径宜标「本机口径」集中一处，双机差异（Windows 无 /Applications）留一行待验标注**

**[P2] skills/pool-usage/SKILL.md:14-16 + README.md:76 — 引用 DevDesk 专名全局池 `mcp__devdesk-pool__*` 作「双池并存」区分段 — 已知并存豁免（docs/archive/2026-10-31-pool-mcp-retro.md：「并存未退役，防串账条款已入 pool-usage」），非新发现；但退役时点到即删 — 证据：README:76「同会话若还挂有 devdesk 全局池…两者是不同 DB」 — 建议：随 devdesk-pool 全局注册退役同步删除两处；并核实「另一套 DB」说法与本地旧 skill「两边打同一个 docs/pool/pool.db」矛盾（见轴③ C3）**

**[P2] tests/pool-mcp-acceptance.py:2-4、:9 — docstring 引「DevDesk 决策文档 §7 编号空间」并指路 `docs/specs/2026-10-31-pool-mcp-and-efficiency.md`（本仓无 docs/specs/ 目录，实文件在 docs/archive/ 同名——死链）；运行示例写死 `DevDesk tools/.pool-mcp-venv/bin/python`（Windows 机无此路径）— 证据：原文见 :2-4/:9 — 建议：路径改 `docs/archive/2026-10-31-pool-mcp-and-efficiency.md`；示例改「任一装有 mcp SDK 的解释器（如某项目内 venv）」**

**[P2] tools/pool_mcp.py:2 docstring「业务层自 DevDesk tools/pool_mcp.py 原样搬运」、tests/eval-run.sh:181「专项预算（DevDesk 迁移 3 skill，池#28）」— 出处/历史注释进运行文本（D.3：出处归 git 与 docs）— 建议：低优先，docstring 出处句移 README 版本历史或删；eval-run.sh 注释可留（预算台账线索）**

**[P2] agents/researcher.md「两档」节 — 落盘路径写死 `docs/research/YYYY-MM-DD-<slug>.md`（这是 DevDesk 目录地图；docs-conventions-template.md 并未固定调研目录路径）— 建议：改「落盘项目文档地图约定的调研目录，命名 YYYY-MM-DD-<slug>.md」**

**developer.md 核对结论（简报点名项）**：DevDesk 构建命令已清干净——现为「构建走项目内循环形态（工作区 AGENTS.md 指定的开发档命令，裸调全量档属越权）」，全 agent 无 cmake/build-test-run/MSYS2/DevDesk 路径残留；grep 全仓命中面即上列各条（monitor/dist 为上游 minified 产物、plugin.json "coderstory" 为市场作者命名空间，均不计）。

---

## 轴③ 副本与遮蔽（.agents/skills 42 个）

**[P1] daily-wrapup-discipline 本地副本 — 应删（插件已覆盖且更新）。本地为旧代：步 3 对账对象写「TODO.md 与 pool.db」（TODO.md 已于 2026-10-31 退役）、词汇「群聊/老板/团队记忆/docs/reports/」；插件版已池化（pool_add/pool_update(note)/pool_stats）并加触发 description — 证据：diff 本地:12「核对多处记录（如 TODO.md 与 pool.db）」 vs 插件:13「dev_progress note、docs/eval-log.md…」 — 裁决：删本地目录**

**[P1] zcode-dispatch 本地副本 — 应删（插件已覆盖且更新）。本地为旧代：「全队最高口径（老板 2026-10-30 令）」「禁用 claude」、任务书写死 DevDesk 路径 `docs/plans/<slug>-task.md`、`tools/zcode_provider_switch.py`；插件版已泛化并加「devflow 语境适配」节 — 证据：diff 本地:8/18/26 原文 — 裁决：删本地目录**

**[P1] devdesk-pool-usage 本地副本 — 应删 + 修指针。本地教的是旧 devdesk-pool 全局 MCP 挂载（venv/pool_ops.py/§6.5 表结构），与插件 pool-usage 口径相反：本地「stdio 跑 tools/.pool-mcp-venv/bin/python…两边打同一个 docs/pool/pool.db」 vs 插件/README「两者是不同 DB，devflow 流程禁用 devdesk-pool」；且 CLAUDE.md:429 仍指引「见 .agents/skills/devdesk-pool-usage」 — 证据：本地挂载现状节原文 vs pool-usage:16 — 裁决：删本地目录；CLAUDE.md:429 指针改 devflow pool-usage；「同一 DB 与否」两说以实机 config 核实后统一表述**

**[P1] ui-probe-shot 本地副本 — 应删（残余特化事实并回项目文档）。本地是 DevDesk 特化超集（uat62_page_shots_probe 批拍、DEVDESK_BUILD_PROBES=OFF、CMake GLOB 自动 add_test、--screenshot-seeded/-publish、Windows 待验段），插件版是泛化方法；按 consolidate 泛化条款「提取进插件后项目原实现随即退役、只留适配点，禁止双存」— 证据：diff 本地 10 坑位/参考探针表 vs 插件 5 坑位合并版 — 裁决：删本地；批拍探针名、`ctest -L probe`、DEVDESK_BUILD_PROBES 三项若 CLAUDE.md 阶段 4 未载则并入（现已大半在载）**

**[P2] devdesk-discipline 本地副本 — 应保留但瘦身。无插件同名对应，属项目特化纪律速查；但 10 条中约 6 条是插件规则的项目实例化（构建唯一入口/先红后绿/复现优先/平台口径/定向不全量），且含需对账的时效项（第 7 条已知跳过清单、第 9 条自制迷你 doctest 限制、第 10 条 bundle 陷阱——lessons/qa-tester.md 标注 bundle 问题已根治）— 证据：SKILL.md 全文（见 diff 批次输出） — 裁决：保留，删规则句只留 AGENTS.md 没有的速查事实（迷你 doctest 限制、已知跳过清单、DEVDESK_DATA_ROOT 隔离），并与 lessons 去重**

**[P2] mattpocock 系副本 37 个 — 同名 27 个建议删：ask-matt、code-review、codebase-design、diagnosing-bugs、domain-modeling、grill-me、grill-with-docs、grilling、handoff、implement、implement-spec、improve-codebase-architecture、pr、prototype、research、retro、setup-matt-pocock-skills、tdd、teach、to-questionnaire、to-spec、to-tickets、triage、wait-what、wayfinder、wizard、writing-for-agents——与已装 mattpocock-skills 1.3.1 逐一同名（A3.9 纯遮蔽重复，占 104 技能共享 description 预算，且旧本地版可能遮蔽插件更新版）；其余 10 个为插件未收录的 mattpocock 系（writing-beats、writing-fragments、writing-shape、scaffold-exercises、migrate-to-shoehorn、setup-ts-deep-modules、setup-pre-commit、loop-me、claude-handoff、git-guardrails-claude-code），无冲突，按需保留 — 建议：删 27 留 10；本条按简报要求只报数量与建议，未逐篇 diff**

---

## 轴④ 模板一致性（devflow-setup assets ⇄ DevDesk 实际）

**[P1] docs-conventions-template.md ⇄ DOCS-CONVENTIONS.md — 模板明文「`-tests` / `-retro` 后缀已废弃（职能移交 dev_progress / acceptance 子表，note 即 markdown 文本块）」，qa-tester.md 流程 0 同口径（清单全文以 pool_update(note) 入 dev_progress）；但 DevDesk 目录地图仍把两后缀列活文档 — 证据：DOCS-CONVENTIONS.md:9-10「测试清单卡 `docs/superpowers/plans/<slug>-tests.md`｜长期」「总结会纪要 `<slug>-retro.md`｜长期」 vs 模板废弃条款 — 裁决：项目该回收（地图两行改「dev_progress note；历史 <slug>-tests/-retro 卡只读归档」），模板不回调——插件流程已按 note 载体运行**

**[P2] gate-template.sh / evidence-wrapper.sh ⇄ DevDesk 实例 — 模板后演进未回灌：模板 gate 有「`{{` 残留自检（未填充即阻断）」块，DevDesk pre-tool-use.sh 无；模板 wrapper 有断链自检（找不到插件脚本 exit 2 并提示重跑 setup），DevDesk evidence-capture.sh 为裸 exec 无自检 — 证据：gate-template.sh:7-9 vs pre-tool-use.sh 全文；evidence-wrapper.sh:6-9 vs evidence-capture.sh 全文 — 建议：项目实例回灌两个自检块（或重跑 devflow-setup 幂等重生成，一并对齐轴① A5）**

**达标项（明说未发现漂移）**：agents-md-section.md 骨架 ⇄ AGENTS.md §十——指针骨架+项目槽位结构一致，DevDesk 增补 bullets 属模板允许的「本项目特有」，构建入口/卡片路径两槽由 §五/§九分节承载语义等价；config-template.json ⇄ .zcode/config.json——字段形态（type/matcher/timeoutMs/statusMessage）完全对齐，DevDesk 多出的 plugins.enabledPlugins 为项目侧扩展非漂移。

---

## 计数与迁移清单汇总

**P0：0（未发现——各缺陷均有兜底或属文档/速查层失效）｜P1：10｜P2：13**

| 类别 | 条数 | 明细 |
|---|---|---|
| 正向迁移（项目→插件） | 3 | F1 需求产物独立审计→ask（P1）；F2 禁阶段性汇报→ask（P2）；F3 交付前三问→qa-tester（P2）。另 F4 注释纪律完整版裁决留原地 |
| 项目内回收（含双权威消解） | 4 | A1 CLAUDE.md 协作段回收指针（P1）；A2 视觉规范双权威待拍板（P1）；A3 评测集分工写明（P2）；A4 lessons 去重（P2）；A5 wrapper 双机重生成（P2） |
| 反向清理（插件去项目残留） | 7 | B1 qa-tester 死 slug（P1）；B2 ui-ux-designer gui/旧橙（P1）；B3 ui-probe-shot glob/API 名（P1）；B4 zcode-dispatch 机器路径（P2）；B5 pool-usage/README devdesk-pool（P2，退役即删）；B6 acceptance.py 死链+venv（P2）；B7 pool_mcp.py/eval-run.sh 出处注释 + researcher 落盘路径（P2×2） |
| 副本裁决 | 6 | 删 4：daily-wrapup-discipline、zcode-dispatch、devdesk-pool-usage（连带修 CLAUDE.md:429 指针）、ui-probe-shot；保留瘦身 1：devdesk-discipline；mattpocock 系删 27 留 10（P2） |
| 模板一致性 | 2 | D1 项目回收 -tests/-retro 行（P1）；D2 回灌 gate/wrapper 自检块（P2）；骨架与 config 达标 |

**未验证项**：①「devdesk-pool 与插件池是否同 DB」两处矛盾表述未实测（需查全局 ~/.zcode/cli/config.json 的 devdesk-pool 注册与 POOL_DB 指向）；② Windows 机上 armory wrapper、hooks 的实际断链行为（本机 macOS 无法代验，与基线遗留 5 同类）；③ A2 设计语言权威归属须用户拍板，报告只给证据不代裁。

（禁做项未触碰：除本文件外未写任何仓库文件；简报「已验证」事实全部沿用未重查。）
