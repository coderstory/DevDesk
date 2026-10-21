# 13-大纲-AUDIT-事实证伪

> 审计日期：2026-10-21 | 审计范围：`13-AI驱动开发实践报告-大纲.md` 全文

---

## 一、素材指针抽查（抽 18 个指针，覆盖率 > 1/3）

### 1.1 策略文档

| 指针 | 路径 | 状态 | 证据 |
|------|------|------|------|
| `docs/strategy/00-总览索引.md` | `DevDesk/docs/strategy/00-总览索引.md` | **CONFIRMED** | ls 5796 bytes, 2026-10-21 |
| `docs/strategy/01-业务功能清单.md` | `DevDesk/docs/strategy/01-业务功能清单.md` | **CONFIRMED** | ls 14373 bytes, 2026-10-21 |
| `docs/strategy/02-技术能力清单.md` | `DevDesk/docs/strategy/02-技术能力清单.md` | **CONFIRMED** | ls 7053 bytes, 2026-10-21 |
| `docs/strategy/05-技术路线图.md` | `DevDesk/docs/strategy/05-技术路线图.md` | **CONFIRMED** | ls 30425 bytes, 2026-10-21 |

### 1.2 跨目录指针（全部抽查）

| 指针 | 路径 | 状态 | 证据 |
|------|------|------|------|
| `.github/workflows/ci.yml` | `DevDesk/.github/workflows/ci.yml` | **CONFIRMED** | ls 4430 bytes |
| `memory/01~09`（9 个） | `/c/Users/.../.claude/memory/0[1-9]-*.md` | **CONFIRMED** | 全部存在，9 文件 |
| `cs-retro` 源码 | 大纲写 `D:\project\x-rs-tools\plugins\cs-retro\` | **存疑—路径错** | 实际路径 `D:\project\x-rs-tools\plugins\x-coding-tools\skills\cs-retro\`，缺 `x-coding-tools/skills` 中间两级。该目录下仅有 `SKILL.md`（3174 bytes），无源码文件 |
| `docs/superpowers/plans/2026-10-20-uat-round2-analysis.md` | 该路径 | **CONFIRMED** | 文件存在 |
| `round3-analysis.md` | 大纲写为裸名 `round3-analysis.md` | **存疑—缩写误导** | 实际文件名为 `2026-10-20-uat-round3-analysis.md`，文件存在但大纲引用名不完整 |
| 项目 MEMORY.md | `C:\Users\...\projects\D--project-gui\memory\MEMORY.md` | **CONFIRMED** | ls 3331 bytes |

### 1.3 CLAUDE.md 段引用

大纲引用 `CLAUDE.md`「项目概况」「功能开发生命周期」「交付流水线」「反事故红线」「已踩过的坑」「派单与并发」「交付前三问」「审计与决策口径」「常用命令」等段——**全部在 DevDesk/CLAUDE.md 中可找到对应段落**。

---

## 二、代表性事故实证抽查（10 条，大纲要求 8 条以上）

| # | 事故 | 状态 | 证据 |
|---|------|------|------|
| 1 | **f274f74 合并后 dev 无法 configure** | **CONFIRMED** | `git cat-file -t f274f74` → commit；CLAUDE.md:305 明确记载"f274f74 事故：审计在 worktree 跑闸、合并进 dev 后从未构建，dev 当场无法 configure，直到 1ad2ac9 补救"；1ad2ac9a commit 存在 |
| 2 | **真机 UAT 三连爆（1632 全绿当天）** | **CONFIRMED** | CLAUDE.md:357 完整记载三 bug（老库缺 context_note 列/星锐网关拒 [1M]/Windows 字体度量致弹窗叠字）；git log 有 context_note 修复提交（785eeb03, ec3432d6）、[1M] 修复提交（2b6e3615, 1e44f960 等）；memory/01 第 199 行同述 |
| 3 | **一次清出 12 个残留 worktree** | **CONFIRMED** | CLAUDE.md:306 "曾一次清出 12 个 EUI-NEO 残留 worktree" |
| 4 | **自定义 check 只 print 不 raise（7/7 ALL PASSED 假象）** | **存疑—归属问题** | CLAUDE.md:401 列入"已踩过的坑"；但 memory/02-anti-hallucination.md 源字段为 `feedback_cs-retro-test-check-must-raise`，原始事故在 cs-retro（x-rs-tools 项目）的 test_distill.py，非 DevDesk 本地事故——DevDesk 继承了这条教训但未在本项目复现此 bug |
| 5 | **星锐网关拒 [1M] 模型名** | **CONFIRMED** | git log 5+ 次命中（2b6e3615, 1e44f960, a2ab3690, 9f93b642, 2298becb, e98575aa），均 DevDesk 提交 |
| 6 | **两个 agent 同写 settings_module.cpp** | **CONFIRMED** | CLAUDE.md:293 "漏检让 R0 与 H2 同写 settings_module.cpp，只能 TaskStop 一条" |
| 7 | **标题栏无底色/详情面板 60% 空白靠用户肉眼发现** | **CONFIRMED** | CLAUDE.md:355 末尾原文："2026-09-28 的真实事故——标题栏无底色、详情面板 60% 空白、卡片缺边距、侧边栏边距改错层次，全都是用户肉眼先发现、我们事后才验证的" |
| 8 | **UAT6 追加消息两起漏做** | **CONFIRMED** | memory/03-ai-orchestration.md:131 源字段 "2026-10-21 DevDesk UAT6 两起漏做实证"；项目 MEMORY.md 亦记录 |
| 9 | **材质从未挂上却一行日志都没有** | **CONFIRMED** | CLAUDE.md:329 "`@catch { return false; }` 让材质从未挂上却一行日志都没有" |
| 10 | **手敲 cmake 造出多份漂移构建目录** | **CONFIRMED** | CLAUDE.md:47 "机器上同时存在 build/build-release/build-debug 三份手工构建目录，互相不一致" |

---

## 三、素材归属审计（重点）

大纲标题为「DevDesk AI 驱动开发实践报告」，但多处"实证"（尤其是第 5 章预防机制的"实证"条目）实际源自其他项目。逐条判定如下：

### 第 1 章

| 文本 | 归属判定 | 理由 |
|------|---------|------|
| "本文所有经验来自真实开发过程（2026-04 至今）" | **跨项目** | DevDesk git 首提交为 2026-09-20，不存在 2026-04 到 2026-09 的开发历史。"2026-04" 来自 rs-host/monitor/winui3 等兄弟项目的经验起点 |

### 第 5 章（预防机制——信息密度最高，归属问题最集中）

| 条目 / 实证 | 归属判定 | 理由 |
|------------|---------|------|
| 5.1 镜像复述实证 | **跨项目** | memory/01 源 "memory.backup-e2e/feedback/mirror-back-b"（global），DevDesk git 无 "镜像复述" 相关提交 |
| 5.1 只修需求交付物缺陷实证（"PDCA 审计中 AI 试图顺手修 8 项"） | **跨项目** | memory/01 源 "audit-historical-issues-no-fix.md"，标注 global；DevDesk git 无 "PDCA" 命中 |
| 5.1 加字段封装独立方法实证 | **跨项目** | memory/01 源 "feedback_add_field_independent_method.md"，标注 global |
| 5.1 scope-drift-audit 实证 | **跨项目** | memory/01 源 "feedback_scope_drift_audit_scope_only.md"，标注 global；DevDesk git 无 "scope-drift-audit" 命中 |
| 5.2 worktree 隔离实证 | **本项目** | CLAUDE.md:306 DevDesk 本地记载 |
| 5.2 同文件互斥实证（settings_module.cpp） | **本项目** | CLAUDE.md:293 DevDesk 本地记载 |
| 5.2 编译槽唯一条目 | **本项目+跨项目** | 机制源自多项目经验（memory/03），但 ccache 冲突实证来自 DevDesk CLAUDE.md「常用命令」段 |
| 5.2 worktree 用完即清实证（12 个残留） | **本项目** | CLAUDE.md:306 |
| 5.3 先红后绿实证（"自定义 check 只 print 不 raise"） | **跨项目** | 源事故在 cs-retro（x-rs-tools），DevDesk 只继承教训、未复现此 bug |
| 5.3 按场景写测试实证（真机 UAT 三连爆） | **本项目** | CLAUDE.md:357 |
| 5.3 智能体测试两段制实证（星锐网关拒 [1M]） | **本项目** | git log 多次命中，DevDesk 本地事故 |
| 5.4 禁止静默吞异常实证（材质从未挂上） | **本项目** | CLAUDE.md:329 |
| 5.4 平台对称性审计实证（"Windows 期间写的 macOS 代码从未编译过"） | **本项目** | CLAUDE.md:328 实证 |
| 5.5 禁止手敲 cmake 实证（三份漂移构建目录） | **本项目** | CLAUDE.md:47 |
| 5.5 追加消息对账实证（UAT6 两起漏做） | **本项目** | memory/03 源 "2026-10-21 DevDesk UAT6" |

### 第 6 章（自动复盘）

| 文本 | 归属判定 | 理由 |
|------|---------|------|
| "cs-retro 自动复盘" 整节 | **跨项目** | cs-retro 源码在 x-rs-tools 项目；DevDesk git 无 cs-retro 相关提交 |
| "cs-retro 在本项目实证偏少"（大纲自评） | **本项目承认** | 大纲第 349 行已自承——这是诚实的，但与全报告标题"DevDesk 实践报告"存在张力 |

### 第 7 章（AI 行为监控）

| 文本 | 归属判定 | 理由 |
|------|---------|------|
| PDCA 多轮递升审计 | **跨项目** | DevDesk git 无 PDCA 命中；PDCA 审计实现与实证来自 rs-host |
| 无人值守 watchdog | **跨项目** | DevDesk git 无 watchdog 命中；watchdog 机制来自 memory/03 "subagent-watchdog-v2-dynamic-registry.md"（global） |
| 追加消息对账（第 7 章重复引用） | **本项目** | UAT6 实证 |
| 独立审计关卡（六维度审计） | **本项目** | CLAUDE.md「功能开发生命周期」阶段 2 在 DevDesk 有完整定义 |
| scope-drift-audit（第 7 章引用） | **跨项目** | 源在 rs-host，DevDesk 无实证 |

### 归属总结

- **本项目实证（DevDesk 独有事故）**：f274f74 合并事故、UAT 三连爆、settings_module.cpp 冲突、标题栏无底色/60% 空白、12 个残留 worktree、材质静默失败、手敲 cmake 漂移构建目录、[1M] 模型名网关拒、UAT6 追加消息漏做、交付前三问、独立审计六维度
- **跨项目实证（非 DevDesk 原生）**：镜像复述、PDCA 审计、scope-drift-audit、自定义 check 只 print 不 raise、watchdog、长 build 期间 commit 吞编辑（winui3/monitor）
- **无法判定**：无

**核心问题**：报告标题和全文叙事定位为「DevDesk 实践报告」，但第 5 章约 40% 的"实证"和"事故"条目不是 DevDesk 项目自身的，而是 rs-host/winui3/monitor/x-rs-tools 等兄弟项目的。第 6 章和第 7 章也有跨项目实证混入。第 349 行自评已承认 cs-retro 实证不足，但此诚实未扩展到其他跨项目条目。

---

## 四、数字核实

### 4.1 "1632 条测试"

**存疑—数字可追溯但无法实时验证**

证据链完整：
- CLAUDE.md:357 明确写 "全量 1632 条测试全绿的当天"
- memory/01 第 199 行同写
- 项目 MEMORY.md 的 devdesk-delivery-standard.md 同写

但 CLAUDE.md 自身规定 "用例总数以运行输出 Total: 为准，不在文档写死"（L70），1632 是某个历史时刻的测试数，当前可能已变化。**建议正文改为"约 1600+ 条"或标明数据日期**。

### 4.2 "约 2000 次提交"

**存疑—近似正确但边际紧**

`git rev-list --count HEAD` = **1910**（2026-10-21 实测）。距 2000 差 90（约 4.7%）。"约 2000"可接受但偏乐观，**建议改为"约 1900+"或以实测为准**。

### 4.3 "6 个月持续迭代"

**REFUTED**

DevDesk git 历史：
- 首提交：2026-09-20 23:33
- 末提交：2026-10-21 12:57
- 跨度：**恰好 1 个月**（31 天）

不存在 2026-09-20 之前的 DevDesk 提交（已查所有分支）。"2026-04 至今"（第 1 章）和"6 个月持续迭代"（第 8 章）均严重失实——这些时间跨度适用于用户的整体 AI 驱动开发实践（rs-host 等项目从 4 月开始），但不适用于 DevDesk 本项目。

**必须改为"1 个月密集迭代"或明确区分"本项目 1 个月 / 整体实践 6 个月"。**

### 4.4 "114 条功能"

**存疑—近似值，计数方法不透明**

01-业务功能清单.md 统计：
- 首页仪表盘：10 条（全部可用）
- Markdown 笔记本：20 条可用 + 2 条规划中
- Todo 看板：15 条可用 + 1 条规划中
- Claude 配置与代理：14 条可用 + 1 条规划中
- 智能体：35 条可用（含 16 个工具项）+ 1 条规划中
- 设置：15 条可用
- 关于：2 条可用

总计：**111 条可用** + 5 条规划中。若工具项不拆开计则更少。距 114 差 3，"114"可能是某次计数的快照或含规划中项。**建议标明计数日期与口径，或改为"110+ 条"。**

### 4.5 "约 120 条可检索的经验条目"

**CONFIRMED——近似正确**

memory/01~09 九文件头+条目行计数约 125 条（按 `###` 和 `-` 行估算），与"约 120"吻合。**可保留。**

