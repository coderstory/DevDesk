# 上线模块 批次 3+4 UI 与接线 合规审计 C2（2026-10-28）

> 审计方式：只读静态核对（本机免编译指令生效中，未构建、未跑测试）。
> 审计基线：`8c6b27b2`（HEAD；批次 3+4 已由 `6ca2174a` 全量提交，worktree 干净）。
> spec 基线：`docs/superpowers/specs/2026-10-27-release-module-design.md`（HEAD 版，含 u8/u9/u10/u14 口径修复）。
> 与既有审计的关系：审计 B 已审批次 2 之前代码，本审只覆盖批次 3+4 的 UI 与接线；审计 A/B 的 minors 分诊见 `2026-10-28-release-minors-triage.md`。

## 一、对照清单逐项结论

| # | 项 | 结论 |
|---|----|------|
| 1 | 配置编辑层（editorOpen/editorServiceId/editorKind/editorText 四态、页内 DialogBuilder screen 编辑层、字数统计、只读态） | **已核**。四态齐备（`release_detail_ui.h:124-127`）；编辑层为页内 `components::dialog().screen(screen.w, screen.h)` 全屏（`release_detail_ui.cpp:2868-2877`）；字数统计 `utf8CharCount` 按码点计（`:2941-2948`）；已跳过/整单只读时只读展示 + 提示原因 + 无保存入口（`:2853-2939`）。保存只写 content，置 dirty 后由 `loadDetail → applyContentRevocations` 对账自动撤回（`:1282-1311`、`:3065-3126`） |
| 2 | 跳过/移除 UI | **已核**。跳勾用自绘 checkbox（`release_checkbox.h`，选中/跳过/禁用/hint 四态）；跳过与完成互斥（`drawStepRow` 仅 `!row.done` 画勾选框 `:2018-2030`；`toggleConfigSkip` 拒已完成项 `:1240-1243`；行 done 判定跳过优先 `:2228`）。服务移除按钮 + 二次确认（`:2125-2145`、`:2991-2996`、`:3039-3043`），环境移除同口径（`:2441-2458`、`:2997-3002`）；Done/Voided 下移除/跳过入口消失（readOnly 时 removeW=0、onSkipToggle 空），勾选/排序/撤回/下一步全禁用 |
| 3 | m1 恢复提交门 | **已核**。`applyRestore` 扫描 `state.logs` 中 `action=="submit"`，无则回 Draft（`:1089-1099`）；同日非作废任务占位拒绝恢复（`:1079-1088`，`findTaskByDate` SQL 排除 voided，`release_store.cpp:666-673`）。**日志来源核验见第四节** |
| 4 | P2 三件 | **已核**。①列表卡 `releaseLoadTaskCards` 改用 `deriveStatus`（`:836`），时间推导不回写库；②详情页前置卡走 6 参 `dueItems`（`:1777`），提醒引擎同入口（`release_reminder.cpp:317`），5 参兼容重载仅剩旧测试；③`appendSyntheticConfigs` 全仓无残留，缺行由 `orderedConfigs` 按 5 期望槽位补 id=0 占位行（`:322-350`），渲染/校验/拖拽/下一步消费同一清单 |
| 5 | P1 两件（提醒窗层） | **已核**。`windowOpen` 只在 `onWindowCreated` 确认句柄后置位（`:609-615`）；5 秒看门狗两条（开窗请求在飞超时 `:555-564`；已开窗句柄失配超时 `:529-541`），宽限期 `kWindowOpenGraceSeconds=5`（`:44`）；`openGeneration` 代数守卫，迟到窗口自关不搅状态（`:616-680`）；迟到窗自关（closeForEmpty 在飞时补发关闭，`onWindowCreated` 确认后立即关，`:691-709`、`:672-674`）；用户关闭 5 分钟冷却 `kReshowSeconds=300` |
| 6 | release_module | **已核**。onBack 注入（`release_module.cpp:160-163`）；viewConfig 全仓无运行时调用方（仅注释残留），留空安全；Esc 分层：编辑层→确认框→返回列表（`:123-137`） |

## 二、六维度结论

