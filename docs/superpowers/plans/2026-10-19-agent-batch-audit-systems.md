# 智能体批只读审计 — C++20 系统专家 lens（并发/UB/生命周期/泄漏/静默失败）

- 日期：2026-10-19；范围：`git diff 033d7db..dev -- src/ tests/`（36 commits，今晚智能体批）
- 口径：只读审计，零构建零测试；severity 按演进代价（未来改动踩雷的概率×代价）；不确定标「待证」
- 行号基准：dev 分支 HEAD（d954cab）

## Findings

### M1（Medium）`src/platform/notification.cpp:264` — `initialize()` 全仓库零调用点，死代码携带两条活语义
`platform::notify::initialize()`（notification.h:80 声明、notification.cpp:264-269 实现）在 `git grep dev -- src/` 下无任何调用点。后果：
1. macOS `MacBackend::initialize`（notification_macos.mm:120-160）里的「NSApplicationDidBecomeActive → 缓存置回 NotDetermined」观察者永不安装——注释声称的「用户运行中改系统设置后重新查询」口径不生效，`cached_` 一旦回填终身有效，与 mm 文件头注释描述的行为不符（文档-行为漂移）。
2. 启动期无预热：授权缓存首个查询/回调落地前，facade `send()` 的授权检查（notification.cpp:294）每次都可能走 ≤1s 同步查询（见 M2）。
维护成本：后续任何人按注释理解缓存行为（改设置后应重新生效）会写错依赖它的代码。
建议：在 app 装配期主线程调一次 `initialize()`（幂等已内建）；或删掉 initialize 的 macOS 激活观察者语义并同步改注释，二选一，不留「看起来接了线其实是死的」路径。

### M2（Medium）`src/module_agent/agent_module.cpp:1335` — D2 run 失败通知在 UI 主线程调 `send()`，macOS 授权未决期可阻塞主线程 ≤1s/次
链路：`refreshData()`（主线程，compose:1546 / pollRunEnd 装配层每帧）→ `platform::notify::send(n)`（agent_module.cpp:1335）→ facade 授权检查 `backend().authorizationStatus()`（notification.cpp:294）→ macOS 同步查询 + `dispatch_semaphore_wait` 1s 上限（notification_macos.mm:241-242）。`authorizationStatus` 查询成功会回填 `cached_`（mm:247），故阻塞窗口 = 授权 `NotDetermined` 未决期：授权弹窗挂着不答、或查询超时（mm:243-245 超时不回填）期间，每次 run 失败/受限通知都可能卡 UI 一帧至 1s。notification_macos.mm 头注释自己写明「**不要在 UI 主线程依赖它做阻塞式决策**：极端情况下会阻塞到 1 秒」——D2 恰好踩进这条自警。Windows/POSIX 恒 Authorized/Unknown 常量，不受影响。
待证：实际触发频率（授权未决窗口长短；本地真机授权已决定后窗口≈首查一次）。
建议：D2 的 OS 通知 send 挪到一次性后台（std::thread detach 或复用 run 收割线程）；或 facade 给 `send` 加 `SkipAuthCheck` 档由调用方声明线程语境。M1 的预热能缩窗但不消除未决期。

### L1（Low）`src/module_agent/agent_module.cpp:591-619` — C1 `activeRunFor` 裸指针逃逸出 `runsMu_`，置位/读取在锁外解引用
`activeRunFor`（591-598）在锁内选元素返回裸指针；`requestStopRun`（601-610）锁外 `run->cancel.store`，`isStopRequested`（617-619）锁外 `load`。今日安全的前提是「runs_ 全主线程读写」（头注释 278 自认：mutex 为纵深防御）——`reapFinishedRuns`（579）erase 同一对象与解引用之间无锁隔断，一旦未来任何调用点挪到非主线程（例如把停止按钮接到通知点击回调），即是 use-after-free。atomic 不能救已释放对象。
建议：`activeRunFor` 返回 `std::shared_ptr<ActiveRun>`（runs_ 本就存 unique_ptr，换 shared_ptr 零成本），或把 cancel 置位收进锁内。

### L2（Low）`src/module_agent/agent_module.cpp:579-590` — `reapFinishedRuns` 持 `runsMu_` join，与同文件自述纪律相悖
onStop:519 注释明写「join 不持锁：run 线程不碰 runsMu_，持锁会白堵并发访问者」，而 `reapFinishedRuns`（每帧从 compose:678 调）恰好在锁内 join 已 done 的 run 线程。无死锁（run 线程确不碰该锁），但 join 耗时期间 `isTaskRunActive`/`activeRunFor` 全部堵死——注释点名的代价在这里复现。主线程内自堵→同线程无感，仍是纪律漂移。
建议：与 onStop 同法——锁内摘出待 join 项，锁外 join，锁回再 erase。

### L3（Low）`src/module_agent/agent_module.cpp:2494、2513` — `startDraftGeneration` 不持 `draftMu_` 读 `draftJob_`，同成员两种锁纪律
`openDraftDialog`（2468-2477）、`reapDraftJob`（2544-2553）、槽位换入（2528-2530）全持 `draftMu_`；`startDraftGeneration` 的两处 `if (draftJob_ != nullptr)`（2494 早退、2513 reap 后复检）裸读。今日全主线程无竞态，但「同一成员一半调用点持锁一半不持」意味着 `draftMu_` 既不能证明必要也无法安全删除——下一个人无论怎么改都会踩一半。
建议：要么全部裸读写并给 `draftMu_`/`draftJob_` 加「仅主线程」注释删锁，要么三处全锁。别停在中间。

