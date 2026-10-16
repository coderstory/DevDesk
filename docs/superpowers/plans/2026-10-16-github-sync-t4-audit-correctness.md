# T4 独立审计（lens = C++20 系统开发专家 / 正确性轴）— GitHub Sync 编排纯件与拉取重写

- 审计对象：`worktree-agent-github-sync-t4`，仅下列 6 提交（另一半由另一 agent 审 F3/F5/F6/F8 + `7982d4c`）
  `07e2096`(S1 skip 引用) / `3486909`(S2 7 态 + FNV-1a) / `3d63f25`(S3 体积上限) / `1e748b4`(M1 listTree + 拉取重写) / `987d4fb`(M2 hash 登记 + 只推变更 + shaIfMatch) / `73799c8`(字面量消重)
- worktree：`/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-github-sync-t4`（HEAD `7982d4c`，含 T4 后续票面；findings 行号按 HEAD 复核，凡按提交时刻才成立的另标 `@<sha>`）
- 依据：spec `docs/superpowers/specs/2026-10-15-github-sync-api-design.md`（决策 #10–#20 + F1–F9）｜`2026-10-15-github-sync-t4-prework.md`
- 复跑（只读，worktree 内）：`DEVDESK_TEST_TMPDIR=/tmp/t4audit-tmp bash scripts/build-test-run.sh --no-run --filter github_sync` → `Total: 24 / Passed: 24 / Failed: 0`；全量 `Total: 1410 / Passed: 1398 / Failed: 6 / Skipped: 6`（6 红全部与本批无关：3 条 `db_backup` 在 dev 基线同一二进制同样红＝环境性；3 条 `agent_ui` 是 base drift——dev 已由 `a5b7117` 修，本分支不含该提交）
- 审计日期：2026-10-16（只读审计，除本文件外零写动作）

## Findings

