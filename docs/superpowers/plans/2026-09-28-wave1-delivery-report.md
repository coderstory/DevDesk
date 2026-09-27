# 波 1 交付报告（2026-09-28）

> 范围：`docs/superpowers/specs/2026-09-27-wave1-fix-design.md`（**11 条线 / 34 条 finding**），波 1 base = `3e07a5c`（9 条线内提交 — L1/L2/L3/L4/L5/L6/L8/L9/L10 各一条 — 均以它为父）。
> 口径：**用例总数以运行输出 `Total:` 为准**，本文里的数字都是**当时快照**；所有 sha 取自 `git log` 实测，未编造。

## 1. 逐线交付

| 线 | 条目 | 合并 commit | 定向测试摘要 |
|---|---|---|---|
| L1 platform-http | CP-06 / CP-10 / NOPROXY | `58e740b`（线内 `3d6af98`） | proxy_router 11/11、http_server 8/8、dialect 8/8、migrate 4/4、http_ 8/8（含 `HTTP_PROXY=http://127.0.0.1:1` 复跑）、streaming 23/23、claude_service 41/41 |
| L2 platform-family | CP-09 / CP-12 / FileWatcher | `e50d82b`（线内 `65e8f42`；解冲突 `763e952` / `e8f3b1e`） | file_open 10/10、FileWatcher 6/6、paths 5/5、theme 22/22、md_file_open 4/4；settings 67 例 66 过 1 败（败者 = 既有模块隔离守卫的 cwd 缺陷，spec §1.2 已记为基线唯一失败，归 L10 修） |
| L3 proxy-sync | CPX-09 / CPX-12 / G8+G9 / CPX-15 | `6d6932e`（线内 `60f4df4`） | 新增 `provider_config_official_keys_derived_from_hints`、`sync_reverse_read_unreadable_aborts`（0 字节对照仍在同用例内）；`provider_config` / `sync_overlay` / `sync_reverse` / `sync_service_warnings` 过滤词全绿（提交正文未逐条记数） |
| L4 proxy-json | CPX-11 | `bc072c8`（线内 `1188028`） | streaming 24/24、dialect 8/8、proxy 22/22、SseUsageParser 2/2；反向对照（把「值必须是字符串」判定注入共享扫描器）该等价性用例 `Failed: 1` → 用例可判别 |
| L5 todo | THS-07 / THS-11 / THS-12 / THS-13 / THS-14 / THS-17 / THS-18 | `7d5693f`（线内 `ed7aeea`；解冲突 `ba3f88a`） | 新增 `todo_svc_rename_missing_notfound` / `todo_svc_update_task_ignores_column` / `todo_ui_overdue_min_one_day` / `migrate_archive_kind_v8`；`TodoService` / `todo_ui` / `migrate` 过滤词全绿 |
| L6 home | THS-10 / THS-16 | `32162ce`（线内 `a9bd3c3`） | 新增 `core_time_days_from_civil`（1970-01-01→0 / 2024-02-29→19782 / 相邻日差 1 / 跨年差 1）；`HomeService` 过滤词全绿 |
| **L7 pet+settings** | PAB-09 / PAB-12 / PAB-13 / CPX-02 残 | **当时未合并**（**同夜 01:42 已补交**，见 §8.1 → merge `ac1b8fb`） | **无**。`wave1-L7` worktree 存在但分支 tip = `e50d82b`（master 祖先），**零提交零产物**：`grep -rn "showPetWindow\|pickPetPhrase\|settings_takeover_warn_unmanaged" src/ tests/` 零命中；`grep -rn 穿透 src/` 仍 13 处（PAB-09 的删除清单全覆盖：`pet_window.cpp:6/144/151/774/798/810`、`pet_window.h:52`、`pet_config.h:19-20`、`pet_config.cpp:20`、`settings_module.cpp:540/647/1765`）；`test_menu_beautify.cpp:45/62` 用例名仍是「6 顶层项 / 5 顶层项」 |
| L8 markdown | MD-06 / MD-07 / THS-15 | `2e6632e`（线内 `27f0dd4`） | markdown 43/43、md_live 7/7、mdspike 9/9、MarkdownService 30/30、core 5/5 |
| L9 tray | CP-05 / TIN-05 | `fed73ab`（线内 `abe5ab2`） | menu 16/16、tray 1/1（基线 17/17、2/2）；`src/` + `tests/` 下 `trayMenuSpecFiltered` / `isNavigationAction` / `trayDisabledHintSuffix` / `unavailableOnly` / 「已关闭）」全零命中 |
| L10 基建 | TIN-01 / TIN-04 / TIN-09 / TIN-11 / PAB-06 / PAB-11 / TIN-13 | `6456c61`（线内 `4477d10`） | 隔离守卫脚本化后全量由 `Failed: 1`（cwd 误报）转 `Failed: 0`；`md_block_model` 去墙钟红线改 `[perf]` 记录 + 逐块等价性断言；`PageId` 契约归位（`core: PageId ordering contract`）；`http_` 新增 3 条直测（连接被拒 / 超时提前返回 / readChunk 三终态）；发布形态守卫 + 隔离守卫串进 `build-test-run.sh` |
| L11 docs（本线） | TIN-12 / CP-05 文档 / CP-12 登记 / G8-G9 说明 / TIN-01 登记 / consolidated 修正 | 本文件所在的 commit（`git log --oneline -1 -- docs/superpowers/plans/2026-09-28-wave1-delivery-report.md`） | 本线不跑构建 / 测试（改动全在 `.md`）；静态对账与守卫复验见 §2 |