1. **正确性** — 已核。编辑层保存→dirty→loadDetail→applyContentRevocations 链路完整（内容变→doneAt 清零+revoke 日志）；跳过与完成互斥；恢复门日志来源=全量持久化历史（第四节）；看门狗在正常创建路径不会误清：`windowOpen` 与 `handle` 在 onWindowCreated 同一回调内成对置位，tick 与窗口创建同在 EUI 主循环帧内（ticker 泵挂 `dslAppConfig()`，`main.cpp:641`），入队后下一帧即创建，5 秒宽限相对"下一帧"余量充足；最小化/挂起期间帧泵与 tick 同步停止，不产生误判；迟到创建由代数守卫兜底。见 m1、m4、m5 三处 minor。
2. **回归面** — 已核。编辑层 Esc 关层、无层时 Esc 返回列表语义保留（模块层分层处理）；编辑层/确认框为全屏模态（backdrop onClick=requestClose + panel blockPointer），打开期间点不到页头返回按钮，不存在"带层返回列表"路径；切任务时 loadDetail 清 editorOpen/editorText/confirm/notice（`:3141-3149`）；viewConfig 无调用方；orderedConfigs 同步进 `configsForValidation`（`:717-725`），提交校验清单与渲染同源。列表页进度分母与详情页不一致（A-02）仍为待拍板项，本审仅交叉引用。
3. **平台对称性** — 已核（静态合理性）。编辑层尺寸走 `screen.width/height` 逻辑像素；字数统计为纯文本码点计算；提醒窗 GLFW（Windows）/SDL2（macOS）双后端均回调 `onWindowCreated/onWindowDestroyed`（EUI-NEO `glfw_app_main.cpp:489-523`、`sdl2_app_main.cpp:507-543`），`getPrimaryMonitorWorkArea` 双后端实现（`window_backend.cpp:691/1436`）；borderless+alwaysOnTop+不可缩放全屏窗双平台常规形态。
4. **静默失败面** — 已核。编辑层保存失败经 notice 回显（`:1303-1307`），但**失败即关层丢弃草稿**（见 m3）；跳过/移除失败均回显 notice；看门狗除 LOG_WARN 外有实动作（清态允许重试、清指纹护栏允许立即重弹）；提醒引擎读库失败保持现窗不误关（`:512-515`），错误节流记录。
5. **测试有效性** — 6 条用例（`tests/test_modules/test_release_detail_ui.cpp`）断言结构有效：全部落在真实节点 id（`ui_kit`/`components::button` 均为 `<id>.bg` 挂 onClick；输入 `<id>.hit`；确认框 `<id>.primary/secondary.bg`），配合 DB 读回断言（content/skipped/removed 落库），实现错位会红。缺口见 M1、M2、m7。
6. **文档口径** — spec §5/§6/§11 与实现一致（含 u8/u9/u10/u14 已修复项：配置顺序上下移+拖拽手柄、缩进 16、几何下移 44px、§11.1 限制清单已按实现重写）。剩余口径差异见 m2（§9 action 词汇 void/restore 从未落库）。

## 三、Findings

### Blocker：0

### Major：2

- **M1｜自动撤回链零自动化断言**｜`tests/test_modules/test_release_detail_ui.cpp`（全文 6 用例）
  - 问题：spec §5.3 + 详情页契约"内容变了旧确认不作废→已完成的对应步骤自动撤回并记 revoke 日志"（`applyContentRevocations`，`release_detail_ui.cpp:3065-3126`）没有任何用例覆盖。6 条用例中无一条种子"已完成的配置（doneAt≠0）+ 修改内容 + 断言 doneAt 清零与 revoke 日志"，服务分支变化撤回（`:3072-3095`）同样无覆盖。该路径只能经整页 compose 触发（匿名命名空间函数），一旦 `loadDetail` 快照对比、dirty 置位或对账循环改坏，全绿照过。
  - 影响：本需求最核心的"保存后自动撤回该步"契约静默回归无警报；与"测试按场景写、每条注明防什么事故"的交付纪律相悖。
  - 建议：补 1-2 条用例：①种子 doneAt≠0 的配置 → 开编辑层改内容保存 → 断言库中 doneAt==0 + release_task_log 出现 action=revoke 行 + 行视觉回到未完成；②种子已完成合并分支 + 分支变更（直接 upsert 后置 dirty 重读）→ 断言 setStepDone 清零。
