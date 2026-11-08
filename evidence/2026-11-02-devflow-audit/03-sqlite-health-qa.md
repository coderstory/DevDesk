# 审计③ SQLite 数据体检：零上下文可读性（qa 面）

- 日期 2026-11-02 ｜ 角色 qa-tester ｜ 池条目 need:77（开工已 `pool_update(77, 进行中, owner=qa-tester)` 认领）
- 判据来源：`docs/research/2026-11-01-skill-quality-baseline.md`，主要用 F.1-F.5、A6.7/A6.9、E.6、D.6
- 只读做法：`docs/pool/pool.db` 只 `cp` 了一次，副本是 `/tmp/audit3-pool.db`（01:26 拷贝，源文件 mtime 01:24；`PRAGMA journal_mode` 为 delete，没有 WAL，所以单文件拷贝就是一致快照）。下面所有 SQL 都只在副本上跑，可以照抄复跑：`sqlite3 /tmp/audit3-pool.db "<SQL>"`。真库只经 MCP 碰过两次：开工认领，以及一次只读 `doc_get`（②-1 的实证）。
- 快照和简报的差异（不算发现）：副本分布是关闭 41 / 在池 16 / 待验收 2 / 拒绝 10 / 挂起 9 / 进行中 2。简报是在池 18 / 待验收 1 / 进行中 1，差额来自简报之后 75/76/78 又往前推进了。
- 分级口径：
  - **P0**：现行条目无法处理，或者会让零上下文会话做出错误动作，或者库完整性已损坏。
  - **P1**：证据链或状态机的保证被打破，包括非法迁移、迁移没留记录、终态没证据、读路径藏了关键状态、权威引用指向不存在的东西、条目没有裁决就掉出了状态机。
  - **P2**：卫生或一致性问题，影响只落在历史条目或单条可读性上。
- 类别列：**机制**表示工具或流程还会继续产生同类问题；**历史**表示一次清理就能了结。

## 总判定

P0=0，P1=7，P2=19。

拿 11-01 的新条目来说，零上下文会话**基本读得懂**：5 条抽样里 4 条可读。但它**不能信任过程记录**：
- 状态日志有 5 条整段缺失、3 条是非法拒绝，另有 53% 的进行中窗口不超过 11 秒，属于事后补登；
- BUG 的已处理标记在读路径上看不到，而且和行内自述矛盾；
- qa 角色定义里的上位依据文档，在台账里根本不存在。

---

## ① open 条目 detail 能否自足

抽了 10 条，覆盖在池 4 / 挂起 4 / 进行中 1 / 待验收 1。库里排产为 0，没法抽。时期上，旧期（10-29/30）2 条、中期（10-31）3 条、新期（11-01）5 条。每条的判定见文末统计表。复跑：
`WITH s(id) AS (VALUES(14),(34),(47),(64),(49),(56),(66),(72),(78),(76)) SELECT n.id,n.status,n.owner,n.blocker,n.detail FROM needs n JOIN s USING(id);`

- **[P2] needs 14/34（同类还有 16/35）：旧期挂起条目只剩碎片、没有 owner，14 不可读**【历史】
  - 证据：
    - 14 的 detail 全文只有「flake，低；docs/plans/full-buglist-2026-10-29.md」，owner=NULL，blocker=登记不派工。
    - 34 引用「TODO.md 旧册需-8」。TODO.md 已在 10-31 删除，`ls` 结果是 MISSING。它还写「见 2026-10-27-memcpp-embed-bakeoff」但没给路径，实际文件在 `docs/superpowers/plans/`。owner=''。
    - detail 短于 60 字的比例：10-31 之前 11/39，10-31 之后 3/41。
    - `SELECT CASE WHEN created<'2026-10-31' THEN 'old' ELSE 'new' END e,COUNT(*),SUM(length(coalesce(detail,''))<60) FROM needs WHERE deleted=0 GROUP BY e;`
  - 建议：挂起条目逐条补上「现象 / 期望 / 验收 / 解挂条件 / owner」。补不出来的，就走挂起→在池→拒绝销账。
