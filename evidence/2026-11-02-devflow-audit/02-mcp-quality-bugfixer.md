# devflow 审计②：MCP 工具质量（bug-fixer 面）

- 日期：2026-11-02 ｜ 角色：bug-fixer ｜ 池条目：[pool:need:76]（已认领 status=进行中）
- 对象：`/Users/coderstory/dev/zcode-plugins/devflow/tools/`（pool_mcp.py 1005 行、pool_migrate.py 187 行）+ tests/（pool-smoke.py、pool-mcp-acceptance.py、gate-test.sh、eval-run.sh）+ skills/pool-usage/SKILL.md + .zcode-plugin/plugin.json mcpServers 段
- 尺子：`docs/research/2026-11-01-skill-quality-baseline.md` A6.1-A6.9 / C.10 / D.6 / E.1 + 官方 diagnosing-mcp SKILL.md（pitfall 1-14、定位流程）
- 纪律：源码零改动；真库仅 `cp` 出副本（`/tmp/mcpaudit/db/real-copy.db`），全部破坏性实验对 `/tmp` 副本；已验证事实（13 工具挂载、状态机口径、E9 冒烟存在、mcpServers 模板变量）沿用不重查
- **简报缺陷两条（打回主会话更正口径）**：① 简报标记为 `[pool:need:76]` 而非 `[pool:bug:*]`，按审计任务性质认领至 need 76 并开工（已在 pool_update note 声明）；② 简报列的对象 `pool_ops.py`、`pool_review_gen.py` **在仓内不存在**（全仓 find 为空），实际工具面 = pool_mcp.py + pool_migrate.py，下游审计请以实际文件为准
- 源码 = 运行版：tools/ 与 cache 0.33.0 逐字节一致（46439/8895 B），源码级结论即线上行为
- 实验装置：`/tmp/mcpaudit/audit.py`（`e1` 稳定性 / `e2` 容错 / `e3` 备份跨机 / `e5` 诊断电池），复现命令均见各条

## 结论先行

1. 结构面强：13 工具与 SKILL.md 承诺一致、状态机/关闭闸/版本链/软删与文档口径吻合、全参数化无注入面、损坏库/锁/参数错 server 均不退出，pool-smoke（E9）当前 HEAD 复跑 PASS。
2. **P1×3**：写后报告/快照生成失败 → 数据已提交却整包报错，重试即重复条目；跨机快照合并零能力（双机自增 id 必撞车、rebuild 只认空库）；无 doctor 且生产库已实锤 FTS 索引漂移（5 缺索引 + 1 陈旧，查重铁律对这 6 条失效）与 3 条关闭态无 acceptance。
3. P2×14：非对象 JSON 行可杀 server、只读库连读工具都挂、快照写不原子、bug_resolved 静默空操作、空标题可入池、无输入上限且每写 O(全池) 重写快照、annotations/inputSchema 约束缺失等，见各轴。

---

## 轴① 完善性（承诺 vs 工具面）

[P2] tools/pool_mcp.py:396（签名含 stage）+ 441-442（AUTO_STAGE 自动推进） vs skills/pool-usage/SKILL.md:21 — pool_update 实际有 `stage` 参数且状态迁移时自动推进（排产→已分析/进行中→开发中/待验收→测试中/关闭→已验收/拒绝→已否决），SKILL.md 全文 0 处提及 stage（`grep -c stage SKILL.md` = 0）——工具有而文档无，调用方无法显式控制阶段。证据：tools/list inputSchema 含 stage（`_tool_schema` 由签名生成，pool_mcp.py:920-930）。建议：SKILL.md:21 签名补 `stage?` 并注明「缺省随状态自动推进」。

[P2] SKILL.md:27 vs tools/pool_mcp.py:707-725 — doc_get 承诺「id/slug 二选一」，实现语义是「都不给才报错；都给且指向同一文档则受理，不一致才拒」（BUG-2 修复后语义，docstring :709-711 已写对）。文档落后实现，按文档写的调用会被无谓拒绝一次再重试。证据：pool-smoke.py:108-114 已断言该语义（通过）。建议：SKILL.md 改为「id/slug 至少一个；都给须一致」。