- **M2｜提醒窗（P1）调度/抑制/渲染零自动化断言**｜`release_reminder.cpp` 全部开窗状态机 + `tests/probes/release_probe.cpp:619-632`
  - 问题：全仓无 `test_release_reminder*`；probe E 块明说"提醒窗的调度 / 抑制 / 渲染行为目前没有任何自动化断言"。看门狗、代数守卫、迟到窗自关、5 分钟冷却、补弹全部只经本审静态核对，未实跑。
  - 影响：整块独立 OS 窗口功能（本需求唯一"新窗口"面）无任何回归防线；独立窗口路径是真机三连爆类事故的高发面。
  - 建议：写能跑 app 主循环的 live 探针（demo 模式已提供零依赖假数据入口 `DEVDESK_RELEASE_DEMO`，可覆盖开窗/关闭/冷却/去处理全链），或至少为 `Impl` 的纯状态机部分（tick 判定、指纹、冷却）拆出可无头测的纯函数。此缺口已在 minors 分诊 u13 记录，本审复核确认仍存在。

### Minor：7

- **m1｜applyRemoveService/applyRemoveEnv 无 readOnly 守卫**｜`release_detail_ui.cpp:1329-1353`
  - 问题：与 `toggleConfigSkip`/`markStepCompleted` 等不同，移除 apply 入口不查 `isReadOnly`。当前可达性由"按钮仅非只读渲染 + 确认框全屏模态"兜底，但防御缺口与其它写入口不一致。
  - 影响：无现时可触发路径（模态期间状态无法翻转为 Done）；属纵深防御一致性缺口。
  - 建议：apply 入口补 `isReadOnly` 检查 + notice，与同文件其它写入口同口径。
- **m2｜void/restore 状态变迁不记日志**｜`release_detail_ui.cpp:1058-1110` vs spec §9（`action` 词汇 create/submit/confirm/revoke/void/restore）
  - 问题：全仓实际写入的 action 仅 confirm/reorder/revoke/submit（grep 实证）；spec §9 声明的 void/restore（及 create）从未有调用方写库。作废/恢复这类终态变迁无审计留痕（spec §5.3 对撤回明确要求记日志，作废/恢复未明示，但词汇表已声明）。
  - 影响：文档口径与实际留痕能力不一致；历史回溯缺终态变迁记录。
  - 建议：applyVoid/applyRestore 成功后各 appendActionLog 一条（action="void"/"restore"），或明确收窄 spec §9 词汇并记录理由（与 A-12 同族）。
- **m3｜编辑层保存失败即关层、丢弃草稿**｜`release_detail_ui.cpp:1302-1307`
  - 问题：`saveConfigEditor` 失败路径 setNotice 后 `editorOpen=false`；用户重新打开时 `editorText` 被库内容覆盖，所输草稿不可恢复。
  - 影响：写库瞬时失败（锁/IO）时用户损失整段编辑内容，仅见一条 notice。
  - 建议：失败时保留 editorOpen（notice 已回显），用户可修正后重试或主动取消。
- **m4｜applyRestore 不重置 statusWriteFailed**｜`release_detail_ui.cpp:1074-1110`、`:1032`
  - 问题：若此前一次状态推进写库失败（statusWriteFailed=true），作废→恢复成功后该标志仍在；此后所有自动推进（Pending→Running、完成→Done）在 `advanceTaskStatus:1032` 被静默挡下，UI 显示推导状态而库中状态滞后，直到下一次 dirty 重读。
  - 影响：需"写失败 + 作废 + 恢复"三连的角落路径；显示与库状态不一致。
  - 建议：applyRestore 成功后重置 `statusWriteFailed=false`（applyVoid 同理可加）。