- **[P2] 挂起 9 条里 7 条（14/16/34/35/46/47/48）：blocker 只写类别，不写解挂条件（F.2/E.6）**【机制】
  - 证据：`SELECT id,blocker FROM needs WHERE deleted=0 AND status='挂起';` 显示这 7 条 blocker 只是「登记不派工 / 条件未触发 / 等环境」这种光秃的类别，只有 61、64 写清了恢复前提。零上下文会话判断不了什么时候能解挂、谁来复查，结果挂起就成了无限期。
  - 建议：blocker 写成「类别：具体条件 + 复查人或时机」；pool_update 遇到只有类别的 blocker 时给提示。
- **[P2] open 29 条：过半没有验收判据**【机制】
  - 证据：
    - `SELECT id FROM needs WHERE deleted=0 AND status IN ('在池','排产','进行中','待验收','挂起') AND detail LIKE '%验收%';` 命中 14/29。
    - 人工复核抽样发现两条假阳性：47 的「QA 验收发现」只是来源，72 的「一次验收」是执行组织方式。所以真正带判据的不超过 12/29。
    - 待验收的 76 自己也没写验收方和判据。
  - 建议：入池模板强制写「验收：」字段。可以写「待 PRD 定」，但必须写明。
- **[P2] 56/66/34/47：引用找不到目标**【机制】
  - 证据：
    - 56 写「源 PRD 同上」，但没有 need_id 也没有 slug；台账里 53 的 prd 也没关联到 56。
    - 66 写「依赖：B2」，这是批次代号，得按标题反查才知道是 65。
    - 34 指向已删除的 TODO.md。
    - 47 的证据写成简写「pool15-accept2」，实际路径是 `evidence/20261031-pool15-accept2`。
  - 建议：引用一律写 `pool:need:<id>`、doc slug 或仓内完整路径。

全量 29 条 open 的要素扫描（`验收` / `依赖|前置` / 文件或 § 指针）：带文件指针 24/29；带依赖说明 9/29；owner 是角色名的 4/29，其余 21 条是「ZCode主会话」、4 条为空（见 ⑤-3）。

## ② 文档台账是否完整

- **[P1] docs（slug=2026-10-31-testing-discipline-policy）：qa-tester 角色定义里的「上位依据（老板口径优先，冲突以此为准）」在本项目台账里不存在**【机制】
  - 证据：
    - `SELECT COUNT(*) FROM docs WHERE slug='2026-10-31-testing-discipline-policy';` 结果是 `0`。
    - `SELECT type,COUNT(*) FROM docs GROUP BY type;` 结果是 `prd|2`、`tech|2`，policy 和 audit 类型都是 0 行。
    - 真库实测 `doc_get(slug=2026-10-31-testing-discipline-policy)` 返回 `{"ok": false, "error": "文档不存在（slug=2026-10-31-testing-discipline-policy）"}`。
    - 引用它的文件：插件 cache 里的 `devflow/0.33.0/agents/qa-tester.md` 和 `devflow/0.33.0/docs/eval-tasks.md`（`grep -rln`）。
    - 后果：零上下文的 qa 会话照角色定义去取冲突裁决口径，一定失败（F.4）。
  - 建议：把这份 policy 用 `doc_save` 存进本项目台账（type=policy）；或者让角色定义改指插件随包文件，并写明缺失时怎么兜底。
- **[P2] 进行中/待验收/关闭共 45 条：几乎都没有台账文档，也没有级别字段，没法机器判断该不该有**【机制】
  - 证据：
    - 45 条里只有 53 有 prd，而且没有 tech。
    - 台账第一条文档是 2026-10-31 22:49 建的。这之后的产品需求里，54 prd 和 tech 都没有；55 没有 prd，tech 虽然有但没挂上（见下一条）。
    - UI 整改 IV 的方案主文档 `docs/design/2026-11-01-UI整改批次IV-分析与实施方案.md` 被 62-71 全部引用，却是散在仓里的 md，没进台账（F.4）。
    - 审计和研究产出（75-78、81）放在 `evidence/`（被 `.gitignore:85` 忽略）或 `docs/research/` 下的 md 里，台账的 audit 类型是 0 行。
    - S/M/L 级别没有入库，靠 detail 和进展关键词只能认出 21、53 两条，所以缺件到底算不算违规，无从判断。
    - `SELECT n.id,n.status,(SELECT GROUP_CONCAT(type) FROM docs d WHERE d.need_id=n.id AND d.deleted=0) FROM needs n WHERE n.deleted=0 AND n.status IN ('进行中','待验收','关闭');`
  - 建议：needs 加一个 `level` 列（或者给 stage 定约定），关闭前置按级别校验台账件；把 53-tech、54、55 补挂上；方案主文档用 doc_save 进台账，仓里只留指针。
