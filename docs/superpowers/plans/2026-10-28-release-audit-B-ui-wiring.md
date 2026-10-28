# 上线模块独立审计 B —— UI 与接线

> 审计者：独立只读审计（不参与实现，未改任何源码；唯一落盘物为本文件）。
> 工作树：`D:/wt-rel-core`（分支 `wt-release-core`），审计基线 HEAD = `06107f70`（审计 A 提交 d806435e 晚于本审计开始，不影响本文件结论）。
> 对照基准：`docs/superpowers/specs/2026-10-27-release-module-design.md`（工作区版，审计时已被改）+ `docs/superpowers/specs/2026-10-27-release-visual-tokens.md`（本分支不含此文件，取自 4980cec1:docs/superpowers/specs/2026-10-27-release-visual-tokens.md，即 feature/20261027-release-module 上的版本，仅作视觉值对照）。
> 结论分档：**3 blocker / 6 major / 14 minor**。

## 0. 审计前记录与并发改动声明

- 审计开始时 `git status --porcelain` 共 53 个变更/未跟踪条目（39 已跟踪修改 + 14 未跟踪），本审计只覆盖任务书列出的文件。
- **审计期间被其它线修改的文件（结论可能已失效）**：
  | 文件 | 审计开始时 mtime | 最终 mtime | 处理 |
  |---|---|---|---|
  | `src/module_release/release_reminder.cpp` | 13:33:56 | 13:42:30 | 最终版已重读（内容渲染修复 + 间距上移），下方结论基于 13:42 版 |
  | `src/module_release/release_module.cpp` | 10:46:23 | 13:44:39 | 最终版已重读（新增 `onBack` 接线），下方结论基于 13:44 版 |
  | `src/app_sidebar.cpp` | 11:36:46 | 13:42:34 | 改动落在流质背景段；分组树相关段落（:386-:572）已复核未变，下方结论仍有效 |
- 其余范围内文件 mtime 均早于审计开始，结论未受并发影响。

---

## 1. 正确性（对照 spec §4–§8）

### 已核对通过（不重复展开）

| 行为 | 落点 |
|---|---|
| 分组默认折叠、父项点击只切展开、当前页在组内时父项承载选中态（子项隐藏、不强制展开） | `app_sidebar.cpp:386-448,559-570`，单测 `test_release_basic.cpp` 4 条 + probe A 组 |
| 周二/周四直接建、其余日期二次确认（换日期后旧确认作废）、同日已有非作废任务直接打开、作废不占当天名额 | `release_dialogs.cpp:284-303,143-231`，`release_store.cpp:665-685`（SQL 排除 voided） |
| 快照：环境/服务/类型/默认分支在创建时复制，之后基础配置改动不回灌 | `release_dialogs.cpp:197-222` |
| 工单卡按配置内容自动出现（未移除环境下的未移除服务、未跳过且内容非空） | `release_detail_ui.cpp:311-325`（与 `release_logic.cpp:51-73` 同口径） |
| 单据勾选前单号必填：勾选框禁用 + hint + 写入口二次拦截 | `release_detail_ui.cpp:1757-1792,472-504` |
| 八步清单：合并分支随时可做；配置/部署/验证要求 `nowMinute >= env.startMinute`；当前步高亮（首个未完成未跳过且 timeReady） | `release_detail_ui.cpp:592-689,2097-2105` |
| 配置项顺序仅 Draft/Pending 可调（执行中锁定）；服务顺序拖拽同理 | `release_detail_ui.cpp:692-741,926-973`（`isConfigPhase`） |
| 撤回：任何一步、二次确认、记 action=revoke 日志、Done/Voided 只读禁撤回 | `release_detail_ui.cpp:1134-1208,2683-2687` |
| 提交校验（validateTask 不过不放行、标红 + 弹清单）与独立「校验」按钮（只读、随时可跑） | `release_detail_ui.cpp:778-844,2551-2560` |
| 作废：二次确认、停止提醒、整单只读、可恢复；恢复按当前进度与时间定状态，且同一天已有非作废任务时拒绝恢复（作废不占名额的反向护栏） | `release_detail_ui.cpp:1089-1132` |
| 瀑布流 10 条分页 + 列宽公式（1600 窗 4 列×320 / 1000 窗 2 列×356）+ 加载占位/失败重试/矮窗自动续载 | `release_list_ui.cpp:509-617,671-699,910-1077`，`test_release_task_ui.cpp` 几何断言 |
| 提醒窗：单窗聚合、多档取最高档配色、条目各自色标、只跳转不勾选、关闭后 5 分钟冷却 + 指纹去重、重启补弹、全做完自动关窗、Draft 未到基准静默 | `release_reminder.cpp:518-684,722-770`；到期判定单源 `release_logic.cpp:121-178` |

