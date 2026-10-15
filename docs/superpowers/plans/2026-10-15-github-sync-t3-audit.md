# T3 独立审计 — GitHub Sync UI 层（commit 38115d0）

- 审计对象：`worktree-agent-github-sync-ui` 分支 `38115d0`（17 文件 +2127/−42），worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-github-sync-ui`
- 依据：spec §2/§2.1（决策 #10–#20 + F1–F9）｜plan T3 票｜逐文件 diff 实读
- 测试复跑：**未跑**（worktree 无 build 产物；且 38115d0 提交树缺 T2 内容，需先补齐才可编译——见 F10）
- 审计日期：2026-10-15（只读审计，除本文件外零写动作）

## Findings

| # | 位置 | 严重度 | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| F1 | settings_module.cpp:607-613, 1082 | **高** | 自动同步失败无退避：失败不清 dirty、`githubSyncLastCommitSec_` 只在成功时更新；线程完成即 `requestUiUpdate` 唤醒下一帧 → `maybeAutoGithubSync` 立即重启任务，自持循环 | 断网/token 失效时每轮全库 dump + 多次 API 调用的重试风暴，GitHub 限流/封 token 风险 | 失败也记 throttle 时点（一行级修复），或连败退避 + 上限 |
| F2 | settings_module.cpp:1093-1104; git_error_dialog.cpp（hint 行） | **中** | 违反 spec F6：409 二次确认只给通用文案，**未列出将被覆盖的本地未推文件清单**；PullThenPush 只备库、**笔记无 backups/notes-\<ts\>/ 备份** | 覆盖动作可能丢本地未推笔记且用户事前无清单可查 | 确认武装前列清单；拉取前整目录备份笔记（pruneBackups 需扩目录型） |
| F3 | settings_module.cpp:108 | **中** | 违反 spec F4 落位修订：`kGithubSyncSkipTables` 本地另抄清单，`core::sql_dump::defaultSkipTables()`（sql_dump.h:18）已存在且明令"不得另抄" | 两处清单漂移风险 | 改引 defaultSkipTables()，删本地常量 |
| F4 | github_sync_logic.cpp:71-87; settings_module.cpp:876-899 | **中** | 偏离 spec F1：diffNotes 每轮 PUT **全部**本地笔记（无内容/sha 比对，注释自认"由调用方判定"但调用方也没判）；database.sql 的 sha 不落状态键 | 每次自动同步为每篇笔记各造一个远端 commit——历史噪音 + API 放大（叠加 F1 更烈） | PUT 前按登记 sha/内容哈希比对，仅变更才推；记入 T4 票面 |
| F5 | settings_module.cpp:976-981 | **中（T4 范围）** | spec F3 缺口：导入直写活库，无暂停后台任务、无导入后数据重载广播、失败不从备份回滚 | 其他模块缓存与真库脱节 | T4 编排补齐（plan T4 已列 F3，勿漏） |
| F6 | settings_module.cpp:893 | 低 | `r.isOk() ? r.value() : "put"` 死分支——`r.isFail()` 已提前 return | 误导性代码 | 删死分支 |
| F7 | settings_module.cpp:156; local_git_probe_macos.cpp:18 | 低 | 探测在 onStart（UI 线程）同步 `popen` 且无超时；结果一次探测后永不刷新（spec F8 要求配置变更时失效） | xcode-select 卡死则启动卡死（概率极低）；会话内装 git 不更新灰字 | 可接受；后续异步化 + 缓存失效 |
| F8 | settings_module.cpp:914 | 低 | 删除传播保守跳过只有 LOG_WARN，无 UI/状态行提示（"静默失败面"仅对日志读者可见）；登记写失败（同函数 WARN）→ 该远端文件永久脱离删除基线 | 保守方向，不丢数据；用户不知晓 | 卡片可加一次性提示；登记失败应计入 summary |
| F9 | settings_module.cpp:1273/1278 | 低 | 换仓/换分支清 first_sync_done 后，FirstSync 不会自动重跑（只在拨启用开关时触发） | 换仓用户需重拨开关才有方向判定 | 可后续在手动同步入口补判定 |
| F10 | worktree 工作区 / 提交树 | **中（合并卫生）** | ①38115d0 提交树**不可独立编译**：settings_module.cpp:856 用 `ErrorCode::Conflict`、git_error_dialog.h include `platform/github_api.h`，两者属 T2（b13b212，分支 `worktree-agent-ghapi`），提交信息已披露；②worktree 工作区残留未提交改动：result.h(+2)、git_sync_service.h/sql_dump.cpp/agent_loop.cpp 各 +1，及 untracked github_api*.h | 合并序错 = CI 编译红；残片误提交 = 无关改动混入 dev | 合并序强制 T2→T3；合并用显式 pathspec；残片由 T2 侧管理 |

## 重点核查回应（实现方自述 vs 实证）

1. **删除传播保守版**：跳过删远端有 LOG_WARN（:914），无 UI 提示（F8，低）；基线登记在 PUT 成功后（:893-897），登记写失败仅 WARN → 漏删方向保守（远端多文件，不丢数据）。崩溃窗口（PUT 成功与登记之间）同方向。
2. **节流**：`kCommitThrottleSec=300`（github_sync_logic.h:27）；起算点 = **上次 commit 成功**（settings_module.cpp:1082）。失败不进窗口 → 重试不被卡死，但因此产生 F1 重试风暴。
3. **首次方向**：探测失败 → `Fail` → summary 带远端错误信息，manual=true → 弹窗可见（:1103），无静默 ✓；换仓/换分支清标记 ✓（:1273/1278）；但不自动重跑（F9，低）。
4. **local_git_probe**：头文件平台无关、`_macos.cpp`/`_win.cpp` 拆分、CMake `if(WIN32)` 注册——符合平台隔离铁律 ✓；macOS 只查 `xcode-select -p` + 文件存在性、不执行 git shim ✓；popen 无超时（F7，低）。
5. **409 两步确认**：两下武装 + 按钮文案/提示行随武装切换、误触防护达标 ✓；但确认框**无覆盖文件清单**、笔记无备份（F2，spec F6 明确要求）。
6. **行数/功能比**：生产约 1564 行（扣 563 行测试），无大段复制；但 T3 票实际吞入 **T4 编排主体**（push/pull/firstsync/backup/节流接线全在 settings_module +719）——plan T4 的 sha 状态机接线/F3/F5/F6 成了"在已写好的编排上补洞"，范围漂移需控制会话重划 T4 票面。
7. **与 T1/T2 边界**：提交内**无 result.h、无 platform 文件**改动 ✓（17 文件全 module_settings/tests）；依赖 `ErrorCode::Conflict` 与 github_api.h 属 T2，合并序 T2→T3（F10）。
8. **测试有效性**：15 例逐一读过——均有对照组/双向断言，**无恒真**；`github_sync_logic_throttle` 测的是 throttleDue 行为（0 值/窗口内/边界 300s/时钟回拨），非仅常量存在 ✓；frame_env `frame()` 返回 bool 透传断言，防假绿 ✓。缺口：编排层（runGithubSyncPush/Pull/FirstSync、maybeAutoGithubSync、harvest、备份）**零测试覆盖**。

## 六维度

| 维度 | 结论 |
|---|---|
| spec 符合性 | 6/10 — 决策 #10/#12/#13/#15/#16/#17/#18/#19/#20 均落地；F4 违反（另抄清单）、F6 缺清单+笔记备份、F1 偏差（全量重推）；F3 明示留 T4 |
| 正确性 | 6/10 — 主路径逻辑成立；F1 重试风暴是真 bug（高）；备份 wal_checkpoint 后拷贝存在微小并发窗口（低） |
| 测试有效性 | 7/10 — 15 例无恒真、对照组齐、frame 透传是实改进；编排层零覆盖；本审计未复跑（无构建产物） |
| 架构/平台隔离 | 9/10 — probe 平台拆文件合规、store 锁纪律（recursive_mutex 随句柄）合规、模块边界干净、弹窗零依赖可无头测 |
| 范围控制 | 6/10 — 无超需求 UI、无大段复制；但 T3 票吞入 T4 编排主体，T4 票面需重划 |
| 失败可见性 | 8/10 — 失败弹窗/红字/详情/复制齐备，manual 才弹窗的区分合理；删远端跳过仅日志（F8） |

## 总裁决：**带病合并**（附条件）

合并前必须：① 修 F1（失败也记 throttle 时点，一行级）；② 合并序 T2→T3、显式 pathspec、不带工作区残片（F10）。
随 T4 票面补：F2（F6 清单+笔记备份）、F3（F4 改引 defaultSkipTables）、F4（F1 内容比对）、F5（F3 暂停/重载/回滚）。