- **[P2] docs id=2：池 55 的技术方案没挂到任何需求上**【历史】
  - 证据：`SELECT id,slug,type,need_id FROM docs WHERE id=2;` 结果是 `2|2026-11-01-pool55-release-list-tech|tech|`，need_id 为 NULL。按 need_id=55 查台账会查不到。
  - 建议：重新保存时带上 need_id=55。
- 没发现问题的项：
  - docs_history 版本链没有断：doc1 的 history 是 v1-3、当前 v4；doc3/doc4 的 history 是 v1、当前 v2；doc2 是 v1，没有 history。全部连续。
  - docs_fts 和 docs 一致（4/4）。
  - `SELECT d.id,d.version,GROUP_CONCAT(h.version) FROM docs d LEFT JOIN docs_history h ON h.doc_id=d.id GROUP BY d.id;`

## ③ bug 池（bugs 子表，全量 3 行）

三要素逐行检查（`SELECT b.id,b.need_id,n.status,b.resolved,b.created_by,b.note FROM bugs b JOIN needs n ON n.id=b.need_id;`）：

| bug | need | 现象 | 定位 | 复现路径 | resolved | 父状态 | 备注 |
|---|---|---|---|---|---|---|---|
| 1 | 3 | ✓ 首次安装下载一律 404 | ✓ llama_host.cpp:19/22 Q8_0→q8_0 | ◐ 只隐含「首装」，没有步骤 | 1 | 关闭 | 行内没有修复证据 |
| 2 | 55 | ✓ Tab 计数≥1 却显示「暂无×的任务」 | ✓ release_list_ui.cpp:1185 | ✓ 21 个任务只加载 20 个，再切 Tab；probe `--accept-shot-tab` | 1 | 关闭 | created_by 为空；note 自述「状态：未修复」 |
| 3 | 55 | ✓ | ✓ | ✗（截短版，没有复现句） | 1 | 关闭 | 和 bug 2 是同一个 BUG-1，重复登记 |

已处理标记和父需求状态**是一致的**：3 行都是 resolved=1，父需求都是关闭；没有 resolved=0 的行挂在终态父需求下。

- **[P1] pool_mcp.py:350 + bugs id 2/3：读路径不返回 resolved，而且行内自述和标记互相矛盾**【机制】
  - 证据：
    - `pool_fetch(need_id)` 的 SELECT 只取 `b.id, b.need_id, b.time, b.note, n.status AS parent_status`（pool_mcp.py:350-352），没有 resolved。
    - bug 2 的 note 结尾写着「状态：未修复，待派发 developer/bug-fixer」，可 resolved=1。
    - 修复的事实只记在 dev_progress 里：55 号 02:26:05 那条写「BUG-1 已修复（d5b82c23）…证据 evidence/20261101-pool55-bugfix-filterempty/」。
    - 后果：零上下文会话调用 `pool_fetch(need_id=55)`，会看到两条 BUG-1，其中一条说自己没修，并且看不到任何已处理标记（A6.7 输出不全；D.6 三态没区分）。
  - 建议：读路径把 resolved 和下一条提到的处理证据字段一起返回；调用 bug_resolved 时要求附处理说明。
- **[P2] bugs 2/3：重复登记、没有 actor、缺复现**【历史，其中 `by` 选填属于机制】
  - 证据：同一个 BUG-1 在 02:12:34（created_by 为空）和 02:26:58（ZCode主会话）各登记了一次（F.1）。bug 1 和 bug 3 都缺复现路径。
  - 建议：登记前按 need_id 查重；`by` 改成必填。
- **[P2] bugs 表结构：「已处理」没有证据，还可能一插入就是已处理**【机制】
  - 证据：
    - bug_resolved 只做一件事：`UPDATE bugs SET resolved = 1`（pool_mcp.py:492-495）。不区分修复还是忽略，也不记证据、处理人和时间。
    - 这一列是迁移加的，`DEFAULT 1`（pool_mcp.py:169）。所以任何没显式写 resolved 的插入，一进来就是「已处理」，会悄悄通过关闭前置检查（pool_mcp.py:425-432）。
    - 这和 qa 纪律里「已修复（回归绿）/ 已忽略（写理由）」的区分对不上（E.6/D.6）。
  - 建议：增加 `resolution`（fixed/ignored）、`resolution_note`、`resolved_by/time`；DEFAULT 改成 0。