## 2. 全量验收（2026-09-28，master）

> **用例数快照（2026-09-28 03:4x 更新）**：**`Total: 707 / Passed: 706 / Failed: 0`**，两条守卫（发布形态 ✅、模块隔离 ✅）全绿 —— 波 2 夜班批全部合并后的最新实测（详见 §8「后续（同夜）」与 `2026-09-28-wave2-delivery-report.md`）。下面 §2 各条是**波 1 合并当时**的快照，保留作沿革。

- **波 1 合并当时全量**：`Total: 646 / Passed: 645 / Failed: 0`（`Skipped: 1` = `DEVDESK_SOAK=1` 门控的 300s soak；`tests/test_main.cpp` 里每条用例恰好落进 passed / failed / skipped 之一，故 Total 比 Passed 多 1）
- **波 1 之前基线**（spec §1.2 记录）：`Total: 621 / Passed: 619 / Failed: 1` —— 唯一失败是模块隔离守卫的 cwd 缺陷，已由 TIN-01 迁到脚本修掉
- **静态对账**（波 1 合并当时实测）：`grep -rE '^\s*REGISTER_TEST\(' tests/test_{core,platform,modules} | wc -l` = **646**，与运行输出一致（旧审计「静态 561」是过期快照）；同口径的最新值见 §8.3
- **两条守卫**（由 `scripts/build-test-run.sh` 在测试段之后自动串跑，失败即整体 `exit 1`）：
  1. **模块隔离守卫** `bash scripts/check-module-isolation.sh`（TIN-01）—— 本线复验：绿路径 `exit 0`；红路径（`--root` 指向仓库外拷贝树 + 在拷贝里注入一行 `#include "module_pet/pet_window.h"`）`exit 1` 并打印 `src/module_settings/settings_module.cpp:<行号>:#include "module_pet/pet_window.h"`（`文件:行:命中行` 三段式）；退出码三级 `0/1/2`，环境错也走 2 不静默绿
  2. **发布形态守卫**（PAB-06）—— `build-test-run.sh`「2b」段：`DevDesk.exe` 的每个 DLL 名必须存在于 `%SystemRoot%/System32`，另断言 `CMakeCache` 的 `DEVDESK_STATIC:BOOL=ON`。**本线 worktree 无 `build/` 产物，未复验**

## 3. 跨线修正