[P2] SKILL.md:7 vs tools/pool_mcp.py:589 — 承诺「每次写操作自动重生成报告 pool-report.html 与快照 pool-snapshot.json」，实现 todo_set 只写快照不写报告（:589 仅 `_write_snapshot()`）。证据：`python3 /tmp/mcpaudit/audit.py e1` 输出 `[11] todo_set 后报告 mtime 变化=False`。建议：todo_set 补报告重生成，或 SKILL.md 措辞收窄为「除 todo_set 外」。

[P2] tools/pool_mcp.py:349-360 vs SKILL.md:20 — pool_fetch(need_id) 三处口径问题：① 返回行缺 `resolved` 列（SQL 只取 id/need_id/time/note/parent_status），关闭闸要求逐行 `bug_resolved`，但此查询无法看出哪行未处理（pool_trace 才带全列）；② docstring :348 与 SKILL.md 称 `status=在池`=「只看未关闭的」，实现是父需求状态**精确等于**在池（`AND n.status = ?`），进行中挂 BUG 的查不到；③ need_id 给定时 priority/pool/owner 被静默忽略。证据：e1 输出 `[13] {"kind":"bugs_by_need","rows":[{"id":1,...,"note":"样例BUG","parent_status":"排产"}]}`（无 resolved 键）。建议：SQL 补 `b.resolved`，docstring 改「status=在池 即父需求恰为在池」。

[P2] tools/pool_mcp.py:95-107 + 768-782 — docs 表有 `deleted` 软删列与部分唯一索引，但 13 工具无任何删除文档入口（pool_soft_delete 只删 needs，:606-618）→ 误建文档无法删除只能覆盖重写，deleted 沦为只有禁掉的直写 SQL 才能碰的死列。建议：补 doc_soft_delete(id, by) 对齐 needs 软删纪律，或删掉该列止血认知负担。

其余逐项核对（13 工具名/状态机七态/关闭-拒绝-挂起三闸/双池禁用全局池/doc 版本链/排序口径）与 SKILL.md 一致——**未发现**更多承诺缺口。

## 轴② 稳定性（异常输入）

[P2] tools/pool_mcp.py:949 — 非对象 JSON 行使 server 进程崩溃退出（`msg.get("id")` 对 list/标量抛 AttributeError，未被捕获），会话内 13 工具全部消失且不可恢复；:976/:989 的「server 不退出」承诺只包住了 tools/call 分支，帧解析层有豁口。证据：`python3 /tmp/mcpaudit/audit.py e1` → `[17] 发送 JSON 数组行 → <EOF 无响应>`、`[18] 进程存活: False | stderr: AttributeError: 'list' object has no attribute 'get'`。建议：`isinstance(msg, dict)` 校验，非 dict 回 -32600 或静默跳过。

[P2] tools/pool_mcp.py:987-996 — 错误包装两层口径：参数错返回 Python TypeError 原文（未知参数有 "Did you mean" 提示，尚可操作）；业务级拒绝（ok:false）一律 isError=False 纯文本 JSON，宿主侧无法靠 isError 识别拒绝，模型必须解析文本；底层 sqlite/OS 异常（DatabaseError/OperationalError）原样透出无下一步建议（对照 A6.7「错误可操作」部分达标）。证据：e1 `[4][5]` TypeError 干净返回；e2 `[a1] DatabaseError: file is not a database`、`[d1] OperationalError: database is locked`。建议：底层异常包一层「建议：损坏→快照 rebuild / 锁→稍后重试」提示；业务拒绝考虑 isError=True。

