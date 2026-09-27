# 波 2 交付报告（2026-09-28 夜班）

> 范围：波 2 = `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md` §4「波 2：结构性收口」十线（2a–2j）+ 收尾独立线 **CP-08** + 波 1 清单里「必须另开」的 **CP-11**。
> 口径：**用例总数以运行输出 `Total:` 为准**，本文里的数字都是**当时快照**；所有 sha 取自 `git log` 实测。找不到对应 merge 的项一律写「见 master 历史」，不编造。
> 同夜的波 1 交付见 `2026-09-28-wave1-delivery-report.md`（其新增的「后续（同夜）」节是本波的摘要版）。

## 1. 本夜总览

时间线（`git log --merges --since='2026-09-27 20:00'` 实测，均为 +0800）：

- **波 1（11 线）** 合入 master：`00:45`（L1 / L3 / L4 / L6 / L8 / L9 / L10 同刻）→ `00:54`（L5、L2，含解冲突 `ba3f88a`）→ `01:30`（L11 docs）→ **`01:42`（L7 pet+settings 补交，波 1 至此 11 线全齐）**
- **波 2 夜班批**：`01:19` → `03:39`，逐线见下表

### 波 2 逐线（按合并时间排序）

| 条目 | 一句话 | 合并 sha（时间） |
|---|---|---|
| THS-06 | 补回 4 处被吞的次生失败（完成 / 取消完成的列迁移失败改为报错不报成功、逾期弹窗与 badge 查询失败不再降级成 0）+ 2 条注入式用例 | `7324442`（01:19） |
| CP-11 | keychain account 文件名改可逆编码（%XX，白名单 `[A-Za-z0-9-_]` 外逐字节转义）+ 双读兼容 + 原子落盘复用 `core::atomicWriteFile` | `ccac5ce`（01:26） |
| MD-08 | 索引剪枝改「先取快照再遍历」+「遍历出错不剪枝」，修掉界面线程新建 / 改名笔记被从索引误删 | `e8621f9`（01:27） |
| THS-04 | 逾期谓词单源 `src/core/overdue.h` + 首页 / Todo / 谓词三向一致性用例（附反证探针） | `a88ccce`（01:41） |
| PAB-05 | 气泡调度抽纯函数 `src/module_pet/pet_bubble.h` + 节拍常量单源 + 注释 60s→100s 对齐（含确定性网格用例） | `0f9c880`（02:08） |
| MD-02 + MD-05 | 文本列壳抽 `drawTextColumnChrome`（等价性含逐项字面量对照 + headless 断言）/ 实时态状态复位收 `enterLiveState`（补 `liveOffset_` 漏复位） | `6fc40be`（02:32） |
| CPX-06 | 调用记录取数后台化（复用 H2 的 future 刷新）+ 搜索 250ms 防抖并合并为单次派发 + 派发计数 / 执行线程测试缝 | `fcb78cd`（02:35） |
| CPX-10 | 缓冲转发路径改走 `resolveProfile`，统一取 profile 失败语义（`503 / no_active_profile`，含两路径同题同文案用例） | `08db1bc`（02:37） |
| TIN-10 / TIN-02 残 / TIN-07 | 测试基建三连：用例漏登记 configure 期自检 / 改按 exe 路径只杀自己的进程（含同名诱饵反证）/ `resetCaseGlobals` 显式清单（补日志级别基线 / 单例 / stats / appRouter） | `8272476`（02:39） |
| CPX-07 | 两段写助手 `TwoPhaseKeyWrite` + 四处复用（`updateProfile` / `deleteProfile` 补补偿）+ 撞名注入实证回写旧密钥 | `ea9de34`（02:47） |
| CPX-08 | 槽位 / 键名 / 显示名收敛为 X 宏单表（6 行派生）+ UI 6 处遍历化 + 编译期守卫（新增槽位成本 1 行，漏改不再静默） | `1fd73b2`（02:49） |
| THS-05 | 最上层单一持有者：todo 8 弹窗打开栈 + z 序由栈派生；settings 7 下拉收单索引（+ LIFO / Esc 用例） | `f0b90ec`（02:56） |
| 残项收口 | pet 穿透说明标【历史】/ `system_theme` macOS 注释写实 / `claude_service` 两处过期注释 / spec v7→v8 / `check-consistency` 与 macOS 脚本口径写实 / failover 错误类别注记（纯注释与文档） | `5bd1789`（02:58） |
| CPX-14 | keychain 最小注入缝（函数指针表 + 原子槽位，生产签名不变）+ 三条补偿失败分支单测（各自独立反证） | `b3051b1`（03:00） |
| CPX-13 | Profile 加可空 `dialectOverride`（DB 列 + v9 迁移，空串 = 未覆盖）+ `forBaseUrl` 可选覆盖参数（存量零变化，单测证明） | `f52a249`（03:09） |
| TIN-06 | 新增 `tests/test_support.h` 四件套（`setEnvVar`/`ScopedEnv` · `makeTempDir`/`TempDir` · `AppDataEnv` · `ScopedAppRouter`），迁 29 处副本，19 文件编译 + 定向用例全绿 | `296d3c4`（03:15） |
| CPX-13 接线 | 三处 `forBaseUrl`（`claude_service.cpp:1258` / `proxy_router.cpp:221` / `streaming_proxy.cpp:194`）接上 `profile.dialectOverride`，空串等价未覆盖（变异检验：回退传参则 3 用例全红） | `4105db6`（03:17） |
| CP-04 | 新增单一 `DEVDESK_DATA_ROOT` 派生全部数据路径（`appDataDir` 唯一入口）+ `pet_skin` 去 getenv 产品分支 + 5 条证伪用例 | `8bceec0`（03:29） |
| **收尾：编译中断 ①** | 补 `tests/test_claude_service.cpp` 的 `test_support::` 限定（TIN-06 迁移漏改 1 处，全仓复扫无第二处） | `fbf08a0`（03:29） |
| CP-08 | `Result` 取值口收敛为 `valueOr` + `value`（`value` 定死 `LOG_FATAL` + `abort`，删 `unsafeValue` / `expect` 共 117 处调用点）+ 源码护栏与编译期 SFINAE 护栏 | `35efad7`（03:32） |
| PAB-10 | `-Werror` 全档生效（含多配置生成器；旧写法只给 Debug 加，Release 与空 `CMAKE_BUILD_TYPE` 从未命中）+ GNU 专属误报白名单 4 条 | `dfa24db`（03:32） |
| **收尾：编译中断 ②** | `src/main.cpp` 去掉已删除的 `unsafeValue()`、改回 `value()`（CP-08 后 `value` 走 `LOG_FATAL` 不抛，Release `-fno-exceptions` 根因已消除） | `dd77bef`（03:39） |
| **在飞：MD-03 / TIN-08** | 分支 `wave2/MD-03` / `wave2/TIN-08` 均**零提交**（`git log master..wave2/MD-03` 为空、worktree tip 都是 `dd77bef`），本报告落笔时**尚未合并** | 见 master 历史 |