| 修正 | 内容 |
|---|---|
| 归档 kind 迁移 **v7 → v8** | L5 线内提交（`ed7aeea`）正文写 v7，但 master 上 v7 已被 profile 认证字段（`kClaudeProfileAuthField`，`src/core/migrations.h:227`）占用且可能已落库 → 解冲突（`ba3f88a`）时顺延为 **v8**（`src/core/migrations.h:241-244` 留痕；测试用例名同步为 `migrate_archive_kind_v8`） |
| `provider_config` 断言 **7 → 8** | 跨线：L5 加归档迁移后全量 schemaVersion = 8；`tests/test_core/test_migrations.cpp:210/292/367`、`tests/test_modules/test_provider_config.cpp:407` 四处均为 `== 8` |
| **NOPROXY**（波 1 新增，超出原审计） | `src/platform/http_client.cpp` 的 `send` / `sendStreaming` 都加 `CURLOPT_NOPROXY=localhost,127.0.0.1,::1` —— 回环请求不再被 `http_proxy` / `ALL_PROXY` 劫持 |

## 4. 已知缺口 / 未取证（沿用 spec §4，逐条给当前状态）

| 项 | 内容 |
|---|---|
| CP-12 macOS 未编译 | `src/platform/system_theme_macos.cpp` 该 TU 本机（Windows）不可编译；无 GUI 会话 / SSH 下 `CFPreferencesCopyAppValue` 返回值未取证。**已登记** `docs/KNOWN_LIMITATIONS.md` §18（含两项验证条件） |
| THS-16 DST 不可复现 | 本机为 CST（无 DST），「跨夏令时错一天」的修复效果无法复现；只有 `daysFromCivil` 的纯算术断言 |
| TIN-13 截屏限制 | RDP 断开 / 锁屏时 `CopyFromScreen` 可能截黑图（未取证）；透明帧缓冲窗口可能截到桌面背景（未取证） |
| **PIL / playwright 解释器错配**（本线新查实） | `scripts/check-consistency.sh:91` 取 PATH 上的 `python3` = MSYS2 **3.14.7**（`import PIL` / `import playwright` 均 `ModuleNotFoundError`）→ 视觉闸门按设计降级 WARN 不 fail；而 `C:\Program Files\Python311\python.exe`（3.11.4）**已装** PIL 12.2.0 + playwright 1.60.0。即「本机没装」只对 3.14.7 成立，**待人工判**（改探测顺序 or 给 3.14 装依赖） |
| FileWatcher | detach 后 `running` 串改与 Impl 生命周期两项由侦察补出，**未做运行期复现** |
| CPX-09 / CPX-12 | 256 条 `kEnvVarHints` 是否 100% 官方来源（禁联网未核对）；真实环境 `*_NAME` 出现频率未取证 |
| TIN-11 / NOPROXY | 公司代理实际劫持路径未取证（本机 `http_proxy` / `no_proxy` 已被 curl 读取一事已实证） |
| PAB-06 | `DEVDESK_STATIC=OFF` 时实际新增的 DLL 清单未取证（未做该构建） |
| PAB-13 | 最小窗内容宽 632 未取证；无头下 `onWindowCreated` 是否触发未取证（该线未交付） |
| TIN-12 | spec §4 记「全仓无落盘的全量运行输出」—— 本次仍以主会话运行输出为准，未留盘 |

## 5. 待人工项

1. **L7 pet+settings 未交付**（PAB-09 / PAB-12 / PAB-13 / CPX-02 残）：`wave1-L7` 有 worktree、分支零提交、master 上零产物。需重派或明确延后 —— **这是本波唯一的范围缺口**。→ **已闭合：同夜 01:42 合入 master（merge `ac1b8fb`），见 §8.1。**
2. **spec 内过期措辞**（spec 不在 L11 可写范围）：`docs/superpowers/specs/2026-09-27-wave1-fix-design.md:166/168/357` 仍写「v7 迁移」/ `migrate_archive_kind_v7` / 「v7 按 title 回填」。
3. **TIN-13 文档残留**（本线范围外，未改）：① `CLAUDE.md`「macOS（历史平台，脚本保留）」段末句仍写「`check-consistency.sh` 用了 macOS `screencapture`/`open -a`，Windows 上不可用」—— 脚本已 Windows 化（见 `scripts/check-consistency.sh:3` 头注释）；② `docs/KNOWN_LIMITATIONS.md`「### 3. 开发脚本 check-consistency.sh 在 Windows 不可用」整条已过时。
4. **CHANGELOG 未回填波 1**：`CHANGELOG.md` 按流水账性质本次不动（TIN-12 拍板口径）。