| # | 位置 | 严重度 | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| C1 | `src/module_settings/settings_module.cpp:1138,1147`（读登记与算动作）；`@1e748b4` 处同逻辑在 sql 导入**之后**读（`git show 1e748b4:src/module_settings/settings_module.cpp` 的 "③ 笔记" 段）；`src/core/sql_dump.cpp:235` + `src/core/schema.h:268` | **高** | **同步登记（`github_sync:sha:*`/`hash:*`）本身在 `app_config` 里，随 dump 跨设备转移**；而笔记拉取判据正是「远端 sha == 登记 sha → 不下载」 | **M1 提交态**：新设备/缺笔记的设备拉取 → 导入远端库（登记里已有 `notes/N`）→ 用**刚导入的**登记比对 → N 判定「远端未变」→ 不下载；下一次 push 侧 `diffNotesByState` 见「登记有、本地无」→ **DELETE 远端 N**（删除分支无冲突检测，`settings_module.cpp:1049-1079`）→ 新设备首次启用同步即可清空远端全部笔记。HEAD 态（F6/F3 把动作计算提到导入前）happy path 已中和，但**导入后拉取一旦中途失败**（GET 429/超时、写盘失败 → `return false`）→ 未下完的笔记留在「已登记未落盘」态 → 下一次 push 同样**批量删除远端笔记** | 把同步基线排除出被同步载荷：导出时剔除 `github_sync:sha:*` / `github_sync:hash:*` / `first_sync_done` 行（或基线挪到不进 dump 的表）；另给 push 的 DELETE 加「登记须由本机写入」的判据（如登记值带 device id）。同根因在 T3 的 pull 已存在（T3 拉取根本不下载笔记），修在基线层而非 M1 层 |
| C2 | `src/module_settings/settings_module.cpp:1216-1221`（KeepLocal 只日志 + `continue`，登记不清）；`src/module_settings/github_sync_logic.cpp:114-117`（InSync 短路）；`settings_module.cpp:1017-1026`（PUT 用登记 sha）；`src/platform/github_api.cpp:125`（422→ValidationError 无特殊处理） | **高** | 远端被别的设备删除笔记 → pull 判 `KeepLocal`（本地保留，方向正确）但**登记不动**；此后该笔记：未改 → InSync 短路永不 PUT；再改 → LocalModified，PUT 带一个**指向已不存在文件**的 shaIfMatch → GitHub 409（走冲突弹窗 → 拉取「保留本地」→ 再推 → 再 409，**死循环**）或 422（ValidationError，永久失败） | 该笔记在本机永久推不上去，只能改名/手工清库；同时丢掉 T3 的**自愈**能力（T3 每轮全量 PUT，远端删了会自动补回）——是本批引入的行为退化 | KeepLocal 分支补 `forgetSyncedNote(name)`（登记清掉 → 下轮 push 视为 New（登记无）→ 无 sha PUT 重建）；或在 PUT 撞 409/422 且 GET 确认远端已不存在时降级为不带 sha 的 PUT |
| C3 | `src/module_settings/settings_module.cpp:1027-1036`（笔记 409 只置 `conflict`，未写 `githubSyncStagedOverwriteFiles_`；唯一写入点在 :988 的 sql 409 分支） | 中 | 笔记 PUT 撞 409 时弹窗缺「将被覆盖的本地未推文件清单」——F6 的核心承诺在笔记路径失效（清单为空） | 用户对着空清单做「拉取后再推」的破坏性确认 | 笔记 409 分支复用 sql 分支的清单构造（`database.sql` + 各 Put）；或把清单构造抽成一个小函数两处共用。（F6 归属，但缺口在 `987d4fb` 引入的 409 分支里） |
| C4 | `src/module_settings/settings_module.cpp:1159-1160,1172`（`sqlImportPlanned = remoteSqlSha != 登记`）；spec `2026-10-15-github-sync-api-design.md:140`（拉取流程=无条件导入） | 中 | 「远端 database.sql 未变 → 跳过导入」是 M1 自加的语义，**偏离 spec 拉取流程**且无强制入口 | 用户点「拉取」想用远端库覆盖/自救援（本地库损坏）时被静默跳过（仅摘要一行「database.sql 未变，未导入」），无 force 入口 | 补「强制导入」入口（或把跳过理由 + 手动强制做成卡片动作）；至少把该偏离回写 spec，别让实现与 spec 两口径 |
| C5 | `src/module_settings/github_sync_logic.cpp:9-21`（`classifyNote` 唯一实现）；生产调用点 **0**（全仓 grep：只有 `tests/test_modules/test_github_sync_logic.cpp` 调它）；实际行为在 `github_sync_logic.cpp:108-128` + `:130-145` | 中 | 7 态状态机是**测试专属死代码**：真行为由 `diffNotesByState`（只表达 New/LocalModified/LocalDeleted/InSync 四态）与 `pullNoteActions`（RemoteNew/RemoteModified/KeepLocal 三态）两套并行实现；`Diverged` 从未被任何路径产生（真冲突靠 409）；注释宣称的跨文件不变量（全假输入只在 pull 枚举域出现）无代码强制 | 表驱动测试给人「状态机管着行为」的错觉；后续改动作逻辑改错地方不会被测试抓到；新增状态无处可加 | 二选一：① 让编排经 `classifyNote` 选动作（单一语义源）；② 明确降级为「参考模型」注释并删掉该函数与用例（或把用例改写成对 `diffNotesByState`/`pullNoteActions` 的等价断言） |
| C6 | `src/module_settings/github_sync_logic.cpp:14`（`!localExists → LocalDeleted`，**不看 `remoteShaMatches`**）；`settings_module.cpp:1049-1079`（DELETE 前 GET 现值即删，无 409 分支） | 中 | 「本地删 + 远端被别人改过」被无条件判为删除传播：先 GET 当前 sha 再 DELETE，**必成功** | 别的设备刚改好的内容被静默删除（无冲突信号、无 app 内备份；仅 GitHub 历史可人工救）。参数化状态机在 `!localExists` 时丢掉了远端信号，spec 的状态表也没定义该组合 | DELETE 前比对「远端现值 sha vs 登记 sha」：不一致 → 不删，报「远端已改，删除已跳过」；或在 `classifyNote` 加第 8 态 `DeleteConflict` |
| C7 | `src/module_settings/settings_module.cpp:1229-1233`（写盘无 `create_directories`）vs `:1344-1360`（`collectLocalNotes` 只扫顶层 `*.md`） | 低 | 远端 `notes/子目录/x.md`（树筛选用前缀 + `.md`，是递归的）→ 写 `markdownDir()/子目录/x.md` 失败 → **整次拉取中止**；且 push 永不枚举子目录 → 读写不对称 | 非本 app 创建的嵌套笔记会让整个 pull 变红；嵌套笔记永远推不上去 | 写前 `create_directories(parent_path)`；或显式把嵌套条目列为「跳过（不改 summary 成败）」并在逻辑层单测里锁住 |
| C8 | `src/module_settings/settings_module.cpp:907-927`（dump → 读文件 → 判超限）；spec `:54`（100MB 口径） | 低 | 体积上限在 **dump 之后**才拦：超限时完整 dump（>73MB 落盘 + 全表遍历）已发生；且 100MB/base64×1.37 是 spec 断言、无真机验证（换算本身保守：两种解释下都在 100MB 以内） | 超限用户的失败路径白付一次全量 dump（清理后重试再付一次） | 先用 DB 文件大小（`std::filesystem::file_size(appDatabase 路径)`）做粗判再决定是否 dump；把「100MB 到底是请求体还是原始文件」登记为待真机核 |
| C9 | `src/module_settings/github_sync_config.cpp:116`（`"github_sync:hash:"` 字面量，而 `:24` 已有 `kKeyHashNotesPrefix`）；`src/module_settings/github_sync_config.h:56` + `.cpp:156`（`syncedNoteNames()` 生产调用点 0，仅测试引用） | 低 | 与 `73799c8` 同类的「同一前缀两份写法」：`save()` 的保留判据用字面量；`syncedNoteNames()` 是 M2 迁移到 `syncedNoteRecords()` 后的遗留 API | 改前缀时易漏一处（清掉不该清的键）；死 API 让测试维持着一条无人使用的路径 | 加 `kKeyHashPrefix = "github_sync:hash:"` 常量并让 `save()` 引用；删 `syncedNoteNames()` 及其用例（或标注 deprecated） |
| C10 | `src/module_settings/github_sync_config.h:7-13,56`（删了 `#include <set>`，仍在用 `std::set`） | 低 | M2 顺手删 include，靠 `github_sync_logic.h` 的 `<set>` 传递；`check-std-includes.sh` 明确允许「直接 include 的项目头提供」故守卫绿 | 头部依赖变成「逻辑头顺带提供容器」的隐式耦合；换依赖即编译红 | 补回 `#include <set>` |
| C11 | `src/platform/github_api.cpp:527`（`Accept: application/vnd.github.raw`，无 `+json` 后缀）+ `settings_module.cpp:1229`（M1 起该返回值直接落盘并登记 hash） | 低（待真机） | GitHub 文档口径是 `application/vnd.github.raw+json`（v3 别名 `application/vnd.github.v3.raw`）；裸 `application/vnd.github.raw` 是否被当未知类型回退成 JSON 未验证 | 若回退：>1MB 笔记落盘成**元数据 JSON**，且登记 hash=该 JSON → 本地笔记被污染且 InSync 短路不再上传（本地坏、远端好）；单测只断言「发了这个 header」，证不了服务端行为 | 真机拉一篇 >1MB 笔记核 content 是否为原文（离线不可判，按铁律 1 登记为待调研项）；或直接改 `application/vnd.github.raw+json` 更贴文档 |