> **同期还有非波次线合入**（不在本波清单，列出使时间线与 `git log --merges` 对得上）：安装器 M2 `69bbfdf`（00:55）、macOS 侧 M4 `597c0eb` / `bb71195`（01:25）、profile 运行功能 `ec4e3aa` / `156fc04`（02:54）。
> **条目覆盖核对**：波 1 的 L1–L11 全部落地（L7 见 `ac1b8fb`）；波 2 已落 21 条 + 2 条收尾线；**未落** = MD-03、TIN-08（在飞）。

## 2. 过程中发现并修掉的真问题（本次最有价值的部分）

1. **master 当晚第一次红：模块隔离守卫在 cwd = 仓库根时误报「找不到仓库根」**
   守卫原编在 `test_settings.cpp` 内、靠 `__FILE__` 上溯仓库根；ccache 把 `__FILE__` 相对化后 `findRepoRoot` 第 4 次 `parent_path()` 得到空候选，`""/marker` 退化成裸相对路径、恰好命中 → 守卫必红。已迁成 `scripts/check-module-isolation.sh`（无 cwd 依赖，退出码 `0/1/2`，环境错也走 2 不静默绿），由 `scripts/build-test-run.sh:185` 在测试段之后自动串跑。

2. **两处跨线语义合并破编译（`merge-tree` 无冲突 ≠ 能编译）**
   ① TIN-06 迁移漏改 `tests/test_claude_service.cpp` 的 `TempDir` 限定，且 CPX-13 同文件新增代码 → `devdesk_tests` 编不过；② CP-08 删了 `unsafeValue` 而 PAB-10 同文件改成用它（`-X ours` 对非冲突块不生效）→ `DevDesk.exe` 编不过。**规则升级：每次合并后必须真编译一次**（不能只看 `git merge` 成功）。

3. **Release 档此前根本编译不过（PAB-10 实证）**
   `src/main.cpp` 调 `.value()` 在 `-fno-exceptions` 下报 `exception handling disabled`。根因是 `cmake/CompilerWarnings.cmake` 旧写法只给 Debug 档加 `-Werror`，Release 与空 `CMAKE_BUILD_TYPE` 从未命中 → Release 档的编译错误一直没被拦住。CP-08 把 `value()` 改成 `LOG_FATAL + abort` 后根因消除。

4. **CPX-13 只交付能力没交付生效**
   `dialectOverride` 加了字段、加了迁移、`forBaseUrl` 也加了可选参数，但三个调用点全走缺省行为 = 功能等于没上。补了一条独立「接线线」（`4105db6`），并用变异检验证明（回退传参则 3 个用例全红）。

5. **MD-08 索引误删（复核已无头复现）**
   剪枝用的索引快照取得太晚 → 扫盘期间界面线程新建 / 改名的笔记会被当成「已删除」从索引清掉（带标签的会级联删、不可恢复）。已改「先取快照再遍历」（`markdown_service.cpp:493`）+「遍历出错不剪枝」（`:547`，本轮放弃剪枝，等下一轮 rescan 自愈）。