- **[P2] BUG 分散在三处**【机制】
  - 证据：
    - needs 里 pool='bug' 的有 13 条；
    - bugs 子表有 3 条；
    - 还有自由文本里的 BUG-n：比如 11 号 acceptance 写「漂移断言缺口挂 BUG-4」，实际对应 needs 30；legacy pool_items 里也有 BUG-1..5。
    - 没有规则说明哪类缺陷记在哪里，彼此之间也没有交叉 id。
    - `SELECT pool,COUNT(*) FROM needs WHERE deleted=0 GROUP BY pool;`
  - 建议：在 pool-usage 里写清分工：独立缺陷记成 bug 池条目；某个需求验收中发现的缺陷记进 bugs 子表。并要求两边互相引用 id。

## ④ 状态机是否合法（status_log 全链 278 行）

合法迁移表（pool_mcp.py:222-230）：
- 在池 → 排产 / 挂起 / 拒绝
- 排产 → 进行中 / 挂起
- 进行中 → 待验收 / 挂起
- 待验收 → 关闭 / 进行中
- 挂起 → 在池
- 关闭、拒绝是终态

用 `SELECT from_status,to_status,COUNT(*) FROM status_log GROUP BY 1,2;` 统计迁移对：**有记录的迁移全部合法**。例外只有迁移导入的几条：∅→关闭 1 条（5 号），∅→挂起 3 条（1/2/3），都标了「自 pool_items 迁移」。**问题出在没记下来的迁移上。**

- **[P1] needs 29/30/38（关闭）和 34/35（挂起）：一条 status_log 都没有，整个生命周期没有迁移记录**【历史】
  - 证据：
    - `SELECT id,status FROM needs n WHERE deleted=0 AND NOT EXISTS (SELECT 1 FROM status_log s WHERE s.need_id=n.id);` 结果是 `29|关闭`、`30|关闭`、`34|挂起`、`35|挂起`、`38|关闭`。
    - 这 5 条正好是简报已确认缺 FTS 索引的那一组；29/30/38 也正是简报已确认关闭却没有 acceptance 的那几条。
    - 另外它们的 body 都是空串，created 都是无时区格式（比如 '2026-10-30 20:58'）。
    - 这是**不经 MCP 直接写库留下的痕迹**：pool_add 一定会写 body、入池日志和 FTS（pool_mcp.py:318-332）（F.3）。
    - 后果：3 条关闭的终态完全没有证据；2 条挂起的没有「何时、为何挂起」的记录。
  - 建议：用维护工具补一条「补录」日志（from=∅，to=现状态，reason 写明是补录及依据），然后重建 FTS。29/30/38 没有证据，由老板裁决是否改判。
- **[P1] needs 12/13/24：非法的挂起→拒绝，没有日志也没有 reason**【历史】
  - 证据：
    - `WITH m AS (SELECT need_id,MAX(id) mid FROM status_log GROUP BY need_id) SELECT n.id,n.status,s.to_status,n.updated FROM needs n JOIN m ON m.need_id=n.id JOIN status_log s ON s.id=m.mid WHERE n.deleted=0 AND s.to_status<>n.status;` 结果里有 `12|拒绝|挂起|2026-10-30 13:21:04`；13、24 也一样，updated 是同一秒。
    - 挂起只能转到在池（pool_mcp.py:227）。
    - 拒绝理由只被追加到 detail 末尾：「| 拒绝理由：老板 2026-10-30 评审页勾选裁决不做」。status_log 里没有拒绝这一行，body 也没重算（见 ⑤-1）。
    - 三行同一秒、无时区，说明是批量直接写 SQL（F.3）。
  - 建议：补录拒绝日志，reason 取 detail 末尾的原文；doctor 增加「最后一条日志 ≠ 现状态」的检查。
