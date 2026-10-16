# T4 修复轮（9 提交）复查 — 只读复审

- 日期：2026-10-16 ｜ 对象：`worktree-agent-github-sync-t4`（HEAD `33b475d`），修复轮 = `f7c26cd 5f7187f 707609e b1dc23e 26d7c15 1be7225 2f5e12b 941f1fe 33b475d`（父 = `7982d4c`，即两份前置审计的现场）
- 依据：`2026-10-16-github-sync-t4-audit-correctness.md`（C1–C11）/ `-architecture.md`（A1–A12）+ spec `2026-10-15-github-sync-api-design.md`
- 复跑（只读，worktree 内，`DEVDESK_TEST_TMPDIR=/tmp/t4-fix-reaudit-tmp`）：
  - `bash scripts/build-test-run.sh --no-run --filter github_sync` → `Total: 27 / Passed: 27 / Failed: 0` + 6 道静态守卫自检全过
  - 全量 `./build/tests/devdesk_tests` → `Total: 1413 / Passed: 1401 / Failed: 6 / Skipped: 6`；6 红 = 3 `db_backup`（环境性，前置审计已在 dev 基线同二进制复核）+ 3 `agent_ui`（base drift `a5b7117`，本分支不含），**与本批无关**
- 写动作：仅本文件

## 逐项裁决

| # | 提交 | 结论 | 一句话 |
|---|---|---|---|
| C2 | `2f5e12b` | ✅ 真解 | KeepLocal 清登记 → 下轮 push 按 New 重推（local wins），死锁解开；一句注释已过期 |
| C1 | `f7c26cd` | ✅ 真解 | 基线移入 `github_sync_state` 并整表排除出 dump；「备份也不含」的说法只对 dump 链成立（文件级备份含，且正确） |
| A1 | `707609e` | ⚠️ 部分 | 退出确实有界了；但「detach 安全」的论证不成立，残留 UAF 窗口，且未做审计要求的取消位 |
| A2 | `5f7187f` | ✅ 真解 | 真读结果行；判词仍只进日志（不外溢给调用方） |
| A3 | `b1dc23e` | ✅ 真解 | 白名单只认 Push/PullThenPush/FirstSync |
| A4 | `26d7c15` | ⚠️ 部分 | 闸挡住了「迟到 handler」，check-then-act 的指令级窗口仍在 |
| C3 | `941f1fe` | ✅ 真解 | 清单构造抽函数，四处冲突分支统一暂存 |
| C5 | `33b475d` | ⚠️ 部分 | 7 态真进生产、两遍判态成立；**但 pull 侧 LocalDeleted→Skip 与 push 侧 LocalDeleted-远端已改→Conflict 组合成死循环**，另留死件 `diffNotesByState` |
| C6 | `1be7225` | ✅ 真解 | 删除传播前比 sha，不静默删他人内容；边界已改（见 C5 死循环） |

## C2 — KeepLocal 清登记

- 修法成立：`settings_module.cpp:1307-1321` 在 KeepLocal 分支 `forgetSyncedNote`（sha+hash 双键）。下轮 push 无登记 → `classifyNote` 判 `New` → `pushActionForState` → Put，`shaIfMatch` 取树现值（远端无 → 无 sha 创建）`settings_module.cpp:1100-1111`。push 只写远端，**本地内容不会被覆盖**；PUT 成功后 `recordSyncedNote` 登记新 sha + 本地 hash → 下轮 InSync（`:1124-1131`）。
- 残留（低）：`settings_module.cpp:1312-1313` 注释仍写「按 New 走 **GET 现值**兜底」——`33b475d` 已改成走 listTree 的树 sha，注释失真。
- 残留（低，属设计取舍）：登记清掉后语义退回「无登记 = 本地领先」。若此后别的设备用同名重建了该笔记，本机下轮 push 会用树现值 sha 直接覆盖远端（不会 409、无弹窗）——这是「解开死锁」的代价，方向可接受，但应知悉。

## C1 — 基线表 + skip 路由

