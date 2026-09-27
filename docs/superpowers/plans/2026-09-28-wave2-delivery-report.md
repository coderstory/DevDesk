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
> **后半夜续做见 §5**（03:52 → 06:19）：上表标「在飞」的 MD-03 / TIN-08 均已落地，随后 R 重构线 R0–R6、Markdown Phase3 M3–M5、收尾批与独立加固线共 18 个 merge + 1 个直接提交陆续合入 master。

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

## 5. 后半夜续做（R 重构线 / Markdown Phase3 / 收尾批）

> 范围：§1 落笔（03:4x）之后到 `06:19` 的续做批 = 波 2 收尾 3 条（TIN-08 / MD-03 / TIN-06b）+ R 重构线 R0–R6 + Markdown Phase3 M3–M5 + 4 条独立加固线（残项二批 / fix-flake / fwatch-arm / TIN-08b）+ 审计表口径刷新。
> 口径：所有 sha 与时间取自 `git log --oneline --merges` / `--format='%h %ci'` 实测（均 +0800）；找不到对应 merge 的项写「见 master 历史」。

### 5.1 后半夜落地的线（按合并时间排序）

| 线 | 一句话 | 合并 sha（时间） |
|---|---|---|
| TIN-08（§1 标「在飞」项落地） | `REGISTER_TEST` 改可变参宏（裸逗号不再切参，护栏覆盖 4 类）+ `CHECK_EQ` 加 `const`（**未采纳** `const auto&`：GCC 16 `-Werror=dangling-reference` 真阳性会破 50 处既有链式调用）+ 3 处示范清理 | `c10c73e`（03:52） |
| MD-03（§1 标「在飞」项落地） | `drawBlockFlow` 活动块特判收成注入点 `BlockFlowHooks`（三个可空回调；预览态传空、实时态三 lambda 复刻原 `isActive` 三处特判）+ 对照断言与源码形态守卫（含变异检验） | `6d8d633`（03:53） |
| 残项二批 | 708 vs 707 差额查实（`terminal_launch_posix_not_implemented` 落在 `#if !_WIN32 && !__APPLE__`，Windows 不编译、非缺陷；给出预处理计数口径 = 707）/ CPX-08 两处硬编码「槽位 2/6」改表派生（变异检验两向）/ `proxy_router` 旧 `500` slug 注释注记 | `4d37216`（03:56） |
| R0（跨模块耦合消解） | `core::ServiceLocator` + `IClaudeProxyService` 窄接口 + 适配器 + 端口常量单源 `core/proxy_port.h`；settings 不再 include `claude_service.h` 且删兄弟模块 include 目录（隔离守卫 exit=0 + 三类反证） | `48a62cc`（04:08） |
| 测试脚手架迁移第二批（TIN-06b） | TestDb×4 / TestEnv 家族 7 / tempDbPath×10 / writeTextFile×6 / tempConfigFile×3 / headerCI×3 / cleanupKey×2 等，失败口径统一为「构造即抛」；23 文件 563+/799− | `d3e303a`（04:12） |
| 审计表口径刷新 | consolidated 加 §0（79 条逐条状态 ✅67 / ⬜9 / ◐2 / 🚫1 + 点名 2 条过期误导 + 剩余待办 15 条）—— **直接提交在 master，无 merge** | `6df00e4`（04:12） |
| fix-flake | watcher 入索引用例归因为**武装竞态**（Windows 只上报首次 `ReadDirectoryChangesW` 之后的改动，丢失不可补发）→ 先等 worker 武装 + 15s 有界预算 + 永久失败诊断；同款 4 处一并加固（含 3 处清场 `remove_all` 竞态） | `2bf855c`（04:38） |
| TIN-08b | 清理 `REGISTER_TEST` 裸逗号绕法：23 文件清 59 行注释/绕法 + 16 处结构性绕法（文件作用域数组 / 类型别名 / 工厂 / 外层括号保护）；用例名与描述串零差异、CRLF 保真 | `252aa9b`（04:48） |
| fwatch-arm | `file_watcher.h/.win.cpp` 补记两条边界（启动武装窗口 / 持句柄期间外部删除会失败，零逻辑改动）；`test_file_watcher` 同款武装等待 + 清场集中化与残留报告（证伪：去掉武装等待 40 跑 2 红） | `a9ce037`（04:50） |
| R1 | `src/ui_kit/` 基座（`devdesk_ui_kit` 静态库 + `ui_kit::drawToast` + `ToastState`/`ToastOptions`），claude / markdown / todo / app_sidebar **4 处消费方**改调（时长与图标差异参数化，视觉逐字等价） | `0057fb5`（05:00） |
| Markdown Phase3 M3 | 点渲染块定位（复用 `mdResolvePreviewJump`）+ 同帧 `requestFocus` / 失焦提交 / 换代释放 / IME 冻结谓词；6 组证伪探针（连续切块 20 次不丢字） | `daa6c4b`（05:13） |
| R2 | `ui_kit/dialog_form.{h,cpp}`（编入既有 `devdesk_ui_kit`），**13/13 调用点**替换（todo 6 / markdown 4 / claude 3，分批绿门禁）；宽度枚举 + 7 种按钮样式参数化，节点 id 逐字不变 | `ff6657f`（05:19） |
| M4 | 跨块 ↑↓ / 块首退格 / 块尾删除 / 块内按键（Tab / Ctrl+B,I,K / 列表续行）纯函数 + `onKeyEvent` 接线；证伪：摘接线 3 条转红、补 undo 栈探针区分块尾删除 | `7e5fba6`（05:32） |
| R3 | `ui_kit/metrics.h` 数值收口（表头/导航/对话框 inset/宽度枚举），表头几何 9 处 + 侧边栏 2 处 + 真间距语义 5 处；**拒绝把元素尺寸硬换成 spacing token**（避免取错档位）；settings 首次接入 ui_kit；含逐位等值性证明 | `077af6b`（05:33） |
| R4 | `claude_module` **2799 → 503 行**（compose 884 → 116，顶层成员 41 → 7），拆出 6 单元 **12 个新文件**（logic/state/list/detail/form/logs）；等价性 = 字面量集合相等 + 逐区域规范化 diff + 26 条无头 UI 用例全绿 | `8076ca1`（05:54） |
| M5 | 模块层撤销栈 `MdLiveUndoStack`（Ctrl+Z/Y/Shift+Z 拦截、600ms 同块合并、容量 200 / 大文档降 50、换代基线）；**收口 M4 遗留「边界删除不可撤销」**（红→绿成对用例）；实证实时态永不消费 framework 栈（D5） | `9829acd`（05:55） |
| R5 | `todo_module` **2411 → 869 行**（顶层成员 45 → 18），拆出 `todo_dialogs.h`（6 弹窗 struct + `PopupStack` + `DialogHost`）/ `todo_ui_board` / `todo_ui_dialogs` / `todo_ui_common.h`；等价性 = **66 个元素 id 多重集逐字一致** + 字面量多重集不变 + 全链路 UI 用例绿 | `ba815d3`（05:56） |
| R0 收尾 | `resetCaseGlobals` 登记定位器槽位（新增最小入口 `ServiceLocator::resetForTest`）+ 本夜新增的 argv 捕获与 `appNotice` 一并登记；两条自证用例 + 逐项变异检验 | `5bad7d4`（06:04） |
| R6 | `settings_module` **1866 → 936 行**（compose 178 → 115），9 张卡改注册表遍历 + 新增 `ui_kit/card.{h,cpp}`（`cardFrame`）+ `settings_ui_logic/cards` 拆出；**等价性主证据 = 无头元素树 dump 逐字节相等（4 tab / 239 元素 / 97526B）** + 变异检验；隔离守卫 exit=0 | `2edf604`（06:19） |
| MD-01（**查实早已修**） | 活动块锚冻结致「换块后输入被整段换成前一块」—— 审计落笔（22:21:42）后约 8 分钟即修；**agent 独立证伪：回退修复 → 3/3 红**（`md_module_live_split_anchor_enter/_heading/_list` 三条由修复提交引入）。**残留入口未修** = 外部改文件触发静默重载后活动块跟错内容，需块身份 stableId / 旧块起点在新文档的映射 = **spec D3 拍板项** | 修复 `e09feb8`（09-27 22:29）→ merge `1784a37`（22:31） |
| M6（滚动跟随） | **在飞**：worktree `wave3-M6`（分支 `wave3/M6`）tip = `9829acd`、工作区 5 文件已改**未提交**（`markdown_module.cpp` / `md_live_edit.h/.cpp` / `test_markdown.cpp` / `test_md_live_edit.cpp`，`git worktree list` + `git status` 实测）→ 本报告落笔时尚未合并 | 见 master 历史 |