- **[P1] status_log 全局：迁移是事后补登的，日志当不了过程证据（跨表时间倒挂）**【机制】
  - 证据：
    - 同时有进行中和待验收日志的需求共 38 条，其中 **20 条（53%）的进行中窗口不超过 11 秒**：MCP 时期 13 条（1/2/15/18/25/26/27/31/32/44/50/52/55），迁移批次同秒 7 条（4/6/7/8/9/11/17）。
    - 55 最清楚：dev_progress 里「QA 独立验收…总判定：通过」是 02:12:34，「BUG-1 已修复（d5b82c23）」是 02:26:05，**都早于**排产→进行中的 02:26:14。18、52 也有交付或验收记录早于进行中。
    - 复跑：`WITH ip AS (SELECT need_id,MIN(time) t FROM status_log WHERE to_status='进行中' AND length(time)=25 GROUP BY need_id), wa AS (SELECT need_id,MIN(time) t FROM status_log WHERE to_status='待验收' AND length(time)=25 GROUP BY need_id) SELECT ip.need_id,CAST((julianday(substr(wa.t,1,19))-julianday(substr(ip.t,1,19)))*86400 AS INT) secs FROM ip JOIN wa USING(need_id) WHERE secs<=11;`
    - 后果：零上下文会话照日志复盘周期、谁在什么时候做了什么，会得出错误结论。状态机「先派发后开发、先开发后验收」的闸门，在这些条目上只是事后补走了一遍（D.6）。62 的 detail 里自己也承认「先干后补登违规已认」。
  - 建议：补登时 reason 必须标明「补登 + 实际发生时间」；进行中停留不到 60 秒就转待验收的 pool_update 调用，要求附 note 说明。
- **[P2] 「放弃」该用哪个状态没有定义，结果两种写法并存，还有走过场的迁移**【机制】
  - 证据：
    - 1 和 2 在 17:32:54 做了挂起→在池，reason 写「老板裁决放弃,按状态机回在池走关闭」。当时在池→拒绝本来就合法，它们却在 17:33:01 到 17:33:08 之间，用空 reason 一路走完排产→进行中→待验收→关闭。
    - 28 是在待验收状态直接以「放弃」关闭的；待验收没有直达拒绝的路。
    - 这三条的 acceptance 都写「放弃性销账(非验收通过)」：`SELECT need_id FROM acceptance WHERE evidence LIKE '%放弃%';` 结果是 28/1/2。
    - 另一边，23/36/39/40/41 放弃时走的是 …→在池→拒绝。
    - 后果：pool_stats 里「关闭=41」包含 3 条并未交付的需求；日志里还多出 2 组实际没发生过的排产/进行中/待验收（D.6）。
  - 建议：pool-usage 里明确「放弃 = 拒绝」，并允许排产、进行中、待验收、挂起直接转拒绝（reason 必填）；1/2/28 由老板裁决要不要改判。
- **[P2] 链条断裂、历史截断（这些条目都已到终态）**【历史】
  - 证据：
    - 23：行 119 的 from 是进行中，但上一行 60 的 to 是挂起，也就是挂起→…→进行中这段没有记录。
    - 27/28/36 的第一条日志就已经在中途：27 是挂起→在池，28 是进行中→待验收，36 是进行中→挂起。
    - 31/32/33/39/40/41/42/43 的第一条日志是挂起→在池，没有入池那一行。
    - `WITH s AS (SELECT id,need_id,from_status,to_status,LAG(to_status) OVER (PARTITION BY need_id ORDER BY id) pt FROM status_log) SELECT * FROM s WHERE pt IS NOT NULL AND pt<>from_status;` 结果是 `119|23|进行中|挂起|挂起`。
  - 建议：doctor 增加链连续性检查；历史条目做个标注就行，不必补造记录。
- **[P2] reason 为空、actor 为空**【机制】
  - 证据：
    - 行 143（45 号在池→挂起）reason=''。原因是 pool_mcp.py:419 允许拿行上残留的 blocker 通过「挂起必填 blocker」校验，而 :479 只记录 `reason or blocker or note or ""`，于是日志里没有原因。
    - actor 为空的有 83 行：10-29/30 遗留 81 行，加上近期 2 行（267：81 号进行中→待验收；274：76 号进行中→待验收；这两个条目的 owner 分别是 researcher、bug-fixer）。原因是 `by` 选填（F.3 可追责性）。
  - 建议：:479 在没有其他原因时回落到 `row["blocker"]`；状态变更时 `by` 必填。