### blocker

**B1. 配置内容无任何填写/查看/编辑入口 —— 后端服务任务的提交在实际上永远失败（核心流程断链）**
- 位置：`release_module.cpp:146-157`（`ReleaseDetailActions` 只接了 `onBack`，`viewConfig` 未注入）；`release_detail_ui.cpp:2181-2195`（「查看」按钮点击只报「未接入配置查看层」）；`release_dialogs.cpp:570-571`（`drawFullConfigEditorDialog` 空实现且无调用方）；分支同样不可编辑（`release_detail_ui.cpp:2036-2047` 纯展示文本，`updateTaskService` 无任何 UI 调用方）。
- 影响：spec §4.2 要求配置内容「粘贴填写」、§4.4 要求提交后可改配置。现状后端服务的 5 个配置项内容恒为空 → `validateTask`（`release_logic.cpp:198-210`）恒报「配置内容为空」→ 点「提交」恒弹校验清单（`release_detail_ui.cpp:784-794`）→ 任何含后端服务的任务都无法提交。`applyContentRevocations`（`release_detail_ui.cpp:2745-2806`）里「改内容/改分支自动撤回 + 记日志」的机制已实现且正确，但**生产装配下无任何路径能触发**（配置内容与分支都改不了）。
- 建议：由任务页模块注入 `viewConfig`（接一个全屏配置编辑层，编辑保存后置 `state.dirty`），分支行改为可编辑；否则 B1 连带 spec §4.4 整条不可交付。若 viewConfig 层在后续票据，spec §11.1 必须把它列为「未实现」而不是只列提交/服务拖拽。

**B2. 跳过分两级均无 UI 入口（配置项级保留 / 服务级移除 / 环境移除）**
- 位置：`release_detail_ui.cpp` 全文只有 `skipped` 的**读取**与渲染（`:2064-2067,2778,2783`），无任何把 `skipped` 置 1 的交互；`release_store.cpp:813`（`removeTaskEnv`）、`:874`（`removeTaskService`）为零调用方；执行页无「移除环境/移除服务」按钮。
- 影响：spec §4.2「配置项跳过：某项本次没有 → 勾跳过（显示已跳过并划掉，校验不报缺）」「移除环境/移除服务（二次确认）」全部缺失；与 B1 叠加，用户对「本次没有」的配置项没有任何办法绕过校验，勾选框的「跳过」视觉态（`release_checkbox.h` 四态之一）在真实数据上永不出现。
- 建议：执行页补三个入口（配置行「跳过」toggle、服务卡「移除」、环境卡「移除环境」，均带二次确认；服务/环境移除调 `removeTaskService`/`removeTaskEnv`）。同样，若属后续票据必须在 spec §11.1 明示。

**B3. 任务创建非原子：createTask 成功后 addTaskEnv 中途失败 → 残留无环境/半环境任务，且无法补救**
- 位置：`release_dialogs.cpp:183-222`（先 `store.createTask`，再逐环境 `store.addTaskEnv`，中间失败只 `state.error` 返回，不回滚）；`release_store.h` 无 `deleteTask`。
- 影响：残留的 Draft 任务占住当天日期，`findTaskByDate`（`release_store.cpp:665-685`）此后对同日「新建」恒返回 OpenExisting，用户被永久导向这个坏任务；坏任务内又无法补环境（加入环境只在创建弹层），成为死单。
- 建议：store 层提供「createTask + addTaskEnv 整事务」的组合写口（一次 transaction），或失败时补偿删除任务；至少失败后立即重试创建路径可覆盖残留。

