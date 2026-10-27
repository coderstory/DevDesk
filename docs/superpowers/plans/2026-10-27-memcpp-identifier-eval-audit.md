# 标识符与 2 字中文召回评测审计（bed0071e，阶段 2 关卡）

- 日期：2026-10-27；审计对象：bed0071e（6 新文件：eval_identifiers/ 5 文件 + tests/test_eval_identifiers.py）
- 独立性：审计者未参与该提交实现，非实现者延续会话，接单有效。
- 实证动作：全量测试实跑、两组变异实验（临时副本，仓库零污染）、5 对查询/gold 抽查 + 64 条全量字面重叠统计。

## 一、正确性 — 有 finding
- 已核：分词契约真复用。eval_identifiers.py:39-52 以 importlib 加载 experiments/eval_retrieval.py 的 bigram_tokens/ranked_ids/fts_match_expr/first_hit_rank，非自写平行实现。
- 已核：FTS 形态同构。eval_identifiers.py:108-114 的 fts_bigram = fts5(unicode61) + 入库前双字空格连接，与冻结 eval_retrieval.py:157 及 P0 规格书 docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md:104,125,173（tokens 列预分词 + unicode61 虚表 + bm25）同构；数字不因简化偏乐观。
- [F1] eval_identifiers.py:86-96,73-83 vs 规格书:195 —— 兜底臂与 P0 规格书不同构：规格书定"FTS 0 行才触发、LIKE 各 token OR、按命中 token 数降序"；评测实现是"FTS <5 行即触发、全 token AND、按 rowid 排"。影响：1.0 不能作为规格书兜底设计的有效性证据，P0 拍板引用时口径漂移（维护代价：决策依据与规格两套语义，后续实现者不知信哪个）。建议：补跑规格书语义一臂，或在 docstring/results.md 标注差异点。
- [F2] eval_identifiers.py:55-66 —— doc_text 复制了 eval_retrieval.py:138-150 的入库文本拼装（未随契约 importlib 复用）。影响：冻结侧入库形态一变，两处漂移，评测测的不再是同一入库文本。建议：提为 eval_retrieval 模块级函数后借用，或注明复制品来源行号。

## 二、回归面 — 已核，未发现问题
- git show bed0071e --stat：6 文件纯新增（6146 insertions / 0 deletions）；experiments/、sanitize/、既有 eval 脚本与测试零触碰。
- 实跑 `python -m unittest discover -s tests`：Ran 124 tests, OK（5.7s），"全量 124 OK"声称属实。

## 三、数据集诚实性 — 最重 finding
- 抽查 5 对：camel "getUserById"=gold 标题逐字子串；path "src/core/sqlite_db.cpp"=gold 正文逐字子串；snake "sqlite_db.cpp 的建表逻辑"=文件名逐字+功能词；error_code "E_PROXY_401 是什么错误"=错误码逐字在文；zh_short "代理 超时"=两 token 均逐字在 gold 正文。
- 程序化统计：21/64 查询串是 gold 文本逐字子串、23/64 全 token 在 gold 文本中、0/64 是同义改写或跨表述查询——全部是"送分题"，1.0 确为上限结论。
- [F3] results/results.md:1-19 —— 整份报告无任何局限声明；冻结版 eval_retrieval 的报告明确写"查询由目标记忆反推生成……不能作为质量结论"（eval_retrieval.py:313 生成口径），本报告把该句丢失；实现者自报的局限（同源构造、1.0=上限）只存在于派单书，commit message 单行、results.md、README 均未落字。影响：1.0 会被后续读者当成质量结论而非机制跑通上限，误导 P0 拍板（演进代价最高）。建议：报告模板加固定声明"数据集同源构造、1.0 为上限、不能作为质量结论"，与冻结版口径对齐。

## 四、静默失败面 — 已核，未发现问题
- SQLite 不可用→connect 抛异常；FTS5 缺失→CREATE VIRTUAL TABLE 抛异常；均响亮非零退出。
- 空结果集：token 为空→fts_match_expr("")→ranked_ids 返回 []（eval_retrieval.py:78-79,101-103），计为可见 miss；per_query 与 collect_failures（eval_identifiers.py:158-169,173-208）保留 rank=None 明细，不会静默 0 分混入。
- 写盘失败→write_text 抛异常→非零退出；main 仅成功路径 return 0（eval_identifiers.py:267-297）。