- 没发现问题的项：
  - 没有原状态到原状态的迁移。
  - 同一种时间格式内没有时间倒挂：拿 `length(time)=25` 的前后行比较，0 行。
  - 没有日志早于创建时间的情况，0 行。
  - 没有进行中直接跳到关闭、绕过待验收的情况：唯一不经待验收的关闭是 5 号的迁移导入。
- **简报事实更正**：简报说「拒绝→在池可逆」，这和代码不符（pool_mcp.py:229 `"拒绝": set()`），也和两份 pool-usage skill 不符（插件 SKILL.md:22、项目 devdesk-pool-usage SKILL.md:33）。拒绝是终态。下游复用简报时请以代码为准。

## ⑤ 脏数据

- **[P1] needs_fts 和 needs 不一致；其中 34/35 是现行条目，查重时看不到**【历史】
  - 证据：
    - `SELECT GROUP_CONCAT(id) FROM needs WHERE deleted=0 AND id NOT IN (SELECT id FROM needs_fts_docsize);` 结果是 `29,30,34,35,38`。这和简报已确认的一致，这次全集比对没有新增。
    - 34/35 是现行挂起条目，pool_search 永远搜不到，入池查重（F.1）对它们完全无效。
    - 新发现：`SELECT id FROM needs WHERE deleted=0 AND body<>substr(coalesce(detail,''),1,400);` 除上面 5 条外，还有 `12,13,24`。这三条的 detail 被直接写库追加了拒绝理由，但没重算 body 和 FTS，所以拒绝理由搜不到。
  - 建议：对全部 live 行从 body 重建 contentless FTS，具体做法是先逐行 'delete' 再按 `_cjk_space` 重新插入，并配合审计② 提出的 doctor/integrity 工具（A6.9）。
- **[P1] pool_items 需-26「像素办公室 demo」：一个没结案的需求，没经过任何裁决就掉出了状态机**【历史】
  - 证据：
    - `SELECT id,status,substr(detail,1,60) FROM pool_items WHERE id='需-26';` 结果是 `需-26|open|来源=需求分析群 10-29 讨论…两道门（静态页+status.json 桥）验证中`。
    - 拿「像素办公室 / Claude-Office」在 needs 的标题和 detail、dev_progress 的 note 里搜，都是 0 命中。
    - 后果：经 pool MCP 工作的会话永远看不到它；直接看旧表的会话又会以为它还开着。
  - 建议：由老板裁决，补迁进 needs，或者明确拒绝。
- **[P2] 时间戳有 4 种格式，而且无时区的值含义不统一（F.5）**【历史】
  - 证据：
    - needs.created 有 10/16/19/25 四种长度；updated 和 status_log 各有 3 种；38 条需求至少有一个时间戳没带时区：`SELECT length(created),COUNT(*) FROM needs GROUP BY 1;`
    - 无时区的值各有各的意思：
      - 12/13/24 的 '13:21:04' 和 29 的 '13:21:49' 只能是 UTC。以 24 为例，它的拒绝写入一定晚于 20:32:33 +0800 那条挂起日志，否则现状态不会是拒绝。
      - personal_todo 里无时区的值和带 +0800 的值在 21:46–22:04 之间交替出现，构成一轮签到，显然是本地时间。
    - 后果：24、29 出现 created>updated 的假倒挂：`SELECT id,created,updated FROM needs WHERE replace(created,'T',' ')>replace(updated,'T',' ');` 结果是 24、29。字符串排序和龄期计算都不可靠。
  - 建议：先备份，再一次性迁移，把所有时间戳统一成带偏移的格式；所有写入方都只用 `_now()`。
- **[P2] open 条目的 owner 不规范**【机制】
  - 证据：
    - 14/16/34/35 的 owner 为空（2 条 NULL、2 条空串），这 4 条都是挂起。
    - 21 条 open 的 owner 是「ZCode主会话」。那是调度者，不是角色名，等于把登记人和承接人混在一个字段里。
    - docs.created_by 里出现了 'product'，规范角色名应是 product-manager。
    - 终态条目还留着 persona 名：惨品、辛勤码农、元气小萝莉、史密斯专员、jQuery。
    - `SELECT coalesce(owner,'<NULL>'),status,COUNT(*) FROM needs WHERE deleted=0 AND status IN ('在池','排产','进行中','待验收','挂起') GROUP BY 1,2;`
  - 建议：未分派就写空或「待分派」；owner 只填承接人的角色名；登记人放在 created_by。