### L4（Low）`src/platform/notification.cpp:330、358-361` — 去重过期重建留陈旧 order 键，容量驱逐可提前抹掉新条目
同一 key 过期后重发：330 行 `g_dedupe.erase(hit)` 只清 map，`g_dedupeOrder` 里的旧键残留；358 行重插又 push 一份 → deque 含重复键。此后 359-361 容量驱逐从 front `erase` 时，可能把**新**时间戳的 map 条目提前抹掉（后续同键重复项 erase 成空操作）→ 2s 窗口内的同内容通知偶发漏去重。best-effort 去重，影响小，但缓存-队列双结构的不变式（order 中每键至多一个有效条目）已经破了，后续改容量/窗口参数的人会踩。
建议：重发路径改「更新 map 时间戳 + order 旧键惰性跳过」（erase 时标记，驱逐时 `if (g_dedupe.find(front()) == end)` 再 pop——现在的代码已经在 pop 后无条件 erase，改成先查即可）。

### L5（Low）`src/module_agent/agent_module.cpp:2688` — `performDeleteTask` 无 `isTaskRunActive` 二道闸，删除/起跑竞态仅靠 UI chip 禁用态
E1 的防「删在跑任务」只在渲染层：列表 chip `enabled=!runBusy`（1816-1821）+ 点击闭包再查一次 `busy`（渲染时捕获）。`performDeleteTask` 本体（2688-2702）不复查。窗口：确认弹窗开着时 scheduler/事件恰好起 run（confirm 点击在晚于渲染的帧）→ 删除事务与 run 收尾写库并发 → run 行/消息以孤儿 task_id 重入库。概率极低、后果轻（脏数据非崩溃），一行守卫可闭。
待证：scheduler 派发是否可能在「确认弹窗开着」的帧间发生（本次未审 scheduler.cpp 派发线程，超范围）。
建议：`performDeleteTask` 入口加 `if (isTaskRunActive(taskId)) { pageError_ = ...; return; }`。

## 已核面（逐面）

- **T7b 草稿 worker 生命周期**：已核，干净。onStop（agent_module.cpp:528-532）与析构（440-444）均 join 先于 `llm_.reset()`；worker 只摸 `raw`（DraftJob 槽位值拷贝）+ `done` release/acquire + join 构成完整 happens-before；epoch bump（close 2511 / start 2527）与 reap 丢弃分支（2581-2587）自洽；`std::thread` 创建失败 catch `system_error` 归还槽位不泄漏（2556-2562）；`reapDraftJob` 锁内摘、锁外 join，姿势正确。
- **T7b 解析层**（task_draft.cpp/h、task_draft_parse.h）：已核。纯函数、零 IO 零全局态；UTF-8 边界截断、fence 剥离 4 层 guard、once「必须晚于当前」防永不出触发任务、降级矩阵恒产可落库组合。nit：task_draft.cpp/task_draft.h 文件尾缺换行符（`\ No newline at end of file`）。
- **draft worker × ToolRegistry**：已核，安全。`registeredToolNames()` 在 worker 线程调 `ToolRegistry::listTools()`，registry 自带单锁串行化（core/tool.h:60-61「主线程写/AgentLoop 线程读」）；onStop join 先于 unregister（528→537-539），无窗口。
- **E1 store 级联事务**（agent_store.cpp:257-308）：已核，干净。`recursive_mutex` 锁 + `db_.transaction` + 子→父三条显式 DELETE（不押 FK pragma）+ `sqlite3_changes==0` → NotFound 整体回滚；语句 RAII 生命周期在事务提交前结束。
- **T1b 通知 facade**（notification.cpp/h）：已核主体。一把 mutex 护全部可变状态、后端调用在锁外（324 注释自证防 1.5s 串死）；清洗/UTF-16 码元截断不切代理对；令牌桶 seed 与 refill 数学正确；`send` 可任意线程的契约成立（除 M2 的主线程阻塞体验问题）。Receipt 三态→工具 JSON 映射（app_notify_tool.cpp）纯净，fallback 异常双 catch 不泄给 AgentLoop。
- **平台后端**：已核。macOS bundle 守卫先于一切 UNUserNotificationCenter 触碰（mm:173）；posix 诚实降级 BackendUnavailable/Unknown；win 恒 Authorized + NIF_INFO 语义注释与实现一致。
- **D2 迁移检测**（除 M2）：已核。`notifiedRunIds_` 「同 runId 一次」去重保留、稳定 id `agent-run-<runId>` 复用系统替换语义、失败 LOG_DEBUG 不重试（口径自洽：受理≠展示，静默抑制不重试是明说的取舍）。
- **C1 停止语义**（除 L1）：已核。协作式 cancel 位 + agent_loop 迭代边界检查（头注释 138-142）、无 run 落空仅 LOG、幂等；详情弹窗 chip 渲染条件与「停止中」反馈态自洽。
- **hotTool_/notifyTool_ 注册注销**：已核。onStop 注册表清理顺序（join → 退订 → stopAll → unregister → reset）与头注释纪律一致；hotTool_ 自持缓存限频，模块零附加态。
- **tests**：新面均有覆盖（test_agent_draft / test_agent_store deleteTask / test_notification facade / test_notify_tool Receipt / agent_e2e_probe）；范围内无整测试文件删除（大行数删除为断言随新契约重写）。

## 汇总

Medium 2 / Low 5；无 High/Critical；待证 2 处（M2 触发频率、L5 scheduler 窗口）。本批并发骨架（join 先于资源释放、epoch 作废、锁外后端调用）方向正确，findings 集中在「锁纪律声明与实际不一致」与「死代码携带活语义」两类演进地雷，无今日即坏的缺陷。
