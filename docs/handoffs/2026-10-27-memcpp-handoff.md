# memcpp 夜间工作交接（2026-10-27）

- 交接对象：下一次全新会话（假设完全不知道这几天发生了什么）
- 工作树：`D:/wt-mem/feat`，分支 `feat/memcpp`（全部工作已提交）
- 读者只需按本文给的路径读原文档，不要在这里找细节

## 读完这份文档，你应该先做什么

1. 读可行性报告第 1 节（结论与六个条件）和第 8 节（三件待拍板），把三件事摆给用户；**用户没拍板前不要写 P0 代码**。
2. 如果用户已拍板或明确让开写：先做 P0.5 风险探针（llama.cpp 静态链接 + 本地 embedding 装进单 exe）；任何云端外发之前，先实现脱敏层并做完红队验证。
3. 动代码前先 `git -C D:/wt-mem/feat log --oneline -3` 与 `git status` 对齐基线；`tools/memory-lab/experiments/` 已冻结，只能重跑不能改数据。

## 一句话现状

memcpp（把 Claude Code 会话消化成结构化记忆、并在合适时机注入的纯 C++ 单机工具）已完成可行性论证与独立审计，结论是**有条件可行**；目前只有 Python 验证平台与冻结评测资产，**没有写任何 C++ 产品代码**，全部文档与实验代码都在 `feat/memcpp` 分支。

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

1. **向量必须本地算**：本机 10 个带凭据的网关配置（去重 4 个上游域名）没有任何“有余额且 OpenAI 兼容”的 embedding 接口；MiniMax 有原生端点但余额为 0。可用 `BAAI/bge-small-zh-v1.5`（fastembed 实测 CPU 冷加载 0.9 秒、512 维）。
2. **云端抽取必须显式把 `thinking` 设为 `disabled`**：开启思考 + 4000 预算时 15 段只有 3 段成功，12 段思考吃满、正文 0 字。该结论限定在“这套抽取提示词 + 约 4KB 段”；短段短提示词下开思考也能成功，不要说成模型固有缺陷。
3. **SQLite FTS5 `trigram` 对 2 字中文查询返回 0 行**（端口/配置/模块），前缀 `*` 也无效。方案改为：入库前中文重叠双字分词 + `unicode61`，短查询或零命中用 `LIKE` 兜底；首版不写自定义分词器。
4. **当前云端外发没有任何脱敏实现**：实验管线会把 `tool_use`/`tool_result` 摘要、`system-reminder`、被引用的插件/技能文档一起发到公司共享网关；“只有脱敏片段外发”“原始工具 I/O 不出机器”都不成立（报告 §8.1 已按事实改写）。
5. **主动注入证据弱**：离线强制注入 5 题只有 3 题明确采用项目记忆（1 题边界、1 题未采用）；hook 全链路一次未跑。官方支持 `type:"http"` hook 直连本地常驻服务（原方案未考虑，报告已补）。
6. **检索评测是 27 条记忆 / 15 个由记忆反推的问题**：FTS5 双字路 Recall@5 1.00 / MRR 0.9667 是天花板效应，不能当质量结论；扩样到 ≥100 条真实记忆、≥30 个真实问题前，不宣称检索可用。
7. **记忆污染/提示注入只有对策、没有红队验证**：来源分级、未复核隔离、“资料非指令”标注、按来源回滚都未实测；红队出现 1 次“注入内容被当事实或指令”即视为不通过。

## 下一步优先级（按报告第 6 节）

- **先等用户拍板三件事**（见下节）；拍板前不写 P0 代码。
- 若开写：先做 **P0.5 风险探针**——llama.cpp 在 Windows/MinGW 下静态链接、量化 embedding 模型进单 exe，拿到包体、冷启动、内存三个数字；任何一项过不了，就当场决定降级为“本地向量 + 云端抽取”或纯全文检索。
- 之后按报告 §6.1 阶段表推进：P0 基础链路（增量尾读 jsonl、SQLite 存储、FTS5 双字 + `LIKE`、MCP `recall`、单 exe 三子命令骨架）→ P1 检索 + 注入（HTTP hook、隔离区）→ P2 后台整理 → P3 云端抽取硬化 → P4 可选。
- 任何云端外发前先做**脱敏层 + 100 条红队样本**（泄漏必须为 0）；记忆污染红队与它同批做。

## 用户待拍板的三件事（报告第 8 节，选项与推荐）

1. **分发形态**：纯本地离线 / 本地 + 中转网关 / 私有化三档。**推荐“本地 + 中转网关”，前置条件是先实现脱敏并验证红队泄漏为 0**；脱敏不过关就只能选纯本地或私有化。
2. **模型资产**：embedding 推荐 `BAAI/bge-small-zh-v1.5` 量化版；抽取首版用云端 `qianfan/deepseek-v4.1-flash`（关闭思考），本地抽取模型等风险探针后再选。需要用户给两个约束：包体上限（如 <200MB / <500MB）、是否接受“首次使用下载模型”。模型许可证已核对（见 docs/superpowers/research/2026-10-27-memcpp-model-license-checklist.md）：许可证风险低，义务=随分发物附三份 MIT 声明。
3. **注入通道**：MCP 返回 / hook 拼上下文 / 两者结合 / 半自动档（卡片审核）。**推荐 hook 为主、MCP 为辅**（`UserPromptSubmit` 预载 + `PreToolUse` 按文件注入，预算 ≤5 条 / ≤500 字，失败静默跳过）；若更在意“不打扰”可缩到 2 条。P1 若 30 次真实抽样采纳率不到 30%，退回 MCP 优先。

**拍板进度（2026-10-27）**：注入通道已有用户初步意向——双档并存（自动静默预载 + 半自动卡片审核，用户逐条接受/拒绝/修改），已录入报告 §8.3“半自动档”选项；仍待用户拍板具体交互形态（卡片放哪个页面、批量还是逐条）。分发形态、模型资产两件完全未拍板；三件未齐前不写 P0 代码。

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
| 三件拍板要改、要重新对齐需求 | `superpowers:brainstorming` |
| 拍板后要出 P0 实施计划 | `superpowers:writing-plans` |
| 按计划逐任务开发、每任务独立校验 | `superpowers:subagent-driven-development` |
| 写 C++ 代码或修缺陷，测试先行 | `mattpocock-skills:tdd` |
| 本轮收口、把踩坑沉淀成经验 | `x-coding-tools:cs-retro` |

## 已知未完成 / 未验证

共 **18 项**，这里不重复罗列，直接看报告第 7 节（本地抽取模型、llama.cpp 集成、hook 全链路、真实唤起率、记忆污染、成本记账、合并质量、断电恢复、多项目隔离、脱敏、网关合规、常驻进程安全、hook 版本漂移等）。任何一项验证完成后，结论都要按报告第 5 节门槛重新判定，不能沿用“有条件可行”。