### 5.2 后半夜新增的实证发现

1. **两处跨线语义合并破编译（第二次）**
   §2 第 2 条已记前两例（① TIN-06 迁移漏改 `test_claude_service.cpp` 的 `TempDir` 限定；② CP-08 删 `unsafeValue` 而 PAB-10 同文件改成用它）。这里补第二次的**机理**：`git merge -X ours` **只作用于冲突块、对非冲突块不生效** → PAB-10 侧对 `main.cpp` 的非冲突改动被原样保留，「被删掉的函数」与「新写的调用」同时落地，`DevDesk.exe` 编不过，由 `dd77bef`（03:39）修回 `value()`。**规则升级：每次合并后必须真编译一次**（`merge-tree` 无冲突 / `git merge` 成功都不构成证据）。

2. **Release 档此前根本编译不过（PAB-10 实证）**
   PAB-10 让 `-Werror` 全档生效（含多配置生成器）后才暴露：`src/main.cpp` 调 `.value()` 在 `-fno-exceptions` 下报 `exception handling disabled`。根因是 `cmake/CompilerWarnings.cmake` 旧写法只给 Debug 档加 `-Werror`，Release 与空 `CMAKE_BUILD_TYPE` 从未命中 → Release 档的编译错误一直没被拦住；CP-08 把 `value()` 改成 `LOG_FATAL + abort` 后根因消除。证据：merge `dfa24db` / `35efad7`、后续修复 `dd77bef`。