- 修法成立：`src/core/schema.h:309-312` 新表；`src/core/sql_dump.cpp:23-31` 整表排除；`github_sync_config.cpp:22-26` 五类键迁入（表内键去掉 `github_sync:` 前缀、值改纯文本）。跨设备唯一载体（SQL dump）不再携带基线 → 新设备基线为空 → pull 全 Download、push 无「登记有本地无」→ **不会再批量 DELETE 远端笔记**。用例非恒真：`sql_dump_import_keeps_local_sync_baseline`（B 自己的行在导入 A 的 dump 后仍为 `b-own-sha`）。
- **备份链的答案**：两条链都**不走** skip 集，且这不是缺陷——
  - `core::dbmaint::createBackup`（启动备份 `devdesk.db.backup-<ts>`）走 `sqlite3_backup` 整页拷贝 `src/core/db_backup.cpp:205-212`；
  - 拉取前备份 `backups/devdesk-<ts>.db` 走 `std::filesystem::copy_file` `settings_module.cpp:1361-1388`。
  两者都含 `github_sync_state`（也含 `claude_call_logs` 等其余 skip 表），是**同机备份**，还原时基线应随数据一起回退——去留与 dump 链不同是对的。
- 由此，`schema.h:301` 注释与 `f7c26cd` 提交信息里的「dump / 备份都不含本表」**不准确**（只对 dump 链成立）。同批 `github_sync_config.h:46,52-53,65,72` 与 `github_sync_config.cpp:120` 的注释仍在描述旧 key 布局（`github_sync:sha:*`、`save() 必须保留它们`）——与该提交改后的代码相反，属该批留下的文档失真。
- 旧库（无该表）：`applySchema(kSql)` 每次开库跑全脚本且幂等（`database.cpp:37-43`、`schema.h:1-6`）→ 表被自动补建，基线从空开始；旧 `app_config` 里的基线键已无任何读者（全仓 grep 0 命中），`save()` 顺手清掉（`github_sync_config.cpp:178-201`）。且同步功能与本批同批落地、无已发布版本 → 无真实旧基线，"不写迁移" 口径不产生代价。

## A1 — 退出等待有界

- 目标达成：`waitOrDetachSyncThread`（`settings_module.cpp:710-731`）对「新起」与「已在飞」两条路都等 5s，到点 `detach()` → `githubSyncThread_.joinable()==false` → `onStop` 的 `join`（`:214`）瞬间返回，退出不再被整轮推送拖住。提示键在**同步写库**后（`:723-727`）才 detach，下次启动卡片可见（onStart 读 `hasUnsyncedNotice`）✅；只在 `dirty` 仍为真时置位，方向正确。
- **残留（真问题）**：注释的论证「SettingsModule 是进程生命期对象（析构晚于全部 atexit 处理器）、appDatabase 堆持有同寿」（`:711-714`）**推不出安全结论**——模块是 `getRouter()` 的函数局部 static（`main.cpp:224`），库是 `openAppDatabase()` 的 static unique_ptr（`main.cpp:126`），二者都在 atexit 处理器跑完之后、**进程终止之前**被析构。detach 线程没有取消位，仍在跑整轮推送：它写 `githubSyncStore_`/`githubSyncMutex_`（析构后 = UB）、调 `stateSet(*db_, …)`（句柄已 close/free）、结尾还调 `core::platform::requestUiUpdate()`（`settings_module.cpp:798` → EUI-NEO `window::postEmptyEvent`）。窗口小、概率低，但这是审计 A1 明确点过的面，且要求的「原子取消位」没做。

## A2 — integrity_check 真判

- 修法成立：`github_sync_config.cpp:363-390` 改 prepare/step 读首行，非 `"ok"`（含无结果行）`LOG_ERROR`。健康库新旧写法行为一致，判据只在坏库分叉——提交信息对此已如实说明。
- 残留（低）：判词不外溢，导入仍算成功（审计原建议是「LOG_ERROR + 报错」）。同类「exec 丢行」写法另有 `settings_module.cpp:1371` 的 `PRAGMA wal_checkpoint(TRUNCATE)`（只 LOG_WARN，无害）。

