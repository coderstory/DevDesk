# 存疑项独立复核 B（只审不改）

- 日期：2026-09-27 ｜ 基线：master tip `dc727cd`（本 worktree HEAD，工作区干净）
- 范围：8 条由各子任务报告自留的存疑项，逐条 **CONFIRMED / REFUTED / NEEDS-MANUAL** + 可复核证据
- 手段：只读源码取证；定向构建（`cmake --build build --target devdesk_tests`、`CMAKE_BUILD_PARALLEL_LEVEL=3`）+ 定向用例（子串过滤，非全量）；1 条临时无头探针（取证后已删，未入库）；`src/**` 未改、未启动 app
- 证据目录：`build/`（gitignored）内 `probe-out.txt`、`ccache-full.log`、`ccache-summary.txt`、`ccache-pid.txt`

---

### 1. MD-01 残留：reloadCurrentNote 不重置活动块锚
**结论**：CONFIRMED（锚确实不复位）；但报告描述的失效形态**部分不成立**——不是"钳到块首"，而是"偏移保持"：活动块换成覆盖旧光标字节的那个块，块内光标 = 旧字节 − 新块起点；旧字节超出新文档长时钳到**块尾**。无数据丢失。
**证据**：
- `src/module_markdown/markdown_module.cpp:reloadCurrentNote`（2712-2734：只做 setEditorText/脏位/mtime，无锚复位）对照 `:openNote`（2546 `liveCursorValid_ = false`）、`:setMode`（2263 同）
- 无头探针实测（同文件 `drawLiveColumn:1672-1702` 的两分支 + `drawLiveInputBlock:1864-1882` 回写）：外部前插 → `before block=1 text="\nbravo\n" cursor=4` →`reloaded=1`→ `after block=0 text="seed line one\n" cursor=10`；外部改短为 `tiny\n` → `after text="tiny\n" cursor=5`（= 文档尾，非块首）
**影响与可达性**：可达——实时态 + 无未保存编辑 + 外部改文件（mtime 按**秒**比较：`markdown_service.cpp:fileMtimeEpoch` + 5s 轮询 ⇒ 最坏 ~6s，同秒内改写不触发）；另一入口是冲突 toast 的「重载磁盘版本」（`resolveConflict(true)`，可带未保存编辑）。后果当帧收敛，之后稳定；input 文本恒等于新文档某块原文，且外部改值不触发 `onChange` ⇒ 不会把旧块文本灌回文档。
**最小修法建议**：**不要**照抄"reload 时 `liveCursorValid_ = false`"——实测同场景结果逐位相同（编辑态光标恰好等于旧字节），而用户在实时态打过字时编辑态光标是进实时态那一刻的陈旧值，重置反而把光标拉回更早位置。真要改需动"锚语义"（块身份 stableId / 旧块起点在新文档的映射），属 spec D3 拍板项 → 建议与 M3 `setLiveCursor` 同批，本批不动。

### 2. THS-01 反证缺口：新用例能否抓住回归
**结论**：CONFIRMED（能抓住）。按旧提交逐条对齐断言：旧实现把 `fs::exists(mdDir)` 内联在 `drawDirectoryCard` 里，每帧执行；用例步骤②（盘上建好目录、**不**调刷新点）在旧实现下必然读到 `exists=true` ⇒ 文案不再含「（尚未创建）」 ⇒ `DOCTEST_CHECK(notCreated(stale.first))` **失败**。旁证：同批还改了既有 `settings_takeover_status_row`（补 `onPageEnter()` 才看到新值）——旧用例正是依赖"每帧 IO"，契约已随修改变更。
**证据**：`git show c72c060 -- src/module_settings/settings_module.cpp`（旧 `const bool exists = std::filesystem::exists(mdDir, ec) && !ec;` → 新 `const bool exists = notesDirExists_;` 快照）；`devdesk_tests.exe THS-01` → `Total 1 / Passed 1`（新实现绿）。
**影响与可达性**：验证类，无运行时影响。
**最小修法建议**：无需修。若要闭环反证，用 `c72c060^` 的 settings_module.cpp + 现测试跑一次即可；本次未跑（需另建一棵全量 configure+build 的树，超出"定向构建"边界）。

### 3. CPX-02 设置页残留：接管警告仍按 managed 门控
**结论**：CONFIRMED（同款"展示门吞警告"仍在，且可达且静默）。
**证据**：`src/module_settings/settings_module.cpp:compose`（534 `if (managed && svc) warnLines = min(lastSyncWarnings().size(),2)`）与 `drawProxyCard`（1303 `if (managed && svc) { …逐行渲染 warns… }`）；对照已修的 `src/module_claude_proxy/claude_module.cpp:580-605`（去 ccManaged 门，注释自陈「设置页同样按 managed 门控列警告」）；`takeoverManaged_` = sidecar 存在性（`settings_module.cpp:refreshExternalState` 1598）。
**影响与可达性**：`settings_sync.cpp:takeOver` 在**写 sidecar 之前**中止的失败（299-304「解析失败，中止接管（未写入）」、365-372 sidecar 序列化/写失败）会把「接管失败: …」压进 `lastSyncWarnings_`（`claude_service.cpp:1687`），此时 sidecar 不存在 ⇒ managed=false ⇒ 设置页既不显示失败也不为其留高度（claude 侧仍显示摘要）。首次接管失败即命中。
**最小修法建议**：两处 `managed && svc` → `svc`（与 claude 侧对齐）。高度（warnLines，含 contentKey）与渲染行**必须同批改**；2 行、单文件。