[P2] tools/pool_mcp.py:309-335 + 920-930 — 输入防御缺口合集：空标题/空 owner 可入池（NOT NULL 不拦空串）；无任何长度上限，1.5MB detail 入库后快照 1.5MB、此后**每次写操作**（含 todo_set 小写）整份重写快照；inputSchema 无 enum（pool/priority/status/type 皆裸 string）、无 description、无 outputSchema/structuredContent；pool_fetch/pool_search/doc_list 无 limit 全量返回（上下文爆仓面，A6.7「分页/截断」缺失）；annotations 全缺（A6.8 不达标，tools/list 仅 name/description/inputSchema，:963-968）。证据：e1 `[1] 空标题入池 ok id=1`；`[9] 1.5MB detail → 快照 1503307B`；`[10] 之后 todo_set 快照 1503512B`。另注：字符串 `"2"` 作 id 经 SQLite 亲和性被静默接受（e1 `[6]`，宽松无害）。建议：标题/owner 非空校验、detail 上限（如 100KB）、schema 加 enum 与 annotations（pool_fetch/pool_search/doc_get/doc_list/doc_search/pool_trace/pool_stats/todo_get=readOnlyHint；pool_soft_delete=destructiveHint）、列表工具加 limit。

[P2] tools/pool_mcp.py:492-496 — bug_resolved 指向不存在行或不属于该需求的行返回 ok:true（静默空操作，rowcount 未检查），调用方无从得知没标上。证据：e1 `[8] bug_resolved=424242 → {"ok": true}`。建议：rowcount=0 时返回 `{"ok":false,"error":"bugs 行 424242 不存在或不属于需求 N"}`。

正面未发现：SQL 注入面为零（全参数化，唯一拼接是常量 ORDER BY CASE，:375-377/:797-799）；FTS keyword 双引号正确转义（:257-258，e1 含引号关键词正常）；未知工具/缺参/未知参数/params 非对象均干净返回且 server 存活。

## 轴③ 容错性（损坏/只读/锁/依赖/重生成失败）

[P1] tools/pool_mcp.py:333-334、502-503、616-617、702-703（写后 `_write_report()/_write_snapshot()` 裸跑）— **数据已提交却整包报错**：POOL_REPORT 落点不可写时，写工具在事务提交后抛 PermissionError（isError=true），调用方按报错重试 → 重复条目；且此后所有写操作持续报错、读操作正常，故障语义完全倒置。证据：`python3 /tmp/mcpaudit/audit.py e2` → `[c1] pool_add: PermissionError ... report.html`、`[c2] 直查 needs 行数=1（已落库但工具报错）`、`[c3] 重试` → `[c4] needs 行数=2（重复条目）`、`[c5] pool_stats 正常返回 total=2`。建议：报告/快照生成 try/except 降级为附加警告字段（如 `{"ok":true,"warning":"已提交；报告重生成失败：<原因>"}`）；给 _write_report/_write_snapshot 加「失败下次写自动补写」标记至少保证最终一致提示。

[P2] tools/pool_mcp.py:53-60 — `_connect()` 每次连接（含纯读）都跑 `_ensure_schema`（executescript DDL + commit）：**只读 DB 文件连读工具都失败**，SKILL.md:39 的「紧急只读」语义在 MCP 面整体不可用（只剩 CLI 直查）。证据：e2 `[b1] 只读库 pool_stats: OperationalError: attempt to write a readonly database`。建议：schema 版本号缓存（首次成功初始化后本进程跳过 DDL），或 DDL 失败时降级为只读连接重试。

[P2] tools/pool_mcp.py:56 — 无 busy_timeout 配置：独占锁下读等满默认 5s 后原样报 database is locked，无「稍后重试」指引；server 存活、锁释放后自愈。证据：e2 `[d1] 等待 5.5s 后 OperationalError: database is locked`、`[d2] 存活: True`、`[d3] 锁释放后 pool_add ok id=2`。建议：`PRAGMA busy_timeout` 显式化 + 错误文案加重试建议（双会话同库写是真实场景）。