## A3 — 清提示收窄

- 修法成立：`settings_module.cpp:1514-1537`，`githubSyncLastAction_` 在启动时即置（`:775`），白名单 `Push/PullThenPush/FirstSync` 才清 dirty / 记节流时点 / 清 F5 黄字；错误行清除对所有成功动作保留（与提交说明一致）。Test 与纯 Pull 不再抹掉未同步语义 ✅。

## A4 — 导入事件闸

- 改对的部分：onStart 先置真再订阅、onStop 先置假再退订再停线程（`claude_module.cpp:309-336`），「handler 在退订后才进入」这一类被挡住，注释也改对了。
- **残留**：handler 是 check-then-act——`load(acquire)` 读到真之后、`startQuotaRefresh()` 之前，onStop 可以完成 `store(false)+stopQuotaRefresh()`；此时 handler 会把 QuotaRefresher 复活（`startQuotaRefresh` 的 `if (quotaRefresher_) return` 只拦「没停过」的情形）。窗口从「整个 handler 体」压到「几条指令」，但没有闭合；要真闭合需把「查闸 + 起线程」与「关闸 + 停线程」放同一把锁内。前置审计给的建议本身也是这个 check-then-act 形态，故按「照建议实现但建议不充分」记，不视为未修。

## C3 — 覆盖清单

- 修法成立：`stashOverwriteListForConflict`（`settings_module.cpp:915-925`）接受 Put 名集，四处冲突分支统一经它暂存（`:1064` 判态冲突、`:1078` sql PUT 409、`:1117` 笔记 PUT 409）。清单内容 = `data/database.sql` + 本轮 Put 集，F6 在笔记路径不再交白卷。

## C5 — 7 态进生产

- 表核对（穷举，与 `classifyNote` 优先级一致）：push `github_sync_logic.cpp:130-146`（InSync→Skip / New→Put / LocalModified→Put / RemoteModified→Skip / Diverged→Conflict / LocalDeleted→远端未动或无→Delete、远端已改→Conflict / RemoteNew→Skip）；pull `:148-161`（New·RemoteNew·RemoteModified·Diverged→Download；InSync·LocalModified·LocalDeleted→Skip）。与 spec F1 的关系：F1 只定义「本地侧三态检测」，`Diverged`/远端比对是本批刻意的超集（决策 #7 的 409 弹窗），语义相容，**但 spec 未回写**（口径欠账，与 C4 同批）。
- 两遍执行的原子性：冲突扫描（`settings_module.cpp:1004-1066`）在任何 PUT/DELETE 之前完成，中止时不推任何笔记、database.sql 也不推 ✅。唯二例外要写清楚：① 中止前**可能已 `createRef`**（`:961-981` 分支缺失自动建分支，是远端写），「不动远端」的说法不严；② 执行期 PUT 409 仍走冲突路径（`:1113-1117`），但**执行期 DELETE 409 走的是纯失败**（`:1152-1160`），下一轮判态会自我纠正，属小裂缝。
- `push` 新增 listTree 的成本：净**变便宜**——前置版本对 New/缺 sha 的 PUT 与每个 DELETE 都单独 `getFile`（`git show 33b475d` 的删行），现在 1 次树列举取而代之；树的 sha 与 `putFile` 返回的 blob sha 同口径。新增副作用：树被截断（超大仓）现在会让**整个 push** 硬失败（`github_api.cpp:562-566`），与 pull 同待遇。
- **新引入问题（本次复查最重，必修）**：`pull` 对 `LocalDeleted` 一律 Skip（`:157-158`，用例 `test_github_sync_logic.cpp` 里 `delActions.empty()` 锁着），而 `push` 对「本地删 + 远端被别人改过」判 Conflict 并**中止整轮 push**。于是 409 弹窗的主动作「拉取远端后再推」（`settings_module.cpp:841-851`）做了 pull（跳过该笔记）再 push（同样 Conflict）→ 弹同一张框，**无限循环**；在此期间**所有**推送（含自动同步）都被这一条笔记卡死。`1be7225` 的注释「拉取后再推会先把远端内容恢复回来」在 `33b475d` 后已不成立（改前 pull 确会因远端 sha 变而下载）。触发场景现实：笔记在 A 设备改过而 B 未拉，用户在 B 删掉它 → 下一次推送即入环。出口只剩「在 GitHub 网页删/改远端同名文件」或「本地把文件恢复成登记 hash」这类手工动作，弹窗内无出路。
  - 建议落点（任一）：① pull 对 `LocalDeleted + 远端已改` 也 Download（恢复文件 → 下轮 InSync，回到改前语义）；② push 侧只跳过这一条删除并出黄字（前置审计 C6 的原始建议「不删，报『远端已改，删除已跳过』」），其余笔记与 database.sql 照推；③ 弹窗加第三个动作「以远端为准（放弃本地删除）」。
