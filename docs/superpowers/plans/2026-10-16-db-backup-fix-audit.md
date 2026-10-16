# 审计：b330bdb 备份产物归一为自包含库（WAL 页头 → rollback journal，修只读校验/还原全链）

- 对象：`b330bdb`（分支 `worktree-agent-db-backup-fix`，基 `9e895e0`，`src/core/db_backup.cpp` +18/−1、`src/core/db_backup.h` +2/−1；测试文件零改动）
- 审计者：独立只读 agent（非实现者），2026-10-16
- 手段：全 diff 精读 + 常驻调用方核对（main.cpp / paths.cpp / settings 侧另一条 backups 链）+ **sqlite 字节级实证探针**（复刻旧/新两条 dst 侧代码路径，读文件头 offset 16-20、列目录伴生文件、只读打开口径复现）+ worktree 定向复跑 + `c3ea4db` 逐字节比对
- 探针位置：`/tmp/dbbackup-audit-probe/`（临时弃用脚本，未入库、不在仓库内）
- 复跑证据：worktree 内 `DEVDESK_TEST_TMPDIR=/tmp/devdesk-audit-dbbackup2-<pid> bash scripts/build-test-run.sh --no-run --filter db_backup` → `Total: 14 Passed: 14 Failed: 0`（exit 0；`git status --porcelain` 空、HEAD=b330bdb，复跑对象即被审提交）

## 实证结果（探针原文摘要）

| 场景 | 页头 @18/@19 | 伴生文件 | 只读打开口径 |
|---|---|---|---|
| 旧路径（WAL 源、无 PRAGMA） | **2/2** | 无 -wal/-shm | open=0 但 prepare=**14 SQLITE_CANTOPEN**，数据读不出 |
| 新路径（WAL 源、PRAGMA 后） | **1/1**（关连接前已落盘） | 关连接后 **`-shm` 仍在（32 KB）**、`-wal` 已无 | open=0 / prepare=0 / quick_check=ok / 数据齐 |
| 新路径 + 热 WAL 源（259 KB 未 checkpoint，源连接持有） | 1/1 | 同上 | `integrity_check=ok`，rows=500 sum(len)=200000 与源一致 |
| 非 WAL 源 + PRAGMA | 1/1（不变） | 无（PRAGMA 结果行 = delete，无副作用） | 正常 |
| 旧式备份（WAL 页头）当主库还原 | 2/2 | 无 | 只读校验 FAIL（prepare=14）；**READWRITE 打开 rc=0 可用** |

## Findings

| # | 位置 | severity | 问题 | 建议 |
|---|---|---|---|---|
| 1 | `src/core/db_backup.cpp:234` | info | 注释「失败只 WARN：文件仍是合法可读库」与自身理由矛盾：归一失败留下的正是**过不了只读校验**的旧状态（探针：WAL 页头文件只读 prepare=14 CANTOPEN，READWRITE 才可读），即本行要防的那个失败 | 措辞改为「该备份退回修复前状态（过不了只读校验）」；行为无需改 |
| 2 | `src/core/db_backup.cpp:253-254` | info | 伴生文件 `fs::remove` 的 `rmEc` 不检查、不记日志（静默失败面）：残留的 `<备份名>-shm` 字典序大于备份本身，会被 `latestBackup` 当「最新备份」选中（还原时把 32 KB shm 拷成主库 → 校验失败 → 用户见「还原失败」而好备份在更早一份），`prune` 也会多占一个保留位。触发面：进程在 PRAGMA 与 remove 之间被杀、Windows 下 add 侧共享占用致删失败、同秒多进程互踩 | 一处 `LOG_WARN` 可见化即可；更彻底是 `latestBackup`/`pruneFilesWithPrefix` 跳过 `-wal`/`-shm` 后缀名（各 1 行），留待后续批次定夺 |
| 3 | 提交/backlog 未登记 | info | 「无兼容」残留未登记：**修复前产出的备份**（WAL 页头、无伴生）在修复后仍还原不回来（探针复现：只读校验 FAIL、RW 可用），提交未动任何文档、backlog 无此条。策略上「不写迁移」成立，且启动备份 5 份滚动会在 ≤5 次启动内自然换代，但用户盘上现存旧备份的确是死票 | 按本仓惯例（父提交 9e895e0 即登记两小项）在 `docs/superpowers/plans/2026-09-22-backlog.md` 登记一行即可；若要救旧备份，最小非迁移改法是校验失败（CANTOPEN）时回退 READWRITE 打开（RW 实测可读），是否做由用户拍板 |
| 4 | `docs/superpowers/specs/2026-10-03-db-startup-backup-design.md:32` | info | 权威 spec 的 `createBackup` 条目未同步新不变量「产物必须自包含（页头归一、无 -wal/-shm 伴生）」——只有 `db_backup.h:38-40` 的头文件契约里有；下一位读 spec 的 AI 推不出这条硬约束 | spec §4.2 该条补一句（不动行为） |