## 五、测试有效性（变异实证）— 最重 finding
- 变异A（兜底永不触发，eval_identifiers.py:90 改 if False）：实跑 124 tests OK，零红灯。原因：纯 bigram 臂本数据集已全类 1.0，test_fallback_not_worse_than_bigram_only（tests/test_eval_identifiers.py:88-95）断言 1.0>=1.0 恒真（单调性只防"兜底有害"，不防"兜底死亡"）；like_only 臂无任何断言；兜底路径零直接测试。结论：兜底代码整体删除，评测结论不变。
- 变异B（冻结 bigram_tokens 中文改单字切分）：实跑 124 tests OK，零红灯。原因：门槛测试只锁 IDENTIFIER_CATEGORIES（全 ASCII，tests/test_eval_identifiers.py:79-86），zh_short 类别无门槛断言。结论：分词契约断裂评测照样亮绿。
- 恒真断言排查：11 条新测试无字面恒真；test_sqlite_reported(:107-109)、test_result_files_written(:103-105) 偏弱（后者被 setUpClass 抛错间接覆盖）但不恒真；test_main_returns_zero(:76-77) 真跑 mod.main() 非 mock。
- [F4] tests/test_eval_identifiers.py:88-95 —— 兜底机制无护栏（变异A实证）。影响：演进中兜底可被无声删除或劣化，评测绿照旧。建议：加构造性测试——微型库上构造 FTS 0 行但 LIKE 可见的查询，断言组合臂命中且 like_only 臂非空（对应规格书 T7 的先红后绿）。
- [F5] tests/test_eval_identifiers.py:79-86 —— 门槛不覆盖 zh_short（变异B实证）。影响：CJK 分词回归无红灯。建议：补 zh_short 类别 >=0.8 断言或断言索引内容含双字 token。
- [F6] tests/test_eval_identifiers.py:62-66 —— setUpClass 内 mod.main() 重写 results/results.json、results.md、raw/eval_run.json（generated_at 取当前时间），跑一次测试即弄脏 worktree，提交产物不可逐字节复现（本次审计实跑后已 git checkout 还原）。建议：测试只读已有产物或写入 tempfile。

## 六、文档口径 — 有 finding
- [F7] README.md:70-75 —— "已实测"仍写"全 suite 113 条全过"，实际 124；全文无 eval_identifiers 目录、入口与判据记载，局限声明无处可查。影响：新会话按 README 理解测试规模与评测资产即偏差。建议：补 eval_identifiers 一段（入口/判据/局限）并更新 113→124。
- docs/RESEARCH-INDEX.md 不存在，登记行无处落（本次按派单书"唯一写动作"约束未代创建）。

## 裁决
有 finding 需修：7 条（F3、F4 为必须修；F1、F5、F7 应修；F2、F6 低）。核心数字与复用声称属实，但"兜底有效"与"召回质量"两个关键结论分别缺测试护栏与局限声明，引用前必须补。

## 处置（2026-10-27 阶段 3 修订，commit 见 feat/memcpp）
- [F1] 已修：兜底臂改规格书 §2.4(:195) 口径——FTS **0 行**才触发、各 token `%token%` OR、按命中 token 数降序（id 升序钉死确定性）；`combined_ranking` 返回触发标记并随 per_query/总数导出。全量重跑后六类 Recall@5 数字不变（本数据集查询由 gold 反推，FTS 从不 0 行，fallback_triggers=0 如实记录），results.md/json/raw 已更新并新增 fallback_semantics、fallback_triggers、dataset_overlap 字段。
- [F2] 部分落实：doc_text docstring 注明复制品来源 experiments/eval_retrieval.py:138-150 与两处同步警告。提为冻结侧模块级函数借用需改冻结文件，worktree 隔离下不越界，遗留待办交非隔离线。
- [F3] 已修：render_report 头部固定「结论效力（先读）」段（与冻结 eval_retrieval.py:313 同口径措辞）；21/64 逐字子串、23/64 全 token 在 gold、由 `dataset_overlap_stats` 程序化复算（口径钉死与审计一致），0/64 同义改写标注为人工核查口径；模块 docstring 同步落字。
- [F4] 已修：新增 FallbackGuardTest 构造性护栏（微型库 memstore⊂memstorex + 全文档不存在的 zzzz token）：①兜底触发标记、gold 命中计入指标、导出计数三处断言；②单调性改强为逐类别 combined >= max(bigram_only, like_only)；③导出 fallback_used/总数必须与独立重算的「FTS 0 行」集合一致。变异自查（临时副本，仓库树已还原）：删兜底→红（test_fallback_triggers_and_counts_hit）；OR 退化 AND→红（同上）；旧 <5 行触发口径→红（test_fallback_trigger_count_consistent_with_fts_emptiness）。
- [F5] 已修：zh_short 类门槛 >=0.8（与标识符类同标准；对齐后真实数字 1.0，达标无需降门槛）；另加冻结 bigram_tokens 中文重叠双字契约钉版测试（"端口配置"→端口/口配/配置），变异 B 直接红灯。
- [F6] 已修：main(base=None) 支持产物根注入，测试写 tempfile，测试运行不再改写仓库 results/、raw/。
- [F7] 未落实：README.md 不在本次派单允许文件清单内，不越界；遗留待办另单补（eval_identifiers 入口/判据/局限一段，113→当前全量 132）。
- 全量 memory-lab 套件：Ran 132 tests, OK（旧评测 11 条替换为 19 条：体例 5 + 门槛/护栏 8 + 构造兜底护栏 4 + 分词契约钉版 2；124 - 11 + 19 = 132）。