## 6. 本线口径变更清单（逐文件）

| 文件 | 改了什么 |
|---|---|
| `CLAUDE.md` | ① 用例数三处（ccache 实测段 `:54`/`:56` 的「589/589 绿」+ 测试框架段的「共 530 个用例」）统一改「**以运行输出 `Total:` 为准，不在文档写死**」，并把 `646/645/0`（波 1 后）与 `621/619/1`（波 1 前，含那条 Failed 的根因与归属）作为当时快照记入，481/514/530 标明「历史」；② 「AI 协作与交付纪律」段「并发上限 10」→「**并发上限 3**」并写明依据（多 worktree 并发构建把机器打满）+ 沿革；③ 「常用命令」段新增「模块隔离静态守卫」条目（`bash scripts/check-module-isolation.sh`，含三级退出码与 `--root` 反证、以及为何从单测迁出） |
| `docs/superpowers/plans/2026-09-27-HANDOFF.md` | 写 `530` 的 7 处（`:26/:30/:64/:71/:84/:88/:129`）+ `:13` 的 `589/589` 就地改为「以运行输出为准（当时快照 …）」或回填实测数；`:192` 表内历史行保留；另给 §0.1「并发上限 7」加一句「已被 2026-09-28 的『上限 3』取代」（本线改了 CLAUDE.md 后它才有矛盾） |
| `docs/USER_GUIDE.md` | ① `:3` 的 `330/330` 改不写死口径；② `:37` 托盘菜单改为真实两项——「显示主窗口 / 退出 DevDesk」（删掉「首页 / 笔记 / 待办 / Claude」与「被关闭的模块置灰 +（已关闭）后缀」，依据 `src/core/tray_menu_spec.h:34-35` 的两项数据源） |
| `docs/KNOWN_LIMITATIONS.md` | ① `:3` 的 `330/330` 改不写死口径；② `:155` 删「+ 按模块动态收缩」；③ 新增 **§17「`settings.json` 存在但读不出 → 接管/还原中止不写」（G8/G9 行为变更，含 0 字节仍 no-op）**；④ 新增 **§18「macOS `system_theme_macos.cpp` 编译未验证」（CP-12，含两项验证条件）** |
| `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md` | ① `:104`（TIN-12）按已修改写 + 注明差额已解释（旧「静态 561」是过期快照，同口径实测与运行输出一致）；② `:105`（TIN-13）按「已拍板：补齐实现」改写 + 未取证项；③ §6 表加「拍板结果」列逐行标注（CP-05 删 / THS-12 删负位 / THS-14 补 kind = **v8** / THS-11 实现校验 / THS-13 max(1) 文案不变 / PAB-08 改选**保留** / CP-12+TIN-13 改选**补齐实现**），新增第 9 行 **NOPROXY**，`:191` 的「② 标注」按拍板改写，PAB-08/PAB-11 与「不在表内但已拍板」三条另注 |
| `README.md` | `:12` 与 `:49` 的 `330/330` / 「全量 330 例」改不写死口径（`README.md` 不在派单硬边界清单内，但派单第 4 节第 1 条显式点名） |

## 7. 本线改动文件

`CLAUDE.md`、`README.md`、`docs/USER_GUIDE.md`、`docs/KNOWN_LIMITATIONS.md`、`docs/superpowers/plans/2026-09-27-HANDOFF.md`、`docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`、本文件（新建）。