无 blocker、无 major。

## 核查点逐条

1. **修法正确性 — 已核，通过（字节级实证）**。`PRAGMA journal_mode=DELETE` 在 dst 打开连接上确有落盘效果：探针场景二显示 PRAGMA 执行后（**关连接前**）文件头 offset18/19 已从 2/2 变 1/1；机制为 pragma 的读事务触发 `pagerOpenWalIfPresent` 读页头入 WAL，再走 WAL→DELETE 转换把版本字节写 1。完整性无损：常规场景 rows=400/sum(len)=160000 与源一致；**热 WAL 源**（259 KB 未 checkpoint、备份后源连接仍开着）经归一后 `PRAGMA integrity_check` = ok 且 500 行/200000 字节全在，`quick_check` 只读口径 ok。非 WAL 源为纯 no-op（结果行 "delete"、无伴生文件、页头不变），无副作用。
2. **伴生文件清理 — 已核，必要性成立、条件精确**。删除谓词是**精确路径** `dest+"-wal"` / `dest+"-shm"`（不是前缀扫描）；注释里的「前缀 + 字典序」只是解释残留为何有害，不是删除条件。误删面：合法备份名恒为 `<库名>.backup-YYYYMMDD-HHMMSS`，不可能以 `-wal`/`-shm` 结尾 → 删不到任何合法备份；也只删本次 dest 的伴生，别的备份（含别的备份的伴生）不碰。必要性实证：归一动作确实现建 `<备份名>-shm`（32 KB）且**越过 sqlite3_close 留在盘上**，必须显式删。`latestBackup`/`pruneFilesWithPrefix` 都按 `<库名>.backup-` 前缀匹配名，残留 shm 会命中且字典序大于备份本身 → 对清理有真实依赖；该依赖已被既有用例 `tests/test_core/test_db_backup.cpp:121`（once 闩后目录计数 == 1）守住——去掉这两行 `fs::remove` 该用例即红。
3. **连带缺陷核实 — 已核，成立（推演 + 探针复现）**。链：备份页头 2/2 无 -wal/-shm → `quickCheckOk(dbFile, readOnlyOpen=true)`（`db_backup.cpp:349`）内 `sqlite3_open_v2(READONLY)` 成功但 `PRAGMA quick_check` prepare 返回 **CANTOPEN(14)**（探针复现）→ `quickCheckOnHandle` 只在 CORRUPT(11)/NOTADB(26) 返回 ok(false)，14 走 `fail(...)` → `restoreFromBackup` 的 `verify.isFail()` → `failRestore` 复原现场、还原失败。而主库恒 WAL（`sqlite_db.cpp:273` `PRAGMA journal_mode=WAL`），故**修复前每一份备份都还原不回来**——不是边角，是全链失效。
4. **回归面 — 已核，通过**。全仓 `*.db.backup-*` 消费方只有 `main.cpp:75/110`（`handleDbCorruption` 的 latestBackup→restoreFromBackup）与测试、`paths.cpp:59` gitignore（`*.db.backup-*` 尾部通配，残留 shm 也已被屏蔽）；无任何读取方按 WAL 假设读该文件。`main.cpp:638 performStartupBackupOnce` 只记日志，行为差异仅「产物页头 1/1」——对唯一下游（只读校验）是修好而非改坏。另一条同名链（settings 侧 git-sync `backups/devdesk-<ts>.db`，`settings_module.cpp:985`，wal_checkpoint 后整文件拷贝）目录/前缀/读取方全不同，本修不触及（其 prunement 只认 `.db` 后缀、无只读读取方，独立成链）。平台对称性：改动是纯 SQLite API + `std::filesystem`，零平台分支；实现在 macOS 验证，Windows 侧行为差异仅在删除时序（remove 在 close 之后，句柄已释放），走 CI 编译门禁，未本地实跑（如实登记）。
5. **测试有效性 — 已核，通过（非恒真）**。断言口径对照 spec：spec:82「create 产可读库｜备份文件 sqlite3 可开、sqlite_master 有表」= 用例 `test_db_backup.cpp:95` 的 `countSqliteMasterRows`（只读打开 + prepare）；spec:30 明确「备份校验用 READONLY」= 用例 `:187`/`:316` 的还原断言。三红候选按源库是否 WAL 逐一推演：`:95`（TestDb=Database::open=WAL）、`:187`、`:316` 三者的断言在旧代码下必红（探针场景一给出同一机制的字面复现），其余 11 例与只读口径无关（once 闩计数在旧代码下本就 ==1，因旧路径不产生伴生文件）。与实现者声明的 11/14 一致；旧版本未复跑（只读范围限于现版本，符合审计纪律）。修后 14/14 实测全绿，且探针证明该绿非恒真（旧机制下同断言必红）。
6. **c3ea4db 无关声明 — 已核，逐字节证实**。`c3ea4db` 是 HEAD 祖先；`git diff c3ea4db 9e895e0 -- src/core/db_backup.{cpp,h}` 为空（基引用与修复父提交在 db_backup 上完全相同）；`diff <(git diff c3ea4db..HEAD -- 两文件) <(git show --format= b330bdb -- 两文件)` 无差异（BYTE_IDENTICAL_OK），两侧 numstat 同为 `17/1` + `3/1` → 该路径的改动可完整归因于 b330bdb。
7. **行数比与「无兼容」策略残留 — 已核**。+18/−1 中可执行代码 ≈12 行（其中 6 行是失败分支/WARN），其余为契约注释，与「一次 PRAGMA + 两次 remove + 一条失败日志」一一对应，无抽象膨胀、无配置项，符合「避免代码膨胀」口径。残留（旧备份 WAL 页头不兜底）**未登记**，见 finding 3；策略上不写迁移代码正确（探针显示旧备份用 READWRITE 仍可读，若用户要救它是可做到的最小改法，非必须）。