---

## 五、大纲夹带私货检查

### 5.1 cs-retro 路径错误

大纲 6 处引用 cs-retro 路径为 `D:\project\x-rs-tools\plugins\cs-retro\`，**该路径不存在**。实际路径为 `D:\project\x-rs-tools\plugins\x-coding-tools\skills\cs-retro\`，且目录下仅含 `SKILL.md`（3174 bytes），无大纲暗示的"源码"文件——cs-retro 的核心实现在 Python 包 `guance_cli` 和 MCP 工具中，不在该 skill 目录。

### 5.2 不存在的人名/事件

未发现大纲凭空引入人名或不存在的事件。所有事故在 CLAUDE.md 或 memory 文件中均可追溯。

### 5.3 "2026-04"的引入

大纲第 1 章写"2026-04 至今"，该日期在 DevDesk git 历史中无依据（最早提交 2026-09-20）。此日期源自用户整体 AI 开发实践的时间线（rs-host 等），在大纲里被无标注地归入 DevDesk 叙事，属于**夹带——跨项目时间线被当作本项目时间线使用**。

---

## 六、总结论

### 事实基础评级：**有必改项（不可直接拆章）**

**抽查统计**：
- 抽查素材指针：18 个
- 抽查事故：10 条
- CONFIRMED：22 项
- REFUTED：1 项（"6 个月持续迭代"）
- 存疑：5 项（cs-retro 路径、round3 文件名缩写、1632 测试数、2000 提交数、114 功能数）

**关键必改项（2 项）**：
1. **"6 个月"/"2026-04 至今"** —— DevDesk 实际 git 跨度仅 1 个月（2026-09-20 至 2026-10-21），必须修正为"1 个月密集迭代"或严格区分"本项目 1 个月/整体方法论实践 6 个月"
2. **跨项目实证无标注混入 DevDesk 叙事** —— 第 5 章约 40% 的实证条目（镜像复述、PDCA、scope-drift-audit、自定义 check raise、watchdog 等）源自 rs-host/x-rs-tools/winui3/monitor，在大纲里无区分标注。应在正文中对每个"实证"标注来源项目，或在章首加一段"以下机制经 DevDesk 及 rs-host 等兄弟项目共同验证"的诚实声明

**无硬伤可保留项**：功能清单（近似 114）、经验条目（约 120）、提交数（约 1900+，建议改"1900+"）、测试数（1632，建议标日期）、所有 CLAUDE.md 内事故引用

**文件产出**：本文件