# 03 · 大笔记（>1MB）与分页超量边界未测未披露

Status: `needs-triage`
建议优先级: 中
来源: handoff §3.C「S11」+ 2026-10-17 静态复核
实现: 部分完成（① 先测已落；② 分页边界仍需实测 GitHub；③ 披露文案未做）`6b10975`（2026-10-17）

## 现象

两处边界既**没有测试钉住**、也**没有对用户披露**：

1. 单篇笔记 > 1MB（GitHub Contents API 的行为分叉）
2. 远端条目量级到分页上限（超量后的拉取行为）

## 证据

- API 层**已实现** >1MB 的分叉：`src/platform/github_api.cpp:30-31`
  「>1MB 的文件 Contents API 不回 base64 正文（`encoding="none"`），改用 `Accept: application/vnd.github.raw` 直取原文」
  实现见 `:538-540`（`kAcceptRaw` 直取，sha 仍取元数据）
- 现有断言只覆盖到 API 层这类行为（如 `tests/test_platform/test_github_api.cpp:327`「>1MB 走 Accept raw 直取原文」）
- **同步链路层无对应测试**：`tests/live/github_sync_live.cpp`（真机验收驱动）与 `tests/test_modules/test_github_sync_engine.cpp`（21 用例）均未覆盖 >1MB 笔记的端到端往返
- **分页上限无测试也无披露**：拉取侧的条目量级边界在测试与 UI 文案中均无体现（卡片文案只说「同步到私有仓库」，见 `github_sync_card.cpp:73`）

## 影响（用户视角）

- 一篇超长笔记可能表现为「同步成功但远端内容不全」或「反复失败」—— 用户拿不到解释
- 库大到某一量级后，拉取可能只拿到前若干页而用户以为已经同步完整 → **静默不完整**（比失败更危险）
- 与 02 叠加：用户既不知道同步了什么，也不知道同步是否完整

## 修复方向（待裁）

1. **先测**：在 engine 层加「>1MB 笔记往返」用例（假发送器即可离线），在 live 驱动加一条真机大文件场景
2. **再定行为**：分页到上限时是「截断 + 明确告知」还是「失败并告知」—— 需先确定 GitHub API 在目标量级下的真实行为（属未验证区，勿凭猜设定）
3. **披露**：把「单篇上限」「总量上限」写进卡片说明或「同步内容」详情（与票 02 可合并展示）

> 顺序建议：1 → 2 → 3。**2 必须实测后再定**，否则是在未验证区写规格。

## 验收判据（待定稿）

>1MB 笔记往返有离线用例；分页边界的真实行为有实测记录；两个边界在 UI 上有对应文案。

## Comments

### 实现（`6b10975`，2026-10-17）—— 只做「① 先测」那一半

范围：引擎层加一条离线用例，**不碰** ② 分页边界（未验证区，凭猜写规格 = 污染）、**不碰** ③ 披露文案。

- 落点：`tests/test_modules/test_github_sync_engine.cpp:978` `gsync_engine_huge_note_roundtrip_push_then_pull`（21 → 22 用例；其余 21 条与假发送器零改动）。
- 钉住的两端（判据全落在**真实请求**（假发送器录下的 method/url/headers/body）与**真实落盘内容**上，不断言假发送器自己的路由字段）：
  - **推送**（1.4e6 B > 1MiB）：恰好一条 `PUT /contents/notes/big.md`，`Content-Type: application/json` + `branch` 显式；请求体 JSON 的 `content` 字段 base64 解码后与本地原文件**逐字节一致**（长度 + 首/中/尾三处标记 + FNV 全文 hash）→ 无截断；同时得到「引擎侧**无单篇笔记体积闸**」的可观测面（全文进请求体，体量 > 原文；73MB 闸只作用于 `data/database.sql`）。登记：sha 取 PUT 响应、contentHash 是整篇的。
  - **拉取**（异设备：空笔记目录 + 空登记）：同一 URL **两跳** GET —— 先 `Accept: application/vnd.github+json`（元数据 `encoding="none"`/`content` 空，取 sha），再 `Accept: application/vnd.github.raw`（原文）；两跳都显式带 `ref=trunk`。真实落盘文件长度 == `1_400_000` 且首/中/尾逐字节 == 设备 A **真实发上去的字节**，contentHash 与本地原文一致 → 往返闭合。