未发现（正面项）：**mcp SDK/venv 缺失轴不适用**——server 零第三方依赖（import 清单全 stdlib，pool_mcp.py:17-27），裸系统 python3 即跑（pool-smoke 以 `shutil.which("python3")` 拉起，PASS）；客户端侧 pool-mcp-acceptance.py 无 SDK 时 exit 2 带明确换解释器指引（实测输出「客户端解释器缺少 mcp SDK…」）；损坏文件头（e2 [a1-a3]：干净 DatabaseError + ping 正常 + server 存活）与并发锁（[d]）server 均不退出；pool-smoke 当前 HEAD 复跑 PASS（E9 等价物，未重写）。

## 轴④ 备份与导入

[P1] tools/pool_mcp.py:892-914 + tools/pool_migrate.py:133-168 — **跨机迁移/合并零能力，双机 id 必撞车**：快照按自增 id 全量导出，双机各自 pool_add 产生同 id 不同内容（两机快照 needs[0] 均为 id=1）；git 合快照必然主键冲突，rebuild 只认空库（防覆盖闸正确但无任何合并/导入/重编 id 工具），恢复全靠人工改 JSON。真库 max id=81，DevDesk 双机硬约束下是现实撞车面。证据：`python3 /tmp/mcpaudit/audit.py e3` → `[e1] A 机 id=1 甲机器条目`、`[e2] B 机 id=1 乙机器条目`、`[e3] rebuild 进非空库: FAIL 目标库非空…只认空库`、`[e5] 人工重编 id 后 union rebuild: rc=0`（可行但纯手工）。建议：pool_migrate 加 `merge --theirs <快照>` 子命令（按 created+title 启发式重编 id 并级联重映射子表/FTS/版本链），或在 SKILL.md/README 明文「单机写纪律 + 冲突时人工重编 id 步骤」。

[P2] tools/pool_mcp.py:914 + tools/pool_migrate.py:136 — 备份件写入非原子 + 恢复件错误不干净：快照 `path.write_text` 无 tmp+os.replace，崩溃可留半截快照；rebuild 对半截快照抛裸 JSONDecodeError traceback（rc=1、stdout 空）。证据：e3 `[e4] 半截快照 rebuild: stderr尾行=json.decoder.JSONDecodeError: Unterminated string...`。建议：写 `.tmp` 后原子替换；rebuild 捕获 JSONDecodeError 输出结构化 FAIL（对齐 :139-146 既有 FAIL 风格）。

覆盖面核对：pool-snapshot.json 八表全量（docs_history 含全文，:907-911），pool.db gitignore、报告含全部卡片段落——备份内容面达标；restore=rebuild 有八键校验 + 非空防覆盖闸（pool_migrate.py:137-146，好）——**其余未发现**。

## 轴⑤ 数据诊断

[P1] tools/pool_mcp.py（全文无 doctor/integrity 工具）+ tools/pool_migrate.py（rebuild/backfill 均不自检）— **无任何自检手段，且生产库已实锤三处不变量破坏**（对真库副本 `cp` 后实跑诊断电池）：
1. FTS 索引漂移：5 条活跃条目在 needs_fts 无索引（id 29/30/34/35/38，均 2026-10-30 旧路径写入）+ 1 条陈旧索引（id 5，索引值与现标题不符）——按自身标题 pool_search 命不中共 6 条 → **入池查重铁律（F.1）对这些条目失效**；
2. 关闭态无 acceptance 记录且无关闭 status_log：id 29/30/38（当前关闭闸 :421-437 会强制留痕，此三条系旧路径产物）；
3. 状态机/外键/孤儿行本身干净（integrity_check ok、FK 违例 0、孤儿 0、非法态 0、挂起缺 blocker 0）。
证据：`python3 /tmp/mcpaudit/audit.py e5` → `[f6] needs_fts 幽灵/缺索引: 0 5`；装置内 g 系列（对 real-copy.db）：`[g1] FTS 缺索引 id=29,30,34,35,38`、`[g2] 按自身标题命不中=6`、`[g3] 关闭无 acceptance=[29,30,38]`、`[g4] 其关闭 status_log=[]`。建议：新增 `pool_doctor` 工具（只读诊断）+ pool_migrate `reindex` 子命令（可修复）：应检清单 = PRAGMA integrity_check / foreign_key_check / 四子表孤儿 / docs.need_id 悬空 / 软删需求上的活跃 docs / needs_fts+docs_fts 幽灵与缺索引双向对账 / 逐条「按自身标题检索」自检 / 关闭⇒acceptance 存在 / 挂起⇒blocker 非空 / 关闭⇒bugs 无 resolved=0 / docs deleted 残留 / personal_todo 同 owner 多活行 / status 枚举外值（电池已实现于 audit.py e5，可平移）。

