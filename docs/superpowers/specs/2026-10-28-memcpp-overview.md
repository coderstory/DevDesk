# memcpp 需求与技术方案总览（一页入口）

- 日期：2026-10-28。读者：项目负责人与任何新接手的开发；零黑话，术语首现就地解释。
- 定位：本文是 memcpp（记忆功能）的需求清单、技术方案终版、实测数字与验收状态的唯一入口；细节按出处路径读原文。
- 现状一句话：功能与界面代码全部落地；端到端验收已通过（2026-10-28 e2e-verify 修 5 缺陷后判「可交付」，至 commit `9ee92079`）；里程碑审计 9 findings 已逐条处置（F1 产品接线、F2 posix 环境、F3 UUID 形态、F5-F8 中低危代码项已修，F4/F9 文档口径随本批落，逐条见审计文件处置栏 `plans/2026-10-28-memcpp-milestone-audit.md`）；并入 `dev` 与用户真机试用（UAT）未开始。
- 上游规格：`docs/superpowers/specs/2026-10-27-memcpp-integration-spec.md`（下称「集成规格」）；本文与其冲突时以集成规格 §0 已拍板定案为准，实现偏差在 §5/§6 如实列出。

## 1. 需求全清单

七条功能需求，每条给「来源 → 实现形态 → 验证物」三列映射。术语：**记忆** = 一条可复用的结论（如「构建走 build-test-run.sh」），不是聊天原文；**telemem** = 第三方 Python 长期记忆库（Apache-2.0），负责存取与检索。

| # | 需求 | 来源 | 实现形态（模块 file） | 验证物 |
|---|---|---|---|---|
| 1 | 记忆面板管理：telemem 的安装、配置、启动/停止、健康检查，配「主控台 / 记忆审核 / 配置」三视图 | 用户 2026-10-27 拍板「组合 1：A telemem 面板起步」（集成规格 §0/§1）；界面稿 `docs/mockups/memcpp-panel-demo.html` | `src/module_memory/engine/telemem_engine.h`（托管编排）+ `engine/python_locator.h`/`engine/telemem_process.h`/`engine/llama_host.h`/`engine/telemem_config.h` + `panel/`（三视图）+ `memory_module.h`（页面注册） | 实测报告 `docs/superpowers/plans/2026-10-28-memcpp-s2-live-verify.md`（五项全过）；单测 `tests/test_modules/test_memory_engine.cpp`；三视图截图 `.verify-screenshots/memory-{1,2,3}-*.png` |
| 2 | 自动采集：把 Claude Code 会话文件（jsonl，每行一个 JSON 对象的文本）增量读成候选记忆段 | 可行性报告 §2.1（Claude Code 已把完整会话写进 jsonl，增量尾读最可靠）`docs/superpowers/research/2026-10-27-memcpp-feasibility.md` | `src/module_memory/collector/session_reader.h`（四道过滤）+ `collector/collector.h`（4KB 分段 + 水位幂等：同一段重复读只入库一次） | 单测 `tests/test_modules/test_memory_collector.cpp`（49 项；四道过滤/截断/水位逐一钉死） |
| 3 | 脱敏：任何数据出机（发给云端）前清洗手机号/邮箱/身份证/内网 IP/带参 URL/密钥六类敏感信息；默认开、面板可关（关闭需二次确认并警示「敏感信息将原样出机」） | 可行性报告 §1 条件 2 + 集成规格定案 2 | `src/module_memory/sanitize/sanitize_gate.h`（C++ 移植，fail-closed：拿不准一律脱敏；非法输入直接中断外发） | 单测 `tests/test_modules/test_memory_sanitize.cpp`：红队 110 条攻击样本泄漏 0、误杀 0；运行时开关与二次确认在面板配置页（截图 memory-3-config.png） |
| 4 | 注入预算：用户每次提问时，hook（Claude Code 的固定时机回调）把相关记忆塞进上下文，至多 5 条、合计 500 字，失败只丢注入、绝不打断提问 | 可行性报告 §8.3 + 提案卡片设计 §4（B4）+ hook 契约钉版 `docs/superpowers/research/2026-10-27-memcpp-hook-contract-facts.md` | `src/module_memory/hook/inject.h`（预算拼装）+ `hook/hook_command.h`（`devdesk.exe memory hook` 子命令）+ `hook/hooks_settings.h`（hooks 配置键级装拆） | 单测 `tests/test_modules/test_memory_hook.cpp`（61 项；additionalContext 字段位置、任何错误 exit 0、绝不产生退出码 2） |
| 5 | 卡片人审：入库前先出提案卡，用户接受/拒绝/编辑后接受/撤销四操作；撤销留痕可再审 | 用户 2026-10-27 拍板意向「部分手动的方式，提供特定的卡片让用户选择」（B4 需求来源）+ 集成规格定案 5；设计全文 `docs/superpowers/plans/2026-10-27-memcpp-proposal-card-design.md` | `src/module_memory/proposals/proposal_store.h`（提案表 16 字段、状态机、undoOf 撤销链、baseVersion 防覆盖校验）+ `proposals/curator.h`（整理器：新增必审/遗忘扫描）+ `proposals/curator.h` 的 `executeProposal`（接受后代理执行引擎动作） | 单测 `tests/test_modules/test_memory_proposals.cpp`（105 项）；审核页四操作/批量/过滤/空态截图 memory-2-review.png |
| 6 | 消耗可控：控云端花销的五条措施（见 §2.3 逐条现状） | 2026-10-27 追加定案（集成规格 §0「端点与消耗控制」块，五条即验收判据） | `src/module_memory/proposals/usage_ledger.h`（token/金额账本）+ `engine/token_usage.h`（月度预算软/硬档判定）+ 面板消耗卡 | 探针账面断言（commit `032506b0`：预算环/四格/卡流）；消耗卡直读账本（commit `214f9043`）。注意 token 为估算口径（见 §5） |
| 7 | 触发线兜底：检索质量若不达标，按预先写死的数字升级检索通道，治理层零返工 | 集成规格定案 7 + §4（触发线口径预写死） | `src/module_memory/gateway/telemem_client.h`（检索接口缝：调用方只依赖 `IMemoryRetriever` 内部接口，不直连引擎——换后端调用方零改动）+ `panel/recall_eval.h`（冻结 15 条查询的评测单元，驱动主控台红线仪表） | recall_eval 单测；评测出数待真机 UAT（当前不构成升级判定） |