## 重点核查回应（审计清单 8 项）

1. **7 态状态机**：16 组合的定义域**完备无洞**，优先级 `!hasRecord > !localExists > localHash > remoteSha` 与注释一致；`RemoteNew` 兜住全假输入合理（调用方枚举域注释也写了）。但**唯一缺陷是 `!localExists` 吞掉远端信号**（C6），且该函数**零生产调用点**（C5）。FNV-1a 实现正确：offset basis `0xcbf29ce484222325`、prime `0x100000001b3`、`for (unsigned char c : bytes)` 无符号拓宽、hex 按 MSB→LSB 输出共 16 位；我用独立实现复算 `""`→`cbf29ce484222325`、`"a"`→`af63dc4c8601ec8c`、`"foobar"`→`85944171f73967e8` 与用例一致（UTF-8 中文串同样一致）。
2. **M2 冲突检测**：登记时机正确——笔记 PUT 与 sql PUT 都**成功后才** `recordSyncedNote` / `recordSyncedSqlSha`（`settings_module.cpp:1042-1047,995-1001`），失败路径不落登记；`putFile` 返回 `content.sha`（blob sha，与 `listTree` 的树条目 sha 同口径，git 内容寻址保证可比）。「只推变更」的 hash 比对**不会**漏推「内容回退到旧版本」：登记对 (sha, hash) 恒成对写入同一内容，hash 相等 ⟹ 本地字节 == 上次成功推送的字节；真改回旧版仍是新 hash → LocalModified → PUT。FNV-1a 32/64 位碰撞面：全仓用的是 64 位，笔记规模（10³ 量级 → ~10⁶ 对）下碰撞概率 ~1e-14，且非对抗场景——可接受。409 后走向：`conflict=true` → 弹窗 `Conflict409` → PullThenPush（拉取刷新登记后再推），语义闭环；但清单为空（C3）、delete 路径无 409 检测（C6）。
3. **M1 拉取重写**：树列举 → 筛 `notes/**.md` + `data/database.sql`（前缀 + `.md` 后缀判定，`size <= prefix` 早退避免了越界）→ 按登记 sha 下载 → 库未变跳过导入，各分支都在。**但判据的输入（登记）来自会被 dump 搬运的 `app_config`**（C1），这是本批最重的问题；`KeepLocal` 登记不清（C2）；无登记的大库首拉行为正确（全量下载 + 导入，登记为空 ⟹ sha 必不等）；C7 是写盘侧缺口。
4. **S3 上限**：`73'000'000 = 100e6/1.37` 与 spec 一致，且**两种解释（100MB=请求体 / 100MB=原始文件）下都安全**（73MB 原始 → base64 ≈ 97.3MB）。拦截点在**dump 之后**（C8）。判定函数用 `>` 语义正确：恰等于上限不拦（用例 `dumpTooLargeForRemote(100,100)==false` 锁住了边界）。
5. **S1 删净性**：`kGithubSyncSkipTables` 全仓零残留（`grep -rn` 命中 0），调用点改引 `::core::sql_dump::defaultSkipTables()`（`sql_dump.cpp:23-26` 的值 `{"claude_call_logs","claude_run_settings"}` 与被删常量逐项相同），`test_sql_dump.cpp:78` 锁死清单；`SqlDump` 用例 5/5 绿。行为不变成立。
6. **`73799c8`**：`kRemoteSqlRelPath` 删净（零残留），key 拼接口径单一来源 `gsync::kRemoteSqlPath`（`github_sync_logic.h:16`），读写两处一致，键名与 `github_sync_config.h:65` 注释一致；仓库内其他 `"data/database.sql"` 字面量只剩**测试 fixture**（`test_github_api.cpp:809,826` 的假响应 / `test_github_sync_card.cpp:476` 的弹窗入参），不构成第三真源。耦合面：`github_sync_config.{h,cpp}` 现在（经 M2）依赖 `github_sync_logic.h`；同模块内、无环，可接受，但数据 struct（`SyncedNoteRecord`）放在「纯逻辑头」里让 config 反向依赖了逻辑层——可留意，不是缺陷。
7. **测试有效性**：FNV 三向量我独立复算一致（非恒真）；状态机用例覆盖全 7 态（9 条断言，等价类全覆盖）= 可信；`diffNotesByState`（InSync 空集/顺序/旧记录缺 hash 视为改动）与 `pullNoteActions`（下载/保留/顺序/双空）都有对照组；store 双键往返 + `save()` 不清状态键 + `forget` 清两键齐备。**缺口**：① 编排层（push/pull 接线）**零覆盖**——M1/M2 的「先红」是**编译红**（新增 API 未实现），不是行为红，所以这两票的核心语义（只推变更、409 走向、登记时机）**没有一条断言锁住**；② C1/C2 无用例；③ `classifyNote` 用例在测一个无人调用的函数（C5）。
8. **回归面**：`github_sync` 24/24、`SqlDump` 5/5、`Settings` 9/9、`listTree` 3/3 全绿；全量 1410/1398/6（红项已定位为环境性 db_backup ×3 + base drift agent_ui ×3，均非本批）；T3 已合入的**节流**（`github_sync 逻辑: 5 分钟节流窗口 + 手动绕开`）与**首次方向**（`首次方向判定`）用例照绿，且 6 提交的 diff 未触碰 `throttleDue` / `firstSyncChoice` / `maybeAutoGithubSync` / dirty 标记代码——`settings_module.cpp` 的改动只落在 dump 调用、push、pull、存储读取四处，无越界。`check-module-isolation.sh` / `check-std-includes.sh` 真树绿。