### major

**M1. 提醒假红：环境红条在服务全部完成后仍持续弹出**
- 位置：`release_logic.cpp:164-170`（`envHasLiveService` 只判「环境里有未移除服务」，不判进度）；条目标题却写「部署未完成」（`:168`）。
- 影响：只要环境里有服务且时间窗已到，红条就存在；即便该环境 8 步全走完、只剩任务级公告未发（公告自己只有黄条），红条也不消失。`release_logic.h:29-31` 注释声称「全部做完后任务状态转 Done，届时整体返回空」——但 Done 要求任务级前置也全部完成（`release_logic.cpp:291-298`），前置未清时红条与黄条并存，红条内容（「仍有服务未完成」）与事实矛盾，用户会被持续误导。spec §6.2「红 = 部署未完成」名不副实。
- 建议：`dueItems` 签名补入 `TaskStep`（或由调用方传「每环境未完成服务数」），仅对确实有未完成步骤的环境发红；签名属外部契约，改动需同步 `release_reminder.cpp:351` 与全部测试。

**M2. 列表卡进度分母与执行页不一致（后端服务少 5 项配置）**
- 位置：`release_list_ui.cpp:770-838`（`releaseLoadTaskCards` 只读 DB 里已有的 `release_task_config` 行）；`release_detail_ui.cpp:327-350`（执行页对后端服务补 5 个 id=0 占位行参与进度/校验）；`release_store.cpp:769-814`（`addTaskEnv` 不建配置行）。
- 影响：同一任务，列表卡显示 `0/6`（3 固定步骤 + 提测/CAB/公告），执行页显示 `0/11`（3 + 5 配置 + 3 前置）。用户从列表点进详情，进度分母跳变，显得数据不一致。
- 建议：`releaseLoadTaskCards` 复用同一套「后端服务固定 5 项」口径（对 Backend 服务把缺失 kind 计入 total），与 `orderedConfigs`/`appendSyntheticConfigs` 收敛到共享 helper，避免两处口径漂移。

**M3. 提醒窗开窗失败 = fail-dead：会话内提醒永久失效且无任何告警**
- 位置：`release_reminder.cpp:626-656`（`app::openWindow` 无失败/超时回调；成功依赖 `onWindowCreated`）；`windowOpen=true` 在开窗前就置位（`:586`），只有 `handleWindowClosed`（销毁回调，`:671-678`）能复位。EUI-NEO `openWindow` 实现（`EUI-NEO/include/eui/detail/dsl_app_impl.h:166-198`）只是把请求入队，窗口创建失败时 `onWindowCreated`/`onWindowDestroyed` 都不会回调。
- 影响：窗口创建失败（资源不足/后端异常）后 `handle` 恒 null、`windowOpen` 恒 true，此后每个 tick 走「已开窗，只刷新内容」分支（`:545-552`），永不重开、永不重试、无日志。提醒是本模块核心价值，静默死掉无信号。
- 建议：tick 内对「windowOpen 为 true 但 handle 为 null 且超时 N 秒」做看门狗复位（置 false 并 LOG_WARN），下一 tick 重开；或给 `openWindow` 的请求增加失败回调。

**M4. 每秒全量 N+1 扫描（提醒聚合 + 列表陈旧刷新叠加）**
- 位置：`release_module.cpp:95-106`（ticker `intervalSec=1`）；`release_reminder.cpp:294-397`（每次 tick `listTasks()` 全表 + 每任务 envs + 每服务 configs 逐条查询）；列表页另有每 1s 陈旧刷新整页重载（`release_list_ui.cpp:975-989`，每卡再 N+1）。
- 影响：任务量上来后（几十个任务 × 5 环境 × 10 服务）每秒上千条 SQLite 查询全部跑在 UI 线程，列表页滚动卡顿、空闲也持续耗电；与本机性能约束（慢机器上减少无谓工作）直接冲突。
- 建议：提醒扫描降频（如 30s）或按「最近到期时刻」排下次唤醒；列表陈旧刷新加最小间隔与变更检测（如 count 不变则跳过重载）。