3. **Windows `ReadDirectoryChangesW` 武装窗口**
   `FileWatcher::start()` 在调用线程开目录句柄、worker 线程异步发出首次读 → 两者之间存在窗口：**`start()` 后、worker 首次读发出前的改动永久丢失**（Windows 只上报首次读之后的改动，事件丢失不可补发）。独立探针实测：开句柄后、首读前写入 **4/4 未捕获**；对照组（首读后写入）**4/4 捕获**。**放宽超时无效**（等待再久也命中不了）—— 正解是测试先等 worker 武装。证据：`581fdf0` / `2bf855c`（04:38）。

4. **CRLF 文件首次保存被洗成 LF（登记项，未改）**
   `markdown_service.cpp:641` 文本模式读（未开 `ios::binary`）→ 读入即归一为 LF；落盘走 `core::atomic_write` 的 **binary** 写 → 首次保存把 CRLF 洗成 LF。M4 的 CRLF 语义（`'\r\n'` 成对删、不计内容）因此**只能在纯函数层验证**。未改：若将来要保留 CRLF，会与 EUI input 无条件丢 `\r` 冲突。证据：`6c0cb6d`（M4）commit 正文「登记（非本线修复）」。

5. **MD-01 残留入口需块身份**
   外部改文件触发静默重载（`reloadCurrentNote`）后活动块跟错内容：锚不复位，且**不能**照抄「reload 时 `liveCursorValid_ = false`」（实测同场景结果逐位相同 —— 编辑态光标是进实时态那一刻的陈旧值，重置反而把光标拉回更早位置）。可达性：实时态 + 无未保存编辑 + 外部改文件（mtime 按**秒**比较 + 5s 轮询 ⇒ 最坏约 6s）；后果当帧收敛、**无数据丢失**。最小修复也要 stableId / 旧块起点映射 = spec D3 拍板项。证据：`2026-09-27-open-questions-review-B.md` §1（CONFIRMED）+ `daa6c4b`（M3 已就地注释登记）。

6. **CPX-13 只交付能力、未交付生效**
   `dialectOverride` 加了字段、加了 v9 迁移、`forBaseUrl` 也加了可选参数，但三个调用点（`testConnection` / `proxy_router` / `streaming_proxy`）全走缺省行为 = 功能等于没上。补接线线 `4105db6`（03:17）后用变异检验证明（回退传参则 3 用例全红）。**规则：能力线必须跟一条接线线**（§0.3 已沿用）。

### 5.3 最终口径

- **最近一次全量**（主工作树，`bash scripts/build-test-run.sh --no-run`）：**`Total: 737 / Passed: 736 / Failed: 0`** + 两条守卫（发布形态 ✅ / 模块隔离 ✅）—— 主会话实测值。**本文落笔时仓库内无运行留档**（`build/` 下无测试日志，只有 `.ninja_log` / 产物），故该数字以主会话实测为准、未作独立复核。静态对账：`grep -rE '^\s*REGISTER_TEST\(' tests/test_{core,platform,modules}` = **738**（比运行 `Total` 多 1，未逐条对账 —— 与波 2 时 708 vs 707 同形）。
- **本夜提交与 merge**（`git log --oneline` 实测）：`--since='2026-09-27 20:00'` → **267 个提交 / 111 个 merge**；其中 `--since='2026-09-28 03:00'` → **55 个提交 / 26 个 merge**；§5.1 覆盖的续做批（03:52 → 06:19）= **18 个 merge + 1 个直接提交**（`6df00e4`）。
- **用例总数口径仍为「以运行输出 `Total:` 为准」**，不在文档写死；上面 `737` 只是**当时快照**。