6. **CP-08 的前提实证**（动手前先证伪）
   `expect` 是零调用（纯删、无风险），`unsafeValue` 实测 117 处调用（必须全量机械替换）。删完有源码护栏用例锁死：`tests/test_core/test_result.cpp:209/238` 扫描全仓不得再出现 `unsafeValue` / `.expect(`。

7. **TIN-06 挖出的静默陷阱**
   `ImportTempDir::write` 收的是**文件名**（内部拼目录），而合流后的共享 `test_support::TempDir::write` 收的是**完整路径**（`tests/test_support.h:128`）。照搬调用不改会静默写到 CWD——不报错、用例假绿。

8. **CPX-09 的连带测试**
   3 处把 `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE` 当「未知键」的断言（它其实在 hints 表里）必须同批改，否则改完官方键派生后这 3 条会红。

9. **CPX-12 的审计建议会引入回归**
   `buildProxyEnv(空 profile)` 只产 2 个键，照抄它做键集检测会丢掉 6 个槽位键 → 改用 `managedEnvKeyNames()` ∪ 实键（`provider_config.cpp:389`），`settings_sync.cpp:242` 的注释写明了这 12 键构成。

10. **THS-14 迁移号冲突**
    master 上 v7 已被 profile 认证字段（`kClaudeProfileAuthField`）占用且可能已落库 → 归档 kind 迁移顺延 **v8**（`src/core/migrations.h:241-244` 留痕），跨线断言同步 `6→7→8`；波 2 的 `dialectOverride` 再顺延 **v9**（`migrations.h:259`）。

## 3. 已知缺口 / 未取证（早上要看的）

| 项 | 内容 |
|---|---|
| CP-12 macOS 未编译 | `src/platform/system_theme_macos.cpp` 本机（Windows）**无法编译**（无 macOS 工具链），只做了静态核对 + `docs/KNOWN_LIMITATIONS.md` §18 登记；无 GUI 会话 / SSH 下 `CFPreferencesCopyAppValue` 返回值未取证 |
| THS-16 DST | 「跨夏令时错一天」的修复**本机不可复现**（CST 无夏令时），只有 `daysFromCivil` 的纯算术断言 |
| TIN-13 截屏 | HTML 截图段依赖 playwright / PIL，且存在**解释器错配**（`scripts/check-consistency.sh:91` 取 PATH 上的 `python3` = MSYS2 3.14.7，无这两个模块；`C:\Program Files\Python311` 的 3.11.4 才有）→ 视觉闸门按设计只 WARN；透明窗口下 `CopyFromScreen` 可能截到桌面背景（未取证） |
| FileWatcher | 「回调线程内析构」属所有者被并发销毁的 UB，本批按**句柄副本 + 契约注释**收口，未做 `shared_ptr` 方案（未做运行期复现） |
| THS-14 回填 | 对「升级前已改过归档列名」的库会落空（与 v4 同病；不引入新机制） |
| 待人工判 | L10 CP-11（PIL / playwright 解释器错配，见上） |
| **R0–R9 重构线整条未开工** | R0 的 worktree 已不在（**非本会话删除**，`git worktree list` 现只剩 `wave2-MD-03` / `wave2-TIN-08` 两条在飞）→ 需重建 worktree + 重新对齐 master 才能起 |
| 已登记但未做 | ① TIN-06 待迁移清单（`TestDb` ×4 —— `test_claude_service.cpp:58` / `test_provider_config.cpp:67` / `test_proxy_router.cpp:55` / `test_settings_sync.cpp:99`；TestEnv 家族 11 个等）；② CPX-08 两处硬编码「槽位 2/6」的 UI 用例 pin（`tests/test_modules/test_claude_ui.cpp:593/738`）；③ CPX-10 遗留注释（`proxy_router.cpp:103`）；④ `proxy_router.cpp` 与 failover spec 里旧的 `500 keychain_read_failed` 口径（spec 已加波 2 注记更正） |

## 4. 如何复核

```bash
cd /d/project/gui/DevDesk

# ① 全量（构建 + 测试 + 两条守卫 + 启动；守卫失败即整体 exit 1）
bash scripts/build-test-run.sh

# ② 只跑模块隔离守卫：0 通过 / 1 违规 / 2 环境错
bash scripts/check-module-isolation.sh; echo "exit=$?"

# ③ 本夜合并历史（逐条 sha 与时间）
git log --merges --format='%h %ci %s' --since='2026-09-27 20:00'

# ④ 自建 worktree 是否清干净（应只剩两条在飞：wave2-MD-03 / wave2-TIN-08）
git worktree list

# ⑤ 在飞线的提交状态（两条都应为空 = 零提交）
git log --oneline master..wave2/MD-03; git log --oneline master..wave2/TIN-08
```

**最新用例数快照**（2026-09-28 03:4x，主工作树）：`Total: 707 / Passed: 706 / Failed: 0`，两条守卫（发布形态 ✅、模块隔离 ✅）全绿。**仍以运行输出 `Total:` 为准**。静态对账：`grep -rE '^\s*REGISTER_TEST\(' tests/test_{core,platform,modules}` = **708**（比运行 Total 多 1，未逐条对账）。
