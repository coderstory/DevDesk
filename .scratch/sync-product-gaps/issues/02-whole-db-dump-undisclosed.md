# 02 · 「同步 = 整库 dump 上云」零知情披露

Status: `needs-triage`
建议优先级: 中高
来源: handoff §3.C「S10」+ 2026-10-17 静态复核

## 现象

卡片文案只承诺「同步笔记和待办」，而实际推上去的是**整库 SQL dump**（除 skip 清单外的所有表）。用户无从知道自己的数据库整份被上传，也无从知道哪些表**没**上传。

## 证据

- 实际载荷：`src/core/sql_dump.h:22-24` — `exportToString/exportToFile` 的语义是「导出**整个数据库**为 SQL 文本（建表 DDL + 索引 DDL + INSERT + 触发器 DDL）」
- 唯一生产者：`src/module_settings/github_sync_engine.cpp:296-297` 调 `exportToFile(*db, dumpFile, ::core::sql_dump::defaultSkipTables())`
- skip 清单只有三张表：`src/core/sql_dump.cpp:24-28` — `claude_call_logs` / `claude_run_settings` / `github_sync_state`
  → 也就是说 **todo / markdown 笔记之外的表（含 claude profile 等）会一起上云**
- 卡片文案（唯一告知点）：`src/module_settings/github_sync_card.cpp:73`
  「将笔记和待办数据同步到 GitHub 私有仓库，实现多设备同步。」← 与实际载荷口径不符
- 现有的唯一风险提示只针对公开仓：`github_sync_card.cpp:298`「仓库为公开（public）：笔记、待办与密钥将公开可见」（且它只在 public 时出现）
- 已知代价已在规格里：`docs/superpowers/specs/2026-10-15-github-sync-api-design.md:172`「密钥列按决策 #8 进入同步 —— 已知代价：密钥进 git 历史」

## 影响（用户视角）

- 隐私知情缺口：用户以为同步的是「笔记 + 待办」，实际是整库
- 与 token 票的关系：token 本身**不上云**（`github_sync_state` 在 skip 清单内，见同票 02 的承重不变量），但用户无法从 UI 得知这一区别 —— 「什么上去了、什么没上去」整体是黑箱
- 反向风险：用户若因为「只有笔记和待办」才选择公开仓，判断依据就是错的

## 修复方向（待裁）

1. **卡片文案改准**（最小）：把「笔记和待办」改为「笔记、待办与本地数据库快照」，并说明「不含以下表：…」或指向「同步内容」说明
2. **加「同步内容」展开区/详情**：列出会同步的表与明确排除的表（含「Token 不上传」），静态可查
3. **首次同步前一次性知情确认**：第一次启用时弹一次说明，之后不再打扰

> 注意：文案里**不得**承诺「不含密钥列」—— 决策 #8 已接受密钥列随 dump 进 git 历史（`api-design.md:172`）；02 只承诺「token 不上云」（这条有 `defaultSkipTables` 承重）。

## 验收判据（待定稿）

UI 上任何一处都能读到「同步的是整库快照（含哪些/不含哪些表）」；公开仓警告与之一致；token 不上云的口径与 `defaultSkipTables` 的实现绑定表述（改 skip 清单时文案会被提醒）。