**M5. 列表状态 chip 用持久化状态，不按时间/完成度推导**
- 位置：`release_list_ui.cpp:786`（`card.status = task.status` 直读库值）；推导逻辑只在执行页 `advanceTaskStatus`（`release_detail_ui.cpp:1041-1076`）。
- 影响：spec §7「状态只跟时间走」。用户建单后不再进执行页时，列表上的「待执行」永远不会自己变「执行中」，只有打开过详情才收敛；对每天都看列表页的用户，状态与时间窗脱节。
- 建议：`releaseLoadTaskCards` 对非 Draft/Done/Voided 的卡按 `planMinuteNow` + `taskProgress` 推导展示态（只改展示，不写库），与执行页口径同源。

### minor

**m1. 恢复绕过提交校验**：Draft 任务作废后恢复 → `restoredStatus`（`release_detail_ui.cpp:1078-1087`）直接给 Pending/Running，从未经过 `validateTask`；一个配置不完整的 Draft 可以「作废 → 恢复」变 Pending，提交门被绕过。建议：恢复前若任务从未提交过（可依日志/状态判断）应回到 Draft。

**m2. `applyContentRevocations` 的 skipped 断链**：`release_detail_ui.cpp:2781-2799` 匹配行若 `cfg.skipped`（本次被置跳过）则 `break` 不撤回旧确认——doneAt 残留在库中。进度口径忽略 skipped（`release_logic.cpp:242-245`）所以暂不影响数字，但恢复非跳过时会凭空「已完成」。建议：匹配到且 doneAt!=0 即撤回，skipped 不构成豁免。

**m3. `planMinuteNow` 与提醒引擎的 DST 口径不一致**：执行页用「日历日差×1440」（`release_detail_ui.cpp:128-145`），提醒引擎特意用 mktime 防 DST（`release_reminder.cpp:96-117` 注释）。DST 切换日执行页的门控/超时判据会偏移 ±60 分钟，与提醒窗自相矛盾。建议统一到 mktime 口径。

---

## 2. 回归面

- **PageId 重排**：Release* 三值插在 Wallpaper 与 Settings 之间，Settings/About 后移 3 位（6→9 / 7→10）。消费者核对：`app_sidebar.cpp:523-526` 按名分 system 组（不受序值影响）；`router.cpp:16-24` 升序插入（表序=枚举序，注释已同步）；home 卡跳转按名；`test_router.cpp` 改为逐位数组比对（更严，无放宽）；`test_wallpaper_module.cpp:98-102` 的 `Agent<Wallpaper<Settings` 仍成立。**未发现依赖旧数值序的消费者。** 只有一处注释残留：`test_claude.cpp:5-9` 仍写「pageIdToString 映射与序号」但断言的是 ClaudeProxy==3（未变，成立）。
- **ModuleEntry 新增 `parent`**：默认 `std::nullopt`、置于末位（`app_registry.h:41`），既有 8 行五参数聚合初始化不受影响；`assembly_registry_table_shape` 已锁 11 行与表序。
- **settings 四处改动**：合法 ID 清单加 release（`settings_config.cpp:46-49`）；默认启用集 6 项（`:79-84`）；版本化迁移 `moduleDefaultsVersion`（`settings_config.cpp:54-76,479-540`，`test_module_gating.cpp` 新增存量升级用例，逻辑自洽：标记缺席=旧配置→并入 release 并回写，用户显式关闭不会被重开）；开关卡第 5 项「上线」+ 卡高 254（`settings_ui_cards.cpp:415-418`、`settings_ui_logic.h:103`，`test_settings.cpp` 钉值同步）。**零影响既有 4 格布局。**
- **两个既有 probe 的适配**：`uat62_page_shots_probe.cpp` 只改 slug 顺序/编号注释 + `static_assert` 锁行数（断言收紧，非放宽）；`module_toggle_sidebar_probe.cpp` 只改一处注释数字。符合「补表行未放宽断言」。
- **app_sidebar 平台下沉**：原 Win32 查询逐行迁入 `window_visibility_win.cpp`（结构/判定/降级/日志文本一致，diff 已逐行比对）；非 Windows 从「恒 true」变为真查询（macOS 行为改善，非回归）。分组树对既有 8 项的渲染路径未动。