## 2. 技术方案终版

### 2.1 架构一图（文字）

```
Claude Code 会话文件（jsonl）
   │ ① 采集：增量尾读 + 四道过滤 + 4KB 分段（collector/）
   ▼
② 脱敏闸（默认开；面板可关，二次确认）（sanitize/）
   ▼
③ 提案库：候选记忆先落「待审」卡，不静默入库（proposals/）
   │        用户接受 → 代理执行（proposals/curator.h executeProposal）
   ▼
④ 引擎（telemem + 本地向量服务，均为 DevDesk 托管的常驻进程）
   │    telemem：MCP HTTP 只绑回环 127.0.0.1:8421；提炼走云端 LLM（见 2.2）
   │    llama-server：本机向量化（embedding = 把文字转成可比对的数字串），
   │    回环 127.0.0.1:11435，模型 bge-small-zh-v1.5
   ▼
⑤ hook 同步注入（hook/）：UserPromptSubmit 预载，≤5 条 ≤500 字，失败开放
   │    壳不直连引擎，只调「检索接口缝」IMemoryRetriever（gateway/）——
   │    升级检索通道时换实现，调用方零改动
   ▼
⑥ 面板三视图（panel/）：主控台（脱敏/评测仪表/注入预算/存储四格）
     记忆审核（提案四操作+批量+过滤） / 配置（脱敏开关/阈值/导出/清除）
```

### 2.2 端点与模型（当前决定 + 出处）

| 项 | 当前决定 | 出处 |
|---|---|---|
| 提炼 LLM | 智谱 anthropic 兼容端点 `https://open.bigmodel.cn/api/anthropic`，模型 `glm-5.3-flash`；key 从用户既有 Claude Code 配置运行时读出、只经 `ANTHROPIC_API_KEY` 环境变量进子进程，配置文件/日志/库恒不含 key | `src/module_memory/engine/telemem_config.h` 契约常量（单测钉死）；S2 实测报告 §2/§5 |
| 向量化 | 本机 llama-server（OpenAI 兼容 `/v1/embeddings`）+ `bge-small-zh-v1.5`（512 维，Q8_0 量化 25MB）；维度在建索引前定死，换档即全量重嵌 | 同上 + 跑分报告 `docs/superpowers/plans/2026-10-27-memcpp-embed-bakeoff.md` |
| 向量升级档 | Qwen3-Embedding-0.6B Q8_0，部署必须显式 `-c 8192` 限上下文（否则常驻内存 4.1GB）；升档条件 = 记忆规模上量/间接表达查询占比高且内存预算容得下约 1.5GB | embed-bakeoff §推荐/§每模型正确用法 |
| 云端向量化备选 | 智谱 `embedding-3`（显式 `dimensions=1024`，无 768 档）——现用套餐 key 在标准通道无按量额度（429 实测），暂不可用；另建标准 key 后可切 | S1 实测 `docs/superpowers/plans/2026-10-27-memcpp-s1-spike-results.md` §5；调研 `docs/superpowers/research/2026-10-27-memcpp-zhipu-endpoint-research.md` |