### 5.4 剩余待办（更新版，覆盖 §3 的旧列表）

| 状态 | 项 |
|---|---|
| ✅ | 波 2 收尾 3 条：TIN-08（`c10c73e`）/ MD-03（`6d8d633`）/ TIN-06b（`d3e303a`）+ 独立加固 3 条：fix-flake（`2bf855c`）/ TIN-08b（`252aa9b`）/ fwatch-arm（`a9ce037`）+ 残项二批（`4d37216`） |
| ✅ | R 线：R0（`48a62cc`）、R1（`0057fb5`）、R2（`ff6657f`）、R3（`077af6b`）、R4（`8076ca1`）、R5（`ba815d3`）、R6（`2edf604`）、R0 收尾（`5bad7d4`） |
| ✅ | Markdown Phase3 M3（`daa6c4b`）/ M4（`7e5fba6`）/ M5（`9829acd`） |
| ✅ | MD-01 本体（`e09feb8` → `1784a37`）+ 残留入口就地注释登记（`daa6c4b`） |
| ✅ | 审计表口径刷新（`6df00e4`）；CPX-13 接线 + 变异检验（`4105db6`）；CPX-08 的 UI 用例 pin 剩余项已清（`4d37216` + `632e673`，无需再排） |
| ⬜ | **R7** `markdown_module` 拆分（MD-04）、**R8** 装配层收口（PAB-02/03/04 + THS-08）、**R10** `ClaudeService` 拆分（CPX-01）—— **R9 已放弃**（用户决策；pet 项改走 1g / 2j 两条独立线，均已交付） |
| ⬜ | **Markdown Phase3 M7–M10**（依赖链 `M3 → {M4,M5,M6} → M7 → M8 → M9 → M10`；M6 在飞，见 §5.1 末行） |
| ⬜ | **MD-01 残留入口**（外部静默重载后活动块跟错内容）—— 需 spec D3 块身份拍板；最小修也要 stableId / 旧块起点映射 |
| ⬜ | **共享配置片段 UI 批 + failover 批**（spec `2026-09-27-shared-config-and-failover-design.md`）—— spec 内 `500 keychain_read_failed` 口径已过时 → 统一为 `503 no_active_profile`（CPX-10 后） |
| ⬜ | **TIN-06 剩余未迁项**：`test_streaming_proxy::TestEnv`、`SpikeEnv`/`FocusEnv`/`MdLiveEnv`（建议单列 `tests/frame_env.h`）、`test_settings::PruneEnv`、`test_streaming_proxy::AppDbScope`、`test_pet` 的 `writeTextFile` |
| ⬜ | **未登记的进程级全局**：`g_sidebarCollapsed`（`src/app_sidebar.cpp:34`）、`g_exitHookArmed`（`src/app_assembly.cpp:23`）—— 需新增复位接口，**属范围外** |
| ⬜ | CPX-08 残留（若有）已无未清项；仍待：THS-16 DST 本机不可复现、macOS `system_theme_macos.cpp` 未编译验证、TIN-13 截图段解释器错配（PIL/playwright）、`proxy_router.cpp` 旧 slug 注释 |
| ⬜ | **codegraph 索引瘦身**（闲时跑一次 `codegraph index D:/project/gui` 重建确认并纳入本夜新文件） |
| ⬜ | **EUI-NEO push**（本地 main 领先 origin；本机无推送通道 → 阻塞在用户侧） |
| ⬜ | **肉眼点验项汇总**（把各线报告里点到「待用户点验」的项合并成一张清单，见下） |

**待用户点验清单**（合并版）：

- 四模块 toast 两态（R1）
- 托盘通知
- 右栏三 pane / 左栏列表 / 表单 3 tab / 两弹窗（R4）
- 拖拽跨列、6 个对话框、搜索 / 逾期视图（R5）
- 设置页 9 张卡（R6）
- Markdown 实时态：点渲染块打字 / 组字中切块被拒 / 切块不回拉 / 多块撤销（M3–M6）