## 六维度

| 维度 | 结论 |
|---|---|
| 正确性（对照 spec 与用户诉求） | 6/10 — F1/F7/F9/S1 主体落地且判据清晰；C1（登记随 dump 转移 → 漏下载 + 误删远端笔记，M1 提交态可清空远端）、C2（远端删除后永久推不回去，退化掉 T3 自愈）、C6（本地删优先于远端改，无提示）是三个真缺陷；C3/C4 是覆盖缺口 |
| 回归面 | 8/10 — 定向 + 全量复跑已做，红项全部归因清楚且与本批无关；T3 的节流/首次方向未受影响；C2 是本批引入的行为退化（T3 全量 PUT 的自愈被 InSync 短路 + KeepLocal 组合吃掉） |
| 平台对称性 | 6/10 — 无新增 `#ifdef`；读写全走 `std::ios::binary`（Windows 无 CRLF 改写，不会造成 hash 抖动）；但**只在 macOS 验过**，Windows 侧未跑；C11 依赖的 raw 媒体类型从头到尾没真机核过 |
| 静默失败面 | 5/10 — C1/C2 的后果是「远端笔记消失」与「笔记永远推不上去」，两者都只在摘要/日志里一句话（`KeepLocal` 仅 LOG_INFO，pull 摘要带「远端已删 N 篇」是唯一可见面）；登记写失败只 WARN（方向保守，可接受）；C2 的 409 死循环会把用户困在弹窗里 |
| 测试有效性 | 6/10 — 纯件用例质量高（向量/边界/对照组/顺序断言齐）；**编排层零覆盖**使 M1/M2 的核心语义无断言，「先红」退化为编译红；C1/C2/C6 无用例；C5 的用例锁的是死代码 |
| 文档口径 | 6/10 — 提交信息与实现基本对得上（含主动披露简化）；但 C4 的 sql-skip 偏离 spec §5 未回写 spec；C5 头注释声称的「跨文件不变量」在无调用点的情况下无人执行、也无人检查 |