### 4. 共享片段键若恰是托管键 → 被槽位静默覆盖
**结论**：CONFIRMED（覆盖静默发生，全链无警告）。
**证据**：`src/module_claude_proxy/provider_config.cpp:buildProxyEnv` 合并顺序 = extra_env(362) → 共享片段(371-376，片段赢 profile) → BASE_URL/占位 token(378-381) → **槽位键最后写**(390-398 `out[envKey]`)；函数返回裸 `std::map`，**无 warnings 通道**；`settings_sync.cpp:detectOverlayConflicts`(244-266) 是唯一警告通道，只扫 overlay 文件且用硬编码 8 键清单(26-38)——不含片段键，也不含 `*_MODEL_NAME`（= 已登记 CPX-12）。
**影响与可达性**：当前**无 UI 触点**（`grep sharedConfig src/` 只命中 claude_service/provider_config/settings_sync，片段编辑器尚未落地），故只经 DB/导入路径发生；一旦片段含 `ANTHROPIC_MODEL` 而 profile 槽位非空，UI 显示"片段生效"但 live 里是槽位值 ⇒ 典型"改了不生效"体验。
**最小修法建议**：**不拦**（优先级 1 > 2 是设计明文），在**片段编辑器 UI 批**加警告：新增独立纯函数 `sharedFragmentShadowedKeys(sharedExtraEnvJson, modelsConfigJson)`（不动既有签名）返回被槽位覆盖的键集，片段保存处显示一行警告（措辞对齐 G10「被片段冻结」）；可选把该集合并入 takeOver 的 warnings 作无 UI 兜底。

### 5/6. ccache "3 个 miss 未定位"（"疑 try_compile"）
**结论**：REFUTED——本仓库 configure 的探测编译**根本不走 ccache**；实测 miss 全是普通 TU（内容/命令与缓存键不符），不是配置期探测。
**证据**（全新 worktree、`dc727cd`、EUI-NEO `269a8c4`、`-j3`、CMake 自动探测 ccache，一次 configure+build）：
- `CCACHE_LOGFILE` 全量（按 pid 切块）：`217 次调用 / 161 direct hit / 56 miss`；miss 分类 = tests 38 + src 17 + EUI-NEO 1
- `grep -c TryCompile|CMakeScratch|CMakeTmp|CompilerId build/ccache-full.log` → `0 / 0 / 0 / 0`
- 全局计数不可用于单树归因：我的 241 次调用窗口内 `ccache -s` cacheable +332（差 91 次属并发会话，全 miss）
**影响与可达性**：miss 只影响构建时长，无功能风险。但"99.2% 命中"是"同内容二次构建"的一次性观测（仓库文档已把它写成常态）——跨批次首建应按"内容/rev 差异"解释，别当异常追。
**最小修法建议**：不改代码。若要常态化定位，把 `CCACHE_LOGFILE`（或 `ccache --show-log-stats`）接进 `scripts/ccache-env.sh` 并在汇总里按 pid 统计。

### 7. U1 采纳：探针 WARN 是否形同虚设
**结论**：非"不可达死代码"，但有两处结构性盲区，护栏价值有限（保留 + 另加更早自检是正解）。
**证据**：`src/module_todo/todo_ui_logic.h:dragScaleMismatch`（137-141 三条件：probe>0 且 probe≠1 且 probe≠框架值）；`todo_module.cpp:hitDropColumn`（569-579：喂 `::app::contentScale()`，探针只打一次 WARN，`dpiProbeWarned_`）；`test_todo_ui.cpp:780-843`（三档 probe==框架值→不告警 / 2.0 vs 1.25→告警 / probe==1 或 0→不告警）；`devdesk_tests.exe "todo_ui:"` → `Total 32 / Passed 32`。
**影响与可达性**：触发必须"真按下卡片开始拖拽"+ 探针自称非 1 尺度；盲区①`fabs(probe-1)>0.001` 把"probe==1 而框架≠1"整类过滤掉——这正是口径漂移的另一半方向（框架给逻辑值）;盲区②启动/换显示器/首次布局无信号。
**最小修法建议**：保留探针；另加**更早的自检**——U1 契约明写 `app::contentScale()` 与 compose 收到的 `Screen::scale` 同源同值，在 todo compose 首帧做一次性 `contentScale() == screen.scale` 的 WARN（任何缩放档都跑、不需拖拽）。若嫌探针冗余，可放开 probe==1 的过滤，但收益低于前者。

### 8. THS-13 逾期天数口径：`max(1, overdueDays)` 会不会改入选语义
**结论**：REFUTED 该顾虑——`max(1, …)` **只改显示**，不动入选、计数与排序。
**证据**：入选/计数全走秒级 `listOverdue`（`todo_module.cpp:refreshOverdueCount`253-258、`openOverdueDialog`260-267；`todo_service.cpp:listOverdue`778）+ home 的 `due_at < now` SQL（`home_service.cpp:getStats`191-213）；`ui::overdueDays`(`todo_ui_logic.h:215`) 唯一消费者是弹窗行文案 `todo_module.cpp:1973-1981`「逾期N天」；home 侧只有计数卡、无天数文案（`home_module.cpp:354-357`）。
**影响与可达性**：修后消除"入口写逾期 1、点进去 0 天"（当天到期且已过点 → 入选但日历日差 0）。副作用：几分钟前过期会显示"逾期1天"（夸大），更贴切文案是"不足1天/今天到期"——属文案拍板项。
**最小修法建议**：显示侧 1 行（1977 处 `std::max(1, ui::overdueDays(...))`）。**不建议**同时改入选口径（会漏掉"今天到期但已过点"的任务，且要动 SQL + home，跨模块）。

---

## 需要人工/拍板的残留
1. 第 1 条的修法方向（锚语义改块身份 vs 维持偏移保持）——设计级，建议随 M3。
2. 第 3 条是否现在改（2 行、单文件、无依赖）。
3. 第 8 条的显示文案（"逾期1天" vs "不足1天/今天到期"）。