## 3. 平台对称性

- 业务层零平台宏：`src/module_release/`、`app_sidebar.cpp`、`app_assembly.cpp`、`settings_config.cpp` 内无 `windows.h` / `#ifdef _WIN32`（已 grep 证实，唯一残留是 `settings_config.cpp:252` 注释提及 NSVisualEffectView）。
- `window_visibility_macos.cpp`：静态合理性 OK——GLFW 查询（GLFW_ICONIFIED/GLFW_VISIBLE 与 Win 的 IsIconic/IsWindowVisible 语义对齐）、`glfwGetCurrentContext()` 取句柄 + fail-open、`EUI_WINDOW_BACKEND_SDL2` 编译期 guard；被非 Apple POSIX 分支复用（CMake 注释说明可链接）。本机无法编译验证，仅静态审查。
- `file_open_macos.cpp`：`_NSGetArgc/_NSGetArgv` 解引用正确；openDirectory 双 fork 防僵尸、execl 直传 argv 防注入，实现自洽。**刻意缺口**：odoc（Finder 双击）桥未实现（文件头已注明原因与接入清单），导致 `enqueueOpenFile`（`file_open.h:41-50`）当前无任何生产者，队列路径是死代码——属有记录的留口，非静默缺陷。
- Windows 侧 `file_open_win.cpp` / `window_visibility_win.cpp` 与旧实现等价（见回归面）。

## 4. 静默失败面

- **Result 忽略点**：执行页写操作失败全部经 `setNotice` 回显（含自动撤回失败 `release_detail_ui.cpp:2760`）；列表加载失败保留已加载内容 + 可点击重试（`release_list_ui.cpp:1069-1074`）；基础配置读失败整页错误行（`release_config_ui.cpp:1143-1158`）。未发现被吞的 Result。
- **提醒窗开窗失败**：见 M3（唯一真正的静默死点）。
- **提醒 store 读失败**：本 tick 不更新现窗 + 首次错误记日志（`release_reminder.cpp:530-537,680-684`），不误判「全做完」。OK。
- **新建弹层读基础配置失败**：`dataLoaded` 置位防止每帧重打库，错误在弹层内显示，重开时重试（`release_dialogs.cpp:96-115`）。OK。
- **空实现/占位**：`drawFullConfigEditorDialog` / `drawWithdrawConfirmDialog`（`release_dialogs.cpp:570-571`）空实现无调用方（配合 B1 存在）；`ReleaseViewState` 全类无消费方（`release_view_state.h/.cpp`）；`ReleaseReminder::demoModeActive()` 无调用方（`release_reminder.h:50-55`，实现侧用 `readDemoLevelFromEnv`）；`composeReleaseListPage(ui,screen)` 骨架重载是 db 未注册时的真实兜底（`release_module.cpp:137-139`），非死代码。均为 minor 级死 API，建议随配置编辑层落地时一并清理。
- 提醒窗「去处理」在 Router 未装配时只置 openTaskId 并 LOG_WARN（`release_module.cpp:183-188`），有信号。OK。

## 5. 测试有效性

**有断言可变红的行为**：分组默认折叠/展开/点击/选中态/既有序（`test_release_basic.cpp` 6 条 + `release_probe` A）；日期约束/同日复用/分页/瀑布流几何/状态文案（`test_release_task_ui.cpp`）；基础配置渲染/表单/删除确认（`test_release_config_ui.cpp`）；到期聚合/校验/进度/完成判定（`test_release_logic.cpp`）；存储 CRUD/分页/日志（`test_release_store.cpp`）；ticker 时间维度（`test_ticker.cpp`）。