[P2] tools/pool_mcp.py:606-618、621-633 — 软删读口径不一致：pool_search / pool_fetch(pool=) 隐藏软删需求，但 pool_fetch(need_id) 仍返回其 BUG 子表（JOIN 无 `n.deleted=0` 条件，:350-352），doc_list(need_id) 同样不校验父需求 deleted；pool_trace 子表查询无 deleted 过滤（审计回溯可接受但未文档化，「默认所有读路径不再返回」的 docstring :607 与实现不符）。证据：e1 `[14] 软删需求2 ok` → `[15] pool_fetch need_id=2 仍返回其 BUG`、`[16] pool_fetch(pool=need) 已不含 id2`。建议：bugs_by_need 分支补 `n.deleted=0`（或返回行加 parent_deleted 标记），docstring 如实化。

## 其他平台风险（本机无法验证，标注待测）

[P2-待验证] .zcode-plugin/plugin.json:14 — pool server `command: "python3"` 裸命令：Windows 默认无 `python3.exe`（仅 python.exe/py.exe），PATH 无 MSYS2 时 spawn ENOENT → pool MCP 整体 failed（diagnosing-mcp pitfall 2：command not found）。DevDesk 双机一等公民硬约束下必须 Windows 机实测 `zcode` 会话内 13 工具可用性；不能保证则改 `"py"` + args `["-3", ...]` 或文档写明 PATH 前置要求。monitor server（:25-29）同理依赖 `node`，无 timeoutMs（默认 30s，两 server 均毫秒级启动，风险低）。

## 防回归建议（给 developer/qa 的修复序）

1. P1#报告失败语义（轴③）：改 5 处写后调用点为降级警告——最小 diff、消除重试重复条目路径。
2. P1#doctor+reindex（轴⑤）：先落只读 doctor 把 6 条 FTS 漂移与 3 条无 acceptance 显性化，再决定数据修复（reindex 可全自动）。
3. P1#跨机合并（轴④）：至少先文档化单机写纪律，merge 工具排期。
4. P2 批量小改：协议层 isinstance 守卫、bug_resolved rowcount、空标题校验、schema enum+annotations、软删口径对齐、快照原子写——均一处一改，各配一条 pool-smoke 红用例（当前 tests/ 对以上全部零覆盖：pool-smoke 只测 happy path + docs 错误面，无损坏/只读/锁/协议行/空输入用例）。

---

## 计数

| 级别 | 数量 | 条目 |
|---|---|---|
| P0 | 0 | — |
| P1 | 3 | 轴③报告失败语义（数据已提交却报错→重试重复）；轴④跨机合并零能力；轴⑤无 doctor+生产库 FTS 漂移/不变量破坏实证 |
| P2 | 14 | 轴①×5（stage 未记档/doc_get 口径/todo_set 报告口径/pool_fetch need_id 三合一/docs 死列）、轴②×4（协议行崩溃/错误包装/输入防御合集/bug_resolved 空操作）、轴③×2（只读库读工具全挂/busy_timeout）、轴④×1（快照非原子+rebuild 裸 traceback）、轴⑤×1（软删读口径）、平台×1（python3 待 Windows 验证） |

（工具调用共 19 次，预算 40，未超。禁做项未触碰：源码零改动，真库仅 cp，实验全在 /tmp/mcpaudit。）
