# memcpp 夜间工作交接（2026-10-27）

- 交接对象：下一次全新会话（假设完全不知道这几天发生了什么）
- 工作树：`D:/wt-memcpp`，分支 `feat/memcpp`（全部工作已提交）
- 读者只需按本文给的路径读原文档，不要在这里找细节

## 读完这份文档，你应该先做什么

1. 路线已拍板：**A telemem 面板起步 + 预埋升级 C 两条缝**（七项定案见集成规格书 §0）；施工依据 = `docs/superpowers/specs/2026-10-27-memcpp-integration-spec.md`，先读其 §3 里程碑，从 S1 spike 五项实测开工。
2. 报告第 8 节的三件待拍板已被路线拍板吸收：①分发形态 → 集成规格定案 6（首跑安装、不捆绑解释器）；②模型资产 → 定案 1/4（提炼与向量化均走云端）；③注入通道 → 沿用 B4 §4 的 hook 静默档（`UserPromptSubmit` 预载 + 预算 ≤5 条 ≤500 字）。**P0 自研规格书已降级为触发线备用图纸，不要再按它开工**；S1 判据全绿则 A 独跑，触发线命中才按备用图纸升级 C（集成规格 §4）。
3. 动代码前先 `git -C D:/wt-memcpp log --oneline -3` 与 `git status` 对齐基线；`tools/memory-lab/experiments/` 已冻结，只能重跑不能改数据。

## 一句话现状

memcpp（把 Claude Code 会话消化成结构化记忆、并在合适时机注入的纯 C++ 单机工具）已完成可行性论证与独立审计，结论是**有条件可行**；目前有 Python 验证平台、冻结评测资产与 P0.5 风险探针（C++，`tools/memcpp-probe/`），**尚未写 P0 产品代码**，全部文档与实验代码都在 `feat/memcpp` 分支。

## 当前进度（2026-10-27 会话产出，供接续）

1. **P0.5 风险探针已落地**：单 exe 包体 35.19 MiB（exe 9.94 + 量化模型 25.25）、冷启动 p50≈92ms、峰值 RSS≈31.7MB；数字仅 Windows 实测（commit `d08f4ac7` + `6a8f8e0c`，资产在 `tools/memcpp-probe/`）。
2. **云端外发前置已达成**：脱敏层已实现，110 条红队样本泄漏 0 / 误杀 0（commit `a98e3f91` + `2f545b41`，全 suite 113 项通过）。
3. **提案卡片设计已就绪、待拍板**：推荐新建记忆页 + 逐条审核为默认、批量为辅助动作（commit `0f2f48a4` + `982b33d5`，见 `docs/superpowers/plans/2026-10-27-memcpp-proposal-card-design.md`）。
4. **模型许可证清单已核对**：风险低，义务=随分发物附三份 MIT 声明（commit `a3e47e57`，见 `docs/superpowers/research/2026-10-27-memcpp-model-license-checklist.md`）。
5. **业界实证调研已落盘**（commit `7a0dc7fc`，见 `docs/superpowers/research/2026-10-27-memcpp-prior-art-evidence.md`）：建议维持 Q8_0、中文双字分词不改、卡片人工审核属差异化设计。
6. **三件待拍板已被路线拍板吸收**：①分发形态 → 集成规格定案 6（首跑安装、不捆绑解释器）；②模型资产 → 定案 1/4（提炼与向量化均走云端，S1-5 验证网关 embedding 接口可用性与余额）；③注入通道 → 沿用 B4 §4 hook 静默档——原三件选项原文保留在下方「用户待拍板的三件事」段，仅作历史背景，不再等待拍板。
7. **路线已拍板、集成规格已出**：telemem 参照拆解与路线比选落盘（commit `4db88c6f`），`feat/memcpp` 已推送 origin 并并入 `dev`（merge `0ad2bc45`）；施工依据 = `docs/superpowers/specs/2026-10-27-memcpp-integration-spec.md`（S1 spike 五项实测先行，见下方拍板进度）。

## 本次交付物清单

| 路径 | 一句话用途 |
|---|---|
| `tools/memory-lab/` | Python 验证平台：真实会话增量摄取、清洗分段、云端抽取、本地向量化、SQLite FTS5；21 条单元测试全过 |
| `tools/memory-lab/experiments/` | 冻结评测资产：27 条记忆 + 15 条查询 + 四路评测脚本与结果，可重放 |
| `docs/superpowers/research/2026-10-27-memcpp-feasibility.md` | 可行性报告（修订版；结论与条件在第 1 节，修订记录在第 10 节） |
| `docs/superpowers/research/2026-10-27-memcpp-feasibility-audit.md` | 独立审计（3 blocker / 6 major），报告已按它逐条修订 |
| `docs/superpowers/research/2026-10-26-memcpp-borrowed-sources.md` | 可借开源源码清单（6 个仓库，含 commit sha 与许可证） |
| `docs/superpowers/specs/2026-10-26-memory-lab-design.md` | 实验台设计文档 |
| `docs/superpowers/plans/2026-10-26-memory-lab-overnight.md` | 夜间台账（A1-A3、H 完成；B/C/D/E/F 未做） |
| `D:/Data/desktop/memcpp技术方案.md` | 原始方案 v0.1（仓库外、**未修订**；直接照做已被审计否掉，必须按报告第 4 节改） |