与集成规格 §0 端点块的偏差（如实登记，非翻案）：原定「提炼与向量化均走云端 paas/v4」；S1-5 实测现用 key 为纯套餐 key（paas/v4 embeddings 429 无额度、模型目录无 embedding 类）后，落地形态改为「anthropic 端点提炼 + 本地向量」。云端 embedding-3 保留为备选档。

### 2.3 控耗五条现状（定案五条 → 逐条现状）

| # | 定案措施 | 现状 |
|---|---|---|
| 1 | 只对新增内容提炼：去重水位，重复摄取云端调用 0 | 已落地且强于定案：采集层 (session_id, event_seq) 唯一键幂等（重复读零入库）；且当前形态「新增必审」——采集默认零云端调用，接受提案才入库且按原文直存（infer=false，不调提炼模型）（出处：`collector/collector.h`、`proposals/curator.h`） |
| 2 | 提炼批处理节流：每小时云端调用上限可配置 | 结构性成立：当前默认链路无自动云端提炼批处理，节流上限随提炼（infer=true）启用时再接（出处：curator.h「默认无静默入库通道」） |
| 3 | 月度 token 预算软/硬档 | 已落地：`engine/token_usage.h` 软档黄条/硬档「暂停提炼·检索不受影响」判定 + 配置页阈值滑条 + 主控台预算环；判定读 §5 所述估算账面 |
| 4 | 面板消耗页 | 已落地：今日/本月 token、估算金额、最近调用记录表，直读 usage_ledger 账本行（commit `214f9043`） |
| 5 | embedding 每条记忆仅计算一次 | 成立：条目仅在入库（提案接受/评测灌入）时嵌入一次，维度定死；检索只对查询文本嵌一次，已入库条目不重嵌（出处：`gateway/telemem_client.h` addDirect + telemem FAISS 索引语义） |

## 3. 选型与实测数字总表（每数字附出处）

### 3.1 本地向量模型同场跑分（llama-server 统一口径，2026-10-27/28）

出处：`docs/superpowers/plans/2026-10-27-memcpp-embed-bakeoff.md`。RSS = 进程实际占用的物理内存。

| 模型 | 维度 | 原集 R@5/MRR | 难集 R@5/MRR | RSS 稳态 | 延迟 p50/p95 | Q8_0 体积 |
|---|---|---|---|---|---|---|
| bge-small-zh-v1.5（当前采用） | 512 | 1.0 / 0.967 | 0.9 / 0.717 | 49 MB | 9.4 / 11.2 ms | 25 MB |
| bge-m3（不推荐） | 1024 | 1.0 / 0.967 | 0.9 / 0.843 | 515 MB | 73.1 / 87.2 ms | 605 MB |
| Qwen3-0.6B 模型卡默认 | 1024 | 1.0 / 0.967 | 1.0 / 0.95 | 4133 MB | 158.3 / 180.8 ms | 610 MB |
| Qwen3-0.6B `-c 8192`（升级档） | 1024 | 1.0 / 0.967 | 1.0 / 0.95 | 1439 MB | 140.5 / 168.9 ms | 610 MB |

R@5 = Recall@5（前 5 条命中率）；MRR = 排名倒数的平均。27 条冻结记忆 + 15 条原集查询 + 10 条难集查询。

### 3.2 S1 spike 五项实测（2026-10-27）

出处：`docs/superpowers/plans/2026-10-27-memcpp-s1-spike-results.md`。