**没有任何能变红的断言的行为（缺口清单）**：
1. **提醒引擎/提醒窗零测试**：无 `test_release_reminder*` 文件；冷却/指纹/补弹/自动关窗/「只跳转不勾选」全部无覆盖。且 `tests/probes/release_probe.cpp:619-624` 注释声称「提醒引擎的调度/抑制逻辑已有无头单测覆盖」——**该声称不属实**，需删除或补测试。
2. **单据号禁用态**（勾选框 disabled + hint）无任何断言（B1/B2 修复后此门槛成为提交放行的唯一防线，必须有测试）。
3. **拖拽**：服务顺序/配置顺序 drag（含执行中锁定、无位移不落库、日志还原顺序）无测试；`parseServiceIdOrder`/`lastReorderOrder` 也无单测。
4. **自动撤回**（`applyContentRevocations`：改分支/改内容 → 撤回 + revoke 日志）无测试。
5. **提交/独立校验/撤回二次确认/作废/恢复** 全部无测试（仅 `release_logic::validateTask` 纯函数被覆盖，UI 侧放行链路没有）。
6. **file_open 队列**（enqueue/take，`file_open.h:41-50`）无测试（`test_file_open.cpp` 只测 selectOpenFileArg/openDirectory）。
7. `release_probe` 明确不覆盖 E（提醒窗），A-D 只钉视觉 token，不断言交互行为。
- 结论：纯逻辑层测试充分，**UI 行为层大面积裸奔**；B1/B2 修复落地时必须同步补 2-5 的用例（每条按「防什么事故」写 description，与既有风格一致）。

## 6. 文档口径

- **spec §10 与实现矛盾**：spec 表内写「进程内状态，默认展开，不落库」（design.md:253），实现为默认折叠（`app_sidebar.cpp:41-43` + `app_sidebar.h:44-48` 注释 + probe/单测均按折叠断言）。应以实现为准改 spec（任务书口径即「默认折叠」）。
- **spec §11.1 已过时**：写「详情页『提交（Draft→Pending）』与服务顺序拖拽排序尚未实现，属后续票据」——两者均已实现（`release_detail_ui.cpp:778-808,926-973`）；而真正缺失的「配置内容编辑、跳过、移除环境/服务、分支编辑」反而没列进限制清单（见 B1/B2）。
- **spec §11 职责表夸大**：`release_detail_ui` 行写「配置查看与顺序」——viewConfig 未接线，配置查看实际不可达（B1）。
- **spec §10「既有 8 行的元数据、节点 id 与几何保持不变」不准确**：上线父项占一整行（40+4），Settings/About 整体下移 44px（`test_release_basic.cpp` 的 `release_sidebar_existing_eight_order_and_shifted_down` 自己都断言了下移）。「元数据/节点 id 不变」成立，「几何不变」不成立，建议改为「相对顺序与缩进不变」。
- **提醒三档配色为硬编码字面量**（`release_reminder.cpp:128-136`：红 #EF4444、橙 #F59E0B、黄 #EAB308），未走 `theme::Tokens`（error/warning 有对应 token，黄无对应 token 档），与视觉 token 文档「取值一律以 theme token 为准」的纪律不符，深色档不跟随主题。建议红/橙改用 `t.colors.error/warning`，黄登记为局部常量并在视觉 token 文档同步。
- **过时数字/文案**：`tests/probes/release_probe.cpp:619-624` 的「已有无头单测覆盖」失实（见 §5）；`release_reminder.cpp:591-595` 临时诊断 `LOG_INFO`（注释自称可删）仍遗留。
- spec §6.2 红档定义（「部署未完成」）与实现粒度不符（见 M1）。

## 附：本次审计的文件 mtime 快照（结论锚点）

```
2026-10-28 13:42:30  src/module_release/release_reminder.cpp   ← 审计中改过，结论基于此版
2026-10-28 13:44:39  src/module_release/release_module.cpp     ← 审计中改过，结论基于此版
2026-10-28 13:42:34  src/app_sidebar.cpp                        ← 审计中改过，分组树段落已复核
2026-10-28 11:01:31  src/module_release/release_detail_ui.cpp
2026-10-28 11:07:41  src/module_release/release_list_ui.cpp
2026-10-28 09:26:34  src/module_release/release_config_ui.cpp
2026-10-28 09:26:41  src/module_release/release_dialogs.cpp
2026-10-28 11:53:01  tests/test_modules/test_release_basic.cpp
2026-10-28 11:47:52  tests/probes/release_probe.cpp
（其余范围内文件 mtime 见 0 节记录，均早于审计开始）
```