- **[P2] 状态变了，blocker 和 stage 没跟着清理**【机制】
  - 证据：
    - 不在挂起状态却仍有 blocker 的有 20 条（14 条关闭、6 条拒绝）。比如 1/2 已关闭，blocker 还是「等授权」；23/36/41 把拒绝理由存进了 blocker 列。
    - `SELECT id,status,blocker FROM needs WHERE deleted=0 AND status<>'挂起' AND trim(coalesce(blocker,''))<>'' AND blocker<>'none';`
    - HTML 报告会把 blocker 这一列渲染出来（pool_mcp.py:822-828），于是已关闭的条目显示成被阻塞。残留的 blocker 也是 ④ 里「reason 为空」的成因。
    - stage 列：41 条关闭里有 34 条是空的，原因见 AUTO_STAGE 那段历史死代码（pool_mcp.py:234-237 的注释），所以不能拿 stage 判断阶段。
  - 建议：挂起→在池时清空 blocker；对现有数据做一次性清理。
- **[P2] 软删除的 37 号没有级联处理**【机制】
  - 证据：
    - 37 已 deleted=1，但 status_log 行 82 仍是 deleted=0，FTS 也还索引着 rowid 37：`SELECT id FROM needs WHERE deleted=1 AND id IN (SELECT id FROM needs_fts_docsize);` 结果是 37。
    - pool_search 在 JOIN 时已经过滤了 `n.deleted = 0`（pool_mcp.py:546），所以检索上看不出影响。
    - 但直接读 status_log 的人会看到这条已删条目的历史。
  - 建议：soft_delete 时对 FTS 执行 'delete'，并把子表行标上标记；或者在 pool-usage 里写明子表行是有意保留的。
- **[P2] pool_items 成了一张影子表，状态和 needs 互相矛盾**【历史】
  - 证据：
    - 共 31 行，其中 5 行是 open。
    - BUG-4、需-14、需-17 在 needs 里对应 30/27/29，标题一致，都已关闭；需-16 疑似对应 26，也已关闭。
    - pool_mcp.py 根本不读这张表（`grep pool_items` 0 命中）。
    - `SELECT id,status,title FROM pool_items WHERE status<>'closed';`
  - 建议：需-26 裁决完之后，把这张表归档或者删掉，删之前先备份。
- **[P2] personal_todo 全是过时的「进行中」**【历史】
  - 证据：
    - 9 行全是「进行中」，最后更新时间都不晚于 2026-10-30。owner 全是 persona，而按 23/36 的 blocker，这些人「全员请假(2026-10-31 老板告知)」。
    - 内容指向已被拒绝的看板 v2（36 号）。
    - `SELECT owner,status,updated FROM personal_todo WHERE deleted=0;`
    - 后果：零上下文会话会以为有 9 个人在岗。
  - 建议：用 todo_set 清空或者结掉。
- **[P2] 41/42/43 只有标题，detail 是空的**【历史】
  - 证据：`SELECT id,status FROM needs WHERE deleted=0 AND trim(coalesce(detail,''))='';` 结果是 41 拒绝、42 关闭、43 关闭。都是终态，影响只落在历史可读性上。
  - 建议：从 dev_progress 或 acceptance 里摘一句，补成 detail。
- 没发现问题的项：
  - 子表没有孤儿行：dev_progress、bugs、acceptance、status_log、docs、docs_history 指向不存在父记录的都是 0 行。
  - docs_fts 和 docs 一致。
  - created/updated 为空的 0 条。
  - 其他条目的文本里引用已删条目的 0 条。

---

## 计数

| 级别 | 数量 | 条目 |
|---|---|---|
| P0 | 0 | — |
| P1 | 7 | ②-policy 文档不存在、③-读路径不返回 resolved、④-零日志 5 条、④-非法拒绝 12/13/24、④-补登与时间倒挂、⑤-FTS 不一致、⑤-需-26 掉出状态机 |
| P2 | 19 | ①×4、②×2、③×3、④×3、⑤×7 |

按类别分：**机制 14 条**（不改工具或流程会继续产生），**历史 12 条**（一次性维护迁移就能清掉，迁移前先备份）。