## 已经确认的硬事实（下次会话不得重新踩）

1. **网关侧暂无有余额的 OpenAI 兼容 embedding 接口**：本机 10 个带凭据的网关配置（去重 4 个上游域名）没有任何“有余额且 OpenAI 兼容”的 embedding 接口；MiniMax 有原生端点但余额为 0。全云端向量化（集成规格定案 4）以此为前置，S1-5 验证可用性与余额，仍找不到则向量化档位需重议；本机备选 `BAAI/bge-small-zh-v1.5`（fastembed 实测 CPU 冷加载 0.9 秒、512 维）。
2. **云端抽取必须显式把 `thinking` 设为 `disabled`**：开启思考 + 4000 预算时 15 段只有 3 段成功，12 段思考吃满、正文 0 字。该结论限定在“这套抽取提示词 + 约 4KB 段”；短段短提示词下开思考也能成功，不要说成模型固有缺陷。
3. **SQLite FTS5 `trigram` 对 2 字中文查询返回 0 行**（端口/配置/模块），前缀 `*` 也无效。方案改为：入库前中文重叠双字分词 + `unicode61`，短查询或零命中用 `LIKE` 兜底；首版不写自定义分词器。
4. **云端外发必须过脱敏层**：脱敏层已实现并经 110 条红队样本验证（泄漏 0 / 误杀 0，见「当前进度」）；任何外发路径不得绕过脱敏直连网关（报告 §8.1）。
5. **主动注入证据弱**：离线强制注入 5 题只有 3 题明确采用项目记忆（1 题边界、1 题未采用）；hook 全链路一次未跑。官方支持 `type:"http"` hook 直连本地常驻服务（原方案未考虑，报告已补）。
6. **检索评测是 27 条记忆 / 15 个由记忆反推的问题**：FTS5 双字路 Recall@5 1.00 / MRR 0.9667 是天花板效应，不能当质量结论；扩样到 ≥100 条真实记忆、≥30 个真实问题前，不宣称检索可用。
7. **记忆污染/提示注入只有对策、没有红队验证**：来源分级、未复核隔离、“资料非指令”标注、按来源回滚都未实测；红队出现 1 次“注入内容被当事实或指令”即视为不通过。

## 下一步优先级（按报告第 6 节）

- 路线已拍板（A telemem 面板起步 + 预埋升级 C 两条缝，定案见集成规格书 §0）；施工依据 = `docs/superpowers/specs/2026-10-27-memcpp-integration-spec.md`，从 S1 spike 五项实测开工（票见其 §3）。
- 报告 §6.1 的 P0 自研阶段表已随路线拍板降级：P0 自研规格书仅作触发线备用图纸（集成规格 §4 触发线命中升级 C 时才按它施工），不再按其阶段表排活。
- 云端外发前置已达成：脱敏层 + 110 条红队样本泄漏 0 / 误杀 0（见「当前进度」）；记忆污染红队（报告 §5 门槛 9）仍未做。

## 用户待拍板的三件事（报告第 8 节，选项与推荐）

> 状态更新（2026-10-27）：三件事已被路线拍板吸收（映射见「当前进度」第 6 条与下方拍板进度段），本节仅作历史背景，不再等待拍板；模型资产与脱敏开关的最新口径以拍板进度段为准（全云端、脱敏默认开可关）。

1. **分发形态**：纯本地离线 / 本地 + 中转网关 / 私有化三档。**推荐“本地 + 中转网关”，前置条件是先实现脱敏并验证红队泄漏为 0**；脱敏不过关就只能选纯本地或私有化。
2. **模型资产**：embedding 推荐 `BAAI/bge-small-zh-v1.5` 量化版；抽取首版用云端 `qianfan/deepseek-v4.1-flash`（关闭思考），本地抽取模型等风险探针后再选。需要用户给两个约束：包体上限（如 <200MB / <500MB）、是否接受“首次使用下载模型”。模型许可证已核对（见 docs/superpowers/research/2026-10-27-memcpp-model-license-checklist.md）：许可证风险低，义务=随分发物附三份 MIT 声明。
3. **注入通道**：MCP 返回 / hook 拼上下文 / 两者结合 / 半自动档（卡片审核）。**推荐 hook 为主、MCP 为辅**（`UserPromptSubmit` 预载 + `PreToolUse` 按文件注入，预算 ≤5 条 / ≤500 字，失败静默跳过）；若更在意“不打扰”可缩到 2 条。P1 若 30 次真实抽样采纳率不到 30%，退回 MCP 优先。