- 边界变化：登记 sha 为空（PUT 响应解析失败的旧记录）时，`remoteShaMatches` 恒假 → `LocalDeleted` 会走 Conflict（不是 `1be7225` 声称的「维持原行为（删）」），`LocalModified` 会退化成 `Diverged`。方向保守（不静默删/不静默覆盖），但两者都落在上面那条无出路的冲突路径上。
- 死件搬家：`diffNotesByState`（`github_sync_logic.h:123`）与 `FileChange`（`:99`）在 `33b475d` 之后**零生产调用点**（全仓仅测试引用）——C5 的原始 finding 正是「测试专属死代码」，本批把它换了个位置，审计给的另一选项（删）未做。

## C6 — 删除传播比 sha

- 修法成立：判定集中在判态阶段（`github_sync_logic.cpp:138-141` + `settings_module.cpp:1049-1051`），DELETE 用树里现值且仅在 `remoteShaMatches || !remoteExists` 时执行（`:1149-1161`），不再「GET 现值即删」。用户可见面：summary 明写「已被其他设备改动，本地删除未传播（409 冲突）」+ 409 弹窗（`:1557/1576`）✅。
- 残留即上节死循环（此路径的唯一出口不通）；另见「登记缺 sha」边界变化。

## 回归 / 用例增量 / 提交卫生

- `--filter github_sync`：**27/27/0 全绿**（含改写后的 pull 表驱动与新增动作表用例）。命中数 24→27 的原因要写清楚：`f7c26cd` 把 `sql_dump_skip_defaults` 的描述改成含 `github_sync_state`（含过滤词）+ 该提交新增 2 条描述含 `github_sync_state` 的用例。
- 新增用例 **+3**（不是 +7）：`tests/test_core/test_sql_dump.cpp` 5→7（`sql_dump_excludes_sync_baseline` / `sql_dump_import_keeps_local_sync_baseline`，`f7c26cd`）、`tests/test_modules/test_github_sync_logic.cpp` 7→8（`github_sync_action_tables_from_state`，`33b475d`）。`REGISTER_TEST` 总数 `f7c26cd^` 1434 → HEAD 1437（`git grep -c` 逐文件比对，只有这两个文件变）。运行口径 1410（前置审计 @`7982d4c`）→ 1413 ✅ 与 `33b475d` 提交信息一致；任务书里的「1413−1406=+7」在仓库里找不到支撑（1406 无出处）。
- 6 红与前置审计逐条同名（3 `db_backup` + 3 `agent_ui`），本批 9 提交未触及这两处代码。
- 提交卫生：9 提交各自只动声明的文件（`git show --stat` 逐条核对：C1 = schema.h/sql_dump.{h,cpp}/github_sync_config.cpp/test_sql_dump.cpp；C5 = github_sync_logic.{h,cpp}/settings_module.cpp/test_github_sync_logic.cpp；A4 = claude_module.cpp；其余单文件 settings_module.cpp / github_sync_config.cpp），无越界、无夹带。

## 六维度（本批）