## 总裁决：**带病合并**（附条件）

**合并前必修**
1. **C2**（一行级）：`KeepLocal` 分支补 `forgetSyncedNote(name)` —— 否则「别的设备删了笔记」= 本机该笔记永久卡死（409 死循环 / 422 永久失败）。
2. **C1**（需拍板，改动小）：同步基线不得随 dump 跨设备转移——导出时剔除 `github_sync:sha:*` / `github_sync:hash:*` / `first_sync_done` 行（或在 push 的 DELETE 处要求「登记须本机写入」）。在 M1 的提交态这条会**清空远端全部笔记**（新设备首次启用同步即触发）；HEAD 靠 F6/F3 把动作计算提到导入前才中和 happy path，而该次序是**载荷性**的（无注释、无测试），且「导入后拉取中途失败 → 下轮 push 删远端未下载笔记」在 HEAD 仍然可达。

**随手带上**（同一批改动内，成本低）
3. **C3**：笔记 409 也填覆盖清单（F6 票面，但缺口在本批引入的分支里）。
4. **C4**：把「拉取跳过导入」的偏离与强制入口（或至少 spec 回写）一并处置。
5. **C5/C9/C10**：删死 API `syncedNoteNames()`、加 `kKeyHashPrefix`、补回 `<set>`。

**登记为后续票**
6. **C6**（删除前比 sha 防误删）、**C7**（嵌套目录）、**C8**（上限前置判定 + 100MB 真机核）、**C11**（`vnd.github.raw` 真机核 / 改 `raw+json`）、**编排层测试补齐**（push/pull 接线至少 mock 到「只推变更 / 409 走向 / 登记时机」三条）。