- **m5｜只读态打开配置编辑层仍会补建缺行**｜`release_detail_ui.cpp:1265-1267`（openConfigEditor→ensureServiceConfigs）
  - 问题：Done/Voided 下"查看"配置仍先调 `ensureServiceConfigs`，若该服务缺配置行（无交互即完成的边界数据）会对**已终态任务写库**（INSERT 5 行）。
  - 影响：违反"整单只读"的数据层边界；此类行若后续作废→恢复，空配置行计入进度会改变恢复状态判定。当前 UI 路径下可达性很低（终态任务的服务一般已全量落行）。
  - 建议：openConfigEditor 入口在 `isReadOnly` 时跳过 ensure（查看只需内存行；缺行时按占位行展示）。
- **m6｜列表页卡片装配每卡 N+1 查询**｜`release_list_ui.cpp:790-826`
  - 问题：每张卡逐环境 listTaskServices + 逐服务 listTaskConfigs + listTaskSteps（SQL 级每卡最多 ~25 条查询），整页一次性主线程执行；分页只减小条数不减小单卡查询数。
  - 影响：低配机（本机即低配）翻页首帧卡顿风险；提醒引擎已做 `listTaskStepsByTask` 归并，列表页未同口径归并。
  - 建议：与提醒引擎同款——按任务一次 JOIN 拉全步骤/配置再内存归组（可复用 `listTaskStepsByTask`，配置侧可加 `listTaskConfigsByTask`）。
- **m7｜测试断言缺口（字数统计/失败路径/Esc 分层）**｜`tests/test_modules/test_release_detail_ui.cpp`
  - 问题：①字数统计节点 `release.detail.config.editor.count`（"共 N 字"）无任何断言；②编辑层保存失败、跳过失败、移除失败三类 Result err 的 notice 回显无用例；③模块层 Esc 分层（编辑层→确认框→返回列表，`release_module.cpp:123-137`）无自动化覆盖。
  - 影响：三个次要契约面静默回归风险；其中 Esc 分层与"返回列表"回归面直接相关。
  - 建议：①在用例 1 补 count 节点断言；②补失败路径用例（TestDb 注入只读句柄或坏 SQL 即可）；③Esc 分层可在 module 级用例构造 ReleaseTasksModule 调 onKeyEvent 断言返回值与 openTaskId_ 变化。

## 四、恢复门日志来源核验结论（重点）

**结论：`state.logs` 是任务全部历史日志，非当前会话日志；重启后恢复不误判。**

证据链：
1. `loadDetail` 每次重读时 `state.logs = store.listLogs(taskId)`（`release_detail_ui.cpp:3189-3194`）。
2. `ReleaseStore::listLogs` 为单条无 LIMIT 查询：`SELECT ... FROM release_task_log WHERE task_id = ? ORDER BY at ASC, id ASC` 后 `fetchAll()` 全量返回（`release_store.cpp:1084-1102`），无分页、无截断。
3. `submitTask` 提交成功即 `appendActionLog(..., "submit", ...)` 持久化落库（`release_detail_ui.cpp:775`），`appendLog` 走 `release_task_log` 表 INSERT（`release_store.cpp:1069-1082`）。
4. 库为进程级 `appDatabase()` SQLite 文件，跨应用重启持久 → 应用重启后打开该任务，`listLogs` 读回的历史含提交时刻的 submit 行，`applyRestore` 的 `submitted` 判定与首次会话一致。
5. 唯一边缘：submit 时日志 append 失败（罕见写库错误）则该任务"已提交但无 submit 行"，恢复时回 Draft——方向安全（降级需重新走提交校验，不会绕过校验凭空得到 Pending/Running）。可接受，无需修。

## 五、关联既有记录（不重复列 finding）

- A-02（列表进度分母与详情 5 槽位不一致）：本审 P2 核对确认现状未变，处置为待拍板，不重列。
- u13 / A-12 / A-09 / u3 / u5：minors 分诊已记录，本审复核确认与 HEAD 一致（u4 注释修复已在 probe 中落地）。
- 审计 B 的 B1/B2/B3/M1~M5：不在本审范围（批次 2 前），分诊文件已列处置口径。