| 维度 | 结论 |
|---|---|
| 正确性 | 7/10 — C1/C2/C3/C6 真解；C5 的方向对但组合出**整轮同步卡死的死循环**（唯一必修项）；A2/A3 落实 |
| 回归面 | 8/10 — 27/27 定向 + 1413 全量与前置审计同红项；C2/C5 动了 pull/push 两侧语义，卡死正是「两侧各自都对、组合不对」的回归 |
| 平台对称性 | 7/10 — 无新增平台分支；`sql_dump`/`github_sync` 改动平台无关；本批仍**只在 macOS 验过**，Windows 侧未跑 |
| 静默失败面 | 6/10 — 判态前置 + 冲突清单 + 删除跳过提示都在；但 A1 的 detach 残留 UAF 面、A2 判词只进日志、C5 的死循环表现为「同一个弹窗再来一次」（无进展提示） |
| 测试有效性 | 7/10 — 新增 3 条都非恒真（导入保留基线那条能抓 skip 移除）；动作表 7 态穷举可判态回退。但**编排层仍零覆盖**（C2/C5/C6 的接线无断言），且 **C5 自己把死环锁进用例**（`delActions.empty()` 只锁了 pull 半边，无人把两侧组合起来断言） |
| 文档口径 | 5/10 — `schema.h:301`「备份都不含本表」、`github_sync_config.h:46/52-53/65/72` + `.cpp:120` 旧 key、`settings_module.cpp:1312` 的 GET 兜底均为失真；spec F1 未回写 Diverged/C4 偏离 |

## 总裁决：**再修**（1 项必修 + 3 项随修）

**必修**
1. **C5 的 LocalDeleted 冲突无出路** —— 要么 pull 对 `LocalDeleted + 远端已改` 下载（恢复改前自愈），要么 push 只跳过该条删除并出黄字（前置审计 C6 原始建议）。现状是整轮 push（含自动同步）被一条局部冲突永久卡死，且用户的唯一按钮原地打转。

**随修（低成本，同批做完）**
2. A1 注释与实现对齐：加原子取消位（线程体在每步与 DB 写前查），或明确写下「接受退出期 UAF 窗口」并把这个取舍登记为已知代价——当前注释的论证链是错的，会误导后来者。
3. 文档失真三处：`schema.h:301`（备份链口径）、`github_sync_config.h` 状态键那四段 + `.cpp:120` 改指新表、`settings_module.cpp:1312` 删「GET 兜底」。
4. 死件处置：`diffNotesByState` / `FileChange` 已无生产调用点——删或按 C5 的选项 ② 明确标注为参考模型（现状是同类问题换了个位置）。

**可选**
5. A4 若要真闭合 check-then-act：把「查闸+起线程」与「关闸+停线程」收进同一把锁。
6. 执行期 DELETE 409 走冲突路径（与 PUT 一致）；spec 回写 Diverged / C4。

> 未跑过的东西（诚实边界）：真机退出补推与退出期 detach 行为（本次只跑单测与静态判读，未起 app）、真机双设备冲突场景（死循环结论来自纯件判读 + 本提交自己的用例断言，未上真机复现）、Windows 侧编译与运行、pull/push 编排层的端到端用例（不存在）。本次实跑证据只有上列两条命令的输出。

---

# 复核（二轮修复，2026-10-16 晚）

- 对象：`worktree-agent-github-sync-t4` HEAD **`d96dc77`**，二轮 4 提交 `56bafe7`(C5 死锁) / `1a3766c`(A1 取消位) / `1edd8f0`(死件+schema 措辞) / `d96dc77`(A2 升格报错)
- 复跑（只读，worktree 内，`DEVDESK_TEST_TMPDIR=/tmp/t4-fix-reaudit-tmp2`）：`bash scripts/build-test-run.sh --no-run --filter github_sync` → **`Total: 26 / Passed: 26 / Failed: 0`**，退出码 0，6 道静态守卫自检全过（26 = 上轮 27 − 删掉的死件用例 1 条）。提交卫生：4 提交 `--stat` 各只动声明文件（56bafe7 = logic.{h,cpp}/settings_module.cpp/其测试；其余单文件或 4 文件含 schema.h）。