| 项 | 数字 | 判定 |
|---|---|---|
| 1 Windows pip 安装 | 退出码 0，**97 秒**，49 个依赖全部预编译轮子、零编译 | 通过 |
| 2 常驻内存 | 启动态 123.4~124.1 MB → 一次调用后 **156.5~157.0 MB**（进程树 RSS；红线 300MB） | 通过 |
| 3 注入延迟 p95 | 未出数：云端 embedding 计费不可用；检索延迟由 S2 端到端补证（搜 157ms） | blocked→已改道 |
| 4 冻结集 Recall@5 | 未出数（同上依赖）；评测单元已内建进面板（recall_eval） | blocked→已改道 |
| 5 云端 embedding 端点 | paas/v4 embeddings **429 无按量额度**（重试一致）；anthropic 端点 200 → 纯套餐 key 判定成立 | 实测完成：云端向量不可用 |

### 3.3 S2 live 层实证（2026-10-28，独立数据根）

出处：`docs/superpowers/plans/2026-10-28-memcpp-s2-live-verify.md`。

| 项 | 数字 |
|---|---|
| venv 创建 + pip 装 telemem | 4 秒 + 68 秒（含 anthropic 补装，commit `00ac9931`） |
| 端到端入库 add | **2316 ms**（含 FAISS 首次建索引） |
| 端到端检索 search | **157 ms** |
| 全量单测 | 1490 例：Passed 1481 / Failed 0 / Skipped 9 |
| 过程修复 | 4 处真实缺陷（Python 探测目录/引擎入口与配置字段/健康探活/pip 目标），提交 `1331450c`→`00ac9931` |

### 3.4 触发线口径（预先写死，定案 7）

冻结 15 查询转测引擎检索，任一命中即升级检索通道：Recall@5 **< 85%**，或 2 字中文词「端口/配置/模块」命中数低于自研全文基线 **5/3/11**（基线出处：可行性报告 §3.4，FTS5 双字分词 Recall@5 1.00）。S1 因 embedding 不可用未出数，触发线判定维持「不触发升级、维持 A 独跑」；终判在真机 UAT（S5）。

## 4. 实现地图（src/module_memory/）

| 子目录 | 一行职责 | 关键 commit |
|---|---|---|
| `memory_module.h` | 「记忆」页注册与三视图 UI 编排（数据全经 PanelState） | `f343f436` |
| `collector/` | 会话 jsonl 增量尾读 + 四道过滤 + 4KB 分段 + 水位幂等 | `8ee59132` |
| `sanitize/` | 脱敏闸 C++ 移植：六类检测、fail-closed、运行时开关（红队 110 条钉进单测） | `c1fed58f` |
| `engine/` | Python 探测/venv+pip 首跑/TELEMEM_CONFIG 生成/telemem 与 llama-server 拉起·树杀·健康判据 | `ccbeaac2`（live 修正 `1331450c` `90cf043f` `473204ff` `00ac9931`） |
| `gateway/` | 检索接口缝 `IMemoryRetriever`（search/addDirect/setMemoryForgotten），MCP-HTTP 转发实现 | `fba5a126` |
| `hook/` | `memory hook` 子命令 + 预算拼装（≤5 条 ≤500 字）+ hooks 配置键级装拆 | `fba5a126` |
| `proposals/` | 提案表 16 字段契约/状态机/undoOf 撤销链/baseVersion 校验 + 整理器（新增必审/遗忘）+ 代理执行映射 + 消耗账本 | `64efb989` |
| `panel/` | PanelState 数据中枢（账本/概览/设置持久化/评测单元）+ 三视图真数据交互 + 消耗卡 + 演示探针 | `758f5c1a` `663a2396` `214f9043` `032506b0` |

单测落点：`tests/test_modules/test_memory_{collector,engine,hook,proposals,sanitize}.cpp`；演示/验收探针：`tests/probes/memory_probe.cpp`。

## 5. 已知限制与边界