**拍板进度（2026-10-27 更新）**：路线已拍板——**组合 1：A telemem 面板起步 + 预埋升级 C 的两条缝**（写入拦截点、检索接口）。七项定案：①提炼走云端大模型（复用 DevDesk 网关配置注入，qwen3:8b 本机档出局）；②脱敏默认开启、面板可关（关闭需二次确认并警示「敏感信息将原样出机」，红队验证资产保留、关闭态风险用户自担）；③外部依赖版本迭代不计风险；④向量化与提炼均走云端（OpenAI 兼容接口、网关注入，DevDesk 侧无任何本机模型组件；S1-5 验证网关 embedding 接口可用性与余额）；⑤卡片=DevDesk 侧记账（自家 proposals 表完整撤销链，代理执行 telemem 改删）；⑥Python 分发=首跑安装（面板检测缺 Python 3.10~3.12 引导，3.13 及以上在检测步即拦截，不捆绑）；⑦触发线：冻结集转测 telemem 检索 Recall@5<85% 或 2 字中文词低于基线 5/3/11 → 升级 C 补全文通道。P0 自研规格书（`docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md`）降级为触发线备用图纸；下一步从 S1 spike 开工（施工票见集成规格 §3）。

**端点定案（2026-10-27 追加拍板）**：提炼与向量化统一用**用户现用智谱 key**——智谱 paas/v4 OpenAI 兼容端点（base_url=`https://open.bigmodel.cn/api/paas/v4`，chat=`glm-5.3-flash`、embedding=`embedding-3` 显式 `dimensions=1024`，建索引前定死维度），key 自用户既有 Claude Code 配置运行时读取注入、绝不入库入日志。**消耗控制五条列为一等设计**：①新增内容去重水位、已入库零重复提炼；②提炼批处理节流（每小时上限可配置、默认保守值）；③月度 token 预算软/硬两档（硬档=暂停提炼只留检索）；④面板消耗页（今日/本月 token 与估算金额、最近调用记录）；⑤embedding 每条记忆仅算一次。逐条验收判据见集成规格 §0「端点与消耗控制」块。**S1 状态**：见 `docs/superpowers/plans/2026-10-27-memcpp-s1-spike-results.md`（①②通过，③④⑤ blocked 待端点）；端点定案后 ③④ 按该文档复跑口径补跑中（另一条线执行，勿重复开工）。

## 环境与纪律速查

- Python：`C:/Tools/Python/3.13/python.exe`；向量 venv：`D:/software/venvs/memory-lab/Scripts/python.exe`（含 fastembed 0.9.1）。
- 网关配置读 `~/.claude/settings.json`，Bearer 鉴权；公网走 `socks5h://127.0.0.1:10808`。
- 机器很慢：禁全量构建、禁全量数据扫描；重活后台跑并落日志；编译/测试子任务必须用 worktree 隔离。
- **不要改 `~/.claude/settings.json`**（由 DevDesk/cc-switch 管理）。
- `D:/wt-mcp` 是另一条会话的 MCP 骨架，不要碰。
- 构建走 `bash scripts/build-test-run.sh`；合并目标是 `dev` 分支（当前工作分支是 `feat/memcpp`）。
- 提交纪律：只改需求内文件，显式 pathspec，禁止 `git add -A`；不 push 除非用户明确要求。

## 建议下次会话调用的 skills

| 什么时候 | 调哪个 |
|---|---|
| 需求要重新对齐 | `superpowers:brainstorming` |
| 要出集成规格 S1~S5 施工计划 | `superpowers:writing-plans` |
| 按计划逐任务开发、每任务独立校验 | `superpowers:subagent-driven-development` |
| 写 C++ 代码或修缺陷，测试先行 | `mattpocock-skills:tdd` |
| 本轮收口、把踩坑沉淀成经验 | `x-coding-tools:cs-retro` |

## 已知未完成 / 未验证

共 **18 项**，这里不重复罗列，直接看报告第 7 节（本地抽取模型、llama.cpp 集成、hook 全链路、真实唤起率、记忆污染、成本记账、合并质量、断电恢复、多项目隔离、脱敏、网关合规、常驻进程安全、hook 版本漂移等）。任何一项验证完成后，结论都要按报告第 5 节门槛重新判定，不能沿用“有条件可行”。