## 1. C5 死锁 — ✅ 真解（`56bafe7`）

- 两态判定正确：`pullDecisionForState(state, remoteShaMatches)`（`github_sync_logic.cpp:126-163`）的 LocalDeleted 臂 `remoteShaMatches ? Skip : Download`。传入的 `remoteShaMatches = hasRecord && rec->second.sha == sha`（`:160`）——LocalDeleted 蕴含 hasRecord，故该式就是「远端 sha == 登记 sha」的干净判据，无恒真/恒假分支；顺带把上轮「登记 sha 为空」两处边界一并收口（空 sha → 恒不等 → 走 Download 恢复 → 下轮 InSync，不再 Conflict）。
- 恢复链完整：pull 的 Download 分支写盘 `settings_module.cpp:1355-1359` → `recordSyncedNote(fileName, remoteNotes[name], contentHash(content))` `:1361-1366`（树 sha + 落盘内容 hash）→ 下轮判态 hasRecord/localExists/localHashMatches/remoteShaMatches 四真 → `InSync` → push Skip。远端若在恢复后又被改：localHashMatches 真、remoteShaMatches 假 → RemoteModified → push Skip，仍无 Conflict。无二次死环。
- 测试真钉死（非恒真）：`delUnchanged`（远端 sha == 登记）→ 空集；`delChanged`（远端 sha ≠ 登记）→ 恰 1 条 Download（旧代码此处为空集，先红成立）；恢复后逐个断言 `classifyNote==InSync` + `pushActionForState==Skip` + `pullNoteActions` 无可下载项。**诚实边界**：恢复后的 local/synced 是测试**手工构造**的，未驱动 settings_module 的写盘+登记代码，故「登记更新」这一步靠的是既有 Download 路径（未改、被其它下载用例间接覆盖），不是本用例直接钉死——链路结论成立，证据强度略低于用例包装给人的印象。
- 侧效应核对：新增的 Download 会进 `downloadCount`（`:1253`）→ 触发拉取前的笔记整目录备份（F6）✅；`settings_module.cpp:1064-1066` 新注释「拉取会下载恢复远端内容」对 Diverged 与 LocalDeleted-远端变两者都成立 ✅。

## 2. A1 取消位 — ✅ 有界成立，注释未同步（`1a3766c`）

- 检查点与长操作对齐度：实际 7 处（定义 1 + 使用 7：`807` 唤醒前、`822` 动作入口、`891` FirstSync 入口、`953` push dump 前、`1118` push 每 PUT/DELETE 前、`1274` pull 备份/导入前、`1329` pull 每下载前）。**未覆盖**的是 push 的中段 ②③⑤（getRepo / getFile 探针＝**整份远端 dump 下载** / createRef / listTree / PUT database.sql）——过了 `953` 之后到 `1118` 之前，最坏是「一次大请求 × 3 次重试（30s 超时 + 1/3/9s 退避）」，量级 ~2 分钟，比 5s 闸长；即 detach 仍会被走到，只是从「整轮」压到「至多一个网络阶段」。方向对、收益真，但提交信息「只剩卡在网络调用中一格」的说法偏乐观（是 3~4 次调用，其中含最长的那次）。补 3 行同款检查（②③⑤ 之前）即可闭合，非阻塞。
- 顺序无竞态：置位（`716`）先于轮询，`done` 观察到即返回不 detach、未观察到才 detach（`729`）✅；`githubSyncAbortRequested_` 在每次动作启动时清位（`:783`），且 `runGithubSyncExitPush` 全仓唯一调用点是 `main.cpp:257` 的 atexit（`settings_module.cpp:2095-2097` + `gExitHookTarget` 在 onStop 注销 `:220`）→ **detach 只发生在退出中，之后不可能再起第二个同步线程**（UI compose 已停，`maybeAutoGithubSync` 不会再跑），无「detach 后双线程」风险 ✅。`done` 与取消位均为 acquire/release ✅。
- **未闭合**：`settings_module.cpp:715-718` 的四行注释仍是上一轮点名的错误论证（「detach 在本路径安全——SettingsModule 是进程生命期对象…appDatabase 堆持有同寿」），与同一提交在 commit message 里写的取舍声明矛盾；成员注释（`:2029-2032`）也只讲了取消位、没写残余窗口。取消位（选项①）做了，但「把错注释改对」这半边没做——后来者读代码仍会拿到那个不成立的论证。