| # | 限制 | 说明 |
|---|---|---|
| 1 | 引擎侧无列举/全清缝 | 检索接口缝只有 search/addDirect/setMemoryForgotten 三方法；telemem 的列表与全清工具未接缝——面板「导出/清除全部」只清自家库（proposals/账本/采集账），telemem 存量记忆需另开缝（升级 C 数据回填时一并补） |
| 2 | 消耗/注入记账为估算口径 | token = 码点数估算（中文 1 字约 1 token），金额按智谱调研单价常量；引擎返回的真 token 对账未接（出处：`proposals/usage_ledger.h` 契约注释），结构已留好、只替换取数 |
| 3 | 审核页批量视图窄窗标签重叠 | 批量模式的类型/状态标签在窄窗口下可能重叠，属界面瑕疵待修，不影响数据与操作语义 |
| 4 | macOS/Linux 平台面未实测 | `python_locator_posix`/`telemem_process_posix` 已按平台拆文件并在守卫编译内，但真机只验过 Windows（S2 报告「未验证」节）；F2 已修——posix spawnDetached 改 per-child 环境构造（密钥不再进父进程环境，与 Windows 语义对齐），仍待 mac 实测 |
| 5 | 采集→脱敏→提案的运行时全链 | **已接线（F1）**：引擎 Running 期间背景循环按 kPollIntervalMs（30s）跑「collector 扫描 → 脱敏闸 → curator.proposeAdd」，拦截/fail-closed 段只出审计行与动态流行、不入提案；循环随引擎停而停。残余边界：decay 扫描已周期触发（6h）但引擎无列举缝 → fail-closed 零卡（待限制#1 缝补齐后换清单实现） |
| 6 | 触发线评测未出终数 | recall_eval 冻结集评测单元已内建，出数依赖引擎 Running 态，终判留待 UAT |
| 7 | baseVersion 防覆盖校验 live 空转（F4） | 引擎无版本查询缝 → 面板以提案快照版本放行（`panel_state` trustSnapshotVersion），防覆盖是名义防线、勿误信已生效；引擎版本缝就绪后删除该旁路并补对照测试 |
| 8 | 评测清理受 get_memories 清单上限约束（F5） | 评测 27 条清理只覆盖「id 回填成功者」；超 200 条清单/正文失配条目残留引擎、可被 hook 检索注入——清理缺口计数已上面板（重跑评测可覆盖清理），根治待引擎列举缝 |

### 触发线与升级路径（预先写死，定案 7）

- 触发条件见 §3.4；命中即在检索接口缝后补自家全文通道（SQLite FTS5 中文双字分词 + 短查询 LIKE 兜底，按 `docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md` §2.3/2.4 施工——该规格书已降级为触发线备用图纸）。脱敏闸/预算/hook 壳/面板零返工，卡片代理层重接自家库。
- 向量升级路径（非触发线）：bge-small → Qwen3-Embedding-0.6B `-c 8192`（§3.1）；云端 embedding-3 备选（§2.2）。
- 反向出口：评测全绿则 A 路线继续独跑，不升级。

## 6. 验收状态（S1~S5，如实）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| S1 spike | **达成（两项改道）**：①安装 97s 通过 ②内存 157MB 通过 ⑤端点实测完成=云端向量不可用→本地 bge-small 定案；③④未出数，由本地向量档的 S2 端到端实测（搜 157ms）与面板评测单元覆盖 | `docs/superpowers/plans/2026-10-27-memcpp-s1-spike-results.md`；触发线判定=不触发升级 |
| S2 托管壳+面板骨架 | **已达成**：真 venv/pip/引擎拉起/健康判据/三视图/key 红线五项全过；4 处缺陷当场修复并钉进单测 | `docs/superpowers/plans/2026-10-28-memcpp-s2-live-verify.md`；提交链 `1331450c..00ac9931` |
| S3 脱敏闸+注入接线 | **已达成（e2e-verify 可交付 + 里程碑审计处置）**：脱敏红队 110 条、hook 契约（exit 0/不 exit 2）、采集幂等、账本接线各有单测；全链真跑探针 39 断言过；采集管线已接入引擎 Running 期背景循环（F1） | `c1fed58f` `8ee59132` `fba5a126` `758f5c1a` `9ee92079`；`tests/test_modules/test_memory_{sanitize,collector,hook,collect_wiring}.cpp`；`plans/2026-10-28-memcpp-e2e-verify.md` |
| S4 卡片代理 | **已达成（e2e-verify + 里程碑审计处置）**：16 字段/状态机/撤销链/baseVersion/整理器/代理执行映射 + 审核页四操作/批量/过滤；UUID id 形态与遗忘撤销重录入已按 live 定案钉版（F3）；baseVersion live 空转如实入限制#7（F4） | `64efb989` `663a2396` `214f9043` `9ee92079`；`tests/test_modules/test_memory_proposals.cpp` |
| S5 真机 UAT | **未开始**：真实会话一周量级、注入采纳率 ≥30% 抽样、触发线终判 | — |

---

*本文只记「当前决定 + 出处」，演进过程（拍板前后、改道缘由）读各出处原文；数字一律以出处文档为准，本文不重估。*
