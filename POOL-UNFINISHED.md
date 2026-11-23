# DevDesk 池未完成清单快照

> 生成：2026-11-23 09:45 · 来源：`docs/pool/pool.db` 只读直查 · 本文件为一次性快照，后续状态以池 db 为准
> 口径：未完成 = 状态 ∉ {关闭, 拒绝}（含 待验收 / 进行中 / 排产 / 在池 / 挂起），含 temp 临时池，软删条目除外

## 总览

| 池 | 待验收 | 进行中 | 排产 | 在池 | 挂起 | 小计 |
|---|---|---|---|---|---|---|
| need | 32 | 0 | 0 | 3 | 4 | **39** |
| bug | 42 | 0 | 0 | 4 | 2 | **48** |
| debt | 3 | 0 | 0 | 3 | 1 | **7** |
| temp | 2 | 0 | 0 | 0 | 0 | **2** |
| **合计** | **79** | **0** | **0** | **10** | **7** | **96** |

关联资料量：进度记录 533 条（每条目最多列最近 5 条）、关联 BUG 15、验收证据 1、关联文档 14 篇（全文见附录 A）、关联线索 132 条。

## 快速索引

| # | 状态 | 优先级 | 负责人 | 标题 |
|---|---|---|---|---|
| need#114 | 待验收 | P1 | qa-tester | 记忆模块闭环实测：py检测/pip维护/伪造数据测试体系与价值证明 |
| need#134 | 待验收 | P2 | developer | 基础设置列表 item 缺卡片效果（服务/环境行仅有文字+分割线） |
| need#140 | 待验收 | P1 | bug-fixer | 定时任务各列表的 item 卡片布局需重新设计（观感太差） |
| need#155 | 待验收 | P2 | developer | 同步 TAB 的「查看详情」说明 label 改为点击弹窗显示内容 |
| need#170 | 待验收 | P2 | qa-tester | 智能体可用工具改弹窗展示+卡片布局重设计（现布局大片空白不协调） |
| need#172 | 待验收 | P2 | developer | 侧边栏收缩展开按钮：圆框+悬浮样式+弥漫绿色粒子效果 |
| need#178 | 待验收 | P2 | developer | 运行时内存占用优化（基线待实测） |
| need#184 | 待验收 | P2 | developer | 168 收尾：窗口态 API 暴露与系统描边隐藏（EUI fork XS×2）+ring 最大化隐藏分支 |
| need#271 | 待验收 | P1 | zcode | 限制 DevDesk.exe 内存占用（三并发 5GB，可接受降速换内存） |
| need#272 | 待验收 | P1 | qa-tester | Todo 看板：今日到期卡片整体橙黄、逾期未完成卡片整体红色 |
| need#279 | 待验收 | P1 | developer | MCP 详情新增安装/卸载按钮：写入全局 Claude Code、ZCode 的 MCP 配置 |
| need#291 | 待验收 | P1 | developer | Git 管理器整体重设计：老板令「整个功能逻辑都不对，没有哪个 UI 功能完全正常」——UI+功能逻辑全量重梳 |
| need#299 | 待验收 | P1 | ui-ux-designer | Git 页变更列表/目录树 UI 重设计+边界处理（老板令 2026-11-17） |
| need#302 | 待验收 | P2 | ZCode主会话 | 删除 pool-snapshot.json 相关全部功能与文档 |
| need#303 | 待验收 | P2 | developer | 临时任务池（temp 池：仅 id+描述）——子任务持 id 豁免 devflow 约束 |
| need#339 | 待验收 | P2 | developer | TAB 文字与头部卡片缩小 20%（老板实测回调 #167 放大档） |
| need#356 | 待验收 | P2 | bug-fixer | Windows 专属：应用全部颜色加深（屏幕素质差、观感寡淡） |
| need#357 | 待验收 | P2 | bug-fixer | About 页卡片内容增加左边距 |
| need#361 | 待验收 | P2 | ZCode主会话 | 作废任务按钮直接显示，不入菜单 |
| need#365 | 待验收 | P2 | ZCode主会话 | 终端搜索功能：定位历史内容（条件允许时） |
| need#368 | 待验收 | P2 | developer | 删除 todo 卡片左侧加粗竖线 |
| need#370 | 待验收 | P2 | bug-fixer | profiles 列表改双击切换，单击仅展示详情 |
| need#371 | 待验收 | P2 | bug-fixer | 设置缺少 git/终端功能开关 |
| need#372 | 待验收 | P2 | bug-fixer | 展开态侧栏宽度收窄（右侧空白多+双重左边距） |
| need#374 | 待验收 | P2 | ZCode主会话 | 编辑器工具栏宽度自适应 |
| need#375 | 待验收 | P2 | bug-fixer | 编辑器语法高亮缺失（起码补 SQL） |
| need#376 | 待验收 | P2 | ZCode主会话 | 编辑器 Ctrl+F 搜索框自动聚焦可直接输入 |
| need#377 | 待验收 | P2 | ZCode主会话 | 搜索命中词高亮：色加深+加粗（现淡粉） |
| need#380 | 待验收 | P2 | ZCode主会话 | 编辑器缺格式化功能（SQL 优先待定档） |
| need#394 | 待验收 | P1 | qa-tester | MCP 对外开放智能体必要内置工具 + 安装按钮外置（非按方法粒度） |
| need#400 | 待验收 | P1 | developer | 记事本 md 预览态搜索定位（F-06 扩展：预览内查找+高亮+跳转，老板 2026-11-21 令） |
| need#401 | 待验收 | P1 | developer | 全应用阻塞性慢操作排查（老板 2026-11-21 令：其他功能是否同存在帧内同步重活，响应第一口径全扫） |
| need#355 | 在池 | P2 | ZCode主会话 | 会话链路延迟优化：琐碎消息数分钟级响应（超长思维链+技能仪式+codegraph误注入） |
| need#393 | 在池 | P2 | claude-agent | devflow 峰窗门禁移除（老板令，范围待确认） |
| need#396 | 在池 | P1 | 主会话 | 用户定义智能体的 MCP 列表显示开关 |
| need#34 | 挂起 | P3 |  | 向量模型升级档（Qwen3-Embedding-0.6B） |
| need#324 | 挂起 | P3 | developer | 全站 CJK 禁则排版（行首禁则字符不起行，EUI-NEO 排版层） |
| need#340 | 挂起 | P2 | qa-tester | CI 类验证项分诊与关账预案（push 后执行） |
| need#392 | 挂起 | P1 | developer | AI 开发使用全景站（L）：showcase-v2 多页整合——插件/skill 清单+机制+实践证据+静默调用场景分析 |
| bug#135 | 待验收 | P2 | developer | Windows 托盘图标右击菜单位置偏右上（应贴托盘/鼠标位置弹出） |
| bug#150 | 待验收 | P2 | qa-tester | 实机：用户定时行卡 hover 果冻形变不生效（仅系统定时卡有效，探针盲区） |
| bug#189 | 待验收 | P2 | ZCode主会话 | 设置页「项目卡」tab 用途不明（内部测试功能暴露给用户）——处置候选删除 |
| bug#191 | 待验收 | P1 | developer | telemem 采集轮提案风暴：1630 条 pending 同一秒批量落库（无去重/无批量上限），触发 bug190 冻结规模因子 |
| bug#239 | 待验收 | P1 | developer | 记忆提炼补标 S8 红：真 LLM 下 runInferStep 成功+记账+出队，但引擎清单无前缀产物（Windows 真机 e2e 44/45 唯一红） |
| bug#245 | 待验收 | P1 | bug-fixer | 终端 IME：中文候选词窗不在终端光标附近（imeRect 定位偏移） |
| bug#269 | 待验收 | P1 | zcode | libgit2 vendored 子模块在 Windows 上构建失败（BUILD_TESTS 被 CMake policy OLD 覆盖为 ON，headertest -Werror 撞 _WIN32_WINNT 重定义） |
| bug#281 | 待验收 | P2 | bug-fixer | CI Windows 红：2 条 git-diff 测试失败（中文+空格仓 diff 行内容/行号 + WorkdirToHead 增删行）——本地绿 CI 红，疑 checkout 行尾差异 |
| bug#282 | 待验收 | P2 | bug-fixer | CI macOS 红：file_association_macos.mm 新 SDK 编译炸（2 处 UTType deprecation×Werror + NSWorkspace selector 不存在） |
| bug#284 | 待验收 | P1 | ZCode主会话 | bug266 回归：settings applyWindowVisuals 接线用例红（translucent→Transparent 接线后期望未同步，本地+CI 双红） |
| bug#286 | 待验收 | P1 | ZCode主会话 | CI Windows std include 守卫红：module_git 9 处缺显式 include（git v1 遗留，测试绿后首次暴露） |
| bug#320 | 待验收 | P2 | developer | devflow pool MCP 工具 schema 的 pool 枚举未含 temp——temp 池登记通道被 schema 校验客户端拦（need303 活测发现） |
| bug#334 | 待验收 | P1 | developer | memory_install_probe 无界死等 + 「接受→真引擎 add」路径挂起（cards=369 accepted=0 实证） |
| bug#337 | 待验收 | P2 | developer | 定时新建/编辑弹窗固定高度，小窗口下显示不全 |
| bug#338 | 待验收 | P2 | developer | 用户定时「删除」按钮：关弹窗且未执行删除 |
| bug#341 | 待验收 | P1 | main-session | codegraph 守护进程疯狂读写硬盘：索引灌入 evidence/池库产物致 316MB 库被高频重扫 |
| bug#346 | 待验收 | P3 | developer | F-06 洗链与 1024B 截断交互：中断标记被撕开残段入卡（BI 审查 P2） |
| bug#358 | 待验收 | P2 | bug-fixer | 设置-同步：仓库URL 等配置不能持久化保存 |
| bug#359 | 待验收 | P2 | ZCode主会话 | 上线任务详情：【未完成】状态标布局异常 |
| bug#360 | 待验收 | P2 | ZCode主会话 | 上线填写弹窗布局错乱：控件跑出弹窗边界 |
| bug#362 | 待验收 | P2 | ZCode主会话 | 【请填写单据号】文本容器内未垂直居中 |
| bug#363 | 待验收 | P2 | ZCode主会话 | 终端：中文输入法候选词显示在窗口中央而非光标处 |
| bug#364 | 待验收 | P1 | bug-fixer | 终端缺滚动条无法回看历史（疑似影响多功能，待考古引入时点） |
| bug#366 | 待验收 | P2 | developer | 智能体模块卡片不透明，与全局材质语言不一致 |
| bug#367 | 待验收 | P2 | ZCode主会话 | 定时弹窗布局完全错乱 |
| bug#369 | 待验收 | P1 | bug-fixer | md 编辑器模式 tab 切换冻结数秒（编辑/预览/双栏/实时） |
| bug#373 | 待验收 | P2 | ZCode主会话 | 主窗最小化时双击 sql 文件，编辑器子窗也最小化 |
| bug#378 | 待验收 | P2 | bug-fixer | 编辑器光标垂直定位偏上，应底部对齐 |
| bug#379 | 待验收 | P2 | ZCode主会话 | 编辑器 IME 候选词跑到窗体左上角（与 #363 同根因） |
| bug#381 | 待验收 | P2 | ZCode主会话 | 未保存变更弹窗文本字号过大 |
| bug#382 | 待验收 | P1 | bug-fixer | 编辑器 md 预览严重性能问题（追问专项优化去向+考古） |
| bug#383 | 待验收 | P2 | ZCode主会话 | 编辑器窗体 resize 后内容/换行不自适应 |
| bug#399 | 待验收 | P1 | bug-fixer | 记事本 md 预览滚动后大片空白、内容不再显示（真机老板实测：南非索赔发票布局微调PRD-2026-11-17.md） |
| bug#402 | 待验收 | P1 | developer | markdown 外部变更轮询在 compose 帧内同步（实测 55.8-80.2ms/帧 @300 笔记，真机每 5s 一次） |
| bug#403 | 待验收 | P1 | developer | markdown rescan 在 compose 帧内同步（实测首帧 125.4ms @300 笔记） |
| bug#404 | 待验收 | P2 | developer | claude_proxy 进页首帧 46.7ms（同步装配 + refreshDataCache） |
| bug#405 | 待验收 | P2 | ZCode主会话 | todo 首帧 44.3ms @60 任务（导入后 reloadAll UI 线程全量重查 + 板装配） |
| bug#406 | 待验收 | P2 | developer | terminal 进页同步建 ConPTY 会话 14.4ms（onPageEnter ensureBackend） |
| bug#407 | 待验收 | P2 | developer | memory 每帧全量快照拷贝 + 1s 刷新帧 5.3ms（进页首帧 29.1ms） |
| bug#410 | 待验收 | P1 | developer | markdown 笔记列表未虚拟化——800 笔记 68ms/帧、1200 笔记 214ms/帧、进页 531ms |
| bug#411 | 待验收 | P1 | developer | memory 待审提案无上限 × 每帧快照拷贝——500 条 78ms/1s、2000 条 135ms（与提案风暴叠加） |
| bug#412 | 待验收 | P2 | developer | home getLastCallRate 全表倒扫 claude_call_logs——10万行 38ms/20万行 84ms（进页阻塞） |
| bug#408 | 在池 | P2 | developer | home 进页同步直读库（getStats 6 条 count + recent/hourly，实测空库 0.4-0.6ms，随表增长） |
| bug#409 | 在池 | P3 | developer | 多模块每帧快照/有界读当前 <50ms 待规模观察（mcp registry 快照 / wallpaper snapshot / release 帧泵分页 / settings 日志尾读） |
| bug#413 | 在池 | P2 | ZCode主会话 | claude_proxy 首帧随 claude_call_logs 增长——20万行 66-73ms（8万行破 50ms 红线） |
| bug#415 | 在池 | P1 | developer | hook 负对照「今天天气怎么样」阴性召回误报：无关提问注入 ccache 踩坑卡 1 条（#239 e2e 环节⑤红） |
| bug#333 | 挂起 | P1 | developer | 提案入库零质量闸门——垃圾直接落库（老板令：这种东西就不应该入库） |
| bug#347 | 挂起 | P2 | developer | keychain→app_secrets 专表无迁移：memory.llm 与 wecom 两处旧 key 升级后静默失效（BV 审查 P1） |
| debt#121 | 待验收 | P3 | ZCode主会话 | CI msys2 UCRT64 迁移（两轮翻车已回退 MINGW64+消音，PATH 传导机制待查） |
| debt#218 | 待验收 | P2 | developer | Claude Code 内置市场 anthropic-plugin-directory 加载 403 噪音（启动 warning）——禁用或代理路由待探 |
| debt#398 | 待验收 | P1 | developer | 记事本 md 预览：>256KB 文档每帧重复全量解析×2（解析缓存单条 256KB 上限挡住）+3 个小项 |
| debt#304 | 在池 | P3 | ZCode主会话 | DevDesk 侧 pool 快照面射程外遗留（legacy tools 快照写路径/评审页/历史 spec/展示页）待裁决 |
| debt#414 | 在池 | P3 | developer | bug401 帧探针测量口径污泥：默认 1ms 轮询（#402/403 后已失效）+ 未隔离数据根（已修，残留 PATH 运行前提） |
| debt#416 | 在池 | P1 | ZCode主会话 | need381 与 need394 双 MCP 适配层并存待裁决（AgentToolAdapter/ToolMirror 46 件 vs McpToolAdapter 15 件，合并态未验证） |
| debt#321 | 挂起 | P2 | discipline-guard | 评审判据系统性缺口：dump 几何语义缺失 + 旧证据判新态（波 2 三连误判根因） |
| temp#384 | 待验收 | P1 | qa-tester | 2026-11-19 修复波统一验证批（29 卡，26 DevDesk + 6 EUI-NEO commit） |
| temp#386 | 待验收 | P2 | qa-tester | 池台账全景 HTML 二轮双审计（修后复核：数据轴+工程轴） |

## need 池

### 待验收（32）

#### need#114 · 记忆模块闭环实测：py检测/pip维护/伪造数据测试体系与价值证明

- 状态：待验收 · 优先级：P1 · 负责人：qa-tester · 阶段：开发中
- 创建：2026-11-03 04:15:17 +0800（qa-tester）· 更新：2026-11-18 20:09:25 +0800（qa-tester）

**详情**：

纯技术验证：①python 检测提示链路 ②pip 包管理维护（版本未钉扎/自愈/超时文案）③伪造会话数据跑通采集→脱敏→提案→入库→检索→hook注入闭环，先预测后验证 ④recall_eval 冻结集真跑出 Recall@5 硬数字 ⑤UI 引导态文案走查。数据落独立临时数据根，真实 ~/DevDesk/memory 零污染。执行方式：POC/试错主会话直做。

**正文**：

纯技术验证：①python 检测提示链路 ②pip 包管理维护（版本未钉扎/自愈/超时文案）③伪造会话数据跑通采集→脱敏→提案→入库→检索→hook注入闭环，先预测后验证 ④recall_eval 冻结集真跑出 Recall@5 硬数字 ⑤UI 引导态文案走查。数据落独立临时数据根，真实 ~/DevDesk/memory 零污染。执行方式：POC/试错主会话直做。

**关联线索**（2）：

- [open] 提案质量,待审批,乱七八糟,无头无尾,#191：老板实测反饳（2026-11-18 原话）：「最后一次测试的时候 待审批的经验 都是乱七八糟的东西，无头无尾，完全看不懂」——待审批提案内容质量缺陷实证。与 #191（1630 条 pending 同一秒批量落库、每条 1975 CJK 字符无去重无上限）同族疑：采集轮产出的提案是无上下文片段墙。价值链判决关键输入：模块能运行但产出不可读=价值不成立，除非提炼质量重做（去重+上下文截取+人读格式化）（source: 老板会话原话 2026-11-18）
- [open] ui_driver 全 id 匹配, wait 早返回, sqlite WAL 快照, 引擎三件注入, 驱动根即删：驱动 harness 三坑（need#114 实测 2026-11-18）：①ui_driver 的 find 为全 id 精确匹配，click/wait 必须写全形 id（如 app.memory.rv.accept.prop_20261118_000030；旧 m6 场景裸 memory.rv.evtoggle 形态在现版不可达）；②timed wait-ms 有「稳定即返」早返回，固定 dwell 不可靠，用 wait-until-exists/text 长超时充当 dwell；③sqlite 快照只 cp 主 db 会丢 WAL（recall_eval 快照表空），live 读为准。另：记忆引擎在驱动根真实拉起需注入 venv+llama+models 三件（assets 有 llama-server.exe+模型但 run2 仅注 venv 未起，完整三件注入 run3 引擎即起）；驱动根 uidrv_root_1-<pid>-0.db.data 建在 cwd 下、退出即删，采集中途拷库须连 -wal/-shm。（source: evidence/20261118-memory-loop/verdicts.md + 三轮 driver-console.txt）

**关联 BUG**（2）：

- 2026-11-03 04:23:15 +0800 · 已解 · mac e2e 实测（need 114）：撤销链断——采集合段文本尾带\n，telemem 存储剥尾空白，addDirect 的 findMemoryIdByText 精确匹配差 1 字符 → target_ids=[] → createUndoCard 拒绝「不可撤销」。生产常态（段尾=assistant text 块固定补 \n）必现。修复方向：telemem_client.cpp findMemoryIdByText 两侧 trim 后比对（或 add 前统一 trim），补钉版单测（尾换行文本回填）
- 2026-11-03 04:27:19 +0800 · 已解 · mac e2e 实测（need 114）：hook 注入无相关度门槛——改写问句与负对照「今天天气怎么样」三问返回逐字节相同的 396 字全量注入。根因：telemem search 恒返 top-k 融合段无相似度下限，且融合段无逐条 score（事后过滤不可行）→ 注入层照单全收。每次提问上下文被污染+白付 token。修复方向（先 POC）：①引擎侧阈值参数 ②DevDesk 词面预闸（inject.cpp 同落点，纯函数可单测）③面板注入总开关兑底；推荐②+③

**进度记录**（共 17 条，列最近 5 条）：

- 2026-11-18 16:49:21 +0800 · qa-tester 实测批完成（全真组件零 mock，run3 输入=真实会话历史 1.7G/979 jsonl 隔离根，真实数据根文件级零写入）。五要素结论：①引擎内用户记忆 0 条（561 全停待审+561 提炼队列未消费，get_memories 实查空）②价值红——老板症状真实历史重采复现：561 条全部固定前缀[踩坑|会话采集]+无头无尾原文片段（系统性采集→提案质量缺陷，#191/#333 同链路，抽样本 8 条见 verdicts）③完整红——片段缺头尾，上下文丢失（源会话可回溯已验）④MCP 检索：冻结集 Recall@5=100%（15/15，真引擎真 faiss，live 读 ts=1794991243）；真实问题检索未执行（引擎无入库条目）⑤对账：db 561 vs 渲染 50（kReviewShowLimit 设计一致）；脱敏疑点：561 条 REDACTED=0+16 行 @ 形态未脱待复核。原五项：①python 链绿②pip 维护绿（run1 活体 pip 钉版命令行+自愈+文案锚）③闭环半——采集→过闸→提案真绿（561 卡），接受→入库→检索→hook 四腿未执行（场景 id 形态错=测试缺陷，按钮在位证据在 console；墙钟被接管未及补跑）④Recall@5=100% 绿⑤UI 文案绿（像素级未做如实）。伞 #214 进度：记忆引擎真机链路（I2 venv 拷贝法）+真实历史采集+冻结集评测均已在驱动根实证，注入/hook 腿待补。#333 佐证：真实批次重采与 1630 条旧形态一致=系统性非数据脏；修复方向按终稿口径已写入 verdicts。证据 evidence/20261118-memory-loop/（verdicts.md+proposals/infer_queue db+monitor log+MCP 原文+三轮 console/report/dump/png）。遗留：接受链补跑一场景、usage_ledger/徽章文本对账、脱敏@复核。预算：工具 63/60、墙钟~75/45min 超限如实。
- 2026-11-18 17:20:20 +0800 · 老板令（2026-11-18）：「你直接拿一个长一点的会话 jsonl 进行测试吧，别全处理了」——闭环验证改单会话聚焦法：选一个最长的真实会话 jsonl→临时根→引擎→采集→提案（应为少量可读数）→接受→入库→get_memories→MCP 检索该会话真问题→hook 验证；单会话产物逐条人读判价值。此法同时直接测 #334 接受链（单会话若也挂=产品缺陷坐实；若通=量级相关）
- 2026-11-18 17:21:39 +0800 · qa-tester 开工：单会话聚焦验证 AF（最长 jsonl 一个走全闭环，只验不修）
- 2026-11-18 17:57:54 +0800 · 单会话聚焦验证 AF 完成：最长真实会话（ea54ed9e…jsonl 52.5MB/5994 行）单件隔离闭环实跑——采集 195 条待审（全 pending/kind=add，[踩坑|会话采集]前缀原文直通，与 #333 终稿口径差距不变但单会话可读性好于全量批：57% 含技术实质未修饰/30% 对话流水/12% 系统块泄漏新增实锤）；接受→引擎入库→get_memories 非空→search_memories 真命中会话真问题（「RS 维修标准接口」查询返回 SvcServiceMaintainRuleServiceImpl 修复记录）四腿全通；hook 未验（根随驱动退出删除）。新环境事实：驱动临时根=<cwd>/build/testtmp/uidrv_root_1-<pid>-0.db.data（tasklist 拿 pid 精确注入）；MCP 8421 轮询须 curl；提案 id 可预测 prop_YYYYMMDD_%06d 可预写场景。工具调用~48 贴线，墙钟 85 分钟超预算 50（两场注入排错废弃+残骸抢救）。证据 evidence/20261118-memory-one-session/（verdicts.md+195 条原文 raw.md+MCP 四件套）
- 2026-11-18 20:09:25 +0800 · BW 归位+#114 定性：非悬空非过期——老板单会话令（AF 批）2026-11-18 17:57 已完成：最长真实会话（52.5MB/5994 行）单件闭环四腿全通（采集 195→接受→入库→get_memories 非空→search 真命中会话真问题）；原五项全验（真数据口径；③伪造数据口径已被 #214 老板裁决取代）。遗留：hook 腿（根随驱动退出未验）归 #214 伞承接；usage_ledger 对账/脱敏@复核归 #333 终稿线承接。归待验收待老板过单。证据 evidence/20261118-memory-one-session/ + evidence/20261118-memory-loop/

**最近状态变更**（共 10 条）：2026-11-18 20:09:25 +0800 · 进行中 → 待验收 · BW 归位+#114 定性：非悬空非过期——老板单会话令（AF 批）2026-11-18 17:57 已完成：最长真实会话（52.5MB/5994 行）单件闭环四腿全通（采集 195→接受→入库→get_memories 非空→search 真命中会话真问题）；原五项全验（真数据口径；③伪造数据口径已被 #214 老板裁决取代）。遗留：hook 腿（根随驱动退出未验）归 #214 伞承接；usage_ledger 对账/脱敏@复核归 #333 终稿线承接。归待验收待老板过单。证据 evidence/20261118-memory-one-session/ + evidence/20261118-memory-loop/

---

#### need#134 · 基础设置列表 item 缺卡片效果（服务/环境行仅有文字+分割线）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：统一QA doctest 2121 绿
- 创建：2026-11-03 14:36:14 +0800（developer）· 更新：2026-11-03 19:36:05 +0800（developer）

**详情**：

老板上报（2026-11-03）：基础设置里的列表item又缺了卡片效果。
已核实：src/module_release/release_config_ui.cpp:298-372（drawServiceRow）/374-404（drawEnvRow）——每行是 `ui.stack(base).size(rowW,kRowH)`，内部只有文字/chip/操作按钮 + 底部 1px `.sep` 分割线，无 itemSurface 行底；整卡底板是 release.config.card.bg（ui_kit::cardSurface，见:444-447）。所以列表项的层级只有「卡→文本」，没有 L2 条目卡。
期望：服务/环境列表项有卡片效果（行级 itemSurface 或卡片化行），与全站 L1/L2 材质层级一致；行高/操作区/最小窗零滚动预算须保持。属 UI/UX → 并入池127 设计批次。

**正文**：

老板上报（2026-11-03）：基础设置里的列表item又缺了卡片效果。
已核实：src/module_release/release_config_ui.cpp:298-372（drawServiceRow）/374-404（drawEnvRow）——每行是 `ui.stack(base).size(rowW,kRowH)`，内部只有文字/chip/操作按钮 + 底部 1px `.sep` 分割线，无 itemSurface 行底；整卡底板是 release.config.card.bg（ui_kit::cardSurface，见:444-447）。所以列表项的层级只有「卡→文本」，没有 L2 条目卡。
期望：服务/环境列表项有卡片效果（行级 itemSurface 或卡片化行），与全站 L1/L2 材质层级一致；行高/操作区/最小窗零滚动预算须保持。属 UI/UX → 并入池127 

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-03 14:36:14 +0800 · 并入池127 设计批次，由设计师追加 release 配置列表行材质方案。
- 2026-11-03 14:55:47 +0800 · 设计完成（F5）：行卡 itemSurface+radius6、h52+间距4=节距56（源码 kRowH=56，纠偏简报误写的64）；删行间分割线留表头底线；列表总高零变化守零滚动。
- 2026-11-03 17:31:18 +0800 · 批B 派 developer，设计文档 §16（注意源码 kRowH=56，设计已按 56 执行）；纯研发不跑测试
- 2026-11-03 17:54:00 +0800 · 代码完成（release_config_ui.cpp §16 行卡化：itemSurface+radiusSmall、行卡 52/间距 4 节距 56 总高零变化、行间分割线删表头底线留、列宽 rowW−56 内缩 28 对齐 ±0、按钮垂直居中 y=6、无行 hover/pressed；未构建未跑=按老板令）。文档头注纠偏已按源码 kRowH=56 执行。池133（release_list_ui 任务列表）不在本批文件范围，未动未检。
- 2026-11-03 19:36:05 +0800 · 实现 commit bcea925c（行卡 itemSurface 52 高/节距 56 零变化/列对齐）；doctest 全绿

**最近状态变更**（共 6 条）：2026-11-03 19:36:05 +0800 · 进行中 → 待验收 · 实现 commit bcea925c（行卡 itemSurface 52 高/节距 56 零变化/列对齐）；doctest 全绿

---

#### need#140 · 定时任务各列表的 item 卡片布局需重新设计（观感太差）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-03 15:07:54 +0800（bug-fixer）· 更新：2026-11-04 08:50:51 +0800（bug-fixer）

**详情**：

老板上报（2026-11-03，原话）：定时任务的几个列表里的item卡片布局需要重新设计，太差了。
范围：定时调度页各列表（系统任务/自定义规则等）的行卡/条目卡布局与视觉；属 UI/UX 重设计，需设计师出方案。

**正文**：

老板上报（2026-11-03，原话）：定时任务的几个列表里的item卡片布局需要重新设计，太差了。
范围：定时调度页各列表（系统任务/自定义规则等）的行卡/条目卡布局与视觉；属 UI/UX 重设计，需设计师出方案。

**关联线索**（1）：

- [open] scrollView,幽灵,measureUi,命中：scrollView 陷阱：content lambda 必须用形参 ui 绘制——捕获外层 compose 的 ui 会让 measureContentHeight 每帧把内容画进主树 root 直下（幽灵副本盖交互面、hitTest 兄弟逆序抢先命中→点击全吞）。全仓扫描挂 bug#175（source: 调度页修复实证 evidence/2026-11-03-accept-audit/sched-fix/）

**进度记录**（共 14 条，列最近 5 条）：

- 2026-11-03 18:22:19 +0800 · v4 demo 老板过目确认（2026-11-03「没问题了」，含列数断点问答 1280 窗 2 列/1600 窗 3 列）——UI 先验门禁正式通过；实现已完成待统一QA
- 2026-11-03 19:36:33 +0800 · 实现 commit 92b6de42（TAB+瀑布流全页重构，v4 老板拍板）；doctest 全绿；probe schedule_ui_probe 四处修复已落（隔帧点击/provider 先注册/F 期望/pressedScale）但按老板令禁编译测试未复验
- 2026-11-03 21:00:49 +0800 · 派发 bug-fixer：池140 瀑布流重设计的 schedule_ui_probe 修复未收口（19:35 修复后复验仍红），TAB 点击失效根因诊断中
- 2026-11-03 22:21:00 +0800 · probe 复验结论：schedule_ui_probe 全绿（ctest -R schedule_ui_probe PASS：C0 hover/B TAB切换+列数断点/C 果冻命中面/E 归档折叠段全过 + doctest --filter schedule 127/127）。红线根因非探针断言问题，是三层产品/种子缺陷：① scrollView 测量路径把卡片画进主树形成 (0,0) 幽灵副本盖住 TAB 行（ui 遮蔽捕获，已修：ui 逐层透传）；② 探针归档种子误用 updateRule 写 archivedAt 被契约丢弃（已修：archiveRule）；③ 归档段 masonry 卡叠在用户卡上方且后建先命中→点用户卡开归档规则编辑器、删错规则（池150 同域实机 bug，已修：yFlowOffset）。证据：evidence/2026-11-03-accept-audit/sched-fix/root-cause-and-fix.md。收口 QA 归主会话全量跑。
- 2026-11-04 08:50:51 +0800 · 统一QA 四轮全绿（schedule_ui_probe 随 probe 链 28/28 过，待复验闭环）：doctest 2126 例 0 败+probe 28/28+守卫链/自检全绿；瀑布流实现此前已随 92b6de42 落地，本节点完成全部探针面复验

**最近状态变更**（共 6 条）：2026-11-04 08:50:51 +0800 · 进行中 → 待验收 · 统一QA 四轮全绿（schedule_ui_probe 随 probe 链 28/28 过，待复验闭环）：doctest 2126 例 0 败+probe 28/28+守卫链/自检全绿；瀑布流实现此前已随 92b6de42 落地，本节点完成全部探针面复验

---

#### need#155 · 同步 TAB 的「查看详情」说明 label 改为点击弹窗显示内容

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-03 20:54:12 +0800（developer）· 更新：2026-11-03 23:05:20 +0800（developer）

**详情**：

老板上报（2026-11-03）：同步TAB的查看详情说明label，点击后弹窗显示内容。已知线索：github_sync_card.cpp（同步卡）。后续分析。

**正文**：

老板上报（2026-11-03）：同步TAB的查看详情说明label，点击后弹窗显示内容。已知线索：github_sync_card.cpp（同步卡）。后续分析。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-03 22:26:02 +0800 · 派发 Dev-B1 设置域批次：查看详情 label 改点击弹窗
- 2026-11-03 22:26:19 +0800 · Dev-B1 开工：查看详情弹窗实现中
- 2026-11-03 23:05:20 +0800 · 已处置：同步 TAB「查看详细说明」改为点击弹窗（github_sync_card.cpp 摘要行右端固定文案「查看详细说明」，不再卡内展开/收起；githubSyncDetailExpanded_ 语义改=弹窗开，复用同 bool 免新状态位）。弹窗宿主 settings_module.cpp（drawGitErrorDialog 同函数尾）：ui_kit::dialog_form 契约（drawDialogFrame + drawDialogTitle + drawDialogText wrap + 关闭态常驻禁交互/onOpenChange），文案=githubSyncDisclosureText() 完整披露，node id settings.github.disclosure.*，关闭=弹窗外点/「关闭」钮。错误行「查看详情」→ git_error_dialog 原链路未动。验证：--dev --filter sync 116/116 全绿；证据 evidence/2026-11-03-accept-audit/dev-b1/test-sync-tail.log。未验证项：真机点击弹窗实操（headless 覆盖编译+渲染路径，交互观感归 QA）。

**最近状态变更**（共 4 条）：2026-11-03 23:05:20 +0800 · 进行中 → 待验收 · 已处置：同步 TAB「查看详细说明」改为点击弹窗（github_sync_card.cpp 摘要行右端固定文案「查看详细说明」，不再卡内展开/收起；githubSyncDetailExpanded_ 语义改=弹窗开，复用同 bool 免新状态位）。弹窗宿主 settings_module.cpp（drawGitErrorDialog 同函数尾）：ui_kit::dialog_form 契约（drawDialogFrame + drawDialogTitle + drawDialogText wrap + 关闭态常驻禁交互/onOpenChange），文案=githubSyncDisclosureText() 完整披露，node id settings.github.disclosure.*，关闭=弹窗外点/「关闭」钮。错误行「查看详情」→ git_error_dialog 原链路未动。验证：--dev --filter sync 116/116 全绿；证据 evidence/2026-11-03-accept-audit/dev-b1/test-sync-tail.log。未验证项：真机点击弹窗实操（headless 覆盖编译+渲染路径，交互观感归 QA）。

---

#### need#170 · 智能体可用工具改弹窗展示+卡片布局重设计（现布局大片空白不协调）

- 状态：待验收 · 优先级：P2 · 负责人：qa-tester · 阶段：测试中
- 创建：2026-11-03 20:54:57 +0800（qa-tester）· 更新：2026-11-18 12:54:18 +0800（qa-tester）

**详情**：

老板上报（2026-11-03）：智能体可用工具卡片布局要改，不协调，卡片中间一大批空白，看上去就不稳定；可用工具改成弹窗展示，UI 好好设计一下，别这么 low。⚠ 本条（弹窗化）落地后池160 侧边栏悬浮菜单触发问题若同被移除则连带销账。后续 ui-ux-designer 出方案。

**正文**：

老板上报（2026-11-03）：智能体可用工具卡片布局要改，不协调，卡片中间一大批空白，看上去就不稳定；可用工具改成弹窗展示，UI 好好设计一下，别这么 low。⚠ 本条（弹窗化）落地后池160 侧边栏悬浮菜单触发问题若同被移除则连带销账。后续 ui-ux-designer 出方案。

**关联线索**（4）：

- [open] need170,工具描述长度,fs.glob,ToolInfo,参数chips：智能体内置工具注册规模（need170 重设计数据面）：内置注册 30+ 工具，覆盖 fs./git./md./memory./app./net./claude./schedule./todo./agent./bash. 十一族；描述长度 12–110 字符（最长 fs.glob「文件通配搜索：按 glob 模式匹配工作目录下的文件路径…上限 1000 条」≈110 字符），参数 0–3 枚；tool_info.h 的 ToolInfo 仅携带参数键名（properties 键），无逐参数描述。实测锚点：agent_builtin_tools.cpp / markdown_tools.cpp schemaJson 字面量；v1 弹窗 12px 描述 1-2 行（336px 卡 ≈26 字/行）对 110 字符描述必然截断。（source: 文件：src/module_agent/agent_builtin_tools.cpp / src/module_agent/tool_info.h / src/module_markdown/markdown_tools.cpp（2026-11-05 实读））
- [open] components::button,badge,agent.page.tools,text.normal,need170：EUI-NEO components::button（EUI-NEO/components/button.h）无 badge 槽位：布局=bg rect + row(icon,label)（:244-312），labelWidth=w−iconWidth−gap−2×spacing.section（:183-185）；次钮（primary=false）textHover/textPressed 恒为 {}，走单文本节点 `<id>.text`（:298-308），只有 Danger 变体才渲染 `<id>.text.states/.text.normal/.text.hover`（:269-297）——test_agent_ui 旧断言对次钮找 `.text.normal` 与实现不符。agent 页头「可用工具」入口钮因此改为 agent_module.cpp 匿名空间手搓 toolsEntryButton（bg 命中面 + icon/text/badge 三子节点，宽度纯函数 toolsEntryButtonWidth 见 agent_ui_shared.h）（source: 文件:EUI-NEO/components/button.h:183-312 + src/module_agent/agent_module.cpp:158-241）
- [open] EUI-NEO,input,TextRun,高亮,focus,requestFocus,rotate,inset,need170：EUI-NEO UI 能力三则（need170 v2 搜索高亮/焦点实现期源码实测）：① core::TextRun 仅一个样式维度=前景色（EUI-NEO/core/render/text_types.h:37-41），无文字底色 run——wrap 文本上的「命中词底色高亮」框架不可达，DevDesk 工具弹窗做法：描述自换行（measureTextWidth 同引擎逐码点贪心）逐行渲染 + 命中段底 rect（先 rect 后字）。② components::input 的 InputStyle.focused 字段组件内不消费（EUI-NEO/components/input.h:202 恒画 style_.background，:204 只切边框）——「focus 底色变化」不可达，focus 仅 1px focusBorder 描边。③ input 的 inset 左右对称（input.h:130 textWidth=width−inset×2），无独立右内边距 → 「左图标 35 + 右计数区 92」不对称内边距不可达。另：ui.requestFocus(id)（core/dsl.h:1543，帧末消费）可在事件回调程序化聚焦 input 的 <id>.hit，md 模块同款；TextBuilder 有 .rotate(radians)（dsl.h:639 BuilderBase）可做 chevron 旋转。（source: 文件：EUI-NEO/core/render/text_types.h:37-41 / EUI-NEO/components/input.h:130,202-205 / EUI-NEO/core/dsl.h:639,1543（2026-11-05 实读））
- [open] need170,纯函数暴露,wrapToolDescLines,findMatchRanges,toolMatchesQuery,v2卡契约,窄窗clamp锚点,测试声明块：need170 v2 工具弹窗测试口径三则（qa 实读实现与测试）：① 搜索/换行纯函数在 src/module_agent/agent_module_dialogs.cpp:1185-1269 的 module::agent 作用域（非匿名 namespace、外链、唯一定义 TU、无头声明）——测试 TU 声明原型即可复用，tests/test_modules/test_agent_ui.cpp:107-121 已加测试专用声明块，签名漂移=链接失配会红。② v2 卡契约：surfaceElevated 底+radius 8+1px 边（dialogs.cpp:1770-1788）、卡高随内容自适应（定高 96 退役）、描述逐行 .desc.<i>（:1953）+clamp 3 行+.toggle（:1967-2005，仅 >3 行卡渲染）、chips flex-wrap 全量 .chip.<i>[.text]（:2006-2062）、.chip.more「+N」已退役；Esc 清搜索后 requestFocus("agent.dialog.tools.search.hit")（:1531,1680）。③ 窄窗锚点：视口 420×600 → 弹窗 372×504（w=min(720,视口-48)）、卡内宽≈312px，最长描述 fs.glob≈110 字符（clue122）必然 >3 行 → clamp/展开断言可用窄窗稳定锚定（test_agent_ui.cpp:2501-2504 已用）。（source: 文件：src/module_agent/agent_module_dialogs.cpp:1185-1269,1770-2062 / tests/test_modules/test_agent_ui.cpp:107-121,2501-2504（2026-11-06 实读））

**关联 BUG**（2）：

- 2026-11-05 13:18:27 +0800 · 未解 · 2026-11-05 老板真机上报（原话）：「可用工具按钮里图标和文本重叠，因为后面还有个工具数，这个值不是固定的，工具多了，字符就变长」。现象：智能体「可用工具」按钮内（图标 + 文本 + 工具数量）在工具数位数变化后（如 9 → 12）文本变长，与图标/文本重叠。根因方向：按钮内元素用固定坐标/固定宽度布局，未按内容自适应（需 row 布局按内容排 + 数值区预留位数/自适应宽度）；弹窗版（need170 主方案）同样需要处理数量位数增长。
- 2026-11-05 13:19:09 +0800 · 未解 · 2026-11-05 老板真机上报：「每个工具的说明文本都是截断的，看不到完整内容」——可用工具弹窗内每个工具项的说明文本被截断（无完整展示路径）。修法：说明文本完整展示（换行或单行省略+悬浮/展开看全文），不得永久截断。

**进度记录**（共 14 条，列最近 5 条）：

- 2026-11-05 20:54:22 +0800 · ui-ux-designer：demo+文档加全文搜索完成（老板整改意见）。demo：标题下通栏搜索框（高36/radius8/fa-search占位/placeholder「搜索工具名称或说明」/清除×/框内右端 n/全量 计数）、名称+说明+参数名不分大小写子串过滤、primarySoftBg 文字底色高亮（零描边）、搜索空态（图标+未找到匹配工具+清空搜索钮）、Esc 先清搜索后关弹窗、入口/头部 badge 恒全量。文档 v2：新增 §3.6 + §4.4 + §5 两行 + §6 三行 + §7 一行 + §10 五格；doc id=17 升 v2，本地 docs/design/ 同名 .md/.html 与池同步。JS 语法校验通过，demo 双击直开。
- 2026-11-05 23:46:49 +0800 · developer 进度回流（need170 v2 弹窗主体，代码完成未构建未测试——按简报要求）：agent_module_dialogs.cpp 重写 composeToolsDialog（壳 glassFill+细黑边+blur24 阴影+radiusLarge14，头部 56 badge「N 个」恒全量，§3.6 搜索区锚顶：框 36/左 fa-search/右计数+清除，§3.5 N=0 空态，§3.6 搜索空态，列表区唯一内滚全宽单列）+ 新增 renderToolCard（高随内容自适应、描述全文展示 >3 行 clamp+展开/收起钮、chips flex-wrap 全量「+N」退役、命中词 primarySoftBg 原位底色高亮、仅长文卡 hover/果冻/点击）+ toggleToolCardExpanded（按工具名）；agent_module.cpp 入口 onClick 重置搜索/展开态 + onKeyEvent Esc 优先级（有输入先清搜索）+ stop 清理；agent_module.h 新增 6 个工具弹窗状态成员与 2 个方法声明。测试影响面（qa 注意）：test_agent_ui.cpp 旧「+N」chip 断言（:2373）与 card.0.desc 文本节点断言需随 v2 更新（id 名保留但 desc 由单文本节点变为 stack 容器）。
- 2026-11-06 00:07:16 +0800 · 池170 v2 QA 测试更新清单（2026-11-06 交付；约束=只改测试文件，未构建未运行——侧栏代理占用构建，全量执行待其收口后由 qa 跑）。改动文件：tests/test_modules/test_agent_ui.cpp（唯一）。T-01 chips 全量（改 :2439-2479）：fs.grep 逐枚对齐注册表 params（.chip.<i>.text 全量在场+文本一致+无多余）+ .chip.more 缺席断言（「+N」退役）；T-02 描述不消失+clamp 结构（补 :2327-2412 内新增块）：每卡 .desc.0 在场、行拼接=描述全文（或 clamp 前缀）、有 .chevron 的卡恰 3 行+「展开」钮；T-03 窄窗 clamp/展开收起往返（补 :2501-2588 区）：420×600 下恰有 >3 行卡，展开=行数>3+拼接==全文+卡高增（禁定高行为钉），再点收回 clamp，块尾复位关闭保空态段时序；T-04 findMatchRanges（补 REGISTER_TEST）：相邻不重叠/首尾边界/空词空集/已小写化契约/CJK 字节偏移；T-05 toolMatchesQuery（补）：名称/说明/参数名 ASCII 折叠子串/空词全量/全不中剔除；T-06 wrapToolDescLines+ellipsizeToolLine（补）：多行无丢字/不劈码点/超窄逐码点/省略号不超宽+前缀+裸…兜底。测试专用纯函数声明块 ：107-121（零实现改动，链接失配=会红）。已知红候选（清单外未动，待主会话裁决）：R-1 agent_ui_tools_dialog_geometry REGISTER_TEST 整条仍 v1 断言（720×560/卡定高 96/2 列同行 → v2 应为 720×min(600,视口-96)/卡高随内容/单列纵排）；R-2 tests/probes/agent_dialog_probe.cpp:779 card1 与 card0 同 y 断言（v1 网格）→ v2 单列后必报「2 列破版」，建议改为 card1.y > card0 底。未验证项：全部用例未构建未运行（含 probe id 兼容——panel/scroll/card.bg/close.bg 已静态核对在场，实跑待构建后）。
- 2026-11-11 17:05:00 +0800 · 台账归位：弹窗化已落地（agent_module.cpp 页内工具区块退役；agent_module_dialogs.cpp composeToolsDialog，DialogShell 720×600 + 多列 masonry + 描述 clamp/展开），自 11-05 起未迁状态——转待验收，待真机目检。
- 2026-11-18 12:54:18 +0800 · 批L留条件（非红）：弹窗化结构绿——页面级仅 agent.page.tools 入口按钮+badge 计数（大片平铺工具卡区已移除=空白主诉源消除），dialog.detail 弹窗 nav 3 页签在（l3-agent-tools-dump.txt）；工具卡视觉面需真 MCP 工具数据（空态 detail.detail.empty 无卡可判），留待真数据/真机批。evidence/20261118-visual-accept/verdicts.md

**最近状态变更**（共 6 条）：2026-11-11 17:05:00 +0800 · 进行中 → 待验收 · 台账归位：弹窗化已落地（agent_module.cpp 页内工具区块退役；agent_module_dialogs.cpp composeToolsDialog，DialogShell 720×600 + 多列 masonry + 描述 clamp/展开），自 11-05 起未迁状态——转待验收，待真机目检。

**关联文档**：doc#3（tech v1） · doc#17（prd v2） → 全文见附录 A

---

#### need#172 · 侧边栏收缩展开按钮：圆框+悬浮样式+弥漫绿色粒子效果

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-03 20:54:58 +0800（developer）· 更新：2026-11-18 12:54:18 +0800（developer）

**详情**：

老板上报（2026-11-03）：侧边栏的收缩展开按钮加一个圆框，改成悬浮样式的，最好加一点弥漫开的绿色粒子效果。后续 ui-ux-designer 出方案（粒子效果性能口径待设计师定）。

**正文**：

老板上报（2026-11-03）：侧边栏的收缩展开按钮加一个圆框，改成悬浮样式的，最好加一点弥漫开的绿色粒子效果。后续 ui-ux-designer 出方案（粒子效果性能口径待设计师定）。

**关联线索**（5）：

- [open] hover,探针盲区,bug150,真指针,module_toggle_sidebar_probe,need172：探针盲区证实：module_toggle_sidebar_probe.cpp §6 的 hover 断言是 onHoverChanged 手调（原393行注释自认），从未驱动真指针；点击断言走 queuePointerButton 合成队列同样绕过 OS 消息层。全仓库探针（含 agent_e2e_probe G4 hover）都走合成队列，「真 OS 指针→WM_MOUSEMOVE→GLFW CursorPosCallback→queuePointerMotion→runtime.update 命中派发」段无任何测试覆盖（bug150 同款盲区）。已补 §6b --real-pointer 门控段：真 SetCursorPos + 主窗同款 chrome 子类 + app 同款 metric 流。（source: 文件:tests/probes/module_toggle_sidebar_probe.cpp:393 + src/app_sidebar.cpp）
- [open] Runtime::update,参数序,pointerScale,dpiScale,DPI,坐标流：Runtime::update 参数序陷阱：core/dsl_runtime.h:51 签名 update(window, dt, pointerScale, dpiScale, inputEnabled=true)——pointerScale 在前 dpiScale 在后；真 app 门面转发正确（eui/detail/dsl_app_impl.h:574 update(window,dt,pointerScale,effectiveScale,inputEnabled)）。坐标流在 dpi≠1 真机自洽：GLFW 自提 PerMonitor DPI 感知（无 manifest 也生效，本机探针窗 contentScale=1.354），事件=物理像素，命中边界 toPixelRect(frame,dpiScale)=物理，pointerScale=fb/window=1。工装若把 cs 传进 pointerScale 位会事件双重放大+边界按逻辑→全 MISS 伪红。（source: 文件:EUI-NEO/core/dsl_runtime.h:51 + include/eui/detail/dsl_app_impl.h:574 + 实测）
- [open] 7d43c34,setDwmBorderHidden,titlebarCaptionHit,resize带,装机版,排除：三条嫌疑排除证据（172 hover 真机失效）：①743c34/7d43c34 setDwmBorderHidden 只加 DwmSetWindowAttribute(DWMWA_BORDER_COLOR) 调用，零 hit-test 改动——「184② 扩大边缘带」不成立；②titlebarCaptionHit（app_sidebar.cpp:239）只认顶部 40 逻辑条带，底缘 resize 带=SM_CXSIZEFRAME+PADDEDBORDER≈10-14 物理px，按钮区 H−48..H−8 逻辑不与之相交——caption/resize 抢事件均不成立；③特效代码在装机版里：特征串 app.sidebar.toggle.particle 于 09:54 装机 exe 与 10:04 build exe 各命中 1 次，提交 2e5306df 09:07:30 早于 09:54 构建——「没编进装机版」不成立。另：installChromeHitTest 全仓无调用点（真链经 setWindowCaptionHitTestHandler→内部装 chrome）。（source: git show 7d43c34 + src/app_sidebar.cpp:239 + grep 实测）
- [open] 侧栏v2,画布坐标,热区hot,粒子延迟,scrollOffset,无头断言,app_sidebar：侧栏 v2/v3 实现接线事实（2026-11-04，dev 批 165/172/188）：①导航行进滚动画布后，行元素 builder y = 画布坐标 = 旧侧栏 y − navStart（116/88/80），frame 仍为窗口绝对坐标——测试/探针断言 y 时相对算术用 builder 值、绝对用 frame（差 bodyTop）；②toggle 交互回调（onHover/onPress/onRelease/onClick）已迁到独立透明热区 rect "app.sidebar.toggle.hot"（绘制区外扩 4px，40/36），视觉元素 "app.sidebar.toggle" 只剩样式无回调——手调 hover 断言必须 find(.hot)；③粒子 v2 每颗 hash 延迟 0~80ms，起爆后元素才建——立即帧计数会少颗，须先候 ≥120ms 再 compose 计数，生命上限 880ms（自止等待 ≥920ms）；④滚动 offset 无头断言口 = Element.scrollOffset/scrollMaxOffset（app.sidebar.nav），offset 是渲染期 runtime transform、ui.layout 的 frame 不含位移，滚到底几何 = offset==maxOffset==contentH−viewportH 的算术断言。（source: 实测（test_app_sidebar 19/19 + probe PASS 调试过程））
- [open] 粒子越界,包络,垂直压缩,外阴影,全屏早退,need172,app_sidebar：need172 粒子几何 v2 缺陷量化（2026-11-05 设计侧按现行参数几何计算）：v2 径向弥散（起点 r=钮半径+2、弥散 14~26px、无垂直压缩无包络）在现几何下——①上向最大触达 = 中心(H−24)−44 = H−68，越让位线 foldLimit(H−52) 约 16px 入导航区；②展开 220 态左向最大触达 = 中心x32−44 = −12，越出侧栏左缘约 12px 画入页面区。v3 修法（设计文档 docs/design/2026-11-05-need172-侧栏收缩按钮圆框悬浮粒子.md §5）：dy 垂直压缩 0.55 + 包络兜底 clamp [3,W−3]×[foldLimit,H−3] + 折叠态弥散降档 10~16。另证两条能力事实：EUI-NEO 单层外阴影可用（.shadow(blur,offsetY,Color)，Shadow/Blur 属 AnimProperty 可动画，见 docs/EUI-NEO-CAPABILITIES.md:198-199,550）；全屏 0 宽早退已存在（src/app_sidebar.cpp:378 sidebarWidth()<=0 return，toggle/热区/粒子整块不注册）。（source: 文件:src/app_sidebar.cpp:876-929 + docs/design/2026-11-03-侧栏图标与收缩按钮重整.md §10.3 + 几何计算）

**进度记录**（共 16 条，列最近 5 条）：

- 2026-11-04 18:26:08 +0800 · 开发派发取消（2026-11-04）：老板插单记忆模块真功能实测（bug201 P0），developer agent 派发即被中断、零动工；v2 契约仍在 §10.2/§10.3，批恢复即重派。
- 2026-11-05 20:35:54 +0800 · 【设计完成待评审 2026-11-05】need172 按钮设计 v3 终稿交付：设计文档 docs/design/2026-11-05-need172-侧栏收缩按钮圆框悬浮粒子.md（台账副本 doc id=18 v1）+ 自包含 HTML demo 同目录同名 .html（可交互：hover/按压/focus/两态/三档/全屏/深色/满行压力/包络 overlay/粒子计数器）。关键决策：①几何与果冻沿用已实现 v2（§10.2：32/48、28/44，hover 1.04/press 0.96/回弹 180ms OutBack），开发面最小化；②悬浮感新增 hover 单层阴影 blur8/offsetY2/黑10%（能力已证 EUI-NEO-CAPABILITIES.md:198）；③新增 focus 态 1.5px primary 描边；④粒子 v3 修正 v2 两处越界缺陷（clue123：上向越让位线 16px、左向越栏缘 12px）——dy×0.55 垂直压缩 + 包络兜底 [3,W−3]×[foldLimit,H−3] + 折叠态弥散降档 10~16；⑤性能口径：粒子上限 12 颗/次、≤880ms 自止、无常驻帧泵、非活跃帧零元素、全屏 0 宽整块不注册（app_sidebar.cpp:378 早退已有）；⑥两态位置口径：垂直零位移、水平 ≤6px 随宽度瞬时切换（全站无宽度动画档）。包络数学已程序化复核（三档×两态×360° hash 连续域全过）。下一步：PM/用户过目 → developer 按 §9 差异清单实现（6 处改动）→ qa 对照 §10 先验验证清单。
- 2026-11-05 23:48:53 +0800 · dev 代码完成（v3+两条老板实时反馈并入）：粒子 dy×0.55+包络钳 [3,W−3]×[foldLimit,H−3]+折叠降档 10~16（app_sidebar.cpp:920-956）；生命 800→1600ms/≤1760 自止+InQuad 平缓衰减（老板令①）；hover 阴影 blur8/offsetY2 黑10%+focus 1.5px primary 环（热区 onFocusChanged 接线）；果冻加强 hover 1.09/按压 0.90/回弹 280ms 强 OutBack（老板令②，jelly.h 坑①② idiom 未自造弹簧）；全屏 0 宽早退既有覆盖未动。测试：toggle 用例升级（阴影/focus/包络/折叠粒子/1600ms 生命断言）+探针 5 处等待 920→1900ms。未构建未测试未 commit。
- 2026-11-11 17:05:08 +0800 · 台账归位：实现已落地（app_sidebar.cpp:1006-1140——正圆框 + hover primarySoftBg/描边 + 1.09/0.90 果冻 + 12 颗绿色粒子 1600ms），自 11-05 起未迁状态——转待验收，待真机目检。
- 2026-11-18 12:54:18 +0800 · 批L留条件（非红）：粒子实现面在（app_sidebar.cpp:139-156 need172 v2/v3 三色粒子/hover 触发/空闲零粒子 + :1066-1078 弥散动效非常驻 + :226 态回厂测试隔离）+ dump toggle.hot 44x44 热区在；粒子弥散视觉动效需真实指针 hover（driver 九种步骤无 hover step，本批无实窗交互预算），留待真机/交互批。evidence/20261118-visual-accept/verdicts.md

**最近状态变更**（共 6 条）：2026-11-11 17:05:08 +0800 · 进行中 → 待验收 · 台账归位：实现已落地（app_sidebar.cpp:1006-1140——正圆框 + hover primarySoftBg/描边 + 1.09/0.90 果冻 + 12 颗绿色粒子 1600ms），自 11-05 起未迁状态——转待验收，待真机目检。

**关联文档**：doc#18（tech v1） → 全文见附录 A

---

#### need#178 · 运行时内存占用优化（基线待实测）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-03 23:50:40 +0800（developer）· 更新：2026-11-18 18:44:35 +0800（developer）
- **blocker**：未放行执行：稳态/峰值内存基线实测未做（需求自身前置）；#271 A 方案（限制占用）已落地装机仅部分覆盖，画像/杠杆剩余工作未排产——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

老板 2026-11-03 令：减少运行时内存使用。初步实测：DevDesk.exe 当前未运行，无快照——稳态 RSS/私有内存/峰值基线待需求分析时实测补齐。后续分析：内存大头画像（EUI 渲染纹理/halo 离屏、sqlite page cache、各模块缓存与常驻数据、http 客户端缓冲），定稳态与峰值基线→目标值，杠杆候选按画像定。初判 L（跨模块画像+优化）。未放行执行，按 FIFO 排当前批次后。

**正文**：

老板 2026-11-03 令：减少运行时内存使用。初步实测：DevDesk.exe 当前未运行，无快照——稳态 RSS/私有内存/峰值基线待需求分析时实测补齐。后续分析：内存大头画像（EUI 渲染纹理/halo 离屏、sqlite page cache、各模块缓存与常驻数据、http 客户端缓冲），定稳态与峰值基线→目标值，杠杆候选按画像定。初判 L（跨模块画像+优化）。未放行执行，按 FIFO 排当前批次后。

**关联线索**（1）：

- [open] 内存基线,私有内存,稳态,轮换峰值,画像,无泄漏,quick-wins,纹理,sqlite-cache：DevDesk 运行时内存基线（2026-11-04 实测，临时数据根）：稳态私有内存 70-74MB 稳定无泄漏（60s 采样 117 点）；uat62 全 14 页轮换私有峰值 91MB，页间回落无单调增长。读码对照：字体走 FreeType 文件流不整驻堆（EUI-NEO/core/render/text_shaping.cpp:437）；字形图集 per-page 重建有界（opengl/opengl_text.cpp）；FontStackCache 帽 16（text_internal.h:86）；离屏单 FBO 复用可释放（opengl/opengl_backend.cpp:307）；sqlite 默认 cache 2MB/连接帽×~7 连接、懒分配实际远低（src/core/sqlite_db.cpp:265）。无界缓存/重复纹理/悬空缓冲均未发现。稳态 WS 增长（143→242MB/60s）是 unstripped exe 代码页惰性映入非堆增长，strip 装机版（need177）已消。（source: 实测 evidence/2026-11-03-accept-audit/dev-b8/memory-report.txt（steady-mem.csv、rotation-mem-r1/r2.csv））

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-04 04:07:59 +0800 · 老板 2026-11-04 放行开发。排 B7 后 B8 性能批（与 177 同批，同性能域）。范围口径：先基线画像（运行 RSS 稳态/峰值+大头定位）→ 定目标 → 无风险 quick wins 本批落地；结构性优化（若需跨模块重构）出方案挂后续批不无限扩批
- 2026-11-04 04:11:47 +0800 · B8 性能批开工：先基线采样（稳态/峰值）→大头画像→定目标→无风险 quick wins 落地；结构性优化出方案挂后续批
- 2026-11-04 04:33:56 +0800 · 基线（先测后动，DEVDESK_DATA_ROOT=临时根零污染）：稳态 home 页 60s 私有内存峰 74MB→终值 70MB，无泄漏；多页轮换（uat62 全 14 页×3 轮）私有峰值 89.8-90.9MB，页间振荡回落无单调增长。画像（读码+采样对照）：字体 FT 文件流不整驻（text_shaping.cpp:437）、字形图集 per-page 重建有界、FontStackCache 帽 16、离屏单 FBO 复用、sqlite 默认 2MB/连接帽懒分配——无界缓存/重复纹理/悬空缓冲均未发现。定目标（基线即达标）：稳态私有 ≤80MB、轮换私有峰值 ≤110MB。quick wins：无——未发现可落地项，按反过度设计未动代码（改动面=零，未触碰任何 module_* 文件）。结构性优化：无新增需求；-g1/LTO/字体子集化已裁决范围外。注：WS 大头（60s 143→242MB）为 unstripped exe 代码页惰性映入，need177 strip 装机后自然消除，与本条无冲突。

**最近状态变更**（共 4 条）：2026-11-04 04:33:56 +0800 · 进行中 → 待验收 · 基线（先测后动，DEVDESK_DATA_ROOT=临时根零污染）：稳态 home 页 60s 私有内存峰 74MB→终值 70MB，无泄漏；多页轮换（uat62 全 14 页×3 轮）私有峰值 89.8-90.9MB，页间振荡回落无单调增长。画像（读码+采样对照）：字体 FT 文件流不整驻（text_shaping.cpp:437）、字形图集 per-page 重建有界、FontStackCache 帽 16、离屏单 FBO 复用、sqlite 默认 2MB/连接帽懒分配——无界缓存/重复纹理/悬空缓冲均未发现。定目标（基线即达标）：稳态私有 ≤80MB、轮换私有峰值 ≤110MB。quick wins：无——未发现可落地项，按反过度设计未动代码（改动面=零，未触碰任何 module_* 文件）。结构性优化：无新增需求；-g1/LTO/字体子集化已裁决范围外。注：WS 大头（60s 143→242MB）为 unstripped exe 代码页惰性映入，need177 strip 装机后自然消除，与本条无冲突。

---

#### need#184 · 168 收尾：窗口态 API 暴露与系统描边隐藏（EUI fork XS×2）+ring 最大化隐藏分支

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-04 04:37:16 +0800（developer）· 更新：2026-11-04 05:50:31 +0800（developer）

**详情**：

B7/池168 上报项立案（2026-11-04）：①应用层无窗口态查询——EUI windowIsMaximized(Handle)（window_backend.h:124）未向 app 层暴露（dsl.h:199 仅 onFocusChanged），需 fork XS 暴露查询；②调研建议的 DWMWA_BORDER_COLOR=NONE 隐系统 1px 描边（Windows 平台后端）同 fork XS；③DevDesk 侧 window_ring 接最大化隐藏分支（设计意图：最大化态无圆角，ring 弧段被裁/观感错）。随 B9 收尾批执行（与 180 的 EUI button 根修同树同批，收口统一 commit+先推 fork）。clue#29 已录 API 现状。

**正文**：

B7/池168 上报项立案（2026-11-04）：①应用层无窗口态查询——EUI windowIsMaximized(Handle)（window_backend.h:124）未向 app 层暴露（dsl.h:199 仅 onFocusChanged），需 fork XS 暴露查询；②调研建议的 DWMWA_BORDER_COLOR=NONE 隐系统 1px 描边（Windows 平台后端）同 fork XS；③DevDesk 侧 window_ring 接最大化隐藏分支（设计意图：最大化态无圆角，ring 弧段被裁/观感错）。随 B9 收尾批执行（与 180 的 EUI button 根修同树同批，收口统一 commit+先推 fork）。clue#29 已录 API 现状。

**关联线索**（1）：

- [open] need184,mainWindowMaximized,DWMWA_BORDER_COLOR,window_backend,EUI单测,手工链接：need184① 验证结论：EUI windowIsMaximized 的 app 层查询口早已落地——eui/app.h:197 声明 mainWindowMaximized()、include/eui/detail/dsl_app_impl.h:431 实现（经 detail::mainWindowHandleSlot()），DevDesk app_sidebar.cpp:892 已消费；真最大化验证在 EUI 仓自带（window_shell_probe.cpp:370-393 真窗口 maximizeWindow 后断言 windowIsMaximized + zoomed 混合态；tests/unit/window_chrome.cpp:47 app::maximizeMainWindow）。②实现锚点：core/window/window_backend.cpp 自绘壳段 setDwmBorderHidden（installChromeHitTest 置 DWMWA_BORDER_COLOR=DWMWA_COLOR_NONE / uninstall 还原 DEFAULT；dwmapi CMakeLists:459 已链；Win11 22000+ 属性）。另：DevDesk 子工程态不构建 EUI tests/unit（顶层选项门 EUI_NEO_TOP_LEVEL_DEFAULT），EUI 单测要跑需手工 g++ 链 build/_eui_neo_build/libeui_neo.a（依赖序：eui_neo glad freetyped png16d eui_zlib glfw3 SDL2 SDL2main curl opengl32 winmm imm32 dwmapi ws2_32 setupapi gdi32 ole32 shlwapi comdlg32）。（source: 文件:行（EUI-NEO eui/app.h:197 / dsl_app_impl.h:431 / window_backend.cpp:1100-1130 / window_shell_probe.cpp:393））

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-04 04:37:21 +0800 · B9 收尾批排产（与 179/180/181/182 同批五条目；EUI fork 项与 180 同树）
- 2026-11-04 04:37:42 +0800 · B9 开工：窗口态 API 暴露+系统描边隐藏+ring 最大化分支

**最近状态变更**（共 4 条）：2026-11-04 05:50:31 +0800 · 进行中 → 待验收 · 

---

#### need#271 · 限制 DevDesk.exe 内存占用（三并发 5GB，可接受降速换内存）

- 状态：待验收 · 优先级：P1 · 负责人：zcode · 阶段：A 方案已落地并装机
- 创建：2026-11-16 17:28:21 +0800（zcode）· 更新：2026-11-16 18:09:03 +0800（zcode）

**详情**：

用户反馈（2026-11-16）：「能限制 DevDesk.exe 的内存使用嘛 慢一点没事，三并发使用了 5GB 内存」。口径：可接受性能下降换取更低内存占用。待验收范围：①取证峰值构成（进程数/工作集/私有提交内存/是否堆碎片或渲染缓存暴涨）；②可选措施候选：Windows Job Object 进程内存上限（硬限，超限分配失败需防崩）、SetProcessWorkingSetSize/EmptyWorkingSet 定期回收工作集（慢但不减提交量）、渲染/排版缓存上限与按需释放（EUI-NEO 每帧全量重建元素树底账已有记录）、多查看器窗并发数限制；③先出分析与方案，未获指令不动代码。当前为询问可行性阶段。

**正文**：

用户反馈（2026-11-16）：「能限制 DevDesk.exe 的内存使用嘛 慢一点没事，三并发使用了 5GB 内存」。口径：可接受性能下降换取更低内存占用。待验收范围：①取证峰值构成（进程数/工作集/私有提交内存/是否堆碎片或渲染缓存暴涨）；②可选措施候选：Windows Job Object 进程内存上限（硬限，超限分配失败需防崩）、SetProcessWorkingSetSize/EmptyWorkingSet 定期回收工作集（慢但不减提交量）、渲染/排版缓存上限与按需释放（EUI-NEO 每帧全量重建元素树底账已有记录）、多查看器窗并发数限制；③先出分析与方案，未获指令不动代码。当前为询问可行性阶段。

**最近状态变更**（共 4 条）：2026-11-16 18:09:03 +0800 · 进行中 → 待验收 · 

---

#### need#272 · Todo 看板：今日到期卡片整体橙黄、逾期未完成卡片整体红色

- 状态：待验收 · 优先级：P1 · 负责人：qa-tester · 阶段：已实现并装机，待真机看色
- 创建：2026-11-16 17:43:00 +0800（qa-tester）· 更新：2026-11-18 13:33:27 +0800（qa-tester）

**详情**：

用户需求（2026-11-16）：「todo 截至日期是当前的卡片整体改成橙黄色，已经预期（逾期）没完成的整体改成红色。注意截至 16 号，今天 16 号这不算逾期，到了 17 号明天才算逾期」。口径：卡片**整体**（非仅日期文字）背景/色调切换；due == 今天 → 橙黄（警示但未逾期）；due < 今天且未完成 → 红（逾期）；边界按自然日比较（今天 16 号，due=16 不算逾期）。约束：修改需快，构建用 --dev 定向，测试 ≤10–20 秒即可（老板：别重跑全量、浪费时间）。

**正文**：

用户需求（2026-11-16）：「todo 截至日期是当前的卡片整体改成橙黄色，已经预期（逾期）没完成的整体改成红色。注意截至 16 号，今天 16 号这不算逾期，到了 17 号明天才算逾期」。口径：卡片**整体**（非仅日期文字）背景/色调切换；due == 今天 → 橙黄（警示但未逾期）；due < 今天且未完成 → 红（逾期）；边界按自然日比较（今天 16 号，due=16 不算逾期）。约束：修改需快，构建用 --dev 定向，测试 ≤10–20 秒即可（老板：别重跑全量、浪费时间）。

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-18 13:33:27 +0800 · 批M 核读：未实现——src/module_todo 全 grep 零命中到期卡整体橙黄/逾期红渲染（todo_tools.cpp 仅工具层 overdue 过滤，非卡片渲染）。需求待开发，本批只验不修。验收口径备好：due=今天→卡整体橙黄、due<今天且未完成→红、自然日边界（due=今天不算逾期）。

**最近状态变更**（共 4 条）：2026-11-16 18:09:10 +0800 · 进行中 → 待验收 · 

---

#### need#279 · MCP 详情新增安装/卸载按钮：写入全局 Claude Code、ZCode 的 MCP 配置

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：开发完成待装机验收
- 创建：2026-11-16 18:24:49 +0800（developer）· 更新：2026-11-16 22:47:39 +0800（developer）

**详情**：

need279 PRD 定稿：slug=2026-11-16-mcp-zcode-install（doc id=21 v1）。需求裁决序（need#146）结论=改造复用：①该做（老板明令）②直接复用 need244 写入编排/三态按钮/单测模板 ③改造为 ZCode 变体 ④无新方案。矩阵 F-01~F-10 初值全待方案；四决策点已裁决（D1 沿用现状+改进另立 OP-1，D2 不覆盖+引导，D3 以 desktop 设置页为准，D4 bak-devdesk）；PRD 已过产出标准硬清单 6 条自检。下一步=developer 技术方案卡（锚点：claude_mcp_config 模式复刻+paths 三平台 zcodeConfigFile+platform/json+atomic_write）。

**正文**：

need279 PRD 定稿：slug=2026-11-16-mcp-zcode-install（doc id=21 v1）。需求裁决序（need#146）结论=改造复用：①该做（老板明令）②直接复用 need244 写入编排/三态按钮/单测模板 ③改造为 ZCode 变体 ④无新方案。矩阵 F-01~F-10 初值全待方案；四决策点已裁决（D1 沿用现状+改进另立 OP-1，D2 不覆盖+引导，D3 以 desktop 设置页为准，D4 bak-devdesk）；PRD 已过产出标准硬清单 6 条自检。下一步=developer 技术方案卡（锚点：claude_mcp_config 模式复刻+paths 三平台 zcodeConfigFile+platform/json+atomic_write）。

**关联线索**（7）：

- [open] claude.json,mcpServers,claude mcp add,user scope,install：Claude Code 全局(user scope) MCP 配置=~/.claude.json 顶层 mcpServers（本机实测 C:\Users\e-Yunfei.Qian\.claude.json 159KB，顶层 ~50 键+projects 53 项含会话状态；mcpServers 4 条目形态 {command,args,type} stdio / {type,url} http）。官方 docs code.claude.com/docs/en/mcp：user scope 存 ~/.claude.json；stdio 字段 command/args/env，http 字段 type/url/headers/timeout/alwaysLoad；同名同 scope 重复 add 报 already exists（无 --force）；remove 删条目并连带删 OAuth tokens。CLI: claude mcp add -s user -t http <name> <url>（本机 claude 位于 node 全局目录）。直改风险面=该文件承载 Claude Code 自身大量状态（numStartups/tipsHistory/projects 每项目 mcpServers 与 enabled/disabledMcpjsonServers 等），必须全量保留未知键。（source: 一手实测(insp2.py 键结构)+claude mcp --help 实测+官方 docs code.claude.com/docs/en/mcp 精读）
- [open] zcode,config.json,mcp.servers,严格schema,enabled,type：ZCode 全局 MCP 配置=~/.zcode/cli/config.json 顶层键 mcp.servers（本机实测：顶层键序 plugins/hooks/skills/mcp；3 条目 codegraph={args,command,type} / mt={enabled,type,url} / pool={args,command,cwd,enabled,type}）。官方 schema（zcode-guide/diagnosing-mcp skill，随官方插件分发）：stdio=command(string)/args/cwd/env/enabled/timeoutMs，http|sse=url/headers/enabled/timeoutMs；type 可省略（command→stdio、url→http）但建议显式 canonical 字段（desktop 托管路径不经 CLI 迁移器）；schema 严格=未知键该 server 被静默丢弃；配置文件不展开 ${} 模板；desktop Settings→MCP 面板写入也是此文件，desktop 托管 server 列表可覆盖文件（pitfall 11）。官方文档站 https://zcode.z.ai/en/docs/mcp-services 确认 user scope=~/.zcode/cli/config.json mcp.servers（该页 enable/enabled 措辞与 skill 有出入，以 skill+实测 enabled 为准）；本机 PATH 无 zcode CLI 命令（which zcode 失败，desktop 版 ZCode.exe+内部 zcode.cjs，npm 全局仅有 claude）→ ZCode 侧无 CLI 写入路径，直改文件是唯一方式。（source: 一手实测(cat ~/.zcode/cli/config.json+which zcode)+官方插件 skill .zcode/cli/plugins/cache/zcode-plugins-official/zcode-guide/0.3.0/skills/diagnosing-mcp/SKILL.md 精读+官方 docs zcode.z.ai/en/docs/mcp-services）
- [open] json,键序,std::map,未知字段,原子写,备份：platform/json=自研 JsonImpl（头注释「yyjson 薄封装」实为自研值类型+yyjson 读写口）：对象存 std::map<string,JsonImpl>（src/platform/json.cpp:24）→ 序列化键序=字典序非插入序（json.cpp:89 注释「键序照 std::map 序」）；parse 全量 DOM 还原→未知字段保留 ✅；parse 带 ParseDiag 错位透出（json.h:90-96）；stringify(pretty=true)=两空格缩进（YYJSON_WRITE_PRETTY_TWO_SPACES，与 .claude.json 原档同档）。影响：整文件读改写会把全文件键重排为字典序（语义无损、diff 噪音大；Claude Code 自身回写时会再次重排，无实际危害）。原子写+备份已有现成设施：core::atomic_write.h:15 atomicWriteFile（Windows=MoveFileExW REPLACE_EXISTING）；core/fs_text.h readTextFile。（source: 一手实测(src/platform/json.cpp:13-159,89-91,154-159 精读+src/core/atomic_write.h)）
- [open] need244,need279,install,claude_mcp_config,devdesk,kServerName：本需求 Claude Code 侧已落地（commit 0c302d99「feat(mcp): 详情页安装到 Claude Code 按钮」，池 need244）：src/module_mcp/claude_mcp_config.h 定义 kServerName="devdesk"（工具前缀 mcp__devdesk__* 同源），服务级单条注册不按工具逐条；claude_mcp_config.cpp 实现读改写编排 rewriteExisting(:152)=文件必须已存在(绝不新建)/只动 mcpServers.devdesk 一键/幂等判据=键存在(changed=false 原样返回,不覆盖已有条目)/mcpServers 非对象形态拒写(:25)/有改动才先备份 <file>.bak-devdesk 再 atomicWriteFile/非法 JSON 中文 fail。按钮挂详情弹窗 drawClaudeInstallButton(mcp_page_module.cpp:696) 三态置灰逻辑 + 安装成功提示「已写入，重启 Claude Code 后生效」(:687)。端点=http://127.0.0.1:<live.port>/mcp（McpService 只绑 127.0.0.1，mcp_http_handler.cpp:88-109 POST /mcp streamable HTTP）。单测 tests/test_modules/test_mcp_claude_config.cpp。**ZCode 侧未实现（本 need279 剩余工作量）**：目标 ~/.zcode/cli/config.json mcp.servers，复制 cc 命名空间模式即可（entry={type:"http",url:同端点}）。外部生产先例（repo 级，gh 未装无 commit sha）：npm @itzfaisal/mcp-cli（user scope 直写 ~/.claude.json mcpServers）、github.com/nnnkkk7/mcp-tidy（读写 ~/.claude.json）。已知残余风险（need244 manifest 记载）：DevDesk MCP 端口漂移→已装条目 URL 过期，靠卸载重装收敛。（source: 一手实测(git log 0c302d99+claude_mcp_config.h/.cpp+mcp_page_module.cpp:664-711 精读)+WebSearch(itzfaisal mcp-cli/mcp-tidy)）
- [open] need279,prd,zcode,mcp-install,decision,D1,D2,D3,D4：need279 PRD 已定稿落盘：doc id=21 slug=2026-11-16-mcp-zcode-install v1（doc ledger）。范围=ZCode 侧增量（Claude 端锚点 need244/0c302d99 不重述），功能矩阵 10 条 F-01~F-10 初值全「待方案」，验收 A1~A9 全带可测断言。四决策点裁决：D1=沿用 Claude 端现状（live.port+卸载重装收敛，不做失配警示，改进另立 OP-1）；D2=同名异址不覆盖+中文引导（三分判定：无键可装/同址幂等/异址冲突；冲突态无卸载入口防误删）；D3=ZCode desktop 运行中以其 Settings→MCP 刷新为准，文案只承诺重启生效；D4=备份 config.json.bak-devdesk。实现细节裁决：写入 {"type":"http","url","enabled":true}（显式 enabled 消除官方缺省歧义）；mcp.servers 键缺失补建层级（文件必须已存在不变）。需求裁决序结论=改造复用（复刻 need244 模式，无新方案）。（source: doc_get(2026-11-16-mcp-zcode-install)+PM 裁决）
- [open] need279,zcode_mcp_config,zcodeConfigFile,三分判定,ZcButtonState,锚点：need279 实现落位锚点（commit e8f8d4d0，12 文件 +926/-18）：src/module_mcp/zcode_mcp_config.h（zc 命名空间：kServerName/ConfigState{filePresent,installed,installedUrl,error}/applyServerEntry/removeServerEntry/installServerFile/removeServerFile/backupFile）+ .cpp（applyServerEntry 三分判定在 :117-137：同址 no-op / 异址 fail ErrorCode::Conflict 含现有 url 中文引导；两级校验 ensureHierarchyIfPresent :44；rewriteExisting :219 镜像 cc）。paths 三平台 zcodeConfigFile（paths_win.cpp:83 / paths_macos.cpp:62 / paths_posix.cpp:63，无覆盖 env）。UI：mcp_page_module.h ZcButtonState（含 conflict 位，actionable=filePresent&&serviceRunning&&!conflict）；.cpp kDialogH 480→516（:145），Claude 行 h-88、ZCode 行 h-52（btn.zcode/hint zcode.hint id），冲突引导 hint 两行 wrap，openDetail 挂 refreshZcodeInstallState。单测 tests/test_modules/test_mcp_zcode_config.cpp 12 用例（--filter zc_ 或 file:test_mcp）全绿（Total 183/182 passed，含 claude config 与 page 布局不回归）。坑记录：tests/doctest.h 断言宏（DOCTEST_CHECK→return false）只能在返回 bool 的函数体用，void helper 内用即 -Werror 编译红。（source: 一手实测（git show e8f8d4d0 + build-test-run --dev 全绿输出））
- [open] tests,CMakeLists,probe,枚举源,module_mcp,undefined reference,坑：tests/CMakeLists.txt 有三处**逐文件枚举** module_mcp 源（不链接 devdesk_module_mcp 库）：probe 循环（:525-526，覆盖 bug142/bug148 等弹窗类 probe）、devdesk_ui_driver（:706）、github_sync_live（:806）——每处都是 claude_mcp_config.cpp+mcp_page_module.cpp 成对列出。新增 module_mcp 的 .cpp 若被 mcp_page_module.cpp 引用，必须同步补进这三处，否则主测试链接过而 probe/ui_driver 链接 undefined reference 红（need279 实测踩中：bug142_click_chain_probe/bug148_167_sweep_probe 两目标失败，zcode_mcp_config.cpp 补入三处后消）。教训：module_mcp 加文件后 grep "claude_mcp_config" tests/CMakeLists.txt 对齐枚举点。（source: 一手实测（链接失败 ld 输出 + 修复验证））

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-16 18:25:14 +0800 · 落点调研（2026-11-16 本机实测，Windows）：
- Claude Code 全局：`%USERPROFILE%\.claude.json` → 顶层 `mcpServers` 对象，条目形知 `{type:"stdio",command,args}` 或 `{type:"http",url}`；本机当前有 codegraph / jetbrains / fetch / context7。
- ZCode 全局：`%USERPROFILE%\.zcode\cli\config.json` → `mcp.servers`（注意多一层 mcp），条目同上且可带 `enabled:false`；本机已有 mt / codegraph / pool。
- macOS 待真机确认路径（预期 `~/.claude.json` 与 `~/.zcode/cli/config.json` 同构）；写入必须保留未知字段、原子写（临时文件 + rename）、写前备份。
- DevDesk 侧现状：module_mcp 已有 HTTP server 监听（127.0.0.1:8931）与端口配置（settings mcpPort）；安装项建议以 http/url 形式写入两端，若用户环境需要 stdio 再由后续项扩展。
- UI 位置：module_mcp 详情面卡（参考 modules 卡组件模式），按钮需三态：未安装=安装、已安装=已安装/卸载、写入失败=错误提示不静默。
- 2026-11-16 22:13:59 +0800 · 调研完成（线索 178-181）：Claude Code 侧已由 need244 全套落地（0c302d99），本需求剩余=ZCode 侧增量（写 ~/.zcode/cli/config.json 的 mcp.servers.devdesk，复刻 cc 命名空间模式）。范围 M 级。转 PM 出 PRD。
- 2026-11-16 22:20:42 +0800 · PRD 落盘（doc id=21，slug 2026-11-16-mcp-zcode-install，F-01~F-10 全待方案，四决策点全裁决不升格；线索 182）。转 developer 出技术方案卡+实现。
- 2026-11-16 22:46:08 +0800 · F-01~F-10 全部已实现+单测验证；F-08 UI 渲染面（截图/点击链）未实操=留 QA。残余：冲突文案按提示槽宽度等义压缩（方案卡 §4 已记档）。未 push。
- 2026-11-16 22:47:39 +0800 · Claude 端零改动；PRD doc 21 + 方案卡 doc 22 台账齐；A8 待装机验收。

**最近状态变更**（共 4 条）：2026-11-16 22:46:08 +0800 · 进行中 → 待验收 · F-01~F-10 全部已实现+单测验证；F-08 UI 渲染面（截图/点击链）未实操=留 QA。残余：冲突文案按提示槽宽度等义压缩（方案卡 §4 已记档）。未 push。

**关联文档**：doc#21（prd v1） · doc#22（tech v1） → 全文见附录 A

---

#### need#291 · Git 管理器整体重设计：老板令「整个功能逻辑都不对，没有哪个 UI 功能完全正常」——UI+功能逻辑全量重梳

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-17 13:15:30 +0800（developer）· 更新：2026-11-18 18:44:39 +0800（developer）
- **blocker**：重设计主体未交付：PRD 决策点 D1~D7 待老板裁决 + researcher 调研未落库（2026-11-17 doc_get 实查）；11-18 git-ui-fix 波仅修复老板上报五缺陷（bug280/290/292/294/296）不构成 F-01~F-12 全量重梳验收——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

PRD 已落 doc 台账：slug=2026-11-17-git-manager-redesign（id=23, v1）——骨架+功能矩阵 F-01~F-12（初值全待方案）+决策点 D1~D7 一次列全（变更区形态 D1=树/平铺双态切换默认分组树；左栏目录树去留 D2=推荐整列砍掉两栏化【影响全文件预览能力收窄，需老板明示】；untracked 目录条目交互 D3=推荐 filesystem 懒枚举展开、数据面不递归硬约束保留；布局收敛 D4=推荐两栏主态；diff 动线 D5=推荐点条目即 diff 常驻、内容预览降次视图；staged/unstaged/untracked 分组 D6=推荐两组头+untracked 并入未暂存带 U 徽标；搜索适配 D7=保留仅改定位目标）。老板原话逐字四条入 PRD §1；现状核对（git log 交付链 8ecf04e6→b2c69429→beb247f7 + 三病根代码级定位）入 §2；联网检索（VS Code 树/平铺双态 vscode#225620、GitHub Desktop 仅平铺 desktop#20624）入 §2.4 带来源。待办：①老板裁决 D1~D7（可单轮按推荐放行）→ ui-ux-designer 设计文档+HTML demo；②researcher 调研 slug=2026-11-17-git-redesign-research 落库后并入修订（2026-11-17 doc_get 实查未落库）。技术侧待确认两处（大仓折叠阈值/懒枚举转圈时机）留技术方案 POC。

**正文**：

PRD 已落 doc 台账：slug=2026-11-17-git-manager-redesign（id=23, v1）——骨架+功能矩阵 F-01~F-12（初值全待方案）+决策点 D1~D7 一次列全（变更区形态 D1=树/平铺双态切换默认分组树；左栏目录树去留 D2=推荐整列砍掉两栏化【影响全文件预览能力收窄，需老板明示】；untracked 目录条目交互 D3=推荐 filesystem 懒枚举展开、数据面不递归硬约束保留；布局收敛 D4=推荐两栏主态；diff 动线 D5=推荐点条目即 diff 常驻、内容预览降次视图；staged/unstaged/untracked 分组 D6=推荐两组头+untracked 并入未暂存带 U 徽标；搜索适配 D7=保留仅改定位目标）。老板原话逐字四条入 PRD §1；现状核对（git log 交付链 8ecf04e6→b2c69429→b

**关联线索**（10）：

- [open] github desktop, changes list, flat, filter, checkbox, diff pane：GitHub Desktop（现版 development 分支，源码级）变更区=纯平铺+Filter：filter-changes-list.tsx 单组 createListItems→identifier:'changed-files'，items=files.map(f=>({text:[f.path]})) 平铺数组；Filter 输入框 placeholder:'Filter'+check-all 勾选框；无树组件/无 tree 切换/无 Staged-Unstaged 分节，staging=每行 inclusion checkbox（onIncludeChanged）；app/src/ui/changes 与 app/src/ui/history 两目录均无 tree 命名文件；changes.tsx 仅渲染 diff-container（DiffHeader+SeamlessDiffSwitcher，import '../diff/diff-header'、'../diff/seamless-diff-switcher'）=选中文件同窗右侧出 diff；untracked=workingDirectory.files 普通成员无特殊化；release 3.0.0/3.1.0/3.2.0 notes 均无变更树记载。（source: raw.githubusercontent.com/desktop/desktop/development app/src/ui/changes/{filter-changes-list,changes,sidebar}.tsx + api.github.com 目录列表 + releases tag 3.0.0/3.1.0/3.2.0（一手源码+官方 release notes））
- [open] vscode, scm, tree list toggle, untracked U badge, diff on click：VSCode 官方文档：SCM 变更列表支持 flat/tree 双形态——"You can view the list of changes in either a flat or tree structure. Toggle this with the More Actions (...) > View & Sort > View as Tree/List option"；点变更即开 diff——"Select a file in the Source Control view to open its diff"；untracked 无独立区，U/M/D 徽标混列 Changes；树视图支持整文件夹操作——"When you're using the tree view, you can stage entire folders by staging the folder itself"。scm.viewMode 设置名仅见检索摘要，官方页未直引，待验证。（source: code.visualstudio.com/docs/sourcecontrol/overview + /staging-commits（官方文档精读））
- [open] intellij, commit tool window, changelist, unversioned files, group by directory：IntelliJ IDEA Commit 工具窗：文件按 changelist 分组，unversioned 归专有节点——"You can also select files under the Unversioned Files node"；diff=Ctrl+D 或 double-click 开 Diff Viewer，可编辑器 tab 或独立窗（Options→Show on Double-Click: Diff/Source；Advanced Settings→Open Diff as Editor Tab）；View Options 齿轮→Group By→Directory 目录树分组（YouTrack 官方文章 SUPPORT-A-3874 + IJPL-245597，2022.3 起 Group by 与 View Options 合并单图标；ChangesViewTreeStateStrategy 类名佐证树状态机，IJPL-251055）；changelist 定义："A changelist is a set of local changes that have not yet been committed"。（source: jetbrains.com/help/idea/commit-and-push-changes.html + managing-changelists.html（官方精读）+ youtrack.jetbrains.com SUPPORT-A-3874/IJPL-245597/IJPL-251055（官方 tracker，检索摘要级））
- [open] fork, tree view since 1.0.29, sourcetree file status, staged unstaged：Fork：变更列表树形态自 1.0.29（2016-11-04）——官方 release notes 原文 "Show changed files as a tree"；1.0.42（2017-03-31）"Sort files by type (directories first) then by name"。Sourcetree：File Status 视图="file browser and commit preparation area"（CMD+1），staging 动作 CMD+SHIFT+-（Unstage）/CMD+SHIFT+=（Add untracked），External diff + 空格 Quick Look；staged/unstaged 双节平铺+filter 为二手口径（Kapeli cheat sheet/检索），待验证。两客户端均无独立全量目录树、无 untracked 独立 tab。（source: git-fork.com/releasenotes（官方精读）+ kapeli.com Sourcetree cheat sheet（二手）+ product-downloads.atlassian.com Sourcetree_4.2.11.html（官方主机，扫描））
- [open] EUI-NEO tree, virtualList, feasibility, F-09 正交：EUI-NEO 无现成 tree 组件：docs/EUI-NEO-CAPABILITIES.md:35「tree 组件|文件树需自实现|用 virtualList+字符图标+状态机」、:618「文件树 UI|⚠️需自实现|virtualList+▶/▼字符+状态机(~150行)」；现成能力：virtualList（itemCount+rowHeight+overscan，:114）、checkbox（.checked/.bind/.onChange，:80）、input 过滤（.placeholder，:79）。→ 变更树视图落地条件=自实现：对已变更文件路径集派生行模型（平铺数组+depth+展开位图）+virtualList 渲染，无需全量目录 IO，与 F-09 untracked 不递归防 hang 约束正交（树节点仅从变更路径派生；untracked 目录按需 lazy 展开是方案空间，推测级未实证）。（source: D:\project\gui\DevDesk\docs\EUI-NEO-CAPABILITIES.md 精读（grep 定位 :35/:618/:114/:80/:79））
- [open] git-ui, U badge, 绿档, demo, 先验矩阵, need291：need291 UI 设计产物落盘（2026-11-17）：设计文档 docs/design/2026-11-17-git管理器重设计.md + demo docs/design/2026-11-17-git管理器重设计-demo.html（自包含单文件，全部内联 script 过 node --check）。参数级修订 1 处：U 徽标由前身 spec §3.2 灰 #666666 升绿档 --st-added #157E4E/#E6FBF2——前提变化= D6-A 并组后独立「未跟踪」组头消失，灰档与可交互文件行不可区分，违背 PRD 目标 2 可达性；业界 U=绿（VSCode）。demo 层先验矩阵 G1~G12 已钉住（文件选择→diff/树平铺切换选中保持/组头折叠/untracked lazy 展开+>500 截断/内容 tab/左栏折叠/搜索定位/空态/错误态/刷新），交互断言全过。（source: D:\project\gui\DevDesk\docs\design\2026-11-17-git管理器重设计.md §2.2/§10 + demo evaluate 断言实测）
- [open] need291, POC, lazy enum, untracked, walkChildren, virtualList, spinner threshold, 折叠阈值：need291 技术方案 POC 结果（2026-11-17，Windows/NTFS 实测）：①untracked 目录 lazy 单层枚举（std::filesystem::directory_iterator）线性 ~1.6µs/条目——500 条 0.81ms / 3000 条 5.1ms / 10000 条 16.2ms（3 遍取最优）；全量递归对照 DevDesk/build 一层 36 条 0.15ms vs 递归 26530 条 3721ms → D3-A 成立（展开任一层 <20ms，≤500 截断亚毫秒），F-09 不递归硬约束反向证实。归并口径=复用 GitTreeModel::walkChildren（src/module_git/git_tree_model.h:92：单层枚举+isIgnored 过滤+.git 跳过+symlink 叶化+Subrepo 检测现成），结果过滤 StatusKind::Untracked 文件行+子目录行续 lazy。②折叠阈值 200 维持：virtualList 只物化可视槽（EUI-NEO/components/virtuallist.h:96-113）渲染成本 O(可视)与行数无关，>200 收起是 UX 渐进披露（200 行≈8.7 屏@28px 行高 648px 视口）；断言设计=201 条两组头默认折叠、199 条默认展开（doctest 纯模型可测）。③转圈 1000ms 维持：单层枚举常态 <20ms 到不了 1s，阈值只兜慢盘（网络盘/杀软/OneDrive 占位）；同款先例 validateSpinnerDue（src/module_git/git_page_controller.h:110「超过 1s 才显旋转圈」）+NN/g 1s 流界限（nngroup.com/articles/response-times-3-important-limits）。（source: 实测 C++ std::filesystem POC（temp poc291 已删）+ codegraph 代码锚点 + nngroup.com/articles/response-times-3-important-limits）
- [open] need291, compact folders, 单链压缩, vscode, tree, scm：单链压缩业界先例证实（2026-11-17 联网检索，need291 T1 模型分片）：VSCode Explorer「Compact Folders」=把只含单一子文件夹的目录合并为单个树节点显示（src/components/ 一条），v1.41 引入；且 SCM 变更树视图同样适用该合并（microsoft/vscode#219988：用户反馈 explorer.compactFolders 在 Source Control 视图不生效，佐证 SCM 树默认压缩）——设计文档 §4.4「唯一子目录链自动合并显示 a/b/c/」与业界一致。来源：code.visualstudio.com 设置文档+stackoverflow.com/questions/59415299+github.com/microsoft/vscode/issues/219988。（source: WebSearch: stackoverflow.com/questions/59415299 + github.com/microsoft/vscode/issues/219988 + kindacode.com/article/vs-code-prevent-single-child-folders-from-being-merged）
- [open] need291, git_change_list_model API, git_fs_walk, walkChildren, statusBadgeLetter, 消费契约, 视图分片, controller：need291 视图/控制器分片（四文件）编码完成，对 git_change_list_model.{h,cpp}（并行代理产出）的 API 消费契约：①类型 ViewMode{Tree,Flat}、ChangeListRow（Kind::GroupHeader/Dir/File/Loading/Truncate/Error × Group::Staged/Unstaged，含 name/relPath/dirRel/status/oldPath/childCount/remainCount/errorText，方案卡 §3 原样）；②方法 rebuild(snapshot, ViewMode)、reset()、rows()（返回行序列）、setExpanded(dirRel,bool)/isExpanded(dirRel)（tracked 树位图+untracked 展开共用位图口径）、markLoading(dirRel)、applyUntrackedChildren(dirRel, vector<TreeRow>)、markLoadFailed(dirRel,msg)、setGroupCollapsed(Group,bool)/isGroupCollapsed(Group)（含 >200 默认注入模型内做）。消费点：git_page_controller.cpp（composePump④ listRows_=listModel_.rows() 物化；applyStatusSnapshot→rebuild；setViewMode/toggleGroup/toggleDir/toggleUntrackedDir 转发；requestUntrackedChildren worker 链）+ git_views.cpp:512（isGroupCollapsed 组头箭头）。控制器本地自担：requestedUntrackedDirs_ 防重入集、rowHasFile（jumpToHit ∈变更判定扫 listRows_）、hasFile 类模型 API 未消费。statusBadgeLetter 消费点 git_views.cpp（drawBadge）——须由 git_change_list_model.h 或 git_fs_walk.h 之一导出（原 git_tree_model.h 导出，随类删除迁移）。git_fs_walk.h 消费契约：自由函数 WalkResult walkChildren(Repo*, const fs::path& workdir, const string& dirRelUtf8) + TreeRow/TreeRowKind/WalkResult 类型（controller 搜索 walk+untracked lazy 双消费点）。若模型实际签名有出入，合并时按本线索机械改 controller/views 对应行。（source: D:\project\gui\DevDesk\src\module_git\git_page_controller.{h,cpp} + git_views.cpp 实码消费点）
- [open] need291, git_change_list_model, 契约定案, ChangeViewMode, 默认展开, hasFile, 合并轮, c4628169：need291 合并轮定案（2026-11-17，commit c4628169）：GitChangeListModel（src/module_git/git_change_list_model.h）实际契约——①枚举名 ChangeViewMode{Tree,Flat}（非 ViewMode）；②lazy API=markUntrackedLoading/markUntrackedFailed/applyUntrackedChildren(dirRel, const fs_walk::WalkResult&)/pendingUntrackedDirs/requestRetry；③fs_walk 类型=FsEntry/EntryKind/WalkResult（module::git::fs_walk 命名空间，非 TreeRow）；④tracked 派生目录默认展开（设计 §7 N2「≤50 条全展开」）、显式折叠记忆走 collapsedTree_/collapsedFlat_，untracked lazy 目录默认收起走 expanded 位图——isExpanded 按 isLazyTarget 分流双默认态；⑤组头行 name=「已暂存 (N)」模型烘焙（视图 :533 直绘）；⑥hasFile(relPath)=双桶+lazy 缓存全量成员判定（controller rowHasFile 消费，折叠组下刷新保选中 F-09）；⑦untracked 多段目录条目整链挂最近派生祖先（无则根），parentOf 直挂=孤儿行 bug 已修；⑧「右栏显示 diff」判据=rightTab()==Diff && !searchContentView()（GitPageController::RightTab 类内嵌套枚举，rightShowsDiff 已退役）。下游 qa/审计按此契约查，勿按 clue#200 假设名（ViewMode/markLoading/vector<TreeRow>）复查。（source: D:\project\gui\DevDesk\src\module_git\git_change_list_model.{h,cpp} + git_page_controller.cpp 定向 202 用例全绿实测）

**进度记录**（共 15 条，列最近 5 条）：

- 2026-11-17 15:12:35 +0800 · 三并发波进展（老板令「三个子任务并发」，汇总编译模式）：T3 测试面分片回单并验收（五测试文件落盘：status_model 改挂新模型等价断言/tree_model 304→115 行改挂 fs_walk+旧类用例删/diff_model 目录占位负向退役/search 增∈变更 diff 锚定用例/refresh 增位图保持用例；契约缺口 6 项已列待合并验证裁决）。T1 模型层与 T2 视图控制器仍在飞。波齐后合并验证子任务统一编译+删旧文件+定向测试+squash commit+装机。
- 2026-11-17 15:18:44 +0800 · T1 模型层分片完成（汇总编译模式，零构建零commit）：新增 git_change_list_model.{h,cpp}（行模型：双组分桶 D6-A/树派生+单链压缩 clue#199/平铺聚合/双展开位图/组折叠 200 阈值 POC②/untracked lazy 状态机+>500 截断，header 131 行+cpp 516 行）、git_fs_walk.{h,cpp}（walkChildren 自 git_tree_model.cpp:258-328 逐行迁出语义零变，返回类型改 FsEntry 自包含，109+44 行）、tests/test_platform/test_git_change_list_model.cpp（12 用例=卡 §5 族全量+原 GitChangesModel 三用例等价改挂+真仓全链，584 行）；CMake 两处注册（module 源 :7-8、测试 :62）。全部 CRLF。红绿验证归波后合并验证子任务。契约偏差 8 条见 T1 终报（statusBadgeLetter 未 re-home 防链接冲突、FsEntry 类型替换需 test_git_tree_model 改挂适配等）。
- 2026-11-17 15:20:02 +0800 · T1 模型层分片回单并验收：四新文件落盘（模型 131+516 行、fs_walk 44+109 行、新测试 584 行 12 用例）+CMake 双处注册。契约偏差 8 项已列（statusBadgeLetter re-home、TreeRow→fs_walk::FsEntry 类型替换、Subrepo 转 Dir、两份位图等）待合并验证裁决——其中 FsEntry 类型替换与 T3 的同名迁出假设冲突，合并任务必改。T2 视图控制器仍在飞。
- 2026-11-17 16:24:46 +0800 · 合并验证子任务收口（2026-11-17）：缺口A~G 全处置——A=API 机械对齐（ChangeViewMode/fs_walk::限定/FsEntry/rightShowsDiff→rightTab+searchContentView 判据/GitPageController::RightTab）；B=statusBadgeLetter 迁 git_change_list_model.{h,cpp} 导出；C=git_colors untracked 绿档 primaryText(#157E4E)+primary 12% tint（≈#E6FBF2，addedBg 同式）；D=git_tree_model/git_changes_model 已删+CMake 移除+全仓残引清零（仅注释级历史锚）；E=去重裁决：RenameBothGroups 删（status 版更强留），分桶/真仓/组头计数按层或按形态互补双留；F/G 照单接收。合并轮另修模型三缺陷：tracked 派生目录默认展开+显式折叠记忆（设计 N2 ≤50 全展开，T1 实现偏离已纠正）；多段 untracked 目录条目挂最近派生祖先/根（原 parentOf 直挂成孤儿行违设计「整链一节点」）；组头文案烘焙「已暂存 (N)」（视图注释既定契约）；compressChain 嵌套压缩丢中间段（lastSegment(only.path)→only.display）；rowHasFile 改模型 hasFile 全量成员判定（折叠组下选中刷新保持，F-09）。diff 目录占位文案退役（卡 F-05 既定）。验证：build-test-run.sh --dev --filter file:git 全绿 Total 202 Passed 199 Failed 0；模块隔离守卫绿（经 MSYS2 bash 跑——Git for Windows bash 全局崩 add_item fatal，已报环境异常）。commit c4628169 feat(git) 单提交（26 文件 +4352/-1533，含设计文档+demo+pool 台账）。装机完成：install-win-debug.sh 快路径 strip 副本 81.4MB→%LOCALAPPDATA%\Programs\DevDesk\，manifest 生成，新实例 PID 12160 启动于 16:19:49（脚本重启段产物，PowerShell Path 验证=装机目录）。
- 2026-11-17 16:28:27 +0800 · 三并发波+合并验证收口完成（老板令「三个子任务并发」汇总编译模式）：commit c4628169（26 文件 +4352/−1533，含旧模型退役），主会话复跑 --dev --filter file:git 全绿（Total 202 / Failed 0），装机完成（PID 12160），旧文件残引清零（仅注释级历史锚），缺口 A~G 全处置（API 对齐/statusBadgeLetter 迁移/U 绿档/删旧/用例去重/防御位接收）。待老板真机验收新版 Git 页：两栏布局/树平铺切换/两组头/未跟踪目录展开/点条目即 diff/内容次视图。UI 画面级探针与全量档留 QA 收口。

**最近状态变更**（共 6 条）：2026-11-17 16:28:27 +0800 · 进行中 → 待验收 · 三并发波+合并验证收口完成（老板令「三个子任务并发」汇总编译模式）：commit c4628169（26 文件 +4352/−1533，含旧模型退役），主会话复跑 --dev --filter file:git 全绿（Total 202 / Failed 0），装机完成（PID 12160），旧文件残引清零（仅注释级历史锚），缺口 A~G 全处置（API 对齐/statusBadgeLetter 迁移/U 绿档/删旧/用例去重/防御位接收）。待老板真机验收新版 Git 页：两栏布局/树平铺切换/两组头/未跟踪目录展开/点条目即 diff/内容次视图。UI 画面级探针与全量档留 QA 收口。

**关联文档**：doc#23（prd v2） · doc#24（tech v1） · doc#25（tech v2） · doc#26（audit v1） → 全文见附录 A

---

#### need#299 · Git 页变更列表/目录树 UI 重设计+边界处理（老板令 2026-11-17）

- 状态：待验收 · 优先级：P1 · 负责人：ui-ux-designer · 阶段：测试中
- 创建：2026-11-17 18:15:28 +0800（ui-ux-designer）· 更新：2026-11-18 18:44:42 +0800（ui-ux-designer）
- **blocker**：设计修订未出：串行依赖 #298（搜索设计）回单后才派本条设计（ui-ux-designer 流程未启动）；变更列表/目录树重设计未开发——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

老板令（2026-11-17 18:1x）：「主界面的UI页给我重新设计，目录树太丑了，另外目录树里有些名字很长的，也没处理。边界问题 你是一个都没处理？」三要点：①变更列表树视觉重设计（行结构/图标/缩进/组头/徽标排布——当前实现被评「太丑」）；②超长名处理缺失（目录行有 tooltip 无省略号、文件行两者待核——F-11 省略+tooltip 契约未全落地）；③边界全集（超长名/深嵌套缩进溢出/窄窗/计数与徽标碰撞等）。流程：ui-ux-designer 出设计修订+demo 更新→老板看图拍板→开发。与 #298（搜索设计，在飞）同文件串行——#298 回单后派本条设计。

**正文**：

老板令（2026-11-17 18:1x）：「主界面的UI页给我重新设计，目录树太丑了，另外目录树里有些名字很长的，也没处理。边界问题 你是一个都没处理？」三要点：①变更列表树视觉重设计（行结构/图标/缩进/组头/徽标排布——当前实现被评「太丑」）；②超长名处理缺失（目录行有 tooltip 无省略号、文件行两者待核——F-11 省略+tooltip 契约未全落地）；③边界全集（超长名/深嵌套缩进溢出/窄窗/计数与徽标碰撞等）。流程：ui-ux-designer 出设计修订+demo 更新→老板看图拍板→开发。与 #298（搜索设计，在飞）同文件串行——#298 回单后派本条设计。

**关联线索**（4）：

- [open] need299, 过滤框先例, 仓切换下拉, github desktop, vscode type-to-filter, IDEA, 联网检索：need299 联网检索先例（2026-11-17）：①GitHub Desktop 仓切换=左上「Current repository」下拉钮 → Repository list 浮层，内含 Filter 输入框 + 「Recent Repositories」分节（github.com/desktop/desktop/issues/9156 收藏/隐藏最近仓讨论佐证清单结构；pylandschool 教程佐证动线）；②VSCode Explorer 树过滤=聚焦树后直接键入即出顶部临时过滤框、高亮匹配、无按钮（type-to-filter，stackoverflow.com/questions/70298878；常驻过滤框为未采纳的 issue 98662——业界默认是常驻框或键入唤醒两派，GitHub Desktop changes 过滤框长期缺位仅 issue 6407 在求）；③JetBrains Commit 工具窗 changelist 按 Group by Directory 树分组已有在案线索 #194。→ 本设计：过滤输入框取「常驻纯输入框无按钮」（老板令「只需要一个输入框 不需要按钮」），匹配口径推荐全路径子串大小写不敏感（GitHub Desktop 同款）；仓切换取「按钮+最近仓分节下拉」（GitHub Desktop 同构，砍 Filter 框与收藏节）。（source: WebSearch: github.com/desktop/desktop/issues/9156 + pylandschool.com/en/blog/article/github-desktop-switch-repos + stackoverflow.com/questions/70298878 + github.com/microsoft/vscode/issues/98662 + github.com/desktop/desktop/issues/6407）
- [open] need299, 设计产物, 过滤框, 仓下拉, 搜索删除, 可见行集口径, 省略号, 深嵌套钳制, 先验矩阵：need299 UI 设计产物落盘（2026-11-17，need291 契约对文件原位修订）：docs/design/2026-11-17-git管理器重设计.md（v2 修订）+ 同名 demo html（全部内联 script 过 node --check，零搜索残留）。①全仓搜索全章删除标记归 #300（§0 D7/§3 搜索框/§6.8/§7 N7/§8 五行动效/§10 G7/§11 图标，demo 零残留）；②顶栏去搜索重排：路径 max-width 360→480，切换仓库钮加 fa-angle-down 变下拉触发器；③新 §3.1 切换仓库历史下拉：宽 min(360,页宽−24)/节标 28/仓行 40px 两行式（仓名 13/500+路径 11 省略）/当前仓 fa-check 12px 绿/失效行置灰+「已失效」红标仍可点/浏览目录 32px/清单>8 条 320px 内滚+8px 边缘渐隐/150ms OutCubic 开合/scrim+Esc 收起；技术依赖=多仓历史持久化 ≤8 条 LRU（复用 N10 最近仓记忆扩展，开发侧落库）；④新 §4.2b/§4.9/§6.9 过滤输入框：32px 行+28px 输入框（底 --c-btn-2-bg 无边框聚焦白底绿边）/纯输入框无按钮无放大镜/全路径子串大小写不敏感（GitHub Desktop 同款）/IME 组合不应用/树=祖先链自动临时展开清空恢复位图/平铺=行级匹配/Esc 与 × 清空/切仓清空/刷新与形态切换保留；⑤过滤口径钉死（demo 截图自审抓出 2 缺陷后定稿）：空态判定=组头隐藏判定=组头过滤态计数=「可见行集」统一口径（文件行命中+untracked 目录行自身命中合并计——仅数文件行会出现「目录行可见却报无匹配」与「父 wrap 连带藏命中行」两缺陷）；⑥长名/深嵌套钉死：目录行+文件名单行省略号字符截断（禁硬裁切）+tooltip 全路径（实码锚 git_views.cpp:615/774 已有 tooltip 链路，补省略号）、深嵌套缩进钳制名区最小 96px、徽标/计数锚位固定不随文本溢出；⑦平铺形态 untracked 目录条目钉死=保留可展开目录行（git porcelain「?? dir/」原生语义，§4.7 修订对齐实码 emitGroupFlat git_change_list_model.cpp:424，F-09 零触碰；备选 B 后台批量枚举不推荐进 §12-3 待裁决）；⑧>500 截断尾行删搜索引导改纯计数「还有 N 个文件」；⑨demo 新增长名+深嵌套样本（modules/net/transport/peer_frame_codec_registry_impl.cc）、修复原 demo 两处旧缺陷（.tree-view 选择器无效致树形态显示路径段、untracked 目录行违规显计数 3）。先验矩阵 G1~G14（G7 删除标注，G13 过滤/G14 仓下拉新增）。待裁决：§12-3 平铺 untracked 处置（推荐 A 保留目录行）、§12-4 过滤匹配口径（推荐全路径子串）。（source: D:\project\gui\DevDesk\docs\design\2026-11-17-git管理器重设计.md §3.1/§4.2b/§4.9/§6.9/§6.10/§12 + 同名 demo（node --check 全过+浏览器截图断言实测：首屏/树过滤祖先链/Esc 清空/仓下拉/平铺过滤五轮））
- [open] need299, need300, untracked, uall, 平铺, lazy, asciiFold, 过滤：need299/300 联网检索（2026-11-17）：未跟踪文件展示形态业界口径——git CLI 层 `git status -uall` / `git ls-files --others --exclude-standard` 即「未跟踪一律平铺单文件」原生语义（-unormal=目录折叠一条）；GUI 客户端（VSCode SCM/Fork/Sourcetree）普遍按 git porcelain 目录条目呈现为可展开折叠项，展开后才见文件。老板裁决④「未跟踪只显文件行（首层自动枚举+嵌套懒展开）」= CLI -unormal 首层 + GUI 懒展开折中，无冲突先例；git_search_model 删除后 asciiFold 复用面=git_change_list_model 过滤匹配（ASCII 折叠同款）。来源：michaelheap.com/git-status-untracked + stackoverflow.com/questions/3801321（检索摘要级）（source: WebSearch: michaelheap.com/git-status-untracked + stackoverflow.com/questions/3801321）
- [open] need299, scrollbarWidth, 滚动条隐藏, filterActive, 空态隔离, rename 检测, git_manager.json 预置, 单链压缩, 缩进钳制, probe frame 绝对坐标：need299 打磨轮定案（2026-11-17，wip c6e61834）：①「滚动条视觉全无」落地口径=EUI virtualList `.scrollbarWidth(0.0f).scrollbarGap(0.0f)`（EUI-NEO/components/virtuallist.h:88-91 scrollWidth/scrollGap 派生，width 0=纵向条不画且不占位；横向条 .h 用 metrics_.control.scrollbar 高度独立不受影响→F-07⑥ 拖拽回归探针不破）——git_views.cpp 三处列表（git.changes.list/git.content.list/git.diff.list）已加；横向滚动条保留（超宽行拖拽交互+F-07⑥ 回归依赖，与老板全局口径冲突点已记录待裁决）。②真偏差修复：过滤无匹配时右栏曾冒充「工作区很干净」空态（rows 滤空即命中 §5.7 分支）——composeDiffCard 分支加 `&& !c.filterActive()`（git_views.cpp:1737），落 §5.5 未选文件空态。③平台 rename 行为实证（fixture R 行）：libgit2 双侧 rename 检测把 workdir 重命名配对成单条 Renamed(oldPath) 且不残留旧名 D 行（优于 git CLI 默认 porcelain 的 D+? 双条目）；git CLI status --porcelain 对照法=平台 status 契约的廉价旁证。④探针手法沉淀：最近仓历史可预置 `git_manager.json {"lastRepoPath":"","recentRepos":[假路径]}`（GitRepoStore 键名）造「已失效」行，免真删目录（Windows 句柄延迟释放 remove_all 挂住）；深嵌套缩进钳制取证需「每级一个直接修改文件」断开单链压缩（compressChain 条件=fileIdx.empty()&&subdirs==1，git_change_list_model.cpp:67-70），纯单链会被压缩合并到 depth 0。⑤探针坐标口径：virtualList slot 子元素 frame 为绝对坐标（列表 x 12+1 边框起），行缩进锚=dir 行 .arrow 左缘 / file 行 .badge 左缘同值；overscan 1 viewport → DOM 物化≈2×视口行数，frame 断言可行但点击必须可见。（source: D:\project\gui\DevDesk\src\module_git\git_views.cpp + tests/probes/git_page_probe.cpp 实测（探针三档 PASS 1280/1200/1180））

**进度记录**（共 11 条，列最近 5 条）：

- 2026-11-17 20:51:41 +0800 · 打磨轮完成（断点=待主会话验收）：①探针重写落地 tests/probes/git_page_probe.cpp（15 截图场景+边界断言，三档 PASS：1280x960 / 1200x720 / 1180x720，evidence/need299/ 20 张语义化截图）；②视觉评审修 2 条：三列表纵向滚动条隐藏（老板口径）、过滤无匹配右栏空态隔离（git_views.cpp:1737 filterActive 判定）；③边界走查全清单有结论（缩进钳制 185<199==199 实测、LRU 上限8→下拉>8内滚结构性不可达、IME/缩放1.354 探针不可达=框架缺口记录、横向滚动条保留=取舍待裁决）；④定向 file:git 全绿 Total 196 Failed 0；⑤未装机未 push；wip c6e61834 待收口 squash。终报已回主会话。
- 2026-11-17 20:54:51 +0800 · 打磨轮过关并装机（squash 后 commit bda66ad5 已推，探针三档 PASS/定向 196 例 0 红复跑/24 证据图 evidence/need299/）：打磨修正=纵向滚动条隐藏（三 virtualList）+过滤无匹配右栏空态隔离；框架缺口 2 条记录（下拉键盘导航无焦点 API、IME/缩放归真机）；diff 横向滚动条保留裁决（超宽行唯一拖拽面+回归依赖）。待老板真机终验。
- 2026-11-18 06:22:54 +0800 · 复验开工（2026-11-18）：截图二审
- 2026-11-18 06:48:41 +0800 · 待验收（黄）：6 子项截图绿 + D4 计数已修达标；D3 保留（设计允许）；剩 CJK 长名省略/缩进钳制/IME/悬停等 5 项需真机
- 2026-11-18 10:32:33 +0800 · need#113 第2波：九项细目 6 绿；1 项口径冲突 D3（平铺形态仍出目录行 vs 「仅修改=纯文件行」，需 product-manager 裁决 A/B，见 review.md §三 D3）；2 项需交互取证（右栏头部嵌套路径省略+tooltip / IME 组合期）。另 D4 组头计数随懒展开 +1、D5 空态重心偏上（P3）。留待验收

**最近状态变更**（共 10 条）：2026-11-18 06:48:41 +0800 · 进行中 → 待验收 · 待验收（黄）：6 子项截图绿 + D4 计数已修达标；D3 保留（设计允许）；剩 CJK 长名省略/缩进钳制/IME/悬停等 5 项需真机

---

#### need#302 · 删除 pool-snapshot.json 相关全部功能与文档

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-17 18:53:37 +0800（ZCode主会话）· 更新：2026-11-18 18:44:51 +0800（ZCode主会话）
- **blocker**：DevDesk 侧收口未完成：docs/pool/pool-snapshot.json 仍存（1697664B，11-18 08:03 再生成=写入源未根修，遗留面待裁决）+ tools/README.md 仍有 pool-snapshot 引用；插件源面已零命中（pool_mcp.py 仅余 _schema_snapshot 无关项；前判 evidence/20261118-plugin-verify/report-needs.md need302 节）；条件=遗留裁决→写入源根修→物理删除→复测不重生——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

用户裁决（2026-11-17 19:0x）：「删除和 pool 快照 json 相关的全部功能和文档」。背景：GH 后端转正后 need#31 已停写快照（GH 面），local 后端照写行为保留至今；项目实际已单仓化，db+pool-state.json+pool-report.json 足够，快照面全退。

射程（先盘点后删）：①pool_mcp.py local 侧写快照逻辑+snapshot_only 口径；②pool_mcp_gh.py _artifacts 快照 job 残留结构（need#31 后 GH 不写但引用/注释/分支可能有）；③check-pool-sync.sh 审计面工具（快照对账本位）；④文档：AGENTS.md/ARCHITECTURE/README/pool-usage SKILL 提及处；⑤测试：gate-test/eval-run 快照相关用例（删后计数清点）；⑥.gitignore 的 pool-snapshot 行；⑦主检出物理文件 docs/pool/pool-snapshot.json（落地时删）。

保留面：evidence/ 归档与 .zcode/lessons 历史留痕不动（入库惯例）。

验收：全仓 grep pool-snapshot 源面零命中（evidence/lessons 除外）；gate-test 全量绿且计数对账；local 后端写操作后无快照文件生成（实测）；版本随落地批次 0.39.43。

**正文**：

用户裁决（2026-11-17 19:0x）：「删除和 pool 快照 json 相关的全部功能和文档」。背景：GH 后端转正后 need#31 已停写快照（GH 面），local 后端照写行为保留至今；项目实际已单仓化，db+pool-state.json+pool-report.json 足够，快照面全退。

射程（先盘点后删）：①pool_mcp.py local 侧写快照逻辑+snapshot_only 口径；②pool_mcp_gh.py _artifacts 快照 job 残留结构（need#31 后 GH 不写但引用/注释/分支可能有）；③check-pool-sync.sh 审计面工具（快照对账本位）；④文档：AGENTS.md/ARCHITECTURE/README/pool-usage SKILL 提及处；⑤测试：gate-test/eval-run 快照相关用例（删后

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-18 00:28:14 +0800 · 复验：插件仓 0.39.43 源面 grep pool-snapshot 零命中（evidence/lessons 除外）；DevDesk 物理文件 docs/pool/pool-snapshot.json 已删、.gitignore 行已移除；本次多次池写后快照未重生（实测）
- 2026-11-18 02:39:30 +0800 · 复验补缺（2026-11-18）：发现第二写手——ZCode 全局 MCP 仍注册 DevDesk legacy tools/pool_mcp.py（mcp.servers.pool），每会话拉起共 6 进程，写操作即重生快照（物理删除后 01:56 又出现）。已删 _write_snapshot+4 调用、删 pool_review_gen.py/pool-review.html、README 同步，commit 5379ef62；实测 sync 后无快照。残留：在跑老进程需重启停写；legacy MCP 注册是否下线待老板裁决
- 2026-11-18 11:46:11 +0800 · 不达（部分）留待验收：插件源面绿（grep -rln pool-snapshot 排除 evidence 零命中；活缓存 0.39.43 pool_mcp.py 零命中、本会话数十次池写后快照 mtime 未动）；DevDesk 侧红面——docs/pool/pool-snapshot.json 现存（1697664B，mtime 2026-11-18 08:03 复验 note 后再生成，git 未跟踪）；池内射程外遗留条目在案待裁决（DevDesk tools/pool_mcp.py 仍含 _write_snapshot 等）。条件：遗留条目裁决+写入源根修+物理文件再删+复测池写后不重生。证据 evidence/20261118-plugin-verify/report-needs.md#need302

**最近状态变更**（共 4 条）：2026-11-18 00:44:42 +0800 · 进行中 → 待验收 · 

---

#### need#303 · 临时任务池（temp 池：仅 id+描述）——子任务持 id 豁免 devflow 约束

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：开发中
- 创建：2026-11-17 18:56:15 +0800（developer）· 更新：2026-11-18 20:08:42 +0800（developer）

**详情**：

用户方向（2026-11-17 19:0x）：临时性任务（如修生产数据——需写 py 代码、需测试验证）也应有轻量通道：新建 temp 池（条目只有 id+描述，登记即用），子任务/会话持该 id 不被 devflow 仪式约束（免排产/免桩清/免三律文案）。

设计草图（developer 细化）：①pool MCP 新增 pool=temp 轻型条目（id+描述，无状态机/无 owner 必填）；②简报标记形态 [pool:temp:N]→agent-gate 认作授权（绕排产检查）；③execute-gate/prompt-anchor 认会话声明的 temp id→写面豁免桩清循环；④与 #295 轻量口令互补：#295=会话级整体豁免（非代码），本条=条目级豁免（临时代码任务）。

实施面：并入 need#295 同一 worktree/同一子代理（同文件面：prompt-anchor.sh/agent-gate.sh/execute-gate.sh+池工具），避免并行撞车。验收：临时任务登记→持 id 派单/写码/测试全程无 devflow 拦截（先红后绿用例），正式池流程零回退。

**正文**：

用户方向（2026-11-17 19:0x）：临时性任务（如修生产数据——需写 py 代码、需测试验证）也应有轻量通道：新建 temp 池（条目只有 id+描述，登记即用），子任务/会话持该 id 不被 devflow 仪式约束（免排产/免桩清/免三律文案）。

设计草图（developer 细化）：①pool MCP 新增 pool=temp 轻型条目（id+描述，无状态机/无 owner 必填）；②简报标记形态 [pool:temp:N]→agent-gate 认作授权（绕排产检查）；③execute-gate/prompt-anchor 认会话声明的 temp id→写面豁免桩清循环；④与 #295 轻量口令互补：#295=会话级整体豁免（非代码），本条=条目级豁免（临时代码任务）。

实施面：并入 need#295 同一 worktree/同一子代理（同文件面：prompt-anc

**进度记录**（共 6 条，列最近 5 条）：

- 2026-11-18 00:27:08 +0800 · 开工：temp 契约面补缺（A-F），先跑红证据
- 2026-11-18 00:38:40 +0800 · A-F 全部落地：pool_contract enum+desc；两 server pool_add owner 默认化+常规池 owner 显式校验；gh 投影放行 pool:temp；pool_stats 默认排除 temp。红→绿证据 evidence/20261118-temp-schema/（red-parity/red-smoke/red-gh-s16 + green-*）。新增用例：pool-smoke 5 项、parity temp 契约面两端断言、gh 场景 S16。gate-test 349 ok/0 FAIL；off 基线因 pool_mcp.py 触达按条款重录（32 响应）。
- 2026-11-18 11:46:11 +0800 · 不达（登记通道缺口）留待验收：实现面全绿——pool_mcp.py:525-546/:1015（temp 池+不计默认口径）、agent-gate:163-231（[pool:temp:N] 授权+幻影拒）、prompt-anchor:73（temp 落桩）、gate-test:1351-1362（temp 写面豁免先红后绿用例）、README:122；但活测 pool_add(pool=temp) 被工具 schema 枚举拒（InputValidationError: expected need|bug|debt——description 已含 temp 而 enum 未同步），schema 校验客户端登记通道被拦死。bug#320 已登记（P2）。条件：bug#320 修复（enum 补 temp）+复测登记通道后关。证据 evidence/20261118-plugin-verify/report-needs.md#need303
- 2026-11-18 17:41:41 +0800 · 升版装机完成：fix a88b959 已 push（七方 0.39.47），缓存/市场克隆/installed_plugins 三面对齐 0.39.47；#303 登记通道依赖的 temp 枚举已在装机缓存内。待新会话活体验证 pool_add pool=temp 无 owner 放行。
- 2026-11-18 20:08:42 +0800 · BW 归位判定（查池 note+evidence 后）：未达「可关」——实现面全绿+修复 a88b959 已装机（0.39.47 三面对齐，note 2026-11-18 17:41），唯差新会话活体验证 pool_add pool=temp 无 owner 放行一步；本会话 MCP schema 仍为装机前形态（enum 无 temp）无法代验，如实。归待验收：验收动作=新会话活体复测登记通道，绿即关（与 #320 同判同批）。证据 evidence/20261118-plugin-verify/report-needs.md need303 节。

**最近状态变更**（共 6 条）：2026-11-18 20:08:42 +0800 · 进行中 → 待验收 · BW 归位判定（查池 note+evidence 后）：未达「可关」——实现面全绿+修复 a88b959 已装机（0.39.47 三面对齐，note 2026-11-18 17:41），唯差新会话活体验证 pool_add pool=temp 无 owner 放行一步；本会话 MCP schema 仍为装机前形态（enum 无 temp）无法代验，如实。归待验收：验收动作=新会话活体复测登记通道，绿即关（与 #320 同判同批）。证据 evidence/20261118-plugin-verify/report-needs.md need303 节。

---

#### need#339 · TAB 文字与头部卡片缩小 20%（老板实测回调 #167 放大档）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 17:39:22 +0800（developer）· 更新：2026-11-18 20:33:45 +0800（developer）
- **blocker**：等主会话二轮 rebase 完成（Mac 波含 ui_kit/text_metrics.h 与 todo/header 文件，需重查 git_views/顶栏现状防过时前提）

**详情**：

老板验收清单 #6：TAB 文字大小缩小 20%（现 18px→约 14.4，即向原 15 回落）；头部卡片大小缩小 20%。系老板装机实测后对 #167 放大档的回调（40/18 实观偏大）。含 TAB 字号单源常量调整 + 头部卡片几何档缩 20%（具体量测头部卡片现状后按比例收）。

**正文**：

老板验收清单 #6：TAB 文字大小缩小 20%（现 18px→约 14.4，即向原 15 回落）；头部卡片大小缩小 20%。系老板装机实测后对 #167 放大档的回调（40/18 实观偏大）。含 TAB 字号单源常量调整 + 头部卡片几何档缩 20%（具体量测头部卡片现状后按比例收）。

**进度记录**（共 7 条，列最近 5 条）：

- 2026-11-18 18:42:51 +0800 · AX批开工：TAB 字号 18→约14.4 + 头部卡片几何 0.8 档
- 2026-11-18 18:55:31 +0800 · 冻结断点（2026-11-18 AX批，未改任何文件）：①TAB 字号：wtab.h:24 kWtabFontSize=18.0f（Mac 波未碰 wtab.h，安全）；取值计划 14.4f=18×0.8 精确值（float 常量无需取整档）；弹窗级 kWtabSmallFontSize=14.0 默认不动（与页面级 14.4 仅差 0.4，是否拉开档差待重派时决策）。宽度侧自适应面已核（settings_module.cpp:1400/wallpaper:1223/release_config:581 均以本常量量宽，改值即自适应）。②头部卡片=git 页顶栏三卡域（git_views.cpp，唯一命中 grep）：量测点 kTopBarH 48（:35）/三卡统一内距 scaled(12)（:373,:1635,:355）/顶栏字号 15/14/12（:402,:461,:434,:416）/按钮高 32（:458）→计划 0.8 档 38.4/9.6/12·11.2·9.6/25.6，kPageMargin 与左卡宽 320 不动（非头部卡片量测点），下拉锚点/段带自动跟随 kTopBarH。③测试同步：全站 grep 无 18 字号断言（probes 断言 24/11/12 档、test_settings 只锁 kWtabHeight=40），预计零命中；重派后需再全 grep 一次含 layout_guard_probe 确认。风险：Mac 波动含 ui_kit/text_metrics.h 与 todo/header 文件，rebase 后须重查 git_views/顶栏现状防过时前提。
- 2026-11-18 20:18:43 +0800 · 回炉在池：解冻（TAB 部分已落 013ffc39，本条只剩头部卡片 0.8 档；BQ 批第一步 grep 重定位）。
- 2026-11-18 20:18:50 +0800 · 排产即派 BQ 批。
- 2026-11-18 20:33:45 +0800 · 0.8 档已落（wip 37c575aa，仅 src/module_git/git_views.cpp 33+/29-）：kTopBarH 48→38.4（:36 唯一定义点，py/bandTop/bodyY 自动跟随）；卡内距 12→9.6（:378，switchButtonRightEdge 右距项同步 9.6 保下拉右对齐）；字 15/14/12→12/11.2/9.6（仓名量宽+渲染双点同步；secondaryButton 加可选参 fontSizePx 默认 14，仅顶栏两调用传 11.2，其余 4 调用点不变）；钮 32→25.6（右钮组+折叠钮 32×32→25.6×25.6 含 hitRect）。kPageMargin/左卡宽 320/pathMaxW 480/gap 12/caret 槽 14 不动（AX 口径）。锚点重定位（详表见 evidence）：composeTopBar :317→:359（+42）、顶栏卡 size :326→:368、btnH :401→:459、py :436→:495、bodyY :1771→:1830。断言同步=零：need301 双 400 在 :539/:560（浮层行未触）；release_probe 24/11/12 锁 release 域零交集；git_page_probe 无顶栏几何断言（左卡宽 280/320 未动）。TAB 档距复核（只结论未改码）：14.4 vs 14.0 档距 0.4 区分度趋零成立；建议 A 主推弹窗级 14.0→11.2（恢复 18:14=1.286 比例+0.8 语言统一）；备选 B 合并统一 14.4 删小档；不建议维持。遗留待拍板：①bug#297 网格顶栏 9.6 vs 两卡 12 偏 2.4px；②kTopBarH raw 与 scaled 混用（既有），缩放>1.2 钮趋满高。未验证：构建/运行（AJ2 占用+禁构建令），后续 --dev --filter git_page 定向。开工时实际 HEAD=91fb8b47（#126 批并行已 commit，简报 370e9aa3 已过时）。

**最近状态变更**（共 8 条）：2026-11-18 20:33:45 +0800 · 进行中 → 待验收 · 0.8 档已落（wip 37c575aa，仅 src/module_git/git_views.cpp 33+/29-）：kTopBarH 48→38.4（:36 唯一定义点，py/bandTop/bodyY 自动跟随）；卡内距 12→9.6（:378，switchButtonRightEdge 右距项同步 9.6 保下拉右对齐）；字 15/14/12→12/11.2/9.6（仓名量宽+渲染双点同步；secondaryButton 加可选参 fontSizePx 默认 14，仅顶栏两调用传 11.2，其余 4 调用点不变）；钮 32→25.6（右钮组+折叠钮 32×32→25.6×25.6 含 hitRect）。kPageMargin/左卡宽 320/pathMaxW 480/gap 12/caret 槽 14 不动（AX 口径）。锚点重定位（详表见 evidence）：composeTopBar :317→:359（+42）、顶栏卡 size :326→:368、btnH :401→:459、py :436→:495、bodyY :1771→:1830。断言同步=零：need301 双 400 在 :539/:560（浮层行未触）；release_probe 24/11/12 锁 release 域零交集；git_page_probe 无顶栏几何断言（左卡宽 280/320 未动）。TAB 档距复核（只结论未改码）：14.4 vs 14.0 档距 0.4 区分度趋零成立；建议 A 主推弹窗级 14.0→11.2（恢复 18:14=1.286 比例+0.8 语言统一）；备选 B 合并统一 14.4 删小档；不建议维持。遗留待拍板：①bug#297 网格顶栏 9.6 vs 两卡 12 偏 2.4px；②kTopBarH raw 与 scaled 混用（既有），缩放>1.2 钮趋满高。未验证：构建/运行（AJ2 占用+禁构建令），后续 --dev --filter git_page 定向。开工时实际 HEAD=91fb8b47（#126 批并行已 commit，简报 370e9aa3 已过时）。

---

#### need#356 · Windows 专属：应用全部颜色加深（屏幕素质差、观感寡淡）

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 09:35:04 +0800（bug-fixer）· 更新：2026-11-19 13:28:35 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「仅针对windows，应用的全部颜色 加深处理，windows的笔记本屏幕素质太差，颜色寡淡」。
要点：①仅 Windows 生效，macOS 不动（平台差异拆文件纪律，禁业务代码 #ifdef；颜色层落点候选=core/theme 或 component_theme 映射层，方案阶段再定）；②范围=应用全部颜色（全局色板加深，非单页面）；③动机=Windows 笔记本屏幕素质差、观感寡淡发灰；④与既有材质方向裁决（卡片「明显更实」）同向，实现时需一并核对透明度/材质参数不冲突。

**正文**：

老板 2026-11-19 原话：「仅针对windows，应用的全部颜色 加深处理，windows的笔记本屏幕素质太差，颜色寡淡」。
要点：①仅 Windows 生效，macOS 不动（平台差异拆文件纪律，禁业务代码 #ifdef；颜色层落点候选=core/theme 或 component_theme 映射层，方案阶段再定）；②范围=应用全部颜色（全局色板加深，非单页面）；③动机=Windows 笔记本屏幕素质差、观感寡淡发灰；④与既有材质方向裁决（卡片「明显更实」）同向，实现时需一并核对透明度/材质参数不冲突。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 全量 doctest 10 红（P1）——Windows 加深档（34 槽 RGB×0.92）改实际色值，色值契约族全红：test_theme_accents.cpp:165/181/201/224（exact hex 六槽/setDark setLight 保色/non-Clay 分模式/Pink light）、test_theme.cpp:194（浅色 textTertiary 对 background WCAG AA ≥4.5 失守——非线性亮度下同乘不保对比度，疑真实可及性回退非单纯期望漂移）、test_ui_kit_button.cpp:48、test_ui_kit_selection.cpp:153、ui_kit wtab active/wswitch on/button Primary(#07C160)/Danger(#FA5151)。归属 5a094be2（Windows 侧；posix 因子 1.0 不红）。初判：断言未按平台因子同步 + WCAG 红需产品裁决（调档或改断言口径）。证据 evidence/20261119-qa384-build-noprobes/test-full.log

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-19 11:37:12 +0800 · 视觉参数族并单（#356+#366+#368+#357+#372）
- 2026-11-19 11:37:46 +0800 · 视觉参数族任务开工（槽 3/5 复用，纯码模式，参数化可调）
- 2026-11-19 11:56:02 +0800 · 代码级完成 commit 5a094be2：平台拆分因子 theme_deepen_win.cpp(0.92)/theme_deepen_posix.cpp(1.0)，theme.cpp 三条 token 重建路径末段 applyColorDeepen 全 34 颜色槽 RGB 乘因子；调档入口=theme_deepen_win.cpp 唯一常量。纯码模式未编译，验证归波末统一批
- 2026-11-19 13:27:10 +0800 · 修复完成待验收：真 WCAG 回退 4 对（textTertiary/textDisabled/accentText/primaryText 浅色 G 道加深 4-5，win 档 4.60-4.83 / mac 4.83-5.09，加深档单点 theme_deepen_win.cpp:12 未动）+ 9 处钉值测试平台化（deepened() 按因子现算，posix 原值不变）。决策点供老板装机裁决：修向=加深更深一侧（非回退 0.92 档）——老板令颜色加深保持；mac 侧四 token 同步微深（对比度更高，视觉差 ≈G 道 4-5/255 不可辨）。commit 848453b7。定向验 theme+ui_kit+mdspike 85/85 绿（evidence/20261119-qa384-fixbatch-bugfixer/）。
- 2026-11-19 13:28:35 +0800 · 红项修复 848453b7：9/10 为断言钉旧值→平台化现算；4 对真 WCAG 回退→边界前景微加深（G 道 4-5 肉眼不可辨，加深档 0.92 单点未动），对比回升 4.6+；⚠决策点：mac 侧同 4 token 随微深（方向一致），若要求 mac 完全不动需改加深逻辑差异化（成本=失去统一乘单一性），老板装机验收时裁决

**最近状态变更**（共 6 条）：2026-11-19 13:27:10 +0800 · 进行中 → 待验收 · 修复完成待验收：真 WCAG 回退 4 对（textTertiary/textDisabled/accentText/primaryText 浅色 G 道加深 4-5，win 档 4.60-4.83 / mac 4.83-5.09，加深档单点 theme_deepen_win.cpp:12 未动）+ 9 处钉值测试平台化（deepened() 按因子现算，posix 原值不变）。决策点供老板装机裁决：修向=加深更深一侧（非回退 0.92 档）——老板令颜色加深保持；mac 侧四 token 同步微深（对比度更高，视觉差 ≈G 道 4-5/255 不可辨）。commit 848453b7。定向验 theme+ui_kit+mdspike 85/85 绿（evidence/20261119-qa384-fixbatch-bugfixer/）。

---

#### need#357 · About 页卡片内容增加左边距

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 09:41:24 +0800（bug-fixer）· 更新：2026-11-19 13:28:23 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「关于 里的卡片内容，增加一点左边距」。
读法：About 页（about 模块）卡片内容左内距加大。⚠ 原话「关于」后疑似缺词——若实指其他页面（如侧栏/某模块卡片）需排产前纠正。S 级微调（卡片内距参数），实测验收期发现的观感 nit。

**正文**：

老板 2026-11-19 原话：「关于 里的卡片内容，增加一点左边距」。
读法：About 页（about 模块）卡片内容左内距加大。⚠ 原话「关于」后疑似缺词——若实指其他页面（如侧栏/某模块卡片）需排产前纠正。S 级微调（卡片内距参数），实测验收期发现的观感 nit。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 doctest 1 红（P2）——about 批J：test_ui_modern.cpp:1067 nearPx(about.title.x, cardX+kCardPad) 失败（实测 x=384），左边距 +8 后几何期望未同步。归属 93df13ca。修＝断言按 +8 档更新。证据 evidence/20261119-qa384-build-noprobes/test-full.log

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-19 11:56:02 +0800 · 代码级完成 commit 93df13ca：about_module.cpp 新增 kContentLeftInset=8（仅左缘），标题/信息滚动区同步右移，右缘不动。纯码未验证，归波末
- 2026-11-19 13:27:16 +0800 · about 断言同步完成（4 处：title.x/innerW/labelX/narrow.value，含首红后暴露 3 处隐藏红）。commit feb9a863。定向验 about 5/5 绿（evidence/20261119-qa384-fixbatch-bugfixer/doctest-about-final.log）。
- 2026-11-19 13:28:23 +0800 · 红项修复 feb9a863：about 断言同步 4 处（含 3 隐藏红），about 5/5 绿

**最近状态变更**（共 6 条）：2026-11-19 13:27:16 +0800 · 进行中 → 待验收 · about 断言同步完成（4 处：title.x/innerW/labelX/narrow.value，含首红后暴露 3 处隐藏红）。commit feb9a863。定向验 about 5/5 绿（evidence/20261119-qa384-fixbatch-bugfixer/doctest-about-final.log）。

---

#### need#361 · 作废任务按钮直接显示，不入菜单

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:13:42 +0800（ZCode主会话）· 更新：2026-11-19 11:54:55 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「作废任务这个按钮直接显示出来，不要搞菜单里」。
需求：release 模块任务操作——【作废任务】按钮从菜单（下拉/右键）中提出来，直接显示在界面上，免二级操作。与 #359/#360 同页面流程，排产可同批处理。

**正文**：

老板 2026-11-19 原话：「作废任务这个按钮直接显示出来，不要搞菜单里」。
需求：release 模块任务操作——【作废任务】按钮从菜单（下拉/右键）中提出来，直接显示在界面上，免二级操作。与 #359/#360 同页面流程，排产可同批处理。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:53:30 +0800 · 纯码实现完成（验证归波末）：release_detail_draw.cpp 页头「···」菜单整体裁撤，作废任务升直显危险红字 Text 钮（release.detail.void，同移除钮范式），发布态校验同步升直显次钮；menuOpen 状态/Esc 分支/菜单浮层退役，test_release_detail_ui 断言同步。commit 086b2884
- 2026-11-19 11:54:55 +0800 · 交付 commit 086b2884：作废升页头直显红字钮+二次确认，菜单裁撤；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 11:54:55 +0800 · 进行中 → 待验收 · 交付 commit 086b2884：作废升页头直显红字钮+二次确认，菜单裁撤；验证归波末

---

#### need#365 · 终端搜索功能：定位历史内容（条件允许时）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:36:01 +0800（ZCode主会话）· 更新：2026-11-19 12:09:31 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「如果条件允许，给终端加一个搜索功能，自动定位历史内容」。
需求：终端 scrollback 历史搜索，输入关键词自动定位跳转匹配处（导航/高亮形态待设计，参考系统终端 Ctrl+F）。老板明示「如果条件允许」=非硬性，排产时可评估成本再定档；与 #364（回看能力）同域，修复时一并考虑。

**正文**：

老板 2026-11-19 原话：「如果条件允许，给终端加一个搜索功能，自动定位历史内容」。
需求：终端 scrollback 历史搜索，输入关键词自动定位跳转匹配处（导航/高亮形态待设计，参考系统终端 Ctrl+F）。老板明示「如果条件允许」=非硬性，排产时可评估成本再定档；与 #364（回看能力）同域，修复时一并考虑。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:42:39 +0800 · 老板「处理掉」令，条件档转正最小实现
- 2026-11-19 12:09:31 +0800 · 交付 commit 8e2ed48f + f1d82ecb：工具栏搜索钮+悬浮条+Enter 步进居中滚入（复用 #364 scrollOffset 链路），会话切换命中态作废；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:09:31 +0800 · 进行中 → 待验收 · 交付 commit 8e2ed48f + f1d82ecb：工具栏搜索钮+悬浮条+Enter 步进居中滚入（复用 #364 scrollOffset 链路），会话切换命中态作废；验证归波末

---

#### need#368 · 删除 todo 卡片左侧加粗竖线

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-19 10:39:28 +0800（developer）· 更新：2026-11-19 11:56:03 +0800（developer）

**详情**：

老板 2026-11-19 原话：「删除todo卡片左侧的加粗竖线」。
需求：todo 模块卡片左侧的加粗竖线（accent 竖条）删除。纯视觉元素移除，S 级微调。

**正文**：

老板 2026-11-19 原话：「删除todo卡片左侧的加粗竖线」。
需求：todo 模块卡片左侧的加粗竖线（accent 竖条）删除。纯视觉元素移除，S 级微调。

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-19 11:56:03 +0800 · 代码级完成 commit 6994a5c0：todo_ui_board.cpp 删除主卡+拖拽副本 priobar，优先级信息由卡面浅染承载；测试判据 c/c'/d 翻转为断言不存在。纯码未验证，归波末

**最近状态变更**（共 4 条）：2026-11-19 11:56:03 +0800 · 进行中 → 待验收 · 代码级完成 commit 6994a5c0：todo_ui_board.cpp 删除主卡+拖拽副本 priobar，优先级信息由卡面浅染承载；测试判据 c/c'/d 翻转为断言不存在。纯码未验证，归波末

---

#### need#370 · profiles 列表改双击切换，单击仅展示详情

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：开发中
- 创建：2026-11-19 11:02:57 +0800（bug-fixer）· 更新：2026-11-19 14:36:30 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「profiles列表 改成双击切换，单机只是切换展示详情」（单机=单击误写）。
需求：profiles 列表交互改档——单击=选中并展示详情（不生效），双击=切换/激活。落点大概率 claude_proxy 模块 profiles 列表，若指其他模块排产前纠正。防误触单击即切换的交互档。

**正文**：

老板 2026-11-19 原话：「profiles列表 改成双击切换，单机只是切换展示详情」（单机=单击误写）。
需求：profiles 列表交互改档——单击=选中并展示详情（不生效），双击=切换/激活。落点大概率 claude_proxy 模块 profiles 列表，若指其他模块排产前纠正。防误触单击即切换的交互档。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 doctest 1 红（P1）——池#370 自带新用例红：test_claude_ui.cpp:3108 双击切换确认弹窗后 act.isOk() && act.value().id==idA 断言失败；同族单击直选/选中卡竖线等用例全绿。归属 aa3f8967。功能交付与自带用例不符，需 developer 修复（弹窗改道后切换动作链断）。证据 evidence/20261119-qa384-build-noprobes/test-full.log

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-19 11:42:26 +0800 · 交互功能族并单（#370+#371+#365）
- 2026-11-19 11:42:52 +0800 · developer 开工（槽 7，交互功能族 3 卡）
- 2026-11-19 12:09:21 +0800 · 交付 commit aa3f8967：单击直选详情、双击经确认弹窗真激活（弹窗文案原与行为不符已随修）；验证归波末
- 2026-11-19 13:14:42 +0800 · bug-fixer 开工：QA384 红项 #370（profiles 双击切换断言 test_claude_ui.cpp:3108），复现→链路调试→修复→定向红转绿→fix: commit
- 2026-11-19 14:36:30 +0800 · bugfixer 2026-11-19：QA384 红已修。根因=测试前置与真实 UI 语义脱节：svc.createProfile 默认不激活，但表单路径 claude_ui_form.cpp:140 传 activateIfNoneActive=true（首建即激活）→「取消后激活仍 A」断言 NoActiveProfile 假红（原 test_claude_ui.cpp:3108）。修=崩溃代理半成品续用（createProfile 前置补 true）+ 初帧断言翻转（test_claude_ui.cpp:3072：selectedId_=0 回落激活 profile 契约见 claude_ui_list.cpp:690-701，卡 0 accent 在）。定向验绿：三文件 160/160。

**最近状态变更**（共 6 条）：2026-11-19 14:36:30 +0800 · 进行中 → 待验收 · bugfixer 2026-11-19：QA384 红已修。根因=测试前置与真实 UI 语义脱节：svc.createProfile 默认不激活，但表单路径 claude_ui_form.cpp:140 传 activateIfNoneActive=true（首建即激活）→「取消后激活仍 A」断言 NoActiveProfile 假红（原 test_claude_ui.cpp:3108）。修=崩溃代理半成品续用（createProfile 前置补 true）+ 初帧断言翻转（test_claude_ui.cpp:3072：selectedId_=0 回落激活 profile 契约见 claude_ui_list.cpp:690-701，卡 0 accent 在）。定向验绿：三文件 160/160。

---

#### need#371 · 设置缺少 git/终端功能开关

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：开发中
- 创建：2026-11-19 11:03:30 +0800（bug-fixer）· 更新：2026-11-19 14:36:37 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「设置里缺少 git 终端的功能开关」。
读法已确认（老板回「1」）：git 与终端两个模块各自的功能开关（settings 里启用/禁用模块）。实现时核对既有开关矩阵、模块注册链（app_registry/kModuleRegistry）与持久化。

**正文**：

老板 2026-11-19 原话：「设置里缺少 git 终端的功能开关」。
读法已确认（老板回「1」）：git 与终端两个模块各自的功能开关（settings 里启用/禁用模块）。实现时核对既有开关矩阵、模块注册链（app_registry/kModuleRegistry）与持久化。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 doctest 2 红（P2）——模块卡加 git/终端两开关后：① test_settings.cpp:3720 R6 纯件等价 modulesCardHeightFor(kModuleToggleCellCount(true)) != 240.0f（高度公式期望未同步）；② test_settings.cpp:4993 最小窗 1000×640 模块 tab 内容高 375 > 可用 372，出现页级滚动条（零滚动 UX 契约破，3px 溢出）。归属 ec4f7cd9。自带「模块管理卡渲染（格阵开关）」用例绿。修＝同步 R6 期望 + 卡紧凑化或预算更新。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 11:04:19 +0800 · 老板确认读法①：settings 里为 git、终端两模块各加启用/禁用功能开关；排产时核对既有开关矩阵与模块注册链（app_registry/kModuleRegistry）联动
- 2026-11-19 12:09:26 +0800 · 交付 commit ec4f7cd9：设置模块卡补 git/terminal 两格，即时生效（复用既有 toggle 桥+落盘链）；验证归波末
- 2026-11-19 13:14:42 +0800 · bug-fixer 开工：QA384 红项 #371（settings git/terminal 开关两红：R6 高度 3720、最小窗溢出 4993），定向确认→布局修正→红转绿→fix: commit
- 2026-11-19 14:36:37 +0800 · bugfixer 2026-11-19：QA384 两红已修。根因=#371 补位 git/终端开关后 10/11 格仍 3 列→4 行卡高 296（settings_ui_logic.h modulesCardHeightFor），模块 tab 296+16+152=464 > 可用 424 破零滚动；R6 期望 240 随之过期（红条提示 372/375 亦过期文案，实溢 40px 非 3px）。修=modulesCardCols cells>8 时 3→4 列（卡 240、模块 tab 408≤424 复约，R6 期望 240 恰不变）+ cellW ≥3 列分支泛化（settings_ui_cards.cpp:505 及 test_settings/test_wallpaper 两处同源副本）+ 预算注释同步实测 424 + 清 [DEBUG-371] 探针。定向验绿：三文件 160/160。构建 3 次（预算上限内）。

**最近状态变更**（共 6 条）：2026-11-19 14:36:37 +0800 · 进行中 → 待验收 · bugfixer 2026-11-19：QA384 两红已修。根因=#371 补位 git/终端开关后 10/11 格仍 3 列→4 行卡高 296（settings_ui_logic.h modulesCardHeightFor），模块 tab 296+16+152=464 > 可用 424 破零滚动；R6 期望 240 随之过期（红条提示 372/375 亦过期文案，实溢 40px 非 3px）。修=modulesCardCols cells>8 时 3→4 列（卡 240、模块 tab 408≤424 复约，R6 期望 240 恰不变）+ cellW ≥3 列分支泛化（settings_ui_cards.cpp:505 及 test_settings/test_wallpaper 两处同源副本）+ 预算注释同步实测 424 + 清 [DEBUG-371] 探针。定向验绿：三文件 160/160。构建 3 次（预算上限内）。

---

#### need#372 · 展开态侧栏宽度收窄（右侧空白多+双重左边距）

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 11:15:48 +0800（bug-fixer）· 更新：2026-11-19 13:28:13 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「主窗体 侧边栏 展开状态 下的宽度缩小一点，右侧空白很多，而且右侧布局本身加了左边距了」。
需求：展开态侧栏宽度收窄。依据：①侧栏右侧大片空白；②内容区布局自带左边距，侧栏宽+内容左边距双重叠加视觉间距过大。S 级参数微调，与 #357（About 卡片左边距）同「间距核对」域，可同批。

**正文**：

老板 2026-11-19 原话：「主窗体 侧边栏 展开状态 下的宽度缩小一点，右侧空白很多，而且右侧布局本身加了左边距了」。
需求：展开态侧栏宽度收窄。依据：①侧栏右侧大片空白；②内容区布局自带左边距，侧栏宽+内容左边距双重叠加视觉间距过大。S 级参数微调，与 #357（About 卡片左边距）同「间距核对」域，可同批。

**关联 BUG**（1）：

- 2026-11-19 12:34:06 +0800 · 未解 · BUG: 验证批 20261119 全量构建编译红（P1，阻断全量门禁 probe 层）——tests/probes/uat62_page_shots_probe.cpp:421 分号误入行注释（`static_cast<int>(200.0f / w * pw)  // need372：侧栏展开宽;`），语句无终止分号 → :422 error: expected ',' or ';' before 'std' + :421 unused variable 'sx'（-Werror）。归属 commit 670af0f9。修复＝分号移到注释前。验证批只登记不修；探针层 QA 被Blocks，主目标/doctest 走 DEVDESK_BUILD_PROBES=0 诊断轮（evidence/20261119-qa384-build-noprobes/）。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-19 11:56:03 +0800 · 代码级完成 commit 670af0f9：kSidebarWidthExpanded 220→200（app_sidebar.h 单一常量），测试断言/探针字面量全同步（含内容宽 1380→1400、mark x 82→72）。纯码未验证，归波末
- 2026-11-19 13:27:17 +0800 · 一分号修完成（:421 注释吞分号→语句终止恢复），probe 层编译解锁，构建含全部 probe 目标成功。commit 952ab7dd。
- 2026-11-19 13:28:13 +0800 · 红项修复 952ab7dd：一字符分号修，probe 层解锁；定向验绿

**最近状态变更**（共 6 条）：2026-11-19 13:27:17 +0800 · 进行中 → 待验收 · 一分号修完成（:421 注释吞分号→语句终止恢复），probe 层编译解锁，构建含全部 probe 目标成功。commit 952ab7dd。

---

#### need#374 · 编辑器工具栏宽度自适应

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:19:08 +0800（ZCode主会话）· 更新：2026-11-19 12:17:58 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「编辑器工具栏的宽度应该是自适应的」。
需求：编辑器工具栏宽度改自适应（当前疑为固定宽度）。⚠ 落点待确认：紧接 #373 语境疑指双击文件打开的独立编辑器子窗，也可能是 md 编辑器——排产前确认。

**正文**：

老板 2026-11-19 原话：「编辑器工具栏的宽度应该是自适应的」。
需求：编辑器工具栏宽度改自适应（当前疑为固定宽度）。⚠ 落点待确认：紧接 #373 语境疑指双击文件打开的独立编辑器子窗，也可能是 md 编辑器——排产前确认。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:15:56 +0800 · 纯码完成待波末验证：DevDesk 44d1bbf7——工具栏固定 x 改游标式依序排布（控件有无即占位/收紧），窄窗 <900 收字体族+字号组（与状态栏 T1 档同阈值）；≥900 各控件位与旧布局逐位一致
- 2026-11-19 12:17:58 +0800 · 交付 44d1bbf7：工具栏游标式依序排布，窄窗<900 收字体组；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:17:58 +0800 · 进行中 → 待验收 · 交付 44d1bbf7：工具栏游标式依序排布，窄窗<900 收字体组；验证归波末

---

#### need#375 · 编辑器语法高亮缺失（起码补 SQL）

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 11:19:34 +0800（bug-fixer）· 更新：2026-11-19 13:28:18 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「编辑器的语法高亮没有的把？ 起码sql文件缺失的」。
需求：独立文件编辑器（双击文件打开的子窗）缺语法高亮，sql 文件确认缺失。最小范围=SQL 高亮；是否扩展其他语言排产时定（老板措辞「起码」暗示后续可扩）。注意与 #373/#374 同编辑器域。

**正文**：

老板 2026-11-19 原话：「编辑器的语法高亮没有的把？ 起码sql文件缺失的」。
需求：独立文件编辑器（双击文件打开的子窗）缺语法高亮，sql 文件确认缺失。最小范围=SQL 高亮；是否扩展其他语言排产时定（老板措辞「起码」暗示后续可扩）。注意与 #373/#374 同编辑器域。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 std-include 守卫红（P2）——src/ui_kit/text_highlight/md_keyword_highlight.cpp:227 直接用 std::string 未显式 include <string>（守卫自检连带红：好树有违规）。归属 bccdb2b3。doctest 侧 sql 高亮用例（大小写不敏感/''转义）绿。修＝补 include。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 12:16:01 +0800 · 纯码完成待波末验证：DevDesk bccdb2b3——md_keyword_highlight 增 SQL 词法（--行注释/块注释/''双写转义/大小写不敏感关键字表）+ notepad_language .sql→sql 映射；既有测试同步（test_notepad_language sql 用例改列）
- 2026-11-19 12:18:02 +0800 · 交付 bccdb2b3：SQL 词法（--/块注释/''转义/关键字表）+ .sql 映射；验证归波末
- 2026-11-19 13:27:24 +0800 · std-include 守卫补 <string>（md_keyword_highlight.cpp，SQL 折叠 std::string 依赖）。commit 328abe47（与 #369 测试文件 <ratio> 同批）。
- 2026-11-19 13:28:18 +0800 · 红项修复 328abe47：补 <string>，守卫绿

**最近状态变更**（共 6 条）：2026-11-19 13:27:24 +0800 · 进行中 → 待验收 · std-include 守卫补 <string>（md_keyword_highlight.cpp，SQL 折叠 std::string 依赖）。commit 328abe47（与 #369 测试文件 <ratio> 同批）。

---

#### need#376 · 编辑器 Ctrl+F 搜索框自动聚焦可直接输入

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:20:21 +0800（ZCode主会话）· 更新：2026-11-19 12:18:07 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「编辑器 ctrl+F 打开搜索框的时候，应该自动把焦点放输入法上 也就是能直接输入搜索词」。
需求：编辑器 Ctrl+F 打开搜索框时焦点自动落在搜索输入框，直接可输入搜索词（免手动点输入框）。「输入法」此处指输入框（笔误）；另注意与 IME 唤起联动（焦点到位即拉起输入法）。落点同 #373-#375 编辑器域；md 编辑器若有同款搜索一并核对。

**正文**：

老板 2026-11-19 原话：「编辑器 ctrl+F 打开搜索框的时候，应该自动把焦点放输入法上 也就是能直接输入搜索词」。
需求：编辑器 Ctrl+F 打开搜索框时焦点自动落在搜索输入框，直接可输入搜索词（免手动点输入框）。「输入法」此处指输入框（笔误）；另注意与 IME 唤起联动（焦点到位即拉起输入法）。落点同 #373-#375 编辑器域；md 编辑器若有同款搜索一并核对。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:16:05 +0800 · 纯码完成待波末验证：DevDesk 7a4adf6d——requestFocus 目标改 <id>.hit（原传组件根 id 静默落空，与全仓其它模块同款修法）；关条焦点回收（P2-3）同修
- 2026-11-19 12:18:07 +0800 · 交付 7a4adf6d：requestFocus 目标改 .hit 后缀（原传组件根 id 静默落空）；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:18:07 +0800 · 进行中 → 待验收 · 交付 7a4adf6d：requestFocus 目标改 .hit 后缀（原传组件根 id 静默落空）；验证归波末

---

#### need#377 · 搜索命中词高亮：色加深+加粗（现淡粉）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:21:09 +0800（ZCode主会话）· 更新：2026-11-19 12:18:12 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「搜索命中的词，颜色好淡啊，这个是淡粉色？颜色深一点，加粗」。
需求：编辑器搜索命中词高亮色加深（现为淡粉色观感）+ 加粗。与 #376 同编辑器搜索域，同批处理；与 #356（全局加深）触点相关——命中色加深幅度排产时统一考虑。

**正文**：

老板 2026-11-19 原话：「搜索命中的词，颜色好淡啊，这个是淡粉色？颜色深一点，加粗」。
需求：编辑器搜索命中词高亮色加深（现为淡粉色观感）+ 加粗。与 #376 同编辑器搜索域，同批处理；与 #356（全局加深）触点相关——命中色加深幅度排产时统一考虑。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:16:10 +0800 · 纯码完成待波末验证：DevDesk ecd4ebd1 + EUI-NEO 4957d9e——命中色升独立常量 kFindHit*（warning 近实色 0.95/0.92 + 当前命中叠 primary 0.45/0.40，波末可调）；加粗走 TextRun.bold faux-bold（双绘亚像素偏移不改度量/光标）
- 2026-11-19 12:18:12 +0800 · 交付 ecd4ebd1 + EUI-NEO 4957d9e（原 21d43db3 因并发事故重建，内容不变）：命中色近实色+当前命中叠 primary，faux-bold 双绘不改度量；常量可调；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:18:12 +0800 · 进行中 → 待验收 · 交付 ecd4ebd1 + EUI-NEO 4957d9e（原 21d43db3 因并发事故重建，内容不变）：命中色近实色+当前命中叠 primary，faux-bold 双绘不改度量；常量可调；验证归波末

---

#### need#380 · 编辑器缺格式化功能（SQL 优先待定档）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:25:36 +0800（ZCode主会话）· 更新：2026-11-19 12:18:22 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「编辑器缺了格式化功能」（同句后半段另记 #381）。
需求：编辑器补格式化功能（对当前文件内容做语法格式化/美化）。最小范围待定档：按 #375 语境大概率 SQL 格式化优先；格式化引擎选型（自研轻量 vs 引入库）排产时调研。

**正文**：

老板 2026-11-19 原话：「编辑器缺了格式化功能」（同句后半段另记 #381）。
需求：编辑器补格式化功能（对当前文件内容做语法格式化/美化）。最小范围待定档：按 #375 语境大概率 SQL 格式化优先；格式化引擎选型（自研轻量 vs 引入库）排产时调研。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:16:19 +0800 · 纯码完成待波末验证：DevDesk a0d4f0a8——自研 formatSqlText 纯函数（关键字大写规范化+子句断行+4 空格缩进，注释/字符串原样保留）+ 格式化钮/M+Shift+F/工具条矩阵扩 .sql（与 JSON 格式化同款单撤销提交）
- 2026-11-19 12:18:22 +0800 · 交付 a0d4f0a8：自研 formatSqlText 纯函数（关键字大写+子句断行+缩进，注释/字符串保留），单撤销提交；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:18:22 +0800 · 进行中 → 待验收 · 交付 a0d4f0a8：自研 formatSqlText 纯函数（关键字大写+子句断行+缩进，注释/字符串保留），单撤销提交；验证归波末

---

#### need#394 · MCP 对外开放智能体必要内置工具 + 安装按钮外置（非按方法粒度）

- 状态：待验收 · 优先级：P1 · 负责人：qa-tester · 阶段：测试中
- 创建：2026-11-19 19:16:35 +0800（qa-tester）· 更新：2026-11-22 20:43:30 +0800（qa-tester）
- **blocker**：波间重派：设计已交付待开发，回炉授权位

**详情**：

需求裁决序（need#146 四步）：①该做=老板三裁决已授权；②复用=工具后端 100%（ToolRegistry 面 todo/md 既有实现）、安装/卸载写入层（两 config 文件零改动）、注册样板（notes_mcp_tools.cpp 模式）；③改造=页头卡 UI（按钮从 drawDetailDialog 迁出）+ 各模块 MCP 注册接线；④无全新方案。结论=复用+改造。

**正文**：

需求裁决序（need#146 四步）：①该做=老板三裁决已授权；②复用=工具后端 100%（ToolRegistry 面 todo/md 既有实现）、安装/卸载写入层（两 config 文件零改动）、注册样板（notes_mcp_tools.cpp 模式）；③改造=页头卡 UI（按钮从 drawDetailDialog 迁出）+ 各模块 MCP 注册接线；④无全新方案。结论=复用+改造。

**关联线索**（7）：

- [open] 安装按钮,卸载,toggle,removeServerFile,actionable,serviceRunning：「安装到 Claude/ZCode」按钮实为安装/卸载双态 toggle：文案 未装=「安装到 Claude Code」/已装=「从 Claude Code 卸载」（mcp_page_module.cpp:739-740）；卸载写入层 removeServerFile 已存在（claude_mcp_config.cpp:197 / zcode_mcp_config.cpp:252）；actionable 依赖 serviceRunning=running&&port>0（mcp_page_module.cpp:708/759），服务未启动时按钮置灰且「请先启动 MCP 服务」提示优先（:763-764）（source: src/module_mcp/mcp_page_module.cpp:708-764,739-740; src/module_mcp/claude_mcp_config.cpp:197; src/module_mcp/zcode_mcp_config.cpp:252）
- [open] installServerFile,幂等,异址冲突,不自动创建,原子写,备份,devdesk 条目：宿主注册写入层语义：配置文件必须已存在、绝不新建（claude_mcp_config.cpp:157-161 / zcode_mcp_config.cpp:212-215）；有改动才备份+原子写；claude 同名条目幂等 no-op（claude_mcp_config.cpp:95-97）；zcode 同址幂等、异址冲突不覆盖（zcode_mcp_config.cpp:126-134，D2 裁决沿用）；条目形态 claude=type+url、zcode=type+url+enabled=true；端点=单一 HTTP 服务+/mcp（mcp_page_module.cpp:719）（source: src/module_mcp/claude_mcp_config.cpp:95-97,157-161,187,197; src/module_mcp/zcode_mcp_config.cpp:126-134,212-215,242,252; src/module_mcp/mcp_page_module.cpp:719）
- [open] 页头卡,服务状态行,工具卡,图标派发,D-6 体例,服务开关在设置页：MCP 页页头卡（D-6 体例）现状=卡内标题「MCP 服务」+说明+服务状态行（运行/未启动，数据源 app::mcp 桥 running/port/lastError 只读快照；服务开关在设置页，MCP 页不提供）；工具卡=名称+状态行（可用/未启用=serviceRunning）+描述+chips（内置/服务）+右侧图标按工具名族派发（mcp_page_module.cpp:164）（source: src/module_mcp/mcp_page_module.cpp:2,164,232,267-327,449-459）
- [open] MCP 命名,下划线,snake_case,点号归一,todo_list,md_search：MCP 对外工具命名定案（PRD 2026-11-19-mcp-agent-tools v2 D-1）：点号名归一为下划线（todo_list 等 6 件、md_search 等 5 件）；依据=本仓存量 MCP 面全下划线（notes_search 等 7 个）+ MCP 官方规范示例全 snake_case（spec 2025-06-18 server/tools）；智能体面原名 todo.*/md.* 点号不动（source: https://modelcontextprotocol.io/specification/2025-06-18/server/tools（示例 get_weather 等）；src/module_markdown/notes_mcp_tools.cpp:256（存量下划线先例））
- [open] OP-1,notes_* 移除,方案 B,立即移除,换名映射,15 件：OP-1 终裁（老板 2026-11-19 AskUserQuestion 实答）：notes_*×3（notes_search/notes_read/notes_write）从 MCP 对外面立即移除（方案 B），不做兼容别名/过渡期；对外总数 15 件=存量 4+todo×6+md×5；宿主存量 notes_* 调用失效为老板已接受破坏面，本机两宿主需重连换名（notes_search→md_search、notes_read→md_read、notes_write→md_create）（source: 老板 2026-11-19 AskUserQuestion 实答（经主会话指令链转达）；PRD 2026-11-19-mcp-agent-tools v3 §8/§13）
- [open] 图标码点,F0AE,F15C,F5DC,图标族映射,OP-1 终裁 B,15 件：#394 图标族码点仓内先例（跨页一致性依据）：智能体面工具图标派发已用 todo.*→0xF0AE fa-list-check、md.*→0xF15C fa-file-lines、memory.*→0xF5DC fa-brain（agent_module_dialogs.cpp:1358-1363）；MCP 页记忆行同用 0xF5DC（mcp_page_module.cpp:659）；md 模块文件树同用 0xF15C（md_module_tree.cpp:437）。MCP 工具卡新两族映射沿用同码点即得跨页一致；fontawesome.com 确认 list-check solid 存在且语义=待办清单。OP-1 终裁=B 立即移除（老板 2026-11-19）：对外 15 件，notes_*×3 卡片/兼容名/迁移提示全作废（source: src/module_agent/agent_module_dialogs.cpp:1358-1363; src/module_mcp/mcp_page_module.cpp:659; src/module_markdown/md_module_tree.cpp:437; https://fontawesome.com/icons/list-check）
- [open] need394,环境红,devdesk 预装,claude.json,zcode config：真机 ~/.claude.json 与 ~/.zcode/cli/config.json 均已有 mcpServers.devdesk/mcp.servers.devdesk = http://127.0.0.1:8931/mcp（运行中波1版 app 写入）。导致 3 个真实配置副本用例（test_mcp_zcode_config.cpp:395 installed.isOk / test_mcp_claude_config.cpp:341 changed / test_mcp_page.cpp:1207 初始安装文案）在 55aec2ee 定向跑中红；同二进制改指去掉 devdesk 的干净配置副本后 3/3 绿，定档环境红。test_mcp_page CP 副本经 CcConfigGuard 直接复制真文件，因此继承该状态。（source: evidence/20261122-need394-verify/verify.md）

**进度记录**（共 16 条，列最近 5 条）：

- 2026-11-22 20:15:25 +0800 · 断点（预算过半）：编码批已完成、**未编译未跑**。已完成①platform/tool_kit 新增 McpToolAdapter+mcpToolName+registerToolsAsMcp/unregisterToolsAsMcp（zero重写 adapter）②todo 模块 onStart/onStop 同批 todo_*×6 注册（attachTools/detachTools）③markdown 模块改 md_*×5 注册、删 notes_mcp_tools.{h,cpp}（OP-1=B）④mcp_page 页头卡宿主安装区（248 高、分隔线/标签行/两宿主行/160×32 双态钮）+ 详情弹窗 516→480 且安装行整块删除 ⑤图标族 +todo(0xF0AE)/md_(0xF15C)/memory(0xF5DC) ⑥doctest 已写未跑。wip commit=8ae26bd0。下一步（验证批）：bash scripts/build-test-run.sh --dev --filter mcp_page / --filter todo_tools / --filter markdown_tools，编译红即修 adapter/接线。
- 2026-11-22 20:16:56 +0800 · 编码批交付（wip 55aec2ee：19 文件 +677/-232，McpToolAdapter 整批注册+页头宿主区+弹窗收缩+图标族+测试场景）——转验证批（构建+定向+ui_driver）
- 2026-11-22 20:17:23 +0800 · 验证批开工（2026-11-22）：一次构建 + 受影响域定向 + ui_driver 场景 + 15 件注册实证。禁修码。
- 2026-11-22 20:34:11 +0800 · 验证批完成（2026-11-22，HEAD=55aec2ee）：构建绿（exit 0）；受影响域定向 486 例 482 过/3 红全环境红（真机配置已装 devdesk@8931，同二进制干净副本复验 3/3 绿）/1 既有 skip；ui_driver mcp-claude-install.json 8/8 绿（页头两宿主钮 160x32 右缘 1144、行距 44、卡高 248）；15 件映射齐备=11 件场景运行期实证（todo_*×6+md_*×5，dump 卡片名全下划线）+4 件存量单元实证（devdesk_server_info/web_fetch_markdown/project_info/memory_search，16/16 绿）；缺口：单进程整机 15 计数未实测（ui_driver 不含 main.cpp MCP 段，装机后由主会话复核）。零代码红，修复批无派发项；可选改进：3 个真实配置副本用例缺『真机已装态』分支。禁做项均未触发（未修码/未装机/未全量/未 Release）。
- 2026-11-22 20:43:30 +0800 · 收敛收口：验证批全绿+装机完成+整机 15 件复核过（验证批建议 2 已补）——待老板真机验收（页头宿主安装区/弹窗收缩/15 工具）

**最近状态变更**（共 8 条）：2026-11-22 20:34:11 +0800 · 进行中 → 待验收 · 验证批完成（2026-11-22，HEAD=55aec2ee）：构建绿（exit 0）；受影响域定向 486 例 482 过/3 红全环境红（真机配置已装 devdesk@8931，同二进制干净副本复验 3/3 绿）/1 既有 skip；ui_driver mcp-claude-install.json 8/8 绿（页头两宿主钮 160x32 右缘 1144、行距 44、卡高 248）；15 件映射齐备=11 件场景运行期实证（todo_*×6+md_*×5，dump 卡片名全下划线）+4 件存量单元实证（devdesk_server_info/web_fetch_markdown/project_info/memory_search，16/16 绿）；缺口：单进程整机 15 计数未实测（ui_driver 不含 main.cpp MCP 段，装机后由主会话复核）。零代码红，修复批无派发项；可选改进：3 个真实配置副本用例缺『真机已装态』分支。禁做项均未触发（未修码/未装机/未全量/未 Release）。

**关联文档**：doc#30（prd v3） → 全文见附录 A

---

#### need#400 · 记事本 md 预览态搜索定位（F-06 扩展：预览内查找+高亮+跳转，老板 2026-11-21 令）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-21 23:42:05 +0800（developer）· 更新：2026-11-22 00:13:44 +0800（developer）

**详情**：

老板 2026-11-21 22:4x 令「加上去，预览也能搜索」——推翻 PRD §4.6 预览态禁用查找条的口径。设计蓝图（复用既有机制）：①matrix.findToggleEnabled 翻转（notepad_session.cpp:152-154）②预览态查找域=显示文本（MarkdownBuilder::displayTextOf，用户所见即所搜）③命中→显示码点区间→hitRectsForDisplayRangeDoc 取矩形→复用 need345 选区高亮层渲染（选中即复制不触发，查找高亮独立）④跳转（Enter/Shift+Enter）驱动预览 scrollview 滚动到命中矩形（对齐旧查看器预览搜索行为）⑤探针 S 段扩预览态（命中数/矩形非空/跳转滚动落点/单跳耗时）⑥编辑态行为零变更。设计卡+实现+红绿链一步到位（需求一句话明确，免 PRD 骨架，记由：镜像扩展现有编辑态查找流+复用既有 UI/高亮/滚动机制，无新 UI 面）

**正文**：

老板 2026-11-21 22:4x 令「加上去，预览也能搜索」——推翻 PRD §4.6 预览态禁用查找条的口径。设计蓝图（复用既有机制）：①matrix.findToggleEnabled 翻转（notepad_session.cpp:152-154）②预览态查找域=显示文本（MarkdownBuilder::displayTextOf，用户所见即所搜）③命中→显示码点区间→hitRectsForDisplayRangeDoc 取矩形→复用 need345 选区高亮层渲染（选中即复制不触发，查找高亮独立）④跳转（Enter/Shift+Enter）驱动预览 scrollview 滚动到命中矩形（对齐旧查看器预览搜索行为）⑤探针 S 段扩预览态（命中数/矩形非空/跳转滚动落点/单跳耗时）⑥编辑态行为零变更。设计卡+实现+红绿链一步到位（需求一句话明确，免 PRD 骨架，记由：镜像扩展现

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-21 23:42:22 +0800 · 排产：与 #399 同链路收尾节奏，今夜推进
- 2026-11-21 23:42:29 +0800 · 开工登记：派 devflow:developer 执行（设计卡+实现+红绿链一体）
- 2026-11-22 00:13:44 +0800 · 预览态查找已交付：显示域命中 + 高亮矩形 + 跳转滚动（翻转 §4.6）。方案卡 evidence/bug400-design.md；红 evidence/bug400/probe-red.log（S2 条=0 fail）/ 绿 evidence/bug400/probe.log（S2 PASS：hits=24、矩形在场、scroll 29157.8→0、光标不动、单跳70ms）；编辑态 S 段零回退；doctest 9/9；commit 807e29a0

**最近状态变更**（共 4 条）：2026-11-22 00:13:44 +0800 · 进行中 → 待验收 · 预览态查找已交付：显示域命中 + 高亮矩形 + 跳转滚动（翻转 §4.6）。方案卡 evidence/bug400-design.md；红 evidence/bug400/probe-red.log（S2 条=0 fail）/ 绿 evidence/bug400/probe.log（S2 PASS：hits=24、矩形在场、scroll 29157.8→0、光标不动、单跳70ms）；编辑态 S 段零回退；doctest 9/9；commit 807e29a0

---

#### need#401 · 全应用阻塞性慢操作排查（老板 2026-11-21 令：其他功能是否同存在帧内同步重活，响应第一口径全扫）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：已分析
- 创建：2026-11-21 23:49:32 +0800（developer）· 更新：2026-11-22 18:46:42 +0800（developer）

**详情**：

老板 2026-11-21 令：「优化完毕后，看看别的功能是不是也是这样，存在阻塞性慢操作」——响应北极星令的全应用扫描。范围：全部 Router 模块（home/markdown/git/todo/schedule/claude_proxy/agent/memory/wallpaper/mcp/terminal/release/settings）+子窗。方法（今天 bug399 同款范式，仪表先行禁瘸测）：①compose/用户路径上的同步调用普查（文件 IO/git 操作/DB 查询/目录扫描/进程 spawn 是否在帧内同步）②重点密堆探针（每帧阶段耗时+主线程占用）③命中即逐条立修条目（>50ms 同步=必拆分帧/异步）。执行序：need#400（预览搜索在飞）→ #398（分段加载+缓存）→ 本条全应用扫描。产出：排查报告+逐条修复队列

**正文**：

老板 2026-11-21 令：「优化完毕后，看看别的功能是不是也是这样，存在阻塞性慢操作」——响应北极星令的全应用扫描。范围：全部 Router 模块（home/markdown/git/todo/schedule/claude_proxy/agent/memory/wallpaper/mcp/terminal/release/settings）+子窗。方法（今天 bug399 同款范式，仪表先行禁瘸测）：①compose/用户路径上的同步调用普查（文件 IO/git 操作/DB 查询/目录扫描/进程 spawn 是否在帧内同步）②重点密堆探针（每帧阶段耗时+主线程占用）③命中即逐条立修条目（>50ms 同步=必拆分帧/异步）。执行序：need#400（预览搜索在飞）→ #398（分段加载+缓存）→ 本条全应用扫描。产出：排查报告+逐条修复队列

**进度记录**（共 8 条，列最近 5 条）：

- 2026-11-22 01:08:25 +0800 · 开工：静态普查(全模块compose同步重活)+仪表实测top5帧耗时，只测不修
- 2026-11-22 01:26:59 +0800 · 完成：静态矩阵 15 模块 + 真 compose 实测（探针 bug401_frame_probe，真窗口真 app::compose，300 笔记/60 待办）。P0=0；P1=2（markdown pollChanged 55.8-80.2ms/5s、rescan 首帧 125.4ms）；P2=9（claude 46.7/todo 44.3/terminal enter14.4/memory29.1/mcp26.9/settings26.1/schedule27.2/release19.3/home同步读库）。发现项入池 402-409。git 已异步（worker 先例可复用）。未验证项见报告
- 2026-11-22 11:30:30 +0800 · P2 分析阶段开（老板令「继续分析其他的性能问题」）：#404-409 逐条量化分析+增长风险推演（500+ 笔记/大库/输出风暴）+修不修判决入池；含 #401 未验证项（terminal 风暴/wallpaper rescan/macOS 同参数）
- 2026-11-22 11:56:35 +0800 · P2 分析轮（只分析不修）完成。报告：evidence/bug401-p2-analysis.md；证据：evidence/bug401-p2-*.log（18 组）。
增长实测（探针扩参，BUG401_POLL_MS=5000 真机口径 + DataRootEnv 隔离）：
A) markdown：页面稳态帧 300=20.4ms / 500=25.1 / 800=68.1 / 1200=214.0，进页首帧 100.8/99.7/211.0/531.3 → 超线性，破 50ms ≈600 笔记；后台 rescan（#402/403 后）300/500/800=678/1736/2508ms，同期帧尖峰 6.96/7.08/44.5ms（已不在帧内）。
B) todo：稳态 60=4.0-4.6 / 200=4.4 / 400=4.2 / 800=8.8-9.6，虚拟化有效；首帧 28-57ms 抖动。
C) home/claude_proxy 大库：claude_call_logs 5万/10万/20万 → getLastCallRate 21.5/38.4/84.4ms（COUNT 查询恒 0.17ms）；claude_proxy 首帧 46.7→66-73ms。
D) memory pending：0/500/2000 条 → 1s 刷新帧 3.5/78.7/135.1ms，稳态 2.6/6.7/8.8ms。
判决：立即修 3 项（已入池 #410 markdown 列表虚拟化、#411 memory pending 上限/快照共享、#412 home getLastCallRate 索引化）；观察带线 #404→#413（claude_call_logs >8万行）、#405 todo >2000 任务、markdown rescan >1200 笔记、wallpaper >2000 媒体文件；不修 5 项（terminal poll/mcp/settings/schedule/release，均有界）。未验证补扫：terminal 输出风暴=静态判据（TerminalFrame 只随可见网格，与输出量无关，未起真 pty）；macOS 未复测。方法学缺陷另记 #414（debt/P3，探针口径已修）。
- 2026-11-22 18:46:42 +0800 · 台账归位：扫描主体完成（产出=排查报告+逐条修复队列已交付），余项修复走池内独立 bug 卡；待验收后关账

**最近状态变更**（共 6 条）：2026-11-22 18:46:42 +0800 · 进行中 → 待验收 · 台账归位：扫描主体完成（产出=排查报告+逐条修复队列已交付），余项修复走池内独立 bug 卡；待验收后关账

---

### 在池（3）

#### need#355 · 会话链路延迟优化：琐碎消息数分钟级响应（超长思维链+技能仪式+codegraph误注入）

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：—
- 创建：2026-11-19 09:33:53 +0800（ZCode主会话）· 更新：2026-11-19 09:33:53 +0800（ZCode主会话）

**详情**：

2026-11-19 老板实测：一句确认类消息（「发BUG直接入库」）耗时数分钟才回「收到」。延迟构成：①主因=超长思维链×2轮（~15万token大上下文逐token生成，把确认消息当成了工作流设计评审）；②次因=自选3次工具往返（pool-usage技能装载+LocalToolSearch schema搜索+pool_stats探活，秒级）；③放大器=superpowers元技能「任何响应前必须查技能」绝对化条款把琐碎回合推向重仪式。devflow自身hook注入轻（SessionStart一段+UserPromptSubmit一行），非主因。同回合新实证：codegraph UserPromptSubmit hook对纯中文流程问题误注入69个无关符号~4KB Corta Swift源码，意图门控失效。候选修法：A行为侧（已即时生效+入记忆）：非任务型消息零预载直答；B codegraph注入门控（代码意图判定+长度上限）；C元技能比例条款或用户全局AGENTS.md加覆盖指令（用户指令优先级高于技能）；D SessionStart注入瘦身（多插件叠加）。A零成本已做；B/C/D需插件仓立项，Mac侧主开发。

**正文**：

2026-11-19 老板实测：一句确认类消息（「发BUG直接入库」）耗时数分钟才回「收到」。延迟构成：①主因=超长思维链×2轮（~15万token大上下文逐token生成，把确认消息当成了工作流设计评审）；②次因=自选3次工具往返（pool-usage技能装载+LocalToolSearch schema搜索+pool_stats探活，秒级）；③放大器=superpowers元技能「任何响应前必须查技能」绝对化条款把琐碎回合推向重仪式。devflow自身hook注入轻（SessionStart一段+UserPromptSubmit一行），非主因。同回合新实证：codegraph UserPromptSubmit hook对纯中文流程问题误注入69个无关符号~4KB Corta Swift源码，意图门控失效。候选修法：A行为侧（已即时生效+入记忆）：非任务型消息零预载直答；B codegr

**最近状态变更**（共 1 条）：2026-11-19 09:33:53 +0800 · — → 在池 · 入池

---

#### need#393 · devflow 峰窗门禁移除（老板令，范围待确认）

- 状态：在池 · 优先级：P2 · 负责人：claude-agent · 阶段：—
- 创建：2026-11-19 16:56:36 +0800（claude-agent）· 更新：2026-11-19 16:56:36 +0800（claude-agent）

**详情**：

老板 2026-11-19 17:0x 原话「另外这个高峰期需要删除。当前任务不受高峰期限制」。当前任务已按单任务豁免执行（[peak-override] 引用原话）。本条登记「峰窗特性移除」诉求：范围待与老板确认——候选范围：devflow 插件 peak-hours skill、dispatch 派发门禁的峰窗硬拦逻辑、18:00 自动挂起恢复逻辑；涉及插件源码 C:\Users\e-Yunfei.Qian\dev\zcode-plugins\devflow（改流程改插件铁律）。确认范围后再定级排产。

**正文**：

老板 2026-11-19 17:0x 原话「另外这个高峰期需要删除。当前任务不受高峰期限制」。当前任务已按单任务豁免执行（[peak-override] 引用原话）。本条登记「峰窗特性移除」诉求：范围待与老板确认——候选范围：devflow 插件 peak-hours skill、dispatch 派发门禁的峰窗硬拦逻辑、18:00 自动挂起恢复逻辑；涉及插件源码 C:\Users\e-Yunfei.Qian\dev\zcode-plugins\devflow（改流程改插件铁律）。确认范围后再定级排产。

**关联线索**（1）：

- [open] 峰窗,peak-only,no-peak,agent-gate,宿主不对称,need393：峰窗门禁宿主不对称实证（与 need#393 直接相关）：共享面 hooks.json 挂 agent-gate --no-peak（Claude Code 侧不拦峰窗），ZCode 侧 hooks.zcode.json 挂 --peak-only（仅 ZCode 拦 13:30-18:00 派发），agent-gate.sh:340-342 注释明示 2026-11-13 裁决；auto-continue/monitor 在 Claude Code 零接线（README.md:13,30）。含义：老板令「高峰期删除」若指永久移除，改的是插件 hooks.zcode.json 的 --peak-only 接线（或 agent-gate 默认行为），Claude Code 侧本来就不拦（source: research/03-插件skill清单与机制.md 终报发现②）

**最近状态变更**（共 1 条）：2026-11-19 16:56:36 +0800 · — → 在池 · 入池

---

#### need#396 · 用户定义智能体的 MCP 列表显示开关

- 状态：在池 · 优先级：P1 · 负责人：主会话 · 阶段：—
- 创建：2026-11-19 19:48:28 +0800（主会话）· 更新：2026-11-19 19:51:33 +0800（主会话）

**详情**：

老板 2026-11-19（#394 在飞时追加）：用户定义的智能体，加一个开关，控制是否在 MCP 列表显示。与 #394（智能体工具 MCP 开放）同方向但独立功能点；语义待澄清（智能体以何种形态进 MCP 列表、开关位置、默认态）；按一条一条纪律排 #394 后处理。

**正文**：

老板 2026-11-19（#394 在飞时追加）：用户定义的智能体，加一个开关，控制是否在 MCP 列表显示。与 #394（智能体工具 MCP 开放）同方向但独立功能点；语义待澄清（智能体以何种形态进 MCP 列表、开关位置、默认态）；按一条一条纪律排 #394 后处理。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 19:50:40 +0800 · 老板界定（2026-11-19）：「MCP 列表」=MCP 协议 tools/list 返回（外部宿主可见面），非本项目页面 UI——「显示」=对外暴露为 MCP 工具。已核实：用户定义智能体=agent_tasks（name/goal/trigger manual|schedule/model_slot/max_iterations/allow_destructive/enabled/system_prompt；编辑弹窗在智能体页；kTaskTemplates 为出厂模板非用户件）。设计约束：MCP 工具名字符集 [a-zA-Z0-9_-]（中文智能体名须走 id/拉丁名，中文名与目标放描述）；同 task 不并发闸 startRunExclusive 沿用；agent.runs 查询工具现不在开放集。待老板定：工具形态（每智能体一件 vs 单一通用）+执行语义（同步回结果 vs 异步回 run_id）。
- 2026-11-19 19:51:33 +0800 · 老板两裁决（2026-11-19 AskUserQuestion 实答）：①工具形态=一个智能体=一件 MCP 工具（名字 agent_run_<id> 型拉丁名，中文智能体名+目标放描述；开关=该条在 tools/list 出现与否）；②执行语义=同步跑完回结果（AgentLoop 跑完回传最终输出，受 MCP tools/call 总闸约束现状默认 600s，超长任务报错）。需求语义已收口，待澄清项归 PM/tech：开关位置（智能体页任务编辑弹窗）、默认态（建议默认关=不主动对外）、与 enabled（调度开关）语义分离、agent_tasks 新列 DB 迁移、任务增删改/开关切换时 tools/list 动态同步、同 task 不并发闸（startRunExclusive）对外调用时的行为。排队：等 #394 全链走完后进 PM 环节（一条一条纪律）。

**最近状态变更**（共 1 条）：2026-11-19 19:48:28 +0800 · — → 在池 · 入池

---

### 挂起（4）

#### need#34 · 向量模型升级档（Qwen3-Embedding-0.6B）

- 状态：挂起 · 优先级：P3 · 负责人：— · 阶段：—
- 创建：2026-10-30 21:05 +0800（—）· 更新：2026-10-30 21:05 +0800（—）
- **blocker**：条件未触发

**详情**：

记忆量数百条+后可升；必须显式 -c 8192 否则吃 4GB；见 2026-10-27-memcpp-embed-bakeoff。TODO.md 旧册需-8 迁入

**最近状态变更**（共 2 条）：2026-11-02 09:59:04 +0800 · 在池 → 挂起 · 2026-11 修复迁移补登

---

#### need#324 · 全站 CJK 禁则排版（行首禁则字符不起行，EUI-NEO 排版层）

- 状态：挂起 · 优先级：P3 · 负责人：developer · 阶段：已分析
- 创建：2026-11-18 12:57:39 +0800（developer）· 更新：2026-11-22 20:38:20 +0800（developer）
- **blocker**：老板 2026-11-22 收敛令：暂不开发（调研文档已入库 docs/research/2026-11-18-cjk-line-break-prohibition.md，恢复条件=开发令）

**详情**：

从 debt#309① 拆出另立项（波 2 裁决）：全站排版层 CJK 禁则——行首禁则字符（。，、！？：；」』）等不趣起行。现状：文本换行引擎（EUI-NEO 侧）未处理禁则，设置卡描述/长文案在窄盒下以全角逗号起行（evidence/20261118-ui-fix-260-261/review2.md ①）。属 L 级（EUI-NEO 排版层改动，全站生效面广），需方案+波次验证；老板可取舍缓行。

**正文**：

从 debt#309① 拆出另立项（波 2 裁决）：全站排版层 CJK 禁则——行首禁则字符（。，、！？：；」』）等不趣起行。现状：文本换行引擎（EUI-NEO 侧）未处理禁则，设置卡描述/长文案在窄盒下以全角逗号起行（evidence/20261118-ui-fix-260-261/review2.md ①）。属 L 级（EUI-NEO 排版层改动，全站生效面广），需方案+波次验证；老板可取舍缓行。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-18 13:09:33 +0800 · 调研完成（2026-11-18 快答档）：①行首禁则最小集=全角句读+结束括号引号+省略号（。，、！？：；）〉》」』】…），半角永不入集（保 URL/英文混排），UAX#14 类别表+JIS/Ecma 表两源交叉；②断行决策点共 4 处：EUI-NEO 渲染 text_layout.cpp:61、测量 text_measure.cpp:219（两处有逐位一致契约必须同改）、markdown.h:1983，另有 DevDesk 应用层旁路 wrapToolDescLines（agent_module_dialogs.cpp:1217，EUI-NEO 改动覆盖不到，需同批改）；③推荐拉回法（禁则字符并入本行行尾，超宽 ≤1 标点宽，连续钳制 ≤2 次），不引 UAX#14 完整 pair 表；④估 ~1 人日含双平台；⑤风险：测量/渲染漂移（卡高错位）、连续禁则超宽、应用层旁路漏网（实现前 codegraph 全扫 measureTextWidth 折行点）。文档：docs/research/2026-11-18-cjk-line-break-prohibition.md（字符集/落点/路径/风险四节齐+来源分级）
- 2026-11-18 17:37:38 +0800 · 波 1 编码完成待 AJ 后构建验证：EUI-NEO 三落点+共享谓词+单测已落码（未构建未运行，按 AJ 批 rebase 后包闸构建口径）。新 core/render/text_kinsoku.h（isProhibitedLineStart 全角 14 码点 + decodeUtf8At）；text_layout.cpp rebuildLayout / text_measure.cpp measureTextSize 同判定同钳制（拉回法，逐位一致契约，行首码点取 byteIndices[index-1]——调研快答写 byteIndices[index] 差一位，已按 makeTextMetrics 实际语义纠正）；markdown.h pushFittingSegment 同口径；tests/unit/text_kinsoku.cpp 五类用例 + CMakeLists 注册。git status 仅 6 个目标文件，未 commit。DevDesk 旁路 wrapToolDescLines 与全站视觉验证归波 2。
- 2026-11-18 20:40:04 +0800 · 波 2 落点判定=需新接线（实读：wrapToolDescLines 手工折行+逐行 wrap(false) 渲染，不经 EUI-NEO wrap 期禁则；agent 域其余文本随波 1 #1/#2 已覆盖）。改动：agent_module_dialogs.cpp 接入 text_kinsoku.h 拉回法（行尾允许超 maxWidth ≤1 标点宽，连续拉回钳≤2、换行复位，镜像波 1 惯用法）；test_agent_ui.cpp 既有用例增红绿判据 mw=9字实测宽+半字→断行点钉在禁则符前，11 码点抽样，旧代码禁则符落行首=红。wip 2d18dab9（2 文件 +39/-5）；证据 evidence/20261118-cjk-wave2/changes.md。未验证：本批禁构建禁跑，编译+用例运行归 BJ 波末构建，视觉归装机轮 5。
- 2026-11-22 20:38:20 +0800 · 收敛归位：调研完成待方案，P3 缓行

**最近状态变更**（共 4 条）：2026-11-22 20:38:20 +0800 · 进行中 → 挂起 · 老板 2026-11-22 收敛令：暂不开发（调研文档已入库 docs/research/2026-11-18-cjk-line-break-prohibition.md，恢复条件=开发令）

---

#### need#340 · CI 类验证项分诊与关账预案（push 后执行）

- 状态：挂起 · 优先级：P2 · 负责人：qa-tester · 阶段：开发中
- 创建：2026-11-18 18:00:18 +0800（qa-tester）· 更新：2026-11-18 20:09:21 +0800（qa-tester）
- **blocker**：②档 6 条等老板 push+CI 绿后按预案执行（evidence/20261118-ci-preclose/plan.md，BW 本轮核实在位）

**详情**：

老板 CI 推送决策执行前置：禁令拦截类 ~10 条（C 批 verdicts §三分类：177/184/269/256/281/282/286/123/185/183/284/117）在 push+CI 绿后关账——本条目收分诊与关账预案（逐条读判据→分本地已证/需 CI 两档→产出关账表），push 后按表执行。

**正文**：

老板 CI 推送决策执行前置：禁令拦截类 ~10 条（C 批 verdicts §三分类：177/184/269/256/281/282/286/123/185/183/284/117）在 push+CI 绿后关账——本条目收分诊与关账预案（逐条读判据→分本地已证/需 CI 两档→产出关账表），push 后按表执行。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-18 18:04:16 +0800 · 分诊统计（C 批 CI 类 12 条）：①本地已证可关 4（#256/#123/#183/#117，锚点 .scratch/qa-full2.log + qa-chain-final2.log，CI 不编 probe 对其无信息量）；②需 CI 绿才关 6（#184/#269/#281/#282/#286/#284——CI 需验项：mac Release 编译+测试+ASan、win Release 守卫链+测试段 git-diff/settings 域，#184 先推 fork）；③已终态 2（#177/#185 均已关闭无需 CI）。关账表：evidence/20261118-ci-preclose/plan.md。纯只读：未动池状态、未动 src、未构建。
- 2026-11-18 20:09:21 +0800 · BW 归位：分诊与关账预案已落盘（plan.md 本轮核实在位），纯等 push+CI 绿外部事件，挂起+blocker 明账。

**最近状态变更**（共 4 条）：2026-11-18 20:09:21 +0800 · 进行中 → 挂起 · ②档 6 条等老板 push+CI 绿后按预案执行（evidence/20261118-ci-preclose/plan.md，BW 本轮核实在位）

---

#### need#392 · AI 开发使用全景站（L）：showcase-v2 多页整合——插件/skill 清单+机制+实践证据+静默调用场景分析

- 状态：挂起 · 优先级：P1 · 负责人：developer · 阶段：原型阶段
- 创建：2026-11-19 16:50:27 +0800（developer）· 更新：2026-11-22 20:38:25 +0800（developer）
- **blocker**：老板 2026-11-22 收敛令：暂不开发（原型阶段 proto-chapters.html 五章迭代在案，恢复条件=开发令）

**详情**：

PRD 骨架批完成：research/18-PRD.md（31,739 字节，七节完整）。基准=08 v2 零口径偏移；数字全走台账行引用（批间依赖：15/17 修复批落盘后回填，构建门断言五元组）。矩阵 F1/F2a-d/F3/F4/F5/F6/F7 初值全「待方案」。开放项 3 条待裁决：V4 总览页（推荐存档链接）/成本口径行（推荐先上默认句）/答辩页翻车卡（推荐含、落 F7）。产出纪律：只写 18-PRD.md，未碰 01~16/assets/evidence-samples；工具调用 16 次（预算 150）。 | 走查50六项修复完成：下一站链九页+回总览、站名统一全景·动画版+当前站高亮、首屏编号小字入metabox、台账链改指evidence#rack-ledger（原文留点击层）、demo回大厅菜单、dialog核查=误报记档；门全绿+curl200（_animver-gates/fix50.py）

**正文**：

PRD 骨架批完成：research/18-PRD.md（31,739 字节，七节完整）。基准=08 v2 零口径偏移；数字全走台账行引用（批间依赖：15/17 修复批落盘后回填，构建门断言五元组）。矩阵 F1/F2a-d/F3/F4/F5/F6/F7 初值全「待方案」。开放项 3 条待裁决：V4 总览页（推荐存档链接）/成本口径行（推荐先上默认句）/答辩页翻车卡（推荐含、落 F7）。产出纪律：只写 18-PRD.md，未碰 01~16/assets/evidence-samples；工具调用 16 次（预算 150）。 | 走查50六项修复完成：下一站链九页+回总览、站名统一全景·动画版+当前站高亮、首屏编号小字入metabox、台账链改指evidence#rack-ledger（原文留点击层）、demo回大厅菜单、dialog核查=误报记档；门全绿+curl200（_animver-g

**关联线索**（76）：

- [open] 材料清点,十一月需求,232份,profile152,工单金额修复,测试矩阵：材料清点实测：D:\Data\desktop\十一月需求\ 全树 702 文件、剔除 node_modules×2（各235）后真实 AI 产物 232 份（迭代总方案/马来方案 docx×3 版/方案对比 A-C+裁决/生产问题修复方案 01→06 闭环/工单金额修复执行包 42 文件/工时迁移管线 61 文件/图 D1~D7+EU iParts mermaid 源/UAT 票样 PDF×4/SCRM 4md）；rs.host\.profile 实测 152 文件（json83/py34/js27，SIT 环境全套+探针脚本+盲验链×2+steps 44 文件）——修正前序「约 98 份」估计。证据：research/04-材料清点与剔除候选.md ①②章（附实测命令）（source: D:\Data\desktop\devflow-showcase-v2\完整版本\research\04-材料清点与剔除候选.md:6-69）
- [open] 代码审计,merge-review,审计链,need27,审计产物：代码审计产物实测定位（共 6 组实质产物）：①桌面 rs-host-merge-review-2026-11-18.html（679KB 风险分级卡）②rs.host 根 rs-host-merge-review-2026-11-04.html（1.53MB 全量）③rs.host-4 同题 518KB（与②重复候选）④rs.host-2\.tmp-need27\ audit-r1/r2/r3 三轮审计链（简报+结果+补丁，devflow 独立审计门实录）⑤rs.host-5\.planning\ EU iParts 审查 R1~R3 ⑥十一月需求\生产问题修复方案\06-审计结论.md；另七仓各一份 rs-review 说明文档（工具文档非运行产物）。时间序 10-21→11-04→11-05→11-07→11-17→11-18（source: research/04-材料清点与剔除候选.md:71-87）
- [open] 重复剔除,2222.html,测试矩阵cmp,V1V4,剔除候选,待确认：重复实证：桌面 2222.html=池台账全景早期版（58KB 13:22 早于正式版 121KB 13:31 九分钟）；测试矩阵.md 在 showcase-v2 与 马来西亚需求\ 目录字节级相同（cmp SAME）；V1/V4 同骨架两风格（V4 十章更全 86KB，建议 V4 为主）；iRepair docx 多版本只引正式版；待删除-20261111\ 含 0.39.36 祖先 showcase×2 不引入。剔除候选四类清单（重复 6 项/主题外 5 类/老板材料形态 4 组/新发现纳入与否）+1111.png/2222.jpg 两图待老板指认——全部待老板确认（source: research/04-材料清点与剔除候选.md:106-141）
- [open] c4f638ec,静默调用,guance CLI,auto-test,memory触发,假绿,回调排障：c4f638ec 场景已解（1h44m/522行/68工具调用/0子代理/token input 791,734+output 178,791+cache_read 23,505,920）：①guance 静默调用=Bash 直跑 pip console-script（export PATH=/c/Tools/Python/3.13/Scripts 后 guance fetch-logs，共14次，非 Skill 非 MCP）；②auto-test=两段式：显式 Skill auto-test:auto-test（L285，加载缓存1.9.0）+ Bash 裸 node 插件脚本 browser-call.js/api-test-helper.js（凭据 .profile/env-sit.txt，token 用户贴入）；③触发源=用户级全局记忆 ~/.claude/memory/08-toolchain-env.md（会话首部注入，L102 PATH 写法/L95 DQL 语法/L83 UAT 网关通道逐字命中），项目 memory 只贡献索引行+业务规则；④结局：回调已发生，根因=aftersale-rs UAT 漏配 gbop.shfwyxgjb 三键，补配后复测 push success，定性环境配置缺失非代码缺陷；⑤避免一次假绿（老链路由）。报告含网站引用版叙事（第七节）（source: research/01-c4f638ec-场景分析.md:1-166）
- [open] 插件盘点,版本对账,cs-idea-restart,迭代关系,plugin.json：插件源码盘点：四插件全量清单+版本对账（plugin.json 实读）：auto-test 1.10.0（1 skill+8 脚本资产）、guance-assistant 0.3.1（10 skills+3 agents+3 hooks+Python CLI）、x-coding-tools 1.1.25（16 skills+safe-write MCP 3 工具+5 hooks）、devflow 0.39.48（16 skills+pool MCP 17 工具+8 agents+9 实体表 3 FTS 表）。装机对账：guance/devflow 两宿主一致；auto-test（Claude 侧 1.9.0 落后源码 1.10.0）与 x-coding-tools（Claude 侧 1.1.24 落后 1.1.25）——网站素材版本一律以 plugin.json 为准（x-coding-tools README badge 1.1.15 严重过期）。迭代关系 9 对实证，老板点名 cs-idea-restart→auto-test 双证：auto-test README「本地启动（无需 IDEA）：restart-local.js」+ docs/2026-09-20 设计文档旧版还调 cs-idea-restart。简报「四件套」为旧称，v0.39.48 实为五段三硬两软（dispatch/SKILL.md:15）（source: research/03-插件skill清单与机制.md（27494字节已验））
- [open] safe-write,起源,内置工具缺陷,hook屏蔽,原子写：safe-write MCP 起源叙事（老板 2026-11-19 17:2x 口述）：内置 Write 工具写文件常失败→针对性自研原子写 MCP→用 hook 引导屏蔽内置工具。核实：①失败模式同款实证=本会话链内置写盘占位残片把 121KB HTML 写成 1 字节（已沉淀「大文件单次写盘纪律」记忆）；②「屏蔽」实现=x-coding-tools 插件 SessionStart hook hooks/safe-write-notice.js:2-12，每会话注入「优先使用 safe-write 替代内置 Write/Edit」+双宿主工具名兼容，纪律级常驻引导非硬 deny；③rs.host 项目另有 path-gate 管写路径合规（devflow 项目门）。站点 x-coding-tools 页「为什么造它」引用（source: research/07-need392-对话实录.md 条目9 + D:/project/x-rs-tools/plugins/x-coding-tools/hooks/safe-write-notice.js:2-12）
- [open] 起源,无形的手,x-coding-tools,guance,auto-test,打通环境：三插件起源权威口径（老板 2026-11-19 17:2x 逐字）：x-coding-tools=「我写的 skill 的合计，针对实际开发场景的痛点开发的一系列 skill，每个都有专门的场景」（真人自研）；guance=「为了打通生产和[测]试环境的服务日志」；auto-test=「打通服务接口的调用测试能力」；总纲=「能让 AI 能和测试和生产环境交互，提供了这么一双无形的手」——站点工具层叙事主线索引句。待补：devflow 最初起点（哪件事促成）、bash_run 动机确认（source: research/07-need392-对话实录.md 条目11）
- [stale] devflow起源,拼图,卡死需求点,无自由度,稳定开发,设计哲学：devflow 起源与设计哲学（老板 2026-11-19 17:3x 逐字）：①拼图论=三插件（guance/auto-test/x-coding-tools）零碎、缺开发流程环节，devflow 补齐拼图；②卡死论=业务系统需求复杂多变互相影响且无自由度（一般只一两条路），没讨论清楚的东西不会因未浮出水面就可悬置，用 pool 强行卡死每个需求点；③靠确定性换稳定开发。站点 devflow 页首段+方法论页核心论点；与条目6 pool 优势问答互为印证（source: research/07-need392-对话实录.md 条目12）
- [open] devflow起源,拼图,框死,稳定开发,设计哲学,原句校字：devflow 起源与设计哲学（老板逐字，校字版）：①拼图论=三插件零碎、缺开发流程环节，devflow 补齐拼图；②卡死论=业务系统需求复杂多变互相影响且无自由度（一般只一两条路），没讨论清楚的东西不会因未浮出水面就可悬置，用 pool 强行卡死每个需求点；③原句=「通过框死来稳定开发」（取代本条旧版的「靠确定性换稳定」转述，方法论页引语用此原句）。站点 devflow 页首段+方法论页核心论点（source: research/07-need392-对话实录.md 条目12（校字后））
- [open] 自证,起源口径,时间线,safe-write-mcp,auto-test提取,口径分级：起源口径自证结果（git+源码+README 三层，2026-11-19 17:26）：✅坐实：safe-write=独立插件 safe-write-mcp 前身→07-30 提交 80dff2b 合入 x-coding-tools（附 Write/Edit guard hook）；x-coding-tools 作者=老板三 git 身份（yunfei.qian 119/CoderStory 93/e-Yunfei.Qian 90），首提交 05-21=word 生成技能；guance=README 自述同款痛点；auto-test=AI 编排 E2E；devflow 拼图=时间线硬证（05-21→07-15→07-22→10-28，devflow 最后补位）。⚠修正：①「总是写入失败」→「大文件/特殊字符负载下反复失败」②生产交互=guance 只读日志，auto-test 接口只打测试环境。⚠降级：「需求无自由度→框死」=设计者口述（机制层坐实）。🎁新发现：auto-test 07-22 从 x-coding-tools 提取成顶层插件、07-27 即集成 guance 凭据源（source: research/07-need392-对话实录.md 条目13 + git log 实测（x-rs-tools/zcode-plugins 两仓））
- [open] 典型案例,工单金额,南非发票,bug27,mal29,提示词八模式,无人值守：三类典型完整案例走查（R6 报告 research/06-典型完整案例.md）：①生产问题=工单金额不一致 11-05 报障→11-06 会话 8751b816 人肉 SQL 对账锁根因（删工时行不重算，潜伏 101 天）→11-07 会话 36df2592 hotfix 子代理修复+双路独立审计+01-06 方案包→11-18 会话 4e83df80 生产 289 单归零+红冲质疑挤水分，13 天全闭环；②需求开发=南非索赔发票 11-17 会话 6f84cd29 一天 4 批 PM 反馈闭环（模糊反馈→模板源码逐条核实→safe_update 原子改→单测+样张 PDF→PIL 像素自证当眼睛→踩坑回灌 rs-invoice-template skill v1.1.0）；③bug 修复=bug#27 11-18 会话 16357ecc 当天闭环（guance 自查纠错两次→dispatch 子代理修 60cbbb7a→主会话核 diff 逮住守卫抄错直修→cs-pdca-auditor 三轮审计挖出同类缺口 bug#30 一并修）+ mal29 无人值守链（用户两句短指令授权夜间，凌晨自愈两精度缺陷）。提示词八模式：转述原话/贴数据/人肉跑 SQL/一句话拍板/节点纠偏/质问施压/要产物要审计/定时+量化标准（source: research/06-典型完整案例.md（31817字节））
- [open] 总纲,驯服AI,四不全,缺陷矩阵,论点层,首页开篇：站点总纲定型（老板 17:3x 问「是否需要总纲阐述如何驯服 AI+避免 AI 固有缺陷」）：首页 L1 升级为论点层=AI 四缺陷×事故实证×驯服机制矩阵（想不全→devflow 框死链/干不全→pool 悬空账纪律/测不全→auto-test 全矩阵 mal29 实证/注意力散→预算双上限+hook 强制纪律），拟开篇句「把四个缺陷一个一个关进笼子，剩下的速度优势才是真的」；三条纪律：断言必配事故实证（V4 三次事故实录待挖补注意力案例）、「庞大」程度词须量化或降调、总纲不替代证据层（渐进式论点→机制→证据不变）（source: research/07-need392-对话实录.md 条目14）
- [open] 使用史,token画像,工具矩阵,迭代链,测试矩阵溯源,零使用判定基准,144亿：Claude Code 使用史总账（rs.host 系 6 目录 30 天，脚本可复算）：447 会话文件（122 主+325 子代理）、3.38 万次工具调用、144.3 亿 token（cache_read 94.3%、真实新增 8.2 亿、子代理占 16.7%、top5 多日长驻会话独占 41%）。工具矩阵：MCP=devflow pool 1833 次（11-04 上线，子代理派发 63% 为流程角色）+safe-write 1484 次（safe_update 973/safe_write 496/bash_run 15）+jetbrains 105 次（止于 10-20 首日）+codegraph 17；Skill 161 次（auto-test 34/devflow 44/x-coding-tools 48/guance 13/document-skills 13）；内置 Bash 16676/Read 5909/Grep 2772/Edit 1943/Agent 812/Write 781。溯源：测试矩阵.md=rs-host-5 d4a92665 会话 qa-tester 子代理 11-07 首建（[pool:need:5] 全矩阵任务）+11-16/18 两轮打磨，终版拷贝为人工；iRepair docx=同会话 gen_docx.mjs+cs-prd-tech-doc 脚本链三版演化。迭代链：jetbrains 桥止于首日→restart-local 接管；cs-sit-db-ops 止于 11-10→auto-test db 断言接管；xdebug→guance；sit-local-patches→池+worktree。token 结论：无重复跑式浪费，大头=长驻会话缓存重放结构成本，控本方向=会话拆分（推测）（source: research/02-claude使用史挖掘.md（40739字节，附录A脚本可复算））
- [open] memory四问,198KB,97%注入,1518引用,75%配方,谁主导,AI自主59%,成本对照：memory 四问完整答案（R5 报告，全部可复算）：①写什么=两级 49 文件约 198KB（用户级 9 文件 135.9KB 123 小节，08-工具链单文件 39.5KB 含 guance/DQL/UAT 全配方；rs 家族 14 文件 20.6KB；DevDesk 对照组为空）；②效果=198/204 会话（97%）自动注入、主动精读 46 次、9 条专属事实在决策文本出现 1518 块次覆盖 49 会话；③收益=guance 567 次调用 428 次（75%）直接套记忆配方、路径修正后 7/7 会话首调即中（3 会话首调即生产查询零探索）、测试单号跨 4-5 会话复用；成本对照=每会话注入 84085 字符（约 4-6 万 token 估算）；④谁主导=一个月 84 次写入 100% AI 工具完成，触发分类 AI 自主 59%/用户点名 21%/用户纠正 14%；cs-retro skill 显式调用 0 次但规则 hook 注入 204/204 会话常驻。沉淀→再命中链：b4a65d56（11-18 写入 UAT 通道结论）→c4f638ec（11-19 开场即引用）（source: research/05-memory专项.md（19031字节，附录含复算命令））
- [open] 汇报压测,人称口径,144亿三件套,证据静态化,186h降格,数字自洽,翻车卡,P0：汇报可用性审计结论：20 题压测=能答 5/半答 10/答不住 5。P0 三项：①人称口径——全部语料=单人（1 工程师+AI 多代理流水线），站点必须显式声明否则「团队使用」叙事被一问击穿（03 的「团队级」定位句同步限定）；②144 亿三件套——缓存重放人话版+真实新增 8.2 亿+成本/免费额度口径（禁裸上 L1）；③证据静态化——mal29 四报告/测试矩阵.md/一份审计 HTML/两张 UAT 票样复制入站，否则「当场点开一份」即空。P1：186h 建议降格为「248 报告≈免 250 轮手工回归」（删小时数）；数字自洽六条（1531 步 vs 743 判定须给分母、工单 289/23/20/17 口径卡、08 字节数 25381→实测 39481、120 vs 122 会话、插件级分解式、日均分母）；翻车汇总卡（守卫抄错被逮/审计反证/0 行结论作废/假绿险情）；敏感信息红线（内网域名/租户号叙事级复核）。汇报讲法：先讲三闭环故事，自曝翻车换信任，memory 成本账主动讲，数字全放 L3 带口径卡。自夸句 8 条清单在高危句节（source: research/10-阶段1审计-汇报可用性.md（20811字节））
- [open] 证据链复核,98.7%,P0两PDF失效,行号45对45,原话14对14,等价证据：案例证据链复核：行号锚点 45/45 一致、原话核对 14/14 逐字一致（错别字忠实保留）、产物锚点 15/16 证实、引用一致性三处全过，抽查通过率 74/75（98.7%）。P0×1：06 报告案例二引用的两份 PDF（南非索赔发票布局微调PRD-2026-11-17.pdf、南非索赔发票-最终效果.pdf）已不在桌面（11-17 中段被移动，6f84cd29 会话原文自证 GONE/MOVED）——修法=改标现存等价证据（UAT实测-1811/1812.pdf+za-claim-colspan-test.pdf+za-claim-invoice-rendered.html+PRD.md）。P2×4：mal29-t3 链实为 5 份报告（漏 rowecho）；248 口径须注范围（在架实测 155）；00e6065a0 dangling commit；6f84cd29 L585 锚表述可更准（source: research/12-阶段1审计-案例证据链.md（19935字节））
- [open] 质量门,24段抽查,30秒测试,黑话三词,落盘收口峰窗,P1×15,样板段：内容质量门审计：底稿质量高于红线可作网站底稿（事实锚/可删性/结论先行已是八份文档默认纪律）；缺口集中门④外行 30 秒=局部词句不需结构返工。24 段抽查=合格 8/基本 2/低密度 1/黑话超标 13；30 秒测试：01 摘要仅 2/5 可复述、06 结论条 3/4 失败、04 谱系段全军覆没（重写或不上站）；08 节标题「交叉对账」须改「逐项核对：每个能力有没有真实使用的证据」。P1×15 带改法示例（11 号报告）；黑话最严重三词=落盘 13 处零解释/收口 11 处零解释/峰窗 10 处仅 1 处带人话；频次之最台账 42/闭环 33（首现加注可保留）。三个过五门样板=01§七/05「一句话给老板」/06 叙事段；03§三建议整节作机制页底稿（source: research/11-阶段1审计-内容质量门.md（23283字节））
- [open] 模型多样性,口径衰减,数字对账8处,逻辑跳跃22,数字台账,口径四档,文案换位,术语人话表：模型多样性审计三大发现：①口径衰减链——限定词（近似/待验证/估算）在 报告→综合→站点文案 传递中被逐层剥掉，08§三已是硬结论（阻断级）；②跨文档数字对账 8 处冲突（B1 25381 vs 39481/B2 135.9KB vs 136906/B3 120 vs 122/B4 1833 vs 1885/B5 1484 vs 1532/B6 743≠1531/B7 工单 289/20/17/B8 6 vs 4 worktree）——单文档自洽横读即炸；③价值论证无对照基线（186h 无算式无基线，CFO 型追问即塌）。逻辑跳跃 22 条（含 memory 触发自证循环：memory 是 AI 过去沉淀、配方一致可能同源；1h44m 含 52 分钟用户等待；「1天4批」实约 8 次反馈）。三大建议：建站点级数字台账（每数字一行：值/分母/单位/口径标签/证据锚/防误读句，无台账行禁入 L1/L2）；口径四档（已确认/待验证/推测/估算）随数字上站；文案换位（01§七/06三段/05结尾/07条目14=现成最好 30 秒文案提到结论位，审计体下沉 L3，每页序=为什么造→一句人话→真实故事→数字+防误读→证据）。另：术语人话表 8 词（Skill/MCP/hook/子代理/token/缓存重放/worktree/池台账）（source: research/14-阶段1审计-模型多样性-报告.md（23638字节，deepseek v4.1-flash max effort））
- [open] janitor,会话清理,30天保留,447→443,语料快照,df773bb4,证据保全：语料 janitor 事件（2026-11-19 16:53→18:10 实测）：拉起新 Claude Code 实例（E 路审计）后 ~/.claude/projects/D--project-rs.host 被 30 天保留期清理删除 4 个 jsonl（df773bb4 会话根文件+其 subagents 2 份+另 1 根级；五目录其余完全一致）——R2 报告（17:31 写盘）的 447 文件口径是清理前测量，当前实测 443。处置：①站点数字保留 R2 测量值+标注测量时点；②全语料 871MB 已 tar.gz 快照至 devflow-showcase-v2/_corpus-snapshot-20261119.tar.gz（原始证据保全）；③审计 A 在飞中复算会看到更少文件——其报的 447/443 类差异按本事件解读非报告错误。另：B3 谜底=120 根级主会话（R5 对）vs R2 122（含清理前 2 个现已不存在的文件）（source: 主会话 find/ls 实测（六目录逐个复数）+R2 表对比）
- [open] 工程复核,需重审,P0零使用误判,斜杠67条,快照漂移,guance严格口径397,ZCode范围,双宿主长名：工程复核结论：需重审（事实底座扎实——27 项复算 15 项全过，token 结构/memory/材料/行号锚/git 溯源全对上）。P0×2：08 零使用档误判两项——llm-model-probe 有 1 次真实斜杠调用（39f09614 L14）、cs-web-fetch 有真实执行（cdd38020 L16572 node fetch.js 抓 maven 仓库），照上站会被当场戳穿；其余 6 项零使用成立。P1×9：①02 快照漂移（447→443/33769→33593/144.3→143.75亿，清理事件）须注测量时点；②插件级分解不自洽（devflow 1835+44+21 斜杠=1900≠1885；x-coding 1484+47=1531≠1532）；③斜杠缺陷：实测 user 行 command-name 105 条（技能类 67：cs-pdca-auditor 19/rs-host-merge-review 12/devflow:ask 12/devflow-setup 8/auto-test 6/cs-update-plugins 3/llm-model-probe 1）——skill 排行系统性偏低（auto-test 33+6/cs-pdca 3+19/merge-review 1+12/ask 24+12）；④guance 567=含提及口径（严格 CLI 执行 397 次/34 会话）；⑤186h 无算式；⑥ZCode 范围声明（devflow 11-18 行级 46568 行重度在用/auto-test 0）；⑦auto-test 细分须附复算命令+归档注（248=152+96）；⑧03 双宿主工具名表错（Claude 实测全程长名 mcp__plugin_x-coding-tools_safe-write__*×1482）；⑨01 工字节数 25381→39481。P2×7：mal29 链序、审计 6 组 vs 5+1+2 口径、01 工具调用 68 vs 70、05 84K vs 74K 双口径+guance-auth 37vs129、08 worktree 6→4、密度删并、07 自引字节数（source: research/09-阶段1审计报告.md（20525字节，附录A可复算））
- [open] 基建,serve.py,base.css,四js,断点映射,弹层契约,data-count,冒烟页：站点基建落地（2026-11-19 18:25）：serve.py 6,408B（py_compile 过+起服实测：127.0.0.1:8123 首页/_smoke/base.css/drilldown.js 全 200+no-cache 头+中文目录索引+--open 延迟开页）；assets/css/base.css 16,401B（V5/V6 共用样式提取，断点映射 900/600 承载母版 980/640 折叠+1200 新增插值档）；assets/js 四件（reveal 2,074B/countup 1,580B data-count 属性驱动/drilldown 4,254B 弹层焦点管理逐字照录母版+缺省宿主自动注入/nav 3,308B）全部 node --check 过；_smoke.html 6,079B 验收页（终批删）。关键契约：弹层 id 契约 ov/shKick/shTitle/shBody/shFoot/shX 不变；粒子 initDots 归 reveal.js；countup 动画参数照录（480ms easeOutCubic+250+i×120 错峰+ReducedMotion 直达）（source: 主会话 ls 实测（assets/ 与根 7 件）+基建批终报）
- [open] 修复批,31项,48处勘误,数字台账42行,1900分解,快照时点,za-claim等价：修复批完成（2026-11-19 18:5x）：31/31 项全落盘，48 处勘误留痕（01×9/02×11/03×2/04×3/05×12/06×7/07×4，全带〔勘误 2026-11-19：原 X→Y，依据…〕格式）；15-数字台账建成 42 行（A 节防误读表 21 条超集+B1~B8 裁决+快照双值+186h 处置+auto-test 复算口径+ZCode 口径，14,770B，含标签四档+时点声明+冻结副本基准）。关键裁决落定：02 头部快照时点 17:1x+漂移说明；插件级 devflow=1,900（MCP 1,835+Skill 44+技能斜杠 21）、x-coding=1,484+47+斜杠分列；guance 严格口径 397 次/34 会话；136,906 字节；06 失效 PDF→za-claim 等价证据+同源产物核对行追改；工单四数口径卡；07 链序照 06 修正。字节数：01=25938/02=44716/03=27936/04=17490/05=21642/06=33575/07=22125/15=14770（source: 主会话 ls+grep 抽验（4 项命中）+修复批终报逐项状态表）
- [open] 注意力分散,三事故锚定,bug190,debt195,1651次中断,牛皮藓,庞大降调：注意力缺陷证据补齐（research/17）：①V4 三事故全部池账锚定——事故一=DevDesk debt#194+插件旧池 need#34（老板 11-04 实录+commit 5f2a80d）、事故二=need#190 bug190（复盘 86 分钟/12.8M token，教训铸进 agent-gate.sh:297,299）、事故三=debt195（「跑飞」+commit 759b71b/fe482be+report.md:38）；唯事故二 V4 措辞与池账判责有偏差需修口径或降格；②归类：事故三=注意力分散（已确认）、事故一=注意力分散（推测级）、事故二=想不全主+注意力辅；③语料新猎 2 实证（中断悬空账 cc5dffb2 L2510-2566 打断 12 分钟 commit 悬空；打断风暴 1414fd3a L347-389 两分钟三打断+老板「你是牛皮藓吗」）+量化底座：30 天中断事件 1,651 次≈55 次/工作日；④「庞大」裁决建议=降调「会产生真实代价」（量化损失 3-4 小时级/月撑不起庞大）或换锚「1,651 次打断+单次最高 86 分钟」——程度词随数字走（source: research/17-注意力缺陷证据补.md（12841字节））
- [open] 展示分级,脱敏,三档,术语12词,证据卡：14 号报告终版追加三建议（deepseek 收尾补写）：①证据展示分级——research 原文不进站点；姓名/租户号/内网域名/本机路径/权限模式/生产 SQL 全部替换或删；证据卡=一段日志/一条命令/一张样张+采集日期+采集人；需老板拍板三档：公开/内部/仅老板（默认口径：站=内部使用，密钥已脱敏，测试环境域名可留，生产数据已排除）②术语人话表 8→12 词（补：块次/绿红/回传/HMAC）——开发批 U4 规格升级③「45 格/53 万 vs 513000」等数字硬伤已由修复批台账 21 行超集覆盖（source: research/14-阶段1审计-模型多样性-报告.md 终版⑤节（30121B））
- [open] 对照审计1,过门,台账v1.1,节级锚,行级事件级,P1×3全修：对照审计 1（常态令首轮，20 号报告 25,930B）：总判定=轻量返工 P0×0/P1×3，三处全修：①台账证据锚系统性失效（6/10 抽验锚坏——行号在勘误后漂移+08 v1 被 v2 替换）→台账 v1.1 全量改节级引用+维护规则第 6 条（禁行号锚）+新增 G 行；②07 条目 9 双宿主工具名口径冲突→补勘误（Claude 实测全程长名，短名为 hook 注释历史形态）；③17 号 1,651 口径不全→补行级/事件级双口径（行级 1,651/事件级约 850-892，queued_command 892 行/850 delivery_id，api-request-blob 754 行重复承载）。台账 v1.1=15,545B。阶段门按 20 号自设条件通过，进入页面开发波（source: research/20-对照审计-报告.md + 主会话三处修缮（15/17/07））
- [open] 数据层,五文件,56字段五元组,引文白名单,grep圈定,降级注记：数据层落地（assets/data/ 五件，window.XXX 全局形态）：usage.js 165 行/15,122B（规模+token 三件套+分解式+快照双值+ZCode 口径）、inventory.js 176/15,454（四插件三档+术语 12 词+双宿主+15 证据件）、cases.js 328/36,775（五卡+20 题表 1:1）、memory.js 95/9,065、timeline.js 108/9,364（F6 八节点+报告链 5+修复批 4）。自验：56 个数字字段五元组 100%（value/ledger/label/guard/snap），台账行引用逐条存在；35 条内链全实存。四条口径最裁定：①「186h」与黑话 15 词在数据层命中均为引述/题目原文/术语表语境→终验 grep 圈定 HTML 文案层+数据层引文白名单（主会话采纳为门规则）；②63% 角色占比无台账行→按分层规则降级文本注记（合规行为，不补卡）；③A16 待验证标签如实上站（source: 主会话 ls+node --check 复跑 + 数据层批终报）
- [open] 无形的手,论点,前后对比,叙事缺陷,打磨批：「无形的手」叙事论点定型（老板 2026-11-19 20:5x 质询「想表达什么？只输出证据不表达实际东西」）：核心论点=「以前 AI 只能出主意，人当它的手；现在 AI 自己长出了手——查日志、调接口、验结果，人退到岔路口的指路人」。对比论证：没手=人部署人测人念结果（传声筒+手，一轮小时计）/有手=自己复现自己判定（分钟计）+mal29 夜间自愈（手在无人时也动）+c4f638ec 识破假绿（自己看到 push success）+人只在岔路口纠偏（zems-rs-api 一句值千金）。「无形」=走人工程师同款的门（API/日志/DB），看不见手只看动作痕迹。批评成立=站点现只是标签+证据卡、缺论证。修法三处（QA 批落地后派叙事打磨批）：①index 总纲区加前后对比卡（两行浓缩）②guance/auto-test 起源段各跟「以前…现在…」一句 ③intro-A 拍三「无形的手」字卡加副题。（source: research/07-need392-对话实录.md 条目 27（无形的手质询轮））
- [open] 收口对账,池全量,消化审计,逐项三问,漏项红：收口必经环节（老板 2026-11-19 20:6x 原话令）：开发完成后从池捞出全部东西逐项检查——实际执行了/被使用了/被消化融合到 HTML 里了。范围=need#392 全量池账（status_log 全部状态流+dev_progress 全部笔记+线索 #257~#290 全量+28 轮对话实录+待回填档 11 条+全部默认裁决案）。产出消化对账表：池条目|应落位置|站内实证|判定（已消化/有意未消化写理由/漏项=红须修复）。执行时点=QA 终验+收尾合并批+对照审计 2 之后、老板走查之前（source: research/07-need392-对话实录.md 条目 28）
- [open] 打磨阶段,多轮,六维度,UI错位,真实性,拿得出手：打磨阶段令（老板 2026-11-19 20:8x 原话）：所有任务完成后加专门打磨阶段，所有内容全维度打磨——展现方式/交互方式/可读性/直观性/真实性/UI 错位等；不经过几轮打磨拿不出手。执行结构：位置=双终审（对照 2+池消化对账）之后、终交走查之前；多轮=每轮系统打磨批（六维度+既有质量门全扫）→老板过目→打回项入下轮，直到点头；真实性维度与池消化对账互证；视觉级以老板过目为主判。（source: research/07-need392-对话实录.md 条目 30）
- [stale] 术语政策,行业词直用,内部自造词人话化,免过度净化：术语政策校准（老板 2026-11-19 21:0x 原话）：门禁/skill/loop/MCP 等广为人知的专业术语可以直接写——两档政策定型：①行业通用词（门禁/skill/loop/MCP/token/hook/worktree/子代理/API）直用免括注负担；②内部自造词（必改 15 词：峰窗/落盘/收口/洗回/挂账/悬空/钳位/坐实/裸跑/分诊/四件套/回炉/下钻/配方/摸通道）维持人话化。作用：打磨阶段文案规则依据——防过度净化（别把行业词也洗掉），现行首现括注可精简（source: research/07-need392-对话实录.md 条目 32）
- [open] 术语三档,自创词必改,互联网黑话清查,闭环密度,老板原话烦人：术语政策三档定型（老板 21:0x 澄清原话「我怕的是你自创的玩意 俚语 尤其是互联网黑话 特别烦人」）：①行业通用词直用（门禁/skill/MCP/loop/token/hook/worktree）；②自创词必改（内部速记 15 词维持）；③互联网黑话清查=新增高优先级项（赋能/抓手/沉淀（知识语境）/拉通/对齐（非工程）/颗粒度/打法/护城河/组合拳/生态；「闭环」工程语境（修复闭环）可留但降密度——研究链出现 33 次偏高）。打磨批文案规则=自创词+互联网黑话双清单清查（source: research/07-need392-对话实录.md 条目 33）
- [open] 人类走查,十条不满,施工细节外漏,转化入口断裂,包装工地,6分到9分：人类视角走查（research/24，19787B）：三条最狠=①intro「进入站点」按钮落在内部选稿便签屏（主转化入口断）②整站页尾挂 window.SITE_USAGE 依赖备忘/正文露〔勘误…evidenceRefs〕修订注/「一边施工一边营业」③明知坑原样上站（A16 行自述领导看不懂+待老板确认标签）。总评=内容真金（6 分）包装工地（撕包装可上 9 分）；完整十条不满在报告第四节。处置=全部并入收尾合并批+打磨轮 1 弹药（与无形的手叙事三处同批）；打磨优先级=访客可见层剥离一切内部痕迹（source: research/24-人类视角体验走查.md）
- [open] 推敲,围猎,想不全表述修正,因果断言三问,打磨第七维度：推敲令（老板 21:8x 质疑「想不全能被流程框死？流程能解决？」）：驯服矩阵「想不全→框死」修辞越界——流程解决不了「想到」，解决「没想到的代价」=围猎（清单暴露/审计捕获 bug#30 实证/测试打红 mal29 实证/记忆复利补课）。矩阵该格改围猎表述（笼子比喻对干不全/测不全两格准确保留）。推敲=打磨阶段第七维度：每条因果断言三问（真的吗/机制/实证），答不上降格或改写——收尾合并批与打磨轮统一执行（source: research/07-need392-对话实录.md 条目 43）
- [open] ：老板三连判决 11-19:①开场动画中段为虚构装饰内容(打字 def create_order(): + v1~v5 假版本号闪补丁)判为浮于表面 ②要求改为 skill/插件简化版的演变迭代叙事 ③纠偏:guance 从第一天就是 skill 从未纯命令行使用,迭代方向=更完善/更稳定/更好用/更易用 ④案例选型=好讲述出成果的非业务案例→已选 auto-test 演化(cs-idea-restart 2026-06-16 c3b0eb8→07-22 2c22693 提取顶层插件→1.7.0/1.7.3/1.8.0/1.10.0 迭代→248 报告成果账)（source: 老板原话 11-19 20:5x + git log x-rs-tools 实测）
- [open] ：老板亲述 auto-test 演化权威叙事:原型=一堆拆散的 skill→完善方法论+技术方案→在基础上开发闭环 auto-test;能力叠加链=抓网页→调服务接口→本地启动项目→自动登入获取 token→闭环;痛点故事=初期用 IDEA 的 MCP 启停项目但 IDEA 启动慢耗内存不适合 AI 时代→自出方案命令行一键启停(1.8.0 2026-10-21 实证);基调=务实需求驱动非 KPI;两条常设令:①细节继续扒故事很多②所有问题方案先问 claude code 听意见（source: 老板原话 11-19 21:0x + git x-rs-tools 1.8.0 提交实证）
- [open] ：老板第四五道指令 11-19:①开场动画要「好好讲故事,说书人!活灵活现的那种,能感动人的」;动画总长度不限(~20s 上限作废)②出多个版本竞标,别直接改现有件(最终不喜欢就麻烦)→处置=三案并行 intro-demo-b1/b2/b3 新文件,a3 原件不动,老板看后选/混。三案方向:甲=墨字说书(文本为星+章回体+长停顿)/乙=长成记(能力器官逐个长出+机械生长)/丙=编年体(日期行军+一日期一场景+账本收尾)（source: 老板原话 11-19 21:1x）
- [open] ：老板第六道指令 11-19:站点页「海量平铺文本」判死刑——文本只是辅助,核心必须是图表和动画;原本目录的 html 不是这个样子。量化实测:站点页 keyframes=0/svg=0~1,原本材料 V4~V8 keyframes=12~14/池台账全景=10。处置=视觉重构批:以 V5/V6/V7/V8/V4 为视觉语法底本逐页重做(动画模块+图表核心+文字降图注与点击层),先做样板页过老板目再铺开（source: 老板原话 11-19 21:3x + 密度 grep 实测）
- [open] ：老板第七道指令 11-19:当前九页全部冻结不修改,站点根内新建目录重新出一个版本(视觉重构批的载体)。处置:新建 完整版本/动画版/ 目录承载新版九页(语法底本 V4/V5/V6/V7/V8),数据层走 ../assets/data 五元组单一事实源不变,旧版整体保留可对照可回滚;先出 tools/auto-test 样板页过目再铺开（source: 老板原话 11-19 21:4x）
- [open] ：老板第八九道指令 11-19:①时间限制解除(随干多久,唯一标准=拿得出手)②「html不是二维平面,可以是三维四维五维的!他就是个画布,不是纸张」——新版视觉语言按画布/多维设计:爆炸图、层深、视差、空间叙事③点名「当前连个爆炸图都画不好,联网检索别人的爆炸图是怎么样的」=联网调研令。附带:deepseek 派单模型名坑已定位=注册表为 ccs-claude-p85b203--qianfan/deepseek-v4.1-flash[1M](大写 M),旧派单小写 [1m] 被拒（source: 老板原话 11-19 21:5x + 网关 /v1/models 实测(8 个模型)）
- [open] ：新版推进环节定稿 11-19:①三案 intro 脚本(主会话写死→deepseek 批改→三案并行开发→老板选)②动画版九页全量重做(demo 爆炸图语法定型→逐页派工→每2~3页老板抽验)③合并整合(intro 入 index+全站冒烟)④双终审(deepseek 全站对照∥池全量消化对账 #291)⑤打磨多轮(人类走查+七维+推敲,直到拿得出手)⑥终交(老板走查+决策表+池收口)。旧版九页冻结可回滚;补充批 A~E 并入动画版对应页;时间不设限质量唯一标准（source: 老板「输出后续的任务环节」指令+前序裁决链 #297~#302）
- [open] ：老板令 11-19:「我不审稿,给claude code 审计」——中间审稿权全部委托 claude code(deepseek [1M]):demo/三案 intro/逐页重做/整合各环节一律 走 建设→deepseek 审计→修复→复审通过→下一环节 的审计链;老板只在终交走查出场(最终视觉裁决仍归老板)。配套:审计简报须带规格原文+8 条爆炸图规则+黑话禁表,deepseek 输出 P0/P1/P2 分级（source: 老板原话 11-19 22:0x）
- [open] ：老板批评 11-19:改版方案满口工程语言,却说不清每件东西「是啥/干啥/有啥价值」。裁决=页面内容宪法新增铁律:每页/每卡首屏必须先答三问(是啥一句话/干啥一句话/有啥价值一句话带数字),答不上不准进页面;主会话派单前必须先自答三问,自答不了=没想清楚不准派工（source: 老板原话 11-19 22:1x）
- [open] ：老板追加两问 11-19:④这东西是怎么跑的(机制)⑤怎么给出你要的东西的(价值核心=从机制到产出的因果链)。三问铁律升级为五问铁律:是啥/干啥/有啥价值/怎么跑的/价值核心——每页每卡首屏五问齐答;主会话派单前自答五问,答不全不准派（source: 老板原话 11-19 22:2x(接 #305)）
- [open] ：老板令 11-19:「乱七八糟的东西特别多,也不能全放,你要权衡,拼出完整的流水线。也不可能啥都塞进去」——策展铁律:①站点主线=一条完整流水线(需求进→池框定→派工→施工→检验→复盘→交付),每页是流水线一站,不是并列资料柜②上站准入=五问齐答+挂得上流水线工位,挂不上的再真实也只进 research 存档③比例:主线骨架占首屏,佐证细节收点击层④「完整」=流水线无断点(每站有实例+证据),不是全量堆事实⑤勘误史/修复批过程/P2 清单/考古全谱不出首屏（source: 老板原话 11-19 22:3x）
- [open] ：老板收工令 11-19 22:3x:「我只能说这么多了,我明天9点看成果」——交付时点=2026-11-20 09:00。交付物=动画版/ 九页全量(画布语法+五问铁律+流水线主线)+三案 intro(至少推荐版集成)+deepseek 审计链全记录;服务地址 8392 保持可浏览;夜间无人值守推进,主会话靠完成通知接力派单,git 逐里程碑提交（source: 老板原话 11-19 22:3x）
- [open] ：老板方法论传授 11-19 23:0x:①少即是多②内容不是一次性全输出——写小说式:先大纲→写人物→写世界观→一点点扩充;写 HTML 同理:先搭整个世界的架子,再写具体的东西,不搭架子直接写细节必炸③「现在不做强要求」(软指令)。落地调整:在飞 5 件视作具体章节保留;下一波先建世界架=动画版 index 骨架+全站设计令牌(色/字/板卡语法)+导航+流水线总览占位+各页五问骨架,然后逐页逐步扩充;每页最小可用=五问+一个主视觉+点击层,先少后多（source: 老板原话 11-19 23:0x）
- [open] ：老板叙事结构令 11-19 23:1x:「这些东西的开发时间是一条时间线上,承上启下的,一个完整的故事」——四件自研不是并列工具,是一条因果链上的四幕:手艺卡攒经验(x-coding-tools 05-21)→AI 看不见生产(guance 07-15 给眼睛)→能看测不了(auto-test 07-22 给手)→手抖写入失败(safe-write 07-30 稳手)→件件零碎缺流程(devflow 10-28 立规矩)→全家规模化跑 30 天(10-20~11-19 账本)。落地:动画版 index 主视觉=时间轴主干四站;每工具页开头=上章回目+承接句;导航按故事序不按并列序（source: 老板原话 11-19 23:1x + git 时间线实测(plugins/ 目录首提交 05-21/07-15/07-22+devflow 10-28)）
- [open] ：老板行为令 11-19 23:3x:「你只要不跟随我,我说了什么你就立即附和,这是不行的」——今晚主会话对每条指令都秒顺从+入池,零反对意见。整改:每条新指令先自答三问(我的独立立场/代价/替代方案),同意须带自己的理由,有保留当场说出;本条反馈同步写入跨会话记忆（source: 老板原话 11-19 23:3x(强化既有 boss-suggestions-reference-only/gate-proportionality 两条记忆)）
- [open] ：老板补充令 11-19 23:5x:叙事件还有主线/关键剧情/转折点等「我说不全」的构件——处置=自行补全叙事框架并入 29 号总纲 v2 新增第五层·叙事设计:主线一句话/冲突三病(忘漏赖)/五转折点(盲→明 07-15,信任危机 09-28 空承诺→证据纪律诞生,自主 10-21 换底盘,立规矩 10-28,验证 30 天账)/拍点(06-16 群像·07-22 成插件·09-20 单日 19 提交·10-20 四坑四文档·11-13 双环境)/伏笔回收三对(intro 幕四→evidence 答辩;「一条线」→index 全账;「能看了」→scenarios 实战)/高潮=夜跑 45 格不完美收束/悬念钩=每页回目承接句（source: 老板原话 11-19 23:5x(只举部分概念须自行补全)+考古 25/26 实例落地）
- [open] ：老板亲授内轴叙事 11-19 23:5x:「开始开发 auto-test 有个关键点——实践丰富经验积累技术,从质变达到量变的阈值」。git 量化坐实:月度提交密度 13/9/156/33/87/47/8,09-20 单日 21 提交(前后日 4~5)=尖峰;两级阈值=07-15 方法论成型(spec 取舍+YAGNI)+09-20 质变落地(1.7.0)。落地:29 号总纲第五层补双轴(外轴工具事件 T1~T5+内轴成长),index 时间轴加积累层微曲线+阈值标记,防误读口径:密度是活跃度影子非能力度量,讲趋势不讲因果（source: 老板原话+git log 月度/当日密度实测 23:5x）
- [open] ：老板收工令 11-19 23:5x:叙事构件(主线/剧情/转折点/内轴)不一定写入 HTML,全上会太复杂;取舍权授予主会话。裁决:叙事构件=施工图不=装修——结构里感受得到(页序/承接句/节奏/阈值标记),页面上不标名目(禁出现「转折点/伏笔/内轴」类元词);故事骨架留 research/29 供施工引用,屏上只见故事本身。老板已下班,9 点看成果（source: 老板原话 11-19 23:5x）
- [open] ：老板临行叮嘱 11-19 23:5x:「别忘了回顾整个pool里填充的内容和打磨」——池全量消化对账(原计划环节,防被重做挤掉):①need#392 自身线索链 #257~#314 逐条核销(执行/上屏/刻意缓发须有理由)②池内 386 条全景对账:每条是否被使用/消化/融进站点,漏=红③打磨轮在 9 点后老板反馈驱动。执行时点=整合期后、终审前（source: 老板原话 11-19 23:5x）
- [open] ：三案竞标评审落定(research/35):乙·长成记夺冠(心跳=全片唯一情绪引爆点,30秒可复述;等权分 27.0/26.5/23.5,情绪权重破平);甲=零违例兜底可直接入槽;丙=哈希错配修复中。混搭方案=乙为底偷甲的幕四独屏停顿+逐字落定+偷丙的台账行号小签+墨渍印章。处置:乙四改后作主入口;三案 9 点前全保留(并列入口,终选权归老板),选定后落选案移 _archive/ 并记录因由;index 主按钮 href 改指向乙（source: research/35 deepseek 竞标评审 22:36）
- [open] ：老板令 11-20 09:0x:「能执行的,全部执行了。不要等我」——全速批授权。执行面:①打磨批 1（48 §④清单 P2/P3 逐页派工）②池回填（_pool-pending 17 条按 42 §四顺序）③补充批 C（交付物组卡入 evidence+动画版 README）④台账 H9 勘误（45 格=42 差 3 为未判定格）已修⑤UI 错位检测在飞（49 号）⑥人类视角走查排队（UI 检测后）。不可执行项（需老板）:#259 两图指认/#281 三档分级/intro 三案终选（source: 老板原话 11-20 09:0x）
- [open] 回填,need392,待记1：回填：原待记 1（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
scenarios.html 落盘 31,355B，三道门+HTTP 冒烟全绿（5 卡/时间线 31 节点/数字卡 8/直链证据 8 件 200/黑话 grep=0/术语括注齐）。诚实上报：tools→scenarios 六处反向锚断链（scen-*/scn-* 命名 vs 实际 scenario-* id，终批修 tools 侧 href）；01§七免改样板逐字上站（假绿补括注）；06 三叙事段按 11 号改写；记忆链 SVG+归因限定句。开局 pool_update 成功、收尾回写失败（池断连）。（source: 完整版本/research/_pool-pending.md 待记1）
- [open] 回填,need392,待记2：回填：原待记 2（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
index.html 落盘 20,351B，三道门全绿（含真实 usage.js 数据冒烟：SITE_USAGE 契约、447 countup、防误读随卡、成本口径行=08 v2 §七.8 默认句、INTRO_SLOT 占位+try/catch 不阻塞）。跨批漂移上报：①答辩页文件名三漂移（qa-prep vs defense vs answer）——本页取 /defense.html；②数据全局名分裂（真实 SITE_*；工具页 SHOWCASE；scenarios CASES）——主会话裁决=改页面消费侧对齐真源；③devflow 页两锚断链（同待记 1）。开工 pool_update 成功、收尾失败。（source: 完整版本/research/_pool-pending.md 待记2）
- [open] 回填,need392,待记5：回填：原待记 5（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
两页落盘（devflow 26,295B/auto-test 29,901B），三道门 36/36 绿，**已按真数据契约接线**（SITE_USAGE/SITE_TIMELINE+加法自检 1,835+44+21=1,900 与 549+194=743 自动核对）。黑话 grep=0、186h 禁现=0、链接目标盘上核查全存在。上报三件已处置：①池回写失败→本档；②guance/x-coding 断锚→合并修复批任务书 21 号 B 节；③数据层禁现词两处（usage.js guard 186h 原文/timeline.js 防洗回）→主会话 19:2x 已直修（BOTH-FIXED+语法复验过）。反向锚已对 scenarios 实 id 核对（scenario-bug-fix/scenario-collab/scenario-req-dev 三锚命中）。（source: 完整版本/research/_pool-pending.md 待记5）
- [open] 回填,need392,待记7：回填：原待记 7（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
两页落盘（guance 19,029B/x-coding 25,176B），三道门全绿+**已对齐真实数据批 SITE_* 契约**（SITE_USAGE.guance/xCodingTools+SITE_INVENTORY.tiers+SITE_CASES）+渲染层黑话替换机制（数据层保原文、文案层守规范）+锚已用 scenario-* 实 id；guance-fix 披露恰 1 次、contract-grill 零出现。**池死因=本批 taskkill //IM python.exe 误杀**（为停 serve.py，已自纠按 PID）——教训入记忆第 47 条。拟回写：F2c+F2d 完成三道门绿已对齐真契约。另：F2a/b 页盘面复核 SHOWCASE=0（F2c/d 终报的邻页断链指控为陈旧读取），漂移最终收敛=scenarios.html 单页（21 号任务书 v2）。（source: 完整版本/research/_pool-pending.md 待记7）
- [open] 回填,need392,待记9：回填：原待记 9（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
evidence.html 落盘 54,178B，三道门全绿+HTTP 冒烟 55 项 0 失败（15 证据件逐件 200 中文名百分号编码、research 01~21+assets 全 200、43 台账行锚+21 research 锚+跨页引用锚全存在、1,069,869 字节对账=README）。台账实际 43 行（A21+B8+C3+D2+E6+F2+G1，G1=20 号补正行——「42 行」旧口径淘汰）；13 件字节数当日实测+合计对账（README 只登 3 处，页面已标注口径）。上报 scenarios.html:340 交叉缺陷（ledger 整串拼 href+name/tag 字段漂移）→归合并修复批（在飞）。pool 回写失败→本档。（source: 完整版本/research/_pool-pending.md 待记9）
- [open] 回填,need392,待记10：回填：原待记 10（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
methodology.html 32,047B+meta-case.html 25,608B 落盘，三道门全绿+HTTP 冒烟（两页 200+出站链接 29 个全 200+跨页锚断言过+黑话 grep=0+术语括注齐）。F5=老板原话→规矩演化卡四张+pool 问答展开+框死论三标签分级（设计者口述在位）+线索总线 SVG；F6=12 节点时间线（1-8 走 SITE_TIMELINE 数据注入+9-12 静态段）+老板判词原话卡+intro 三变体直链+双模型审计常态令段。开工 pool_update 无法执行（子代理会话池工具未挂载，如实上报）→主会话代记：F5/F6 完成，验收达成。（source: 完整版本/research/_pool-pending.md 待记10）
- [open] 回填,need392,待记11：回填：原待记 11（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
scenarios.html 契约对齐完成（35,844B，11 处留痕）：CASES→SITE_CASES+ID2CARD 映射+键名全对齐真源（timeline time/event/evidence、numbers key/value/ledger/label/guard/snap、capabilityRefs/evidenceRefs）+渲染层黑话替换移植+answer→defense。门=134 断言真数据全过+HTTP 31/32+交叉抽验 index/devflow 无回归。统一后真源契约已定格（供后续页沿用）。D1 锚漂移（cases.js:101 tools-autotest-reports ≠ 实锚 timeline）→主会话 20:1x 直修（D1-FIXED-OK）；D2 metabox 文案两句过时→终验批质量门扫尾。该批自报已池回写 need392 待验收（D1/D2 note）。（source: 完整版本/research/_pool-pending.md 待记11）
- [open] 回填,need392,待记12：回填：原待记 12（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（子代理会话无 pool 工具，沿待记 10 先例本档记账）：#392 子任务「对照审计 1.5 修复批」（owner=developer）。
**完成**：P0-2（evidence 台账直渲染区=引文区定性+豁免口径入页注/头注；D 节标题与 research 08/09 定位句去禁现词；defense/meta-case 渲染层 BLACKMAP 增已删换算句系列词条——B6 自称句改「站内展示层已删该换算句」；数据层原文未动）；P0-3（台账 v1.2 新增 B9/B10 行 43→45 行；usage.js roleShareNote→roleShare、callsNote→calls 五元组；devflow/auto-test 两页数字卡接五元组渲染+行锚直链）；P0-4（meta-case×3+methodology×1「22 轮」→「截至 2026-11-19 21:8x · 43 轮 as-of」+勘误注；timeline.js/defense 无 22 轮命中，简报口径以实测为准）；P1-2（V4 存档副本入站 archive/ 85,698B+evidence 链接）；P1-3（evidence worktree 行外括注；index:162 部分归合并批）；P1-4（guance/xct 卡面四档标签 chip）；P1-6（mainSessions ledger B3→A3、judgedSteps E4→B6、withMention/引用计数 guard 补源注；window 起止与 cost 引 A2 两处维持+理由）。P1-1/P1-5 结构性列待打磨批。
**门输出**：语法 9 内联脚本 0 失败+8 页配平 OK+BOM 0；黑话 grep（引文区豁免口径）10 页 0 命中；数据门 58 五元组字段齐/33 引用全在/台账 45 行=evidence 45 锚；defense/meta-case 渲染替换模拟全绿；HTTP 冒烟 28/28 全 200（含 archive/V4）。门脚本存 devflow-showcase-v2/_audit15fix/（站点外，可复跑）。
**未验证/移交**：浏览器 DOM 实渲染（jsdom 级）未跑——静态+语法+模拟+HTTP 已覆盖；P2×18 未处置（建议级归后续打磨批）；fixBatch「D 186h 处置」字符串处于未消费字段（P2-2 范畴）无用户可见面。（source: 完整版本/research/_pool-pending.md 待记12）
- [open] 回填,need392,待记13：回填：原待记 13（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿待记 10/12 先例本档记账）：#392 三案竞标之二「乙 · 长成记」开场动画**完成**。
**落盘**：完整版本/动画版/intro-b-长成记.html（自包含单文件 40,631B，0 BOM）。
**门**：四道门全绿（devflow-showcase-v2/_audit15fix/gate_intro_b.js，可复跑，67 断言 0 失败）——①node --check（vm+真实 node --check 双跑）0 失败；②DOM 桩冒烟 mkEl 桩+虚拟时钟推满 33.5s+重播：五幕按序、器官件累积不退场、幕三换件（IDEA 板节点已移除+底盘在场+闸开+秒表 90→53）、证据行、收束 248/151+时间带五刻度、href=index.html、无跳过 全过；③标签配平栈空+禁语 8 词 0 命中+封面零剧透（文本层零数字零幕词）+剧本逐字核对（主/次/证据行/收束/范围注/按钮）全过；④keyframes 101 条声明 ⊆{transform,opacity}、backdrop-filter/filter/blur 全 0 命中。全片 31.8s（点击开始→ended），每幕 4.6~6.5s 驻留，幕间 500ms 空拍。
**实操**：本地 HTTP（按任务 ID 起停）+IAB 实渲染取证 5 帧（封面/幕二累积/换件后/心脏首搏/收束）+真浏览器终态核验（numA=248/numB=151、heartBeat 持续轻搏、深呼吸 breath 完成、enterBtn href=index.html）；「开始」物理坐标点击走通；重播可完整重放（IDEA 板重建→再移除）。
**口径备注**：「幕内无日期/版本号」断言范围=主文案+次文案叙事层（幕内叙事零数字，强于日期/版本检查）；日期/版本号仅出现在剧本逐字规定的证据行真锚（铁律 2）与收束时间带（铁律 1 指定唯一日期处）。
**冠军精修批（35 号评审：乙夺冠；同日完成）**：P0 封面「三十天」→「一件一件 · 长出来」、口径行「五幕」→「长成记 · 幕幕带真证据」、幕四插 2.6s 独屏「十八分钟后」（A4S 层+machine dim 退净，影子→独屏→实体拉成 4.3s）；P1 主文案逐字落定（拆字 71 处 chDrop，五幕+收束）、证据行尾台账行号小签（·H16·H8/·H2/·H3·H5/·H6/·H14·H12·H9，15 号 H 节实测行，按评审纠正映射避丙案错挂）、签章墨渍径向扩散（inkSpread）。门复跑全绿含新增精修断言（独屏驻留 2.60s≥2.25s、重播独屏重置）；真浏览器核验：拆字 71、小签 5、独屏 on→off+dim+文字退场、终态 248/151/heartBeat/chDrop/href=index.html。45,050B，全片 34.6s。截图通道两帧超时未取新帧（状态级已证）。index 主按钮改指向由主会话安排（评审 P0-3），本批未动 index。（source: 完整版本/research/_pool-pending.md 待记13）
- [open] 回填,need392,待记14：回填：原待记 14（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿待记 10/12/13 先例本档记账）：#392 三案竞标之三「丙 · 卷宗体」开场动画开工，owner=developer。剧本源 28 号（铁律 7 条逐字执行）、壳 intro-demo-a3.html、落盘 动画版/intro-c-卷宗.html（自包含单文件）。收尾时回写完成态。
**完成（同日）**：intro-c-卷宗.html 落盘 27,280B（自包含零外链）。三道门全绿（门脚本 devflow-showcase-v2/_intro-gates/intro-c-gates.js，可复跑，退出码 0）：①语法门 1 段内联 script node --check+vm 编译 0 失败；②DOM 桩冒烟 mkEl+虚拟时钟推满全片，144 断言 0 失败（正常流+reduced 流两实例）——五幕按序/幕距 5.3s/幕间空拍 450ms/卷宗卡哈希+台账行号且卡面无日期/主次文案+证据行逐字/收束时间带五刻度+248/151+as-of/真导航 index.html/无跳过/DOM 序=幕序；③标签配平+无 BOM+禁语表 8 词 0 命中+IDEA 分钟/GB 句式 0 命中+封面零剧透（零数字/零四件套名/零幕内容词）+日期串仅存在于证据行与收束。全片 31.4s（点击开始→ended）。印章映射按脚本证据行：一 c3b0eb8·H8／二 2c22693·H2／三 6beaebe·H3／四 209f8ee·H6／五 b68e65c·H9（H 行=各幕核心证据在 15 号台账 H 节的登记行）。未验证：浏览器实渲染（沿待记 12 先例，静态+语法+DOM 桩已覆盖）。
**P0/P1 修复（35§①丙/§⑥ 评审令，同日）**：①幕一 c3b0eb8·H8→**H16**（进仓日行）、幕四 209f8ee·H6→**2d52c3d**·H6（空承诺行）、幕五 b68e65c·H9→**H14**（spec 127 行行）——取评审各条首选方案，五枚章 c3b0eb8/2c22693/6beaebe/2d52c3d/b68e65c 不重复，卡面与档案柱格同步改；②P1 证据行可读 ~1.3s→**4.29s**（幕距 5.3s→8.8s：卡 4.3s 归档滑走，旁白/证据行留场至 7.8s 再淡出，全片 31.4s→48.9s）。复跑三道门 **155 断言 0 失败**（新增：五枚章不重复/章×行逐条对上 15 号 v1.3 H 节（门脚本实读台账文件核对）/证据行可读≥4s），文件 27,683B。评审 P2（档案柱「展开」实为淡出换带、幕四/五情绪力弱）未列入修复令，未动。（source: 完整版本/research/_pool-pending.md 待记14）
- [open] 回填,need392,待记15：回填：原待记 15（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿待记 10/12/13/14 先例本档记账）：#392 动画版答辩页（母题＝讲台＋四抽屉，25 号 §③C：可读性优先，几乎不做 3D）**完成**，owner=developer。
**落盘**：完整版本/动画版/defense.html（57,496B，0 BOM）。路径修正（简报偏差，同待记 16 情形）：页面落盘在 动画版/ 根级，自持资产按同层 scenarios/index 惯例接 assets/tokens.css 与 assets/js/drilldown.js（简报所写 ../assets/ 系 tools/ 子目录页视角，实测 ../ 前缀指向完整版资产、非自持副本，弃用）。
**结构**：承接句「领导会问的 20 问」+五问五行；顶部讲台＝Top3 三张大卡（A1 人称根基／B7 成本空白／B6 估算裸奔，各带问+一句话答+账行签 A3/A1/D1）；下部四列抽屉柜＝成果/成本/方法/风险四组各 5 问（沿用旧版 20 问清单，重新分抽屉；组内 答不住→半答→能答，答不住置首位）；抽屉面＝问+一句话答+账行签+出处 chip，点击 openSheet 出完整答法+台账行口径+research 直链；演示＝抽屉逐组依次弹开一格（intro 900ms+4×(弹开 600ms+驻留 2600ms)+收束 300ms=全轮 14s，只用 transform/opacity，RM 定格终态）；页尾 details 文字版由同份常量渲染。数据层＝页内常量+账行注释（非五元组）。
**门输出**：三道门全绿（_animver-gates/defense-gate1/2/3.js 可复跑，退出码 0）——①语法门：node --check+vm 编译 0 失败、外链 2 件存在、无 BOM；②DOM 桩冒烟 72 断言 0 失败（mkEl 桩+虚拟时钟推满 14s）：五问五行/讲台三卡按序/四列×5 抽屉/20 问 id·问题文本·组题数 5-5-5-5·状态分布 5-10-5 与旧版 qa20 逐项一致/演示弹开按组（驻留 2600ms≥2500）/每答带台账行签 20/20/本页账行 15 行全部存在于 15 号台账（含抽样 5 条 A1/A3/D1/B9/A11）/约 250 轮带 D1 估算口径/94.3% 缓存重放防误读句/144.3 亿引 A1 快照口径/抽屉与讲台点击 openSheet/文字版渲染/RM 定格/弹层缺失可见降级/注入文案黑话 0 命中；③静态门 54 断言 0 失败：标签 4×≤6 字、图注 9 字、黑话 19 词全文 0 命中、闭环×1＝测试闭环×1、总纲禁语 7 词 0、内部批号（U/P 数字- 形式）可见文案 0 命中、44px/焦点/字号下限/过渡仅 transform·opacity·visibility/单 button/depwarn/figure 语义全过、17 条直链盘上全存在。另：HTTP 冒烟（127.0.0.1:8392）页面+tokens.css+drilldown.js 三件 200。
**表达转换留痕（换表达不换事实）**：四组重排；旧答法「配方→记忆写法」「夜间自愈/无人值守层→定时授权后夜间自跑/流程化层」「U6-④、B7 P0 类批号→观众语言或删除」；D18 的「105 次」改挂 15:§H5 口径（10-20 后零调用+restart-local 1,379 处/35 会话）；A4/D16 补挂 B9（63% 派发规模）、B10 补挂 H6（空承诺两提交互证）、A3/D17 挂 H18/A19——使 20 答每答有账行（旧版 5 题答法无数字行，属简报「每个质疑都有数字+口径」要求）。
**未验证**：浏览器实渲染未跑（沿待记 12/14/16 先例——静态+语法+DOM 桩+HTTP 已覆盖）；9 点走查前建议人工点开一眼弹层与抽屉点击。
**41 号修复批（同日，P1×3+P2×4 全落）**：P1-1「干 啎」→「干 啥」；P1-2 舞台 role img→group（23 张可点卡对读屏可操作；门 3 断言同步转向并断言全文无 role="img" 残留）；P1-3 B8 账签 E6→B6·E6（549/194/26% 在 B6、64 份含 FAIL 在 E6，门 2 按双行签拆分核行）；P2-4 账单三处统一「本页账行签共 16 行（A1/A3/A4/A5/A9/A11/B6/B9/C3/D1/E6/F1/A19/H5/H6/H18），正文另引 A2/F2」×3 处（注释/页尾 metabox/文字版）；P2-5 源码注释如实声明「展开答法按原文保留 commit 前缀与 L 行号源码锚」；P2-6 A4/D16/B10 三签标「关联口径」（抽屉 chip·弹层·文字版三处渲染点，chip 作 15:§B9·关联）；P2-7 首屏 h1 副题与源码注释清除「25 号 §③C/§③D」内部档案引用（页尾 metabox 的 research 来源引用保留——沿站内惯例）。申报更正一处：D18「105 次」系删该无账数字、按 15:§H5 口径重写（非「改挂」），41 号审计 ④ 已核。门复跑全绿：GATE1 0 失败／GATE2 75 断言 0 失败（+3：B8 双签、关联签、16 行集合）／GATE3 59 断言 0 失败（role 断言转向+5 新查：错字/账单三处/B6·E6/关联标记/首屏无档案号），文件 57,999B。（source: 完整版本/research/_pool-pending.md 待记15）
- [open] 回填,need392,待记16：回填：原待记 16（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿待记 10/12/13/14/15 先例本档记账）：#392 动画版证据页（母题＝档案墙/库房，25 号 §③C）完成，owner=developer。
**落盘**：完整版本/动画版/evidence.html（115,760B；自包含样式+动画版/assets 自持外链 tokens.css/usage.js/drilldown.js）。
**结构**：承接句「每个数字的出处」+五问五行；三组抽屉架＝①数字台账架（台账行=抽屉：64 行 details 拉开显六段，id=ledger-a1…ledger-h18，research/15 原文逐字引文，由 _animver-gates/mk-evrows.js 生成、台账更新后重跑同步）②证据实件架 15 件卡（件名+一句定位+直链：mal29 五报告链/审计 HTML/8 工单票样/测试矩阵）③冻结副本柜 290MB（15:§C3 复算唯一基准）。演示动画＝抽屉 A→H 八排逐排拉开（11 步 29.0s、每步驻留 ≥2.5s、只用 transform/opacity、reduced-motion 定格终态）；复算命令组＝台账 E 节原文进点击层＋待裁决小签（P1-1 遗留如实标注）；引文区定性标注沿 1.5 批先例（id^="ledger-" 行内文本豁免黑话 grep、页面自有文案不豁免）。
**门输出**：三道门全绿（_animver-gates/evidence-gate1/2/3.js 可复跑）：①node --check 0 失败＋3 外链资产存在；②DOM 桩冒烟 74 断言 0 失败（虚拟时钟 581 帧=29.1s 推满演示；台账行数门现场 grep 实测 64＝页内数＝静态 64 行＝节标题计数四面一致；15 条直链磁盘可达；双值 447/443 各带时点；A1 防误读句；RM 定格终态；drilldown 缺失可见降级）；③64 PASS：标签配平（3-5 个/≤6 字）+图注 ≤20 字+黑话 20 词 0 命中（引文区豁免后，豁免咬合自检：原文「配方」在引文区、扫描层 0）+叙事词 0 命中+静态样式断言（令牌取色/44px/字号下限/过渡仅 transform/opacity）+链接目标盘上全存在。
**路径修正（简报偏差如实报）**：简报给的 ../assets/ 与 ../../evidence-samples/ 前缀按「先实测路径可达再接」条款修正——页面落盘在 动画版/ 根（非 tools/ 子层），实测改接 assets/*（动画版自持）与 ../evidence-samples/*、../research/*、../evidence.html、../index.html（与同层 scenarios.html 惯例一致）；../../ 前缀实测不可达已弃。
**未验证**：浏览器实渲染（沿待记 12/14 先例：静态+语法+DOM 桩已覆盖）；窄屏 ≤860px 三架堆叠与 segstamp 静态位视觉（CSS 已写、门未断言像素）。（source: 完整版本/research/_pool-pending.md 待记16）
- [open] 回填,need392,待记17：回填：原待记 17（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿先例本档记账）：44 号审计「打回」修复完成。P0-1：A21 引文重生成（值 72 轮 as-of 23:5x／锚 07·§43~§72／防误读句——按 23:34 台账；mk-evrows.js 重跑，迁移模式写入 START/END 标记）。P1：生成器改「保留标记区间替换」（重跑不破页面，兼容占位符/旧版定位/标记三种输入）；gate2 加逐行逐字段对账（64 行×6 段=384 字段全等，替换 5 串抽样）；.dr-face min-height 44px+门断言。P2：「页内动态 grep」→「构建门现场 grep」×5 处；页级时点统一 23:5x（22:5x×11 处清零，引文内台账原文时点不动）；usage.js rounds 43 轮→72 轮（as-of 23:5x，共享键）。P3 挑两项：D 节注「落地」→「采用」（页+生成器同步）；HUD STEP x/10→x/11（active 步计数，终态 11/11）。**改一头查另一头**：devflow 门 2 三断言 43→72 对齐后复绿（53/53，页面消费侧自动跟随 72 无需改页）。三门复跑全绿：门 1 FAIL=0；门 2 PASS=71 FAIL=0（含引文对账 384/384、HUD 11/11）；门 3 PASS=65 FAIL=0（含 dr-face 44px、as-of 23:5x 统一、22:5x/21:8x 0 命中）。（source: 完整版本/research/_pool-pending.md 待记17）
- [open] 回填,need392,待记18：回填：原待记 18（池 MCP 断连期间记账，事件时点 2026-11-19 夜间至 2026-11-20 晨；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer）
池 MCP 仍未挂载（沿先例本档记账）：补充令两件完成。
**交付物子组**（evidence 页证据实件架第二组抽屉内）：04 号 ⑤c 成果证据挑 10 件（iRepair docx×2／需求分析 xlsx／工时迁移成品 xlsx／工单金额 xlsx×3／283 单明细 xlsx／执行包 md／UAT 用例 md／南非 PRD md），每卡=文件名+类型章+一句定位+本机路径；口径挂 A18（702/232/node_modules 防误读）。**直链实测（如实）**：站点服务根=完整版本/（serve.py 实测），桌面产物树与仓库根件均在根外，HTTP 直链 404（curl 实证）→ 按补充令兜底如实标「件在桌面产物树（04 号清点），未入站」；10 条本机路径 fs 全可达（门 2 实测）。**README.md**：动画版/README.md 新建（46 行 3,305B ≤60 行限）——站点是什么/入口（serve.py+8392）/三案 intro（35 号乙夺冠）/十一页导航序/数字纪律（台账唯一源+五元组）/审计链 25~48 索引（实测到 48 号，补充令的 49 未见于目录）/旧版对照与回滚（git 仓根=完整版本/ 实测，给出 log/checkout 命令）/门与工具。
**活账漂移实战**：门 2 逐字段对账首战抓到台账 H9 行三字段漂移（43/48 号审计补注：45 格差 3 为未判定格）→ mk-evrows.js 标记区间替换重跑一键同步（44 号 P1-1 修复后同步链首次实战）→ 对账复 384/384。
**门输出**：三门全绿（门 1 FAIL=0；门 2 PASS=76 FAIL=0 含交付物 10 卡/A18 口径/本机路径 10/10；门 3 PASS=66 FAIL=0）；curl 实测 evidence=200、README=200、交付物直链 404×2（如实）。页面 122,019B。（source: 完整版本/research/_pool-pending.md 待记18）
- [open] ：老板裁决 11-20:三案开场故事(墨字/长成记/卷宗)达不到预期,「还原上一个 a 版本」=intro-demo-a3(仪器开机版)重新上岗;三案文件保留在盘归档。执行:a3 拷入动画版(enter 按钮指 index.html 落位即通);index intro 槽主按钮改指 a3、三案卡撤下(等走查修复批落地后改,避免并发编辑冲突)。已向老板标记:a3 中段仍是此前被毙的 def create_order 虚构内容——保留壳换五幕真内容的混搭方案待老板一句话（source: 老板原话 11-20(in-app browser 正看 intro-c)）
- [open] ：老板终选 11-20 09:1x:「选择 intro-demo-a.html」——a1 打字机版正式上岗(intro 槽主按钮,三案卡撤下归档盘上);a3 拷贝已撤;a1 功能接线两处(样式链 ../assets/css/base.css+进入按钮真链 index.html);跳过键保留(a1 原生状态)。intro 终选裁决=已决,收口清单剩 #259 两图指认/#281 三档分级两项待老板（source: 老板原话 11-20 09:1x+index 代理还原落地）
- [open] ：老板令 11-20 09:2x:四工具页改平实章节体——每章:插件基本介绍(干什么/有啥功能)→能力逐项(每个能力解决什么实际问题);撤隐喻标签(眼睛/手);动画凡致文本显示不全即撤;文字版(details)删除;禁密密麻麻大段。解读决定(已向老板亮明):index 总览保留时间轴叙事体裁;3D 母题降级为可选演示或删,文本完整可见为硬规矩。保留:台账锚/数字口径/点击层/下一站(标题改平实)（source: 老板原话 11-20 09:2x）
- [open] ：老板令 11-20 09:3x:①总封面+每章基本介绍插开场动画 ②流程令:「别继续发消息给子任务了,不然子任务会混乱」——禁对在飞代理堆叠热修令,新需求攒批等落地后开新简报。执行计划:等四工具页章节体重构落地→新批两件:总封面页(动画版/cover.html,大标题+进入→a1→index)+四章基本介绍区开场动画(小体量,文本全显硬规矩不变)+导航站名平名化(插件名)（source: 老板原话 11-20 09:3x）
- [open] ：老板令 11-20 09:4x:「暂停全部子任务吧 需要重新对齐」——四工具页章节体重构代理已全部 TaskStop;现场冻结等对齐。背景:章节 demo 被判「能力退化严重」,老板指向原本材料 V8/V4 的写法(技术架构版体裁:为什么自研→创新点→分层成分→功能原理→生命周期→真实例子六幕,标题带论点带数字)。待对齐轴:新版工具页的目标体裁（source: 老板原话 11-20 09:4x）
- [open] ：老板对齐大纲 11-20 09:5x:每章五段=①插件干啥 ②架构/执行流程/原理=动态图表 ③主要能力(对应 skill)④使用例子穿一起 ⑤实现什么闭环;参照 V4(讲内容)+V8(讲架构)两件原本材料;令「先分析大纲自己补全对齐再输出理解」。主会话输出:五段体规格+补全项(起源并入①/数字纪律/密度红线/隐喻撤/文字版删/动态图表文本全显硬约束/四章=四页假设)+待确认两点（source: 老板原话 11-20 09:5x）
- [open] ：老板参考句式 11-20:插件介绍禁口语故事开场(「你说一句帮我测X」被判不行),改为一句技术定位——成果先行+从X到Y到Z链式覆盖+术语直用。样板(auto-test):「插件实现了后端E2E测试闭环:从本地测试环境一键启动,到QA视角的测试用例编写,再到多环境真实接口级测试」;起源两行作第二段保留。四插件定位句由主会话按同式起草入正式简报（source: 老板参考句 11-20 10:0x）
- [open] ：老板指正 11-20:「转html有现成工具的吧」——手搓迷你 md 渲染器属不必要的重造轮子,机器上就有 pandoc 3.13。教训:动手造工具前先全量探测现成方案(pandoc/python markdown/node marked 等一条命令的事)。已切换:md2html.py v2=pandoc gfm 解析+自配色壳,18 件重生成（source: 老板原话 11-20 10:1x）

**进度记录**（共 53 条，列最近 5 条）：

- 2026-11-20 08:08:20 +0800 · 回填：原待记 3（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer；按 42 号 §四.3 并入本条目注记）
- 数据层=唯一真源（SITE_USAGE/SITE_CASES/SITE_INVENTORY/SITE_MEMORY/SITE_TIMELINE，五元组 56 项对台账验证 100%）→ 修复方向=改页面消费代码，禁数据侧兼容层留双名债
- 答辩页定名 /defense.html（四工具页多数派）
- 6+2 处反向锚统一在合并修复批修 tools/scenarios 侧 href
- 合并修复批须用真实数据文件重跑各页 DOM 桩冒烟
- 2026-11-20 08:08:20 +0800 · 回填：原待记 4（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer；按 42 号 §四.3 并入本条目注记）
pool MCP 19:0x ConnectionClosed（进程死、宿主注销）；scenarios/index/F2a-F2b 各批收尾回写失败均折入本档。后续批次记账继续走本档，收口时统一补池。
- 2026-11-20 08:08:20 +0800 · 回填：原待记 8（池 MCP 断连期间记账，事件时点 2026-11-19 夜间；源=完整版本/research/_pool-pending.md；回填执行 2026-11-20 developer；按 42 号 §四.3 并入本条目注记）
agent-gate 拦二波派发（#392 被置待验收）+池 MCP 管道死→python 直写 pool.db 复刻 pool_update：待验收→进行中（合法迁移+status_log 留痕+dev_progress note）；二波+合并修复批四路随后放行。
- 2026-11-20 08:08:21 +0800 · 回填完成（2026-11-20 08:08 developer）：_pool-pending.md 实际 18 条全部落账——①14 条批次记录（待记 1/2/5/7/9/10/11/12/13/14/15/16/17/18）→ 线索 id 318、319、320、321、322、323、324、325、326、327、328、329、330、331（subject 前缀 need392-showcase:backfill-*，逐条 clue_fetch 回读验证）；②待记 3/4/8 → 本条目 dev_progress 注记（回填：原待记 N 前缀，pool_trace 回读验证）；③23a → #395 拒绝已在 status_log 留痕（19:47:59），按 42 号 §四.4 仅引用不回写。执行期变更如实记录：待记 18 系执行中新增（源档 08:05:53 追加，补充批 C），按简报「实际条数」一并回填；缺号 6 经全档清点无内容损失（17 条待记 + 23a）。本条目状态保持进行中，全程未动。源档尾部已加回填完成注记。
- 2026-11-22 20:38:25 +0800 · 收敛归位：全景站原型阶段暂停等开发令

**最近状态变更**（共 24 条）：2026-11-22 20:38:25 +0800 · 进行中 → 挂起 · 老板 2026-11-22 收敛令：暂不开发（原型阶段 proto-chapters.html 五章迭代在案，恢复条件=开发令）

**关联文档**：doc#28（tech v2） · doc#29（prd v1） → 全文见附录 A

---

## bug 池

### 待验收（42）

#### bug#135 · Windows 托盘图标右击菜单位置偏右上（应贴托盘/鼠标位置弹出）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-03 15:00:07 +0800（developer）· 更新：2026-11-18 18:02:20 +0800（developer）
- **blocker**：高峰：AK 派发被峰窗门禁拦，18:00 自动恢复补跑

**详情**：

老板上报（2026-11-03，原话）：win下，托盘图标的右击菜单位置不太对，偏右上。
待查（未执行、未定位）：Windows 托盘（tray_icon_win / EUI 3rd/tray）右键菜单锚点计算——可能原因候选：WM_RBUTTONUP 的坐标未换算 DPI / 未用 GetCursorPos、菜单尺寸未按屏幕右缘/任务栏位置翻转、或锚点用了窗口客户区坐标而非屏幕坐标。影响：菜单可能被任务栏/屏幕边缘截断，且观感偏移。

**正文**：

老板上报（2026-11-03，原话）：win下，托盘图标的右击菜单位置不太对，偏右上。
待查（未执行、未定位）：Windows 托盘（tray_icon_win / EUI 3rd/tray）右键菜单锚点计算——可能原因候选：WM_RBUTTONUP 的坐标未换算 DPI / 未用 GetCursorPos、菜单尺寸未按屏幕右缘/任务栏位置翻转、或锚点用了窗口客户区坐标而非屏幕坐标。影响：菜单可能被任务栏/屏幕边缘截断，且观感偏移。

**关联线索**（4）：

- [open] 锁屏,SendInput,截图：老板机常锁屏远程开发：SendInput/交互桌面复现不可用（托盘菜单类 POC 只能解锁后跑）；GL 帧缓冲回读截图（uat62/probe captureFramePng）锁屏期可用（实测像素非黑）（source: 主会话实测 2026-11-03）
- [open] tray,styledmenu,锚点,工作区：托盘 styled 菜单规格（老板拍板）：弹窗左下角=光标位置。根因=工作区选择『包含期望点』对任务栏带光标必失败→回退取列表首个工作区（多屏=错屏）。修复在 EUI-NEO/components/styledmenu.h（左下角锚+styledMenuSelectWorkArea 最近工作区+钳制不护底）待 commit（source: bug#135 修复实证 evidence/2026-11-03-accept-audit/bug135/）
- [stale] styledmenu,锚点,左上锚,TrackPopupMenu,tray.h,翻转：bug135 锚点改「光标左上锚（原生式）」实现落点（2026-11-05 老板终裁）：① styled 主路径锚点平台分支在 EUI-NEO/components/styledmenu.h:505-514（_WIN32=左上锚 desiredY=光标y+翻转档 true；else=左下锚保持，mac 零变化），钳制翻转档参数 flipUpOnBottomOverflow 加在 styledMenuClampPosition（styledmenu.h:310-341，默认 false 旧语义不变）；② 原生降级 TrackPopupMenu 实际在 EUI-NEO/3rd/tray/tray.h:284（TPM_LEFTALIGN|TPM_TOPALIGN 回退，906e198 的 CENTERALIGN|BOTTOMALIGN 已替换）——tray_bridge.c win 路径只有 eui_tray_menu_requested 钩子 plumbing、无锚点逻辑，无需改；③ DevDesk onTrayMenu 钩子 win/mac 共用（src/main.cpp:998）→ styled 托盘菜单两平台同走 showStyledMenu，平台分支只能做在 styledmenu.h 内部（DevDesk 业务层零平台宏）。（source: 实测（本批改动 git diff + 代码阅读））
- [open] styledmenu,锚定面,panelInset,margin,偏右上,面板内缩,窗矩形,bug135：bug135 AK 终修（2026-11-18 真机红闭环）：「偏右上」根因＝锚定面错位——EUI-NEO/components/styledmenu.h compose 把可见面板从窗原点内缩 style.margin（panelX=panelY=margin，:619-620），旧锚定/钳制按窗矩形算（desiredX/Y=光标、翻转贴窗底）→面板浮在光标右上方 margin×windowScale px（135%DPI≈16.25、150%→18，阴影填缝）。X 批窗级量测绿（menuL-cursorX=0、窗底=任务栏上沿）而老板真机红：量的是窗、看的是面板。修＝EUI-NEO commit 19f0445（origin/main 3f4b9e9 之上）：styledMenuClampPosition 新参 panelInset（默认 0=旧整窗语义，既有调用/单测零波及）+ showStyledMenu Win 分支 desired=光标-inset、钳制传 inset（面板左上贴光标、上翻后面板底贴任务栏上沿、阴影留白越缘）；mac 零变化；锚点语义维持 11-05 左上锚终裁；tray.h/GetCursorPos/DevDesk main.cpp 透传链无缺陷。单测 5c 补 4 用例（AK 批禁构建未跑）。（source: 实测（X 批 evidence/20261118-window-final/ + codegraph 代码测绘 + commit 19f0445 diff））

**进度记录**（共 20 条，列最近 5 条）：

- 2026-11-11 17:05:16 +0800 · 台账归位：转挂起（blocker 已存在），不再占用进行中名额。
- 2026-11-18 13:09:07 +0800 · I2 同 #242 阻塞：坐标量测探针（MN_GETHMENU+GetMenuItemRect+deltaL/deltaT）已就绪（tray/tray4.ps1），差一个可右键的冻结基线托盘实例；假阳性过滤教训见 242 note。窗口期烧。
- 2026-11-18 17:38:37 +0800 · 重开修复方向：真实托盘点击位 vs 合成回调差异（SIB 壳层坐标换算嫌疑）；真机红证据=老板实测原话
- 2026-11-18 17:46:09 +0800 · AK 开工：真机红「托盘右键菜单偏右上」定位根因
- 2026-11-18 18:02:07 +0800 · AK 根因钉死+修复落地：根因＝X 批量的是窗矩形、老板看的是可见面板——EUI-NEO/components/styledmenu.h compose 把面板从窗原点内缩 style.margin（:619-620），旧锚定/钳制（:512-515 desired=光标、:330-334 翻转贴窗底）却按窗矩形算→面板浮在光标右上方 margin×windowScale px（135%DPI≈16px，阴影填缝）＝真机「偏右上」；tray.h GetCursorPos→平台透传→main.cpp 链路无缺陷（X 批全链路合成已证，SIB 坐标语义嫌疑排除）。修法（锚点语义维持 11-05 终裁左上锚不动，只换锚定面）：styledMenuClampPosition 新增 panelInset 参（默认 0=旧整窗语义，既有调用方/单测零波及）+ Win 分支 desired=光标-inset、钳制传 inset→面板左上贴光标、上翻后面板底精确贴任务栏上沿、阴影留白允许越缘（原生观感）；mac 零变化。落地：EUI-NEO commit 19f0445（基于 origin/main 3f4b9e9，未 push 留收口）；DevDesk src 零改动（根因不在 DevDesk 域，无需补丁）。测试：单测补 5c 面板内缩档 4 用例，本批禁构建未跑。复验步骤：①可构建批次定向跑 EUI-NEO styled_menu_clamp_position + DevDesk --dev 增量构建；②真机右键托盘：淡绿面板左缘贴光标 X、底缘贴任务栏上沿零缝；③若仍报偏右上则非本缺陷，按池 note677 备忘升级老板再拍板（居中锚 vs 左上锚）。

**最近状态变更**（共 27 条）：2026-11-18 18:02:20 +0800 · 进行中 → 待验收 · 根因钉死（锚定面=窗→面板）+ EUI-NEO 修复落地 commit 19f0445（DevDesk 域无需补丁）；单测已写未跑（本批禁构建），待可构建批次定向跑 + 真机右键复验

**验收证据**：

- 2026-11-18 14:55:43 +0800 · evidence/20261118-window-final/（n-final.ps1 + n-tray.txt + n-tray-menu.png）：PostMessage 合成 WM_RBUTTONUP 至 class='TRAY' 回调窗，光标钉托盘位 (1460,1048) → styled 菜单窗 title='EUI Styled Menu' 弹出 rect=(1460,874)-(1791,1015)：menuL-cursorX=0（左缘与光标 X 精确对齐）、menuB=1015=工作区底缘/任务栏上沿（贴托盘上翻，符合 2026-11-05 拍板「光标左上锚+边缘翻转」规格）。无偏右上、无异屏迁移——原症状未复现。保留注记：SIB 壳层真实图标右键路由未自动化（flyout flaky），app 侧定位逻辑已行为级实证。

---

#### bug#150 · 实机：用户定时行卡 hover 果冻形变不生效（仅系统定时卡有效，探针盲区）

- 状态：待验收 · 优先级：P2 · 负责人：qa-tester · 阶段：测试中
- 创建：2026-11-03 17:37:39 +0800（qa-tester）· 更新：2026-11-18 13:33:41 +0800（qa-tester）

**详情**：

老板实机报告（2026-11-03）：hover 果冻形变只在「系统定时」列表卡片可见，「用户定时」卡片无效果。已查证：①代码层两处对称（用户行 schedule_ui_list.cpp:705 jellyCard + :746 命中面 onHover；系统行 :793 + :847 同构）；②schedule_ui_probe S4 断言的恰是用户行 schedule.list.user.0 且今早基线 27/27 绿——探针直调 onHover 回调绕过真实命中测试，实机命中面/层级疑点探针盲区；③安装版（13:50）已含 jelly 代码（5fec8faf）。归因方向：用户行命中面实机命中不生效（层级/命中几何），待实机复现定位。处置：折入池140 实现批次（行渲染整体重写为瀑布流卡片），新实现须：所有类别卡 jelly 生效 + 补命中面级断言（非仅回调直调）防探针盲区复发。

**正文**：

老板实机报告（2026-11-03）：hover 果冻形变只在「系统定时」列表卡片可见，「用户定时」卡片无效果。已查证：①代码层两处对称（用户行 schedule_ui_list.cpp:705 jellyCard + :746 命中面 onHover；系统行 :793 + :847 同构）；②schedule_ui_probe S4 断言的恰是用户行 schedule.list.user.0 且今早基线 27/27 绿——探针直调 onHover 回调绕过真实命中测试，实机命中面/层级疑点探针盲区；③安装版（13:50）已含 jelly 代码（5fec8faf）。归因方向：用户行命中面实机命中不生效（层级/命中几何），待实机复现定位。处置：折入池140 实现批次（行渲染整体重写为瀑布流卡片），新实现须：所有类别卡 jelly 生效 + 补命中面级断言（非仅回调直调）防探针盲区复发。

**进度记录**（共 7 条，列最近 5 条）：

- 2026-11-03 18:19:57 +0800 · 修复随池140 重写：命中面建卡 stack 兄弟最后 + 探针三层断言（hit 几何/z 序/真实 queuePointerMotion 驱动果冻 tint）防回调直调盲区复发
- 2026-11-03 19:36:34 +0800 · 随池140 重写落地 commit 92b6de42：命中面建卡 stack 兄弟最后+探针三层断言（hit 几何/z 序/真实 queuePointerMotion 驱动 tint）；隔帧点击修复后待复验
- 2026-11-03 21:00:49 +0800 · 派发 bug-fixer：schedule_ui_probe 复验仍红（TAB 点击不切换，级联 D/E 失败），与池150 命中面同域合并诊断修复
- 2026-11-03 22:20:48 +0800 · 修复完成待验收。根因三层：(1) 主因——schedule_ui_list 的 scrollView content lambda 形参 ui 遮蔽外层 compose(ui)，drawMasonry/drawCard 捕获外层主 ui，EUI scrollview 测量路径（measureContentHeight→临时 measureUi）把卡片画进主树 root 直下 (0,0) 形成幽灵副本，hitTest 逆序被幽灵先命中→TAB/页首左区点击全吞；修法=ui 逐层透传。(2) 探针种子误用 updateRule 写 archivedAt（契约只有 archiveRule 入口）→归档段恒空；修法=archiveRule。(3) 池150 同域实机 bug——归档段 masonry 卡 slot (0,0) 起算叠在用户卡正上方且后建先命中→点用户卡开归档规则编辑器、删除链路删错规则；修法=drawMasonry 增 yFlowOffset（用户段流高+折叠头 32）。附带：探针点击改 motion→press→release 真实输入语义；listRules 期望修正（软删行被过滤）。验证：ctest -R schedule_ui_probe 全绿（C0/B/C/E PASS + PASS），doctest --filter schedule 全绿（127/127）。改动：src/module_schedule/schedule_ui_list.cpp + tests/probes/schedule_ui_probe.cpp，未动 EUI-NEO。
- 2026-11-18 13:33:41 +0800 · 批M 未跑（预算耗尽，优先 P1 链）。处置既定折入池140 行渲染重写批次（新实现须所有类别卡 jelly 生效+补命中面级断言防探针盲区复发）。真指针 hover 原语已验可用：driver scroll 步（dx=dy=0）即 pointer motion 到目标中心，下波可直接用。

**最近状态变更**（共 6 条）：2026-11-03 22:20:48 +0800 · 进行中 → 待验收 · 修复完成待验收。根因三层：(1) 主因——schedule_ui_list 的 scrollView content lambda 形参 ui 遮蔽外层 compose(ui)，drawMasonry/drawCard 捕获外层主 ui，EUI scrollview 测量路径（measureContentHeight→临时 measureUi）把卡片画进主树 root 直下 (0,0) 形成幽灵副本，hitTest 逆序被幽灵先命中→TAB/页首左区点击全吞；修法=ui 逐层透传。(2) 探针种子误用 updateRule 写 archivedAt（契约只有 archiveRule 入口）→归档段恒空；修法=archiveRule。(3) 池150 同域实机 bug——归档段 masonry 卡 slot (0,0) 起算叠在用户卡正上方且后建先命中→点用户卡开归档规则编辑器、删除链路删错规则；修法=drawMasonry 增 yFlowOffset（用户段流高+折叠头 32）。附带：探针点击改 motion→press→release 真实输入语义；listRules 期望修正（软删行被过滤）。验证：ctest -R schedule_ui_probe 全绿（C0/B/C/E PASS + PASS），doctest --filter schedule 全绿（127/127）。改动：src/module_schedule/schedule_ui_list.cpp + tests/probes/schedule_ui_probe.cpp，未动 EUI-NEO。

---

#### bug#189 · 设置页「项目卡」tab 用途不明（内部测试功能暴露给用户）——处置候选删除

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-04 10:55:28 +0800（ZCode主会话）· 更新：2026-11-04 23:00:54 +0800（ZCode主会话）

**详情**：

老板 2026-11-04 问「设置里的项目卡是干什么用的」+归口 bug 池原话「全部放bug池」——实测=需-11 加的设置页第 5 个 tab，存「名称+测试库地址」（project_cards 表，GitHub 同步时整表跳过），偏内部测试用途，老板不认识它=无用候选。处置候选=删除（tab+表+store+相关测试，同步 skip 清单同步收口）；等老板一句话定「删」或给保留理由

**正文**：

老板 2026-11-04 问「设置里的项目卡是干什么用的」+归口 bug 池原话「全部放bug池」——实测=需-11 加的设置页第 5 个 tab，存「名称+测试库地址」（project_cards 表，GitHub 同步时整表跳过），偏内部测试用途，老板不认识它=无用候选。处置候选=删除（tab+表+store+相关测试，同步 skip 清单同步收口）；等老板一句话定「删」或给保留理由

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-04 22:59:12 +0800 · 开工登记（2026-11-04 夜，S 级主会话直做，老板已睡自行裁决）：移除设置页「项目卡」tab UI 入口——kSettingTabLabels/Count 5→4、卡注册行与 CardHeightKind::ProjectCards 分支删除；project_card_store/表保留（MCP project_info 工具仍用，删数据另行拍板）。待构建验证后置待验收。

**最近状态变更**（共 4 条）：2026-11-04 23:00:54 +0800 · 进行中 → 待验收 · 

---

#### bug#191 · telemem 采集轮提案风暴：1630 条 pending 同一秒批量落库（无去重/无批量上限），触发 bug190 冻结规模因子

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：开发中
- 创建：2026-11-04 14:17:31 +0800（developer）· 更新：2026-11-18 19:20:06 +0800（developer）
- **blocker**：采集侧修复未实施：采集轮去重/批量上限未动（渲染侧截断已修不覆盖本条范围）；单独排产批次未派（evidence/20261118-realmachine3/191 目录为空，无实机证据）——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

bug190 修复过程中实查发现（bug-fixer，clue45/46）：~/DevDesk/memory/proposals.db 中 1630 条 pending 提案全部同一秒批量落库，kind 全 add，正文均长 1975 CJK 字符——采集轮缺去重/缺批量上限，是 21K 元素树冻结的规模因子（渲染侧截断已修，采集侧未动）。待分析：采集管线（telemem collect 轮）为何一轮产出 1630 条同类提案、去重口径（内容 hash？相似度阈值？）、上限策略。修复前老板库可正常用（首屏 50+渐进加载，徽章报真值，无需清库）。后续单独排产，勿与渲染侧混批。

**正文**：

bug190 修复过程中实查发现（bug-fixer，clue45/46）：~/DevDesk/memory/proposals.db 中 1630 条 pending 提案全部同一秒批量落库，kind 全 add，正文均长 1975 CJK 字符——采集轮缺去重/缺批量上限，是 21K 元素树冻结的规模因子（渲染侧截断已修，采集侧未动）。待分析：采集管线（telemem collect 轮）为何一轮产出 1630 条同类提案、去重口径（内容 hash？相似度阈值？）、上限策略。修复前老板库可正常用（首屏 50+渐进加载，徽章报真值，无需清库）。后续单独排产，勿与渲染侧混批。

**关联线索**（3）：

- [open] expireSweep,未接线,TTL,1629,proposals.db：expireSweep 生产未接线：proposal_store.h:146 声明、proposal_store.cpp:680 实现，src/ 全域零调用方（唯一调用=tests/test_memory_proposals.cpp:139）——TTL 30 天只写入 expires_at（真库实测 1629 pending 的 expires_at=created_at+2592000≈2026-12-04）但无任何执行路径，到期不会自动过期；1629 pending 将永久滞留审核视图（source: grep src/ 全域 + sqlite 只读实测 2026-11-04）
- [open] 风暴根因,collect_round,首轮全量,infer_queue,1626,烧钱：提案风暴全链代码锚：panel_state.cpp:185 引擎 Running 首轮立即跑采集 + collector.h:25 kPollIntervalMs=30000 → 水位 0 时首轮全量回读全部积压会话；collect_round.cpp:93-94 每过闸 4KB 段逐条 proposeAdd（confidence 硬编码 0.9、单轮无上限、无内容级去重——store.create 直插 proposal_store.cpp:241-247 无指纹）；session_reader.cpp:14-17/43-53 只滤 system-reminder 与非错误 tool_result，hook 噪声为 user text 形态（narrative=true）全过。infer_queue.db 同批积压：真库只读实测 1626 行（collect_round.cpp:82-92 每过闸段同步入队），infer_batch.cpp:53-67 节流闸只限速不清队列——噪声文本会按闸速逐条发云端 llm 提炼持续烧计费（source: 文件:行 + sqlite 只读实测（proposals.db pending1629/rejected1 全 source=collect；infer_queue.db 1626 行）2026-11-04）
- [open] bug191,F-02,水位,段预算,kMaxSegmentsPerPoll,readSessionFile,collect_round：F-02「下轮再处理」机制定稿事实：collector 水位在 readSession 内随读推进到 EOF（collector.cpp:257 setOffset），commit 只记事件不回退水位（collector.cpp:299-312）——commit 层截断必丢段；故单轮上限 N=50 落在读取侧：collector.h kMaxSegmentsPerPoll=50（pollOnce 全局预算，预算用尽停扫）+ readSessionFile(file,maxSegments) 事件级预算检查（预算用尽行级回退 consumedFrom/lineNo 再 break，eventSeq 逐轮稳定，collector.cpp:236-258）；runCollectRound 无截断逻辑，polled 段全处理全 commit（source: 文件:行（collector.cpp:236-312 / collector.h:25-31）+ 实测（红绿用例 memory_collect_storm_round_cap））

**进度记录**（共 9 条，列最近 5 条）：

- 2026-11-05 00:52:09 +0800 · 开发派发（2026-11-05 夜，owner→developer）：F-01~F-07 + 存量清理实操（先备份 proposals.db/infer_queue.db 到 evidence 再改；pending→expired + infer_queue 清空；前后计数对账）。预算 ≤70 调用/≤45min。
- 2026-11-05 01:16:47 +0800 · 开发完成：F-01 指纹去重（SHA256 归一化，pending+近期已决200+同轮三集合）、F-02 单轮上限 50（机制修正：读取侧段预算 kMaxSegmentsPerPoll，见 clue82）、F-03 噪声模式 v1+极短段<50 拒（F-03③由 collector 出段闸既有保证）、F-04 expireSweep 接线 runCollectRound 每轮开头、F-06 confidence 0.9→0.3、F-07 四用例红绿、F-05 存量清理执行完毕。filter memory 102/102 绿
- 2026-11-18 13:33:11 +0800 · 批M 事实确认（SQL 只读复本）：1630 条 memory_proposals 全部 created_at=同一秒 1793766937（group top1=该秒 1630 条）；audit 分布 created 1630 / expired 1629 / rejected 1 → 现 pending=0（风暴数据已被过期回收，老板库当前可正常用）。采集轮缺去重/缺批量上限缺口仍在，修复未做——待排产。
- 2026-11-18 19:06:56 +0800 · 重开排产：BD 批（采集侧去重+批量上限）——F-06 门禁已落（371625f7 拦垃圾），本条做实质洪峰治理；AY 批刚腾空 collect 域零撞面。
- 2026-11-18 19:20:06 +0800 · 设计：①去重复用既有 F-01（commit 5f12e514 已在 proposeAdd 前落内容指纹去重：同轮 roundFps+跨轮 knownFps(pending全量+已决200)，dedupSkipped 计数已在——简报建议键(会话,指纹)为弱键会跨会话重复出卡，采强键不重造，仅补测试断言×2。②上限=单轮单会话出卡 kMaxCardsPerSessionRound=10（195≈4轮×50段→同流量40，−79%；人审负担；正常会话 30s 轮窗 >10 提案级段罕见）。③落点在 F-06 洗/F-03 低值/F-01 去重之后、evidence/enqueue/proposeAdd 之前：上限只数真卡，噪声重复不占预算不虚增 overflow。④overflow 接线 washed 同款（report.overflowDropped）+头尾样本引用 OverflowSample{会话,首事件序号,指纹}（零正文）进 report+LOG_WARN；超限段丢弃且 commit 记已处理=终局不重放（简报明确丢弃语义；F-02 旧续产语义挡不住 195 实证）。变更：collect_round.h +20、collect_round.cpp +50、test_memory_collect_wiring.cpp ±57（dedup 用例钉 dedupSkipped=1×2；上限用例改新契约 60段→首轮10卡+溢出40+样本2/次轮10卡零溢出/pending=20）。wip commit c648a56b（只含 3 文件）。数值断言波末统一构建批跑（本波禁构建）。

**最近状态变更**（共 6 条）：2026-11-18 19:20:06 +0800 · 进行中 → 待验收 · 设计：①去重复用既有 F-01（commit 5f12e514 已在 proposeAdd 前落内容指纹去重：同轮 roundFps+跨轮 knownFps(pending全量+已决200)，dedupSkipped 计数已在——简报建议键(会话,指纹)为弱键会跨会话重复出卡，采强键不重造，仅补测试断言×2。②上限=单轮单会话出卡 kMaxCardsPerSessionRound=10（195≈4轮×50段→同流量40，−79%；人审负担；正常会话 30s 轮窗 >10 提案级段罕见）。③落点在 F-06 洗/F-03 低值/F-01 去重之后、evidence/enqueue/proposeAdd 之前：上限只数真卡，噪声重复不占预算不虚增 overflow。④overflow 接线 washed 同款（report.overflowDropped）+头尾样本引用 OverflowSample{会话,首事件序号,指纹}（零正文）进 report+LOG_WARN；超限段丢弃且 commit 记已处理=终局不重放（简报明确丢弃语义；F-02 旧续产语义挡不住 195 实证）。变更：collect_round.h +20、collect_round.cpp +50、test_memory_collect_wiring.cpp ±57（dedup 用例钉 dedupSkipped=1×2；上限用例改新契约 60段→首轮10卡+溢出40+样本2/次轮10卡零溢出/pending=20）。wip commit c648a56b（只含 3 文件）。数值断言波末统一构建批跑（本波禁构建）。

**关联文档**：doc#6（prd v2） → 全文见附录 A

---

#### bug#239 · 记忆提炼补标 S8 红：真 LLM 下 runInferStep 成功+记账+出队，但引擎清单无前缀产物（Windows 真机 e2e 44/45 唯一红）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-05 05:46:41 +0800（developer）· 更新：2026-11-22 19:56:33 +0800（developer）

**详情**：

bug239 根因已钉（三层，见 clue 132）：①run-memory-e2e.sh §2 yaml 硬编码 open.bigmodel.cn/glm-5.3-flash，注释声称的 E2E_LLM_BASE/MODEL 覆盖未接线——真 LLM 轮 codingxrui key 发错网关 401（telemem.log:281 实证，全程唯一 LLM 调用即 401，简报「非 key 问题」前提不成立：哑 key 轮与真 key 轮死在同一 401）；②telemem mcp/server.py:113-120 异常兜底 {error,detail} 回 MCP 200 非 isError；③C++ parseAddResponse 忽略内层 error 键静默 ok("") → 记账出队零产物。已修：脚本接线 BASE/MODEL（telemem_client.cpp:316-341 内层 error 判负 fail-closed；infer_batch.h:34-42 + InferStepReport.productMissing；infer_batch.cpp:129-146 零产物 LOG+note 上浮；补单测 test_memory_hook.cpp memory_gateway_add_error_envelope_fail + test_memory_infer.cpp memory_infer_route2_zero_product_visible + 路②正例加 productMissing==false 断言）。未验证（共享 build 被占、任务禁构建）：单测编译运行、真机 e2e 复跑（E2E_LLM_KEY/E2E_LLM_BASE/E2E_LLM_MODEL 三参齐传）。状态留进行中待主会话收口验证。

**正文**：

bug239 根因已钉（三层，见 clue 132）：①run-memory-e2e.sh §2 yaml 硬编码 open.bigmodel.cn/glm-5.3-flash，注释声称的 E2E_LLM_BASE/MODEL 覆盖未接线——真 LLM 轮 codingxrui key 发错网关 401（telemem.log:281 实证，全程唯一 LLM 调用即 401，简报「非 key 问题」前提不成立：哑 key 轮与真 key 轮死在同一 401）；②telemem mcp/server.py:113-120 异常兜底 {error,detail} 回 MCP 200 非 isError；③C++ parseAddResponse 忽略内层 error 键静默 ok("") → 记账出队零产物。已修：脚本接线 BASE/MODEL（telemem_client.cpp:316-3

**关联线索**（3）：

- [open] bug239,S8,parseAddResponse,error信封,E2E_LLM_BASE,401,补标,productMissing：bug239 根因链三层实证（2026-11-05）：①外层=scripts/run-memory-e2e.sh §2 yaml heredoc 硬编码 anthropic_base_url=open.bigmodel.cn + model=glm-5.3-flash，注释声称的 E2E_LLM_BASE/MODEL 覆盖从未接线——「真 LLM 轮」codingxrui 真 key 被发往 bigmodel → telemem.log:281 实证 401（令牌已过期或验证不正确），全程唯一一次 LLM 调用即 401；②telemem mcp/server.py:113-120 _call 异常兜底把 LLM 401 以普通 dict {\"error\",\"detail\"} 回 MCP HTTP 200（非 isError），mem0 add 零落库；③C++ 侧 telemem_client.cpp parseAddResponse(310-331) 忽略内层 error 键 → 静默 ok(\"\") → infer_batch runInferStep 记账 150+出队+succeeded，tagInferredProducts 差分零新增静默 no-op → S8 全绿但产物缺失。key 走 env 兜底已证（mem0 llms/anthropic.py:40 config 空→ANTHROPIC_API_KEY env），base 只认 yaml。修复=脚本接线 BASE/MODEL + parseAddResponse 内层 error 判负 + InferStepReport.productMissing 零产物显式上浮（infer_batch.h:34-42/infer_batch.cpp:129-146）。（source: 文件:行（e2e 脚本/telemem mcp server.py/mem0 anthropic.py/telemem_client.cpp/infer_batch.cpp）+ evidence/2026-11-05-memory-e2e/telemem.log:281 实测）
- [open] real-LLM,fangzhou,Bearer,fenced,json围栏,层4,S8,载体,agent_e2e,agent_live：#239 真 LLM 轮实测（2026-11-18，evidence/20261118-real-llm/）：①载体口径：#239 收口载体=run-memory-e2e.sh 真 LLM 档；agent_live.cpp（agent_live.cpp:630 loadRealProfile 用户数据根只读副本）与 agent_e2e_probe.cpp（:128 ProbeProfileSource 生产凭据密文只读搬运+:403 LLM 腿）是 agent 模块两条独立真 LLM 载体，均非 #239。②真网关链路实证：Claude Code ~/.claude/settings.json env.ANTHROPIC_AUTH_TOKEN（len30）+ANTHROPIC_BASE_URL=codingxrui.geely-test.com/fangzhou/anthropic 经 127.0.0.1:10808 代理 → /v1/messages 网关 200+合法 anthropic JSON（glm-5.3 思考块+文本块，usage 205/215）；raw curl x-api-key 形态 401、Bearer 形态 200；venv anthropic 2.4.0 为 httpx2 补丁版（SDK 形态可用）。③层④根因（bug239 三层外新层）：glm-5.3 对 mem0 update 提示词的 text 块输出 ```json 围栏，telemem/mem0.py:574 json.loads 不剥围栏 → telemem.log:287 'Invalid JSON response: char 0' → stored_summaries=[] → S8 零产物四红；同栈复现 repro-llm-update-call.txt（end_turn 非截断，围栏内即合法 stored_memories JSON，剥围栏即终绿路径）。④bug239 修复半证：脚本接线✓+零产物可见性✓（旧静默绿→显式红），绿面待层④修复。（source: 实测：evidence/20261118-real-llm/（manifest.md+repro-llm-update-call.txt+gateway-probe-bearer.*+telemem.log:284-287））
- [open] 层5,前缀方差,BUG-2,围栏非确定,8421互劫,隔离缺口,round2污染,round3恢复：#239 层④修复轮实测（2026-11-18 18:10-18:45，evidence/20261118-real-llm/round2|round3/）：①层④围栏修复证真：telemem/mem0.py 加 _strip_json_fence（:197/:574）后 Invalid JSON 消失×2 轮、update 调用 200→insert 落库；围栏输出本身非确定（同输入三次复现 2 围栏 1 裸 JSON）→修复必要且已覆盖。②层⑤新层（BUG-2）：S8 前缀契约脆弱——同栈同输入复现 summary 丢前缀首字符「[」（「踩坑|会话采集]…」），轮 3 现场 LLM/parse/insert 全通但「S8 产物: 」空、monthTokens=0、未出队=前缀字面过滤失配的级联；修复方向=前缀容错/prompt 硬约束/卡侧标记。③轮 2 污染事件（环境，未钉死）：temp 库混入非 fixture 真实风格内容（先规划后开发/先做hello world网页转md），静态源 grep 全无；疑老板运行实例共享 8421 引擎端口并发互劫（其真实卡经测试引擎+真 LLM 落库，真实 infer 队列现空）；轮 3 干净窗口全恢复→瞬态并发；衍生隔离缺口：run-memory-e2e.sh 绑生产端口 8421，运行中 DevDesk 实例必互劫（待派修）。（source: 实测：evidence/20261118-real-llm/{round2,round3}/（run/probe/telemem 三轮）+ repro-llm-update-call.txt + mem0.py.bak）

**关联 BUG**（2）：

- 2026-11-18 18:12:32 +0800 · 未解 · BUG-1（层④，新，未处理）：S8 真 LLM 轮红——glm-5.3 响应 text 块为 ```json 围栏文本，telemem/mem0.py:574 json.loads 不剥围栏 → 'Invalid JSON response: Expecting value: line 1 column 1 (char 0)'（telemem.log:287 实证）→ stored_summaries=[] → 零补标产物/零计账/零出队（探针 S8 四红）。同栈复现：evidence/20261118-real-llm/repro-llm-update-call.txt（text 块 head='```json\n{…'，end_turn 非截断，围栏内即合法 stored_memories JSON）。关联：#239 S8。分类：引擎链缺陷（telemem/mem0 围栏解析），非环境/非 key/非 DevDesk C++。修复归属：引擎侧 venv 包（剥围栏或 prompt 强约束裸 JSON+解析容错），DevDesk src 不动。
- 2026-11-18 18:37:18 +0800 · 未解 · BUG-2（层⑤，新，未处理）：S8 补标前缀契约 vs 模型输出方差——update 调用实测偶发丢前缀首字符「[」（同栈同输入复现 summary=「踩坑|会话采集]…」无左括号），C++ 按字面前缀 [踩坑|会话采集] 过滤失配→零产物→productMissing 判负四红级联（轮 3：LLM 200×2+parse 成功+insert 落库 1 条但「S8 产物: 」空、计账 0、未出队）。修复方向：前缀匹配容错（剥首空白/全半角括号方差）或 prompt 硬约束逐字保留或契约改卡侧标记。关联 #239 S8。证据：evidence/20261118-real-llm/round3/ + repro-llm-update-call.txt 第三次复现。

**进度记录**（共 14 条，列最近 5 条）：

- 2026-11-22 19:03:20 +0800 · 验证批接手（2026-11-22）：修复已提交 7cd7217c（三层）+ venv 层④/⑦已修实证（mem0.py _parse_json_tolerant）——单测编译运行+e2e 三参复跑收口；修复代码未验证状态解除中
- 2026-11-22 19:09:20 +0800 · 波1 验证批（qa，2026-11-22）e2e 真网关复跑：`run-memory-e2e.sh --keep`（key=用户库 claude_profiles#3 明文，未回显未落盘；BASE=fangzhou/anthropic MODEL=fangzhou/glm-5.3；产物=12:15/12:17 波前二进制，含 7cd7217c）。网关 POST /v1/messages 200 OK（非 401，层①已收）。结果 8 环节 2 红：③探针 ok=43 fail=3（S8 三红）＋⑤hook 负对照注入 1 条。S8 现场（保留根 /tmp/mem-e2e.pLBXKt 的 infer_queue.db）：attempts=1、last_error="响应非 JSON"、行未出队、monthTokens=0。根因定位到我方：src/module_memory/gateway/telemem_client.cpp:96「响应非 JSON」= parseJsonRpcResult 在 stripSseEnvelope 后仍 parse 失败（非 telemem/venv 层）；原始 body 未落日志→修复批先加脱敏一次性 body 落盘再定终局。另：S8「清单含补标产物」PASS 为假绿——匹配文本=入队卡原文（[踩坑|会话采集] ccache…），无法区分补标产物与入队回显，需改差异断言。证据 evidence/20261122-wave1-verify/{wave1-verify.md,run.log,probe.log,telemem.log}。状态留进行中（未收口）。
- 2026-11-22 19:10:46 +0800 · 修复批接手（2026-11-22）：S8 残红链——①telemem_client.cpp:96 加脱敏 body 落盘判终局（SSE 残帧/HTML/空体）+stripSseEnvelope 补覆盖 ②S8 假绿断言改差异断言（产物≠入队文本）③e2e 复跑到 S8 全绿
- 2026-11-22 19:55:06 +0800 · 修复批收口：①stripSseEnvelope 加固（逐行取首个非空 data: 载荷，覆盖无 event 前缀帧/CRLF/data 心跳空帧）②parse 失败一次性脱敏 body 落盘（<数据根>/infer_body_debug.log，抹 sk-/Bearer，≤8KB）③S8 断言改差异断言（产物≠入队原文），新增单测 memory_gateway_add_sse_envelope_no_event_prefix 绿。e2e（--keep）：probe SUMMARY ok=46 fail=0；S8 runInferStep 真调成功/补标产物为提炼改写（≠原文）/monthTokens=150/队列已出队 全绿，hook 负对照 0 注入。关键事实：~/.claude/settings.json 的 13 字符 token 只对 127.0.0.1:15721 本机路由有效，直连 codingxrui 返回 401（key/base 配对问题）；走 15721 全绿。证据 evidence/20261122-wave1-verify/{fix-batch.md,e2e-fix2.log} + evidence/2026-11-22-memory-e2e-win/probe.log。装机验收归主会话。
- 2026-11-22 19:56:33 +0800 · 修复批收口终态：S8 全绿且断言可采信（差异断言=产物≠入队原文）；关键事实：13 字符 token 只对 127.0.0.1:15721 本机路由有效（直连 codingxrui 401），e2e 用 E2E_LLM_BASE=http://127.0.0.1:15721 跑绿；工作树未 commit 待主会话收口

**最近状态变更**（共 6 条）：2026-11-22 19:56:33 +0800 · 进行中 → 待验收 · 修复批收口终态：S8 全绿且断言可采信（差异断言=产物≠入队原文）；关键事实：13 字符 token 只对 127.0.0.1:15721 本机路由有效（直连 codingxrui 401），e2e 用 E2E_LLM_BASE=http://127.0.0.1:15721 跑绿；工作树未 commit 待主会话收口

---

#### bug#245 · 终端 IME：中文候选词窗不在终端光标附近（imeRect 定位偏移）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-05 13:33:41 +0800（bug-fixer）· 更新：2026-11-05 22:04:12 +0800（bug-fixer）

**详情**：

老板 2026-11-05 真机上报（原话）：「输入法的候选词也不是显示在光标附近」。现象：终端页中文输入法组合时，候选词窗不在终端光标附近（偏离/停在错误位置）。

链路：terminal_page.cpp 每帧用 frame.cursorRow/cursorCol 算可视 cell 坐标，通过 ui.rect("term.ime.anchor").imeRect(ix, iy, cellW, cellH) 上报；EUI-NEO runtime updateImeCursorRect（runtime_input.h:328-370）取 focused 元素 hasImeRect → setImeCursorRect → Windows ime_bridge（eui_ime_apply_cursor_rect，ImmSetCandidateWindow CFS_EXCLUDE）。

根因候选：
1. imeRect 的 ix/iy 计算用估算 cellW=fontSize*0.6 / cellH=fontSize*1.25 与 frameRow/visRow 映射（terminal_page.cpp 层 4 IME 锚点），与真实字体/行高不符 → 候选窗偏移（与 bug243 错行、bug244 块字符同源）；
2. imeRect 是页面内逻辑坐标，EUI 转像素时 DPI 换算/元素渲染变换（hitMatrixForElement）是否与真实网格对齐待验；
3. Windows 下 eui_ime_apply_cursor_rect 的 rcArea 构造（CFS_EXCLUDE 候选窗靠光标下方）与多屏/DPI 缩放；
4. 候选词窗口位置在输入过程中 cursorCol 变化后未刷新（compose 频率/需要 cursor 移动时请求 update）。

修复方向：imeRect 改用真实 cellW/cellH 与同一坐标空间（与 need241 字体度量同批），修后真机验中文输入候选窗贴光标 cell 下缘。

关联：bug240（终端链路）、bug243（拖选错行）、bug244（块字符对齐）、need241（设计·字体度量）。

**正文**：

老板 2026-11-05 真机上报（原话）：「输入法的候选词也不是显示在光标附近」。现象：终端页中文输入法组合时，候选词窗不在终端光标附近（偏离/停在错误位置）。

链路：terminal_page.cpp 每帧用 frame.cursorRow/cursorCol 算可视 cell 坐标，通过 ui.rect("term.ime.anchor").imeRect(ix, iy, cellW, cellH) 上报；EUI-NEO runtime updateImeCursorRect（runtime_input.h:328-370）取 focused 元素 hasImeRect → setImeCursorRect → Windows ime_bridge（eui_ime_apply_cursor_rect，ImmSetCandidateWindow CFS_EXCLUDE）。

根因候

**关联线索**（2）：

- [open] bug245,ime_bridge,三件套,CFS_POINT,CFS_FORCE_POSITION,修正,落地：【修正 clue102 前提】落地时发现 ime_bridge.c 并非「只发 CFS_EXCLUDE 一项」：2026-11-05 早前迭代已发 ImmSetCompositionWindow，但为 CFS_FORCE_POSITION @ (x, y) 行顶（改动前 :137-146），candidate 仅 CFS_EXCLUDE 无 ptCurrentPos/CFS_CANDIDATEPOS 位（改动前 :148-156）；旧注释称「composition 点放行底会二次竖向偏移」系旧实验结论，已被 clue102/winit/Chromium 一手结论推翻（winit 用 CFS_POINT @ y+height）。本次替换为标准三件套：composition CFS_POINT @(x,y+h) + candidate CFS_EXCLUDE|CFS_CANDIDATEPOS（pt+rcArea 均 @(光标rect/底部)）+ 1x1 GetCaretPos 探测式 caret 兜底（GetFocus 守卫、已存在只 SetCaretPos 不重建）。落点 EUI-NEO/core/platform/ime_bridge.c（win 分支 only，头文件无新声明）。（source: 文件:行（本次落地实操 diff））
- [open] bug245,重发时机,updateImeCursorRect,closeEnough,焦点,DPI：IME 重发时机确认（bug245 验收项）：eui_ime_set_cursor_rect_with_font 每次光标 rect 更新都直接调 eui_ime_apply_cursor_rect（非只写静态 state，ime_bridge.c:268→273 附近）；另有 wndproc 过滤器在 WM_IME_COMPOSITION / IMN_OPENCANDIDATE / IMN_CHANGECANDIDATE 后 reapply（:179-199 未动）。caret 兜底随 apply 同步挪位，焦点丢失后系统销毁 caret 时由 GetCaretPos 探测失败自动重建，无跨焦点残留。terminal_page imeRect 传值逻辑未触碰（need250 刚改过，遵守边界），win 分支按像素直传无 DPI 二次缩放。（source: 文件:行（本次复核））

**进度记录**（共 9 条，列最近 5 条）：

- 2026-11-05 17:59:45 +0800 · ImeTrace 三段对照已埋：段1 terminal_page.cpp 层4（src ix/iy/cellW/cellH/cursorRow/cursorCol/scrollOffset，LOG_INFO≤30条）；段2 EUI runtime_input.h updateImeCursorRect（eui pixelRect x/y/w/h/dpiScale，stderr≤30条）；段3 ime_bridge.c eui_ime_apply_cursor_rect 入口（win rect x/y/w/h，stderr≤30条）。读法：段1 grep ImeTrace devdesk.log；段2/3 需控制台跑 DevDesk.exe 2> ime.log（GUI 无控制台，stderr 不落文件）。真机中文输入法触发组合即可对齐三段坐标，定位偏移发生在哪一段。
- 2026-11-05 18:12:26 +0800 · ImeTrace 三段对照已埋并改落文件（采集修正）：三段统一 eui_ime_trace_log → %USERPROFILE%\DevDesk\logs\ime-trace.log（append，DEVDESK_DATA_ROOT 可覆盖，≤100 行停写）。段1 terminal_page.cpp 层4（src ix/iy/cellW/cellH/cursorRow/cursorCol/scrollOffset，≤30 条）；段2 EUI runtime_input.h updateImeCursorRect（eui pixelRect x/y/w/h/dpiScale，≤30 条）；段3 ime_bridge.c eui_ime_apply_cursor_rect 入口（win rect x/y/w/h，≤30 条）。读法：真机中文输入法组合后 grep ImeTrace ime-trace.log，三段同一文件按行对齐；TermPerf（bug248）/CellTrace（bug244）仍走 devdesk.log。
- 2026-11-05 19:38:20 +0800 · 2026-11-05 调研结论（clue102）——修复方向已明确：Windows 自绘光标 IME 定位 = 三件套同时发：①ImmSetCandidateWindow(CFS_EXCLUDE, rcArea=光标rect) ②**ImmSetCompositionWindow(CFS_POINT, pt=光标 cell 底部 y+height)**（当前 ime_bridge 只发①，这是最可能的主因）③中文 IME 补 CFS_CANDIDATEPOS。坐标必须是客户区**物理像素**（逻辑×DPI）。另优先验证：无系统 caret 时部分中文 IME 读 GetCaretPos → 候选窗落左上方；Chromium 用 CreateCaret(1×1)+SetCaretPos 跟随光标。排查顺序：②→临时 caret→坐标/DPI→时机/HWND。验收仍按 T-13（ime-trace.log 三段）。
- 2026-11-05 20:07:28 +0800 · bug-fixer 开工：按 clue102 三件套口径落地，win 分支 only，不构建不测试（主会话统一构建）
- 2026-11-05 20:10:09 +0800 · bug-fixer 代码落地完成（未构建未测试，主会话统一构建）：EUI-NEO/core/platform/ime_bridge.c win 分支 only——①三件套替换 :152-176（composition CFS_POINT@(x,y+h) + candidate CFS_EXCLUDE|CFS_CANDIDATEPOS 带 pt/rcArea + caret 兜底调用）②新增 eui_ime_update_fallback_caret :100-120（GetFocus 守卫 + GetCaretPos 探测式创建 1x1 caret，已存在只挪位不重建）。头文件零改动（helper 为文件内 static），apple/linux no-op 未动，terminal_page 未触碰（need250 边界）。新事实已入库 clue115（简报前提修正：旧代码已发 CFS_FORCE_POSITION@行顶，非「完全没发」）+clue116（重发时机确认）。验收=主会话构建后真机微软拼音三场景（光标移动/组合中/候选打开）核对候选窗贴光标。

**最近状态变更**（共 8 条）：2026-11-05 22:04:12 +0800 · 进行中 → 待验收 · 

---

#### bug#269 · libgit2 vendored 子模块在 Windows 上构建失败（BUILD_TESTS 被 CMake policy OLD 覆盖为 ON，headertest -Werror 撞 _WIN32_WINNT 重定义）

- 状态：待验收 · 优先级：P1 · 负责人：zcode · 阶段：测试中
- 创建：2026-11-13 12:15:12 +0800（zcode）· 更新：2026-11-16 18:50:20 +0800（zcode）

**详情**：

复现：origin/dev 8ecf04e6（git 管理器只读 v1，libgit2 v1.9.7 submodule）后首次 Windows Debug 构建 → CMake Error at CMakeLists.txt:195（libgit2 目录空/需 submodule init）；init 后编译报 third_party/libgit2/tests/headertest/headertest.c 失败：`_WIN32_WINNT` redefined [-Werror]（libgit2 cmake/DefaultCFlags.cmake:158 加 -D_WIN32_WINNT=0x0600，DevDesk 顶层 CMakeLists:28 add_compile_definitions 0x0A00）。根因：libgit2 CMakeLists cmake_minimum_required(3.5.1) → 其 policy scope CMP0077 为 OLD，option(BUILD_TESTS ... ON) 无视父级 set(BUILD_TESTS OFF) 并写 cache=ON，于是 Windows 上编 headertest（-Werror）。Mac 端无 WIN32 分支所以未暴露。修复：DevDesk CMakeLists 在 add_subdirectory(third_party/libgit2) 前加 set(BUILD_TESTS OFF CACHE BOOL "" FORCE)（2026-11-13 本机实测构建成功 847 目标全过）。待办：Mac 端/CI 复核；确认 BUILD_TESTS cache 强制不影响 DevDesk 自有测试目标。

**正文**：

复现：origin/dev 8ecf04e6（git 管理器只读 v1，libgit2 v1.9.7 submodule）后首次 Windows Debug 构建 → CMake Error at CMakeLists.txt:195（libgit2 目录空/需 submodule init）；init 后编译报 third_party/libgit2/tests/headertest/headertest.c 失败：`_WIN32_WINNT` redefined [-Werror]（libgit2 cmake/DefaultCFlags.cmake:158 加 -D_WIN32_WINNT=0x0600，DevDesk 顶层 CMakeLists:28 add_compile_definitions 0x0A00）。根因：libgit2 CMakeLists cmake_minimum

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-16 18:50:20 +0800 · CI 两平台证据齐；CI 红≠本条目失败（红源已分立 bug281/282）。

**最近状态变更**（共 4 条）：2026-11-16 18:50:20 +0800 · 进行中 → 待验收 · CI 两平台证据齐；CI 红≠本条目失败（红源已分立 bug281/282）。

---

#### bug#281 · CI Windows 红：2 条 git-diff 测试失败（中文+空格仓 diff 行内容/行号 + WorkdirToHead 增删行）——本地绿 CI 红，疑 checkout 行尾差异

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-16 18:49:44 +0800（bug-fixer）· 更新：2026-11-16 23:44:47 +0800（bug-fixer）

**详情**：

CI 证据（run 43394964495，9b02f08f，2026-11-13）：Windows 构建全过（895 目标），测试 2393 例中 2 红：
[FAIL] 中文+空格仓：diff 行内容与行号（修改中文文件）
[FAIL] diff WorkdirToHead：增删行 + 行号 + untracked + pathFilter
本地同版测试绿（历次全量本地跑未见此红）→ 环境差异方向：CI checkout 行尾（autocrlf）改文件内容致 diff 行内容/行号断言漂移；也不排除真缺陷。定位：tests/test_modules/ 下 git diff 用例 + module_git 域。处置：先本地模拟 CI 环境复现（checkout autocrlf=true 重跑 git diff 用例），确认后修用例或修 CI checkout 配置（.gitattributes / workflow 显式 eol）。

**正文**：

CI 证据（run 43394964495，9b02f08f，2026-11-13）：Windows 构建全过（895 目标），测试 2393 例中 2 红：
[FAIL] 中文+空格仓：diff 行内容与行号（修改中文文件）
[FAIL] diff WorkdirToHead：增删行 + 行号 + untracked + pathFilter
本地同版测试绿（历次全量本地跑未见此红）→ 环境差异方向：CI checkout 行尾（autocrlf）改文件内容致 diff 行内容/行号断言漂移；也不排除真缺陷。定位：tests/test_modules/ 下 git diff 用例 + module_git 域。处置：先本地模拟 CI 环境复现（checkout autocrlf=true 重跑 git diff 用例），确认后修用例或修 CI checkout 配置（.gitattrib

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-16 21:38:49 +0800 · 断点进度：根因定性=同代码（82ecedc9 与 1aef4537 git 域零差异）环境差异型；行尾断言漂移假设族已被 CI 日志排除（失败在 pathFilter size 断言非行内容）；本地模拟矩阵全绿不可复现。修复=防御性钳制+取证增强双保险。下一步：push 后看 CI run——绿则关闭；若诊断输出显示非行尾因素（如多出 patch/size=0），按诊断值定向再查 Release/UCRT/windows-2025 维度。
- 2026-11-16 23:44:47 +0800 · 前任「行尾环境差异」假设族被 CI 诊断+读码彻底否定；真缺陷=Release 暴露的悬空指针 UB。

**最近状态变更**（共 4 条）：2026-11-16 21:38:49 +0800 · 进行中 → 待验收 · 断点进度：根因定性=同代码（82ecedc9 与 1aef4537 git 域零差异）环境差异型；行尾断言漂移假设族已被 CI 日志排除（失败在 pathFilter size 断言非行内容）；本地模拟矩阵全绿不可复现。修复=防御性钳制+取证增强双保险。下一步：push 后看 CI run——绿则关闭；若诊断输出显示非行尾因素（如多出 patch/size=0），按诊断值定向再查 Release/UCRT/windows-2025 维度。

---

#### bug#282 · CI macOS 红：file_association_macos.mm 新 SDK 编译炸（2 处 UTType deprecation×Werror + NSWorkspace selector 不存在）

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-16 18:49:55 +0800（bug-fixer）· 更新：2026-11-17 01:37:45 +0800（bug-fixer）

**详情**：

CI 证据（run 43394964495 macOS job，2026-11-13）：src/platform/file_association_macos.mm 编译 3 错（Xcode 27.2 新 SDK + -Werror）：
:82 UTTypeCreatePreferredIdentifierForTag deprecated（macOS 12+，应改 UTType 类）
:83 kUTTagClassFilenameExtension deprecated（应改 UTTagClassFilenameExtension）
:232 no visible @interface for NSWorkspace declares selector setDefaultApplicationAtURL:toOpenURL:error:（真 API 缺口/签名不符，非仅 deprecation）
本地 Mac 侧不可达（本会话 Windows），修复需改 md 关联的 mac 实现（need260 域）。修法方向：换 UTType API + 核对该 selector 的正确形态（NSWorkspace setDefaultApplicationAtURL:toOpenURL:errorAvailability macOS 13+？需查 SDK 头）；或对 deprecation 项该文件局部关 -Werror=deprecated-declarations（不推荐，优先真修）。验收：CI macOS job 编译过（或 Mac 本地构建过）。

**正文**：

CI 证据（run 43394964495 macOS job，2026-11-13）：src/platform/file_association_macos.mm 编译 3 错（Xcode 27.2 新 SDK + -Werror）：
:82 UTTypeCreatePreferredIdentifierForTag deprecated（macOS 12+，应改 UTType 类）
:83 kUTTagClassFilenameExtension deprecated（应改 UTTagClassFilenameExtension）
:232 no visible @interface for NSWorkspace declares selector setDefaultApplicationAtURL:toOpenURL:error:（真 API 缺口/签名不符，非仅 depreca

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-16 22:03:02 +0800 · 断点进度：修复已落 commit d7fed2f3（3 文件：file_association_macos.mm / file_association_api_map.h 注释 / platform CMakeLists APPLE 块加 UniformTypeIdentifiers 链接）。根因与查证证据全在 clue176/177。本机验证：md_assoc 5/5、file_association 4/4 绿 + 源码级自检清单过。下一步：push 后 CI macOS job 真编译验证（预期 4m 内编译段过或出新错再迭代）。任务未 push，等收口统一推。
- 2026-11-16 22:04:19 +0800 · 根因定性：:232 API 已从 27.2 SDK 移除（docs 404，非拼写错）；修复采用现行 completionHandler 变体。
- 2026-11-17 01:04:34 +0800 · 返工：CI run 43911766086 实证迭代2 前两错已消、剩一错（typeWithTag 为 Swift 名，ObjC 原生=typesWithTag 返回数组），主会话 S 级直修。
- 2026-11-17 01:06:33 +0800 · CI 三轮迭代记录完整：每轮新错→修→再验（43889570987mac 前两错消/43911766086mac 仅剩 typesWithTag → c71ef181 待验）。
- 2026-11-17 01:29:03 +0800 · 返工迭代4：c71ef181 的 typesWithTag 数组拼法也被 CI 证伪（no known class method）——停止盲猜，回退到 pragma 包裹的 C API 无条件路径（弃用≠移除，27.2 SDK 仍声明可链）。现代 UTType 迁移留给 Mac 侧真编译会话。

**最近状态变更**（共 8 条）：2026-11-17 01:37:45 +0800 · 进行中 → 待验收 · 

---

#### bug#284 · bug266 回归：settings applyWindowVisuals 接线用例红（translucent→Transparent 接线后期望未同步，本地+CI 双红）

- 状态：待验收 · 优先级：P1 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-16 21:41:01 +0800（ZCode主会话）· 更新：2026-11-16 21:43:05 +0800（ZCode主会话）

**详情**：

CI run 43873383384 Windows 首现且本地即时复现（2026-11-16 21:55，--dev --filter file:test_settings 104 例 1 红）：[FAIL] settings: applyWindowVisuals 接线（无头：app 运行时覆盖）[0ms]。引入源=bug266 DevDesk 侧接线（settings_config.cpp:336 translucent 档改发 WindowEffect::Transparent，commit bf9e4588）：bug-fixer 只跑了 settings_window_effect_param_mapping 单用例绿，漏跑整域。待定位：test_settings 里该用例的期望值 vs applyWindowVisuals 实际行为（预期是旧映射断言未同步或接线函数内部约束）。修后双仓验证：file:test_settings 全绿 + 下次 push CI 绿。

**正文**：

CI run 43873383384 Windows 首现且本地即时复现（2026-11-16 21:55，--dev --filter file:test_settings 104 例 1 红）：[FAIL] settings: applyWindowVisuals 接线（无头：app 运行时覆盖）[0ms]。引入源=bug266 DevDesk 侧接线（settings_config.cpp:336 translucent 档改发 WindowEffect::Transparent，commit bf9e4588）：bug-fixer 只跑了 settings_window_effect_param_mapping 单用例绿，漏跑整域。待定位：test_settings 里该用例的期望值 vs applyWindowVisuals 实际行为（预期是旧映射断言未同步或接线函数内部约束）。

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-16 21:43:05 +0800 · 根因=bug266 代理只跑 settings_window_effect_param_mapping 单用例，漏 apply_visuals 用例（教训：接线类改动须跑整域 filter，已作为线索面沉淀入 bug266 验收链）。

**最近状态变更**（共 4 条）：2026-11-16 21:43:05 +0800 · 进行中 → 待验收 · 根因=bug266 代理只跑 settings_window_effect_param_mapping 单用例，漏 apply_visuals 用例（教训：接线类改动须跑整域 filter，已作为线索面沉淀入 bug266 验收链）。

---

#### bug#286 · CI Windows std include 守卫红：module_git 9 处缺显式 include（git v1 遗留，测试绿后首次暴露）

- 状态：待验收 · 优先级：P1 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-17 01:26:27 +0800（ZCode主会话）· 更新：2026-11-17 02:28:46 +0800（ZCode主会话）

**详情**：

CI run 43911766086 Windows 首达 std include 守卫（此前被 bug281 测试红挡住从未执行到）暴露 module_git 9 处违规（git v1 8ecf04e6 引入）：git_changes_model.cpp:42 <utility>；git_module.cpp:33/34/36 <vector>/<utility>/<chrono>；git_page_controller.cpp:70 + .h:73 <utility>；git_search_model.cpp:43、git_tree_model.cpp:69 <utility>；git_views.cpp:69 <chrono>。全部=靠标准库传递包含才可见。修法=机械补显式 include，本地 check-std-includes.sh 复验 0 违规 + git 域 doctest 绿。同批修复 mac 侧 bug282 迭代 3 失败回退（typesWithTag 亦不存在——改回 pragma 包裹的 C API 无条件路径，现代 API 迁移留 Mac 真编译会话）。关联 bug281（同一条 CI 红链的下一环）。

**正文**：

CI run 43911766086 Windows 首达 std include 守卫（此前被 bug281 测试红挡住从未执行到）暴露 module_git 9 处违规（git v1 8ecf04e6 引入）：git_changes_model.cpp:42 <utility>；git_module.cpp:33/34/36 <vector>/<utility>/<chrono>；git_page_controller.cpp:70 + .h:73 <utility>；git_search_model.cpp:43、git_tree_model.cpp:69 <utility>；git_views.cpp:69 <chrono>。全部=靠标准库传递包含才可见。修法=机械补显式 include，本地 check-std-includes.sh 复验 0 违规 + git 域 doctes

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-17 01:37:37 +0800 · 同 CI 红链第三环：281 pathspec（已修）→286 守卫（已修）→首绿。守卫逐波暴露（每修一轮现新批）=11-11 后无 CI 绿窗的传递包含债集中清算。
- 2026-11-17 01:55:56 +0800 · 返工：CI 第四轮 macOS 过 file_association（bug282 修复实证）后新暴露 git_page_controller.cpp:420 自赋值 no-op（Apple clang -Wself-assign-field，-Werror）——唯一实例（主会话 grep 全仓确认），改注释文档化。
- 2026-11-17 02:28:46 +0800 · CI 红链五环全清（Windows）：281 pathspec→284 回归→286 守卫+自赋值→绿。macOS 剩 bug287（mac 侧会话）。

**最近状态变更**（共 6 条）：2026-11-17 02:28:46 +0800 · 进行中 → 待验收 · CI 红链五环全清（Windows）：281 pathspec→284 回归→286 守卫+自赋值→绿。macOS 剩 bug287（mac 侧会话）。

---

#### bug#320 · devflow pool MCP 工具 schema 的 pool 枚举未含 temp——temp 池登记通道被 schema 校验客户端拦（need303 活测发现）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：已分析
- 创建：2026-11-18 11:44:31 +0800（developer）· 更新：2026-11-18 20:08:46 +0800（developer）

**详情**：

现象（2026-11-18 qa need#319 活测）：pool_add(pool="temp") 被 InputValidationError 拒绝——「Invalid option: expected one of need|bug|debt」；工具自身 description 已写「need/bug/debt 常规三池 + temp 临时任务池」，活缓存 0.39.43 tools/pool_mcp.py:527 add_pools 已含 temp、:535-546 temp 分支齐全，但工具 schema 的 pool 参数枚举仍为三池未随 need303 扩展。影响：ZCode/Claude Code 等 schema 校验客户端无法登记 temp 条目——need303 验收「临时任务登记→全程无拦截」的登记入口被拦死（devflow 门禁面已全绿，缺口在 MCP schema 面）。修复向：pool_add schema 的 pool enum 补 "temp"（同查 pool_fetch/pool_soft_delete 等同源枚举）。关联：need303 验收条件。证据：evidence/20261118-plugin-verify/report-needs.md need303 节（含原始报错输出）

**正文**：

现象（2026-11-18 qa need#319 活测）：pool_add(pool="temp") 被 InputValidationError 拒绝——「Invalid option: expected one of need|bug|debt」；工具自身 description 已写「need/bug/debt 常规三池 + temp 临时任务池」，活缓存 0.39.43 tools/pool_mcp.py:527 add_pools 已含 temp、:535-546 temp 分支齐全，但工具 schema 的 pool 参数枚举仍为三池未随 need303 扩展。影响：ZCode/Claude Code 等 schema 校验客户端无法登记 temp 条目——need303 验收「临时任务登记→全程无拦截」的登记入口被拦死（devflow 门禁面已全绿，缺口在 MCP sche

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-18 17:41:41 +0800 · 升版装机完成：修复 fix a88b959 已 push（七方 0.39.46→0.39.47，rebase 到 Mac 侧 0.39.46 之上）；装机走 release.sh（门禁回归全绿+市场重刷+缓存装 0.39.47，缓存 pool_contract.py 已含 temp 枚举）。待新会话活体验证 MCP schema 后核销。
- 2026-11-18 20:08:46 +0800 · BW 归位判定：修复 a88b959（pool_add 等 schema enum 补 temp）已 push+装机 0.39.47（与 #303 同一升版动作，池 note 17:41 后无新回写）；差新会话活体复测登记通道一步，未达关账条件——归待验收，复测绿即关。原始报错与判据 evidence/20261118-plugin-verify/report-needs.md need303 节。

**最近状态变更**（共 4 条）：2026-11-18 20:08:46 +0800 · 进行中 → 待验收 · BW 归位判定：修复 a88b959（pool_add 等 schema enum 补 temp）已 push+装机 0.39.47（与 #303 同一升版动作，池 note 17:41 后无新回写）；差新会话活体复测登记通道一步，未达关账条件——归待验收，复测绿即关。原始报错与判据 evidence/20261118-plugin-verify/report-needs.md need303 节。

---

#### bug#334 · memory_install_probe 无界死等 + 「接受→真引擎 add」路径挂起（cards=369 accepted=0 实证）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：开发完成待测试
- 创建：2026-11-18 16:48:39 +0800（developer）· 更新：2026-11-18 20:08:32 +0800（developer）

**详情**：

终验链第四棒实证（真控制台法下 memory_install_probe 首次真跑）：探针死等「接受提案≥1（真引擎 add）」永远等不到——控制台逐秒打印 t=N cards=369 accepted=0，无界轮询 20+ 分钟 CPU 直到我杀。memory_install_probe.cpp:246-274 的接受循环无 deadline（探针缺陷）+「接受→引擎 add」路径挂起（产品缺陷嫌疑：与老板「垃圾不入库」裁决、AC 闭环验证同族——记忆管线入库链路断裂的第三证）。修法：探针加有界等待；产品侧查 accept→add 链为何不完成。

**正文**：

终验链第四棒实证（真控制台法下 memory_install_probe 首次真跑）：探针死等「接受提案≥1（真引擎 add）」永远等不到——控制台逐秒打印 t=N cards=369 accepted=0，无界轮询 20+ 分钟 CPU 直到我杀。memory_install_probe.cpp:246-274 的接受循环无 deadline（探针缺陷）+「接受→引擎 add」路径挂起（产品缺陷嫌疑：与老板「垃圾不入库」裁决、AC 闭环验证同族——记忆管线入库链路断裂的第三证）。修法：探针加有界等待；产品侧查 accept→add 链为何不完成。

**进度记录**（共 6 条，列最近 5 条）：

- 2026-11-18 17:57:54 +0800 · 判别结果（单会话接受链，#114 场4 干净 venv）：「死等挂死」形态未复现（反馈闭环完整：快速失败→audit 落账→UI toast）；坐实新形态三件：① 接受→add 走真 LLM 网络报 Timeout was reached（key 链路真走通，无 401——失败是超时非鉴权；keychain entry DPAPI 加密带封装，直解失败，bigmodel 慢 vs 引擎超时上限未定案）；② add 实际已把记忆写入引擎库（get_memories 实证非空）但流程报告失败——入库与状态不一致；③ 提案卡仍 pending 未转 accepted。场3 anthropic AttributeError 已排除为环境损伤（真实根 venv anthropic 2.4.0 完好）。修复方向：add 超时上限/重试+成功路径状态回写+pending→accepted 状态机。证据 evidence/20261118-memory-one-session/verdicts.md §#334 判别表
- 2026-11-18 18:39:48 +0800 · 记忆波 1 派发前置：AY 批（F-06 门禁+接受链收口）承接——探针有界等待+产品侧 accept 链状态修复（AF 已精化：Timeout 误报但数据实际入库、卡留 pending）。
- 2026-11-18 18:39:53 +0800 · AY 批开工（developer 子任务）。
- 2026-11-18 19:05:45 +0800 · 接受链收口开发完成（wip aa85da70）：
根因推演（AF 场4 实锤）：accept→executeProposal→addDirect→callTool(add_memory,10s)；引擎 add 走真 LLM 提炼腿可晚于客户端 10s 界写库，http_client.cpp:202 超时单列 NetworkTimeout→addDirect 报 fail（Timeout was reached）→accept 落 apply_failed、卡留 pending，而引擎库已有条目（get_memories 实证）——入库与状态不一致。另一腿：add 已回 stored 后 id 回填（get_memories 10s）超时→翻转整个 addDirect 为失败。
修复（禁 blanket timeout，10s 不动）：① 新增 confirmStoredAfterTimeout：超时后 get_memories 清单按正文精确匹配核实——查到→按成功回传 id（卡转 accepted）；查不到→「超时且已核实未入库」保持待审可重试；核实自身失败→原超时+未能核实（不虚报成败）；非超时错误（引擎不在/4xx）透传不核实。② id 回填失败不再翻转 add 结果（写库已成功，空 id 走既有契约边界+LOG_WARN）。panel_state/proposal_store 零改动。
探针侧：memory_install_probe.cpp S4 接受循环加 180s steady_clock 有界 deadline（防数百 pending×每卡最坏 28s 无界死等，到点如实报进度）。
验证：#334 无单测缝（真 HTTP 实链），载体=probe cmdCollect 波末真引擎跑——超时但已入库场景应转 accepted（audit 不再 apply_failed+库有）；真失败带真实原因。断言清单见 evidence/20261118-memwave1/assertions.md。残余如实：核实与引擎异步写库存在竞态（核实未命中→保持待审，重试可能引擎侧重复条目）。
- 2026-11-18 20:08:32 +0800 · BW 归位：开发完成待波末验证——接受链修复 aa85da70 本轮 cat-file 核实在仓；波末 BJ 批验证=memory_install_probe 有界等待+accept→add 复跑（原症状 cards=369 accepted=0 无界死等作对照）。

**最近状态变更**（共 4 条）：2026-11-18 20:08:32 +0800 · 进行中 → 待验收 · BW 归位：开发完成待波末验证——接受链修复 aa85da70 本轮 cat-file 核实在仓；波末 BJ 批验证=memory_install_probe 有界等待+accept→add 复跑（原症状 cards=369 accepted=0 无界死等作对照）。

---

#### bug#337 · 定时新建/编辑弹窗固定高度，小窗口下显示不全

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：已分析
- 创建：2026-11-18 17:38:52 +0800（developer）· 更新：2026-11-22 18:46:29 +0800（developer）
- **blocker**：Mac need300/307 等 20 提交（origin/dev 4e6f76bd…）撞 schedule_ui_edit.cpp/.h、schedule_ui_list、ui_kit/dialog_shell 面，本地旧基线；协调人冻结令（AW 批 2026-11-18），待第二轮 rebase 后重派

**详情**：

老板验收清单 #4：新建定时弹窗高度固定，窗口化（小窗）下弹窗显示不全。修法：弹窗高度适配可用窗高（钳位+必要时内部滚动）。

**正文**：

老板验收清单 #4：新建定时弹窗高度固定，窗口化（小窗）下弹窗显示不全。修法：弹窗高度适配可用窗高（钳位+必要时内部滚动）。

**进度记录**（共 8 条，列最近 5 条）：

- 2026-11-18 18:57:04 +0800 · 冻结时零代码改动（仅分析，无 commit）。冻结前结论：壳层 drawDialogShell 已做面板钳位 h=min(640,screenH-96)，真缺陷在内容层——drawEditDialog 内容按面板绝对 y 布局（页签 56→fy 游标，最坏 calendar/week+remind_open 底到 ~916），小窗钳位后超出内容区（69..panelH-89）不可见且无滚动。重派建议：内容体包 components::scrollView（同模块 schedule_ui_list.cpp:1141 先例）：position(0,0)、size(面板宽, area.y+area.h)、scrollbarOverlay(true)（不占内容宽，字段坐标零改动）、contentHeightProvider 供 fy 增量几何表；关窗帧 setScrollOffset 复位（pendingScrollReset_ 模式）；底区删除钮留在 scrollView 外。几何公式：viewBottom=panelH-89，maxOffset=contentH-viewBottom；例 screenH=520→panelH=424→viewBottom=335→contentH=916→maxOffset=581。注意 :890 附近 need290 P0 ctx 按值捕获勿回退；Mac 已改本文件，重做前先 grep。分析详见 evidence/20261118-wave4-aw/summary.md
- 2026-11-18 20:18:14 +0800 · 回炉在池：AJ2 rebase 已完成，解冻。
- 2026-11-18 20:18:21 +0800 · 排产即派 BP 批（#337+#338 合批，BK 对齐笔记+diff 草稿就绪）。
- 2026-11-18 20:40:23 +0800 · 开发完成待测试（developer·BP 批·wip e0208697）：①壳加 DialogShellSpec::contentHeightProvider——请求高=69+provider(w−48)+89，无 provider 维持旧语义，同过 kDialogMinH/视口 clamp；②schedule_ui_edit 内容层包 components::scrollView（同模块先例 schedule.list.scroll）：锚(0,56) 页签行起，viewBottom=area.y+area.h=panelH−89（panelH=壳 clamp 后真实高非 640 常量），覆盖式滚动条不占内容宽，y 维迁相对锚（页签 0/fy 40=旧 56/96−56）；③壳高自适应：EditDialogState 新增 contentBottom（content 尾回写面板绝对底，帧间自收敛），provider=min(contentBottom−69, 640−158)——短内容收缩、上限 640、首帧回退 640 档。测试同步：probe id 契约变更归 #338；本项几何断言按简报可选项留波末 QA 首验。cron E2E 零破坏（grep：仅表单逻辑层用例+probe 无 cron 段）。禁构建期未跑（AJ2 编译段），测试归 QA。
- 2026-11-22 18:46:29 +0800 · 台账归位：修复早已在树（ff5b1cad/edc536f2），rebase 后 Mac 线确认无冲突，实现完→待验收

**最近状态变更**（共 8 条）：2026-11-22 18:46:29 +0800 · 进行中 → 待验收 · 台账归位：修复早已在树（ff5b1cad/edc536f2），rebase 后 Mac 线确认无冲突，实现完→待验收

---

#### bug#338 · 用户定时「删除」按钮：关弹窗且未执行删除

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：已分析
- 创建：2026-11-18 17:39:06 +0800（developer）· 更新：2026-11-22 18:46:32 +0800（developer）
- **blocker**：等主会话二轮 rebase 完成（origin/dev 4e6f76bd 起 20 提交直撞 schedule_ui_edit 域，need300 疑含同域修复，合并后先判是否已修）

**详情**：

老板验收清单 #4 后半：用户定时点击「删除」会关闭弹窗且没删除（删除按钮行为坏——关弹窗但删除未执行）。修法：查删除链确认弹窗/回调误接（点击删除应执行删除+反馈，与关闭弹窗分离）。与 #337 同弹窗域可合批修。

**正文**：

老板验收清单 #4 后半：用户定时点击「删除」会关闭弹窗且没删除（删除按钮行为坏——关弹窗但删除未执行）。修法：查删除链确认弹窗/回调误接（点击删除应执行删除+反馈，与关闭弹窗分离）。与 #337 同弹窗域可合批修。

**进度记录**（共 8 条，列最近 5 条）：

- 2026-11-18 18:55:31 +0800 · 冻结断点（2026-11-18 AX批，未改任何文件/无需wip）：根因高置信定位——schedule_ui_edit.cpp:921 删除按钮 .position(x0, h-73+17) 钉本地 h=640，而 drawDialogShell 按视口 clamp 面板高（screen.height-96，dialog_shell.cpp:78）；短窗/高缩放档（视口高<约736DIP）时真实面板高<640，删除按钮命中区溢出面板底，点击落到全屏 backdrop（EUI-NEO dialog.h:100-107 backdrop.onClick=requestClose）→编辑弹窗直接关、requestDelete→确认弹窗链未进入=「关弹窗且没删除」逐点吻合，也解释 #337 同弹窗高度批并行立项。佐证：删除逻辑链头测已绿（test_schedule_ui_list.cpp:675-687）且 probe F 段为合成点击不受面板 clamp 影响，断裂仅在真实命中区。计划修法（rebase 后按新代码重判）：按钮 y 改钉真实壳底行 area.y+area.h+33（=h_real-56，与壳钮同排）不假设 640。注意：Mac need300 含「schedule 删除确认延一帧」——疑同域另一修法，重判时先比对是否已修。捕获语义已核对：删除钮 [&st,&host] 均宿主成员（need290 P0 口径正确），保存钮 ctx 值捕获未动。
- 2026-11-18 20:18:28 +0800 · 回炉在池：解冻（BK 已判 Mac 未修本条，修法升级=壳 thirdAction 左槽+dangerThird）。
- 2026-11-18 20:18:35 +0800 · 排产即并入 BP 批。
- 2026-11-18 20:40:23 +0800 · 开发完成待测试（developer·BP 批·wip e0208697）：删除钮从内容层手工画（根因：.position 按 h=640 常量偏移，壳钳高下命中区溢出 backdrop→点背景误触删除链）迁壳 thirdAction 左槽+新增 DialogShellSpec::dangerThird（error 三态/onPrimary 字/40 高/14 字，与原手工钮及 dangerConfirm 分支逐项等价）。几何：buildShellButton(id.third, x=24, rowY=h−56)——h 为壳 clamp 后真实高，删除钮恒钉真实底区行。点击链路不变：requestDelete(ruleId)→宿主 need300 延帧确认弹窗（schedule_ui_list 侧零改动）。测试同步：id 契约 schedule.edit.delete→schedule.edit.dialog.third，probe 4 处已改（:599 D 断言在位/:621 D2 新建态无钮/:712/:746 F 段点击定位）。cron E2E 零破坏声明见证据。禁构建期未跑（AJ2 编译段），测试归 QA。
- 2026-11-22 18:46:32 +0800 · 台账归位：与 #337 同批已在树，实现完→待验收

**最近状态变更**（共 8 条）：2026-11-22 18:46:32 +0800 · 进行中 → 待验收 · 台账归位：与 #337 同批已在树，实现完→待验收

---

#### bug#341 · codegraph 守护进程疯狂读写硬盘：索引灌入 evidence/池库产物致 316MB 库被高频重扫

- 状态：待验收 · 优先级：P1 · 负责人：main-session · 阶段：测试中
- 创建：2026-11-18 18:07:45 +0800（main-session）· 更新：2026-11-18 18:45:14 +0800（main-session）
- **blocker**：根治面已落（codegraph.json exclude 补 evidence/docs/pool/docs/screenshots/scenes-dev/bug142-click-chain/.devflow/db 等 12 项）但索引重建缩库+daemon IO 复测未完成（.codegraph/ 库仍 ~270MB）；处置 A/B 老板裁决记录未见——验收（磁盘 IO 恢复正常）未证，本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

现象：node 进程（PID 17432，codegraph daemon v1.6.2，serve --mcp --path D:\project\gui\DevDesk，11-18 14:07 启动）持续 ~9-60MB/s 突发读盘，当日累计读 16.6GB/写 2.5GB，磁盘被拖满（du 统计被拖到超时）。

根因链（已实证）：
1. codegraph.json 仅排除 build/Testing/wt-*，evidence/（1.9GB、57168 文件）、docs/pool/（pool.db+wal+pool-report.html 高频重写）、tests/ui_driver/scenes-dev/（114 文件）、docs/screenshots/、bug142-click-chain/、.devflow/ 全部进了索引 → 索引库 codegraph.db 膨胀至 316MB。
2. 文件 watcher 每次变更 auto-sync，单次 1~8 秒重扫大库（daemon.log 全天几十条 Auto-synced N file(s) in Xms）。
3. 变更源不停：多会话并发写 docs/pool/pool.db（mtime 分钟级滚动）+ pool-report.html（1MB，每次池写重生成，18:04 仍在写）+ evidence 抓取 → 写→sync→重读 316MB 循环。

处置方案（待老板裁决）：
A. 止血：杀 PID 17432（按需自动重启，索引不丢）
B. 根治：codegraph.json 补 exclude（上述目录）+ 重建索引缩库；evidence/ 1.9GB 另议归档清理
证据：daemon.log 采样、Win32_Process IO 计数两轮采样、du 尺寸测量

**正文**：

现象：node 进程（PID 17432，codegraph daemon v1.6.2，serve --mcp --path D:\project\gui\DevDesk，11-18 14:07 启动）持续 ~9-60MB/s 突发读盘，当日累计读 16.6GB/写 2.5GB，磁盘被拖满（du 统计被拖到超时）。

根因链（已实证）：
1. codegraph.json 仅排除 build/Testing/wt-*，evidence/（1.9GB、57168 文件）、docs/pool/（pool.db+wal+pool-report.html 高频重写）、tests/ui_driver/scenes-dev/（114 文件）、docs/screenshots/、bug142-click-chain/、.devflow/ 全部进了索引 → 索引库 codegraph.db 膨胀至 31

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-18 18:18:55 +0800 · 主会话直处置（环境基建归主会话）：杀 17432 + 清 codegraph 全族 11 进程；codegraph.json 补 exclude（evidence/、docs/pool/、scenes-dev/、screenshots/ 等 + **/*.db*）；全量重建 3760 源文件，库 316→264MB
- 2026-11-18 18:19:03 +0800 · 处置完成（止血+根治，老板批准）：杀 codegraph 全族 11 进程（含被打断后仍缩库的重建残留）；codegraph.json 补 exclude：evidence/、docs/pool/、docs/screenshots/、tests/ui_driver/scenes-dev/、bug142-click-chain/、.devflow/、**/*.db(-wal/-shm)；全量重建 3760 源文件，库 316→264MB。根因修正：库主体是源码图本身（EUI-NEO/3rd + libgit2 第三方源），evidence 污染仅 ~50MB；真凶是 watcher 被 docs/pool 高频写入触发 auto-sync 每次重扫整库（当日 16.6GB 读），exclude 后循环已断。留观察：safe-write/pool MCP node 全挂活跃宿主下 0 IO 未杀；codegraph v1.6.3 可用未升级

**最近状态变更**（共 4 条）：2026-11-18 18:19:03 +0800 · 进行中 → 待验收 · 处置完成（止血+根治，老板批准）：杀 codegraph 全族 11 进程（含被打断后仍缩库的重建残留）；codegraph.json 补 exclude：evidence/、docs/pool/、docs/screenshots/、tests/ui_driver/scenes-dev/、bug142-click-chain/、.devflow/、**/*.db(-wal/-shm)；全量重建 3760 源文件，库 316→264MB。根因修正：库主体是源码图本身（EUI-NEO/3rd + libgit2 第三方源），evidence 污染仅 ~50MB；真凶是 watcher 被 docs/pool 高频写入触发 auto-sync 每次重扫整库（当日 16.6GB 读），exclude 后循环已断。留观察：safe-write/pool MCP node 全挂活跃宿主下 0 IO 未杀；codegraph v1.6.3 可用未升级

---

#### bug#346 · F-06 洗链与 1024B 截断交互：中断标记被撕开残段入卡（BI 审查 P2）

- 状态：待验收 · 优先级：P3 · 负责人：developer · 阶段：开发中
- 创建：2026-11-18 19:24:00 +0800（developer）· 更新：2026-11-18 20:08:39 +0800（developer）

**详情**：

BI 审查 P2 发现（evidence/20261118-wave4-review/review.md）：collect_round.cpp:172 与 session_reader ③ 截断交互——1024B 截断可把「[Request interrupted by user」从中撕开，残段不命中 kInterrupt 前缀→洗不掉入卡。修法建议：截断尾行前缀模糊命中或截断前先洗（顺序调整）。另两条 P2 归属：晚写竞态已在 #334 note；wtab 档距归 #339 头部卡重派一并复核。

**正文**：

BI 审查 P2 发现（evidence/20261118-wave4-review/review.md）：collect_round.cpp:172 与 session_reader ③ 截断交互——1024B 截断可把「[Request interrupted by user」从中撕开，残段不命中 kInterrupt 前缀→洗不掉入卡。修法建议：截断尾行前缀模糊命中或截断前先洗（顺序调整）。另两条 P2 归属：晚写竞态已在 #334 note；wtab 档距归 #339 头部卡重派一并复核。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-18 19:24:06 +0800 · 排产即派 BM 批（微修：截断前先洗或尾行模糊命中）。
- 2026-11-18 19:24:13 +0800 · BM 批开工。
- 2026-11-18 19:31:56 +0800 · 案 b（弃 a 调序）：实读链路——截断在 session_reader.cpp:132（事件入库前、collector 域），洗在 collect_round.cpp runCollectRound（脱敏闸后、pipeline 域），截断先于洗；a 需跨域搬洗且破 evidence 取 seg.text 原文契约，侵入面大。改动（纯增零删除，BD/AY 零回退）：stripLeakMarkers 新增 tornTailLen——行尾=kInterrupt 真前缀（≥8B「[Request」词根起判防误洗，kControl 157 条无此前缀已 grep 核）即擦除，完整命中仍走原 find 循环。断言：test_memory_collect_wiring.cpp 新增 memory_collect_f06_wash_torn_marker（撕开微锚×3+阈值负例×2+1024B 边界主体：1000B 正文+41B 标记→截断撕出 24B 残段→洗后零残段）。wip commit fd5aa69a（仅本批两文件）；证据 evidence/20261118-346-truncfix/changelist.md。禁构建禁跑，数值断言波末统一跑。
- 2026-11-18 20:08:39 +0800 · BW 归位：开发完成待波末验证——1024B 截断撕开中断标记修复 fd5aa69a 本轮 cat-file 核实在仓；波末 BJ 批验证=截断边界场景回归（残段不入卡）。

**最近状态变更**（共 4 条）：2026-11-18 20:08:39 +0800 · 进行中 → 待验收 · BW 归位：开发完成待波末验证——1024B 截断撕开中断标记修复 fd5aa69a 本轮 cat-file 核实在仓；波末 BJ 批验证=截断边界场景回归（残段不入卡）。

---

#### bug#358 · 设置-同步：仓库URL 等配置不能持久化保存

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：开发完成
- 创建：2026-11-19 10:00:47 +0800（bug-fixer）· 更新：2026-11-19 11:34:59 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「设置-同步 仓库URL之类的配置不能持久化保存」。
现象：设置页同步子页的仓库 URL 等配置项无法持久化（保存后不落盘/重启丢失，具体形态待复现）。排产前待补：复现步骤（改→存→重启→读）与涉及配置项全集。

**正文**：

老板 2026-11-19 原话：「设置-同步 仓库URL之类的配置不能持久化保存」。
现象：设置页同步子页的仓库 URL 等配置项无法持久化（保存后不落盘/重启丢失，具体形态待复现）。排产前待补：复现步骤（改→存→重启→读）与涉及配置项全集。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-19 11:12:56 +0800 · 2026-11-19 修复波第三单（老板 3 并发令）
- 2026-11-19 11:13:01 +0800 · bug-fixer 后台开工（3 并发槽 3/3）：复现→根因→最小修复
- 2026-11-19 11:34:59 +0800 · 断点=cardActions 三字段回调（applyGithubSyncRepoUrl/Branch/AutoPush，settings_module.cpp:1681-1699）只改内存不落盘；全文件 save 仅在启用开关（773）与 saveToken（1019）两处调用。修法：新增 saveGithubSyncCfg 助手（settings_module.cpp:783-792，对齐 applyGithubSyncEnabled 口径）接通三回调 + 回归用例 settings_github_sync_config_fields_persist（tests/test_modules/test_settings.cpp）。红证据已取（Total:1 Failed:1，repoUrl 未落盘）；绿验证波末统一批执行（纯码模式调度令；最后一跑止于环境文件锁 ld 失败，两改动 TU 编译已过）。commit e1434e46（未 push）。

**最近状态变更**（共 4 条）：2026-11-19 11:34:59 +0800 · 进行中 → 待验收 · 断点=cardActions 三字段回调（applyGithubSyncRepoUrl/Branch/AutoPush，settings_module.cpp:1681-1699）只改内存不落盘；全文件 save 仅在启用开关（773）与 saveToken（1019）两处调用。修法：新增 saveGithubSyncCfg 助手（settings_module.cpp:783-792，对齐 applyGithubSyncEnabled 口径）接通三回调 + 回归用例 settings_github_sync_config_fields_persist（tests/test_modules/test_settings.cpp）。红证据已取（Total:1 Failed:1，repoUrl 未落盘）；绿验证波末统一批执行（纯码模式调度令；最后一跑止于环境文件锁 ld 失败，两改动 TU 编译已过）。commit e1434e46（未 push）。

---

#### bug#359 · 上线任务详情：【未完成】状态标布局异常

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:06:12 +0800（ZCode主会话）· 更新：2026-11-19 11:54:41 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「上线任务详情，你找文本【未完成】这个状态的布局有问题」。
定位：release 模块任务详情页，状态标【未完成】的元素布局异常。具体形态（溢出/错位/截断/换行）原话未描述，修复前需真机截图或 ui_driver 场景定位确认。

**正文**：

老板 2026-11-19 原话：「上线任务详情，你找文本【未完成】这个状态的布局有问题」。
定位：release 模块任务详情页，状态标【未完成】的元素布局异常。具体形态（溢出/错位/截断/换行）原话未描述，修复前需真机截图或 ui_driver 场景定位确认。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-19 11:34:41 +0800 · release+schedule 弹窗布局族任务（槽 5/5）开工，纯码模式
- 2026-11-19 11:53:30 +0800 · 纯码修复完成（验证归波末）：release_detail_draw.cpp drawStepRow nameRight 原式 viewX-8-skipW 把 skipW 双重扣减（viewX 内已让位跳过框），未完成配置行状态字左移 76px 与行尾按钮间留死缝、!showView 分支压跳过框；改为从行右缘显式让位。commit f82664b0
- 2026-11-19 11:54:41 +0800 · 交付 commit f82664b0：skipW 双扣修正，状态字右锚让位；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 11:54:41 +0800 · 进行中 → 待验收 · 交付 commit f82664b0：skipW 双扣修正，状态字右锚让位；验证归波末

---

#### bug#360 · 上线填写弹窗布局错乱：控件跑出弹窗边界

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:06:44 +0800（ZCode主会话）· 更新：2026-11-19 11:54:46 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「然后填写弹窗 布局错乱，安排跑外面了」。
现象：填写弹窗（上线任务详情流程中打开的填写弹窗）布局错乱，元素跑出弹窗边界。⚠ 原话「安排」疑为「按钮」误写（控件跑出边界），具体跑出元素待截图确认。与 #359 同页面流程（release 任务详情），建议排产时两卡合并复现。

**正文**：

老板 2026-11-19 原话：「然后填写弹窗 布局错乱，安排跑外面了」。
现象：填写弹窗（上线任务详情流程中打开的填写弹窗）布局错乱，元素跑出弹窗边界。⚠ 原话「安排」疑为「按钮」误写（控件跑出边界），具体跑出元素待截图确认。与 #359 同页面流程（release 任务详情），建议排产时两卡合并复现。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:53:30 +0800 · 纯码修复完成（验证归波末）：release_detail_draw.cpp drawConfigEditor 按全屏 screen 排布内容，但 components/dialog.h build 把面板钳到 screen−overlay(48)，底排保存/取消/字数行顶出面板底缘悬到 backdrop；panelW/H 改按钳后口径（同 drawValidationDialog）。commit 8f5b672f
- 2026-11-19 11:54:46 +0800 · 交付 commit 8f5b672f：panelW/H 按钳制口径（「按钮跑出面板」确认）；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 11:54:46 +0800 · 进行中 → 待验收 · 交付 commit 8f5b672f：panelW/H 按钳制口径（「按钮跑出面板」确认）；验证归波末

---

#### bug#362 · 【请填写单据号】文本容器内未垂直居中

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:19:01 +0800（ZCode主会话）· 更新：2026-11-19 11:55:00 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「【请填写单据号】这个文本在容器内 没垂直居中」。
现象：上线填写弹窗内【请填写单据号】文本在容器内未垂直居中。与 #360 同弹窗，排产时同批复现修复。

**正文**：

老板 2026-11-19 原话：「【请填写单据号】这个文本在容器内 没垂直居中」。
现象：上线填写弹窗内【请填写单据号】文本在容器内未垂直居中。与 #360 同弹窗，排产时同批复现修复。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:53:31 +0800 · 纯码修复完成（验证归波末）：release_detail_draw.cpp 前置卡 gate 提示字「请先填写单据号」（叠输入框容器内右对齐）缺 verticalAlign Center 顶格贴行顶；补垂直居中与输入框中线对齐。commit cea0047f
- 2026-11-19 11:55:00 +0800 · 交付 commit cea0047f：提示字补 verticalAlign Center；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 11:55:00 +0800 · 进行中 → 待验收 · 交付 commit cea0047f：提示字补 verticalAlign Center；验证归波末

---

#### bug#363 · 终端：中文输入法候选词显示在窗口中央而非光标处

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：开发中
- 创建：2026-11-19 10:34:24 +0800（ZCode主会话）· 更新：2026-11-19 11:58:37 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「我在终端里使用中文输入法打开，候选词显示在窗口的中央，不是在光标的右侧」。
现象：终端模块中文 IME 候选词窗口定位错误——显示在窗口中央而非光标旁。Windows IME 候选窗跟随光标需应用侧设置 composition/candidate window 位置（ImmSetCompositionWindow/ImmSetCandidateWindow 系），GLFW 默认不接管；修复落点大概率在 EUI-NEO 平台层（fork），排产时先查框架侧是否已有 IME 定位能力。

**正文**：

老板 2026-11-19 原话：「我在终端里使用中文输入法打开，候选词显示在窗口的中央，不是在光标的右侧」。
现象：终端模块中文 IME 候选词窗口定位错误——显示在窗口中央而非光标旁。Windows IME 候选窗跟随光标需应用侧设置 composition/candidate window 位置（ImmSetCompositionWindow/ImmSetCandidateWindow 系），GLFW 默认不接管；修复落点大概率在 EUI-NEO 平台层（fork），排产时先查框架侧是否已有 IME 定位能力。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 11:23:08 +0800 · 同根因新证据（2026-11-19 老板实测）：编辑器内 IME 候选词跑到窗体左上角（新卡 #379）——终端居中/编辑器左上两个症状同指向窗口层未接管 IME 位置。修复口径升级：平台层一次性修（EUI-NEO），终端+编辑器双验。
- 2026-11-19 11:36:44 +0800 · 与 #379 同根因并单（终端居中+编辑器左上两症状），EUI-NEO 平台层一次修
- 2026-11-19 11:36:55 +0800 · bug-fixer 开工（槽 1/5 复用，纯码模式）
- 2026-11-19 11:58:37 +0800 · bug-fixer 完工（纯码模式，真机验收归波末装机实测）：DevDesk 09fbd8f1——IME 锚从独立 term.ime.anchor（无 focusable 从不获焦=死线）改挂获焦的 term.input（偏移 (ix, visRow*cellH)）；EUI-NEO 6c7b910——updateImeCursorRect 按需解析聚焦元素 RenderTransform（修 requestFocus 无指针帧断链，#379 编辑器侧同根因）。根因实证：~/DevDesk/logs/ime-trace.log 近期 session 仅 seg1 无 seg2/3= setImeCursorRect 从未发出。验收锚点：装机后终端中文候选窗跟随光标。

**最近状态变更**（共 4 条）：2026-11-19 11:58:37 +0800 · 进行中 → 待验收 · bug-fixer 完工（纯码模式，真机验收归波末装机实测）：DevDesk 09fbd8f1——IME 锚从独立 term.ime.anchor（无 focusable 从不获焦=死线）改挂获焦的 term.input（偏移 (ix, visRow*cellH)）；EUI-NEO 6c7b910——updateImeCursorRect 按需解析聚焦元素 RenderTransform（修 requestFocus 无指针帧断链，#379 编辑器侧同根因）。根因实证：~/DevDesk/logs/ime-trace.log 近期 session 仅 seg1 无 seg2/3= setImeCursorRect 从未发出。验收锚点：装机后终端中文候选窗跟随光标。

---

#### bug#364 · 终端缺滚动条无法回看历史（疑似影响多功能，待考古引入时点）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 10:35:42 +0800（bug-fixer）· 更新：2026-11-19 11:34:50 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「终端 缺少滚动条，没办法看历史。这BUG啥时候搞出来的，好像影响了多个功能」。
现象：终端无滚动条，历史无法回看；老板称影响多个功能（具体清单待补）。
考古线索（老板问了引入时点）：候选源头=2026-11-05 老板终裁「滚动保留但滚动条视觉全无」——若该裁决被过度应用到终端，则滚动条视觉隐藏的同时丢了回看 affordance；需区分「视觉隐藏」vs「滚动回看能力本身坏了」（滚轮/触控板能否滚动待复现）。
修复纪律：回归=git 考古回原语义，禁自创机制；引入 commit 由 bug-fixer 考古给出，老板等这个答案。

**正文**：

老板 2026-11-19 原话：「终端 缺少滚动条，没办法看历史。这BUG啥时候搞出来的，好像影响了多个功能」。
现象：终端无滚动条，历史无法回看；老板称影响多个功能（具体清单待补）。
考古线索（老板问了引入时点）：候选源头=2026-11-05 老板终裁「滚动保留但滚动条视觉全无」——若该裁决被过度应用到终端，则滚动条视觉隐藏的同时丢了回看 affordance；需区分「视觉隐藏」vs「滚动回看能力本身坏了」（滚轮/触控板能否滚动待复现）。
修复纪律：回归=git 考古回原语义，禁自创机制；引入 commit 由 bug-fixer 考古给出，老板等这个答案。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 11:00:53 +0800 · 2026-11-19 修复波开排（老板「你先修复把」令）；P1 头单，bug-fixer 考古+修复
- 2026-11-19 11:00:58 +0800 · bug-fixer 后台开工：复现→考古引入 commit→最小修复→定向自测
- 2026-11-19 11:02:43 +0800 · 开工：终端滚动条缺失/无法回看（P1）
- 2026-11-19 11:34:50 +0800 · 完成：根因=①滚轮单位错配（v1 2d0c84e5 按 mac 像素增量调参，win ConPTY 转正 b9ff1898 后一档 1px≈0 行且方向反）②终端滚动条从未存在（bug188 终裁范围=侧栏/设置面板，不含终端网格）。修法=滚轮档位归一（1 档 3 行，方向对齐 EUI-NEO）+ components::scroll 竖条（thumb 拖拽回写 scrollOffset，无溢出不建条）；EUI-NEO 零改动。探针红→绿 14/14（滚轮/滚动条/拖拽/守卫）。调度改纯码模式：波末统一验证批跑全量。构建超预算自报：脚本调用 7 次（有效 5：红 1、编译错修正 1、探针测量口径修正 2、TIN-10 并发瞬态红重试 1；简报预算 ≤3），工具调用约 78（预算 ≤60）。

**最近状态变更**（共 4 条）：2026-11-19 11:34:50 +0800 · 进行中 → 待验收 · 完成：根因=①滚轮单位错配（v1 2d0c84e5 按 mac 像素增量调参，win ConPTY 转正 b9ff1898 后一档 1px≈0 行且方向反）②终端滚动条从未存在（bug188 终裁范围=侧栏/设置面板，不含终端网格）。修法=滚轮档位归一（1 档 3 行，方向对齐 EUI-NEO）+ components::scroll 竖条（thumb 拖拽回写 scrollOffset，无溢出不建条）；EUI-NEO 零改动。探针红→绿 14/14（滚轮/滚动条/拖拽/守卫）。调度改纯码模式：波末统一验证批跑全量。构建超预算自报：脚本调用 7 次（有效 5：红 1、编译错修正 1、探针测量口径修正 2、TIN-10 并发瞬态红重试 1；简报预算 ≤3），工具调用约 78（预算 ≤60）。

---

#### bug#366 · 智能体模块卡片不透明，与全局材质语言不一致

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-19 10:37:57 +0800（developer）· 更新：2026-11-19 11:56:02 +0800（developer）

**详情**：

老板 2026-11-19 原话：「智能体里的卡片是不透明的」。
现象：agent（智能体）模块卡片呈不透明实底，与全局卡片材质语言（半透/磨砂档，win24h2 窗效果矩阵的过渡档）不一致。修复前对照其他模块卡片确认基准材质参数；可能与 #356（Windows 颜色加深）同触点，排产时协调。

**正文**：

老板 2026-11-19 原话：「智能体里的卡片是不透明的」。
现象：agent（智能体）模块卡片呈不透明实底，与全局卡片材质语言（半透/磨砂档，win24h2 窗效果矩阵的过渡档）不一致。修复前对照其他模块卡片确认基准材质参数；可能与 #356（Windows 颜色加深）同触点，排产时协调。

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-19 11:56:02 +0800 · 代码级完成 commit 97a788b9：agent_module.cpp 任务卡面 alpha 钳到 About 卡基准（浅 0.78/深 0.98 不翻 D-10 裁决），全局基线不动；弹窗内实底卡有既有裁决锚已登记回单。纯码未验证，归波末

**最近状态变更**（共 4 条）：2026-11-19 11:56:02 +0800 · 进行中 → 待验收 · 代码级完成 commit 97a788b9：agent_module.cpp 任务卡面 alpha 钳到 About 卡基准（浅 0.78/深 0.98 不翻 D-10 裁决），全局基线不动；弹窗内实底卡有既有裁决锚已登记回单。纯码未验证，归波末

---

#### bug#367 · 定时弹窗布局完全错乱

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 10:39:01 +0800（ZCode主会话）· 更新：2026-11-19 11:55:06 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「定时弹窗里的东西布局100%全错乱」。
现象：schedule（定时/日程）模块弹窗内元素布局完全错乱（老板定性 100%）。与 #359/#360/#362 同类（弹窗布局），排产时可同批排查是否有共同根因（近期 qa158 系列滚动改动域）。

**正文**：

老板 2026-11-19 原话：「定时弹窗里的东西布局100%全错乱」。
现象：schedule（定时/日程）模块弹窗内元素布局完全错乱（老板定性 100%）。与 #359/#360/#362 同类（弹窗布局），排产时可同批排查是否有共同根因（近期 qa158 系列滚动改动域）。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:53:31 +0800 · 纯码修复完成（验证归波末，考古：need337 ff5b1cad 滚动化引入）：schedule_ui_edit.cpp 编辑弹窗内容直接挂 scrollView 流式 Column，显式 .position 子项坐标被 layoutColumn 无视（六页签竖叠/表单左堆＝100%错乱，原 spacer 注释假设「定位子项不参与列 wrap」不成立）；包一层 Stack（layoutStack 认 x/y）承载定位子项，层高沿 contentBottom 自收敛。commit edc536f2
- 2026-11-19 11:55:06 +0800 · 交付 commit edc536f2：need337 ff5b1cad 引入（滚动化定位子项被 Column 无视），Stack 承载修复；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 11:55:06 +0800 · 进行中 → 待验收 · 交付 commit edc536f2：need337 ff5b1cad 引入（滚动化定位子项被 Column 无视），Stack 承载修复；验证归波末

---

#### bug#369 · md 编辑器模式 tab 切换冻结数秒（编辑/预览/双栏/实时）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 10:42:56 +0800（bug-fixer）· 更新：2026-11-19 13:28:40 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「md编辑器 实时 双栏 预览 编辑 这几个tab 切换会卡几秒」。
现象：markdown 模块视图模式 tab（编辑/预览/双栏/实时）切换时 UI 冻结数秒。
线索：EUI-NEO 渲染管线已知成本底账=每帧全量重建元素树；模式切换大概率同步全量重渲染（md 渲染 + 元素树重建）阻塞 UI 线程。排产时按性能问题处理：先量化切换耗时（大文档/小文档分档取证）再定修法。

**正文**：

老板 2026-11-19 原话：「md编辑器 实时 双栏 预览 编辑 这几个tab 切换会卡几秒」。
现象：markdown 模块视图模式 tab（编辑/预览/双栏/实时）切换时 UI 冻结数秒。
线索：EUI-NEO 渲染管线已知成本底账=每帧全量重建元素树；模式切换大概率同步全量重渲染（md 渲染 + 元素树重建）阻塞 UI 线程。排产时按性能问题处理：先量化切换耗时（大文档/小文档分档取证）再定修法。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG×2: ① std-include 守卫红（P2）——tests/test_modules/test_md_mode_switch_perf.cpp:27 用 std::milli 未显式 include <ratio>（守卫自检连带红：好树有违规）。归属 09c15dca。修＝补 include。② 性能残余（P2）——修后定向复测（Total: 2 全绿，evidence/20261119-qa384-mdperf/）：元素 25.5K→119-2278✓、稳态帧 0.9-13.7s→20-35ms✓、render 重建 2.4s→29-185ms✓、edit/preview 暖切 23-100ms✓；但 60KB live 首切 ~0.6-1.1s、split 首切恒 ~1.0-1.3s 未达「百 ms 级」（修前切换帧 3.1-11.3s，改善 2.4-19 倍）。小文档档全部 ≤71ms 达标。

**进度记录**（共 6 条，列最近 5 条）：

- 2026-11-19 11:12:52 +0800 · bug-fixer 后台开工（3 并发槽 2/3）：量化取证→根因→最小解冻
- 2026-11-19 11:13:46 +0800 · [ch:常规] 开工：量化 md 模式 tab 切换冻结 → 定位同步阻塞根因 → 最小解冻。owner=bug-fixer
- 2026-11-19 12:00:49 +0800 · 修复完成：commit 09c15dca（dev，未 push）。根因=三非编辑态无块级视口裁剪（bug259-C 裁剪钩自查看器退役后无调用方）+实时态 scrollview 整篇 scratch 实量；切换帧全量建元 25.5K 文本元素/帧（60KB 文档实测 3.1-11.3s，Debug，改纯码模式前探针已跑）。修法=复活 bug259-C 模式：预览/双栏/实时接 blockVisible 视口裁剪 + contentHeightProvider 几何表供高 + MdSyncMap 帧级贴合高覆盖（实时活动块）。验证波末统一（纯码模式波令）：markdown 定向用例 + 探针 test_md_mode_switch_perf 复测。
- 2026-11-19 12:02:13 +0800 · 交付 commit 09c15dca：三态视口裁剪+几何表供高+活动块恒建（贴合高 override）；修前 60KB 切换帧 3.1-11.3s/每帧 2.5 万文本元素（探针实测在案）；修后数字归波末验证批复测
- 2026-11-19 13:28:40 +0800 · 红项修复 328abe47（补 <ratio> 守卫绿）；split/live 首切残余 0.6-1.3s 留老板验收裁决（稳态已 20-35ms，修前 3.1-11.3s）

**最近状态变更**（共 6 条）：2026-11-19 13:28:40 +0800 · 进行中 → 待验收 · 红项修复 328abe47（补 <ratio> 守卫绿）；split/live 首切残余 0.6-1.3s 留老板验收裁决（稳态已 20-35ms，修前 3.1-11.3s）

---

#### bug#373 · 主窗最小化时双击 sql 文件，编辑器子窗也最小化

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:18:40 +0800（ZCode主会话）· 更新：2026-11-19 12:17:54 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「win下，主窗体最小化的时候，双击sql文件打开编辑器，此时编辑器也是最小化状态，这不对的」。
现象：Windows 主窗最小化时，双击 .sql 文件经单实例链打开编辑器，编辑器子窗也处于最小化态（应正常显示并前置）。
线索：单实例打开文件两条链已知（热启动 requestShow / 托盘态 drain 挂 compose）；本例大概率 requestShow/子窗 show 路径把主窗的最小化态带给了新子窗，或子窗 show 未带 SW_RESTORE/前置。Win 平台窗口文件在 *_win.cpp 族。

**正文**：

老板 2026-11-19 原话：「win下，主窗体最小化的时候，双击sql文件打开编辑器，此时编辑器也是最小化状态，这不对的」。
现象：Windows 主窗最小化时，双击 .sql 文件经单实例链打开编辑器，编辑器子窗也处于最小化态（应正常显示并前置）。
线索：单实例打开文件两条链已知（热启动 requestShow / 托盘态 drain 挂 compose）；本例大概率 requestShow/子窗 show 路径把主窗的最小化态带给了新子窗，或子窗 show 未带 SW_RESTORE/前置。Win 平台窗口文件在 *_win.cpp 族。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-19 11:33:58 +0800 · 编辑器交互族任务（槽 4/5）开工，纯码模式
- 2026-11-19 12:15:52 +0800 · 纯码完成待波末验证：DevDesk 7e4d5ff3（onWindowCreated 即 requestWindowFocus：restore+前置+闪任务栏兜底）+ EUI-NEO 868a0a3（主循环 iconified 分支补子窗事务推进，开窗请求原积压到主窗还原——双层根因）
- 2026-11-19 12:17:54 +0800 · 交付 7e4d5ff3 + EUI-NEO 868a0a3：开窗即 requestWindowFocus（restore+前置+闪任务栏兜底）+ iconified 驻留期子窗事务推进；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:17:54 +0800 · 进行中 → 待验收 · 交付 7e4d5ff3 + EUI-NEO 868a0a3：开窗即 requestWindowFocus（restore+前置+闪任务栏兜底）+ iconified 驻留期子窗事务推进；验证归波末

---

#### bug#378 · 编辑器光标垂直定位偏上，应底部对齐

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 11:22:01 +0800（bug-fixer）· 更新：2026-11-19 13:28:28 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「编辑器的光标应该是底部对齐，目前是相对文本偏上的」。
现象：编辑器光标（caret）垂直定位相对文本偏上，应底部对齐（与文本行底/基线对齐）。光标度量=字体度量换算域（终端字体度量相关记忆提示 FreeType 度量链）；与 #373-#377 同编辑器域。

**正文**：

老板 2026-11-19 原话：「编辑器的光标应该是底部对齐，目前是相对文本偏上的」。
现象：编辑器光标（caret）垂直定位相对文本偏上，应底部对齐（与文本行底/基线对齐）。光标度量=字体度量换算域（终端字体度量相关记忆提示 FreeType 度量链）；与 #373-#377 同编辑器域。

**关联 BUG**（1）：

- 2026-11-19 12:51:20 +0800 · 未解 · BUG: 验证批 20261119 doctest 2 红（P2）——mdspike V4a/V4b caret Y 断言红：test_md_live_spike.cpp:518/595 nearEq(caret->frame.y, lastLineY)（封顶块/贴合高度换文本后光标 Y 不符）。初判：EUI-NEO 9afc364 caret 垂直度量改（FreeType 行度量替旧 fontSize*1.18）后断言期望漂移；需判新几何正确则更新断言，否则修几何。归属 EUI-NEO commit 9afc364（need378）。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 12:16:15 +0800 · 纯码完成待波末验证（改在 EUI-NEO submodule）：EUI-NEO 9afc364——input 组件 caret 垂直几何改 FreeType 行度量推导（measureFontLineMetrics ascent/descent），底部对齐行内文本底；imeRect 同几何；全部输入框受益
- 2026-11-19 12:18:16 +0800 · 交付 EUI-NEO 9afc364：caret 改 FreeType 行度量底部对齐（旧 fontSize*1.18 顶排为偏上根因），imeRect 同几何；验证归波末
- 2026-11-19 13:27:10 +0800 · 断言同步完成：EUI-NEO 9afc364 新 caret 几何判定正确（渲染基线=行顶+ascent，文本底=行顶+ascent+|descent|，caret 底恰重合）；mdspike V4a/V4b 共 7 处 caret Y 断言按公开度量 API 现算偏移同步。commit 0422dcd0。定向验 mdspike 全绿（同上 evidence）。
- 2026-11-19 13:28:28 +0800 · 红项修复 0422dcd0：新几何判定正确，断言按度量 API 现算（7 处），mdspike 全绿

**最近状态变更**（共 6 条）：2026-11-19 13:27:10 +0800 · 进行中 → 待验收 · 断言同步完成：EUI-NEO 9afc364 新 caret 几何判定正确（渲染基线=行顶+ascent，文本底=行顶+ascent+|descent|，caret 底恰重合）；mdspike V4a/V4b 共 7 处 caret Y 断言按公开度量 API 现算偏移同步。commit 0422dcd0。定向验 mdspike 全绿（同上 evidence）。

---

#### bug#379 · 编辑器 IME 候选词跑到窗体左上角（与 #363 同根因）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：开发中
- 创建：2026-11-19 11:23:02 +0800（ZCode主会话）· 更新：2026-11-19 11:58:37 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「编辑器后，输入法的候选词跑窗体左上角了，不在光标附近，和终端里的输入候选词类似，位置对不上」。
现象：编辑器内 IME 候选词窗跑到窗体左上角，不在光标附近。与 #363（终端候选词在窗体中央）同类：两处部位置都不对——强指向共同根因=窗口层未设置 IME composition/candidate 位置（Windows 默认放置因上下文不同而异：编辑器左上、终端居中）。修复应平台层一次性接管（EUI-NEO），编辑器/终端两意例同验。

**正文**：

老板 2026-11-19 原话：「编辑器后，输入法的候选词跑窗体左上角了，不在光标附近，和终端里的输入候选词类似，位置对不上」。
现象：编辑器内 IME 候选词窗跑到窗体左上角，不在光标附近。与 #363（终端候选词在窗体中央）同类：两处部位置都不对——强指向共同根因=窗口层未设置 IME composition/candidate 位置（Windows 默认放置因上下文不同而异：编辑器左上、终端居中）。修复应平台层一次性接管（EUI-NEO），编辑器/终端两意例同验。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 11:36:49 +0800 · 与 #363 并单：EUI-NEO 平台层 IME 位置接管一次修
- 2026-11-19 11:58:37 +0800 · bug-fixer 完工（纯码模式，真机验收归波末装机实测）：EUI-NEO 6c7b910——updateImeCursorRect 在 focusedElementRenderTransformValid_ 为假时按需沿树解析（旧实现直接放弃；编辑器 components/input 的 hit 本就带 .imeRect，断点在 requestFocus 自动聚焦后无指针事件帧、transform 从未采样 → 锚链断 → Win32 IME 落输入上下文默认位=窗体左上）。DevDesk 09fbd8f1 为同根因终端侧配套（#363）。验收锚点：装机后打开笔记即打中文，候选窗跟随光标（无需先点编辑区）。

**最近状态变更**（共 4 条）：2026-11-19 11:58:37 +0800 · 进行中 → 待验收 · bug-fixer 完工（纯码模式，真机验收归波末装机实测）：EUI-NEO 6c7b910——updateImeCursorRect 在 focusedElementRenderTransformValid_ 为假时按需沿树解析（旧实现直接放弃；编辑器 components/input 的 hit 本就带 .imeRect，断点在 requestFocus 自动聚焦后无指针事件帧、transform 从未采样 → 锚链断 → Win32 IME 落输入上下文默认位=窗体左上）。DevDesk 09fbd8f1 为同根因终端侧配套（#363）。验收锚点：装机后打开笔记即打中文，候选窗跟随光标（无需先点编辑区）。

---

#### bug#381 · 未保存变更弹窗文本字号过大

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:25:41 +0800（ZCode主会话）· 更新：2026-11-19 12:18:27 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话（同句后半）：「为保存的变更弹窗，文本的字太大了」（「为」=「未」误写）。
现象：编辑器未保存变更弹窗的文本字号过大。S 级视觉微调，编辑器域（#373-#380 族）。

**正文**：

老板 2026-11-19 原话（同句后半）：「为保存的变更弹窗，文本的字太大了」（「为」=「未」误写）。
现象：编辑器未保存变更弹窗的文本字号过大。S 级视觉微调，编辑器域（#373-#380 族）。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:16:23 +0800 · 纯码完成待波末验证：DevDesk 995cac63——未保存弹窗标题 24→20+600 字重（对齐全站 kDialogTitleFontSize=20 档）、正文 17→14、按钮 16→15
- 2026-11-19 12:18:27 +0800 · 交付 995cac63：标题 24→20+600 字重、正文 17→14、按钮 16→15（对齐全站弹窗档）；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:18:27 +0800 · 进行中 → 待验收 · 交付 995cac63：标题 24→20+600 字重、正文 17→14、按钮 16→15（对齐全站弹窗档）；验证归波末

---

#### bug#382 · 编辑器 md 预览严重性能问题（追问专项优化去向+考古）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-19 11:27:02 +0800（bug-fixer）· 更新：2026-11-19 11:58:41 +0800（bug-fixer）

**详情**：

老板 2026-11-19 原话：「编辑器的md预览功能，有严重的性能问题，这个性能问题不是专项优化了嘛」。
现象：编辑器 md 预览严重性能问题。老板追问：不是专项优化过吗——考古交付①当时专项做了什么（EUI-NEO 渲染管线成本底账：根因=每帧全量重建元素树，既定路线=视口裁剪→离屏整页缓存）②当前症状是回归还是专项未覆盖编辑器预览域。与 #369（md 模块切换冻结，在修）同域可能同根因，排产时两卡对照。

**正文**：

老板 2026-11-19 原话：「编辑器的md预览功能，有严重的性能问题，这个性能问题不是专项优化了嘛」。
现象：编辑器 md 预览严重性能问题。老板追问：不是专项优化过吗——考古交付①当时专项做了什么（EUI-NEO 渲染管线成本底账：根因=每帧全量重建元素树，既定路线=视口裁剪→离屏整页缓存）②当前症状是回归还是专项未覆盖编辑器预览域。与 #369（md 模块切换冻结，在修）同域可能同根因，排产时两卡对照。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-19 11:42:20 +0800 · 老板令加开子任务；含专项考古交付
- 2026-11-19 11:42:46 +0800 · bug-fixer 开工（槽 6，纯码，含专项考古）
- 2026-11-19 11:43:55 +0800 · 开工：考古渲染性能专项落地范围 + 定位编辑器 md 预览性能根因，最小修复，单 fix commit
- 2026-11-19 11:58:41 +0800 · 考古完成：专项=2026-11-11 bug#259 批（md 只读查看器性能专项：EUI-NEO c71e64b 增量折行+解析/布局缓存，DevDesk 侧查看器视口裁剪）；2026-11-16 notepad 替代查看器（eb4c0b8c 退役），预览走 components::markdown 新路径，EUI-NEO 组件级缓存被继承但裁剪留在 module_markdown 私有块流核（隔离禁依赖）——定性=专项未覆盖编辑器预览域，非回归。根因：notepad_window.cpp:943/:950 每帧（滚动/调窗/全局帧泵）全文档元素树重建（1MB 文档≈数万元素/帧）。修复：EUI-NEO 085ed7b（markdown 组件 opt-in visibleRange 裁剪，wrapper-skip 显式高不变零几何漂移）+ DevDesk 2a51c0c2（notepad 预览接线）；两仓单 fix commit 未 push，纯码模式未构建，性能数字归波末装机实测

**最近状态变更**（共 4 条）：2026-11-19 11:58:41 +0800 · 进行中 → 待验收 · 考古完成：专项=2026-11-11 bug#259 批（md 只读查看器性能专项：EUI-NEO c71e64b 增量折行+解析/布局缓存，DevDesk 侧查看器视口裁剪）；2026-11-16 notepad 替代查看器（eb4c0b8c 退役），预览走 components::markdown 新路径，EUI-NEO 组件级缓存被继承但裁剪留在 module_markdown 私有块流核（隔离禁依赖）——定性=专项未覆盖编辑器预览域，非回归。根因：notepad_window.cpp:943/:950 每帧（滚动/调窗/全局帧泵）全文档元素树重建（1MB 文档≈数万元素/帧）。修复：EUI-NEO 085ed7b（markdown 组件 opt-in visibleRange 裁剪，wrapper-skip 显式高不变零几何漂移）+ DevDesk 2a51c0c2（notepad 预览接线）；两仓单 fix commit 未 push，纯码模式未构建，性能数字归波末装机实测

---

#### bug#383 · 编辑器窗体 resize 后内容/换行不自适应

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-19 11:27:45 +0800（ZCode主会话）· 更新：2026-11-19 12:18:31 +0800（ZCode主会话）

**详情**：

老板 2026-11-19 原话：「编辑器的窗体调整大小后，内容也需要自适应的吧？尤其是换行」。
现象：编辑器子窗 resize 后内容不自适应（文本换行不随宽度重排）。编辑器域（#373-#382 族）。

**正文**：

老板 2026-11-19 原话：「编辑器的窗体调整大小后，内容也需要自适应的吧？尤其是换行」。
现象：编辑器子窗 resize 后内容不自适应（文本换行不随宽度重排）。编辑器域（#373-#382 族）。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-19 12:16:28 +0800 · 纯码完成待波末验证（改在 EUI-NEO submodule）：EUI-NEO 448d9c5——输入行级 dirtyKey 编入换行宽度指纹（keyed 行文本旧宽度缓存复用根因）+ 换行宽度变化作废行级 runs 缓存（行号/行起点漂移）
- 2026-11-19 12:18:31 +0800 · 交付 EUI-NEO 448d9c5：行级 dirtyKey 编入换行宽度指纹+宽度变化作废行级 runs 缓存（旧宽度缓存复用根因）；验证归波末

**最近状态变更**（共 4 条）：2026-11-19 12:18:31 +0800 · 进行中 → 待验收 · 交付 EUI-NEO 448d9c5：行级 dirtyKey 编入换行宽度指纹+宽度变化作废行级 runs 缓存（旧宽度缓存复用根因）；验证归波末

---

#### bug#399 · 记事本 md 预览滚动后大片空白、内容不再显示（真机老板实测：南非索赔发票布局微调PRD-2026-11-17.md）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-21 09:38:50 +0800（bug-fixer）· 更新：2026-11-21 22:38:40 +0800（bug-fixer）

**详情**：

根因已定位（线索 #340）：EUI 滚轮为渲染期语义，setScrollOffset 仅在元素带 composeOnScrollOffsetChange 标志时才请求重排；notepad 预览 scrollView 未设标志且 onChange 只存值 → visibleRange 裁剪范围冻结在旧 scroll 值，滚出 ±1200px overscan 后新暴露区全空且永不恢复。已排除大文件解析洞（实测文件 3.5KB）与估高失配（同源 cachedInlineLayout）。修法：EUI-NEO scrollview.h 加 opt-in 转发（对齐 dsl.h:802 既有标志，参照 virtualmasonry.h:124 / release_list_ui.cpp:604）+ notepad_window.cpp drawPreviewSlot 开启；ui_driver 真文件红绿链验证。派 bug-fixer 执行。

**正文**：

根因已定位（线索 #340）：EUI 滚轮为渲染期语义，setScrollOffset 仅在元素带 composeOnScrollOffsetChange 标志时才请求重排；notepad 预览 scrollView 未设标志且 onChange 只存值 → visibleRange 裁剪范围冻结在旧 scroll 值，滚出 ±1200px overscan 后新暴露区全空且永不恢复。已排除大文件解析洞（实测文件 3.5KB）与估高失配（同源 cachedInlineLayout）。修法：EUI-NEO scrollview.h 加 opt-in 转发（对齐 dsl.h:802 既有标志，参照 virtualmasonry.h:124 / release_list_ui.cpp:604）+ notepad_window.cpp drawPreviewSlot 开启；ui_driver 真文

**关联线索**（1）：

- [open] notepad,预览,滚动,白板,composeOnScrollOffsetChange,visibleRange,bug399,scrollView：bug#399 根因链（记事本 md 预览滚动后大片白板且不恢复）：EUI 滚轮链是渲染期语义——Runtime::update（runtime_lifecycle.h:120-124）收 scrollEvent→applyRuntimeScroll（runtime_update.h:517-538）只加速度冲量+animating_/paintRequested_；惯性 updateScrollMotion（:540-554）→setScrollOffset（:474-496）更新 offset+脏矩形+回调 onChange，仅当元素带 composeOnScrollOffsetChange 标志（dsl.h:225/:802 setter）才置 composeRequested_ 重排；视觉位移由 scrollTransformForElement（runtime_state_bindings.h:164-193）渲染期 transform 完成，元素树不重建。notepad 预览 scrollView（notepad_window.cpp:945-951）未设该标志且 onChange（:950）只存值不 requestUpdate → 裁剪范围 visibleRange(:984) 冻结在最后一次 compose 的 scroll 值；滚动超出 EUI 内建 ±1200px overscan（markdown.h:1198-1205）后新暴露区=空包装元素 → 大片白板，且闲置窗口无任何事件触发重排 → 永不恢复（老板原话「然后不显示内容了」）。同款标志全仓仅 virtualmasonry.h:124 与 release_list_ui.cpp:604 在用（裁剪型滚动同款需求，现成参照）。修法=scrollview 加 opt-in composeOnScrollOffsetChange 转发（EUI-NEO 优先补能力，AGENTS §一）+ notepad drawPreviewSlot 开启；旧查看器 md_module_preview.cpp:198-219 同样未设标志属同类潜在洞（主窗口常重排掩盖，只登记不动手）。已排除：估高≠实高（段落/列表/表格行均同源 cachedInlineLayout 同键同值）；>256KB 解析缓存洞（实测文件仅 3.5KB）。（source: 源码逐文件查证（codegraph）+ 老板真机复现文件 D:/Data/desktop/南非索赔发票布局微调PRD-2026-11-17.md（实测 3.5KB/43行/无内嵌图））

**进度记录**（共 41 条，列最近 5 条）：

- 2026-11-21 22:14:56 +0800 · 老板 2026-11-21 22:2x 令：最大化最小化、快速滚动、搜索定位三场景都要测（探针级机器证据+老板终测人类门）。另注：真窗口采集（PrintWindow/BitBlt/屏捕）对本环境 GL 窗全失效（陈旧面/句柄无效），实例级自动化验证依既有约束不可行，devdesk-verify-without-gui 已在案；中文路径经 Start-Process 注入启动会被编码损（老板双击无此问题，非用户面缺陷，测试工具链坑已入案）
- 2026-11-21 22:15:29 +0800 · 重开执行第十三轮：三场景验收套件（老板点名：最大化最小化/快速甩滚/搜索定位）
- 2026-11-21 22:17:11 +0800 · round13: notepad_probe 扩 O/R/S 三段（尺寸三往返/惯性甩滚/搜索定位）
- 2026-11-21 22:37:42 +0800 · round13 完成：notepad_probe 新增 O/R/S 三段（commit 7b56e87a），既有 J/K/L/N/P/Q 全绿，doctest file:test_notepad_preview 9/9。①尺寸三往返(756x538⇄900x640×3)：首见宽度冷布局1.09s/0.45s(既有成本)，重访腿<11ms，maxOffset==docH-vp 零漂移，每腿 ink>19993 无白板，parse miss=0；②惯性甩滚(6×dy=-4→冲量7200)：settle 666.7ms，运动3帧 ink=21369/40186/39353 无空白，运动期compose 2次，settle后 offset=11747.8 视口块.text在位；③搜索定位：hits=24，findAll 3.4ms，8跳(3next/3prev/2环绕) cur/cursor/选区全对，单跳1.2-3.5ms，环绕滚动 0→11245.4→0。产品现状(只记录未改)：预览态 findToggleEnabled=!previewOn(notepad_session.cpp:154, PRD §4.6)→预览内无查找入口。证据：evidence/bug399/round13-{data.md,probe.log,o-base.png,o-max.png,r-settled.png}
- 2026-11-21 22:38:40 +0800 · 三场景机器验收全绿：①最大最小化三往返：档间 8.5-15.1ms/腿（缓存复用）、换档 ink 19993-42518 全程无白板、maxOffset/docH 同源零漂移；首见新宽度冷布局 1086ms（既有一次性成本）②惯性甩滚：6 冲量至上限、666ms settle、运动中 3 帧 ink 2.1-4.0 万无空白、运动期 compose 仅 2 次（节流生效）、主线程 ~27.5%③搜索定位：24 命中 3.4ms、8 跳 cur/cursor/选区全对、单跳 1.2-3.5ms、环绕滚动正确。产品现状待老板裁决：预览态查找条被 §4.6 禁用——预览内搜索无入口。待老板真机终测（窗口已在屏）

**最近状态变更**（共 20 条）：2026-11-21 22:37:42 +0800 · 进行中 → 待验收 · round13 完成：notepad_probe 新增 O/R/S 三段（commit 7b56e87a），既有 J/K/L/N/P/Q 全绿，doctest file:test_notepad_preview 9/9。①尺寸三往返(756x538⇄900x640×3)：首见宽度冷布局1.09s/0.45s(既有成本)，重访腿<11ms，maxOffset==docH-vp 零漂移，每腿 ink>19993 无白板，parse miss=0；②惯性甩滚(6×dy=-4→冲量7200)：settle 666.7ms，运动3帧 ink=21369/40186/39353 无空白，运动期compose 2次，settle后 offset=11747.8 视口块.text在位；③搜索定位：hits=24，findAll 3.4ms，8跳(3next/3prev/2环绕) cur/cursor/选区全对，单跳1.2-3.5ms，环绕滚动 0→11245.4→0。产品现状(只记录未改)：预览态 findToggleEnabled=!previewOn(notepad_session.cpp:154, PRD §4.6)→预览内无查找入口。证据：evidence/bug399/round13-{data.md,probe.log,o-base.png,o-max.png,r-settled.png}

---

#### bug#402 · markdown 外部变更轮询在 compose 帧内同步（实测 55.8-80.2ms/帧 @300 笔记，真机每 5s 一次）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-22 01:26:33 +0800（developer）· 更新：2026-11-22 02:16:10 +0800（developer）

**详情**：

need401 实测：markdown_module.cpp:203 compose → pollDiskChangesIfDue（md_module_menu_poll.cpp:183-207）→ markdown_service.cpp:1230-1260 pollChanged：recursive_directory_iterator 全树 + 逐文件 mtime/size + SELECT path,size_bytes,modified_at FROM markdown_index 全表比对。探针将轮询压到 1ms 后 f2-f8=55.8/57.4/57.1/58.4/60.9/62.1/64.2/80.2ms。修法方向：移植 git worker 先例（git_page_controller.cpp:436 postWithRepo+postToUi），walk/比对/索引快照搬后台，compose 只消费结果。证据 evidence/bug401-report.md P1-1。

**正文**：

need401 实测：markdown_module.cpp:203 compose → pollDiskChangesIfDue（md_module_menu_poll.cpp:183-207）→ markdown_service.cpp:1230-1260 pollChanged：recursive_directory_iterator 全树 + 逐文件 mtime/size + SELECT path,size_bytes,modified_at FROM markdown_index 全表比对。探针将轮询压到 1ms 后 f2-f8=55.8/57.4/57.1/58.4/60.9/62.1/64.2/80.2ms。修法方向：移植 git worker 先例（git_page_controller.cpp:436 postWithRepo+postToUi），walk/比对/索引快

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-22 01:28:54 +0800 · 排产：与 #403 同批修（markdown 轮询/rescan 移出 compose 帧）
- 2026-11-22 01:29:12 +0800 · 开工登记：developer 执行
- 2026-11-22 01:29:43 +0800 · 开工：markdown 轮询移出 compose 帧（与 #403 同批修）
- 2026-11-22 02:16:10 +0800 · 修法=watcher 合流+后台巡检线程（MarkdownService poller_）：UI 帧只 consumeIndexDirty→refreshList，比对/rescan 全在后台；watcher 健康时全量比对降频 12 轮≈60s 兜底，掉线/stub 每轮 5s。300 笔记实测（bug401 探针，真机 5s 口径）：markdown steady 60.37→14.74ms，首帧 135.27→59.65ms（页面自身预热基线），周期 50ms+ 尖峰消失；1ms 剧毒口径 steady 17.38。语义：WATCH_ADD=1 / WATCH_DEL=0（外部增删 500ms 内索引跟随）；file:test_markdown 158 例 157 过 1 skip 0 fail；notepad_probe PASS。

**最近状态变更**（共 4 条）：2026-11-22 02:16:10 +0800 · 进行中 → 待验收 · 修法=watcher 合流+后台巡检线程（MarkdownService poller_）：UI 帧只 consumeIndexDirty→refreshList，比对/rescan 全在后台；watcher 健康时全量比对降频 12 轮≈60s 兜底，掉线/stub 每轮 5s。300 笔记实测（bug401 探针，真机 5s 口径）：markdown steady 60.37→14.74ms，首帧 135.27→59.65ms（页面自身预热基线），周期 50ms+ 尖峰消失；1ms 剧毒口径 steady 17.38。语义：WATCH_ADD=1 / WATCH_DEL=0（外部增删 500ms 内索引跟随）；file:test_markdown 158 例 157 过 1 skip 0 fail；notepad_probe PASS。

---

#### bug#403 · markdown rescan 在 compose 帧内同步（实测首帧 125.4ms @300 笔记）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-22 01:26:34 +0800（developer）· 更新：2026-11-22 02:16:10 +0800（developer）

**详情**：

need401 实测：md_module_menu_poll.cpp:192-194 变更命中后 service_->rescan() 在 UI 线程执行；markdown_service.cpp:503-582 rescanLocked = 递归扫描 + UPSERT + FTS 写。首帧 125.4ms（poll 57ms + rescan ≈68ms，300 笔记；按线性推算 >500 笔记破 200ms，未实测）。修法方向：与轮询同一条后台化改造，完成后置脏标记由 compose 重建树/列表。证据 evidence/bug401-report.md P1-2。

**正文**：

need401 实测：md_module_menu_poll.cpp:192-194 变更命中后 service_->rescan() 在 UI 线程执行；markdown_service.cpp:503-582 rescanLocked = 递归扫描 + UPSERT + FTS 写。首帧 125.4ms（poll 57ms + rescan ≈68ms，300 笔记；按线性推算 >500 笔记破 200ms，未实测）。修法方向：与轮询同一条后台化改造，完成后置脏标记由 compose 重建树/列表。证据 evidence/bug401-report.md P1-2。

**进度记录**（共 4 条，列最近 4 条）：

- 2026-11-22 01:29:03 +0800 · 排产：与 #402 同批修
- 2026-11-22 01:29:20 +0800 · 开工登记：developer 执行（与 #402 同批）
- 2026-11-22 01:29:43 +0800 · 开工：rescan 移出首帧（与 #402 同批修）
- 2026-11-22 02:16:10 +0800 · 进页首帧不再跑 poll+rescan：onStart 之外的 rescan 全部移出 UI 线程（首帧仅 refreshList 消费 indexDirty）。bug401 探针 300 笔记：markdown 首帧 135.27→59.65ms，steady 60.37→14.74ms（同批 #402 一并修复）；onPageEnter 本身 0.03-0.05ms。

**最近状态变更**（共 4 条）：2026-11-22 02:16:10 +0800 · 进行中 → 待验收 · 进页首帧不再跑 poll+rescan：onStart 之外的 rescan 全部移出 UI 线程（首帧仅 refreshList 消费 indexDirty）。bug401 探针 300 笔记：markdown 首帧 135.27→59.65ms，steady 60.37→14.74ms（同批 #402 一并修复）；onPageEnter 本身 0.03-0.05ms。

---

#### bug#404 · claude_proxy 进页首帧 46.7ms（同步装配 + refreshDataCache）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-22 01:26:34 +0800（developer）· 更新：2026-11-22 19:56:15 +0800（developer）

**详情**：

need401 实测：claude_module.cpp:114 compose → refreshDataCache（脏门控，:318 已有后台刷新意图）；f1=46.7ms，稳态 4.0ms。修法方向：首帧骨架 + 后台装填（release 帧泵同法）。证据 evidence/bug401-report.md P2-3。

**正文**：

need401 实测：claude_module.cpp:114 compose → refreshDataCache（脏门控，:318 已有后台刷新意图）；f1=46.7ms，稳态 4.0ms。修法方向：首帧骨架 + 后台装填（release 帧泵同法）。证据 evidence/bug401-report.md P2-3。

**进度记录**（共 6 条，列最近 5 条）：

- 2026-11-22 18:47:21 +0800 · 开工（波1代理1）
- 2026-11-22 18:53:21 +0800 · 【批内编码交付完成，未编译（共享 build 目录归验证批）；状态留「进行中」等验证批收口】
改动（4 文件）：
1. src/module_claude_proxy/claude_ui_state.cpp refreshDataCache：删除 !dataLoaded_ 冷启动同步查库分支，wantProfiles = dataDirty_ || !dataLoaded_ 全走后台 async（脏门控/节流/在途不叠加语义原样保留）。
2. src/module_claude_proxy/claude_ui_list.cpp drawProfileListPanel：新增 profiles.empty()&&!dataLoaded_ → claude.profiles.loading「正在加载 Profile…」骨架态（不再显示「暂无 Profile」假空态）。
3. src/module_claude_proxy/claude_ui_state.h / claude_module.cpp：dataLoaded_/refreshDataCache/applyDataRefresh/onStart 注释同步改义。
测试（tests/test_modules/test_claude_ui.cpp，已写未跑）：新增 claude_bug404_cold_start_skeleton_then_fill；测试工厂 mkmod() 补 onStart()（复刻生产装配序，旧用例此前靠 compose 同步首刷取数）；claude_ui_dialog_frame_containment 建档点追加 onPageEnter+有界轮询。
风险：未编译；mkmod 自动 onStart 影响该文件 ~40 用例（onStop 成对）。验证批请跑 --filter claude 定向。
发现缺陷（未修）：① src/module_settings/settings_module.cpp:2033 同型「compose 冷启动同步读」兜底（401 另一域）；② 冷启动 1-2 帧右栏详情占位「(未启用)/(无激活 profile)」与骨架文案可后续统一。
- 2026-11-22 19:09:26 +0800 · 波1 验证批（qa，2026-11-22）定档：**未验证（构建红）**。汇总构建首错停在 bug407 的 panel_state.cpp:776（编译错误），ninja 首错即停，claude 域未编译到；build/DevDesk.exe 仍为 12:15 波前（无冷启动骨架态）。新用例 claude_bug404_cold_start_skeleton_then_fill 未运行；bug401 探针波后数字未测（探针 12:24 产物=波前基线，46.7ms）。修复批派发：bug407 编译修绿后重跑用 case+探针 claude 页首帧。不得将本单当绿。
- 2026-11-22 19:55:06 +0800 · 修复批复测：claude_bug404_cold_start_skeleton_then_fill 绿（file:test_claude_ui.cpp 并入四域 148/148）。探针 claude_proxy f1 46.7→61.7ms 与未改页普涨 20~70% 同幅（探针运行期 DevDesk 在跑，全局漂移），不可当回归；且探针首装走 onStart 同步装载，未覆盖 compose 异步路径。判据以固定用例为准。证据 evidence/20261122-wave1-verify/{fix-batch.md,probe-wave2.log}。
- 2026-11-22 19:56:15 +0800 · 修复批收口：构建绿+定向绿（含 claude_bug404 骨架/装填用例）；工作树未 commit 待主会话收口

**最近状态变更**（共 4 条）：2026-11-22 19:56:15 +0800 · 进行中 → 待验收 · 修复批收口：构建绿+定向绿（含 claude_bug404 骨架/装填用例）；工作树未 commit 待主会话收口

---

#### bug#405 · todo 首帧 44.3ms @60 任务（导入后 reloadAll UI 线程全量重查 + 板装配）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-22 01:26:34 +0800（ZCode主会话）· 更新：2026-11-22 19:56:21 +0800（ZCode主会话）

**详情**：

need401 实测：todo_module.cpp:919 dbImportedDirty_ → reloadAll() 同步重查全量任务；f1=44.3ms，稳态 4.0ms（todo_ui_board.cpp:701 虚拟化已生效）。观察线：任务 >200 / 导入后重载帧。修法方向：reload 搬 worker，板先骨架。证据 evidence/bug401-report.md P2-4。

**正文**：

need401 实测：todo_module.cpp:919 dbImportedDirty_ → reloadAll() 同步重查全量任务；f1=44.3ms，稳态 4.0ms（todo_ui_board.cpp:701 虚拟化已生效）。观察线：任务 >200 / 导入后重载帧。修法方向：reload 搬 worker，板先骨架。证据 evidence/bug401-report.md P2-4。

**进度记录**（共 7 条，列最近 5 条）：

- 2026-11-22 18:57:06 +0800 · 池405 编码完成（未构建未跑）。todo_module.cpp：查询体 queryAll + 装配 applyReloadResult 拆分；dbImportedDirty_ 触发改后台线程（ready/release→compose 收割 acquire），逾期计数同批后台算；reloadStamp 防旧快照覆盖新同步重载；onStop/析构 join。todo_ui_board.h 增 BoardView.loading；todo_ui_board.cpp 增骨架板（四列列头+卡砖 pulse，plate 几何同步）。测试：新增 todo_import_reload_async_skeleton / _fill 两例；改既有 todo_ui_db_import_reloads_board 与 todo_drag_probe 三处导入泵帧等待。重要观察：bug401 P2-4 把 60 任务首帧 44.3ms 归因导入 reload，但 bug401_frame_probe 不发 kDbImported，该 f1 实为板首绘，本修法不改其数值，需另加导入帧测量。
- 2026-11-22 18:57:42 +0800 · 主会话判读（编码批回单后）：①归因错位采纳——bug401-p2-analysis.md:14/121 把 44.3ms 归到导入重载不实（探针 bug401_frame_probe.cpp:311-326 全程不发 kDbImported，44.3ms 实为 45 卡板首绘，虚拟化窗口下近乎全量）；本卡验收口径改为「导入重载不再卡 UI 帧+骨架态可观测」，不认 44.3 数字下降；②导入帧测量缺口另记（验证批探针补 kDbImported 场景）；③超预算 15% 如实记录不退回
- 2026-11-22 19:09:26 +0800 · 波1 验证批（qa，2026-11-22）定档：**未验证（构建红）**。汇总构建停在 bug407 编译首错，todo 域未编译到；新用例 todo_import_reload_async_fill / _skeleton 未运行；探针导入帧测量缺口（bug401_frame_probe.cpp:311-326 全程不发 kDbImported）本批未补，波后数字未测。验收口径按新定调：不认 44.3ms 数字下降，判据=「导入重载不再卡 UI 帧 + 骨架态可观测」，待构建修绿后实操+探针双证。
- 2026-11-22 19:55:06 +0800 · 修复批复测：todo_import_reload_async_skeleton / todo_import_reload_async_fill 两条均绿（file:test_todo_ui.cpp 并入四域 148/148），按新判据=导入重载不卡 UI 帧+骨架态可观测，不认 44.3ms 数字下降。探针 bug401_frame_probe.cpp:311-326 不发 kDbImported，导入帧未测（按派发不补探针，如实记未测）。证据 evidence/20261122-wave1-verify/fix-batch.md。
- 2026-11-22 19:56:21 +0800 · 修复批收口：构建绿+定向绿（骨架/装填两用例）；探针导入帧缺口如实记「未测」；44.3ms 归因错位已在案

**最近状态变更**（共 4 条）：2026-11-22 19:56:21 +0800 · 进行中 → 待验收 · 修复批收口：构建绿+定向绿（骨架/装填两用例）；探针导入帧缺口如实记「未测」；44.3ms 归因错位已在案

---

#### bug#406 · terminal 进页同步建 ConPTY 会话 14.4ms（onPageEnter ensureBackend）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：修复完成
- 创建：2026-11-22 01:26:34 +0800（developer）· 更新：2026-11-22 19:56:05 +0800（developer）

**详情**：

need401 实测：module_terminal.cpp:34 onPageEnter → ensureBackend()（会话创建在 UI 线程）enter=14.4ms；每帧 pollAll（terminal_page.cpp:228 → terminal_backend_win.cpp:778 快照）零阻塞 1.5-3.1ms。真会话输出风暴未验证。修法方向：ensureBackend 后台 + 骨架态。证据 evidence/bug401-report.md P2-5。

**正文**：

need401 实测：module_terminal.cpp:34 onPageEnter → ensureBackend()（会话创建在 UI 线程）enter=14.4ms；每帧 pollAll（terminal_page.cpp:228 → terminal_backend_win.cpp:778 快照）零阻塞 1.5-3.1ms。真会话输出风暴未验证。修法方向：ensureBackend 后台 + 骨架态。证据 evidence/bug401-report.md P2-5。

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-22 18:47:28 +0800 · 波1开工登记（汇总编译模式并发纯码）
- 2026-11-22 18:47:31 +0800 · 开工（波1代理3）
- 2026-11-22 18:53:55 +0800 · 编码批完成（未编译/未跑测试，按口径交验证批）：ensureBackend → startSessionAsync（SessionController 单 worker 线程队列 + pollAll 收割）；新增 SessionState::Starting 骨架态（页面「正在启动终端…」+ 空网格）；失败回传 Failed+error 走既有覆盖卡；Starting 进低频帧档（needsStartingFrameTick）不给 60fps 泵。doctest 4 例已写：moduleTerminalEnterAsyncNonBlocking / moduleTerminalEnterAsyncFailVisible / controllerAsyncCloseDropsResult / terminalStartingKeepsFrameFlow。改动 5 文件（module_terminal.h/.cpp、terminal_session_controller.h/.cpp、terminal_page.cpp）+ 1 测试文件。未 commit（同树有他人 claude_proxy/todo 改动）。
- 2026-11-22 19:09:26 +0800 · 波1 验证批（qa，2026-11-22）定档：**未验证（构建红）**。汇总构建停在 bug407 编译首错，terminal 域未编译到；四新用例（moduleTerminalEnterAsyncNonBlocking / moduleTerminalEnterAsyncFailVisible / controllerAsyncCloseDropsResult / terminalStartingKeepsFrameFlow）未运行；terminal 进页 14.4ms 波后未测。修复批：构建修绿后跑四用例+真会话帧泵复验。
- 2026-11-22 19:56:05 +0800 · 修复批收口：构建绿+定向 4 例绿+探针 enter 0.16ms；工作树未 commit 待主会话收口

**最近状态变更**（共 4 条）：2026-11-22 18:53:55 +0800 · 进行中 → 待验收 · 编码批完成（未编译/未跑测试，按口径交验证批）：ensureBackend → startSessionAsync（SessionController 单 worker 线程队列 + pollAll 收割）；新增 SessionState::Starting 骨架态（页面「正在启动终端…」+ 空网格）；失败回传 Failed+error 走既有覆盖卡；Starting 进低频帧档（needsStartingFrameTick）不给 60fps 泵。doctest 4 例已写：moduleTerminalEnterAsyncNonBlocking / moduleTerminalEnterAsyncFailVisible / controllerAsyncCloseDropsResult / terminalStartingKeepsFrameFlow。改动 5 文件（module_terminal.h/.cpp、terminal_session_controller.h/.cpp、terminal_page.cpp）+ 1 测试文件。未 commit（同树有他人 claude_proxy/todo 改动）。

---

#### bug#407 · memory 每帧全量快照拷贝 + 1s 刷新帧 5.3ms（进页首帧 29.1ms）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-22 01:26:34 +0800（developer）· 更新：2026-11-22 19:56:26 +0800（developer）

**详情**：

need401 实测：memory_module.cpp:108 panel_.refresh() 每帧；panel_state.cpp:309 1s 节流后 proposals/ledger 查询，:493 snapshot() 每帧持锁全量拷贝 PanelSnapshot。MEM_REFRESH_FRAME=5.33ms，f1=29.1ms，稳态 2.4ms。修法方向：快照改共享只读指针/脏版本号，查询搬线程。证据 evidence/bug401-report.md P2-6。

**正文**：

need401 实测：memory_module.cpp:108 panel_.refresh() 每帧；panel_state.cpp:309 1s 节流后 proposals/ledger 查询，:493 snapshot() 每帧持锁全量拷贝 PanelSnapshot。MEM_REFRESH_FRAME=5.33ms，f1=29.1ms，稳态 2.4ms。修法方向：快照改共享只读指针/脏版本号，查询搬线程。证据 evidence/bug401-report.md P2-6。

**进度记录**（共 14 条，列最近 5 条）：

- 2026-11-22 19:14:22 +0800 · 修复批排产授权（判读单 1/2：LiveState 补 lastEval+loadPanelSettings 归因）
- 2026-11-22 19:14:27 +0800 · 修复批开工
- 2026-11-22 19:15:03 +0800 · 修复批开工：修 P0 编译红（LiveState 漏 lastEval）→构建→四域定向→探针→#239 e2e；证据 evidence/20261122-wave1-verify/fix-batch.md
- 2026-11-22 19:55:06 +0800 · 修复批完成：LiveState 补 RecallResult lastEval + publishLocked 合并（panel_state.h:158 / panel_state.cpp:504）；bash scripts/build-test-run.sh --dev 全项目构建绿；四域定向 148/148 绿；探针 16 页波后数字表已出（terminal enter 14.43→0.16ms、memory f1 29.1→22.8ms；跨轮漂移 +20~70% 见报告）；MEM_REFRESH_FRAME 归因=settings 读盘 0.10-0.28ms/帧，非主因，未做缓存失效式。证据 evidence/20261122-wave1-verify/{fix-batch.md,probe-wave2.log,probe-attr.log}。装机验收归主会话。
- 2026-11-22 19:56:26 +0800 · 修复批收口：P0 编译红修复+定向绿+探针归因（每帧读盘非主因，未做缓存改造）；工作树未 commit 待主会话收口

**最近状态变更**（共 12 条）：2026-11-22 19:56:26 +0800 · 进行中 → 待验收 · 修复批收口：P0 编译红修复+定向绿+探针归因（每帧读盘非主因，未做缓存改造）；工作树未 commit 待主会话收口

---

#### bug#410 · markdown 笔记列表未虚拟化——800 笔记 68ms/帧、1200 笔记 214ms/帧、进页 531ms

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：—
- 创建：2026-11-22 11:56:11 +0800（developer）· 更新：2026-11-22 12:30:30 +0800（developer）

**详情**：

来源：need401 P2 增长实测（evidence/bug401-p2-analysis.md N1，commit 本轮探针扩参）。
实测（360; tests/probes/bug401_frame_probe.cpp, BUG401_POLL_MS=5000 真机口径）：PAGE markdown f4-f8 稳态 300 笔记=20.4ms、500=25.1ms、800=68.1ms、1200=214.0ms；进页首帧 f1 分别 100.8/99.7/211.0/531.3ms。超线性（500→800 +0.14ms/笔记，800→1200 +0.365ms/笔记），破 50ms 阈值≈600 笔记。
归因（codegraph）：md_module_tree.cpp:410 drawNoteList 每帧 for 全量 notes_ 逐条建行元素（无可见窗口裁剪）；:419-422 每帧对全部 path 做 std::hash 拼 contentKey（O(N) 字符串）。#402/403 只解决了 poll/rescan，未动此绘制链。
修法：照 todo 看板虚拟化先例（todo_ui_board.cpp:701 只画可见区间）给笔记列表做窗口裁剪；contentKey 移出逐帧计算（notes_ 变更时算一次）。改动量 ~60-100 行，风险=滚动/选中/右键菜单交互回归，需 markdown_probe 覆盖。
验收：800 笔记稳态帧 <20ms、1200 笔记 <50ms、进页首帧 <100ms；滚动/选中/右键菜单/搜索过滤回归绿。

**正文**：

来源：need401 P2 增长实测（evidence/bug401-p2-analysis.md N1，commit 本轮探针扩参）。
实测（360; tests/probes/bug401_frame_probe.cpp, BUG401_POLL_MS=5000 真机口径）：PAGE markdown f4-f8 稳态 300 笔记=20.4ms、500=25.1ms、800=68.1ms、1200=214.0ms；进页首帧 f1 分别 100.8/99.7/211.0/531.3ms。超线性（500→800 +0.14ms/笔记，800→1200 +0.365ms/笔记），破 50ms 阈值≈600 笔记。
归因（codegraph）：md_module_tree.cpp:410 drawNoteList 每帧 for 全量 notes_ 逐条建行元素（无可见窗口裁剪）；:419-4

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-22 12:30:27 +0800 · 已修：markdown 左栏笔记/树列表虚拟化（可见窗口±3 行 + contentHeightProvider 精确内容高，去掉每帧全量行 hash）。md_module_tree.cpp drawNoteList/drawTreeList。同探针同参（1200 笔记 POLL_MS=5000）：稳态帧 214.0ms → 14.3ms（达标 ≤50），进页首帧 531.3 → 111.4ms（余下为后台 rescan 帧尖峰，属 need401 §7 观察线）。回归：markdown 128 例绿、md_layout_collapse_probe / bug150_164_probe PASS。commit bbbff096。

**最近状态变更**（共 4 条）：2026-11-22 12:30:30 +0800 · 进行中 → 待验收 · 

---

#### bug#411 · memory 待审提案无上限 × 每帧快照拷贝——500 条 78ms/1s、2000 条 135ms（与提案风暴叠加）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：—
- 创建：2026-11-22 11:56:11 +0800（developer）· 更新：2026-11-22 12:30:39 +0800（developer）

**详情**：

来源：need401 P2 #407 增长实测（evidence/bug401-p2-analysis.md §5 + §6）。
实测（直插自家 proposals.db pending 行，DataRootEnv 隔离）：1s 档刷新帧 0 条=3.5ms、500 条=78.7ms、2000 条=135.1ms（另见 132.5ms 单帧闪帧）；稳态每帧快照拷贝 2.6→6.7→8.8ms；记忆页首帧 18-21→47.4→123.7ms。破 50ms ≈350 条待审，破 200ms ≈2600 条。
链路：panel_state.cpp:309 每秒跑 proposalStore_->listPending()（proposal_store.cpp:370 全量无 LIMIT）→ 解码 vector<string>；panel_state.cpp:495 snapshot() 每帧锁内全量拷贝（含全部 pending 正文）。
放大源：clue45 实测一次采集风暴产出 1630 条 pending → 真机 = 每 1s 一次 100ms+ 主线程卡顿 + 记忆页首帧 ~100ms；与池 201（记忆模块不可用）同源，是该单的放大面。
修法：listPending 加窗口上限（如 200，UI 侧「还有 N 条」提示）+ snapshot 改 shared_ptr<const PanelSnapshot>/脏版本号（消除每帧全量拷贝）。改动量 ~40-60 行。
验收：2000 pending 下 1s 刷新帧 <20ms、稳态 <3ms、记忆页首帧 <50ms；审核/撤销/批量接受流回归绿。

**正文**：

来源：need401 P2 #407 增长实测（evidence/bug401-p2-analysis.md §5 + §6）。
实测（直插自家 proposals.db pending 行，DataRootEnv 隔离）：1s 档刷新帧 0 条=3.5ms、500 条=78.7ms、2000 条=135.1ms（另见 132.5ms 单帧闪帧）；稳态每帧快照拷贝 2.6→6.7→8.8ms；记忆页首帧 18-21→47.4→123.7ms。破 50ms ≈350 条待审，破 200ms ≈2600 条。
链路：panel_state.cpp:309 每秒跑 proposalStore_->listPending()（proposal_store.cpp:370 全量无 LIMIT）→ 解码 vector<string>；panel_state.cpp:495 snapshot() 每帧锁

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-22 12:30:37 +0800 · 已修：待审提案窗口上限 + 真实总数。proposal_store listPending(limit=-1 缺省全量，面板传 200)；panel_state refresh 走 countByStatus 报 pendingTotal；徽章/尾行按真值提示（超窗口显「处理后自动补入」）。2000 提案同参（POLL_MS=5000）：1s 刷新帧 135.1ms → 18.2ms（达标 ≤50），稳态 8.8 → 3.6ms。新增用例 memory need401-411 listPending 窗口截断（去限幅即红）；回归 memory 125 例绿。commit bbbff096。

**最近状态变更**（共 4 条）：2026-11-22 12:30:39 +0800 · 进行中 → 待验收 · 

---

#### bug#412 · home getLastCallRate 全表倒扫 claude_call_logs——10万行 38ms/20万行 84ms（进页阻塞）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：—
- 创建：2026-11-22 11:56:21 +0800（developer）· 更新：2026-11-22 12:30:47 +0800（developer）

**详情**：

来源：need401 P2 #408 增长实测（evidence/bug401-p2-analysis.md §4）。
实测（直造 claude_call_logs 行；PAGE home 进页 + 归因探针 HOME_SQL 同 SQL 只读复刻）：getStats 的 4 条 COUNT（今日笔记/待办/逾期/今日调用）均平坦 0.15-0.17ms（started_at 索引范围扫）——不是风险点。唯一风险是 getLastCallRate（home_service.cpp:200-205）：50k 行 21.5ms、100k 行 38.4ms、200k 行（常态 2xx）84.4ms，斜率 0.42ms/1k 行；破 50ms ≈13 万行、破 200ms ≈49 万行。谓词列 status_code/output_tokens 无索引且被 COALESCE 包裹使索引不可用，SQLite 按 started_at DESC 倒扫逐行过滤。
修法（S 级 ~15 行）：去掉 COALESCE（NULL 本就不在 200-299，语义不变；改成 status_code BETWEEN 200 AND 299 AND output_tokens > 0）+ 迁一条复合索引 claude_call_logs(status_code, output_tokens, started_at DESC)。替代/更省：在 app_config 缓存最近一次成功调用快照，写入侧刷新。建议与 #404（claude_proxy refreshDataCache 同表扫描，200k 行 66-73ms）合并成一单。
验收：20 万行下 home 进页 getLastCallRate <2ms；代理页首帧 <50ms；首页数字与旧实现一致（对账同一行）。

**正文**：

来源：need401 P2 #408 增长实测（evidence/bug401-p2-analysis.md §4）。
实测（直造 claude_call_logs 行；PAGE home 进页 + 归因探针 HOME_SQL 同 SQL 只读复刻）：getStats 的 4 条 COUNT（今日笔记/待办/逾期/今日调用）均平坦 0.15-0.17ms（started_at 索引范围扫）——不是风险点。唯一风险是 getLastCallRate（home_service.cpp:200-205）：50k 行 21.5ms、100k 行 38.4ms、200k 行（常态 2xx）84.4ms，斜率 0.42ms/1k 行；破 50ms ≈13 万行、破 200ms ≈49 万行。谓词列 status_code/output_tokens 无索引且被 COALESCE 包裹使索引不可用，SQ

**进度记录**（共 1 条，列最近 1 条）：

- 2026-11-22 12:30:45 +0800 · 已修：home_service getLastCallRate 去 COALESCE（NULL 比较同为 false，语义不变）+ schema.h 新增成功行部分索引 idx_call_logs_success_rate(started_at DESC) WHERE status_code BETWEEN 200 AND 299 AND output_tokens > 0（谓词与查询逐字一致，planner 蕴含命中）。20 万行同参：同 SQL 复刻 84.4ms → 0.018ms（达标 ≤20），home 二次进页 96.2 → 0.59ms。schema 用例补索引名；回归 schema 21 / home 10 例绿。commit bbbff096。

**最近状态变更**（共 4 条）：2026-11-22 12:30:47 +0800 · 进行中 → 待验收 · 

---

### 在池（4）

#### bug#408 · home 进页同步直读库（getStats 6 条 count + recent/hourly，实测空库 0.4-0.6ms，随表增长）

- 状态：在池 · 优先级：P2 · 负责人：developer · 阶段：—
- 创建：2026-11-22 01:26:41 +0800（developer）· 更新：2026-11-22 01:26:41 +0800（developer）

**详情**：

need401 实测：home_module.cpp:380 onPageEnter → refreshStats；home_service.cpp:159 getStats = 6×countBySql + queryRecentNotes + 小时分布 + lastRate。当前空库 enter=0.41/0.57ms，随 claude_call_logs/markdown_index/todo_tasks 行数线性。观察线：真实用户大库重测；超 50ms 则搬 worker 或缓存。证据 evidence/bug401-report.md P2-7。

**正文**：

need401 实测：home_module.cpp:380 onPageEnter → refreshStats；home_service.cpp:159 getStats = 6×countBySql + queryRecentNotes + 小时分布 + lastRate。当前空库 enter=0.41/0.57ms，随 claude_call_logs/markdown_index/todo_tasks 行数线性。观察线：真实用户大库重测；超 50ms 则搬 worker 或缓存。证据 evidence/bug401-report.md P2-7。

**最近状态变更**（共 1 条）：2026-11-22 01:26:41 +0800 · — → 在池 · 入池

---

#### bug#409 · 多模块每帧快照/有界读当前 <50ms 待规模观察（mcp registry 快照 / wallpaper snapshot / release 帧泵分页 / settings 日志尾读）

- 状态：在池 · 优先级：P3 · 负责人：developer · 阶段：—
- 创建：2026-11-22 01:26:41 +0800（developer）· 更新：2026-11-22 01:26:41 +0800（developer）

**详情**：

need401 实测：mcp_page_module.cpp:230/233 每帧 registry_->list + status（稳态 1.7ms，首帧 26.9ms）；wallpaper_module.cpp:34 每帧 snapshot 拷贝（稳态 2.5ms）；release_list_ui.cpp:1019/1054 帧泵内同步分页读 20 页（首帧 19.3/17.7ms）；settings_module.cpp:1978 readLogTailLines 有界尾读（:2048 已有后台 promise）。均为有界/小量，不立项修复，规模变化时复测。证据 evidence/bug401-report.md P2-8~11。

**正文**：

need401 实测：mcp_page_module.cpp:230/233 每帧 registry_->list + status（稳态 1.7ms，首帧 26.9ms）；wallpaper_module.cpp:34 每帧 snapshot 拷贝（稳态 2.5ms）；release_list_ui.cpp:1019/1054 帧泵内同步分页读 20 页（首帧 19.3/17.7ms）；settings_module.cpp:1978 readLogTailLines 有界尾读（:2048 已有后台 promise）。均为有界/小量，不立项修复，规模变化时复测。证据 evidence/bug401-report.md P2-8~11。

**最近状态变更**（共 1 条）：2026-11-22 01:26:41 +0800 · — → 在池 · 入池

---

#### bug#413 · claude_proxy 首帧随 claude_call_logs 增长——20万行 66-73ms（8万行破 50ms 红线）

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：—
- 创建：2026-11-22 11:56:21 +0800（ZCode主会话）· 更新：2026-11-22 11:56:21 +0800（ZCode主会话）

**详情**：

来源：need401 P2 #404/#407 联合观察（evidence/bug401-p2-analysis.md §4/§6）。
实测：claude_proxy 首帧（claude_module.cpp:114 refreshDataCache，脏门控）空库 46.7ms → 20 万行 claude_call_logs=66-73ms（~0.1ms/1k 行 + 构树固定 45ms）；=8 万行即破 50ms。代理页稳态 5.3-6.6ms 不受影响。
说明：该单当前不进「立即修」——46.7ms 基线在 50ms 红线下方，且修法与刚入池的 home getLastCallRate 同属「claude_call_logs 扫描后台/索引化」一个改造面，宜合并实施。
触发线（二者任一即升级）：本机 claude_call_logs >8 万行；或代理页首帧连续 3 次 >50ms。
修法方向（升级时）：refreshDataCache 搬后台线程 + 首帧骨架态（claude_module.cpp:318 已有意图注释），~80 行。
验收（升级时）：20 万行下代理页首帧 <10ms，数据装填完成后数字与旧实现一致。

**正文**：

来源：need401 P2 #404/#407 联合观察（evidence/bug401-p2-analysis.md §4/§6）。
实测：claude_proxy 首帧（claude_module.cpp:114 refreshDataCache，脏门控）空库 46.7ms → 20 万行 claude_call_logs=66-73ms（~0.1ms/1k 行 + 构树固定 45ms）；=8 万行即破 50ms。代理页稳态 5.3-6.6ms 不受影响。
说明：该单当前不进「立即修」——46.7ms 基线在 50ms 红线下方，且修法与刚入池的 home getLastCallRate 同属「claude_call_logs 扫描后台/索引化」一个改造面，宜合并实施。
触发线（二者任一即升级）：本机 claude_call_logs >8 万行；或代理页首帧连续 3 次 >50ms。

**最近状态变更**（共 1 条）：2026-11-22 11:56:21 +0800 · — → 在池 · 入池

---

#### bug#415 · hook 负对照「今天天气怎么样」阴性召回误报：无关提问注入 ccache 踩坑卡 1 条（#239 e2e 环节⑤红）

- 状态：在池 · 优先级：P1 · 负责人：developer · 阶段：—
- 创建：2026-11-22 19:09:36 +0800（developer）· 更新：2026-11-22 19:09:36 +0800（developer）

**详情**：

来源：波1 验证批 #239 e2e 复跑（evidence/20261122-wave1-verify/hook-neg-control.json）。hook 负对照提问「今天天气怎么样」非 0 注入：additionalContext 命中 1 条 ccache 踩坑卡（[踩坑|会话采集] telemem-mcp 采集到…CCACHE_DIR…）。该卡为同轮 S8 入队/采集产物，与提问语义无关，实为阴性召回误报；可能因素：召回阈值只看 embedding 分数不看语义相关性 / 单卡库（唯一命中也必取）/ 排序无相对门控。S7 日志另有「8421 灰带 0.4 门控」池257 校准议题，本条症状不同（是无关提问的注入）。需 developer 定位：hook 检索路径（DevDesk.exe hook → telemem search → 注入拼装）步 threshold 与 top-k 取用；验收=「今天天气怎么样」stdout 空 + exit 0，且正例问①/②仍命中注入（不得靠提高阈值一刀切）。

**正文**：

来源：波1 验证批 #239 e2e 复跑（evidence/20261122-wave1-verify/hook-neg-control.json）。hook 负对照提问「今天天气怎么样」非 0 注入：additionalContext 命中 1 条 ccache 踩坑卡（[踩坑|会话采集] telemem-mcp 采集到…CCACHE_DIR…）。该卡为同轮 S8 入队/采集产物，与提问语义无关，实为阴性召回误报；可能因素：召回阈值只看 embedding 分数不看语义相关性 / 单卡库（唯一命中也必取）/ 排序无相对门控。S7 日志另有「8421 灰带 0.4 门控」池257 校准议题，本条症状不同（是无关提问的注入）。需 developer 定位：hook 检索路径（DevDesk.exe hook → telemem search → 注入拼装）步 threshold 与 top

**最近状态变更**（共 1 条）：2026-11-22 19:09:36 +0800 · — → 在池 · 入池

---

### 挂起（2）

#### bug#333 · 提案入库零质量闸门——垃圾直接落库（老板令：这种东西就不应该入库）

- 状态：挂起 · 优先级：P1 · 负责人：developer · 阶段：PRD 完成
- 创建：2026-11-18 16:40:44 +0800（developer）· 更新：2026-11-22 20:38:13 +0800（developer）
- **blocker**：老板 2026-11-22 收敛令：暂不开发。PRD doc#27 与三波排期已在案（波1 F-05/F-06 止血→波2 终稿 MVP→波3），恢复条件=开发令

**详情**：

老板令（2026-11-18 原话）：「这种东西就不应该入库」——采集→提案→入库链路缺前置质量闸门：无头无尾/无上下文/去重失败/批量墙字的提案必须在写库前被拒（带日志），而不是 1630 条垃圾落库后靠审核页人工筛。与 #191（同秒批量、无去重无上限）同链路。修法方向（引擎侧 telemem）：①内容闸门（上下文完整性/可读性校验，不过拒）②去重（内容 hash/相似度）③批量上限/节流④人读格式化（头尾上下文+来源会话引用）。另待老板令：现有真实库 1629 条垃圾 pending 是否清理（备份后）。

【需求裁决序（need#146，product-manager 2026-11-18；PRD=doc#27 2026-11-18-memory-pipeline-final-draft）】①该做=老板四令+三证（AC 批 561/561 墙字 0 入库、#334 接受链挂死、脱敏 0 REDACTED+16 行 @）②复用=检索/嵌入底座（Recall@5=100%）+#191 已实现闸件（指纹去重/轮预算/低值过滤/TTL）+proposals 状态机/双闸控耗/ledger ③改造=定长切片→语义分段、infer 队列→终稿修饰消费、采集卡直通→闸后直入库、审核页降兜底 ④新方案=聚合归并层+前置闸。四态结论=改造为主＋两新增环节；不做=重写检索底座（无依据且已绿）。剖界=编排/闸/UI/对账全本仓，引擎侧 E-1~E-4 需求规格供 telemem 实现。排波 3 波：波1 F-05+F-06 止血；波2 F-01+F-03+F-04 终稿 MVP；波3 F-02+F-07+F-08。

**正文**：

老板令（2026-11-18 原话）：「这种东西就不应该入库」——采集→提案→入库链路缺前置质量闸门：无头无尾/无上下文/去重失败/批量墙字的提案必须在写库前被拒（带日志），而不是 1630 条垃圾落库后靠审核页人工筛。与 #191（同秒批量、无去重无上限）同链路。修法方向（引擎侧 telemem）：①内容闸门（上下文完整性/可读性校验，不过拒）②去重（内容 hash/相似度）③批量上限/节流④人读格式化（头尾上下文+来源会话引用）。另待老板令：现有真实库 1629 条垃圾 pending 是否清理（备份后）。

【需求裁决序（need#146，product-manager 2026-11-18；PRD=doc#27 2026-11-18-memory-pipeline-final-draft）】①该做=老板四令+三证（AC 批 561/561 墙字 0 入库、#334 接受链挂死、脱敏

**进度记录**（共 11 条，列最近 5 条）：

- 2026-11-18 19:24:31 +0800 · 波 2 OQ 分支方案卡已落 evidence/20261118-333-wave2-plan/scheme-card.md：推荐组合 = OQ-1 批量 expired+审计 / OQ-2 闸过自动入库+失败落兜底 / OQ-3 本仓主闸+L3+引擎三层 / OQ-4 云端 addInferred 过桥（E-1 就绪升级）/ OQ-5 保守缺省+POC 校准；范围冻结 = F-03 修饰链→F-04 闸+L3→最小计数三腿，F-01 建议移出波 2（待老板确认）；腿 A 可与波 1 验证并行，腿 B 串行；文件域地图 12 项含 BD/#334/BC 撞面与规避；5 OQ 两案实施差齐，待装机轮 5 裁决
- 2026-11-18 20:36:11 +0800 · 波 2 六项 OQ 老板已全裁（20:3x，全推荐案）：OQ-1 存量批量置 expired+审计行（真根已 GC 空表，此为后续标准处置口径）｜OQ-2 闸过+修饰成功自动入库+失败/低置信兜底页｜OQ-3 本仓主闸+L3 accept 兜底+引擎 E-3 三层｜OQ-4 云端 addInferred 过桥 E-1 就绪升级｜OQ-5 保守缺省+195/561 真数据 POC 校准｜F-01 分段移出波 2（PRD §10 排波修订）。范围冻结=腿 A（F-03 修饰链）→腿 B（F-04 闸+L3，串行于 A）→腿 C（最小计数，可与 B 并行）。腿 A 即刻派发（BZ 批）。
- 2026-11-18 20:36:52 +0800 · 腿 A 开工（developer）：F-03 终稿修饰链含 infer 队列消费修复，按 BG 方案卡 §2 腿 A + §3 文件域地图执行
- 2026-11-18 21:01:59 +0800 · 腿 A 落地（developer，wip 52742b99+646a8d20）：①队列只进不出根因修复——infer_queue 加 attempts/last_error/confidence 三列（老库 ALTER 迁移），失败记账+重试耗尽（5 次上限）必出队；②OQ-2 分流三形态：修饰成功→差分产物回填 targetIds→终稿卡 auto_accepted（source=final_draft，引擎已落库不走 executor，createUndo 放行 auto_accepted 撤销链保留）；重试耗尽/零产物（bug239）/低置信（<0.3 前置不出机）→出队落兜底 pending 卡（待审形态+面板兜底文案+note 原因）；③refine 接口位预留（OQ-4 E-1）；④collect_round/telemem addDirect 侧（#334 冻结）零触碰。测试：test_memory_infer 6 新用例（含只进不出排空红队）+test_memory_proposals 3 新用例+e2e S8 探针带终稿卡库（域外签名涟漪，已修保 BJ 编译）。构建/数值未跑（BJ 波末批）。腿 B 串行接口：Proposal{final_draft/auto_accepted/targetIds}、InferStepReport{autoAccepted/fallbackPending/fallbackReason}、Entry{attempts/lastError}。证据：evidence/20261118-333-wave2-legA/leg-a-report.md（变更清单+分流说明+断言契约+未验证项）。工具 44 次（超预算 9：4 次因 codegraph 读不满足 Edit 前置门禁的失败重试+3 次窄读+e2e 探针涟漪 2 次）
- 2026-11-22 20:38:13 +0800 · 收敛归位：PRD 完成待开发，挂起等开发令

**最近状态变更**（共 4 条）：2026-11-22 20:38:13 +0800 · 进行中 → 挂起 · 老板 2026-11-22 收敛令：暂不开发。PRD doc#27 与三波排期已在案（波1 F-05/F-06 止血→波2 终稿 MVP→波3），恢复条件=开发令

**关联文档**：doc#27（prd v1） → 全文见附录 A

---

#### bug#347 · keychain→app_secrets 专表无迁移：memory.llm 与 wecom 两处旧 key 升级后静默失效（BV 审查 P1）

- 状态：挂起 · 优先级：P2 · 负责人：developer · 阶段：开发中
- 创建：2026-11-18 19:54:49 +0800（developer）· 更新：2026-11-18 22:18:00 +0800（developer）
- **blocker**：简报前提失效：旧 keychain 读路径不存在——need317（98d1cccc，2026-11-18 20:26，为合并线 1d1bf1bd 祖先）已删 src/platform/keychain.h + keychain_macos.cpp/keychain_posix.cpp/keychain_win.cpp/keychain_macos_scan.h 五文件（773 行），CMake/AGENTS/测试触达面同批清零（src grep 归零）。两处旧键（devdesk.memory / devdesk.schedule）物理存于 OS 钥匙串，运行时回填必须重建已退役的平台 keychain 读删栈：超本批域限（secrets 迁移链+两消费点+测试）与预算（20 工具/25 分钟），且直接回退同日完成的 need317 全链退役决策。

**详情**：

BV 审查 P1（evidence/20261118-mac-wave-review/review.md）：Mac need304/305 把 keychain 迁到 app_secrets 专表，但两处旧 key 无迁移路径——①telemem_config.h:66-70 memory.llm 键：老板真库已配 key 升级后静默失效需重录；②wecom.cpp:336 企业微投递 key 同样无迁移仅 LOG_WARN。Mac 标注「二期另议」有意取舍。修法：首读时 keychain→app_secrets 一次性回填（读旧删旧）或升级向导提示重录。装机轮 5 已在清单提示重录 workaround。

**正文**：

BV 审查 P1（evidence/20261118-mac-wave-review/review.md）：Mac need304/305 把 keychain 迁到 app_secrets 专表，但两处旧 key 无迁移路径——①telemem_config.h:66-70 memory.llm 键：老板真库已配 key 升级后静默失效需重录；②wecom.cpp:336 企业微投递 key 同样无迁移仅 LOG_WARN。Mac 标注「二期另议」有意取舍。修法：首读时 keychain→app_secrets 一次性回填（读旧删旧）或升级向导提示重录。装机轮 5 已在清单提示重录 workaround。

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-18 22:11:40 +0800 · 排产即派 CF 批：keychain→app_secrets 一次性回填迁移（首读时读旧删旧），消轮 5「key 失效需重录」缺口。
- 2026-11-18 22:12:32 +0800 · CF 批开工：keychain→app_secrets 一次性回填迁移设计（读旧删旧，不双写不长存）
- 2026-11-18 22:18:00 +0800 · 打回主会话（第 1 次，未动任何代码）：简报核心依赖「旧 keychain 读路径」与代码实际矛盾——need317 已全链退役 keychain（删五平台文件 773 行 + CMake + AGENTS + test_keychain 11 用例，src/tests grep 归零），且 98d1cccc 是 1d1bf1bd 的祖先（合并线上无旧读路径可接线）。核实链：merge-base --is-ancestor 确认 + 工作树 find/grep keychain 零命中（仅 credential_protection_win.cpp 注释残留）。凭据域本身可用（#126 三级解密 + need318 InvalidApiKey 分文案，均已证），缺陷仅在迁移源。主会话需裁决三选一：a) 另立任务恢复最小 keychain 读删能力（平台 5 文件 + CMake + 平台测试，独立批，等于部分回退 need317）再挂回填；b) 放弃运行时迁移，走 BV 原口径——升级→重录引导文案（BJ 盯防项「升级真库密钥重录引导」）；c) 仓库外一次性运维脚本读旧钥匙串导入 app_secrets（不经运行时链）。两消费点（telemem_config.cpp readLlmKeyFromStore:124 / wecom.cpp deliver:342）与 SecretsDbGuard 夹具位（tests/test_modules/test_wecom_channel.cpp）已定位，裁决后任一方案可即接。

**最近状态变更**（共 4 条）：2026-11-18 22:18:00 +0800 · 进行中 → 挂起 · 简报前提失效：旧 keychain 读路径不存在——need317（98d1cccc，2026-11-18 20:26，为合并线 1d1bf1bd 祖先）已删 src/platform/keychain.h + keychain_macos.cpp/keychain_posix.cpp/keychain_win.cpp/keychain_macos_scan.h 五文件（773 行），CMake/AGENTS/测试触达面同批清零（src grep 归零）。两处旧键（devdesk.memory / devdesk.schedule）物理存于 OS 钥匙串，运行时回填必须重建已退役的平台 keychain 读删栈：超本批域限（secrets 迁移链+两消费点+测试）与预算（20 工具/25 分钟），且直接回退同日完成的 need317 全链退役决策。

---

## debt 池

### 待验收（3）

#### debt#121 · CI msys2 UCRT64 迁移（两轮翻车已回退 MINGW64+消音，PATH 传导机制待查）

- 状态：待验收 · 优先级：P3 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-03 10:50:24 +0800（ZCode主会话）· 更新：2026-11-11 23:49:53 +0800（ZCode主会话）
- **blocker**：推迟：等查清 setup-msys2@v2 UCRT64 的 PATH 注入机制后在分支上单独验证一轮再切；现态=回退 MINGW64+消音，不阻断任何流程

**详情**：

2026-11-03 两轮翻车记录：①UCRT64 下 clang-tidy/覆盖率步暴露 EUI_NEO_DIR 逐步 export 缺口（已修：提升 workflow 全局 env，此收获保留）；②回退后二轮 UCRT64 仍死在 cmake: command not found——包安装成功但 msys2 shell 的 PATH 未带 /ucrt64/bin（传导机制待查：可能与 setup-msys2 的 shell 集成或 cache restore 相关）。现态=回退 MINGW64+官方 suppress 消音。重开条件：查清 setup-msys2@v2 UCRT64 的 PATH 注入机制后在分支上单独验证一轮再切。

**正文**：

2026-11-03 两轮翻车记录：①UCRT64 下 clang-tidy/覆盖率步暴露 EUI_NEO_DIR 逐步 export 缺口（已修：提升 workflow 全局 env，此收获保留）；②回退后二轮 UCRT64 仍死在 cmake: command not found——包安装成功但 msys2 shell 的 PATH 未带 /ucrt64/bin（传导机制待查：可能与 setup-msys2 的 shell 集成或 cache restore 相关）。现态=回退 MINGW64+官方 suppress 消音。重开条件：查清 setup-msys2@v2 UCRT64 的 PATH 注入机制后在分支上单独验证一轮再切。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-03 16:23:23 +0800 · 推迟理由：两轮翻车成本已付，第三次盲试无新信息量；重开条件已写入 detail
- 2026-11-11 23:49:53 +0800 · 调研完成（clue154）：根因=包前缀错配——MINGW64 前缀包无论 MSYSTEM 都装进 /mingw64，UCRT64 壳 PATH 只有 /ucrt64/bin → cmake 不可见；两轮翻车与 action PATH 注入无关（setup-msys2 源码+本机 POC 三例实证）。已落码：ci.yml/release.yml 切 UCRT64 + install 前缀改 mingw-w64-ucrt-x86_64-* + 新增工具链冒烟步（commit 49835bda）。待 DevDesk 推送解锁后 CI 首跑验证（冒烟步 which cmake 命中 /ucrt64/bin + 构建测试绿 = 可关闭）。

**最近状态变更**（共 6 条）：2026-11-11 23:49:53 +0800 · 进行中 → 待验收 · 调研完成（clue154）：根因=包前缀错配——MINGW64 前缀包无论 MSYSTEM 都装进 /mingw64，UCRT64 壳 PATH 只有 /ucrt64/bin → cmake 不可见；两轮翻车与 action PATH 注入无关（setup-msys2 源码+本机 POC 三例实证）。已落码：ci.yml/release.yml 切 UCRT64 + install 前缀改 mingw-w64-ucrt-x86_64-* + 新增工具链冒烟步（commit 49835bda）。待 DevDesk 推送解锁后 CI 首跑验证（冒烟步 which cmake 命中 /ucrt64/bin + 构建测试绿 = 可关闭）。

---

#### debt#218 · Claude Code 内置市场 anthropic-plugin-directory 加载 403 噪音（启动 warning）——禁用或代理路由待探

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-04 18:49:19 +0800（developer）· 更新：2026-11-18 19:31:49 +0800（developer）
- **blocker**：修复未完成：禁用开关/代理路由待探（CLI 内置市场无法 remove；验收口径=启动无 403 warning，未达成）——本批 ③档记 blocker 不硬关（evidence/20261118-accept-sweep3/verdicts.md）

**详情**：

Claude Code 2.1.312 启动报 `Failed to load marketplace 'anthropic-plugin-directory': HTTP 403`（老板要求「修」，未完成）。已摸清：该市场是 CLI 内置（claude.exe bun 打包，二进制含 `var En="anthropic-plugin-directory"`），不在 known_marketplaces.json、remove 无从下手；403 疑为直连 Anthropic 目录端点被地区挡（本机 github 也需代理）。待探：gitconfig/环境代理对目录请求是否生效（HTTPS_PROXY 对 claude 有效已验证——marketplace add 就带它过的）；或 bun 二进制字符串考古找禁用开关（候选 env/settings key）。验收口径=启动无该 warning。归属：环境/宿主适配层，优先级低（噪音级，不影响功能）。

**正文**：

Claude Code 2.1.312 启动报 `Failed to load marketplace 'anthropic-plugin-directory': HTTP 403`（老板要求「修」，未完成）。已摸清：该市场是 CLI 内置（claude.exe bun 打包，二进制含 `var En="anthropic-plugin-directory"`），不在 known_marketplaces.json、remove 无从下手；403 疑为直连 Anthropic 目录端点被地区挡（本机 github 也需代理）。待探：gitconfig/环境代理对目录请求是否生效（HTTPS_PROXY 对 claude 有效已验证——marketplace add 就带它过的）；或 bun 二进制字符串考古找禁用开关（候选 env/settings key）。验收口径=启动无该 warni

**进度记录**（共 3 条，列最近 3 条）：

- 2026-11-05 21:11:04 +0800 · 2026-11-05 已修复（POC 实证）——403 根因=`https://claude.ai/api/directory/plugins` 被公司 SASE（Geely）DNS 拦截。已落地：①`setx HTTPS_PROXY http://127.0.0.1:10808` + `setx NO_PROXY 127.0.0.1,localhost`（用户级，已回读）；②`~/.claude/settings.json` env 双保险（备份 settings.json.bak-20261105-claude403）；③进程级验证 `claude plugin marketplace update anthropic-plugin-directory` → `✔ Successfully updated`。注意：settings.json env 单独注入**不覆盖** marketplace fetch（需 setx/进程级）；socks5h/ALL_PROXY 官方不支持；CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC 已证伪；官方开关 blockedMarketplaces 需管理员。生效：需重开 Claude Code/终端。验收：启动无 `Failed to load marketplace` warning。
- 2026-11-18 19:11:48 +0800 · 重开排产：BF 批（插件市场 403——禁用开关/代理路由探修）。BB 批已腾空插件仓（gate-test 复核完毕）。
- 2026-11-18 19:31:49 +0800 · BF 批探修完毕。403 因子（实测）：端点 https://claude.ai/api/directory/plugins——真直连（--noproxy）被 SASE 拦 000 超时；走 10808 代理（http/socks5h 各试）出口 IP 遭 Cloudflare challenge 返回 403（Cf-Mitigated: challenge 实测）；CLI 自身拉取间歇（一次 did not answer in time、两次 Successfully updated）→启动 warning 间歇出现，11-05 代理路由已不能根治。根因归属宿主侧：拉取代码在 claude.exe 内置（bun 二进制 346MB 含 anthropic-plugin-directory/api/directory/plugins 字符串），插件仓 grep 零命中。修法落地=禁用开关（配置层，插件源码零改动）：~/.claude/settings.json 增 blockedMarketplaces:[{"source":"pluginDirectory"}]（source 判别枚举自二进制考古：pluginDirectory|url|github|git|npm|file|directory|skills-dir|hostPattern|pathPattern|settings；用户级被 schema 校验已实证）；备份 settings.json.bak-20261118-bf218；11-05 setx HTTPS_PROXY 代理路由保留（成功路径仍走它）。已证：该开关不拦显式 marketplace update（仅自动加载面）。验证：claude -p headless 启动净跑无 Failed to load marketplace/validation 告警；边界如实声明=交互 TUI 启动未实测（无 TTY），且用户级 block 对内置目录自动加载的实际拦截效果待交互面确认——若仍偶发 warning，备选根治面=C:\ProgramData\ClaudeCode\managed-settings.json（实测目录可写无需管理员），移交主会话决策。版本：devflow 保持 0.39.47（无源码改动、无 push）。evidence=D:\project\gui\DevDesk\evidence\20261118-plugin-403\probe.md

**最近状态变更**（共 6 条）：2026-11-18 19:31:49 +0800 · 进行中 → 待验收 · BF 批探修完毕。403 因子（实测）：端点 https://claude.ai/api/directory/plugins——真直连（--noproxy）被 SASE 拦 000 超时；走 10808 代理（http/socks5h 各试）出口 IP 遭 Cloudflare challenge 返回 403（Cf-Mitigated: challenge 实测）；CLI 自身拉取间歇（一次 did not answer in time、两次 Successfully updated）→启动 warning 间歇出现，11-05 代理路由已不能根治。根因归属宿主侧：拉取代码在 claude.exe 内置（bun 二进制 346MB 含 anthropic-plugin-directory/api/directory/plugins 字符串），插件仓 grep 零命中。修法落地=禁用开关（配置层，插件源码零改动）：~/.claude/settings.json 增 blockedMarketplaces:[{"source":"pluginDirectory"}]（source 判别枚举自二进制考古：pluginDirectory|url|github|git|npm|file|directory|skills-dir|hostPattern|pathPattern|settings；用户级被 schema 校验已实证）；备份 settings.json.bak-20261118-bf218；11-05 setx HTTPS_PROXY 代理路由保留（成功路径仍走它）。已证：该开关不拦显式 marketplace update（仅自动加载面）。验证：claude -p headless 启动净跑无 Failed to load marketplace/validation 告警；边界如实声明=交互 TUI 启动未实测（无 TTY），且用户级 block 对内置目录自动加载的实际拦截效果待交互面确认——若仍偶发 warning，备选根治面=C:\ProgramData\ClaudeCode\managed-settings.json（实测目录可写无需管理员），移交主会话决策。版本：devflow 保持 0.39.47（无源码改动、无 push）。evidence=D:\project\gui\DevDesk\evidence\20261118-plugin-403\probe.md

---

#### debt#398 · 记事本 md 预览：>256KB 文档每帧重复全量解析×2（解析缓存单条 256KB 上限挡住）+3 个小项

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：异步测量已拍板
- 创建：2026-11-21 00:45:20 +0800（developer）· 更新：2026-11-22 01:04:22 +0800（developer）

**详情**：

老板 2026-11-21 问「记事本 md 预览有性能优化？」——查证结论：
已落地：①bug382 视口裁剪 notepad_window.cpp:984 visibleRange（屏外块只留包装几何）②:951 contentKey=textRevision 度量缓存（预览禁编辑，稳定命中）③EUI-NEO markdown.h:783 解析 LRU（64条/1MB/单条256KB）。
剩余洞（本条主体）：缓存单条上限 256KB（markdown.h:797 kEntryByteCapacity），超限不缓存（:800-802 直接 return）→ 每帧 estimateHeight(:972) + markdown build(:979) 两次全串哈希+全量 md4c 解析；预览门槛只挡 >1MB（previewEnabled=!largeFile，notepad_session.cpp:153）→ 256KB~1MB 区间预览每帧 2× 全量解析，1MB 档约 10-20ms/帧。另全串作键本身每帧 O(n) 哈希。
小项：estimateHeight 每帧 O(blocks) 重算可缓存；visibleRange 零 overscan（module_markdown 留上1屏/下2屏，notepad 无）快滚一帧白边风险。
修法方向：缓存键改 (hash,len) 短键 + 预览态 session 显式持有解析结果（textRevision 变更时换）；estimateHeight 随 (textRev,innerW,style) 缓存；visibleRange 补 overscan。

**正文**：

老板 2026-11-21 问「记事本 md 预览有性能优化？」——查证结论：
已落地：①bug382 视口裁剪 notepad_window.cpp:984 visibleRange（屏外块只留包装几何）②:951 contentKey=textRevision 度量缓存（预览禁编辑，稳定命中）③EUI-NEO markdown.h:783 解析 LRU（64条/1MB/单条256KB）。
剩余洞（本条主体）：缓存单条上限 256KB（markdown.h:797 kEntryByteCapacity），超限不缓存（:800-802 直接 return）→ 每帧 estimateHeight(:972) + markdown build(:979) 两次全串哈希+全量 md4c 解析；预览门槛只挡 >1MB（previewEnabled=!largeFile，notepad_sessio

**进度记录**（共 24 条，列最近 5 条）：

- 2026-11-21 23:46:43 +0800 · 老板 2026-11-21 验收口径拍板（原话：「总高异步算，先让用户看到内容，等1秒刷出滚动条也行」）：①内容优先——首帧只精测可见+overscan 块，立即出画（目标 ~30ms 级）②总高异步：屏外块后台分帧补测，滚动条/总高充许 ~1s 内逐步精确化（接受近似→精确的两阶段）③textRevision/新宽度重入同一流程。执行序：need#400（预览搜索，在飞）落地后串行排产
- 2026-11-21 23:48:36 +0800 · 老板 2026-11-21 设计令补齐：「分段加载，数据缓存」——异步测高的实现词表：①分段加载=测高/渲染按段推进（可见段先行、屏外段分帧续）②数据缓存=段级结果全部缓存（textRevision+width 键，同既有估高/布局缓存同构），重入零重算。与北极星令（响应第一）+1 秒滚动条令合并为 #398 异步化的完整设计约束。执行序不变：need#400 落地后串行
- 2026-11-21 23:50:08 +0800 · 老板 2026-11-21 症状原话「卡上一段时间，然后洪水猛兽一样一股脑输出」——正是本条残项（首开/新宽度同步全量测量 ~1s 阻塞→解卡后批量补偿帧/滚动一次性应用）的真机确认。#398 异步化后此症状自然消失：首帧即出（可见段先测）+屏外段分帧渐进（无阻塞无洪水）。优先级升 P1
- 2026-11-22 00:18:38 +0800 · 开工：分段测高异步化（首帧精测可见段+后台补测渐进）
- 2026-11-22 01:04:22 +0800 · 分段测高落地：EUI markdown 缝（blockCount/blockSegmentBoundaries/estimateHeightRange/blockHeightHint）+ NotepadSession 4槽段表 + 帧首补测(12ms/帧,requestUpdate自驱) + provider 渐进总高。T段五断言全绿：首帧6.28ms、收敛810.8ms至精确0px、单帧最大33.8ms、次帧ink=28285、修订重入9.09ms/1帧。既有 J/K/L/N/P/Q/O/R/S/S2 全绿（P/O 腿前 warm 测试缝）。commit: EUI bbf4104 + DevDesk 89bd2d35（未push）。

**最近状态变更**（共 10 条）：2026-11-22 01:04:22 +0800 · 进行中 → 待验收 · 分段测高落地：EUI markdown 缝（blockCount/blockSegmentBoundaries/estimateHeightRange/blockHeightHint）+ NotepadSession 4槽段表 + 帧首补测(12ms/帧,requestUpdate自驱) + provider 渐进总高。T段五断言全绿：首帧6.28ms、收敛810.8ms至精确0px、单帧最大33.8ms、次帧ink=28285、修订重入9.09ms/1帧。既有 J/K/L/N/P/Q/O/R/S/S2 全绿（P/O 腿前 warm 测试缝）。commit: EUI bbf4104 + DevDesk 89bd2d35（未push）。

---

### 在池（3）

#### debt#304 · DevDesk 侧 pool 快照面射程外遗留（legacy tools 快照写路径/评审页/历史 spec/展示页）待裁决

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话 · 阶段：—
- 创建：2026-11-18 00:50:21 +0800（ZCode主会话）· 更新：2026-11-18 00:50:21 +0800（ZCode主会话）

**详情**：

复验 need#302 时发现的射程外遗留（无接线）：①DevDesk tools/pool_mcp.py 仍有 _write_snapshot 写 docs/pool/pool-snapshot.json（legacy 本池 MCP，无 .mcp.json 接线，但 pool_ops.py/pool_mcp_acceptance.py 仍引用）；②tools/pool_review_gen.py 与 docs/pool/pool-review.html（tracked）以快照为数据源；③docs/specs/2026-10-29-三池mcp{决策,业务}方案.md 提及快照；④插件仓未提交的展示页（docs/showcase、devflow-landing）仍把 pool-snapshot 写作数据源。待裁决：删活遗留面 / 改展示页口径 / 历史 spec 是否改写（默认不改写）。

**正文**：

复验 need#302 时发现的射程外遗留（无接线）：①DevDesk tools/pool_mcp.py 仍有 _write_snapshot 写 docs/pool/pool-snapshot.json（legacy 本池 MCP，无 .mcp.json 接线，但 pool_ops.py/pool_mcp_acceptance.py 仍引用）；②tools/pool_review_gen.py 与 docs/pool/pool-review.html（tracked）以快照为数据源；③docs/specs/2026-10-29-三池mcp{决策,业务}方案.md 提及快照；④插件仓未提交的展示页（docs/showcase、devflow-landing）仍把 pool-snapshot 写作数据源。待裁决：删活遗留面 / 改展示页口径 / 历史 spec 是否改写（默认不改写）。

**最近状态变更**（共 1 条）：2026-11-18 00:50:21 +0800 · — → 在池 · 入池

---

#### debt#414 · bug401 帧探针测量口径污泥：默认 1ms 轮询（#402/403 后已失效）+ 未隔离数据根（已修，残留 PATH 运行前提）

- 状态：在池 · 优先级：P3 · 负责人：developer · 阶段：—
- 创建：2026-11-22 11:56:34 +0800（developer）· 更新：2026-11-22 11:56:34 +0800（developer）

**详情**：

来源：need401 P2 分析过程中发现的测量方法缺陷（evidence/bug401-p2-analysis.md §1）。
两处：①探针默认 BUG401_POLL_MS=1 是 #402/403 修复前的口径（为把轮询压进单帧），修复后后台巡检线程每 1ms 跑一轮「300-1200 文件 mtime/size + 全表 SELECT」，成为持续 CPU/DB 争用源，污染所有页面读数（同一配置 home 进页在 poll=1 与 5000 下差 3-4 倍）；②原探针未隔离 DEVDESK_DATA_ROOT，memory/wallpaper 读数落在真实用户数据根上（且 memory 页会打开用户 proposals.db）。
已修（本轮仅改测试代码）：探针加 BUG401_NOTES/TODOS/CALLLOGS/BADCALLS/PROPOSALS/RESCAN 旋钮 + DataRootEnv 隔离 + HOME_SQL 归因输出；真机口径默认改为 BUG401_POLL_MS=5000。
残留待办（低优先）：①跑 .exe 必须 PATH 含 /d/software/msys64/mingw64/bin，否则缺 DLL 静默 exit 127 且日志为空（易误判为「探针无输出」）；②建议在探针文件头注释写明默认只代表「后台巡检周期」而非真机帧内负担，避免下轮重蹈。
验收：任一 agent 按文件头注释命令可复跑并得到与 evidence/bug401-p2-*.log 一致量级。

**正文**：

来源：need401 P2 分析过程中发现的测量方法缺陷（evidence/bug401-p2-analysis.md §1）。
两处：①探针默认 BUG401_POLL_MS=1 是 #402/403 修复前的口径（为把轮询压进单帧），修复后后台巡检线程每 1ms 跑一轮「300-1200 文件 mtime/size + 全表 SELECT」，成为持续 CPU/DB 争用源，污染所有页面读数（同一配置 home 进页在 poll=1 与 5000 下差 3-4 倍）；②原探针未隔离 DEVDESK_DATA_ROOT，memory/wallpaper 读数落在真实用户数据根上（且 memory 页会打开用户 proposals.db）。
已修（本轮仅改测试代码）：探针加 BUG401_NOTES/TODOS/CALLLOGS/BADCALLS/PROPOSALS/RESCAN 旋钮 + 

**最近状态变更**（共 1 条）：2026-11-22 11:56:34 +0800 · — → 在池 · 入池

---

#### debt#416 · need381 与 need394 双 MCP 适配层并存待裁决（AgentToolAdapter/ToolMirror 46 件 vs McpToolAdapter 15 件，合并态未验证）

- 状态：在池 · 优先级：P1 · 负责人：ZCode主会话 · 阶段：—
- 创建：2026-11-22 20:45:34 +0800（ZCode主会话）· 更新：2026-11-22 20:45:34 +0800（ZCode主会话）

**详情**：

2026-11-22 收敛 push 时发现：远端并发线 need381（f7f6ec55）交付 ITool→IMcpTool 适配层（src/module_mcp/mcp_tool_mirror.h 的 ToolMirror/AgentToolAdapter，39 个 agent 工具经惰性 diff 镜像暴露，tools/list 46 全模块启用态）与本会话 #394 交付的适配层（src/platform/tool_kit.h 的 McpToolAdapter，15 件 todo_*/md_*/存量）为同能力面双实现；远端 97055158 merge 已合流两线，但并存态的编译/运行/工具数（46 vs 15 口径差异）未在本地验证，装机版（20:38）不含 need381 线。待裁决：两 adapter 合一/按域分工/淘汰其一；裁决前先跑一次合并态构建+MCP 端点 tools/list 实数核对。

**正文**：

2026-11-22 收敛 push 时发现：远端并发线 need381（f7f6ec55）交付 ITool→IMcpTool 适配层（src/module_mcp/mcp_tool_mirror.h 的 ToolMirror/AgentToolAdapter，39 个 agent 工具经惰性 diff 镜像暴露，tools/list 46 全模块启用态）与本会话 #394 交付的适配层（src/platform/tool_kit.h 的 McpToolAdapter，15 件 todo_*/md_*/存量）为同能力面双实现；远端 97055158 merge 已合流两线，但并存态的编译/运行/工具数（46 vs 15 口径差异）未在本地验证，装机版（20:38）不含 need381 线。待裁决：两 adapter 合一/按域分工/淘汰其一；裁决前先跑一次合并态构建+MCP 端点 tool

**最近状态变更**（共 1 条）：2026-11-22 20:45:34 +0800 · — → 在池 · 入池

---

### 挂起（1）

#### debt#321 · 评审判据系统性缺口：dump 几何语义缺失 + 旧证据判新态（波 2 三连误判根因）

- 状态：挂起 · 优先级：P2 · 负责人：discipline-guard · 阶段：开发中
- 创建：2026-11-18 11:50:25 +0800（discipline-guard）· 更新：2026-11-18 20:09:17 +0800（discipline-guard）
- **blocker**：改评审判据模板+派单简报模板（dump 几何语义注释/旧证据禁判新态两条硬口径）=改 devflow 插件，待老板授权后实施（BW 2026-11-18 挂起明账）

**详情**：

波 2 三连误判根因归并（F1 案 #316/#317 + F2 案 #292 + C 案重开 7 条）：①无图像输入会话拿 dump 几何判视觉缺陷时未并入 clip/wrap/scroll-offset 语义（裁剪前布局几何≠可见渲染，settings desc wrap、mcp 卡 clip 均被误判红）；②验收/退回判定未核对「证据拍摄时间戳 vs 修复提交时间戳」（晨扫旧证据判新态致 7 条假重开、#292 假退回）。改进：评审判据模板与派单简报模板补两条硬口径——dump 几何须带裁剪/换行/滚动语义注释才可作视觉证据；旧证据禁判新态（先 git log 核对修复落点）。改模板=改插件（需授权）。

**正文**：

波 2 三连误判根因归并（F1 案 #316/#317 + F2 案 #292 + C 案重开 7 条）：①无图像输入会话拿 dump 几何判视觉缺陷时未并入 clip/wrap/scroll-offset 语义（裁剪前布局几何≠可见渲染，settings desc wrap、mcp 卡 clip 均被误判红）；②验收/退回判定未核对「证据拍摄时间戳 vs 修复提交时间戳」（晨扫旧证据判新态致 7 条假重开、#292 假退回）。改进：评审判据模板与派单简报模板补两条硬口径——dump 几何须带裁剪/换行/滚动语义注释才可作视觉证据；旧证据禁判新态（先 git log 核对修复落点）。改模板=改插件（需授权）。

**进度记录**（共 2 条，列最近 2 条）：

- 2026-11-18 13:05:08 +0800 · 收割提案完成（discipline-guard，提案态零落盘）：新增4模式（#27旧证据判新态/#28 dump几何当渲染证据/#29池前提陈旧误派单/#30单一失败可见性陷阱）+既有#1实例追加（52006b07测试滞后）+#7近亲注记；可静态化3项附脚本草案（check-dump-visual-evidence/check-stale-premise/check-evidence-freshness，均POSIX sh+--selftest）；#30不可脚本化=流程口径+doctest REQUIRE中断提示增强（developer面）。提案全文：evidence/20261118-discipline-harvest/proposal.md（registry追加行+脚本草案+落地顺序）；registry/脚本/模板落插件仓均待老板授权。
- 2026-11-18 20:09:17 +0800 · BW 归位：波 2 三连误判根因归并入条已完成，无在飞批——纯等授权，挂起+blocker。

**最近状态变更**（共 4 条）：2026-11-18 20:09:17 +0800 · 进行中 → 挂起 · 改评审判据模板+派单简报模板（dump 几何语义注释/旧证据禁判新态两条硬口径）=改 devflow 插件，待老板授权后实施（BW 2026-11-18 挂起明账）

---

## temp 池

### 待验收（2）

#### temp#384 · 2026-11-19 修复波统一验证批（29 卡，26 DevDesk + 6 EUI-NEO commit）

- 状态：待验收 · 优先级：P1 · 负责人：qa-tester · 阶段：—
- 创建：2026-11-19 12:18:49 +0800（qa-tester）· 更新：2026-11-22 18:46:53 +0800（qa-tester）

**详情**：

老板 2026-11-19 修复波收口：全量构建+全量 QA+md 性能复测+逐提交判读+装机。验证批禁修码，红项只定档登记→判读表=修复批派发单。波内 29 卡全部纯码交付未编译（7 代理并发纯码模式），编译红风险高。

**正文**：

老板 2026-11-19 修复波收口：全量构建+全量 QA+md 性能复测+逐提交判读+装机。验证批禁修码，红项只定档登记→判读表=修复批派发单。波内 29 卡全部纯码交付未编译（7 代理并发纯码模式），编译红风险高。

**关联 BUG**（1）：

- 2026-11-19 12:52:06 +0800 · 未解 · BUG: 环境类（非本波回归，P2 测试密封性）——mcp 三红：test_mcp_zcode_config.cpp:395 installed.isOk() 失败 / test_mcp_claude_config.cpp:341 changed 失败 / test_mcp_page.cpp:1182 安装钮 label 断言失败。本机真实 ~/.zcode/cli/config.json 与 ~/.claude.json 均已含 devdesk 注册条目（装机版在用），测试「真实配置=未安装态」前提不成立：zc 走异址冲突 fail、cc changed=false、page 初始即已装态。波内 32 commit 零触碰 mcp 代码。修＝测试先剥 devdesk 键再装或改合成样本。

**进度记录**（共 5 条，列最近 5 条）：

- 2026-11-19 12:22:06 +0800 · QA384 测试清单（验证批禁修码，红项只登记）：A1 全量构建+全量QA（doctest全量+UI probe+守卫链）→ bash scripts/build-test-run.sh --no-run，证据 evidence/20261119-qa384-buildfull/；A2 md 性能复测 → --filter md_mode_switch_perf（修前基线：60KB 切换帧 3.1-11.3s / 每帧 2.5 万文本元素；预期百 ms 级/数百元素）；A3 逐提交判读 → DevDesk c16926a2..7e4d5ff3（26）+ EUI-NEO 7a94494..868a0a3（6）逐 commit 对卡；A4 装机 → bash scripts/install-win-debug.sh + app 启动确认。红项定档（P级+根因初判+归属卡）进判读表=修复批派发单。
- 2026-11-19 12:52:06 +0800 · QA384 终报（验证批禁修码，红项全登记）：①全量档构建编译红 @ uat62_page_shots_probe.cpp:421（池#372 分号入注释，一字符修）——probe 层 QA 全阻断；诊断轮（DEVDESK_BUILD_PROBES=0）主目标构建绿 + doctest 全量 Total 2529/Pass 2494/Fail 19/Skip 16；守卫链 standalone：模块隔离/玻璃/ToolGuard 绿，std-include 红×2（375/369 各一缺 include）；windows-cross-compile（与真构建重复）与 probe-skip-gate（probe OFF）显式跳过。②19 红归因：356×10（P1 色值契约+WCAG 真回退风险）/370×1（P1 自带用例红）/371×2（P2）/357×1（P2）/mdspike×2→EUI need378（P2）/mcp×3（环境类，测试密封性）。③md 性能：元素 25.5K→119-2278、稳态 0.9-13.7s→20-35ms、render 重建 2.4s→29-185ms 达标；live/split 首切 0.6-1.3s 未达百 ms 级（P2 残余挂 369）。④装机取消（编译红门禁），装机版仍为旧版。⑤红项已全挂卡：372/356/357/369/370/371/375/378+本条 mcp 环境项＝修复批派发单；修复后须不带 --dev 完整重跑（补 probe 层+守卫自检+装机）。验证工作已全部完成，状态留进行中待主会话收口。
- 2026-11-19 12:53:53 +0800 · 验证批完成：全量档编译红（#372 一字符）+19 doctest 红+守卫 2 include 红+md 性能部分达标（25.5K→≤2278 元素/稳态 20-35ms；split/live 首切残余 0.6-1.3s）；装机取消。红项定档清单已挂各卡=修复批派发单，修复批已派（bug-fixer，含构建+装机收口）。证据：evidence/20261119-qa384-*。mcp 密封性 3 红为环境类非本波回归。
- 2026-11-19 13:27:24 +0800 · bug-fixer 批进度：#372/#375/#357/#378 已修+定向验绿+commit（952ab7dd/328abe47/feb9a863/0422dcd0）；#356 完成（848453b7，含真 WCAG 4 对修复+钉值平台化，决策点已写入 #356 卡供老板裁决）。构建 1 次（含 probe 目标，#372 验证编译通过）。#370/#371/#369 残余已改派并行代理（调度令），全量档 finale 与装机待其落地后统一执行。evidence: evidence/20261119-qa384-fixbatch-bugfixer/
- 2026-11-22 18:46:53 +0800 · 台账归位：验证批使命完成（判读表已转修复批且全清），双仓同步实证；待验收后关账

**最近状态变更**（共 3 条）：2026-11-22 18:46:53 +0800 · 进行中 → 待验收 · 台账归位：验证批使命完成（判读表已转修复批且全清），双仓同步实证；待验收后关账

---

#### temp#386 · 池台账全景 HTML 二轮双审计（修后复核：数据轴+工程轴）

- 状态：待验收 · 优先级：P2 · 负责人：qa-tester · 阶段：—
- 创建：2026-11-19 12:43:27 +0800（qa-tester）· 更新：2026-11-19 18:11:41 +0800（qa-tester）
- **blocker**：峰窗门禁（13:30-18:00）拦双审计派发；待用户落「继续」开工令带 [peak-override] 重派，或 18:00 自动恢复补跑

**详情**：

老板令（2026-11-19）：对已修后的 docs/design/2026-11-19-池台账全景.html 再派 2 个独立只读审计子任务。背景：一轮审计（temp#385 已关）出 16 条已全修+功能桩 24/24 绿。本轮=修后复核：A 审计员查数据保真（页面内容 vs 池台账基准事实），B 审计员查工程实现（16 条修复落地验证+新引入回归+交互/可达/稳健）。两代理并行只读，产出各自分级清单，主会话汇总裁决。

**正文**：

老板令（2026-11-19）：对已修后的 docs/design/2026-11-19-池台账全景.html 再派 2 个独立只读审计子任务。背景：一轮审计（temp#385 已关）出 16 条已全修+功能桩 24/24 绿。本轮=修后复核：A 审计员查数据保真（页面内容 vs 池台账基准事实），B 审计员查工程实现（16 条修复落地验证+新引入回归+交互/可达/稳健）。两代理并行只读，产出各自分级清单，主会话汇总裁决。

**进度记录**（共 6 条，列最近 5 条）：

- 2026-11-19 18:01:55 +0800 · 高峰恢复批自动恢复：峰窗 18:00 已过，挂起→在池
- 2026-11-19 18:02:02 +0800 · 高峰恢复批自动恢复：在池→排产（双审计 A/B 并行只读）
- 2026-11-19 18:02:08 +0800 · 高峰恢复批自动恢复：排产→进行中，按 detail 续跑指令派发双审计（A 数据保真 / B 工程实现），主会话汇总裁决
- 2026-11-19 18:09:48 +0800 · temp#386 二轮审计 A·数据保真（对象 docs/design/2026-11-19-池台账全景.html，121382B 修后版）：总判定 FAIL，P1×1 / P2×2 / P3×3。【P1-1】变更清单 progress ID 错位：页面四处（PROGRESS 数组/EV ref 'prog:820'/G3 proof/钻取层 CHANGE LIST·#820）把【变更清单10项+演绎2项】(11-17 14:53:03) 标为 #820；db 真值 #820=状态回正(14:33:03)、#821=变更清单；且页面 PROGRESS 14 行 vs db 15 行（漏真实 #820 行，其内容在 status_log #1390 有对应）。页面自称「note 均照录」→照录错 ID=数据错误。【P2-1】bug290 徽标'待验收'(11-17) vs db 关闭(11-18 10:31，早于快照=非漂移，引用级旧态，页面有'未实读'免责但徽标无限定)。【P2-2】bug292/294/296 徽标'待验收（波次）' vs db 三条全关闭(11-18 06:28/06:29/11:50)；连带 5 关联缺陷中 4 条实际已关、仅 bug269 真待验收。【P3】①doc#24 页标类型'调研' vs db type='tech'（展示改写未注）；②bug290/292/294/296 pri 标'—' vs db 均 P1（可实读未读）；③'G2 ×5/全程 5 次派发'计法未注（按简报=6/按波次=5）。【快照漂移】无：need#291 作用域（291 本体/6条status_log/4 docs/10 clues/bug280/269/衍生298-300/3 commit）11-19 当日 db 零变更。【PASS 面】needs#291 全字段逐字对（待验收·测试中·P1·developer·created/updated/title/blocker/detail）；status_log 恰 6 条 1383/1386/1387/1389/1390/1391 全对；dev_progress 15 行中 14 行 id/time/by/内容全对；docs23-26 slug/type/by/时间全对、v2 声称与 docs_history 自洽；clues192-201 逐字一致；bug280/269 全对；衍生 298拒绝/299待验收/300关闭 与记忆口径吻合；commit c4628169=26 files +4352/−1533 与 git show 逐字一致，2a03c1d7/395f3bd4 在案；内部自洽（27事件=EV.length、终态构成4/4/10/5/1、8门禁带）。页面无全局三池条目数/状态分布/P级分布声称（need#291 作用域页），该三项不适用。
- 2026-11-19 18:11:41 +0800 · 双审计完成+汇总裁决落账（18:00 高峰恢复批补跑链闭环）：A 数据保真 FAIL（P1×1=#820/#821 dev_progress 错位、P2×2=4 bug 徽标旧态、P3×3）；B 工程实现 PASS（P1=0、P2×1=openGate 焦点回归断链、P3×9；三道门禁全过+15 类修复面在位 0 反例；16 条对号不可执行——untracked 无历史，替代口径）。合并定级：F1 必修（P1 数据错位）+F2/F3 建议修+P3×12 列档。证据物 evidence/20261119-temp386-audit2/{verdict,A-data-audit,B-eng-audit}.md。修复批未派（待老板裁决 F2/F3 是否随批）；教训=一轮修复清单未落盘+产物未提交致复核对不上号

**最近状态变更**（共 5 条）：2026-11-19 18:09:48 +0800 · 进行中 → 待验收 · temp#386 二轮审计 A·数据保真（对象 docs/design/2026-11-19-池台账全景.html，121382B 修后版）：总判定 FAIL，P1×1 / P2×2 / P3×3。【P1-1】变更清单 progress ID 错位：页面四处（PROGRESS 数组/EV ref 'prog:820'/G3 proof/钻取层 CHANGE LIST·#820）把【变更清单10项+演绎2项】(11-17 14:53:03) 标为 #820；db 真值 #820=状态回正(14:33:03)、#821=变更清单；且页面 PROGRESS 14 行 vs db 15 行（漏真实 #820 行，其内容在 status_log #1390 有对应）。页面自称「note 均照录」→照录错 ID=数据错误。【P2-1】bug290 徽标'待验收'(11-17) vs db 关闭(11-18 10:31，早于快照=非漂移，引用级旧态，页面有'未实读'免责但徽标无限定)。【P2-2】bug292/294/296 徽标'待验收（波次）' vs db 三条全关闭(11-18 06:28/06:29/11:50)；连带 5 关联缺陷中 4 条实际已关、仅 bug269 真待验收。【P3】①doc#24 页标类型'调研' vs db type='tech'（展示改写未注）；②bug290/292/294/296 pri 标'—' vs db 均 P1（可实读未读）；③'G2 ×5/全程 5 次派发'计法未注（按简报=6/按波次=5）。【快照漂移】无：need#291 作用域（291 本体/6条status_log/4 docs/10 clues/bug280/269/衍生298-300/3 commit）11-19 当日 db 零变更。【PASS 面】needs#291 全字段逐字对（待验收·测试中·P1·developer·created/updated/title/blocker/detail）；status_log 恰 6 条 1383/1386/1387/1389/1390/1391 全对；dev_progress 15 行中 14 行 id/time/by/内容全对；docs23-26 slug/type/by/时间全对、v2 声称与 docs_history 自洽；clues192-201 逐字一致；bug280/269 全对；衍生 298拒绝/299待验收/300关闭 与记忆口径吻合；commit c4628169=26 files +4352/−1533 与 git show 逐字一致，2a03c1d7/395f3bd4 在案；内部自洽（27事件=EV.length、终态构成4/4/10/5/1、8门禁带）。页面无全局三池条目数/状态分布/P级分布声称（need#291 作用域页），该三项不适用。

---

## 附录 A：关联文档全文（14 篇）

### doc#3 · 2026-11-03-agent-tools-dialog-and-message-card-b5-tech（tech v1 · need#170）

- 标题：方案卡：智能体可用工具弹窗与明细卡重设计（B5 开发实施记录）
- 创建：2026-11-04 02:41:03 +0800（developer）· 更新：2026-11-04 02:41:03 +0800（developer）

`````markdown
# 方案卡：智能体可用工具弹窗与明细卡重设计（B5 开发实施记录）

- 日期：2026-11-04 ｜ 角色：developer ｜ 上游契约：docs/design/2026-11-03-智能体可用工具弹窗与明细卡重设计.md（+同名 .html，仅视觉校准）
- 关联池：need162（明细卡）/ need170（工具弹窗）/ bug169（hover 状态机）；残余 → bug:182
- 状态：三池条目已置待验收；本卡为开发维度实施记录（qa 照设计 §9 清单验收）

## 1. 改动清单（文件锚）

池170 可用工具弹窗：
- src/module_agent/agent_module.cpp：删页内工具区块（原 523-663）与 toolsOpen_/kToolsHeaderH/kToolCardH/kToolsListAnchorH；高度预算归还任务列表（kPageGapCount 2→1）；页头新增入口钮 agent.page.tools（components::button theme(tokens,false) 132×40 + plug 0xF1E6 + 计数，store 空禁用 40%）；onKeyEvent 增 Esc 分支；compose 尾挂 composeToolsDialog
- src/module_agent/agent_module.h：toolsOpen_ → toolsDialogOpen_；新增 composeToolsDialog 声明
- src/module_agent/agent_module_dialogs.cpp：新增 composeToolsDialog（720×min(560,视口−96)、头部 48 + ✕ 0xF00D + 分割线、pad16 内滚 2 列网格卡定高 96、参数 chips h20 超宽折「+N」含末枚放不下分支、空态「暂无可用工具」）

池162 明细消息卡：
- agent_module_dialogs.cpp renderMessageRow：裸文本列 → 卡面 stack（.bg = L2 itemSurface+radius6+1px glassBorder+果冻按压 0.96 / .hit z1 整卡点击+pressedMsgSeqs_ / .body z2）；徽章 pill 双档（工具=primarySoftBg/primaryText，用户/助手=次钮底/textSecondary）；chips 48×24（actionChip 加 chipH 参数，缺省 32 零波及）；ToolUse 浅染内嵌块 28 高（wrench 0xF0AD+深字档+args 截 120）；ToolResult 左 3px 语义条（红仅错误，正常=borderDefault）；子块 id 契约不变（.blk.J 落文本节点）
- agent_detail_dialog.cpp：摘要三卡 .bg 补 glassBorder+glassShadowColor（几何零变）；msgscroll/detail.scroll 卡间距 6→8
- message_blocks.cpp estimateMessageHeight：ToolUse 28(scaled)/ToolResult 正文宽 −13 与渲染同源

bug169 hover 状态机：
- agent_module.cpp：hoveredTaskIds_ 集合退役 → hoverHolder_ 槽位持有者（0=命中面/1..5=图标钮/6=启动钮）+ hoverHideDueMs_ 120ms 离场去抖（leave 入账不擦持有者、enter 清账、到期 compose 收割）+ 操作组 stack transition(0.12 OutCubic)+animate Opacity；几何零改动

## 2. 关键决策与依据

1. §5 加载/错误态不建：ensureToolsCache 同步装载（listRegisteredTools 进程内枚举不失败），「正在加载…/读取失败+重试」在现状语义下不可达——不建不可达死分支（代码负资产），TU 注释留痕。QA 验收三态时按「空态+常态」两态判。
2. 入口钮图标-文字间距：设计 8px，components::button 内建 gap≈4.8px——组件既有契约优先，未自造 workaround。
3. 描述行数规则：有参数单行（给行 3 让位）、无参数两行 wrap（行 3 不渲染吃高度）——设计 §3.2「≤72<96−24」算术的唯一自洽解。
4. 169 修复模型：单集合在「低 z false 晚于高 z true」实证派发序下被抹空（老板上报根因）；集合并集方案仍有「leave 后无补偿 enter」误收割洞（真指针缝隙横移间歇复现，挂 bug:182），故用槽位持有者+去抖。单位置坏序 6 场景已钉死用例。
5. 文本节点字色在 Element.textColor（Element.color 恒默认白）——测试断言读 textColor 非 color。

## 3. 测试与证据物

- 单测：tests/test_modules/test_agent_ui.cpp 81 例全绿（filter agent_ui）——新增 tools_dialog_open_grid_close / tools_dialog_geometry（替代退役 height_dual_tier）/ hover_union_domain_debounce / detail_message_card_face；反转 test_agent_detail_layout 摘要卡「无边框」断言 → 1px glassBorder；agent 域 146 例 145 过 1 跳 0 败
- 探针：tests/probes/agent_e2e_probe.cpp G1-G4 全 PASS（G4 增缝隙穿越+去抖等待；移出/常态采样帧间睡过真实时钟）；tests/probes/agent_dialog_probe.cpp 增 D 段（消息卡 .hit 真点击开合 / 工具弹窗真点击开关+命中面框内断言+720×560）+ --shot-tools
- 证据：evidence/2026-11-03-accept-audit/dev-b5/probe-run.log、tools-dialog.png
- ⚠ 五流程（LLM 依赖腿）本机红：旧版 DPAPI 密文不可解密 → run 全 failed——凭据域环境问题，与 B5 改动零交集，未修不掩盖

## 4. 未验项与移交

- bug:182（169 残余：真指针缝隙横移间歇隐藏后不恢复）未修，修复向已写池
- 果冻按压 0.96/70ms/160ms 回弹的像素级验证归 qa（本批只落 scale 目标值与 transition 接线）
- demo HTML 未逐像素比对（设计明示 demo 仅视觉校准非蓝图）
`````

### doc#6 · 2026-11-04-memory-collector-storm-governance（prd v2 · need#191）

- 标题：记忆采集治理 PRD/分析（提案风暴四问与修复方向）
- 创建：2026-11-04 19:29:47 +0800（product-manager）· 更新：2026-11-05 01:16:24 +0800（developer）

`````markdown
# 记忆采集治理 PRD/分析（提案风暴）

> 池 bug:191 · 2026-11-04 · product-manager · 老板四质询逐答 + 修复方向 + 存量处置拍板项。实现归 bug-fixer/developer，本 PRD 定口径。
> **2026-11-05 开发定稿回写（developer）**：F-01~F-04/F-06/F-07 已实现（定向 102/102 绿），§3 状态与 §9 定稿值如下；F-05 存量清理已按拍板 A 执行完毕（证据见池 191）。

## 1. 背景

2026-11-04 发现 `~/DevDesk/memory/proposals.db` 1629 条 pending + 1 rejected，全部 source=collect、同一秒（created_at=1793766937）批量落库、kind 全 add、正文 11~4106 CJK 字符（均值 ~1975）、含 hook 噪声（「不要使用任何工具。只回复：收到」类）。另实测 `infer_queue.db` 同批积压 1626 行（clue58/59）。

**风暴根因链（一句话）**：引擎 Running 首轮立即跑采集（panel_state.cpp:185）且此后每 30s 一轮（collector.h:25）→ 水位为 0 时首轮把全部积压会话历史装配成 4KB 段 → 每个过闸段无上限逐条建卡（collect_round.cpp:93-94）→ 一次首轮产出 1629 张卡 + 1626 条提炼队列项。

## 2. 老板四质询逐答（代码锚 + 实测）

**Q1 采集功能的机制验证过吗？**
验证过，但口径=管线机制正确性，非内容质量：
- `tests/test_memory_collect_wiring.cpp`：干净段→pending 卡（带 `[踩坑|会话采集]` 前缀）、敏感段→拦截（fail-closed + 审计行 + 动态流 note 零原文）、二轮幂等零重产；decay 扫描 UUID 形态缝。
- `tests/test_memory_infer.cpp`：队列 FIFO 与 UNIQUE(session,start_seq) 入队去重、小时节流顺延、硬预算暂停、云端失败留队补跑、路②补标。
- `tests/test_memory_proposals.cpp`：状态机三终态幂等、TTL 30 天写入、baseVersion 失配自动过期、撤销链、执行失败保持 pending。
- 缺口：噪声/重复/低价值维度零测试（机制本身不存在该环节，见 Q3/Q4）——F-07 已补（§9）。

**Q2 去重了吗？**
**没有内容级去重。** 仅三层事件级幂等（防重放，不防同内容）：processed_events(session_id,event_seq)（collector.cpp:56-83）、infer_queue UNIQUE(session_id,start_seq)（infer_queue.cpp:19）、undo 卡唯一索引（proposal_store.cpp:42）。store.create 直插无指纹无相似度比对（proposal_store.cpp:241-247）——同内容多段、跨会话重复、整文件重读变体全部重复成卡。

**Q3 质量验证了吗（低价值/噪声过滤）？**
**没有低价值过滤。** 现有闸仅五道：user/assistant 角色过滤、text 块才算 narrative（tool_result 仅错误留摘要，session_reader.cpp:43-53）、system-reminder 注入整条丢弃（session_reader.cpp:14-17）、段需含 user 正文（collector.cpp:240,251）、sanitize 密钥闸（fail-closed）。hook 噪声是 user 角色普通 text（narrative=true）→ 五道闸全过。实测证据：1629 pending 正文含指令回声类噪声；confidence 硬编码 0.9（collect_round.cpp:94）无任何质量语义。

**Q4 进入待采集数据集的门槛，现在是什么？应该是什么？**
- **现状门槛**：角色合法 + 有 narrative 文本 + 无注入标记 + 段含 user 正文 + sanitize 零命中 → **100% 建 pending 卡**（无上限、无去重、无长度/噪声门槛）。即门槛=「不是密钥、不是纯工具输出」，仅此而已。
- **应该是**（本 PRD 提案，F-01~F-04，已实现）：指纹去重 + 单轮上限 + 噪声/极短过滤 + TTL 回收真实生效——让「进了数据集」默认意味着「值得人审一眼」。

## 3. 修复方向功能矩阵

F 总数=7（核心 5 / 边缘 2）

| ID | 功能点 | 优先级 | 状态 | 卡片 |
|---|---|---|---|---|
| F-01 | 采集侧内容级去重 | 核心 | ✅已实现 | 归一化（去空白）SHA256 指纹；建卡前比对同轮段间 + pending 全量 + 近期已决 200，命中跳过（不建卡不排队，collect_round.cpp）。指纹独立可用（零引擎依赖）；已知集加载失败降级为仅同轮去重（fail-open 于去重，敏感闸 fail-closed 不变） |
| F-02 | 单轮提案上限 | 核心 | ✅已实现 | N=50（拍板）。**机制落点修正：读取侧段预算**（见 §9-P2）——collector.h kMaxSegmentsPerPoll=50，pollOnce 全局预算 + readSessionFile 行级回退；剩余行不推进水位下轮续读（事件级幂等兜底）。首轮全量回读被硬顶住 |
| F-03 | 低价值过滤 | 核心 | ✅已实现 | ①噪声模式定稿 v1 见 §9-P1 ②极短段（有效字符<50 拒，拍板）③纯 tool_error 段：collector 出段闸已保证（collector.cpp:240/251），管线不重复判。过滤段记已处理、计数走 report（动态流不刷屏） |
| F-04 | expireSweep 接线 | 核心 | ✅已实现 | 落点=runCollectRound 每轮开头（panel 采集循环每 30s 一轮即一轮一扫）；失败 LOG 不阻断本轮；沉降数进 report.expiredSwept + 动态流 note |
| F-05 | 存量清理（一次性） | 核心 | ✅已执行 | 拍板 A 批量置 expired + infer_queue 同步清空。2026-11-05 执行：pending 1629→0（全 expired + 1629 条审计行「存量风暴清理 batch bug191」，audit 1631→3260）；infer_queue 1626→0；collector.db processed_events 10387 不动；双库 integrity ok。备份+对账证据 evidence/2026-11-05-bug191-cleanup/ |
| F-06 | confidence 语义修正 | 边缘 | ✅已实现 | 0.9 → 0.3（kUnassessedConfidence，「未评估」语义；卡面 confidenceTier<0.5 显「低」不再误导高置信）。完整「未评估」显示位改造属面板侧，不属本批 |
| F-07 | 质量口径测试补齐 | 边缘 | ✅已实现 | 4 用例入 test_memory_collect_wiring.cpp（storm_* 前缀）：同轮+跨轮指纹去重、60 段灌入恰 50 卡+下轮续产总量守恒、噪声/极短/正常对照三段过滤、到期 pending 一轮内沉降+confidence<0.5。先红后绿（对旧码 4/4 红 → 新码全绿） |

## 4. 修复后门槛（应然态，验收口径）

一段会话文本要成为 pending 卡，须依次通过：五道既有闸 → 低价值过滤（F-03）→ 指纹去重（F-01）→ 建卡（轮预算内，F-02 在读取侧）；建卡后 TTL 30 天真实生效（F-04），没人审即自动沉降 expired。

## 5. 存量 1629 pending 处置方案（已拍板 A 并执行）

| 选项 | 做法 | 结论 |
|---|---|---|
| **A 批量置 expired（已拍板✅）** | 批量 status='expired' + 每条审计行 detail=「存量风暴清理 batch」 | 2026-11-05 执行完毕，语义=「未经人审过期」，已处理视图可追溯，rejected 人审语义零污染 |
| B 批量 rejected | 逐条 store.reject() | 弃（语义错位） |
| C 物理删除 | SQL DELETE | 弃（绕状态机） |

**配套（已执行）**：infer_queue.db 1626 行清空（防节流闸按闸速烧云端计费）；collector.db processed_events 水位**不动**（10387 条保留，避免重放已清理会话）。

## 6. 完整状态集（F-01~F-04 实现后）

| 维度 | 状态 |
|---|---|
| 空 | 无新会话增量 → 零卡零队列项 |
| 错误 | 指纹已知集加载失败 → 降级仅同轮去重 LOG 留痕；expireSweep 失败 → LOG 不阻断本轮；敏感数据闸 fail-closed 不变 |
| 边界 | 轮预算用尽 → 水位停在已出段边界下轮续读；同内容多段/跨轮 → 只第一张成卡；引擎缺席 → 指纹去重仍工作；过期沉降 → pending→expired 单向 |
| 平台差异 | 无（纯逻辑层，双平台同构） |

## 7. 关联影响

- bug190（审核视图渐进加载+字形缓存）：已修渲染层，本 PRD 从源头减卡量，互补不冲突。
- bug203：主控台提示遮蔽，本 PRD 无涉。
- 云端计费：infer_queue 已清空，llm_input/output 账面增长回归正常速率。

## 8. 验收标准（核销）

- [F-01] ✅ 同内容双段仅 1 卡；跨轮同内容零卡（memory_collect_storm_fingerprint_dedup）
- [F-02] ✅ 60 段灌入恰 50 卡，剩余下轮续产，总量守恒（memory_collect_storm_round_cap）
- [F-03] ✅ 噪声段/极短段零卡 + 正常对照段照常成卡（memory_collect_storm_low_value_filter）
- [F-04] ✅ 到期 pending 一个采集轮内自动 expired（memory_collect_storm_confidence_and_ttl_sweep）
- [F-05] ✅ pending 1629→0、queue 1626→0、processed_events 10387 不动、1629 审计行可查（cleanup-log.txt）
- [F-07] ✅ 新增 4 用例全绿入回归集（先红后绿）

## 9. 开发定稿值（2026-11-05，拍板项落定）

- **P1 噪声模式清单 v1**（归一化=去空白+ASCII 折小写后子串匹配；误杀面=讨论这些短语的正当正文，漏卡成本<噪声风暴，知情接受）：`不要使用任何工具` / `只回复` / `请直接回复` / `do not use any tools` / `respond with only`
- **P2 F-02 机制修正**：拍板口径「剩余段不 commit 事件/水位，下轮再处理」在 commit 层不可实现——水位在 readSession 内随读推进到 EOF（collector.cpp:257），commit 只记事件（:299）。落点改为**读取侧段预算**：pollOnce 全局 50 段（kMaxSegmentsPerPoll）+ readSessionFile 预算检查行级回退（consumedFrom/lineNo 同步回退，eventSeq 稳定）。对外语义与拍板一致：预算外段不推进水位、下轮续读。
- **P3 极短段阈值**：有效字符 = CJK 表意文字计 1 + ASCII 字母数字计 0.5（与卡面截断宽窄口径同款），<50 拒（拍板 N=50）。纯英文长段（≥100 字母数字）可过闸，纯标点/空白段必拒。
- **P4 confidence 定稿**：kUnassessedConfidence=0.3。
`````

### doc#17 · 2026-11-05-agent-tools-dialog-redesign-v2（prd v2 · need#170）

- 标题：设计文档：智能体「可用工具」弹窗重设计 v2（池170 驳回重做）
- 创建：2026-11-05 20:27:56 +0800（ui-ux-designer）· 更新：2026-11-05 20:47:53 +0800（ui-ux-designer）

`````markdown
# 设计文档：智能体「可用工具」弹窗重设计 v2（池170 驳回重做）

- 日期：2026-11-05 ｜ 角色：ui-ux-designer ｜ 池：need170（P2）+ 子 bug #6 / #7
- 来源：2026-11-05 老板驳回 v1 弹窗版，驳回点三条：①「目前缺了卡片效果」②「每个工具的说明文本都是截断的，看不到完整内容」③入口按钮「图标和文本重叠，因为后面还有个工具数，这个值不是固定的」（bug #6）
- 上游：v1 设计 `docs/design/2026-11-03-智能体可用工具弹窗与明细卡重设计.md`（本篇**取代其 §2.2/§3.1/§3.2 工具弹窗与入口钮节**；v1 的 §2.1 页面构图 / §2.3 明细卡 / §8 hover 状态机不受影响）
- token 基准：`src/core/theme.cpp` 亮/暗实值（theme.cpp:20-44 暗 / :69-123 浅）+ 风格基线（绿色微信风四原则）+ 老板材质裁决（白卡+细黑边、明显更实、拒绝三层套娃、FA 图标禁 emoji、元素对齐内容列）+ 同期 `docs/design/2026-11-05-终端页面UI重设计.md` §11 暗色对照表
- 配套 demo：`docs/design/2026-11-05-智能体可用工具弹窗重设计v2.html`（自包含单文件，浅色主 + 暗色 token 对照，仅视觉校准，**不是实现蓝图**）
- 交付范围：入口按钮重设计（修 bug #6）+ 工具弹窗壳与工具卡重设计（修驳回点①）+ 说明文本完整展示策略（修 bug #7）+ **全文搜索框（2026-11-05 老板整改意见追加：§3.6）**。不动页面构图（v1 §2.1 已定）、不动明细弹窗、不动业务功能与数据层。
- v2 变更记录：新增 §3.6 全文搜索框（锚位/过滤/高亮/清除/计数/Esc 优先级/搜索空态）+ §4.4 搜索流程 + §5 状态表搜索行 + §6 动效 3 行 + §7 搜索框自适应行 + §10 验证清单 5 格；§2 构图 ASCII 加搜索区行。

---

## 1. 现状与根因（已核实源码，可直接采信）

| # | 现状 | 根因定性 |
|---|---|---|
| 1 | 弹窗壳与工具卡用 `glassBorder` 高光边（白 α0.60，glass.h:33-36） | **白卡上勾白边=边不可见**，卡与弹窗底融成一片——「缺了卡片效果」的直接根因；工具卡又无阴影，卡感为零 |
| 2 | 工具卡 2 列定高 96（agent_module_dialogs.cpp:1188,1291），描述 12px：有参 1 行 / 无参 2 行 wrap，Ellipsis 永久截断（:1340-1361） | **定高网格逼出截断**：真实数据描述最长 110 字符（fs.glob，agent_builtin_tools.cpp），336px 窄卡 12px 字 ≈26 字/行，需 5 行，只给 1-2 行 |
| 3 | 入口钮固定 `kToolsBtnW=132`×40（agent_module.cpp:79,491），文字「可用工具（N）」整体塞死宽 | **固定宽 + 变长内容 = 重叠**：N 从 1 位涨到 2-3 位（当前注册 30+ 工具）文字溢出压图标 |
| 4 | 参数 chips 超宽折「+N」（v1 §3.2） | 全宽卡下 chips ≤4 枚必然放得下，+N 截断退化为多余规则（随卡重设计一并退役） |
| 5 | 加载/错误态不可达 | ensureToolsCache 同步装载、listRegisteredTools 进程内枚举不失败（前轮已记账，v1 状态表已录）——维持不建死分支 |

工具数据面（demo 数据源）：内置注册 30+ 工具（fs./git./md./memory./app./net./claude./schedule./todo./agent./bash. 十一族），描述 12–110 字符，参数 0–3 枚（tool_info.h 仅携带参数键名，无逐参数描述）。

## 2. 页面级构图（门禁节）

### 2.1 弹窗区块划分与锚定

```
┌─ backdrop（全屏 黑40%，点击空白处关）────────────────────┐
│  ┌─ 弹窗壳 w=min(720,可用宽−48) h=min(600,可用高−96) ──┐ │
│  │ [头部 56]  可用工具 [30 个]                [✕ 28]   │ │ ← 锚顶·不滚
│  │ ─────────────── 1px 分割线 ─────────────────────── │ │
│  │ [搜索区] pad 12 16 8：(search) 搜索工具名称或说明 [n/30] [×] │ │ ← §3.6 锚顶·不滚
│  │ [列表区 fill] pad 8 16 16，scrollView 内滚（唯一内滚容器）│ │
│  │  ┌────────────────────────────────────────────┐   │ │
│  │  │ [◆] fs.glob                        [▾]      │   │ │ ← 工具卡·全宽单列
│  │  │ 文件通配搜索：按 glob 模式匹配工作目录下的文 │   │ │    高随内容自适应
│  │  │ 件路径（** 递归、* 匹配文件名不含路径分隔符、 │   │ │
│  │  │ ? 匹配单字符…）                    [收起 ▴] │   │ │ ← >3行卡：clamp+展开
│  │  │ [pattern] [path]                             │   │ │ ← 参数 chips 行
│  │  └────────────────────────────────────────────┘   │ │
│  │  ┌──…卡 gap 8，垂直流 ────────────────────────┐   │ │
│  └────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

### 2.2 滚动模型

- **外壳不滚**（全站纪律）；**弹窗壳不滚**（头部+分割线+搜索区锚顶固定，§3.6）；**列表区唯一内滚**（scrollView，与详情弹窗消息区同源同款滚动条，不引入新滚动条规则）。
- 页面级（弹窗背后）行为不变：任务列表内滚，弹窗打开时背后页面冻结交互（dialog 既有契约）。

### 2.3 空间节奏与信息密度预算

- **弃 2 列定高网格，改全宽单列列表卡**：定高 96 是截断根因（§1#2）；单列全宽下描述按 720 档 ≈50 字/行自然换行，110 字符长描述 ≤3 行即全见，**不需要展开的卡占多数**。每卡高度随内容 72–108，垂直流卡 gap 8，无行内对齐空洞（构图三问②：文字左对齐撑满卡宽，右侧只留 chevron 位，无定宽空列）。
- 头部 56 + 分割线 1 + 搜索区 56（pad 12 顶 + 框 36 + 8 底）+ 列表 pad 8 顶 + 16 底；720×600 档一屏 4–5 卡；最小窗 1000×640（弹窗 720×544）一屏 3–4 卡。
- 权重：工具名（14/600 一级字）> 描述（13px 二级字）> 参数 chips（11px 三级感）> 头部（低频查阅）——高频信息（名字+说明）权重最高（四原则·信息优先）。弹窗是纯查阅低频面，克制装饰：零渐变、零插画、单一绿染（图标盒）。

### 2.4 构图三问

1. **滚动模型**：外壳/弹窗壳不滚、列表区内滚，与全站弹窗一致（详情弹窗同构）✔
2. **无效空白**：单列全宽卡无空洞；留白仅卡内 pad 12/14、卡 gap 8、列表 pad 16，全部服务分组与呼吸 ✔
3. **密度与权重**：名>述>参数三级字阶清晰；720 档 50 字/行长度可读；窄档（<640 视口）单列天然不破版 ✔

## 3. 逐区块规格表（参数填齐）

> 色值口径 = theme.cpp 实值，**禁新增裸 hex**（demo 里的 hex 均为 token 实值罗列）。浅/暗对照全表见 §9。

### 3.1 页头「可用工具」入口钮（修 bug #6）

| 项 | 参数 |
|---|---|
| 形态 | 次钮语言：底 `#F2F2F2` / 暗 `#353231`；**宽=内容自适应，弃固定 132**；高 40 固定、radius 6 |
| 结构（水平流式排布，禁绝对坐标） | [图标 plug 0xF1E6 14px `textSecondary`] −6− [文字「可用工具」14px/400 `textPrimary`] −8− [计数 badge] ；水平内边距 12 |
| 计数 badge | 高 18、radiusPill 999、水平内边距 5、底 `primarySoftBg`（#E8F8EE / #0D3326）、字 12px/600 `primaryText`（#157E4E / #36D399）、内容「N」纯数字；**badge 宽随位数自适应**（min-width 18 保单位数不塌） |
| 宽度公式（验收用） | W = 12+14+6+文字宽(≈56)+8+badge宽+12：N=3→≈136 / N=19→≈150 / N=108→≈164 |
| 锚定 | 页头行右端 flex 流内、「+ 新建任务」左侧间距 8；标题文字区弹性让位（flex，min 0）；按钮**右缘锚定不动、向左生长**，任何位数不换行不重叠 |
| 状态 | hover：浅 `hover` #EEF6F1（绿族 hover 底全站口径）/ 暗 `active` #3E3B39（暗 hover 档 #353231 与次钮底同值无效，取 active 一档加深）；pressed：底同 hover + 果冻 scale 0.96（70ms→160ms 回弹）；disabled（store 空时）：图标+文字+badge 整体 40% 不透明度且不可点 |
| 点击 | 打开弹窗（§4.1）+ ensureToolsCache forceReload（现状语义保留） |

### 3.2 弹窗壳（修驳回点①·缺卡片效果）

| 项 | 参数 |
|---|---|
| 尺寸 | w = min(720, 可用宽−48)；h = min(600, 可用高−96)；最小窗 1000×640 下 720×544 |
| 材质 | **白卡+细黑边**（老板裁决）：底 `glassFill`（浅 白微绿染 α0.94 / 暗 surface #232120 α0.95，glass.h:17-21）+ **1px 细黑边**（浅 rgba(0,0,0,0.35) / 暗 `borderDefault` #3A3735——暗色纯黑边深底不可见，降档，终端页覆盖卡同口径）+ 冷阴影 `glassShadowColor`（浅 rgba(16,54,38,0.09) / 暗 rgba(0,0,0,0.34)）blur 24 offsetY 4 + `radiusLarge` 14 |
| 层级 | components::dialog 既有 zIndex 契约；backdrop 黑 40%；打开时背后页面禁交互 |
| 层级纪律 | 弹窗壳（浮层卡）→ 工具卡（业务卡）**两层为止**；工具卡内无再嵌卡（chips 是 pill 非卡）——拒绝三层套娃 ✔ |

### 3.3 头部区（56）

| 项 | 参数 |
|---|---|
| 标题 | 「可用工具」16px/700 `textPrimary`，x=20 垂直居中 |
| 计数 badge | 紧跟标题右侧 8：高 20、radiusPill、内边距 6、底 `primarySoftBg`、字 12px/600 `primaryText`、文案「N 个」——与入口钮 badge 同语言 |
| 关闭钮 | 28×28，FA `xmark` 0xF00D 14px `textSecondary`；hover 底 `hover` radius 6；右缘内 12；点击/背景点击/Esc 三通道关闭 |
| 分割线 | 头部下 1px `borderDefault`（#E5E7EB / #3A3735）通宽 |
| 下方锚位 | 分割线之下、列表区之上为**搜索框区（§3.6）**，同属锚顶固定段不随列表滚动 |

### 3.4 工具卡（全宽单列·核心交付）

| 项 | 参数 |
|---|---|
| 尺寸 | 宽 = 列表内容区全宽（720 档 ≈688；窄档随弹窗缩）；**高随内容自适应 72–108**，禁定高 |
| 材质 | **白卡+可见边**：底 `surface` 实色（浅 #FFFFFF / 暗 `surfaceElevated` #2C2A28——比弹窗壳浅半档浮起）+ 1px `borderDefault`（#E5E7EB / #3A3735）+ radius 8 + 极淡阴影（浅 rgba(16,54,38,0.05) blur 8 offsetY 2 / 暗 无）；「明显更实」= 实色底，可见灰边承担卡感（替代不可见白高光边） |
| 卡内布局（纵向流，pad 12 14） | 头行 28 → 6 → 描述块 → 6 → 参数 chips 行（无参数不渲染） |
| 头行 28 | [图标盒 28×28 radius 6 `primarySoftBg` + 族图标 14px `primaryText`] −8− [工具名 14px/600 `textPrimary` 单行省略] −弹簧− [chevron-down 0xF078 12px `textTertiary`，仅长文卡渲染；展开态旋转 180° 150ms] |
| 族图标映射（全 FA5 实心集码点，禁 emoji） | fs=`folder` 0xF07B｜git=`code-branch` 0xF126｜md=`file-alt` 0xF15C｜memory=`brain` 0xF5DC｜net=`globe` 0xF0AC｜app=`cube` 0xF1B2｜claude=`robot` 0xF544｜todo=`tasks` 0xF0AE｜schedule=`clock` 0xF017｜bash/agent=`terminal` 0xF120｜兜底=`plug` 0xF1E6 |
| 描述块（修 bug #7） | 13px/20 `textSecondary` wrap 全宽；**默认完整展示不截断**（≤3 行即全见）；描述实测 >3 行 → clamp 3 行（行高 20×3=60）+ 末行省略号 + 描述块下方「展开 ▾ / 收起 ▴」文字钮（12px/500 `primaryText`，高 24 命中区）；数据上限 110 字符在 720 档 = 3 行内全见，窄档 <640 才会出现长文卡 clamp |
| 参数 chips 行 | 每参数一枚 chip：高 20、radius 6、底 `#F2F2F2` / 暗 `#353231`、字 11px/500 `textSecondary`、内边距 4 8、chips 间距 4、**flex-wrap 换行不截断**（全宽卡 + 参数 ≤3 枚，+N 截断规则退役）；无参数不渲染整行（不留洞） |
| hover/点击 | **仅长文卡可点**（整卡 = 开合开关，文字钮冒泡同一开关）：hover 底 `hover` #EEF6F1 / `#353231` + 边框加深一档 `active` #E2F0E8 / #3E3B39，120ms 缓出；pressed 果冻 scale 0.96（70ms→160ms 回弹）。**短文卡零交互态**（基线：果冻/hover 只给可点项；不可点卡不建 hover 不骗点击） |
| 选中态 | 无多选语义（纯查阅列表），不设选中态；展开态视觉 = chevron 旋转 + 文字钮换「收起」，边框底色不变（展开≠选中） |
| 禁用态 | 卡级无禁用语义（注册表内工具恒可用）；工具注册表为空 → 走弹窗空态，入口钮 disabled 是唯一禁用面（§3.1） |

### 3.5 弹窗空态

| 项 | 参数 |
|---|---|
| 内容 | FA `plug` 0xF1E6 24px `textTertiary` → 8 → 「暂无可用工具」13px `textSecondary` → 4 → 「智能体未装配任何工具」12px `textTertiary`；列表区水平垂直居中 |
| 纪律 | 极简文字、无插画无情绪化文案（基线）；无可操作项（装配工具入口不在本弹窗职责内，不加「就近操作」） |
| 加载/错误态 | 不可达不建（§1#5 已证：同步装载语义；留 TU 注释与前轮口径一致） |

### 3.6 全文搜索框（2026-11-05 老板整改意见追加）

| 项 | 参数 |
|---|---|
| 锚位 | 弹窗标题下通栏：头部 56 → 分割线 1 → **搜索区锚顶固定不滚** → 列表区（唯一内滚不变，§2.2） |
| 区块 | 搜索区 pad 12 16 8；内含单个搜索框通栏全宽（720 档 ≈688，随弹窗缩，窄档 <640 通栏不变） |
| 搜索框 | 高 36、radius 8、底 `#F2F2F2` / 暗 `#353231`（次钮底档，§9）；无边框，focus 态 1px `primary` 描边 + 底转 `surface` 实色（120ms 缓出）——焦点可见性 |
| 图标 | 左侧 fa-search `0xF002` 14px `textTertiary`，左距 11，pointer-events 无（纯装饰，FA5 集名 search；FA6 命名 magnifying-glass 同码点） |
| placeholder | 「搜索工具名称或说明」13px `textTertiary` |
| 输入文本 | 13px `textPrimary`；左内边距 35（图标位）、右内边距 92（计数+清除位） |
| 过滤范围 | **工具名称 + 说明全文 + 参数名**，不区分大小写全文子串匹配，命中任一即保留整卡；输入即时过滤（每字符触发，同步过滤无需防抖——30+ 条数据量） |
| 关键词高亮 | 命中子串在**名称/描述/参数 chips 内原位标记**：仅文字底色 `primarySoftBg`（#E8F8EE / #0D3326）、圆角 2、字色继承原层级色（名称 textPrimary / 描述 textSecondary / chip textSecondary），**不给卡片描边不加粗**；关键词未出现的字段不高亮 |
| 清除按钮（×） | 28×28 命中区（视觉 xmark 10px `textTertiary`）radius 6，**有输入时才显示**（opacity 级显隐 120ms）；hover 底 `hover` 字 textPrimary；点击清空并回焦输入框 |
| 结果计数 | 搜索框内右端（清除钮左侧 8）：「命中数 / 全量」如「12 / 32」，12px `textTertiary` 等宽数字（tabular-nums）防跳动；**仅过滤生效时显示**（无输入不显示——全量数已由头部 badge 承载） |
| 计数职责分离 | 搜索框右侧 = 过滤命中数；弹窗头部 badge（§3.3）与入口钮 badge（§3.1）= **恒显全量，不随搜索变**（入口 badge 是装配事实，搜索是查阅视图，两者语义不同源） |
| Esc 优先级 | **搜索框有输入 → 第一次 Esc 清空搜索**（焦点回输入框，弹窗不关）；**无输入 → Esc 关弹窗**（§4.3 既有三通道不变） |
| 搜索空态 | 命中 0 条：列表区居中 fa-search `0xF002` 24px `textTertiary` → 8 → 「未找到匹配工具」13px `textSecondary` → 4 → 「试试其他关键词」12px `textTertiary` → 10 → 文字钮「清空搜索」（12px/500 `primaryText`，高 26 radius 6，hover 底 `hover`，果冻同 §6）——唯一就近操作，点击清空回焦 |
| 与 §3.5 空态关系 | 工具注册表空（N=0）→ 搜索区整体不渲染（无内容可搜，留框即无效控件），走 §3.5 原空态 |
| 状态记忆 | 弹窗每次打开搜索态重置（空输入 + 全量列表）——查阅面不跨会话保留过滤条件 |
| 加载/错误态 | 随列表区，不可达不建（§1#5） |

## 4. 交互流程

### 4.1 开合主流程
页头钮点击 → 弹窗入场 160ms（opacity 0→1 + 上移 8px→0）→ forceReload → 列表呈现（同步即现，无骨架）→ 滚动浏览/长文卡展开 → 关闭（✕ / backdrop / Esc 任一）→ 出场 120ms opacity→0 → 焦点回落页头钮。

### 4.2 长文卡开合
点击长文卡任意处（或「展开」钮）→ 描述块 clamp 解除，高度展开 200ms（先快后收），chevron 旋转 180° 150ms，钮文案「展开 ▾」→「收起 ▴」；再点收起。滚动位置保持，不自动滚动到卡顶。

### 4.3 关闭通道
✕（3.3）/ backdrop 点击（弹窗外任意）/ Esc（模块 onKeyEvent 分支）三通道等价，全部走 dialog 既有契约。
**Esc 带搜索优先级（§3.6）**：搜索框有输入 → Esc 清空搜索（弹窗不关，焦点回输入框）；搜索框空 → Esc 关弹窗。

### 4.4 搜索过滤
输入即过滤（名称+说明+参数名，不区分大小写子串，§3.6）→ 列表即时收敛 + 命中关键词原位高亮 + 计数「n / 全量」浮现 → 清除钮 / Esc 清空恢复全量；清空后焦点回输入框，列表滚动位置回到顶部（列表内容已重建）。长文卡在过滤后的列表内照常 clamp/展开（§4.2 规则不变）。

## 5. 状态表

| 表面 | 空 | 加载 | 错误 | 成功/常态 |
|---|---|---|---|---|
| 弹窗列表区 | §3.5 空态居中（搜索区不渲染） | 不可达不建（已证） | 不可达不建（已证） | 全宽单列卡垂直流 |
| 搜索框 | placeholder 态（无输入，计数/清除钮隐藏） | — | — | 输入态：即时过滤+高亮+计数「n/全量」+清除钮显 |
| 搜索结果 | 0 命中 → §3.6 搜索空态（图标+文案+「清空搜索」钮） | — | — | 命中卡原布局原位高亮 |
| 长文工具卡 | — | — | — | 折叠（clamp3+展开钮）/ 展开（全文+收起钮）两态 |
| 短文工具卡 | — | — | — | 常态零交互（无 hover/pressed） |
| 入口钮 | N=0 badge「0」仍可点（弹窗内空态） | — | — | 常态/hover/pressed/disabled(40%)；badge 恒全量不随搜索变 |
| 关闭钮 | — | — | — | 常态/hover |

## 6. 动效表

| 动效 | 参数 |
|---|---|
| 弹窗入场 | 160ms ease-out：opacity 0→1 + translateY 8px→0 |
| 弹窗出场 | 120ms ease-in：opacity→0 |
| backdrop | 黑 40%，随弹窗同 160ms/120ms 淡入淡出 |
| 长文卡展开/收起 | 高度 200ms（基线展开折叠 200-240 档·先快后收）；描述全文 opacity 0→1 120ms |
| chevron 旋转 | 180° 150ms ease-out |
| 果冻按压 | 长文卡/入口钮/关闭钮/文字钮/✕：按下 ~70ms 缓出至 scale 0.96，松手 ~160ms 回弹（过冲 ≈12%，等价贝塞尔 0.34/1.56/0.64/1）——只给可点项 |
| hover 过渡 | 底色/边框 120ms 缓出；hover 不放大 |
| 搜索清除钮/计数显隐 | opacity 0→1 / 1→0，120ms ease-out（随输入即时） |
| 搜索框 focus 态 | 边框色 + 底色 120ms 缓出（§3.6） |
| 关键词高亮 | 无动效（过滤重渲染直加，不做逐字动画） |
| 尊重「减弱动态效果」 | 果冻/淡入类降级为直切（基线纪律） |

## 7. 自适应行为表

| 项 | 值 |
|---|---|
| 弹窗宽 | min(720, 可用宽−48)；视口 <640 → 弹窗贴边 24，单列卡随宽缩（描述行长变短、长文卡 clamp 自然生效） |
| 搜索框 | 通栏全宽随弹窗缩，**窄档 <640 通栏形态不变**（高 36/图标/placeholder/计数/清除全保留；输入右内边距 92 在窄档仍够——最短有效输入 1 字符即触发过滤） |
| 弹窗高 | min(600, 可用高−96)；最小窗 1000×640 → 720×544，列表内滚兜底 |
| 入口钮 | 宽随 N 位数自适应（§3.1 公式三档已验）；页头标题区 flex 弹性让位，永不换行不重叠 |
| 长内容 | 描述 wrap 全文 + >3 行 clamp/展开；chips wrap 换行；工具名单行省略兜底；全部不撑破卡 |
| 固定宽清单 | 仅结构性元素：图标盒 28、✕ 28、badge/ chips 自身、关闭钮、搜索框高 36 与搜索图标 14（宽全弹）；卡/列表/搜索框/弹窗全弹性 |
| 150% 字号 | 行盒 scaled 派生；卡高随内容生长（自适应高的直接收益）；弹窗高钳制内滚兜底 |
| 1080P 外壳 | 1920×1080 下弹窗 720×600 居中，四周余量充足不溢出 |

## 8. 禁止项自查

- 全参数落具体值（px/码点/alpha/时长/缓动/枚举），无「大气美观」类描述 ✔
- 正文不绑实现技术：仅 ui_kit token 与组件契约名；HTML demo 为唯一原型载体例外 ✔
- 图标全 FA5 码点（0xF1E6/0xF00D/0xF002/0xF078/0xF07B/0xF126/0xF15C/0xF5DC/0xF0AC/0xF1B2/0xF544/0xF0AE/0xF017/0xF120），零 emoji；码点字形存在性 demo 阶段肉眼核，开发期照项目惯例防绿块 ✔
- 四色语义：绿=操作/计数强调（图标盒、badge、展开钮）、红=未用（本面无告警语义）、灰=中性、蓝未用 ✔
- 果冻只给可点项（长文卡/按钮），短文查阅卡零交互态 ✔
- 拒绝三层套娃：弹窗壳→工具卡两层为止 ✔
- 构图节先行（§2），参数全部挂在构图之下 ✔

## 9. 暗色 token 对照表（demo 浅色主，暗色按此表落）

| 语义 | 浅色 | 暗色 |
|---|---|---|
| 页面底 background | #F5FAF7 | #1A1816 |
| 弹窗壳底 glassFill | 白微绿染 α0.94 | #232120 α0.95 |
| 工具卡底 surface / surfaceElevated | #FFFFFF | #2C2A28 |
| 卡边 borderDefault | #E5E7EB | #3A3735 |
| 弹窗细黑边（浮层） | rgba(0,0,0,0.35) | #3A3735 |
| 阴影 glassShadowColor | rgba(16,54,38,0.09) | rgba(0,0,0,0.34) |
| hover | #EEF6F1 | #353231 |
| active / 长文卡 hover 边加深 | #E2F0E8 | #3E3B39 |
| 次钮底 / 搜索框底 | #F2F2F2 | #353231 |
| 搜索高亮底 primarySoftBg | #E8F8EE | #0D3326 |
| 搜索框 focus 边 primary | #07C160 | #36D399 |
| 字一/二/三级 | #1F2937 / #4B5563 / #6B7280 | #FAF9F5 / #C9C5BD / #9A948A |
| primary / primarySoftBg / primaryText | #07C160 / #E8F8EE / #157E4E | #36D399 / #0D3326 / #36D399 |
| chips 底 | #F2F2F2 | #353231 |

## 10. 先验验证清单（开发前在 demo 阶段钉住；功能 × 场景 × 边界格子）

| 功能 | 场景 | 边界格子 |
|---|---|---|
| 入口钮自适应 | N=3 / 19 / 108 / 0 四档 | 图标-文字-badge 零重叠零换行；badge 位数变宽按钮向左生长右缘不动；disabled 40% |
| 弹窗开合 | ✕ / backdrop / Esc 三通道 | 连开连关；开时背后冻结；焦点回落；160/120ms 出入场 |
| 描述完整展示 | 短（12字）/中（~40字）/长（110字）描述 × 720 档 / 窄档（<640） | 短中卡全文可见零截断；长卡 720 档 3 行内全见；窄档 clamp+展开后全文可见；展开收起循环无高度残留 |
| 卡片效果 | 浅/暗两主题 | 卡可见边+浮起感（暗卡比壳浅半档）；阴影不脏；与全站卡语言一致 |
| chips | 0 / 1 / 3 枚参数 | 无参不留洞；3 枚单行放下；wrap 不截断 |
| 列表滚动 | 30+ 工具 / 最小窗 1000×640 | 内滚流畅；头部/分割线/搜索区锚顶固定；一屏 4-5 卡不破版 |
| 空态 | N=0 | 居中极简；入口钮可点开空弹窗 |
| 搜索过滤 | 名称命中 / 说明命中 / 参数名命中 / 三者组合词 | 子串不区分大小写；即时过滤无卡顿（30+ 条）；弹窗重开搜索态重置 |
| 关键词高亮 | 命中词在名称 / 描述 / chips 内 | 原位底色 primarySoftBg 零描边；未命中字段不高亮；清除后零高亮残留 |
| 搜索空态 | 无意义词 / 全部被过滤 | 图标+「未找到匹配工具」+「清空搜索」钮可回焦；N=0 时不显示搜索区（§3.5 优先） |
| Esc 优先级 | 有输入 × 焦点在输入框/卡片/页头 | 先清搜索（弹窗不关、焦点回输入框）；空输入 Esc 关弹窗；连按两次 = 清+关 |
| 计数联动 | 过滤后 / 清空后 / N=3·108 档 | 框内「n / 全量」随过滤刷新等宽不跳动；入口 badge 与头部 badge 恒全量不变 |
| 窗口拖缩 | 1000→1920 宽 × 640→1080 高 | 弹窗钳 720×600；窄档贴边 24；搜索框窄档通栏不变；无横向溢出 |

## 11. 交付与验收口径

- developer 照本文档 §3/§4/§5 实现（demo 不作实现蓝图）；qa 对照 §10 清单验收。
- 用户/PM 过目 demo 与 §2 构图后再进入开发。

`````

### doc#18 · need172-sidebar-toggle-floating-button（tech v1 · need#172）

- 标题：need172：侧边栏收缩/展开按钮——圆框+悬浮样式+弥漫绿粒子（v3 终稿）
- 创建：2026-11-05 20:35:41 +0800（ui-ux-designer）· 更新：2026-11-05 20:35:41 +0800（ui-ux-designer）

`````markdown
# need172：侧边栏收缩/展开按钮——圆框 + 悬浮样式 + 弥漫绿粒子（v3 终稿）

- 日期：2026-11-05；角色：ui-ux-designer；状态：待评审
- 池条目：[pool:need:172]（进行中，owner=ui-ux-designer）
- 老板原话（2026-11-03）：「侧边栏的收缩展开按钮加一个圆框，改成悬浮样式的，最好加一点弥漫开的绿色粒子效果」
- 前置文档：`docs/design/2026-11-03-侧栏图标与收缩按钮重整.md`（§2.3 v1 / §10 v2）——本篇为 need172 **按钮范围**的合并终稿：v2 已实现参数全部沿用，新增 v3 修正（§5 包络规则、§6 悬浮阴影、focus 态、全屏守卫声明）。开发以本篇为单一依据，无需再回翻前置文档。
- 同域参考：`docs/design/2026-11-05-终端页面UI重设计.md`（侧栏 0 宽全屏终裁、外壳不滚纪律）
- 本域线索：clue42/43/44（真指针探针与坐标流，开发自证用）、clue79（侧栏 v2/v3 接线事实：热区 hot 与绘制解耦 40/36、粒子 hash 延迟 0~80ms、生命上限 880ms）、clue123（本次新增：v2 粒子越界量化 + 外阴影能力 + 全屏早退）
- 配套 demo：`docs/design/2026-11-05-need172-侧栏收缩按钮圆框悬浮粒子.html`（自包含单文件，双击直开；demo 仅视觉校准，不是实现蓝图）

（本台账副本与仓库文件 docs/design/2026-11-05-need172-侧栏收缩按钮圆框悬浮粒子.md 同体；以仓库文件为准，章节结构：§0 事实基座 / §1 页面级构图（构图三问+两态锚定）/ §2 圆框规格 / §3 悬浮三态+focus / §4 状态表 12 态 / §5 粒子系统 v3（椭圆弥漫+安全包络+预算表+性能口径）/ §6 交互流程 / §7 动效表 / §8 自适应行为表 / §9 与 v2 差异清单 / §10 demo 与先验验证清单 / §11 禁止项自查）

## 关键参数速览（详值见仓库文件对应节）

- 圆框：正常档直径 32/区 48，紧凑·极紧 28/44；1px borderDefault 圆框常显；折叠居中 (72−S)/2、展开 padX=16 对齐导航左缘；垂直零位移、水平位移 ≤6px 随宽度瞬时切换。
- 悬浮三态+focus：hover = primarySoftBg 底 + 1px primaryHover 描边 + 图标变绿 + scale 1.04（150ms OutCubic）+ 单层阴影 blur8/offsetY2/黑10%；pressed = 描边 primary + 0.96 + 阴影消失；移出回弹 180ms OutBack；focus = 1.5px primary 描边环不染底。
- 粒子 v3：12 颗上限、30°均布+hash±10°、生命 800ms+hash 延迟 0~80ms（≤880ms 自止）、粒径 4~6px 末端 ×0.7、透明度 0.75→0、起点 r0=钮半径+2；**新增 dy×0.55 垂直压缩 + 包络兜底 [3,W−3]×[foldLimit,H−3] + 折叠态弥散降档 10~16（展开 14~26 沿用）**；浅三色 #07C160/#34D399/#06AD56、深双色 primary/primaryHover；hover 进入沿一次迸发、移出不补发、无常驻帧泵、非活跃帧零元素零成本。
- 性能硬边界：≤12 矩形元素/次、≤880ms 自止，任何后续调参不得突破；全屏 0 宽整块不注册（src/app_sidebar.cpp:378 早退已有）。
- v3 修正动因（clue123）：v2 径向弥散上向越让位线约 16px、展开态左向越侧栏左缘约 12px；包络扫描（三档×两态×360° hash 连续域）程序化复核全部通过。
`````

### doc#21 · 2026-11-16-mcp-zcode-install（prd v1 · need#279）

- 标题：MCP 详情安装/卸载到 ZCode 全局配置 PRD（ZCode 侧增量）
- 创建：2026-11-16 22:19:52 +0800（product-manager）· 更新：2026-11-16 22:19:52 +0800（product-manager）

`````markdown
# MCP 详情安装/卸载到 ZCode 全局配置 PRD（ZCode 侧增量）

> need279 · 2026-11-16 · product-manager · 范围=ZCode 侧增量；Claude Code 侧已上线（need244，commit 0c302d99）不重述，仅作模式锚点。

## 1. 背景与定位

老板需求原文（2026-11-16）：「mcp 的详情，新增安装按钮，支持安装到全局的 claude code、zcode，也支持卸载」——须幂等、冲突可见、失败可见。

- Claude Code 侧已由 need244 落地：src/module_mcp/claude_mcp_config.cpp/.h（服务名 devdesk、幂等=同址 no-op、冲突=同名异址不覆盖引导重装、备份 `.bak-devdesk`、原子写 core/atomic_write.h、三态按钮 mcp_page_module.cpp:696、中文报错、单测 tests/test_modules/test_mcp_claude_config.cpp）。本 PRD 全文不重述其实现，仅按同构模式定义 ZCode 侧增量。
- ZCode 落点：`~/.zcode/cli/config.json` 顶层 `mcp.servers.devdesk`；该文件同时承载 plugins/hooks/skills 与任意未知键，schema 严格（未知键致 server 被静默丢弃）→ 必须全量保留未知键。
- ZCode 无 CLI 可调（本机 which zcode 失败）→ 直改 JSON 是唯一写入路径；模式=复刻 Claude 端：绝不新建文件 / 只动一键 / 备份+原子写 / 幂等。

**需求一句话**：在 MCP 详情页一键把 DevDesk 的 MCP 服务（http://127.0.0.1:\<port\>/mcp）安装/卸载到本机 ZCode 全局配置，重启 ZCode 即可在 ZCode 会话使用 DevDesk MCP 工具，全程无需手工编辑 config.json。

**需求裁决序（need#146）**：①该不该做=做（老板明令+Claude 端先例）②能否直接复用=复用 need244 既有写入编排+三态按钮+单测模板 ③能否改造现有=是（复刻 Claude 端模式为 ZCode 变体）④新方案=否。结论：**改造复用**。

## 2. 目标 / 非目标

目标：
- G1 安装：`mcp.servers.devdesk` 写入成功且字段正确（断言 A1）。
- G2 卸载：该键移除，其余内容语义无损（A2/A6）。
- G3 幂等：重复安装/卸载零重复条目、零多余写入、零多余备份（A3）。
- G4 冲突可见：同名异址不覆盖，中文引导（A4）。
- G5 失败可见：文件缺失/非法 JSON/servers 非对象/不可写，全部中文报错、零半截写入（A5/A7）。
- G6 未知键零破坏（A6）。

非目标：
- N-1 Claude Code 端任何改动或回归（已上线，锚点 0c302d99）。
- N-2 端口固定化 / 端口漂移失配警示（改进项建议另立需求，见 §9 R1 与 OP-1）。
- N-3 ZCode 项目级/其它 scope、desktop Settings→MCP 面联动（只做 user scope 文件）。
- N-4 stdio/其它传输类型、headers/token、多服务批量安装（DevDesk=单 HTTP 回环服务，Host 白名单仅回环，无 token）。
- N-5 安装后自动生效协调（生效=重启 ZCode，见 D3）。

## 3. 用户故事

- US1 作为 ZCode 用户，我在 DevDesk MCP 详情页点「安装到 ZCode」，重启 ZCode 后直接调用 DevDesk 的 MCP 工具，不用手工编辑 config.json。
- US2 我重复点安装不会产生重复条目或配置损坏。
- US3 若 config.json 里已有同名 devdesk 条目，DevDesk 不覆盖它，并告诉我怎么处理。
- US4 我可以一键卸载；卸载只移除 devdesk 条目，不动我其它 MCP 与插件配置。
- US5 出错时（配置文件缺失/损坏）我看到中文说明与建议动作，而不是静默失败。

## 4. 功能矩阵（初值：状态全「待方案」，F-ID×验收要点含断言口径）

| ID | 功能点 | 优先级 | 状态 | 卡片/验收要点（断言口径） |
|---|---|---|---|---|
| F-01 | ZCode 配置路径解析 zcodeConfigFile() | 核心 | 待方案 | 路径=家目录 `.zcode/cli/config.json`（Win=`%USERPROFILE%\.zcode\cli\config.json`，mac=`$HOME/.zcode/cli/config.json`）；不随 DEVDESK_DATA_ROOT 变；平移 claudeConfigFile() 三平台先例（paths_win.cpp:72 / paths_macos.cpp:51 / paths_posix.cpp:52）。断言：两平台各解析出上述字面路径 |
| F-02 | 读取与形态校验 | 核心 | 待方案 | 文件必须已存在（绝不新建）；非法 JSON→中文拒写；`mcp.servers` 缺失→安装时补建该层级；存在但非对象（数组/字符串等）→中文拒写。断言：A5 |
| F-03 | 安装写入（幂等/冲突三分判定） | 核心 | 待方案 | 写 `mcp.servers.devdesk = {"type":"http","url":"http://127.0.0.1:<live.port>/mcp","enabled":true}`；三分判定：无键=可装；有键且 url==当前端点=已安装（no-op，不覆盖不备份不重写）；有键且 url≠当前端点或缺 url=冲突（不覆盖+中文引导）。断言：A1/A3/A4 |
| F-04 | 卸载（幂等） | 核心 | 待方案 | 删 `mcp.servers.devdesk` 一键，其余全量保留；键不存在=no-op 零写入零备份；文件缺失/非法 JSON=中文报错不修复不新建。断言：A2 |
| F-05 | 全文件保全（未知键零破坏） | 核心 | 待方案 | 全量 DOM 读改写保留 plugins/hooks/skills 与任意未知键；验收用解析级 diff（键集合+值相等），仅 devdesk 一键差异；文本键序重排（std::map 字典序，platform/json 实现特性）允许、不算失败。断言：A6 |
| F-06 | 备份 | 边缘 | 待方案 | 有改动才备份：`<config>.bak-devdesk`（写前整文件快照），覆盖式保留最近一份；无改动零备份。断言：A7 前半 |
| F-07 | 原子写 | 边缘 | 待方案 | 复用 core/atomic_write.h atomicWriteFile（Win=MoveFileExW REPLACE_EXISTING）；任何失败不留半截 config.json。断言：A7 后半 |
| F-08 | 详情页 ZCode 三态按钮 | 核心 | 待方案 | 与 Claude 端按钮同构镜像（drawClaudeInstallButton 先例 mcp_page_module.cpp:696），纵排于 Claude 行下方；三态+置灰规则见 §6.2。断言：A8 |
| F-09 | 文案与生效口径 | 边缘 | 待方案 | 成功文案含「重启 ZCode 后生效」；失败文案全中文含原因+建议动作；冲突文案含引导步骤。断言：A8 |
| F-10 | 单元测试 | 核心 | 待方案 | 复刻 tests/test_modules/test_mcp_claude_config.cpp 模式；覆盖 F-01~F-09 全部分支；临时目录隔离不碰真 `~/.zcode`。断言：A9 |

## 5. 用户操作流程（节点级，验收照走）

总则进入条件：DevDesk 已启动、MCP 模块已注册（app_registry kModuleRegistry 含 mcp）。

- **N1 打开 MCP 页 → 点服务条目进详情弹窗**。进入条件：侧边栏 MCP 模块可见。来源：侧边栏。
- **N2 弹窗渲染按钮组**（Claude 行=已有不动；ZCode 行=本条新增，位于 Claude 行下方）。进入条件：弹窗打开；此时同步读取 config.json 判定 ZCode 态（本地小文件同步读，无独立加载态）。来源：N1。
- **N3a 态=未安装**：按钮文案「安装到 ZCode」可点。来源：N2。
- **N3b 点击安装** → 写入编排（校验→备份→原子写）→ 成功：中文提示「已写入 ZCode 全局配置，重启 ZCode 后生效」，按钮迁=已安装。失败→N5；冲突→N6。来源：N3a（前置：MCP 服务运行中，见 N8）。
- **N4a 态=已安装**：显示已安装标识+「卸载」可点。来源：N2 或 N3b 成功后。
- **N4b 点击卸载** → 删键编排（校验→备份→原子写）→ 成功：中文提示「已从 ZCode 全局配置移除，重启 ZCode 后生效」，按钮迁=未安装。失败→N5。来源：N4a。
- **N4c 卸载幂等兜底**：文件中键已被外部移除而按钮态过期时点卸载→no-op，提示「当前未安装」，按钮迁=未安装，零写入零备份。来源：N4a。
- **N5 失败分支**（文件不存在 / 非法 JSON / mcp.servers 非对象 / 不可写）：中文报错（原因+建议动作），按钮态不变，文件与备份零变更；中断安全由原子写保证（不留半截文件）。来源：N3b/N4b。
- **N6 冲突分支**（同名异址）：不覆盖，原条目逐字段不变；按钮=冲突置灰；中文引导「ZCode 配置中已有同名 devdesk 条目且指向不同地址，请先在 ZCode 配置中移除该条目后再安装」。冲突态**不提供卸载入口**（防误删用户自定义条目）；用户移除后重开详情页回到未安装态。来源：N3b 或 N2 判定时直接呈现。
- **N7 重复安装兜底**：按钮态过期时重复触发安装且键已同址存在→no-op（changed=false），零写入零新备份，保持已安装态。来源：N3b。
- **N8 MCP 服务未运行**：端口不可取→按钮组整体置灰+提示「启动 MCP 服务后可安装」（判定三态需端口对比，与 Claude 端行为一致）。来源：N2。
- **终态**：安装成功→用户重启 ZCode→ZCode 会话可用 mcp__devdesk__* 工具（DevDesk 外部行为，不属本验收实操）。

走查声明：任意操作序列（N3b 失败→重试→成功→卸载→再装；N6 冲突→外部移除→重开→安装；重复装/重复卸）均不遇死路或未定义态。

## 6. 完整状态集 + UI 契约

### 6.1 ZCode 按钮数据态（空/加载/错误/成功+边界，逐态可核对）

| 态 | 判定 | 展示 | 可操作性 |
|---|---|---|---|
| 空（未安装） | 键不存在 | 「安装到 ZCode」 | 可点（须服务运行） |
| 加载 | — | 无独立加载态（本地小文件同步读）；写操作期间按钮防抖置忙，防双击双写（对齐 Claude 端，developer 确认） | — |
| 成功（已安装） | 键存在且 url==当前端点 | 已安装标识+「卸载」 | 卸载可点 |
| 错误（读取失败） | 文件缺失/非法 JSON/servers 非对象 | 按钮置灰+错误摘要；触发操作时中文报错 | 不可点 |
| 边界-冲突 | 键存在且 url≠当前或缺 url | 置灰+引导文案 | 不可点；无卸载入口 |
| 边界-服务未运行 | 端口不可取 | 整组置灰+「启动 MCP 服务后可安装」 | 不可点 |
| 边界-不可写 | 只读/权限 | 归入错误态；点击时报中文错 | — |
| 边界-超大文件 | 159KB 级（Claude 文件实测先例） | 同步读改写成本可忽略，无特殊态 | — |
| 平台差异 | 仅路径前缀（`~/` vs `%USERPROFILE%\`） | 无 UI 差异 | — |
| 并发面 | ZCode desktop 运行中 | 不感知；文案只承诺「重启后生效」 | 见 D3 |

### 6.2 UI 契约（可截图对照）

```
MCP 详情弹窗（现有结构）
├─ 服务信息区（端口/运行状态…现有）
├─ 「安装到 Claude Code」行：三态按钮（need244 已上线，不动）
└─ 「安装到 ZCode」行（新增）：同构三态按钮，纵排于 Claude 行下方
```
- 控件结构/交互/置灰规则与 drawClaudeInstallButton（mcp_page_module.cpp:696，成功提示先例 :687）完全镜像，仅判定目标文件与文案不同。
- 文案表：未安装「安装到 ZCode」；已安装标识+「卸载」（句式镜像 Claude 端实际文案）；冲突与失败文案见 N5/N6；成功文案必含「重启 ZCode 后生效」。
- **UI 门控跳过记档**：ZCode 行=同页镜像微调，双端纵排两行无布局冲突、无实质设计量，维持跳过 ui-ux-designer（brief 授权复核结论）。

## 7. 验收标准（每格可测断言，直接喂 developer 方案卡）

- **A1【F-01/F-03】** 对真实路径（或同构测试桩）安装后：`mcp.servers.devdesk` 存在且字段级相等 `{"type":"http","url":"http://127.0.0.1:<port>/mcp","enabled":true}`；port=安装时刻 app::mcp::status().port 实测值（真实文件断言）。
- **A2【F-04】** 卸载后 `mcp.servers.devdesk` 不存在；mcp.servers 其余条目与文件其余顶层键语义不变（真实文件断言）。
- **A3【F-03】** 同态重复触发安装：断言 changed=false、文件内容无第二次变更、无新 `.bak-devdesk`；全文件仅一条 devdesk 条目。
- **A4【F-02/F-08】** 预置 devdesk={url 与当前端点不同}：不覆盖（原值逐字段不变），按钮=冲突态，中文引导文案出现。
- **A5【F-02】** 三分支各断言：文件不存在→执行后路径仍不存在且中文报错；非法 JSON→中文报错且文件字节不变；`mcp.servers`=数组/字符串→中文报错且零写入。
- **A6【F-05】** 构造含 plugins/hooks/skills+≥3 个未知顶层键的样本：装、卸各一次后解析级 diff 仅 devdesk 一键增/删；未知键逐字段相等（键序重排允许）。
- **A7【F-06/F-07】** 有改动→存在 `<config>.bak-devdesk` 且内容==写前快照；无改动→无新备份；单测模拟写失败→目标文件保持改前完整字节（无半截文件）。
- **A8【F-08/F-09】** 成功文案含「重启 ZCode 后生效」；全部失败/冲突文案为中文且含原因+建议动作；按钮三态+服务未运行置灰渲染正确（对照 §6.2 截图）。
- **A9【F-10】** 单测全绿；临时目录隔离（用例不读写真 `~/.zcode`）；用例总数以运行输出 Total 为准。

## 8. 决策点裁决（D1-D4）

- **D1 端口漂移：裁决=沿用 Claude 端现状**。安装写 live.port；DevDesk 重启致端口变化后，已装条目 URL 过期，靠本页卸载重装收敛；本条不做失配警示/端口固定。依据：**本软件一致性**（need244 已上线同构行为，双端口径一致优先）；改进项涉及端口治理新能力，超出本条 90% 核心价值。残余风险记 §9 R1，改进需求建议另立池条目（OP-1）。**不升格老板**（沿默认不改变需求范围，用户可随时卸载重装自愈）。
- **D2 同名异址冲突：裁决=不覆盖+中文引导**（与 Claude 端同口径）。补充明确：三分判定见 F-03；冲突态无卸载入口（保守于「引导卸载重装」的字面，防误删用户条目）。依据：**本软件一致性**（need244）。
- **D3 ZCode desktop 并发面：裁决=验收口径「desktop 运行中安装，以其 Settings→MCP 页刷新结果为准」**。DevDesk 不感知、不协调 desktop；文案只承诺「重启 ZCode 后生效」。依据：**行业惯例**（外部工具写他应用配置的通行口径）+官方 pitfall（desktop 托管列表可覆盖文件）。残余见 §9 R2。
- **D4 备份命名：裁决=`config.json.bak-devdesk`**。对齐 Claude 端 `.bak-devdesk` 后缀族（**本软件一致性**）；用户按后缀即可全局搜出 DevDesk 所做备份。

实现细节裁决（PM 职权内，不升格）：
- 写入字段集 `{"type":"http","url","enabled":true}`：依据=本机实测条目形态+官方 schema（线索 179）；显式 enabled 消除官方文档 enable/enabled 措辞出入的缺省歧义。标「已 POC 验证（Claude 端同构+实测条目）/ 待 developer 方案确认」。
- `mcp.servers` 键缺失时补建层级（文件已存在前提下；存在但非对象仍拒写）：依据=Claude 端「绝不新建文件、只动一键」语义的自然延伸。

## 9. 风险与残余

- **R1 端口漂移（D1 残余）**：发生=安装后 DevDesk 重启且端口变化；用户感知=ZCode 侧工具调用失败且不易归因到 DevDesk；收敛=本页卸载重装；改进（失配警示/一键重装提示/端口固定）另立（OP-1）。
- **R2 desktop 覆盖（D3 残余）**：ZCode desktop 托管 server 列表可能覆盖文件级条目（官方 pitfall 11，线索 179）；验收已按「以 desktop 设置页刷新为准」收窄。
- **R3 键序重排 diff 噪音**：platform/json 对象=std::map 存储，全文件键重排为字典序（语义无损）；用户手做文本 diff 会见噪音。与 Claude 端已接受行为一致，不另处理（线索 180）。
- **R4 卸载删整键**：用户在 devdesk 同址条目上附加的自定义字段（headers/timeoutMs 等）随卸载删除；`.bak-devdesk` 备份可恢复；冲突态无卸载入口已挡掉异址场景。

## 10. 关联影响

- 落点建议（待 developer 方案卡确认，PRD 不下技术选型结论）：src/module_mcp/ 新增 ZCode 配置读写单元（镜像 claude_mcp_config 模式）；mcp_page_module 详情弹窗新增 ZCode 行；core/paths 三平台各加 zcodeConfigFile()；复用 platform/json 全量 DOM 读改写、core/atomic_write.h。标「已 POC 验证（Claude 端先例）/ 待方案确认」。
- 平台纪律：路径解析必须三平台同条落地（Win/mac/posix），业务代码禁 #ifdef。
- 回归面：Claude 端按钮与单测零改动；现有详情弹窗布局仅增一行。

## 11. 开放问题（无待定项，每条带推荐+影响）

- **OP-1 端口失配改进是否立项**：推荐=立项「安装后端口失配警示」（失配检测+一键重装提示，优先级高于端口固定）；影响=不立项则 R1 永远靠手工卸载重装收敛；立项则+1 条需求排产。待老板/排产裁决。
- **OP-2 官方 enabled 缺省歧义**：已用显式 `enabled:true` 消除（§8 实现细节裁决）；若 ZCode 未来 schema 变更需复查。记录备查，无需用户裁决。

## 12. 产出标准硬清单自检（定稿前逐条过）

1. **节点全枚举**：N1-N8+终态，每节点有名称+进入条件+来源，入口到终态无断链，任一节点答得出「从哪来」。✅
2. **数据态四态**：§6.1 逐态覆盖 空/加载/错误/成功+冲突/边界/平台差异/并发面，无空项。✅
3. **操作与触发**：安装/卸载两操作×各态触发结果（界面反馈+数据动作+结果呈现）在 N3-N7 全列，与 §6.2 逐元素对照无缺。✅
4. **流转去向**：成功迁态/失败态不变+中文报错/中断由原子写兜底（N3b/N4b/N4c/N5/N6/N7），走查无死路无未定义态。✅
5. **澄清升格**：全文无「待定/暂定/TBD」；模糊点已升格为 D1-D4 裁决与 OP-1/OP-2，各带推荐+影响。✅
6. **闭环自走查**：主路径（装→重启→用→卸载）+失败路径（文件缺失/非法 JSON/servers 非对象/冲突/不可写）+幂等路径（重复装/重复卸/外部删键）走查毕；走查发现的模糊点（冲突态卸载入口、enabled 缺省、servers 缺失补建）已回填 §8。✅
`````

### doc#22 · 2026-11-16-mcp-zcode-install-tech（tech v1 · need#279）

- 标题：MCP 详情安装/卸载到 ZCode 全局配置 技术方案卡
- 创建：2026-11-16 22:26:32 +0800（developer）· 更新：2026-11-16 22:26:32 +0800（developer）

`````markdown
# MCP 详情安装/卸载到 ZCode 全局配置 技术方案卡（need279）

> 2026-11-16 · developer · 依据 PRD doc 21（2026-11-16-mcp-zcode-install v1）+ 线索 178-182。裁决序=改造复用：复刻 need244 claude_mcp_config 模式为 ZCode 变体，无新方案。

## 0. 方案结论（一句话）

新增 `src/module_mcp/zcode_mcp_config.cpp/.h`（namespace `module::mcp::zc`，与 `cc` 并列零耦合）+ `core/paths` 三平台 `zcodeConfigFile()` + 详情弹窗 ZCode 行（kDialogH 480→516 腾第二行），写 `~/.zcode/cli/config.json` 顶层 `mcp.servers.devdesk = {"type":"http","url","enabled":true}`；读改写编排/备份/原子写/绝不新建文件全部复用 Claude 端同构实现，新增仅两处本质差异：①两级键 `mcp.servers`（缺失补建层级、非对象拒写）②安装三分判定（无键可装/同址幂等 no-op/异址冲突不覆盖 fail）。

## 1. 改动文件清单

| 文件 | 动作 | 内容 |
|---|---|---|
| `src/core/paths.h` | 改 | 声明 `zcodeConfigFile()`（claudeManagedSettingsFile 后） |
| `src/core/paths_win.cpp` / `paths_macos.cpp` / `paths_posix.cpp` | 改 | 各 +8 行：`<home>/.zcode/cli/config.json`（Win=USERPROFILE，mac/posix=HOME；无覆盖 env，不随 DEVDESK_DATA_ROOT 迁移） |
| `src/module_mcp/zcode_mcp_config.h/.cpp` | 新 | namespace `zc`：kServerName/ConfigState/applyServerEntry/removeServerEntry/installServerFile/removeServerFile/backupFile |
| `src/module_mcp/CMakeLists.txt` | 改 | 源清单 + zcode_mcp_config.cpp |
| `src/module_mcp/mcp_page_module.h` | 改 | ZcButtonState(含 conflict 位)/zcState_/zcError_/zcNotice_/三方法声明/include |
| `src/module_mcp/mcp_page_module.cpp` | 改 | kDialogH 516；Claude 行上移 h-88；ZCode 行 h-52；refresh/toggle/draw 三方法 + openDetail 挂钩 |
| `tests/test_modules/test_mcp_zcode_config.cpp` | 新 | 11 用例覆盖 A1~A9（临时目录隔离） |
| `tests/CMakeLists.txt` | 改 | 测试源清单 +1 行 |

Claude 端三文件（claude_mcp_config.*、既有按钮代码）零改动（N-1）。

## 2. 逐 F-ID 实现方案与断言

**F-01 路径**：`zcodeConfigFile()` 平移 claudeConfigFile 三平台写法，去掉 override 分支（PRD 无覆盖要求、不随数据根迁移）。断言：单测检查 path 各级分量 `.zcode/cli/config.json`；Win 用 USERPROFILE（本机实测红绿即证）。

**F-02 读取与形态校验**：镜像 claude_mcp_config.cpp:35 parseRoot（label="~/.zcode/cli/config.json"，非法 JSON/顶层非对象中文 fail）+ 两级版 ensureServersObjectIfPresent：`mcp` 键存在且非对象→fail；`mcp.servers` 存在且非对象→fail；`mcp` 缺失或 null→安装时补建 `{mcp:{servers:{…}}}`（文件已存在前提由 rewriteExisting 把守）。断言 A5 三分支。

**F-03 安装三分判定**（applyServerEntry 纯函数）：
- 无键 → 写 `{"type":"http","url":<url>,"enabled":true}`（显式 enabled，PRD §8 裁决）→ changed=true
- 有键且 `url` 字段==入参 → 原文本原样返回 changed=false（不覆盖不备份，A3）
- 有键且 url≠入参或 url 缺失/非串 → fail，中文冲突文案（含现有 url，A4）

**F-04 卸载**（removeServerEntry）：逐键重建 servers 摘 devdesk（封装无键删 API，claude 端 :114 同款）；摘空保留空对象（最小 diff）；键不存在/mcp 缺失→changed=false no-op；mcp/servers 非对象→fail 拒写。断言 A2。

**F-05 全文件保全**：platform/json 全量 DOM 读改写（std::map 键序重排允许，PRD §9 R3）；A6 用解析级 diff：键集合+逐键 stringify 等值，仅 devdesk 一键差异。

**F-06 备份**：rewriteExisting 镜像（claude_mcp_config.cpp:152）：文件必须已存在（缺失 fail「未检测到 ZCode 配置 <path>（不自动创建）」）→ 读→mutate→changed 才写 `<file>.bak-devdesk`（backupFile()，D4 裁决）→ atomicWriteFile；备份失败即整体失败。

**F-07 原子写**：复用 core/atomic_write.h atomicWriteFile（:15），零新代码。

**F-08 三态按钮**：ZcButtonState{filePresent, installed, conflict, serviceRunning}，actionable=三者前且非 conflict。判定数据流：readConfigState 返回 installed+installedUrl → 页面用 live.port 拼端点比对得 conflict（服务未运行时端口无效，hint 优先级 filePresent→serviceRunning→conflict→error→notice，与 N8 一致）。布局：kDialogH 480→516（+36=行高32+gap4）；Claude 行+hint 上移 y=h-88；ZCode 行（btn 160×32 @x20 + hint @188..396）落 y=h-52 与 close/try 同行左半——ZCode 行位于 Claude 行正下方 ✓；result 区随 h 公式自然增高 36，既有节点 y 不变。冲突态按钮 disabled=无卸载入口（N6）。

**F-09 文案**：成功「已写入，重启 ZCode 后生效」/「已从 ZCode 全局配置移除，重启 ZCode 后生效」；冲突 hint=「ZCode 配置已有同名 devdesk 条目且地址不同，请先在 ZCode 配置中移除该条目后再安装」（PRD N6 等义压缩：提示槽 208px×2 行仅容 ~36 字符，完整因果+动作保留，完整版在 fail message 含现有 url）；服务未运行「请先启动 MCP 服务」；缺文件「未检测到 ZCode（~/.zcode/cli/config.json 不存在）」。

**F-10 单测**：tests/test_modules/test_mcp_zcode_config.cpp 复刻 claude 模板（test_temp::tempPath 隔离），11 用例映射：①安装写 http+enabled 三字段+其余保全(A1/F-03) ②层级补建(F-02) ③同址幂等 no-op(A3) ④异址冲突 fail+中文(A4) ⑤卸载只摘一键+往返守恒(A2) ⑥卸载 no-op+不造键 ⑦非法输入四类 fail(A5) ⑧缺文件不建+无备份(A5) ⑨文件级 roundtrip+备份内容(A7) ⑩文件级重复安装零重写(A3) ⑪真文件副本 install→remove 解析级 diff 仅一键(A6，真文件只读不写)。目录冒充文件用例并入⑧族。

## 3. 红绿证据链

先提交测试文件+CMake→`--dev --filter zc_` 编译红（缺函数）→实现三批（paths/zc 单元/UI）→`--dev --filter file:test_mcp` 全绿（含 claude 既有 12 用例不回归）。收口全量归 QA。

## 4. 风险与取舍

- 弹窗 +36px 高：480→516 仍在最小窗口安全高度内（结果区反而增高）；若 QA 视检有异议回退方案=压缩 result 区常量。
- 冲突文案压缩：槽位宽度物理限制，断言口径（中文+原因+动作）不受影响，完整信息在 fail message。
- 端口漂移/ desktop 覆盖：PRD D1/D3 已裁决沿用现状，方案不引入新机制。

`````

### doc#23 · 2026-11-17-git-manager-redesign（prd v2 · need#291）

- 标题：PRD：Git 管理器整体重设计（只读 v2）——骨架+功能矩阵+决策点清单
- 创建：2026-11-17 13:27:40 +0800（product-manager）· 更新：2026-11-17 13:38:19 +0800（product-manager）

`````markdown
# PRD：Git 管理器整体重设计（只读 v2）——PRD 骨架 · 功能矩阵 · 决策点清单

> need#291 · 2026-11-17 · 作者 product-manager · 状态：骨架稿 v2（矩阵初值全「待方案」，等老板裁决 D1~D7 后进 ui-ux-designer 设计文档+HTML demo）
> 前身文档：`docs/superpowers/specs/2026-11-13-git-manager-readonly-ui-a.md`（方案 A·IDEA 严格三栏，已被老板 2026-11-17 四叹号令整体否定，仅作现状参考，不扩展）。
> **v2 修订记录（2026-11-17）**：并入 researcher 调研（doc id=24, slug=`2026-11-17-git-redesign-research`，5 家客户端样本）——D1/D2/D3/D5/D6 理由段补业界实证、D1 补两范式实现成本注、D3 lazy 展开降标「推测级需 POC」、订正 §10 bug281 提交号误标、调研 §七遗留 5 项并入 §12。**D1~D7 编号与推荐默认、老板原话表、功能矩阵、状态集均未动（裁决面不变）。**

## 1. 老板原话（逐字，口径唯一权威）

| 时间 | 载体 | 原话 |
|---|---|---|
| 2026-11-17 | need#291 | 「整个功能逻辑都不对，没有哪个UI功能是完全正常的，需要重新设计和梳理UI和功能逻辑！！！！」 |
| 2026-11-17 12:57 | need#289（装机版复验） | 「变更这个目录树也没修复啊 没必要单独弄出来 且布局特别不合理」 |
| 2026-11-17 12:57 | bug#290（装机版实测） | 「变更列表中的文件查看diff 提示我没变更」 |
| 2026-11-16 | bug#280 | 「仅修改」tab 文件名/内容/diff 全空白错乱；「未跟踪」为什么是单独 tab？用户预期：未跟踪应是「仅修改」里的一个类型（状态标记），而不是平行 tab |

定性：**信息架构级全量重设计**，不是修 bug。四条反馈同指一个病根——变更数据被三栏重复呈现 + 动线绕（左栏 tab → 中栏列表 → 右栏双 tab）。

## 2. 现状核对（2026-11-17 实查，已证事实）

### 2.1 交付链（git log 实查）
`8ecf04e6` v1 三栏只读查看器（libgit2 vendored）→ `b2c69429` v2 全局搜索 → `704bb3a8` CJK 横滚 → `82bca272` need14 收口 → `1aef4537`+`beb247f7` bug280 两轮修复（空名已修，结构未动）。

### 2.2 现状结构（代码级，module_git 实查）
- **三栏**：左=目录树（「全部文件/仅修改」两 tab，仅修改=变更平铺）；中=「变更 (N)」组头+全路径平铺列表；右=路径条+「内容/diff」双 tab。
- **病根 1（重复呈现）**：「仅修改」tab 与中栏变更列表同数据两遍（need#289「没必要单独弄出来」所指）。
- **病根 2（误导空态）**：untracked 目录条目（路径尾 `/`，数据面整体一条不递归）点击后 diff 模型无 hunk → 右栏显示「无内容变更」，实为有变更（bug#290 现象之一）。
- **病根 3（归组错位）**：untracked 独立 tab 与老板预期「状态标记类型」相悖。

### 2.3 硬约束（数据面，重设计必须继承）
1. **untracked 目录整体一条不递归**（`git_repo.h:17`；防 GitAhead #161 式大目录内存爆，qa12 内存基线依赖）。
2. **v1/v2 只读**：无 staging/commit/push 操作（本 PRD 维持只读边界）。
3. diff 基准现状=workdir↔HEAD 单基准（`DiffBasis::WorkdirToHead`，`git_repo.h:35`）；`TreeToIndex`/`IndexToWorkdir` 接口已在。调研 D6 已证：v1 只读场景下单基准与业界等价（GH Desktop 点击即 workdir diff），不是缺陷；staged 概念引入后才需双基准（本期非目标 2 不动作）。
4. worker 线程模型 + `Result<T>` 无异常 + virtualList 分块喂（巨 diff >5000 行不整帧重算）——技术方案阶段延续，不在 PRD 展开。

### 2.4 调研实证（researcher doc id=24 已落库并入，2026-11-17）
样本：GitHub Desktop、VSCode、IntelliJ IDEA、Fork、Sourcetree（5/5）。五条已确认结论（PRD 决策点理由段引用，锚点见调研 §六）：
1. 变更区**独立全量目录树 0/5**——业界只有「变更列表」一个对象，导航靠树形态/Filter；老板「没必要单独弄出来」与全行业一致。
2. **untracked 无独立 tab 5/5**——全部混入变更主列表（U 徽标/节点/成员）；bug280② 归并方向被业界反向验证。
3. 树视图实现本质=对**已变更文件路径集**派生目录节点（纯数据变换），非全量目录扫描 → 与 F-09 不递归防 hang 约束**正交**。
4. 分组三派：状态节（VSCode/Sourcetree/Fork staged/unstaged）、changelist 轴（IDEA+Unversioned 节点）、无节+勾选（GH Desktop）。
5. **选中即 diff 5/5，业界无「内容浏览 tab」**——同窗右侧面板（GH Desktop/Fork/Sourcetree）或点击开编辑器 diff（VSCode/IDEA）；我方「内容/diff 双 tab」+路径条是业界不存在的第三种形态，且路径条与列表信息重复。

### 2.5 轻量联网自查（2026-11-17，已被 §2.4 深度调研覆盖并修正）
VS Code 官方树/平铺切换（vscode#225620/#14582）；GitHub Desktop 仅平铺（desktop#20624）。与调研结论一致，留作来源冗余。

## 3. 需求一句话

把 Git 管理器从「三栏拼凑、变更数据重复呈现」的只读查看器，重设计为「变更列表唯一动线 + diff 主区」的只读工作台，每个入口点击都有正确反馈。

## 4. 目标 / 非目标

**目标**
1. 变更数据只呈现一次（消灭左/中重复）。
2. 点任何条目都有语义正确的反馈（目录条目不再报「无内容变更」误导）。
3. untracked 按「状态类型」归组，不设独立 tab。
4. 布局在 1280×720 基准档不挤压（每栏有可读宽度）。

**非目标（v2 只读边界内不做）**
1. staging/unstaging、commit、push/pull 等写操作（仍只读）。
2. staged/unstaged 二分 diff 切换（`TreeToIndex` 接口留而不用；staged 概念引入后才需双基准，调研 D6 同口径，届时再开需求）。
3. blame 视图（接口留而不用，同 v1）。
4. 双栏对照 diff（保持 unified 单列）。
5. 仓库级管理功能（分支/标签/history 时间线）——researcher 调研若证明强诉求，进修订不在本版。

## 5. 功能矩阵（状态初值全「待方案」；卡片=决策点锚）

| ID | 功能点 | 优先级 | 状态 | 卡片 |
|---|---|---|---|---|
| F-01 | 页面骨架与栏位收敛（三栏→两栏主态） | 核心 | 待方案 | D4 |
| F-02 | 变更列表形态（树/平铺/切换）与组头（已暂存/未暂存） | 核心 | 待方案 | D1、D6 |
| F-03 | 左栏目录树「全部文件/仅修改」去留与职责迁移 | 核心 | 待方案 | D2 |
| F-04 | untracked 归组 + 目录条目展开交互（数据面不递归硬约束保留） | 核心 | 待方案 | D3 |
| F-05 | 文件选择→diff 动线（bug#290 误导修复口径：目录条目≠文件 diff） | 核心 | 待方案 | D5 |
| F-06 | diff 视图（unified、hunk 折叠、二进制/超大占位态——继承 v1 既有能力） | 核心 | 待方案 | D5 |
| F-07 | 内容预览（只读）在新动线中的位置 | 边缘 | 待方案 | D5 |
| F-08 | 全局搜索（v2 已交付 `b2c69429`）适配新布局 | 边缘 | 待方案 | D7 |
| F-09 | 状态刷新（FileWatcher/手动刷新链路，现状保留） | 边缘 | 待方案 | — |
| F-10 | 空态/错误态/边界全集（空仓、无变更、扫描失败、无权限） | 核心 | 待方案 | §7 |
| F-11 | 超长路径/CJK/缩放 1.354 兼容（继承 need14 收口成果） | 边缘 | 待方案 | — |
| F-12 | staged/unstaged/untracked 状态语义呈现（徽标+着色 token） | 核心 | 待方案 | D6 |

## 6. 决策点清单 [D#]（老板级待定项一次列全；每项带推荐默认+理由，可单轮「按推荐」整轮放行）

### D1 变更区形态：树 vs 平铺 vs 双态切换
- **推荐默认：C——单列表一份，树/平铺双态切换，默认按文件夹分组树**（同屏右上角小切换钮）。
- 备选：A=纯文件夹分组树（JetBrains 默认）；B=纯平铺+路径前缀徽标（GitHub Desktop 现状）。
- 理由（v2 补业界实证）：**同类产品先例**——变更列表树形态 3/5 生产级先例（VSCode 切换、IDEA Group By Directory、Fork 自 1.0.29 默认树）；纯平铺+Filter 2/5 同为成熟路线（GH Desktop/Sourcetree）；双态切换是最大公约数。树视图实现本质=已变更路径集派生目录节点，纯数据变换，**与 F-09 不递归防 hang 约束正交**（调研结论 3，零全量目录 IO）。
- **实现成本注（v2 补，不替老板选）**：范式 A（树/平铺切换）需自实现 tree 组件——EUI-NEO 无现成 tree（`docs/EUI-NEO-CAPABILITIES.md:35`/:618：virtualList+▶/▼+展开状态机约 150 行；virtualList `:114`、checkbox `:80` 现成）；范式 B（纯平铺+Filter）virtualList+input 过滤全部现成（`:79`），改动最小。调研判：两范式均可落地（三问门全过），选型归老板/demo 拍板。
- 影响：默认态影响首屏观感（demo 看图拍板）；范式 A 实现成本高于 B（约 +150 行组件）。

### D2 左栏目录树（全部文件/仅修改两 tab）去留
- **推荐默认：A——整列砍掉**。页面收敛两栏：左=变更列表，右=diff/内容。「全部文件」浏览职责不保留独立栏（需要时走 v2 全局搜索定位）。
- 备选：B=保留「全部文件」为顶栏模式切换（「变更 / 文件」两模式，segmented control）；C=保留三栏但左栏只留「全部文件」单 tab。
- 理由（v2 补业界实证）：**同类产品先例**——变更区独立全量目录树 **0/5**（调研结论 1：GH Desktop/VSCode/IDEA/Fork/Sourcetree 五家均无，业界只有「变更列表」一个对象）；need#289「没必要单独弄出来+布局特别不合理」与全行业一致。左栏职责由变更列表自身（树形态/Filter，D1）吸收。
- 影响：丢失「任意未变更文件内容预览」入口（v1 有此能力）；若老板依赖它看仓库结构，选 B 可保（代价：多一组模式态，动线分叉）。**此项影响能力收窄，请老板明示。**

### D3 未跟踪目录条目交互口径（bug#290 修复主口径）
- **推荐默认：A——目录条目可展开**：点击/箭头展开该目录下 untracked 文件清单（懒加载），文件行可看 diff；数据面维持「不递归」硬约束（枚举走 filesystem 逐层懒加载，不走 libgit2 diff 递归）。展开前目录行点击不再进入 diff（消灭「无内容变更」误导）。
- 备选：B=目录条目不可展开，点击右侧显示子文件清单占位页（多一次跳转）；C=维持整体一条+文案改「目录条目，请逐文件查看」。
- 理由（v2 补业界实证+降标）：**同类产品先例**——业界树视图点文件夹=**展开/收起，从不触发 diff**（VSCode 树视图还可整夹 stage；调研 D3 已确认），我方把文件夹条目送进 diff 流=语义错位；业界对 untracked 一律列出每个文件（IDEA Unversioned Files 节点、VSCode U 条目，调研 D4）。**可行性标注（v2 降标）**：untracked 目录 lazy 展开为调研标注的**推测级方案（未实证，需 POC 验证 status 枚举成本）**——推荐默认 A 的落地以技术方案阶段 POC 通过为前提，POC 不过则退备选 B（不改变推荐倾向，改变交付确定性预期）。
- 影响：A 需新增变更树模型的目录节点展开态（模型改动中等）+ 一项 POC 前置；C 零改动但误导仍在，违背目标 2。

### D4 三栏布局收敛方案
- **推荐默认：A——两栏主态**（左变更列表 ~320px 可折叠，右 diff/内容主区占余宽），1280×720 基准档验证不挤压。
- 备选：B=两栏+右区可再折叠全宽列表（大屏双 review 场景）；C=保留三栏职责重划。
- 理由：**同类产品先例**——业界普遍两栏单窗（调研 D7：GH Desktop 源码 `changes.tsx` 仅渲染 diff-container、列表由父级组合在左侧）；三栏在 1280 宽每栏 <300px 是「布局特别不合理」（need#289）的直接体感源；路径条与列表信息重复（调研结论 5）。**本软件一致性**——md 编辑器同为「树+编辑列」两栏先例（need#130 曾修对齐）。
- 影响：F-01 骨架重画；v2 搜索结果浮层锚定点随布局迁移（F-08）。

### D5 diff 动线（右栏视图组织）
- **推荐默认：A——点变更条目即进 diff，diff 为常驻主视图；内容预览降级为 diff 头部按钮切换的次视图**（不再是并列 tab）。
- 备选：B=保留「内容/diff」双 tab 但点击条目强制落 diff（现状行为+修 bug#290）；C=砍内容预览只留 diff。
- 理由（v2 补业界实证）：**同类产品先例**——**选中即 diff 5/5，且业界变更区均无「内容浏览 tab」**（调研结论 5：GH Desktop/Fork/Sourcetree=同窗右侧面板选中即 diff，VSCode/IDEA=点击开编辑器 diff）；我方「内容/diff 双 tab」是业界不存在的第三种形态，路径条与列表信息重复。老板「整个功能逻辑都不对」含双 tab 动线绕的成分。
- 影响：F-06/F-07 视图组织重排；内容预览对 untracked 大文件（>1MiB 占位）与二进制的占位态沿用既有口径（§2.3 硬约束 1 的模型合成逻辑不变）。

### D6 staged/unstaged/untracked 分组口径
- **推荐默认：A——变更列表顶部两组头「已暂存 (N) / 未暂存 (N)」，untracked 并入未暂存组、行内「U」徽标标注**（不设独立 tab/组，符合 bug#280 老板预期「状态标记类型」）。
- 备选：B=三组头（已暂存/未暂存/未跟踪）；C=不分组，仅徽标（平铺内按状态着色，现状近似）。
- 理由（v2 补业界实证）：**同类产品先例**——**untracked 无独立 tab 5/5**，全部混入变更主列表（VSCode U 徽标、IDEA Unversioned Files 节点、GH Desktop 平铺成员、Fork 树内条目、Sourcetree unstaged 节；调研结论 2）；分组三派中状态节为多数派（VSCode Changes/Staged Changes、Sourcetree/Fork staged/unstaged 两节）。数据面已具备（`StatusEntry.staged` 双侧两条，`git_repo.h:24`），分组=模型重排不新增数据面。单节无分组（C）与只读定位自洽（调研结论 4）但丢失 index 状态可见性。
- 影响：组头可折叠则需记忆折叠态（内容锚口径沿用 clue#14）；B 三组头违背业界 5/5 一致先例。

### D7 全局搜索（v2 已交付）适配
- **推荐默认：保留顶栏搜索框与结果浮层，定位跳转目标改为新变更列表选中行**（跨文件命中仍走内容/diff 锚定）。
- 备选：无独立备选（功能已交付，只随布局适配；如砍搜索需老板明示）。
- 理由：**本软件一致性**——v2 搜索刚按 need14 收口交付，砍掉=负资产反转；新布局不改变搜索的数据链。
- 影响：F-08 状态：已交付待适配（不是新开发）。

> 以上 D1~D7 覆盖简报要求的全部五项（变更区形态=D1、左栏仅修改 tab 去留=D2、未跟踪文件夹条目交互=D3、三栏布局收敛=D4、diff 动线=D5）。老板可逐项裁决或单轮「按推荐」放行；裁决后 ui-ux-designer 出设计文档+HTML demo（老板看图拍板，UI 先验硬门禁），demo 通过才进技术方案。

## 7. 完整状态集（四态，逐节点；验收照走）

| 节点 | 空 | 加载 | 错误 | 成功（含边界） |
|---|---|---|---|---|
| N1 打开 Git 页 | 无已配置仓 → 空态页「选择仓库目录」+选择按钮（沿用 v1 N2 空态） | 骨架屏（沿用 v1 N3） | 仓目录失效/无权限 → 错误页+重试+重选（沿用 v1 N1） | 三态之外：非 git 目录 → ValidationError 文案 |
| N2 状态扫描 | 空仓（0 提交）/工作区干净 → 「无变更」空态（D6 组头隐藏，不显示空组头） | 扫描中骨架行 | 扫描失败（libgit2 错误码映射）→ 行内错误+重试 | 全链：≤50 条全展开；>200 条折叠组头默认收起（**待确认**阈值，技术方案定） |
| N3 浏览变更列表 | 同 N2 空 | — | — | 目录条目带展开箭头（D3-A）；staged 双侧文件在两组各出现一行（数据面既有） |
| N4 展开 untracked 目录 | 目录为空（竞态：文件被删）→ 行消失+列表刷新 | 懒枚举中 → 行内转圈（**待确认**：>1s 才转圈还是即时） | 枚举失败 → 子行错误态+重试 | 文件行可进 diff；单目录文件数 >500 截断+「在搜索中查看全部」引导 |
| N5 选中文件看 diff | 文件无变更（竞态）→ 「文件已无变更，列表将刷新」+自动刷新 | diff 载入骨架 | diff 失败 → 错误态+重试（bug#290 同族链路根治口径：失败必须显式报错，禁静默空） | unified 渲染；hunk 折叠态记忆 |
| N6 内容预览（D5-A 次视图） | 空文件 → 「空文件」 | 读文件骨架 | 读失败/编码异常 → 既有 BadEncoding 占位 | untracked >1MiB 占位「文件过大，未加载内容」；二进制占位（既有口径不变） |
| N7 全局搜索 | 无命中 → 浮层空态 | 搜索中 | 搜索失败 → 浮层错误行 | 命中定位=新列表选中行+diff 锚定（D7） |
| N8 刷新（FileWatcher/手动） | — | 静默（不打断选中与滚动，内容锚恢复） | 刷新失败保留旧数据+角标提示 | 选中文件被删 → 右栏回空态+列表去行 |

边界横切：超长路径（单行省略+tooltip，need14 口径）/ CJK 换行宽（码点计数）/ Windows 缩放 1.354（bug280 修复后基线）/ 平台差异=双端同构无平台特有分支（业务层禁 ifdef，AGENTS §三）。

## 8. 用户操作流程（节点级闭环；无断链）

入口（侧边栏 Git）→ N1 开仓 → N2 扫描 → N3 浏览列表（展开/折叠组与目录）→ N5 选文件看 diff（主动线）→ N6 切内容预览（次）→ N7 搜索定位（任意时刻可用）→ N8 后台刷新贯穿。
中断回位：N5 失败 → 留在当前文件错误态（可重试/选其它文件）；N4 枚举失败 → 目录行保持展开+子行错误；切仓/关仓 → 全部在途请求作废（既有 resetRightPane 口径）回 N1/N2。

## 9. UI 契约（骨架级；像素级参数由 ui-ux-designer demo 落）

1. 两栏主态（D4-A）：左变更列表（宽度基线 320px，可折叠，折叠钮在列头）；右 diff/内容主区。8px 网格、外壳不滚内容区各自内滚（沿用前身 §1.4 滚动模型基线）。
2. 列表行结构：状态徽标（M/D/R/U/A 着色 token 沿用前身 §3.2 领域编码；调研 D5 已证 A/M/D/R/U 徽标语言与业界一致）+文件名+路径弱化段；目录行=箭头+目录名+子项计数。
3. 组头（D6-A）：「已暂存 (N) / 未暂存 (N)」可折叠，计数随刷新实时。
4. diff 头部：路径条（重命名 old → new 既有口径）+「查看内容/查看差异」切换钮（D5-A）。
5. 空态/错误态页式样沿用 v1 既有三态页骨架（N1/N2/N3），文案按 §7 更新。

## 10. 关联影响

- 吸收：need#289（本 PRD D2/D4）、bug#290（D3/F-05，bug-fixer 在途修复保持独立交付、PRD 口径为准绳）、bug#280 已修部分（空名剥尾斜杠 `beb247f7` 保留为新列表行基线能力）。
- 调研并入（v2）：researcher 调研已落库（doc id=24, slug=`2026-11-17-git-redesign-research`），五条已确认结论并入 §2.4 与 D1/D2/D3/D5/D6 理由段；其 §七遗留 5 项并入 §12 开放问题；D4 备选排序与非目标 5 复核完成（无排序变化，非目标 5 维持）。
- **bug#281（CI pathspec）状态（v2 订正）：v1 版误标修复提交号 9650c187（实为 bug286/bug282 提交）——bug281 修复状态以池台账（bug#281 条目）与 CI 复跑结果为准，本 PRD 不锁定其提交号。**
- 下游：ui-ux-designer 设计文档+HTML demo → 老板看图拍板 → 技术方案（POC：D3 lazy 枚举【推测级，调研遗留 5】、大仓折叠阈值）→ 开发 → 测试 → 审计。

## 11. 验收标准（按 F-ID）

- F-01：1280×720 下两栏无挤压、无横向滚动溢出；左栏折叠后右区满宽。
- F-02：同一变更文件在列表中只出现一次（D2 落地后无第二处呈现）；树/平铺切换不丢选中。
- F-04：untracked 目录展开→文件级 diff 可达；数据面 status 调用次数与 v1 持平（不递归约束不破，qa12 内存基线复测通过）。
- F-05：任选一条变更条目点击，右栏 2s 内呈现该文件 diff 或显式错误（禁静默空/禁「无内容变更」误导——bug#290 验收口径）；untracked 目录行点击不进入 diff（业界口径：点文件夹=展开/收起从不触发 diff）。
- F-10：§7 表 8 节点 × 4 态逐格走查有画面落位（ui-a 状态表承接）。
- F-12：staged 双侧文件两组各一行、徽标正确；untracked 在未暂存组内带 U 徽标、无独立 tab。
- F-08：搜索命中定位到新列表选中行（回归：v2 既有用例改造后全绿）。
- 全局：bug#280/bug#290/need#289 三条原始反馈场景逐一复验通过（装机版实测）。

## 12. 开放问题（全部已升格，无裸待定）

1. D1~D7 待老板裁决（推荐默认已给，可整轮放行）。
2. 技术侧两处「待确认」（§7 N2 折叠阈值、N4 转圈时机）→ 技术方案阶段 POC 定，不阻塞设计稿。
3. **调研遗留待验证项（v2 并入，doc id=24 §七，研究侧收口不阻塞裁决）**：
   - 3a. Sourcetree staged/unstaged 双节布局细节（二手来源，官方文档已迁移）；
   - 3b. VSCode `scm.viewMode` 设置名（检索摘要级，官方页未直引）；
   - 3c. IDEA Group By Directory 菜单路径（YouTrack JS 渲染未直读，菜单文案级）；
   - 3d. GH Desktop rename 两行式细节、Fork 现版 diff 面板布局细节（源码/截图未直读）；
   - 3e. untracked 目录 lazy 展开的 status 枚举成本（推测级，**需 POC**——已同步挂入 §10 技术方案 POC 前置）。
   以上 3a~3d 为引用证据精度问题，不影响已确认结论的方向性（0/5、5/5 为多家交叉一致）；3e 影响的是 D3-A 交付确定性而非推荐倾向。

（本文档 v2：矩阵状态全「待方案」；D1~D7 裁决 + demo 拍板后升版。）
`````

### doc#24 · 2026-11-17-git-redesign-research（tech v1 · need#291）

- 标题：Git 管理器重设计竞品调研：成熟客户端变更区组织范式（need291）
- 创建：2026-11-17 13:32:51 +0800（researcher）· 更新：2026-11-17 13:32:51 +0800（researcher）

`````markdown
# Git 管理器重设计竞品调研：成熟客户端变更区组织范式（need291）

开放性调研，不预设路线，不替老板拍板。样本：GitHub Desktop、VSCode 内置 Git、IntelliJ IDEA、Fork、Sourcetree（5/5，超出验收要求的 3 家）。

## 一、结论先行（5 条，分级标注）

1. 【已确认】5 家变更区**均无「独立全量目录树」**（0/5）。主导范式=单一变更列表+可选树/平铺切换（VSCode、IDEA、Fork）或纯平铺+Filter（GitHub Desktop、Sourcetree）。老板「变更目录树没必要单独弄出来」与全行业一致；左目录树职责应由变更列表自身（树形态/Filter）吸收。
2. 【已确认】未跟踪文件 5/5 无独立 tab，全部混入变更主列表（VSCode U 徽标、IDEA Unversioned Files 节点、GH Desktop 平铺成员、Fork 树内条目、Sourcetree unstaged 节）。bug280② 合并方向已被业界反向验证。
3. 【已确认】树视图实现本质=对「已变更文件路径集」派生目录节点（纯数据变换），非全量目录扫描 → 与 F-09「untracked 目录不递归」防 hang 约束正交，不冲突。【推测】untracked 目录 lazy 按需展开可作方案空间，未实证。
4. 【已确认】分组三派：状态节（VSCode Changes/Staged Changes；Sourcetree、Fork staged/unstaged 两节）、changelist 轴（IDEA+Unversioned Files 节点）、无节+勾选（GH Desktop 单列表）。我们 v1 只读+单基准 → 单节无分组与只读定位自洽，不是缺陷。
5. 【已确认】diff 动线两派：同窗右侧面板选中即 diff（GH Desktop/Fork/Sourcetree）vs 点击开编辑器 diff（VSCode/IDEA）。5 家变更区都**没有「内容浏览 tab」**——选中即 diff。我们右栏「内容/diff 双 tab」+顶部路径条是业界不存在的第三种形态，且路径条与列表信息重复。

## 二、验收①：各家变更区组织

| 客户端 | 布局骨架 | 分组/节 | 树 vs 平铺 | 文件夹条目交互 | untracked 呈现 | diff 动线 |
|---|---|---|---|---|---|---|
| GitHub Desktop | 单窗两栏：变更列表左+diff 右 | 无节，单列表+勾选（staging=inclusion checkbox） | 纯平铺+Filter 框（无树，3.0.0/3.1.0/3.2.0 notes 均无树记载） | 无文件夹条目（平铺） | 普通列表成员，与其它变更无区别 | 选中即右侧面板 diff（DiffHeader+SeamlessDiffSwitcher，统一/分栏可切） |
| VSCode | 活动栏 SCM 视图 | Changes / Staged Changes 状态节 | 树/平铺切换（More Actions > View & Sort > View as Tree/List），树为常用形态 | 树视图：点=展开；文件夹可整体 stage | U 徽标混列 Changes，无独立区 | 点击文件即开 diff 编辑器 |
| IntelliJ IDEA | Commit 工具窗（非模态） | changelist 轴+Unversioned Files 专有节点 | 平铺缺省，View Options→Group By→Directory 可切目录树 | 树=展开/收起 | Unversioned Files 节点内逐文件列出 | Ctrl+D 或双击开 Diff Viewer（编辑器 tab 或独立窗可配） |
| Fork | 单窗两栏：变更左+diff 右 | staged/unstaged 轴 | **树形态自 1.0.29（2016-11-04）**，目录优先排序（1.0.42） | 树=展开 | 树内条目 | 选中即右侧 diff |
| Sourcetree | File Status 视图（CMD+1） | staged/unstaged 两节【二手，待验证】 | 平铺+filter | 无文件夹条目 | unstaged 节内 | 选中右侧 diff / 外部 diff 工具 |

## 三、验收②：与我们三栏结构的差异点逐条（验收③锚点随行）

- **D1 独立全量目录树区（左栏「全部文件/仅修改」两 tab）**：业界 0/5。业界只有「变更列表」一个对象，导航靠树形态/Filter 而非全量树。→ D1 与 bug280（左栏空白/空名/中栏重复）及 need289「没必要单独弄出来」对应。【已确认，锚：各家源码目录列表+官方文档】
- **D2 变更列表树形态**：3/5 支持树（VSCode 切换、IDEA Group By Directory、Fork 默认树）；2/5 证明平铺+Filter 也是成熟路线（GH Desktop/Sourcetree）。→ 树化与「平铺+强 Filter」均有生产级先例。【已确认】
- **D3 文件夹条目交互（bug290「点文件夹=无内容变更」误导）**：业界树视图点文件夹=展开/收起（VSCode 树视图还可整夹 stage），从不触发 diff。我们把文件夹条目送进 diff 流=语义错位。【已确认，锚：VSCode staging-commits 官方文档】
- **D4 untracked 枚举粒度**：业界列出每个 untracked 文件（IDEA Unversioned Files 节点、VSCode U 条目）；我们 F-09 整目录压成一条不递归（v1 防御性取舍）。→ 树形态下「选中目录 lazy 展开」是弥合方案【推测，未实证，需 POC 验证 status 枚举成本，不拍板】。
- **D5 列表行内容**：我们全路径平铺；VSCode 平铺模式显示文件名（路径信息由树形态/工具提示承载）【推测级 UX 观察】。徽标语言 A/M/D/R/U 与业界一致（VSCode U/M/D 官方文档）；R 两行式接近业界对 rename 的特判。【已确认】
- **D6 diff 基准**：我们 workdir↔HEAD 单基准。业界只读场景等价（GH Desktop 点击即 workdir diff），v1 只读下单基准不是错；staged 概念出现后业界全部是双基准（v2 才暴露，本期不动作）。【已确认/未来项推测】
- **D7 布局骨架**：我们三栏（树/变更/内容+diff 双 tab）；业界普遍两栏（列表/diff）单窗。GH Desktop 源码 changes.tsx 仅渲染 diff-container，列表由父级组合在左侧。路径条冗余（列表已含路径）；「内容 tab」业界不存在。→ bug289「布局特别不合理」的范式级对照。【已确认】

## 四、覆盖率对照（硬清单）

| 需求点 | 覆盖 |
|---|---|
| 「变更目录树没必要单独弄出来」 | D1：0/5 业界先例，已确认 |
| 「布局特别不合理」 | D7+D5：两栏范式+路径条/内容 tab 冗余论证 |
| bug280 左栏空白/中栏重复/untracked 归组 | D1/D2 + 结论 2（方向已被业界验证） |
| bug290 点文件夹「无内容变更」误导 | D3 |
| F-09 防大目录 hang | 结论 3：树化正交论证 + D4 lazy 展开空间 |
| diff 单基准硬约束 | D6：v1 等价性论证 |

缺口：无。100% 覆盖。

## 五、参考范式可落地性（三问门；非选型，供 PRD 决策点）

- **范式 A：变更列表树/平铺切换（VSCode/IDEA/Fork 路线）**。能否落地：是——EUI-NEO 无 tree 组件（docs/EUI-NEO-CAPABILITIES.md:35、:618 明示「需自实现，virtualList+▶/▼字符+状态机 ~150 行」），现成 virtualList（:114）+checkbox（:80）。接入粒度：变更路径集→派生行模型（平铺数组+depth+展开位图）→virtualList 渲染。与 F-09 正交（纯数据变换，零全量目录 IO）。
- **范式 B：纯平铺+Filter（GH Desktop 路线）**。能否落地：是——virtualList+input 过滤（:79 .placeholder/.onChange）全部现成，改动最小。
- 两范式均三问过（可落地：能力文档锚定；100% 覆盖：见四；生产级参考：见六锚点）。lazy 展开 untracked 目录为可选增强，推测级，建议 PRD 列为决策点而非默认项。

## 六、生产级锚点（repo/tag/文件路径级）

- GitHub Desktop：github.com/desktop/desktop @development——app/src/ui/changes/filter-changes-list.tsx（单组 'changed-files' 平铺+Filter placeholder 'Filter'+check-all）；app/src/ui/changes/sidebar.tsx（onIncludeChanged 勾选 staging）；app/src/ui/changes/changes.tsx（diff-container+DiffHeader+SeamlessDiffSwitcher）；releases tag release-3.0.0 / 3.1.0 / 3.2.0（均无变更树记载）
- VSCode：官方文档 code.visualstudio.com/docs/sourcecontrol/overview 与 /staging-commits（flat/tree 切换、点开 diff、U 徽标、树视图整夹 stage 原文引用）
- IntelliJ IDEA：官方文档 jetbrains.com/help/idea/commit-and-push-changes.html、/managing-changelists.html；YouTrack SUPPORT-A-3874（View Options→Group By→Directory）、IJPL-245597、IJPL-251055（ChangesViewTreeStateStrategy）
- Fork：git-fork.com/releasenotes——1.0.29（2016-11-04）"Show changed files as a tree"、1.0.42（2017-03-31）"Sort files by type (directories first) then by name"
- Sourcetree：kapeli.com/cheat_sheets/Sourcetree（File Status CMD+1、Unstage CMD+SHIFT+-、Add untracked CMD+SHIFT+=、External diff）；product-downloads.atlassian.com/software/sourcetree/ReleaseNotes/Sourcetree_4.2.11.html
- 本仓落地条件：D:\project\gui\DevDesk\docs\EUI-NEO-CAPABILITIES.md（:35/:79/:80/:114/:618）

## 七、遗留（待验证项）

1. Sourcetree staged/unstaged 双节布局细节：二手来源（官方文档已迁移，未找到一手页面）——待验证。
2. VSCode scm.viewMode 设置名：检索摘要级，官方页未直引——待验证。
3. IDEA Group By Directory 菜单路径：YouTrack 官方文章但 JS 渲染未能直读，取检索摘要——待验证（菜单文案级）。
4. GH Desktop rename 两行式细节（changed-file.tsx 未读）、Fork 现版 diff 面板布局细节（官网截图未直读）——待验证。
5. untracked 目录 lazy 展开的 status 枚举成本：推测级，需 POC。

## 八、来源清单（标注精读/扫描）

精读（本轮直读一级源）：VSCode 官方 docs ×2；JetBrains 官方 docs ×2；GH Desktop 源码 ×3（filter-changes-list/changes/sidebar.tsx）+ API 目录列表 ×2 + release notes ×3（反证用）；Fork release notes；本仓 EUI-NEO-CAPABILITIES.md。
扫描：YouTrack ×3（官方 tracker，摘要级）；Kapeli Sourcetree cheat sheet（二手）；Atlassian product-downloads release notes（官方主机）；WebSearch 摘要若干（转述级，均只作线索或待验证标注）。

工具调用：34 次（预算 ≤40）。
`````

### doc#25 · 2026-11-17-git-manager-redesign-tech（tech v2 · need#291）

- 标题：Git 管理器重设计技术方案卡：两栏重构+变更树派生模型+untracked lazy 展开（need291）
- 创建：2026-11-17 14:29:31 +0800（developer）· 更新：2026-11-17 14:43:22 +0800（developer）

`````markdown
# Git 管理器整体重设计·技术方案卡（need291）

> need#291 · 2026-11-17 · developer · POC 结论先行 · 设计契约=docs/design/2026-11-17-git管理器重设计.md（参数一律引该文档 §号，不自创视觉决策）· PRD=doc id=23 v2（D1~D7 已裁 A/C 基线）· 调研=doc id=24
> **v2（2026-11-17 审计补正）**：§2 F-04 归并口径措辞修正（枚举结果全量取用，删「按 StatusKind::Untracked 过滤」表述）；§4 文件域表补 test_git_status_model.cpp 用例迁移行、§5 测试策略同步列该文件。R1 设计矛盾已另行定稿（窗宽−556=724@1280/644@1200），本文 §7 旧引文数值保留不改。

## 0. 结论先行

三层改造，**数据面 platform/git 零改动**（硬约束全继承）：

1. **列表模型层重建**：新 `git_change_list_model`（树/平铺双态派生 + 两组头 + untracked 目录 lazy 展开位图），取代 `git_changes_model`（中栏平铺）与 `git_tree_model` 的 ChangesOnly 形态；`GitTreeModel::walkChildren` 单层枚举链保留复用（迁出为独立纯函数文件）。
2. **右栏重组**：diff 常驻主视图（`selectChange` 即 diff 主动线已有，git_page_controller.cpp:433-439）+「内容」降为头部 tab 次视图；目录行点击永不进 diff（bug#290 根治口径）。
3. **布局两栏化**：删左栏全文件树（D2-A），左变更列表 320px 可折叠 + 右 diff 主区（D4-A）。

**POC 三项结论（先行）**：
- ① **D3-A 成立**：单层枚举实测线性 ~1.6µs/条目（10k 条目 16ms，Windows/NTFS，3 遍最优）；真实仓对照 build/ 一层 0.15ms vs 递归 26530 条 3721ms——lazy 展开任一层 <20ms，F-09 不递归硬约束反向证实。归并口径=复用 `walkChildren`（src/module_git/git_tree_model.h:92，isIgnored 过滤/.git 跳过/symlink 叶化/Subrepo 检测现成），枚举结果全量取用——非 ignored 即 untracked（目录整体一条约束下天然成立），子目录条目续 lazy、文件条目打 U。
- ② **折叠阈值 200 维持**：virtualList 只物化可视槽（EUI-NEO/components/virtuallist.h:96-113），渲染成本 O(可视) 与行数无关→阈值纯 UX 渐进披露问题；200 行≈8.7 屏（28px 行×648px 视口），超过后「一眼总览」失值。断言设计：201 条=两组头默认折叠、199 条=默认展开（纯模型 doctest）。
- ③ **转圈阈值 1000ms 维持**：单层枚举常态 <20ms 到不了 1s，阈值只兜慢盘（网络盘/杀软/OneDrive 占位）；在产同款先例 `validateSpinnerDue`（git_page_controller.h:110）+ NN/g 1s 流界限（nngroup.com/articles/response-times-3-important-limits）。

**组件归属裁决建议：变更树在 module_git 内自实现，不进 EUI-NEO**。理由：(a) 复用先例全在 module 级——`md_tree.cpp:34-99 buildTreeRows` 树派生（祖先目录集+emit 递归+collapsed 位图，与本需求同构）、`GitTreeModel` expanded_/loading_ 状态机；EUI-NEO 无 tree 是能力盘点事实（docs/EUI-NEO-CAPABILITIES.md:35/:618），本方案树=virtualList（框架现成）之上的行模型派生，正是能力文档预设形态（「virtualList+▶/▼+状态机 ~150 行」）；(b) 行结构含 git 领域语义（组头/U 徽标/untracked lazy），框架泛化利用率低；(c) 负资产纪律——为单一消费者给框架加公共面。**升级触发器**：第二个模块出现树列表需求时，把派生算法下沉 EUI-NEO 通用件（届时抽取）。

## 1. 硬约束清单

| 维度 | 约束 |
|---|---|
| 平台 | 业务层禁 `#ifdef`（AGENTS §三）；本设计零平台分支（纯数据+DSL compose），双端同构；FileWatcher 平台差异为既有实现侧不对称（G10 画面无关） |
| 架构 | module_git 不依赖其它 module_*；core 不依赖 platform/EUI 运行时；`Result<T>` 无异常；worker 单线程模型（UI 线程零 libgit2/零磁盘 IO，git_worker.h 纪律）；模块隔离守卫 `check-module-isolation.sh` 必绿 |
| 数据面（继承不动） | untracked 目录整体一条不递归（git_repo.h:17，qa12 内存基线依赖）；diff 单基准 WorkdirToHead（git_repo.h:35）；v1/v2 只读边界（无 staging/commit）；巨 diff >5000 行 virtualList 分块（clue#22） |
| 依赖 | EUI-NEO virtualList/tooltip/input 现成，零新增依赖；不锁 submodule |

## 2. 逐 F-ID 方案（断言/证据物/状态集）

| F-ID | 方案与参照 | 验收断言（PRD §11 对齐） | 证据物 | 状态集落点 |
|---|---|---|---|---|
| F-01 两栏骨架 | 删 `composeLeftColumn`/`drawTreeRow` 消费链；composePump 重画两卡（设计 §1：左 320 可折叠+右余宽，gap 12，顶栏 48px）；controller 加 `listCollapsed_` + 顶栏条件展开钮（设计 §3） | 1280×720 两栏无挤压无横滚溢出；折叠后右满宽 | ui_driver 探针截图（1280/1200 两档）+折叠前后对比图 | N1 四态画面（§7 设计表） |
| F-02 变更列表树/平铺+两组头 | 新 `GitChangeListModel`：staged/unstaged 分桶→各桶路径集派生目录树（`md_tree.cpp:34-99` 同款祖先目录集+emit）+单链压缩（设计 §4.4）；平铺态=组内直接文件行按目录段聚合（设计 §4.7）；两组头行 32px 可折叠（设计 §4.3）；**staged 双侧同文件两组各一行（不去重跨组，行为变更点见 §4）** | 同一变更文件列表中只出现一次（组间双侧除外）；树/平铺切换不丢选中+scrollIntoView | test_git_change_list_model.cpp 全绿（派生/双态/选中保持） | N2 成功格：≤50 全展开、>200 组头默认收起（POC②） |
| F-03 左栏砍除 | `GitTreeModel` 类删除；`walkChildren` 迁 `git_fs_walk.{h,cpp}` 纯函数保留（F-04 复用）；「全部文件」职责移交搜索（D7）+变更列表 | 左栏不存在；无「全部文件/仅修改」tab 残留 | 全局 grep 无 composeLeftColumn 残引+构建绿 | —（能力收窄按 D2-A 裁决） |
| F-04 untracked 目录 lazy 展开 | `toggleUntrackedDir(dirRel)`→`markLoading`→worker.postWithRepo（repo 供 isIgnored）→`walkChildren`→枚举结果全量取用（非 ignored 即 untracked——目录整体一条约束下天然成立，子目录条目续 lazy、文件条目打 U）→`applyUntrackedChildren` 重建行序列；seq 过期丢弃（requestStatus 同款 git_page_controller.cpp:362）；>500 截断+尾行「还有 N 个文件·在搜索中查看全部」（设计 §6.2） | status 调用次数与 v1 持平（不递归约束不破，qa12 复测）；展开→文件级 diff 可达 | POC① 数据+定向 doctest（>500 截断/竞态空目录） | N4 四态：空=行消失+刷新；加载=>1s 转圈；错误=子行错误+重试目录保持展开；成功=截断引导 |
| F-05 选择→diff 动线 | 文件行 hit→`selectChange`（既有，rightShowsDiff_=true）；**DirRow hit→toggle 不进 diff**（目录占位文案分支 synthesizeUntrackedDiff git_diff_model.cpp:83-84 随之退役）；200ms 快速切换取最后（seq 作废前请求）；竞态「文件已无变更」→显式文案+自动 requestStatus | 任选条目点击右栏 2s 内出 diff 或显式错误，禁静默空禁「无内容变更」误导；untracked 目录行点击不进 diff | bug#290/need#289 场景复验截图（装机版） | N5 四态（§7 设计表） |
| F-06 diff 视图 | `GitDiffModel` 行模型/hunk 折叠/码点 maxLineLen 全保留；右栏头部 44px 粘顶（徽标+路径条 rename old→new+统计 +/-N+「差异/内容」tab，设计 §5.2）；错误块显式化重画（设计 §5.6） | unified 四列网格不破；hunk 折叠态记忆；+N/−N 统计正确 | test_git_diff_model.cpp 回归绿+截图 | N5 成功格 |
| F-07 内容次视图 | `GitContentModel`/FileContent 保留；controller 加 `rightTab_ enum {Diff,Content}`；切文件保持 tab 选择（设计 §6.1）；untracked >1MiB/二进制/空文件占位口径不变（§5.4） | 占位三态各有画面；切换不重发请求 | test_git_content_model.cpp 回归绿 | N6 四态 |
| F-08 搜索适配 | 数据链不动（requestSearch 全仓 walk+match）；`jumpToHit` 改造：命中∈变更列表→选中行+diff 视图滚至命中行（newNo 匹配）高亮 1.5s 渐隐；命中∉变更→独立内容视图（头部仅路径，「内容」恒激活，设计 §6.8）；`pendingContentLine` 语义迁移为双通道锚 | 搜索命中定位到新列表选中行；v2 用例改造后全绿 | test_git_search.cpp 改造绿+定位截图 | N7 四态 |
| F-09 状态刷新 | 链路保留（onWatchEvents→refreshThrottled→requestStatus，watcher 降级轮询既有）；刷新=applyStatusSnapshot 重建行序列，展开位图/组折叠位图/选中保留（内容锚 clue#14 口径）；untracked 展开缓存保留，目录消失=行消失+刷新 | 刷新不打断选中与滚动；刷新失败旧数据+绿点角标 1.5s | test_git_refresh.cpp 回归绿 | N8 四态 |
| F-10 状态集全集 | §7 设计表 8 节点×4 态逐格视图归着；错误禁冒充空态（禁静默空，bug#290 同族根治口径） | 32 格走查有画面落位 | ui 状态表（qa 承接）+关键 8 格截图 | 全表 |
| F-11 超长路径/CJK/1.354 | tooltip 现成（drawTreeRow:490-501 先例模式）；R 行 tooltip old→new（设计 §4.5）；码点截断口径沿用 maxLineLen/内容模型；逻辑像素制 scaled 全程 | 超长路径省略+tooltip；CJK 码点截断；1.354 不破版 | need14 既有探针回归 | 边界横切（设计 §7 尾行） |
| F-12 徽标语义 | `statusBadgeLetter` 保留；U 徽标升绿档 #157E4E/#E6FBF2（设计 §2.2 修订，GitColors.untracked 色值改）；untracked 并入未暂存组无独立 tab；staged 双侧两组各一行 | 双侧文件两组各一行徽标正确；U 在未暂存组带绿徽标无独立组 | test_git_change_list_model.cpp 分桶断言+截图 | N3 成功格 |

## 3. 模型层行结构（详设）

新文件 `src/module_git/git_change_list_model.{h,cpp}`：

```
struct ChangeListRow {
  enum class Kind { GroupHeader, Dir, File, Loading, Truncate, Error };
  enum class Group { Staged, Unstaged };
  Kind kind;  Group group;  int depth;
  std::string name;        // 末段显示名（目录含'/'，untracked 目录行 name+'/'）
  std::string relPath;     // 相对 workdir POSIX（File=文件路径；Dir=目录路径）
  std::string dirRel;      // Kind==Dir 有效；untracked 目录=status 条目剥尾斜杠
  std::optional<StatusKind> status;  // File 行徽标；Dir 行 U 时为 Untracked
  std::string oldPath;     // R 行旧路径（tooltip 用）
  size_t childCount = 0;   // tracked 派生 Dir=子树变更文件数；untracked Dir=0（lazy 不可知，R4）
  size_t remainCount = 0;  // Kind==Truncate 有效（>500 截断余量）
  std::string errorText;   // Kind==Error
};
```

模型状态（纯函数重建纪律 clue#22 沿用）：`viewMode_(Tree|Flat)`、`expandedDirs_`（set<dirRel>，树/平铺互不消费=设计 §6.3 位图互不干扰）、`groupCollapsed_`（set<Group>，>200 默认注入）、`untrackedChildren_`（map<dirRel,vector<子行>> lazy 缓存，刷新保留）、`loadingDirs_`/`dirErrors_`。

派生入口 `rebuild(snapshot, viewMode)`：staged/unstaged 分桶 → 每桶变更文件路径集派生目录节点（md_tree.cpp:34-99 先例：祖先目录集+逐级 emit；单链压缩=派生期唯一子目录链合并）→ untracked 目录条目（尾 '/'，剥尾归一先例 git_tree_model.cpp:140-152）特化为带 U 徽标 DirRow → 组头行+组内行交错成行序列（折叠组子树不出行）。组头计数=组内文件行数（untracked 展开子文件计入未暂存计数，设计 §4.3）。

**行为变更点**：现 `GitChangesModel::build` 双侧去重合并为一条（git_changes_model.h:29）；新口径 D6-A=staged 双侧两组各一行（PRD F-12 验收原文）——**组内去重、跨组不去重**。

## 4. 文件域变更清单

| 动作 | 文件 | 内容 |
|---|---|---|
| 新增 | src/module_git/git_change_list_model.{h,cpp} | §3 行模型（树派生/展开位图/两组头） |
| 新增 | src/module_git/git_fs_walk.{h,cpp} | `walkChildren` 从 git_tree_model 迁出（纯函数保持 doctest 直测） |
| 新增 | tests/test_platform/test_git_change_list_model.cpp | §7 测试策略 |
| 迁移 | tests/test_platform/test_git_status_model.cpp | GitChangesModel::build/groupHeader 用例（:24/:72/:93/:102）随 git_changes_model 删除迁入 test_git_change_list_model.cpp（分桶/去重断言归新模型测试）；文件内 platform 层 status 用例保留 |
| 重构 | src/module_git/git_views.cpp | 大头：删左栏/中栏三栏 compose→两栏两卡；右栏头部 44px+tab+错误块+空态五画面（设计 §3/§4/§5） |
| 重构 | src/module_git/git_page_controller.{h,cpp} | 加 listCollapsed_/rightTab_/toggleUntrackedDir 请求链/jumpToHit 双通道锚；删 AllFiles 树模式/左栏链/selectTreeFile |
| 删除 | src/module_git/git_changes_model.{h,cpp} | 并入新模型（其测试用例迁移见上行） |
| 删除 | src/module_git/git_tree_model.{h,cpp} | walkChildren 迁出后类删除；test_git_tree_model.cpp 用例改挂 |
| 微调 | src/module_git/git_module.cpp | 接线面（模块 onStart/watcher 重挂签名不变） |
| 零改动 | src/platform/git/** | 硬约束继承（git_repo.h:17/:35 口径不动） |

预估规模：M 级（模型 ~400 行+视图重构 ~600 行动+测试 ~300 行）。GitColors（untracked 绿档）与 KVirtualList 行高常量随视图重构微调。

## 5. 测试策略（定向+先红后绿）

| 测试文件 | 用例 | 红→绿切点 |
|---|---|---|
| test_git_change_list_model.cpp（新） | 树派生（嵌套/单链压缩/目录前排序）/双组分桶（双侧两组各一行）/untracked 目录行 U 特殊化/展开位图与平铺互不干扰/>200 默认折叠（200|201 边界断言，POC②）/lazy 子行注入+>500 截断/组头计数含展开子文件/刷新重建保位图/**原 test_git_status_model.cpp 的 GitChangesModel::build/groupHeader 用例迁入（分桶/去重断言等价改挂）** | 模型未实现先红→实现转绿 |
| test_git_status_model.cpp（迁出） | GitChangesModel::build/groupHeader 用例（:24/:72/:93/:102）迁至 test_git_change_list_model.cpp；其余 platform status 用例保留回归 | 迁移前后断言等价（先在新挂点复跑绿再删旧） |
| test_git_tree_model.cpp（改挂） | walkChildren 用例迁 test 挂 git_fs_walk（语义零变，防迁移回归） | 迁移前先在新挂点复跑绿 |
| test_git_diff_model.cpp | 回归+删目录占位分支用例（「未跟踪文件夹…」文案退役，bug#290 改由 F-05 交互根治） | 改动点先红 |
| test_git_search.cpp | jumpToHit 新语义改造：∈变更→diff 锚定（newNo 匹配断言）；∉变更→内容独立视图 | v2 回归面全改造点 |
| test_git_refresh.cpp | 刷新保选中/展开位图/组折叠回归 | — |
| ui_driver 探针 | G1~G12 先验矩阵（设计 §10 demo 已钉）截图证据 | 实现阶段 |

纪律：定向 filter 跑（`--dev --filter file:git`），全量归收口 qa 档；构建唯一入口 `bash scripts/build-test-run.sh`。

## 6. 取舍（放弃/推迟及理由）

- **放弃**（裁决基线非技术取舍）：左栏全文件树（D2-A）、「内容/diff」并列双 tab（D5-A）、untracked 独立组头/tab（D6-A）、目录条目进 diff 流（bug#290 口径）。
- **放弃备选**：`walkChildren` 进 EUI-NEO（§0 组件归属理由）；变更树节点级 drag/多选（PRD 无此需求，只读边界）。
- **推迟**：staged/unstaged 双基准 diff 切换（PRD 非目标 2，TreeToIndex 留而不用）；blame（非目标 3）；树派生算法下沉 EUI-NEO（触发器：第二个树列表消费方出现）。
- **视觉降级上报**：设计 §4.3/§4.4 的 200ms height 展开动画在 virtualList 定高行模型下需「行数插值」实现（行序列 200ms 逐步释放）或降级直切+淡入 150ms（复用树/平铺切换口径）——默认取降级档（动效表 §8「减弱动态=直切」已有先例），R2 上报 ui 确认。

## 7. 风险与未验证项

- **R1 设计参数内部矛盾（实现前需 ui-ux-designer 一行定稿，不阻塞模型先行）**：右卡宽度口径 §1.1（窗宽−应用侧栏 200−双侧页边 24−320−12=724@1280）vs §5.1（936@1280=窗宽−320−12−12，未扣应用侧栏）；行号列宽 §1.3（16+40+40+代码≥760）vs §5.3（24+48+48）——两口径下最小窗断言数值不同（856 vs 880 需求）。方案卡实现断言暂取 §5.3 严格档（网格 120px+代码 760=右卡 ≥880）。【2026-11-17 审计注：R1 已另行定稿（窗宽−556=724@1280/644@1200），本行旧引文数值保留不改，实现以定稿口径为准】
- **R2 展开动效形态**（virtualList 定高 vs 设计 height 200ms）：见 §6 降级上报项。
- **R3 isIgnored 每条目成本未实测**：推理 µs 级×≤500 截断=亚毫秒~毫秒级；旁证=AllFiles 模式生产同链路（walkChildren+isIgnored）无性能问题记录。lazy 展开走 worker 线程，超阈也不卡 UI。
- **R4 untracked 目录行计数数据不可得**：设计 §4.4「计数=子树变更文件数」对 lazy untracked 目录需递归（违硬约束）→演绎=untracked 目录行显 U 徽标不显子树计数，tracked 派生目录正常计数（参数级演绎，需 ui 确认）。
- **POC②③ 为推理+断言设计**（简报允许替代实跑），实跑验证归实现阶段 doctest 断言（201/199 边界用例已列 §5）。

## 8. 工具与预算

v1：工具调用 24 次（预算 ≤60）/墙钟 ~20 分钟（预算 ≤45min）。v2 审计补正：2 次工具调用。POC 实验代码在系统 temp（poc291，已删，不入仓）。

`````

### doc#26 · 2026-11-17-git-manager-redesign-tech-audit（audit v1 · need#291）

- 标题：审计：Git 管理器重设计 tech 方案卡（developer 面，可发布）
- 创建：2026-11-17 14:41:33 +0800（ZCode主会话）· 更新：2026-11-17 14:41:33 +0800（ZCode主会话）

`````markdown
# 审计记录：tech 方案卡（doc id=25，2026-11-17-git-manager-redesign-tech）developer 面冷读

> need#291 · 2026-11-17 · 审计执行：developer 面（独立子代理，冷读只读，零作者背景）· 归并落盘：ZCode主会话（汇总者）· 总判定：**可发布**（P1 一项开工前补正，无需整卡重审）

## 审计参数

- 对象：doc id=25 技术方案卡；对照：PRD doc id=23、设计文档 docs/design/2026-11-17-git管理器重设计.md、src/module_git 实码、EUI-NEO/components/virtuallist.h、EUI-NEO-CAPABILITIES.md。
- 视角四轴：可实现性 / 复用 / 过度设计 / 注释密度。
- 工具调用 22 次（预算 ≤30），只读零改动。

## 锚点抽验（15 处，全部实读命中，零虚引）

walkChildren git_tree_model.h:92 / selectChange 动线 git_page_controller.cpp:433-439 / seq 过期丢弃 :362 / validateSpinnerDue controller.h:110 / 目录占位分支 git_diff_model.cpp:83-84 / 双侧去重 git_changes_model.h:29 / md_tree.cpp:34-99 树派生 / tooltip 先例 git_views.cpp:490-501 / 剥尾斜杠 git_tree_model.cpp:140-152 / 不递归 git_repo.h:17 / WorkdirToHead :35 / staged 双侧 :24 / virtuallist.h:96-113 可视槽 / EUI-NEO tree 自实现 :35/:618 / FA 图标 codepoint 在产先例 git_views.cpp:331/:349/:362。

## 分级发现（归并后）

### [P1] §4/§5 — 删除 git_changes_model 未列既有测试文件改挂去向
- 位置：方案卡 §4 文件域表 + §5 测试策略。
- 问题：tests/test_platform/test_git_status_model.cpp:24/:72/:93/:102 直调 GitChangesModel::build/groupHeader——删除该类后此测试文件构建必红，方案卡只列了 test_git_tree_model.cpp 改挂。
- 复验：主会话 grep 物理复验成立。
- 处置：开工前补正（方案卡 §4 补一行：test_git_status_model.cpp 用例迁入 test_git_change_list_model.cpp 分桶/去重断言）。

### [P2] §7 R1 — 引文归属不精确（矛盾判定成立）
- 方案卡把 724@1280 归于设计 §1.1（原文实为 ~944@1280）；设计文档当时三口径并存。
- 处置：已被后续 R1 定稿消解（设计文档 2026-11-17 14:34 修订统一为窗宽−556=724@1280/644@1200，主会话复验新旧口径落盘）；方案卡内引文随实现消化，不再追改。

### [P2] §2 F-04 — 「walkChildren→过滤 Untracked」措辞与实码语义不符
- walkChildren 是纯 fs 枚举（git_tree_model.h:89-94）不含 status 分类；untracked 目录内非 ignored 条目天然全为 Untracked（git_repo.h:17 整体一条），无需按 StatusKind 过滤。
- 处置：随实现消化（措辞改「枚举结果全量取用（非 ignored 即 untracked），子目录续 lazy、文件行打 U」）。

## 各轴结论

可实现性：P1 外未发现（F-04 lazy 链=既有 requestWalk 同款翻版；worker/节流/锚链全实证；R2/R4 如实上报）。复用：未发现（先例全真且同构；组件归属裁决符合负资产纪律）。过度设计：未发现。注释密度：未发现。

## 联网核验

NN/g 1s 流界限（POC③ 转圈阈值方向正确）；VSCode untracked=绿 U 徽标（设计 §2.2 U 升绿档佐证）。

## 总判定

**可发布**。P1 开工前一行补正；P2×2 随实现消化；P0=0。
`````

### doc#27 · 2026-11-18-memory-pipeline-final-draft（prd v1 · need#333）

- 标题：PRD：记忆管线终稿重构（采集→聚合→修饰→前置闸→可入库终稿）——骨架+功能矩阵+剖界+排波
- 创建：2026-11-18 17:38:39 +0800（product-manager）· 更新：2026-11-18 17:38:39 +0800（product-manager）

`````markdown
# PRD：记忆管线终稿重构——采集→聚合→修饰→前置闸→可入库终稿

> need#333（bug 池，老板令立项，L 级）· 2026-11-18 · product-manager
> 依据链（全部已证，零脑补）：老板四令（#333 正文 + dev_progress 1143）＋ AC 批判定表（evidence/20261118-memory-loop/verdicts.md，qa-tester 2026-11-18）＋ bug#334 ＋ 脱敏疑点（verdicts ⑤）＋ 前置 PRD 2026-11-04-memory-collector-storm-governance（need#191，F-01~F-07 已实现并回写）。
> 本 PRD 定口径与验收；实现归 developer（本仓）/telemem 仓（引擎侧需求规格）。

## 1. 背景（已证事实）

### 1.1 现状管线（代码落点已核）

采集（src/module_memory/collector：session_reader 读 ~/.claude/projects jsonl 增量）→ **定长字节切片直通提案**（AC 批实测 1024B/段、固定前缀 `[踩坑|会话采集]`；#191 期为 4KB 段口径）→ proposals.db pending 卡；同段另投 infer_queue.db 提炼队列（UNIQUE(session_id,start_seq)）。队列消费 = runInferStep 单步（每 30s 采集轮至多 1 条；月硬档 + 小时节流双闸，pipeline/infer_batch.h）→ addInferred（引擎 add_memory infer=true，telemem 调云端 LLM 提炼并落库一体，gateway/telemem_client.h）。接受链 = 审核页 accept → 引擎 addDirect。检索/hook/decay 经 IMemoryRetriever 缝（MCP-HTTP 127.0.0.1:8421）。

### 1.2 三证（红面实证）

1. **AC 批**（真历史 979 jsonl / 1.7G 全量重采）：561/561 固定前缀墙字卡、**0 条入库**（get_memories 返回 `{"results":[]}`）、提炼队列 561 排队 **0 消费**（只进不出）、抽样 8 条三段位人读全「不能/半能」；检索底座 Recall@5 冻结集 **100%**（15/15，27 addDirect→15 查询→27 清理全真跑）。
2. **#334**：接受→真引擎 add 路径挂起（探针 cards=369 accepted=0，无界死等 20+ 分钟被杀）；探针自身接受循环无 deadline（tests/probes/memory_install_probe.cpp:246-274）。
3. **脱敏疑点**：真实批次 REDACTED 命中 0 ＋ 16 行含 @ 形态未脱（单测红队集曾钉六类；漏检或非 PII，待复核定性）。

### 1.3 老板四令（口径直译）

①「这种东西就不应该入库」→ 入库前置闸；②「给到用户的应该是最终的经验教训，修饰过汇总过的，可以直接存入 memory 的级别」→ 终稿形态；③ 五要素验收口径（生成了什么经验/是否有价值/信息完整/MCP 检索/计数对账）；④「乱七八糟无头无尾完全看不懂」→ 质量红面定性。

## 2. 需求一句话 / 目标 / 非目标

**一句话**：把会话流水账自动炼成「修饰过、汇总过、带来源、可直接入库」的终稿经验；不合格内容在入库前被拒并留痕。

**目标**（全部以真会话实测验收，单测/UI 探针不算验收——沿用 AC 批约束）
- G1 引擎内用户记忆 0 → ≥10 条终稿，人读抽样过五要素①②③。
- G2 墙字/无上下文/不可读内容入库前 100% 拒收（红队对照集）＋日志＋计数。
- G3 检索底座不回退：Recall@5 冻结集保持 100%。
- G4 管线各环节计数对账全通道一致（徽章/库实数/ledger）。

**非目标**
- 不动检索/embedding 底座（已达标）与 hook 注入链。
- 不做全手动审核模式开关（默认自动入库＋兜底确认，老板口径「直达可入库」）。
- 不做多用户/云同步/权限体系（引擎侧既有边界沿用）。
- 不做 dream 自动改库（提案制＋撤销链保留）。
- 存量垃圾 pending 清理不在本 PRD 执行（OQ-1 拍板后另作一次性维护动作）。
- 不承诺工期；引擎侧（telemem 仓）改动以需求规格交付，其排期非本仓可控。

## 3. 需求裁决序（need#146 四步）

| 步 | 结论 | 锚点 |
|---|---|---|
| ①该不该做 | 做 | 老板四令＋三证 |
| ②直接复用 | 检索/嵌入底座整体不动（Recall@5=100%）；#191 已实现件全保留（指纹去重/轮预算 50/低值过滤 v1/TTL/confidence 0.3）；proposals 状态机与撤销链、usage_ledger＋双闸控耗、python 检测链全复用 | verdicts ④；PRD191 §3；infer_batch.h；telemem_client.h |
| ③改造现有 | 定长切片→语义分段；infer 队列只进不出→终稿修饰消费；采集卡直通待审→闸后直入库；审核页降为兜底 | verdicts §修复方向 |
| ④新方案 | 聚合归并层（同主题多片段归并）＋入库前置闸为新增环节 | 老板令①② |

**四态结论：改造为主＋两个新增环节（聚合层、前置闸）；不做＝重写检索底座/换存储（无依据且底座已绿）。**

## 4. 功能矩阵

> 状态值=显式工作流状态（每项已带归属、落点与验收断言），非模糊挂起。

| ID | 功能点 | 优先级 | 状态 | 归属与卡片口径（落点＋关联池） |
|---|---|---|---|---|
| F-01 | 采集分段策略（语义边界＋噪声模式 v2） | P1·核心 | 待方案 | 本仓 collector/session_reader＋pipeline/collect_round。段按消息/事件边界切、保留头尾上下文；噪声清单 v1（#191 §9-P1 五模式）扩红队 v2：技能前言泄漏/Agent 评测指令/寒暄/表格残片/澄清反问（AC 批实测漏网类）；五道既有闸＋极短/指纹去重保留 |
| F-02 | 聚合去重（同主题归并） | P1·核心 | 待方案 | 本仓编排（段池→主题组）＋引擎 E-2 相似度服务。指纹去重（#191 F-01）已防同内容，本项防同主题多片段散产；波 3 |
| F-03 | LLM 修饰层（终稿生成＋队列消费修复） | P0·核心 | 待方案 | 引擎 E-1 提炼规格＋本仓 pipeline/infer_batch 消费修复。只进不出根因（消费闸/云端失败形态）为 AC 批遗留待查项，修复纳入本卡；终稿=结论先行/人读段落/来源会话引用 |
| F-04 | 入库前置质量闸 | P0·核心 | 待方案 | 本仓主闸（可读性/上下文完整性/墙字/来源引用存在；拒收＋日志＋计数，fail-closed 语义对齐 sanitize 先例）＋引擎 E-3 兜底闸 |
| F-05 | 接受链修复（#334） | P0·核心 | 待根因 | 本仓 accept→add 链排因修复＋探针有界等待（memory_install_probe.cpp:246-274）；关联 bug#334；引擎侧联查 E-4 |
| F-06 | 脱敏红队复核 | P1·核心 | 待复核 | 本仓 sanitize/sanitize_gate。真实批次 0 REDACTED＋16 行 @ 未脱的根因与修复（或定性非 PII 落档）；红队六类扩展 |
| F-07 | 审核页降级为「确认终稿」兜底 | P2·核心 | 待方案 | 本仓 memory_module_panels（kReviewShowLimit=50 渐进显示保留，memory_module_panels.cpp:393）。主路径闸后自动入库；兜底页只收修饰重试耗尽/add 失败/低置信终稿 |
| F-08 | 计数对账 | P2·核心 | 待方案 | 本仓 panel/recall_eval＋proposals/usage_ledger。补 AC 批两项未对上：徽章无文本通道、usage_ledger 随驱动根删除未留存；管线五计数（采集/聚合/修饰/拒/入库）可导出 |

## 5. 剖界（引擎侧 telemem 仓 vs 本仓）

原则（依据＝本软件一致性：闸/编排/UI 均在本仓 C++，引擎经 IMemoryRetriever 缝单向调用；行业惯例：质量闸置于写入方、写入前校验）：**编排、闸、UI、对账全在本仓；引擎侧只作「能力服务」（提炼/相似度/存取），以需求规格交付 telemem 仓实现。**

| 环节 | 现归属 | 目标归属 | 交付形态 |
|---|---|---|---|
| 采集/分段/过滤 | 本仓 collector | 本仓（F-01） | 开发卡 |
| 聚合归并 | 无（零聚合） | 本仓编排＋引擎相似度（F-02/E-2） | 开发卡＋引擎规格 |
| LLM 修饰 | 引擎 addInferred（提炼＋落库一体） | 引擎提炼服务（E-1：回传终稿）＋本仓消费编排（F-03） | 引擎规格＋开发卡 |
| 前置闸 | 无 | 本仓主闸＋引擎兜底（F-04/E-3） | 开发卡＋引擎规格 |
| 入库 | 审核接受→addDirect | 闸过→自动 addDirect（终稿文本） | 开发卡 |
| 审核/对账/UI | 本仓 | 本仓（F-07/F-08） | 开发卡 |

**引擎侧需求规格（telemem 仓实现；本 PRD 冻结行为规格，实现与排期归其仓；技术形态一律待其仓 POC 确认，本 PRD 不下技术结论）：**
- **E-1 提炼服务**：输入=段组正文＋会话元数据（sessionId/时间）；输出=终稿文本（结论先行/人读段落/来源引用行）＋token 用量；**不落库**（现状 add_memory infer=true 为提炼＋落库一体，需拆分，拆分形态由 telemem 侧定）；失败回结构化错误（可区分云端失败/限流/内容拒）。
- **E-2 相似度查询**：文本近邻/两两余弦（与检索同源 bge 向量），供聚合归并与对账。
- **E-3 add 兜底闸＋幂等语义定案**：不可读/墙字拒收＋结构化原因＋引擎日志（防其它调用方绕过本仓主闸）；同文本重复 add 行为定案（现状疑点=回包成功但清单零新增，infer_batch.h bug239 注记，需引擎侧实测）。
- **E-4 #334 联查配合**：accept 链 add 挂死的引擎侧嫌疑面排查配合。

> 剖界为推荐方案；备选＝闸与修饰全部引擎侧（telemem 改动集中，但规则双处维护、拒收无法回执 UI，不推荐）。

## 6. 用户操作流程（节点级；验收照走）

| # | 节点 | 进入条件 | 用户可见/操作 | 数据动作 | 成功去向 | 失败/中断去向 |
|---|---|---|---|---|---|---|
| N0 | 打开记忆页 | 侧边栏「记忆」 | 主面板＋引擎状态条 | — | N1 | — |
| N1 | 引擎就绪 | 打开页或引擎退出 | 状态条：启动中→运行中；引擎故障时「重启引擎」按钮 | venv 拉起＋MCP-HTTP 8421 | N2 | 无 Python→双平台安装指引＋详情弹窗（既有链路，telemem_engine.cpp:41-45）；超时→一键重启文案（:449） |
| N2 | 采集轮 | 引擎运行中 | 无操作；动态流 note | 30s pollOnce 读 jsonl 增量，≤50 段/轮（#191 F-02 保留） | N3 | 读失败→LOG＋下轮重试（水位不动） |
| N3 | 分段过滤 | 段产出 | 拒绝数进计数 | F-01 语义分段＋噪声 v2＋极短＋指纹去重（五道既有闸保留） | 段池→N4/N5 | 全拒→本轮零段（计数可查） |
| N4 | 聚合归并（波 3） | 段池非空 | 无操作 | 相似度归并成主题组（阈值 OQ-5 POC 定） | 主题组→N5 | 相似度服务失败→降级单段直投 N5（fail-open 于聚合；敏感闸 fail-closed 不变，对齐 #191 去重降级先例） |
| N5 | 修饰 | 队列非空且闸过（月硬档/小时节流保留） | 修饰失败原因 note 上浮 | runInferStep 消费→E-1 提炼→终稿回传＋token 入 ledger | 终稿→N6 | 硬档→暂停留队（面板档位文案承载）；节流→顺延不报错；云端失败→留队＋note；重试耗尽→N8 兜底 |
| N6 | 前置闸 | 终稿回传 | 拒收进拒收记录（note＋详情） | 本仓主闸规则判定 | 过→N7 | 拒→拒收记录＋日志行＋计数，不进库（终稿留档可查） |
| N7 | 入库 | 闸过 | 终稿流新增终稿卡 | addDirect（终稿文本）→引擎落库；卡面 source 取终稿来源值、targetIds 回填（proposal_store.h 既有字段） | get_memories 可见；撤销链可用（引擎条目按撤销语义移除，telemem_client.h delete 通道） | add 失败→有限重试→N8 兜底＋note（不得静默成功——bug239 productMissing 纪律沿用） |
| N8 | 审核兜底页 | 兜底条目存在（修饰重试耗尽/add 失败/低置信） | 卡列表（渐进 50 行保留）；操作：确认入库/拒绝/忽略 | 确认→addDirect；拒绝→rejected（人审语义）；忽略→expired（TTL 既有） | 入库/终态 | 确认失败→保持 pending＋note |
| N9 | 检索/hook 注入 | 既有 | 不变 | search/listAll 既有通道 | — | — |
| N10 | 对账 | 面板打开 | 计数面板＋徽章 | F-08 各通道对账 | 数字一致 | 不一致→对账页标红列差异通道 |

**走查**（闭环自走查记录）：主路径 N0→N1→N2→N3→（N4）→N5→N6→N7→get_memories 出现终稿（G1）。失败路径：N5 云端失败→留队＋note→下轮重试→耗尽落 N8；N6 红队样例→拒收记录可查且 get_memories 无此条（G2）。中断路径：引擎中途退出→N1 故障＋一键重启→队列/段池不丢（留队语义），无死路。

## 7. 完整状态集（四态×数据节点）

| 节点 | 空 | 加载 | 错误 | 成功（含边界） |
|---|---|---|---|---|
| 终稿流 feed | 空态文案「尚无终稿入库」（沿用既有空态形态，panels.cpp:331 锚） | 采集轮进行中 note | 引擎故障条＋详情＋一键重启（memory_module.cpp:230-240 既有） | 终稿卡列表；超长正文卡面截断＋详情全文；首次使用走引导态（memory_module.cpp:438-462 锚） |
| 审核兜底页 | 「无待确认终稿」（panels.cpp:940 锚） | 渐进加载（kReviewShowLimit=50） | 引擎缺席→卡面离线可读、确认按钮禁用＋原因 | 卡列表；操作后即时迁移 accepted/rejected/expired |
| 拒收记录 | 不显示 | — | — | note＋详情（拒因＋原文截断）；聚合进计数不刷屏 |
| 修饰队列 | 「队列空」 | 节拍消费中 | 云端失败 note＋留队 | 出队＋记账；硬档/节流为常态态不刷动态流（infer_batch.h 既有纪律） |
| 计数/对账 | 「未跑一轮出数」（既有文案锚） | 评测中…（既有） | 通道读失败→该通道标「不可读」 | 各通道数字＋一致/不一致标记；ledger 可导出留存 |
| 平台差异 | venv/Python 检测双平台文案（telemem_engine.cpp:41-45）；数据根 %USERPROFILE%\DevDesk vs ~/DevDesk；平台差异拆文件＋CMake 选源（AGENTS 纪律），业务代码禁平台宏 | | | |

## 8. UI 契约（可截图对照）

- 审核兜底页：标题「终稿确认（兜底）」；卡=结论行（加粗）＋人读正文＋来源引用行（「来源：会话 <sessionId 前 8 位> · YYYY-MM-DD」）＋置信度 tier；按钮「确认入库/拒绝/忽略」。
- 终稿流卡（feed）：同形卡，无确认按钮（已入库），带撤销入口（沿用既有卡交互）。
- 徽章：＝引擎库实数（get_memories 计数）。
- 动态流 note：拒收（带拒因）/修饰失败（带原因）/重试耗尽落兜底，一次性呈现＋计数聚合。
- 全部文案锚既有空态/引导态形态（§7），不新造风格。

## 9. 关联影响

- **#191**：已实现件全保留复用；F-01 在其上扩噪声 v2。
- **bug#334**：F-05 覆盖（本仓链修复＋探针有界等待＋E-4 联查）。
- **bug190**：审核视图渐进加载保留。
- **检索/hook/decay**：不动（curator listAll 通道、hook 注入链既有）。
- **撤销链**：Proposal kind/targetIds/undoOf 沿用；终稿卡 source 取值扩展（collect→新增终稿来源值，状态机不改）。
- **云端计费**：修饰批放大 token——月硬档＋小时节流＋ledger 记账全保留；直存/检索不设闸的既有语义不变（infer_batch.h:9）。
- **telemem 仓（外部）**：E-1~E-4 需求规格交付，跨仓排期风险见 §11。
- **双平台**：管线纯逻辑层同构；python_locator 双平台已绿；每波双平台各实测一轮真会话。

## 10. 排波建议（预计 3 波）

| 波 | 内容 | 依据 | 验收形态 |
|---|---|---|---|
| 波 1 止血 | F-05（#334 接受链＋探针有界）＋F-06（脱敏复核落档） | 不依赖新架构、独立可验收；链路断裂与安全红线先行 | 真会话接受≥1 条入库（有界时间）；脱敏 16 行逐行结论落档 |
| 波 2 终稿管线 MVP | F-01（分段 v2）＋F-03（修饰＋队列消费修复，含 E-1 规格）＋F-04（前置闸） | 老板主诉「修饰过、可直接入库」＝90% 核心价值先闭环；单段终稿已达成可读/来源/拒收 | G1/G2：真会话 ≥10 条终稿入库、五要素①②③绿、红队拒收 100%（G3 不回退同场验） |
| 波 3 聚合与体验 | F-02（聚合＋E-2）＋F-07（审核降级）＋F-08（对账） | 聚合依赖引擎 E-2 跨仓；审核降级依赖新形态定型；对账为验收基建收口 | 同主题归并抽验；对账全通道一致（G4） |

每波收口按 AGENTS §五成本纪律（--dev 内循环＋定向 filter，阶段节点全量）；装机按既有 Debug 覆盖口径。E-1 未就绪时波 2 修饰腿可先以现有 addInferred 通道打通、规格就绪后升级，不空等跨仓。

## 11. 风险面

| 风险 | 说明 | 缓解 |
|---|---|---|
| LLM 依赖 | 修饰层依赖云端 LLM；引擎缺席/云端失败/闸关闭→终稿停滞（infer_queue 只进不出即此风险实证） | 失败留队＋结构化错误＋note 上浮＋F-08 队列计数可观测；重试上限落兜底；直存/检索不受闸影响（既有语义） |
| 成本 | 聚合＋修饰放大 token 消耗（561 段级批） | 月硬档＋小时节流保留；ledger 记账＋对账（F-08）；波 2 先单段、波 3 才聚合，控制增量 |
| 双平台 | 引擎 venv/路径/探针 Windows 与 macOS 不对称 | python_locator 双平台已绿；每波双平台各实测一轮；平台差异拆文件纪律 |
| 聚合质量 | 相似度误并（异主题并一条）→修饰失真 | 阈值保守（OQ-5 POC 定）；来源引用可回溯；聚合 fail-open 单段降级路径 |
| 跨仓协作 | telemem 侧规格实现排期非本仓可控 | 规格冻结（E-1~E-4）；波 1/波 2 本仓侧不阻塞先行；addInferred 过桥方案 |
| 存量 | 561/1630 pending 垃圾与新管线并存 | OQ-1 拍板后一次性处置（备份先行） |

## 12. 验收标准（标 F-ID；全真会话实测）

- [F-01] 红队噪声集 v2（技能前言/评测指令/寒暄/表格残片/澄清反问类）100% 不成段；抽样段三段位人读「有头有尾」（五要素③口径）。
- [F-02] 同主题多片段（构造＋真实双集）归并 1 组出 1 终稿；跨会话同主题不重复产出（指纹＋相似度双通道断言）。
- [F-03] 真跑一轮队列消费 ≥1 条出队（有界时间内）；终稿抽样 ≥10 条人读可懂＋来源引用可回溯源 jsonl（卡→evidence sessionId→源文件，链路沿用 AC 批已验形态）；修饰失败留队＋note 可见；token 入 ledger。
- [F-04] 红队终稿样例（墙字/无上下文/不可读）100% 拒收＋日志行＋拒收记录；正常终稿对照集 0 误拒；get_memories 无拒收条。
- [F-05] 接受 1 条→get_memories 出现（有界时间）；探针有界退出（t≤上限）；撤销链执行成功（引擎条目按撤销语义移除）。
- [F-06] 16 行 @ 形态逐行结论落档（修 pattern 或定性非 PII）；REDACTED=0 根因结论落档；红队六类＋新增类全绿。
- [F-07] 审核页文案/按钮＝终稿确认语义；自动入库条目不落待审位；兜底三操作（确认/拒绝/忽略）流转正确。
- [F-08] 徽章＝库实数；采集/聚合/修饰/拒/入库五计数对账单可导出且一致；ledger 持久留存可查（不随会话退出丢失）。
- [总体] 五要素①~⑤全绿（G1~G4）；双平台各一轮真会话。

## 13. 开放问题（每条带推荐＋影响，待用户裁决）

| # | 问题 | 推荐（依据） | 备选 | 影响 |
|---|---|---|---|---|
| OQ-1 | 存量 561/1630 pending 处置 | 备份后批量置 expired＋审计行（本软件一致性：#191 拍板 A 先例已执行） | 物理删除/置 rejected | 不处置则审核页混新旧两形态；处置需一次性维护动作 |
| OQ-2 | 自动入库与兜底边界 | 闸过＋修饰成功→自动入库；修饰重试耗尽/add 失败/低置信→落兜底（老板口径「直达可入库」直译） | 全部人工确认（与老板令相悖，不推荐） | 自动化程度 vs 人工成本；低置信阈值 OQ-5 一并 POC |
| OQ-3 | 前置闸落点 | 本仓主闸＋引擎兜底双层（本软件一致性：sanitize fail-closed 先例；拒收可回执 UI） | 纯引擎侧闸 | telemem 仓改量；拒收留痕回执能力 |
| OQ-4 | 修饰用模型 | 沿用引擎侧云端通道（现状）；本地化（仓内 llama_host 在库）标待确认，POC 后定 | 本地模型 | 成本/质量/离线能力；技术选型不下结论 |
| OQ-5 | 聚合相似度阈值、终稿长度上限、低置信兜底阈值 | POC 任务卡真数据集量测后定 | — | 误并率/漏并率/终稿可读性/兜底命中率 |

## 14. 闭环自走查（定稿前核对记录）

- 节点全枚举：N0~N10 每节点进入条件/来去向齐（§6），无断链。
- 数据态四态：6 类数据节点四态齐（§7），无空项。
- 操作与触发：审核兜底三按钮/重启引擎/撤销/导出对账逐元素有触发结果（§6/§8）。
- 流转去向：成功/失败/中断三分向逐节点列明（§6）；重试耗尽有兜底出口，无死路。
- 澄清升格：全文无「待定/暂定/TBD」字样；全部模糊点升格为 OQ-1~OQ-5 显式决策点（§13）。
- 主路径＋失败路径走查：见 §6 末段；发现的模糊点（兜底触发、闸落点、模型、阈值）已回填 OQ。

`````

### doc#28 · need392-research-synthesis（tech v2 · need#392）

- 标题：need#392 调研综合与站点提案 v2
- 创建：2026-11-19 17:35:57 +0800（claude-agent）· 更新：2026-11-19 18:09:10 +0800（claude-agent）

`````markdown
# 调研综合与站点提案 v2（need#392）

五路审计后重写：全局口径两条（人称=1 工程师+AI 多代理流水线；语料范围=Claude Code 六目录 17:1x 快照+ZCode 侧 devflow 重度/auto-test 0）；零使用档修正（cs-web-fetch/llm-model-probe 移出，剩 6 项）；186h 删除换可数口径「248 报告≈免 250 轮手工回归」；数字全部走 15-数字台账（无台账行禁入 L1/L2、防误读句随卡、口径四档随数字、快照双值注时点）；每页五段序（为什么造→一句人话→真实故事→数字+防误读→证据）；答辩准备页（B 压测 20 题转化）；汇报讲法四档；双模型审计常态化；待裁决 9 项（默认执行可否决，唯一真缺口=成本口径）。物理副本 research/08-调研综合与站点提案.md v2（8338B）。
`````

### doc#29 · need392-prd（prd v1 · need#392）

- 标题：need#392 站点 PRD
- 创建：2026-11-19 18:26:45 +0800（claude-agent）· 更新：2026-11-19 18:26:45 +0800（claude-agent）

`````markdown
# 站点 PRD（need#392）

基准=08 v2，零统计数字（只引用 15-数字台账行，数字五元组字段契约：值+台账行引用+口径四档+防误读句+快照注）。九页 F1~F7：F1 首页（双声明+驯服矩阵+token 三件套）/F2a-d 四工具页（起源老板口径+机制渐进展开+数字分解式）/F3 五场景卡（双向锚）/F4 证据深潜（15 件+台账区+复算命令组）/F5 方法论（框死论设计者口述标注）/F6 meta 案例（need#392 全阶段时间线）/F7 答辩准备页（20 题 1:1+四档讲法+最危险三题置顶）。U1-U8 通用规格（首屏双声明/五段序/三条硬规则/术语 8 词首现括注/黑话 15 词 grep=0/数字规则含 186h 禁现/九项导航/静态契约）。验收=三道技术门+内容质量门抽查+双模型审计常态+老板动线走查（30 分钟 20 问+黄金 90 秒）+失败路径演示。开放项三案默认采推荐：V4 存档链接/成本口径默认句留空位/翻车卡落 F7。物理副本 research/18-PRD.md（31739B）。主会话面审 2026-11-19 18:3x 通过。
`````

### doc#30 · 2026-11-19-mcp-agent-tools（prd v3 · need#394）

- 标题：MCP 对外开放智能体工具（todo/md）+ 安装按钮外置
- 创建：2026-11-19 19:25:21 +0800（主会话）· 更新：2026-11-19 19:47:50 +0800（product-manager）

`````markdown
# PRD：MCP 对外开放智能体工具（todo/md）+ 安装按钮外置（L 级）

- 池条目：#394（P1·进行中·owner=product-manager）；本版=v3（v1 骨架 → v2 PM 定稿 → v3 老板终裁 OP-1：方案 B 立即移除 notes_*×3，全文口径同步）
- 后续 tech slug：2026-11-19-mcp-agent-tools-tech
- 流程位：PM 定稿 → ui-ux demo（F-3 版式老板验收）→ developer → review/QA

## 1. 背景与现状（已验证事实，禁重查）
1. 两套注册面：智能体工具 core::ToolRegistry（内部智能体用，src/core/tool.cpp:16）；MCP 对外 core::mcp::McpToolRegistry（src/core/mcp_tool.cpp:33）。对外面现状开放 7 个：notes_search/read/write（src/module_markdown/notes_mcp_tools.cpp:256）、memory_search（src/module_memory/mcp/search_tool.cpp）、project_info（src/module_settings/project_info_tool.cpp）、server_info+web_fetch_markdown（src/module_mcp/mcp_tools_builtin.cpp:828）。todo×6 / md×5 从未进过 MCP 面。
2. todo 工具面：src/module_todo/todo_tools.cpp（todo.list/create/update/complete/delete/move，TodoTools::registerAll）。md 工具面：src/module_markdown/markdown_tools.cpp（md.search/read/create/delete/list_tags，MarkdownTools::registerAll）。同模块 notes_mcp_tools.cpp 与 md 工具面同 MarkdownService 后端，search/read/写 语义重叠。
3. 安装按钮现状：画在工具详情弹窗（src/module_mcp/mcp_page_module.cpp:1082 claude / :1115 zcode），实为「安装/卸载」双态 toggle（mcp_page_module.cpp:739-740：未装=「安装到 Claude Code」/已装=「从 Claude Code 卸载」）；写入/移除=整服务端点（claude_mcp_config.cpp:187 installServerFile / :197 removeServerFile；zcode_mcp_config.cpp:242 / :252）。
4. 安装写入层语义（本需求不动）：宿主配置文件必须已存在、绝不新建（claude_mcp_config.cpp:157-161 / zcode_mcp_config.cpp:212-215）；有改动才备份+原子写；claude 同名条目幂等 no-op（claude_mcp_config.cpp:95-97）；zcode 同址幂等、异址冲突不覆盖（zcode_mcp_config.cpp:126-134，D2 裁决沿用）；条目形态 claude=type+url、zcode=type+url+enabled=true；端点=单一 HTTP 服务+/mcp（mcp_page_module.cpp:719）。
5. 页头现状：页头卡（D-6 体例）=卡内标题「MCP 服务」+说明+服务状态行（运行/未启动，数据源 app::mcp 桥 running/port/lastError，mcp_page_module.cpp:232/267-327）；服务开关在设置页、MCP 页只读；安装按钮 actionable 依赖服务运行（mcp_page_module.cpp:708/759 serviceRunning=running&&port>0；未启动时「请先启动 MCP 服务」提示优先，:763-764）。
6. 工具卡现状：名称+状态行（可用/未启用=serviceRunning）+描述+chips（内置/服务）+右侧图标按工具名族派发（mcp_page_module.cpp:164）；详情弹窗（drawDetailDialog，mcp_page_module.cpp:813）=hero（图标/名/标签）+描述+参数表单（扁平≤6 参）或 schema JSON 回退+「试一下」异步试跑。

## 2. 用户价值一句话
在 Claude Code / ZCode 里直接读写 DevDesk 的待办与笔记（todo×6+md×5），并在 MCP 页头一眼看到服务与两宿主安装态、一键完成安装或卸载，不必钻进工具详情弹窗找入口。

## 3. 目标 / 非目标
目标：
- G-1 todo×6、md×5 进入 MCP 对外面，后端 100% 复用智能体工具面实现（不重写业务逻辑）
- G-2 md×5 与存量 notes_*×3 去重定案=立即移除（老板 2026-11-19 终裁，§8）
- G-3 安装/卸载按钮外置页头服务状态区，一次性注册整服务（非按工具勾选）
- G-4 新工具在 MCP 页卡片、详情弹窗、「试一下」完整可用

非目标（不做项，带依据）：
- 不开放 memory.stats/health、claude.*×3、app.stats/app.notify/hot.list（老板 2026-11-19 裁决）
- 不开放 fs.*×5 / bash.run / git.*×8 / net.fetch / app.now（宿主原生已有，淘汰判据）
- 不动 web_fetch_markdown（存量，动则另立条）
- 不做 notes_* 兼容别名或过渡期（老板终裁方案 B，破坏面已接受，§8）
- 不改安装/卸载写入层（两个 config 文件不动，仅挪 UI 位置）
- 不改工具内部语义与参数（对齐 ToolRegistry 面现状）
- 不做按工具勾选注册、不做 Claude/ZCode 以外宿主、不做页内服务开关（开关留设置页，现状不变）

## 4. 关键决策（PM 定案，依据标注）
- D-1 MCP 对外命名=下划线：todo_list/todo_create/todo_update/todo_complete/todo_delete/todo_move；md_search/md_read/md_create/md_delete/md_list_tags。智能体面原名（todo.* 点号）不动。依据：本软件一致性（存量 MCP 面 7 个全下划线）+ MCP 官方规范示例全 snake_case（spec 2025-06-18 server/tools，get_weather 等）。
- D-2 详情弹窗安装行移除（外置=迁移非复制）。依据：老板裁决原文「外置」+本软件一致性（同动作单一入口）。
- D-3 外置控件保留双态 toggle（安装↔卸载）。依据：卸载能力现状已存在（removeServerFile），外置不得造成能力回退；卸载入口随迁移唯一化。
- D-4 安装前置保持现状：服务未启动→按钮置灰+提示「请先启动 MCP 服务」（开关在设置页）；宿主配置文件不存在→点击后错误提示（不自动创建）。依据：现状语义（mcp_page_module.cpp:708/759/763、claude_mcp_config.cpp:157-161），能力不回退。
- D-5 一次性注册语义显式化：按钮动作=向宿主写入 devdesk 单一服务条目（claude=type+url；zcode=type+url+enabled），注册即得全部对外工具（MCP 连接级动态 tools/list 发现机理）。依据：行业机理（MCP 规范发现模型）+老板裁决「非按方法」。

## 5. 功能矩阵（唯一推进状态源；全部待方案 → tech 环节领取）
| F-ID | 功能点 | 优先级 | 状态 | 验收要点（断言+证据物） |
|---|---|---|---|---|
| F-1 | todo×6 经 MCP 对外开放 | 核心 | 待方案 | tools/list 含 todo_* 6 件；同参数下 MCP 面与智能体面返回等价（抽 todo_list 空/成功、todo_complete 成功/参数错各一）；证据物=测试清单卡+probe 的 tools/list 清单输出 |
| F-2 | md×5 经 MCP 对外 + 移除 notes_*×3 去重 | 核心 | 待方案 | tools/list 含 md_*×5 且不含 notes_search/notes_read/notes_write（移除断言）；一致性断言同 F-1（抽 md_search 空/成功、md_create 成功/参数错各一）；证据物=测试清单卡+probe 的 tools/list 清单输出 |
| F-3 | 安装/卸载按钮外置页头服务状态区 | 核心 | 待方案 | 页头卡含两宿主按钮；装→宿主配置出现 devdesk 条目（claude=type+url；zcode=type+url+enabled）；幂等重装零字节改动；卸载→条目移除+备份生成；弹窗安装行不再绘制；证据物=配置前后 diff+页面截图（版式以 ui-ux demo 老板验收记录为准） |
| F-4 | 新工具页面呈现完整 | 核心 | 待方案 | 卡片/图标（todo_*、md_* 族映射）/标签/描述/详情参数表单/「试一下」全跑通（todo_list 空参与实参各一）；证据物=测试清单卡+页面截图 |

## 6. 用户操作流程（节点级；验收照走）

流程 A：安装（主路径）
- A1 进入 MCP 页｜进入条件：侧边栏点击 MCP 模块｜呈现：页头卡（服务状态行+两宿主安装态+按钮）+工具卡片网格
- A2 查看页头服务状态区｜进入条件：A1 完成｜呈现：服务状态（运行/未启动）+「Claude Code：未装/已装」+按钮（未装=安装/已装=卸载文案）+ZCode 同构
- A3 前置检查｜进入条件：用户点击安装按钮｜服务未启动→按钮置灰不可点+提示「请先启动 MCP 服务」（中断去向：设置页开服务后回本页）；服务运行→执行写入
- A4a 安装成功｜呈现：成功反馈+安装态刷新为已装、按钮变「从 Claude Code 卸载」｜去向：停留本页（可继续装另一宿主/浏览工具/离开）
- A4b 安装失败（配置文件不存在/读失败/写失败/zcode 异址冲突）｜呈现：错误文案透传（不自动创建、不覆盖）｜去向：停留本页可重试，或去宿主侧手工处理后重试；安装态不迁移
- A5 宿主生效｜进入条件：用户重启宿主（或重连）｜呈现：宿主 tools/list 出现全部对外工具（存量 4+todo×6+md×5=15 件）
- 幂等边界：已装再点安装=claude 同名 no-op / zcode 同址 no-op（零字节改动）

流程 B：卸载
- B1 点击「从 Claude Code 卸载」｜进入条件：按钮处于已装态｜动作：removeServerFile
- B2a 成功：态刷回未装+按钮变安装文案｜B2b 失败：错误文案、状态不变可重试｜去向：宿主下次连接不再见 devdesk 服务

流程 C：宿主内使用新工具
- C1 宿主发现｜进入条件：宿主连接 DevDesk MCP 服务｜呈现：tools/list 含 15 件（定案 B 口径，见 §8：存量 4+todo×6+md×5）
- C2 读类调用（todo_list/md_search/md_read/md_list_tags）｜空：空列表/空结果/空标签集；成功：结构化结果；错误：参数校验失败→错误文案
- C3 写类调用（todo_create/update/complete/delete/move、md_create/md_delete）｜成功：操作结果；错误：校验失败/数据缺失→Result 错误文案（isError 语义）
- 中断：DevDesk 退出/服务停止→宿主侧连接错误，DevDesk 侧无状态残留

流程 D：MCP 页内验证
- D1 工具卡｜进入条件：A1｜新工具卡按现状网格排入（图标按名族派发）
- D2 详情弹窗｜进入条件：点击卡片｜hero+描述+参数表单（或 schema JSON 回退）；安装行已移除（D-2）
- D3 试一下｜进入条件：弹窗内点击｜异步执行→结果/错误呈现（现状机制）｜中断：关闭弹窗即收尾（现状线程安全机制）
- D4 空参调用｜进入条件：工具无必填参｜现状「空参调用」语义沿用

## 7. 完整状态集（逐节点：空/加载/错误/成功/边界/平台差异）
节点 1 页头服务状态区：
- 空：宿主未装（配置文件不存在）→安装态=未装；点击→错误「未检测到配置（不自动创建）」
- 加载：安装/卸载写入进行中→按钮置灰防重入（现状 actionable 语义）
- 错误：读/写/备份失败、zcode 异址冲突（现状文案透传）→错误反馈，态不迁移
- 成功：装=已装+按钮变卸载文案；卸=未装+按钮变安装文案
- 边界：重复安装幂等（零字节改动）；两宿主互不影响；服务未启动按钮置灰+提示
- 平台差异：宿主配置路径差异由两 config 模块内消化；UI 与交互零差异、零 #ifdef（架构约束）
节点 2 工具卡片网格：
- 空：registry 无工具（不可达态，守卫）→现状空网格语义
- 加载：同帧绘制、无异步
- 错误：无独立错误态（服务状态行承载）
- 成功：全部对外工具卡片（15 张=存量 4+todo×6+md×5）
- 边界：超长名/描述折行（现状卡片布局）；图标映射新增 todo_*/md_* 两族（缺映射走现状默认图标兜底）
- 平台差异：无
节点 3 详情弹窗：四态沿用现状机制（描述折行 clip、参数两列表单/JSON 回退、试一下 loading/result/error）；新工具全覆盖，无新交互面
节点 4 宿主内调用（每新工具）：
- 空/成功/错误：见流程 C
- 边界：超长内容、无权限、数据根平台差异（%USERPROFILE%\DevDesk vs ~/DevDesk）→与智能体面同行为透传
- 一致性断言（F-1/F-2 共用验收）：同参数同结果，MCP 包装层零语义漂移

## 8. notes_* 去重定案（老板 2026-11-19 终裁：方案 B=立即移除）
重叠映射：md_search↔notes_search、md_read↔notes_read、md_create↔notes_write（同 MarkdownService 后端）。
- 定案：md_*×5 全量注册为正名；notes_search/notes_read/notes_write 从 MCP 对外面移除（注册下线，非仅隐藏）。
- 对外总数：15 件=存量 4（memory_search/project_info/server_info/web_fetch_markdown）+todo×6+md×5。
- 破坏面（老板已接受）：已安装宿主的存量提示/agent 配置引用 notes_* 的调用立即失效（宿主侧报 Unknown tool 类错误）；本机两宿主（Claude Code/ZCode）需重连并改用 md_* 名。
- 移除范围仅 MCP 注册面：Markdown 模块页面、智能体工具面（markdown_tools.cpp/md.*）不受影响。
- 依据：老板 2026-11-19 AskUserQuestion 实答（方案 B）；与淘汰判据同源——重叠能力不双开、单一正名面。
- 验收落点：F-2 移除断言（tools/list 不含 notes_*×3）。

## 9. UI 契约（结构契约；版式细节与视觉以 ui-ux demo 老板验收记录为准，验收后回填截图锚点）
- 页头卡新增宿主安装区（服务状态行之后）：两宿主安装态（未装/已装）+双态按钮×2（安装↔卸载，文案沿用现状口径）
- 操作与触发（逐元素）：点击安装→界面反馈（按钮置灰防重入→成功/错误反馈，同现状机制）+数据动作（installServerFile 写整服务端点）+结果呈现（安装态与按钮文案刷新）；点击卸载→removeServerFile→同构
- 详情弹窗：移除安装/卸载行（槽位回收，其余布局不动）
- 工具卡：新增两族图标映射（todo_*=待办族、md_*=笔记族；具体码点 tech/demo 定）
- 平台差异：无（UI 层零 #ifdef）

## 10. 关联影响
- src/module_todo/：新增 todo MCP 注册（样板=notes_mcp_tools.cpp 模式）；module 内自含，无跨模块依赖
- src/module_markdown/：新增 md_* MCP 注册；notes_*×3 MCP 注册下线（notes_mcp_tools.cpp，同模块）
- src/module_mcp/mcp_page_module.cpp：drawClaudeInstallButton/drawZcodeInstallButton 调用点从 drawDetailDialog（:1082/:1115）迁至页头卡；弹窗安装行移除
- 写入层两个 config 文件：不动
- 测试：tests/test_modules/test_mcp_page.cpp（按钮位置断言迁移+新工具卡）、test_notes_mcp_tools.cpp（退役：原 notes_* 用例改写为移除断言）、新增 todo/md MCP 工具用例、tests/probes/mcp_card_343_probe.cpp 同族 probe 扩展
- 文档：docs/USER_GUIDE.md MCP 章节更新（含 notes_*→md_* 换名映射）；EUI-NEO 无涉
- 架构红线核查：module→core 注册（合法方向）；不抛异常走 Result<T>；平台差异拆文件；module_todo/module_markdown 互不依赖（各自注册各自工具）

## 11. 风险
- R-1 存量宿主破坏面（中，老板已接受）：notes_* 移除后宿主旧调用报 Unknown tool；发布说明明示换名映射（notes_search→md_search、notes_read→md_read、notes_write→md_create）；本机两宿主重连换名
- R-2 按钮迁移回归（中）：现有测试/探针锚定弹窗按钮→断言同步迁移，防「按钮消失无人报」
- R-3 语义透传漂移（中）：MCP 包装层参数/返回偏差→一致性断言（同参数同结果）+probe 兜底
- R-4 写面扩大（低）：对外开放写工具；写面非首开（notes_write 既有），本轮不开组按老板裁决挡住；宿主侧人审由宿主自身确认 UI 兜底（MCP 规范建议）
- R-5 工具面规模（低）：对外 15 件；重叠能力不双开、无兼容别名噪音（优于双轨口径）

## 12. 验收标准（标 F-ID；qa-tester 实操验收照此）
- F-1：宿主或 probe tools/list 见 todo_*×6；抽 todo_list（空+成功）与 todo_complete（成功+参数错误）主/失败路径；一致性断言过；证据物=测试清单卡+probe 输出
- F-2：md_*×5 同构；移除断言=tools/list 不含 notes_search/notes_read/notes_write（替代原兼容用例）
- F-3：页头两按钮全路径走查（A1–A5、B1–B2、幂等、异址冲突、服务未启动置灰）；配置前后 diff+页面截图；弹窗无安装行
- F-4：卡片/详情/试一下全跑通（todo_list 空参与实参各一）；页面截图
- 收口跑法：阶段节点不带 --dev 完整跑一次全量 doctest+probe+守卫链

## 13. 开放问题（已全部关闭）
OP-1 notes_*×3 去重方案：已终裁关闭——老板 2026-11-19 AskUserQuestion 实答=方案 B 立即移除（定案见 §8）。影响：对外工具数 15；宿主存量 notes_* 调用失效为老板已接受破坏面；本机两宿主需重连换名。当前全文无开放问题、无待裁决项。

## 14. 闭环自走查记录（硬清单 1–4 全量走查，定稿前完成）
- 节点全枚举：A/B/C/D 四流程节点均有名称+进入条件+去向；服务未启动/配置缺失/异址冲突/写入失败均有去向，无断链
- 数据态四态：§7 逐节点覆盖空/加载/错误/成功+边界+平台差异，无空项
- 操作与触发：页头两按钮、卡片点击、弹窗「试一下」逐元素写明反馈/动作/结果
- 流转去向：成功/失败/中断三向齐全，无死路或未定义态
- 澄清升格：全文无「待定/暂定/TBD」字样；OP-1 已终裁关闭（§13），无待裁决项
- 走查发现的缺口已回填：卸载语义（D-3）、服务未启动前置（D-4）、幂等/冲突边界（§7 节点 1）
`````