## 抽样条目可读性统计

| id | 状态 | 时期 | 要干什么 | 验收 | 依赖 / 解挂 | 判定 | 缺什么 |
|---|---|---|---|---|---|---|---|
| 14 | 挂起·bug | 旧 10-29 | ✗ | ✗ | ✗ | **不可读** | 现象细节和复现、期望行为、验收、解挂条件、owner |
| 34 | 挂起·need | 旧 10-30 | ◐ | ✗ | ◐「数百条+」没量化 | 部分可读 | 验收、量化阈值、owner；引用的 TODO.md 已失效，bakeoff 没给路径 |
| 47 | 挂起·bug | 中 10-31 | ✓ | ✗ | ✗ 只有「登记不派工」 | 部分可读 | 验收（比如负载下连续 N 次绿）、复现方法、解挂条件 |
| 49 | 在池·debt | 中 10-31 | ✓ | ◐ 断言形态 | ✓ | 可读 | 行号会漂移，最好改用函数名锚定 |
| 56 | 在池·need | 中 10-31 | ◐ 范围没界定 | ✗ | ◐「若体验不一致」太主观 | 部分可读 | 「源 PRD 同上」的指代、验收、启动判据 |
| 64 | 挂起·need | 新 11-01 | ✓ | ✓ 像素判据没量化 | ✓ 恢复步骤写在 blocker 里 | 可读 | 没写明未提交的改动在哪台机器、哪个分支或 worktree |
| 66 | 在池·need | 新 11-01 | ✓ | ✓ | ◐「依赖：B2」 | 可读 | 依赖应写成 `pool:need:65` |
| 72 | 在池·bug | 新 11-01 | ✓ | ✗ | ✓ 和 73 合批 | 部分可读 | 复现步骤和数据条件、验收；定位只到文件级 |
| 76 | 待验收·need | 新 11-01 | ✓ | ◐ 只有产出格式 | ✓ | 可读 | 谁来验收、按什么判 |
| 78 | 进行中·need | 新 11-01 | ✓ | ◐ 只有产出格式 | ✓ | 可读 | 产出物放在哪里 |

| 维度 | 可读 | 部分可读 | 不可读 |
|---|---|---|---|
| 合计（10 条） | 5 | 4 | 1 |
| 旧期 10-29/30（2） | 0 | 1 | 1 |
| 中期 10-31（3） | 1 | 2 | 0 |
| 新期 11-01（5） | 4 | 1 | 0 |
| 挂起（4） / 在池（4） / 进行中（1） / 待验收（1） | 1 / 2 / 1 / 1 | 2 / 2 / 0 / 0 | 1 / 0 / 0 / 0 |

最常见的缺口有三类：验收判据（10 条里 5 条完全没有：14/34/47/56/72；3 条只有产出格式或断言形态：49/76/78）；挂起条目的解挂条件（7/9）；引用没落到 id 或 slug 上（4 条）。

## 建议的执行顺序

1. **先修机制，否则清了还会再脏**：
   - 读路径返回 resolved，并补上处理证据字段，DEFAULT 改成 0；
   - `by` 必填；
   - :479 的 reason 回落到 blocker；
   - 离开挂起时清空 blocker；
   - 放弃统一走拒绝，允许从排产、进行中、待验收、挂起直接转拒绝；
   - 增加 level 字段，关闭前置按级别校验台账件。
2. **再做一次维护迁移，迁移前先备份**：
   - 补录 29/30/34/35/38 的日志，以及 12/13/24 的拒绝日志；
   - 全量重建 needs_fts；
   - 统一时间戳格式；
   - 清理残留的 blocker 和 personal_todo；
   - 老板裁决需-26 之后，归档 pool_items。
3. **内容补齐**：
   - testing-discipline-policy 存进台账；
   - 挂起条目逐条补解挂条件，或者直接销账；
   - 入池模板加「验收：」字段。
4. 本报告里的 SQL 可以直接沉淀成 doctor 的检查项，和审计② 的「数据诊断」子维衔接。

（工具调用共 22 次（包括报告写完后 5 次更正编辑），预算 30，没有超。真库只经 MCP 碰过两次（认领 + doc_get），sqlite3 一律只查副本，除本报告外没有改任何仓内文件。）