- 为什么这么断言：只断长度区分不了「整篇正确」与「中段错位 / 尾部丢 `=` 填充」，故首/中/尾三处 + 全文 hash；对 1.4MB 串刻意不用 `==`（失败会把 1.4MB 灌进日志）。假发送器的 `Recorded` 增录 `headers`：raw 二跳与元数据跳 URL 完全相同，只有 Accept 头能区分（不录头就只能靠响应体反推 = 断言假发送器自己的字段 = 恒真）。
- TDD 轨迹：**本用例首次运行即绿**（离线未发现 >1MB 笔记路径缺陷）→ 补一次**变异验证**证明断言非恒真：把 `src/platform/github_api.cpp:548` 的 raw 直取改成 `substr(0,512)` → 本用例红（`FAIL ...:1084: file_size != kBytes`）；已还原（该文件零 diff）。
- 门禁：定点 `--filter 1MB` 3/3、`--filter gsync` 22/22；全量 `--no-run` **Total 1441 / Passed 1435 / Failed 0**（6 skip = 平台不对称，非本改动）+ UI probe 7/7。

### 离线覆盖不到的部分（如实登记，未伪造）

1. **服务端对 Accept 媒体类型的真实响应**：离线用例只能断言「请求里发了这个头」，证不了服务端行为（T4 审计 C11 的原话）。→ 本次用**真网实测**补上（见下第 1 条），故不再是悬空项。
2. **真机 live 驱动未加大文件场景**：`tests/live/github_sync_live.cpp` 要真网 + 真 PAT 且默认不构建，本票未动。
3. **超限笔记**（>100MB PUT 请求体 / 原始 >73MB）的失败形态未测：单测里造 73MB+ 笔记不现实（内存 ~300MB+），且 413/422 的判定需真网。当前可从代码读出：笔记路径无闸 → 只能等远端拒绝，摘要落「上传笔记 notes/X.md 失败：HTTP 413」。
4. **② 分页边界**（含「超量时是截断+告知还是失败+告知」）与 **③ UI 披露文案**：本票明确不做。票面验收判据里的「分页边界的真实行为有实测记录」「两个边界在 UI 上有对应文案」**未满足**。

### 发现（未修） / 与票面不符之处

1. **`Accept: application/vnd.github.raw`（裸值，无 `+json`）不是缺陷 —— 真网实测判定**（T4 审计 C11 / `docs/superpowers/plans/2026-09-22-backlog.md:70` 的「>1MB 笔记待真机核」可关）。2026-10-17 对公开仓库**无认证** GET（零写入、无 PAT）：
   - `GET /repos/git/git/contents/po/bg.po`（`size` = 1,088,754 B > 1MB）+ `Accept: application/vnd.github+json` → 200，692 B 元数据：`encoding="none"`、`content=""`、`sha=e11e53618242d5dbeeab1ff15707c7ec88c907cc`。
   - 同 URL + `Accept: application/vnd.github.raw` → 200，`Content-Type: application/vnd.github.raw`，**1,088,754 B**，且该体量的 git blob sha1 == 元数据 sha → 拿到的就是逐字节原文（长度也与元数据 `size` 一致）。
   - `Accept: application/vnd.github.raw+json` 行为完全相同 → **无需**改成 `raw+json`（API 层当前值与平台层既有断言都保持）。
   - **未做**：没有把这两处「待真机核」标注改掉（不在本票范围，且 `backlog.md` 可能与在飞 agent 冲突）→ 建议另开 `docs:` 一行销账。
2. **引擎层笔记路径无体积闸**（与 `database.sql` 的 `kMaxDumpRawBytes` 不对称）：属 ③ 披露面 / 待拍板，**未修** —— 在未验证区（PUT 上限对笔记是否同口径）写闸就是污染规格。
3. **空内容会多打一次 raw 二跳**（低价值，仅记录）：`src/platform/github_api.cpp:528` 的判据是 `!content.empty()`，故 0 字节文件（base64 + `content=""`）不早退、也走 raw 分支。正确性无损（raw 回空体），只是每篇多一发请求。
4. 票面事实核对：引擎层 21 用例 ✓（现 22）、API 层分叉位置 `github_api.cpp:30-31` / `:538-540` ✓、平台层既有断言 `test_github_api.cpp:327` ✓ —— 无其他与票面不符的事实。

### 建议的下一步

- ② 分页边界：按票面顺序**实测 GitHub 后**再定规格（本票不动）。
- ③ 披露：把「单篇/总量上限」与「>1MB 走 raw」写进卡片说明（可与票 02 的披露行合并）。
- 销账：`docs/superpowers/plans/2026-09-22-backlog.md:70` 与 T4 审计 C11 的「待真机核」按上面第 1 条标记为已核实（非本票范围）。