## 六维结论

1. **正确性 — 已核，通过**（核查点 1；字节级实证页头归一 + 数据无损含热 WAL 场景）。
2. **回归面 — 已核，通过**（核查点 4/6；消费方仅 main.cpp 还原链，14/14 实测绿，`c3ea4db` 比对逐字节）。
3. **平台对称性 — 已核，通过（Windows 走 CI，如实登记）**。零平台分支；close 先于 remove 的时序对 Windows 删除语义正确。
4. **静默失败面 — 已核，通过（一条 info）**。正常路径的失败（PRAGMA 失败、备份未完成）都有 WARN/fail；唯一静默点是伴生文件 remove 的 ec（finding 2），影响面已写实。
5. **测试有效性 — 已核，通过**（核查点 5；断言与 spec 逐条对齐、旧机制下必红、清理动作被 once 闩计数用例守住）。
6. **文档口径 — 已核，通过（两条 info）**。头文件契约已同步新不变量；权威 spec 条目未同步（finding 4）、失败注释措辞与自身理由矛盾（finding 1）。

## 行数/功能比

+18/−1（.cpp）+ +2/−1（.h）：产品代码 ≈12 行可执行（PRAGMA 调用 + 失败 WARN 分支 + 两次伴生删除），评论 ≈10 行且皆为反重构/契约性质（为什么必须归一、为什么必须删伴生、失败后果），与「一个 PRAGMA + 两次 remove」的功能量对应，无超需求实现。

## 裁决

**放行** — 根因（备份页头 2/2 无伴生 → 只读 prepare CANTOPEN）与链式后果（每份备份都还原不回来）经字节级探针独立复现，修法在关连接前把页头落盘归一为 1/1、数据无损（含热 WAL 与 400/500 行载荷完整性）、非 WAL 源零副作用；伴生文件删除为精确路径、删不到合法备份且必要性实证（`-shm` 越过 close 存活），既有用例对其有守卫；`c3ea4db` 逐字节比对证实改动归因唯一；14/14 实测全绿且非恒真。findings 四条全 info 级（失败注释措辞 / remove 静默 ec / 旧备份残留与 spec 条目未登记），不阻断合并；建议随下次注释或文档批次顺带处理（尤其 finding 3 的登记与 finding 2 的可见化）。
