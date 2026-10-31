# 需-10 会话资产库(段1)测试清单卡

- 脚本:`tools/session_archive.py`;spec:`docs/specs/2026-10-31-需-10-会话资产库-spec草案.md`
- 测试环境:临时库 `SESSION_ASSETS_DB=/tmp/sa-test/sa.db`(F-1);正式库 `~/DevDesk/session-assets/session-assets.db`(F-2/F-3)
- 执行人/时间:ZCode 主会话,2026-10-31 03:1x-03:3x

| F-ID | 内容 | 方法 | 结果 | 证据 |
|---|---|---|---|---|
| F-1 | ingest+中文两字词命中+噪音剔除+幂等+注入安全 | 构造样例 jsonl 模拟 SessionEnd payload,断言脚本 `/tmp/sa-test/assert_f1.py`(5 断言) | **PASS** | 输出 F-1/a/b/c/d/e 全 PASS(turns=2 证 tool 噪音剔除;幂等 assets=1 fts=1) |
| F-2 | 真实历史全量 sync+幂等 | `python3 tools/session_archive.py sync` 两轮 | **PASS** | 首轮 `{"total":1086,"added":1086,"failed":0}` 2.5s,库 26M;复轮 `added=0, skipped=1086` |
| F-3 | 真数据检索+项目/时间过滤 | search 三组:EUI-NEO submodule(2 命中)/高频词 测试=156 构建=159 提交=176 /「测试」+--project DevDesk+--since 2026-10-25 三重过滤断言 | **PASS** | 命中行均 DevDesk 且 started≥门槛 |
| F-4 | hook 接线后不阻塞退出 | 接线完成(async:true,timeout 30);**真触发待下一次 Claude Code 会话自然结束,当前未验证** | **未验证(待自然回归)** | `~/.claude/settings.json` SessionEnd 键;备份 `.bak-sa10-0322` |
| F-5 | 软删纪律 | 读路径 `deleted=0` 过滤已在 search SQL;段1 无删除入口(留给段2) | **部分(设计内)** | 代码 cmd_search |

## 独立审计(2026-10-31,general-purpose 子代理,938K tokens/33 工具调用)

- 总判定:**有条件通过**→ P1×1+P2×3 已修,定向复验绿。
- **P1(已修)**:parse_transcript 对「合法 JSON 非 dict 行」炸 AttributeError,破 exit-0 契约且中断 sync 全量对账 → 加 isinstance 防御;复验=坏行样例 ingest exit 0 无 traceback,正常行仍入库。
- **P2(已修)**:①ingest 整体 try/except 兜底(DB 不可写静默 exit 0);②sync except 补 rollback(防半途脏提交致 FTS/assets 失配);③证据物归档 evidence/2026-10-31-need10-audit/。
- **P3(已修)**:search 参数缺值/非法值明确报错 exit 2;spec 表结构字段名对齐(archived_at→created/updated)。
- P3 备忘(挂账):段1 UPDATE 路径显式 deleted=0,软删后源文件变更会被「复活」——段2 必须定义冲突口径;pool_soft_delete 子行不随删(孤儿,无害)。
- 修复回归断言:evidence/2026-10-31-need10-audit/assert_audit_fix.py(P1/P2/P3 全 PASS + sync 幂等保持)。

## 过程缺陷记录(修复链)

1. INSERT 占位符 16≠15 → 修
2. FTS MATCH 用 join 别名失效 → 子查询包住 MATCH 修
3. **fts 表写成普通 CREATE TABLE**(UNINDEXED 被宽松放行)→ CREATE VIRTUAL TABLE ... USING fts5 修;正式库因此重建(数据可 sync 再生,零损失)
4. **FTS5 unicode61 不切中文**(整句单 token,中文必查不中)→ CJK bigram 入库/查询两侧同法预切分修(本需求最大坑)
5. bm25() 外层作用域不可见 → 子查询带 rank 修
6. 查询词引号注入 → 剔无实义词+引号双写修

## 已知边界

- ZCode 内核会话不采集(POC:hook 事件集无 SessionEnd),见 spec
- 子代理转写(~754 个)不在段1 范围,见 spec 非目标