## 8. 后续（同夜）

> 本节于 2026-09-28 03:4x（波 2 交付时）追加。**上面各节的数字与结论仍是波 1 合并时的快照**，未回溯改写，只在本节给出后续状态。

### 8.1 §5 的唯一范围缺口已闭合

**L7 pet+settings 已交付并合入 master** —— merge **`ac1b8fb`**（2026-09-28 01:42:50）：PAB-09 穿透残留注释清理 / PAB-12 桌宠随机播种（`mt19937`）/ PAB-13 设置页「显示桌宠」入口 / CPX-02 残「警告不再按 managed 门控」。**波 1 至此 11 线全部落地**（§1 表 L7 行与 §5 第 1 条的「未合并」按当时状态保留，已就地标注）。

### 8.2 波 1 之后又落了哪些线

**波 2 夜班批**（`01:19`→`03:39`，共 21 条线 + 2 条收尾线）逐线清单、一句话、合并 sha 见 **`2026-09-28-wave2-delivery-report.md` §1**。按编号归组：

- **波 1 清单里「必须另开」的 core/platform 小收口**：`CP-11`（`ccac5ce`，keychain account 文件名可逆编码 %XX）
- **波 2 主体（对应 consolidated §4 的 2a–2j + CP-08 收尾独立线）**：THS-06 / MD-08 / THS-04 / PAB-05 / MD-02+MD-05 / CPX-06 / CPX-10 / TIN 基建三连（TIN-10 + TIN-02 残 + TIN-07）/ CPX-07 / CPX-08 / THS-05 / CPX-14 / CPX-13 / TIN-06 / CPX-13 接线 / CP-04 / CP-08 / PAB-10
- **两条收尾线**：波 2 残项收口 `5bd1789`（02:58，纯注释与文档）；master **两处编译中断**修复 `fbf08a0`（03:29，TIN-06 漏改 `test_support::` 限定）+ `dd77bef`（03:39，`main.cpp` 残留已删的 `unsafeValue()`）
- **同期非波次线**：安装器 M2 `69bbfdf`、macOS 侧 M4 `597c0eb`/`bb71195`、profile 运行功能 `ec4e3aa`/`156fc04`
- **在飞未合并**：`wave2/MD-03`、`wave2/TIN-08`（两条分支均零提交）

### 8.3 用例数快照（更新为最新实测值）

| 时点 | 运行输出 | 说明 |
|---|---|---|
| **最新**（2026-09-28 03:4x，主工作树） | **`Total: 707 / Passed: 706 / Failed: 0`** | 波 2 全部合并后；两条守卫（发布形态 ✅、模块隔离 ✅）全绿 |
| 波 1 合并当时（上面 §2 记录） | `Total: 646 / Passed: 645 / Failed: 0` | `Skipped: 1` = `DEVDESK_SOAK=1` 门控的 soak |
| 波 1 之前基线 | `Total: 621 / Passed: 619 / Failed: 1` | 唯一失败 = 模块隔离守卫 cwd 缺陷，已由 TIN-01 修掉 |

静态对账（波 2 实测，同 §2 口径）：`grep -rE '^\s*REGISTER_TEST\(' tests/test_{core,platform,modules} | wc -l` = **708**，比运行 `Total` 多 1（未逐条对账）。**仍以运行输出 `Total:` 为准**，不在文档写死。

### 8.4 波 2 暴露的两条新纪律（写进流程，供下一个 agent 遵守）

1. **每次合并后必须真编译一次** —— `merge-tree` 报无冲突 ≠ 能编译。本夜两次跨线语义合并（TIN-06 × CPX-13 同文件、CP-08 × PAB-10 同文件）都是「无冲突」却让 `devdesk_tests` / `DevDesk.exe` 编不过。
2. **能力线要跟一条接线线** —— CPX-13 加了字段 + 迁移 + 可选参数，但三个调用点全走缺省 = 功能等于没上；补接线后还要用**变异检验**（回退传参 → 用例必红）证明真的接上了。