## 3. 死件 + schema 措辞 — ✅（`1edd8f0`）

- `grep -rn "diffNotesByState\|FileChange" src tests docs/superpowers/specs` = **0 命中**，双轨消除干净；连带删掉的 `github_sync_diff_notes_by_state` 测的语义仍由 `github_sync_classify_note_all_states`（判态）+ `github_sync_action_tables_from_state`（态→动作）覆盖，纯件层无覆盖面损失。
- `schema.h:300-303` 改后与备份链实测行为一致：dump 排除（`sql_dump.cpp:23-31`）、本地备份是整库文件拷贝（`db_backup.cpp:205` sqlite3_backup / `settings_module.cpp:1378` copy_file）**含且应当含**——上轮那句不实措辞已消。上轮提到的 `github_sync_config.h:46/52-53/65/72` 旧 key 注释本轮未动（仍在错，属遗留小项）。

## 4. A2 升格报错 — ✅ 语义与路径都通（`d96dc77`）

- 错误码真实存在且有串：`core/result.h:22` 定义 + `:51` `errorCodeToString` 映射；语义贴切（`sqlite_db.cpp:18` 拿 SQLITE_CORRUPT 也映射到它）。
- UI 路径通：`importDatabase` fail → `settings_module.cpp:1305-1310` 置 summary「导入失败：…」+ `return false` → harvest `githubSyncResultOk_=false` → 卡片红字 + 详情/409 弹窗面 ✅（不再是「只进日志」）。
- 事件序无重发：失败分支**显式 `emit(kDbImportEnd)` 后 return**（`github_sync_config.cpp:388-393`），尾部那次 `emit(kDbImportEnd)`（`:399`）被 return 跳过 → begin/end 恰一对 ✅；`imported` 不发 → 各模块不重载已损坏的库 ✅（与 `test_github_sync_card.cpp:624` 那条事件序用例不冲突）。校验跑不起来（prepare/step 失败）维持 WARN 不阻断，已注明「判不了≠判坏」，与旧行为一致 ✅。
- 残留（低）：失败摘要不提示刚生成的库备份路径（`backups/devdesk-<ts>.db`），且启动期自动还原只认 `<db>.backup-<ts>` 命名（`db_backup.cpp:107-110` 的前缀匹配，不含 `backups/` 子目录）——用户只能自己去备份目录找；同 A7 的可见面遗留，非本轮引入。

## 复核裁决：**可合并**

- 上轮唯一必修（C5 死循环）已真解且有用例钉住；A2/A3/C1/C2/C3/C6 维持 ✅；A4 维持 ⚠️（指令级 check-then-act 窗口，低风险）。
- 合并前顺手（各 ≤5 行，均非阻塞，无功能风险）：① `settings_module.cpp:715-718` 改掉「detach 安全」的错误论证、把「残余窗口 = 线程尾部撞 static 析构」写进代码；② `1a3766c` 补 ②③⑤ 三处取消检查点；③ `github_sync_config.h` 四段旧 key 注释改指 `github_sync_state`。
- 合并后的门禁照旧：主树全量 + `ctest -L probe` + Windows CI；本轮只跑了 `--filter github_sync`（26/26/0）与静态守卫自检，全量与真机均未跑。

> 二轮复跑的诚实边界：只跑 `--filter github_sync`，**未跑全量**（上轮全量 1413/1401/6/6 的 6 红与本批无关，本轮改动集中在同名文件，未重跑确认）；未起 app、未验真机退出/双设备场景；未在 Windows 编译。
