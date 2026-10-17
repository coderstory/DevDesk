# 02 · 「同步 = 整库 dump 上云」零知情披露

Status: `needs-triage`
实现: 已完成 0a58276（2026-10-17）
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

## Comments

### 实现（0a58276，2026-10-17）

范围收在本票「修复方向 1（卡片文案改准 - 最小）」，不动布局：

- `src/module_settings/github_sync_card.h/.cpp` 新增两个自由函数（与 `drawGithubSyncCard` 并列）：
  - `disclosureLabelFor(table)` —— 表名 → 中文标签（`claude_call_logs`→「调用日志」、`claude_run_settings`→「运行设置」、`github_sync_state`→「Token 与同步状态」）；未知表名**原样返回**，不编造。
  - `githubSyncDisclosureText()` —— 由 `core::sql_dump::defaultSkipTables()` **驱动**拼文案，覆盖三点：笔记 + 整库数据库快照 / 明确列出不上传的表 / GitHub 私有仓库。实际文案：`同步笔记与整库数据库快照到 GitHub 私有仓库；不上传：调用日志、运行设置、Token 与同步状态。`
- 卡片描述行（`github_sync_card.cpp` `<id>.desc`）改渲染该文案。描述行单行固定高（`kDescH=18`，卡片高 `kGithubSyncCardHeight=460` 固定），文案只压缩措辞，未新增行、未改行高/卡高。
- token 口径绑定：token 存于 `github_sync_state`（列 `token`），该表在 skip 清单内 → 其标签「Token 与同步状态」即「token 不上云」的实现绑定表述（改 skip 清单时标签随之变）。

### 防漂移用例

`tests/test_modules/test_github_sync_card.cpp` 新用例 `github_sync_card_disclosure_text`：
- 对 `defaultSkipTables()` **每一张**表断言 `disclosureLabelFor(t) != t` 且该标签出现在披露文案里 → 以后加 skip 表却没补中文标签 / 文案与 skip 清单脱钩，均必红；
- 对照组：文案含「笔记」（否则「找不到标签」可能只是空串恒真）、含「快照」「私有仓库」；
- 断言卡片描述节点 `<id>.desc` 渲染的就是这份文案（防「只测函数漏绘制层」）。

定点：`--filter 描述行披露` 改前红（`FAIL text.find("快照")`）→ 改后 1/1 绿；全量 `--no-run`：`Total 1440 / Passed 1434 / Failed 0`（6 条为平台不对称跳过，非本改动引入）。

### 未做（后续增强，不在本提交）

- 「同步内容详情展开区」（修复方向 2）：本方案只在描述行一次性披露，无列表展开。
- 「首次同步前一次性知情确认」（修复方向 3）：未加首次弹窗。
- 公开仓警告文案未动：`github_sync_card.cpp` 的 `仓库为公开（public）：笔记、待办与密钥将公开可见` 仍是旧口径（提「笔记、待办」而未提整库）——本票验收判据里「公开仓警告与之一致」**未满足**，属已知遗留。

### 与票面/既有事实的核对

- 票面证据全部与代码一致（整库 dump、skip 3 表、token 不入 dump、唯一告知点、公开仓提示只在 public 出现）。
- 一处**指向订正**（非事实冲突）：票面第 25 行写「见同票 02 的承重不变量」是自指（本文件本身就是 02），实际「承重不变量」定义在 `docs/superpowers/specs/2026-10-15-github-sync.md:23`（决策 11）与 `:94`，并被 `.scratch/sync-product-gaps/spec.md:18` 引用为「02 的披露文案不得与它冲突」。本实现按该不变量表述：token 存 `github_sync_state` → 该表在 `core::sql_dump::defaultSkipTables()` 内 → token 不入 dump，与 `sql_dump.cpp:24-28` 注释一致。
- 无其他与票面不符的事实。

