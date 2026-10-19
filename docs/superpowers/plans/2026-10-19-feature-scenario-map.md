# DevDesk 功能 × 业务场景全地图（2026-10-19）

> 性质：只读盘点产物，唯一写动作即本文件。基线：dev `aafd326c`。
> 方法：从代码到产品——`src/app_registry.h` 7 页面注册表 + Router/托盘/全局快捷键/通知链 + 各模块 compose 与 Service 公开方法逐个深读；每个用户可触达的按钮/输入/菜单项入图。
> 模块开关口径：可插拔的只有 **markdown / todo / claude_proxy / agent** 四者（`kModuleRegistry` 的 gatingKey；home/settings/about 恒注册）。**agent 默认关闭**（`settings_config.h:40`），其余默认启用。下文各场景若依赖被关模块，标「模块关=不可达」。
> pet 模块：已于 2026-10-13 整模块摘除，不在图内。
>
> **场景定义**：每条 =「用户从什么状态来 → 做什么操作（具体 ui id / 菜单名 / 快捷键）→ 期待什么结果」。
> **正反用例**：正向 = 正常操作→期待结果；反向 = 空输入 / 非法值 / 极值 / 中断 / 环境缺失 / 并发 → 期待**正确拒绝或降级，不许崩、不许静默失败**。反向现状标注：〔有〕= 代码有处理（附位置）；〔未知〕= 代码现状未核/依赖上游，须 probe 实测。
> **覆盖口径**（对照 tests/ 与 tests/probes/ 实际文件）：
> - `[probe]` = 真窗口探针已跑（`tests/probes/`：home_probes 组 A–M / settings_dropdown_z / module_toggle_sidebar / agent_dialog / agent_e2e / screenshot；`ctest -L probe`）
> - `[live]` = 真网/真机（`tests/live/agent_live`（LLM 腿，`scripts/run-agent-live.sh`）/ `github_sync_live`（`run-ghsync-live.sh`）/ `tools/quota_live_probe`）
> - `[单测]` = 无头单测 / compose harness（业务级**未**实际跑，只证明逻辑/几何）
> - `[未覆盖]` = 无任何测试命中
> - 2026-10-03 probe 轻量化（commit `9d032c2`）删掉的 8 个 probe 面（about/agent 列表/claude calls/claude 表格/markdown 树/settings 卡/todo 看板/pet）——按 **2026-10-19「所有业务场景必须实际跑起来」新标准**，这些面现为 `[单测]` 的场景一律属于「业务级未跑、须业务流程 probe 回补」，本文在总览单列一列计数，并沿用并入 `2026-10-19-ui-test-gap-map.md` 的缺口 G1–G5。

## 0. 总览

| 模块 | 功能面 | 场景数 | 未覆盖（无任何测试） | 业务级未跑（无 probe/live） |
|---|---|---|---|---|
| 全局/装配（非 Router 页） | 启动链/单实例/DB 损坏还原/托盘/侧边栏/文件打开/退出链/快捷键 | 18 | 4 | 17 |
| Home 首页 | 统计五卡/快捷按钮/用量图/余量卡/滚动 | 14 | 7 | 10 |
| Markdown 笔记 | 目录树/四态编辑/搜索/标签/弹窗/外部修改/git | 30 | 4 | 30 |
| Todo 看板 | 列/卡片/拖拽/右键菜单/弹窗/逾期/归档 | 24 | 1 | 24 |
| Claude 切换器 | profile CRUD/代理启停/接管/表单/共享片段/调用日志/余量/直连 | 27 | 2 | 25 |
| Agent 智能体 | 任务 CRUD/触发四型/草稿/运行/详情/通知/工具 | 24 | 1 | 16 |
| Settings 设置 | 外观/模块开关/代理/日志/编辑器/GitHub Sync | 27 | 2 | 19 |
| About 关于 | 静态展示/光晕动画 | 2 | 0 | 2 |
| **合计** | — | **166** | **21** | **143** |

正反用例设计总数 = 166 × 2 = **332**（正向 166 + 反向 166）。

三条最危险的未覆盖/未跑场景（按用户主链危害排序）：
1. **Todo 真指针拖拽跨列/列内重排**（T-15/16）：OS 级真指针路径自 2026-10-03 刻意不测，现仅纯几何函数 + 帧驱动模拟。命中换算回归时任务落错列 = **数据写错位置**（用户主链「整理看板」每日必用）。
2. **GitHub Sync 拉取覆盖本地库全链**（S-22/24/27）：pull = DROP 重建整库 + markdown/todo 缓存重载 + 「未保存编辑不丢」守卫。引擎逻辑有单测、真网有 live，但「拉取→三模块缓存重载→编辑中状态不丢」的**整链**从未实际跑；失败形态是**整库数据丢失**。
3. **Claude 代理启停的 settings.json 接管/还原**（C-02/03/25）：用户 Claude Code 全局配置被改写（`PROXY_MANAGED`），「启→进程崩溃→重启补还原」链只有 service 单测，真机从未跑；还原失败 = 用户配置永久指向死端口，Claude Code 全局不可用。

---

## 1. 全局/装配层（app 层，非 Router 页）

代码源：`src/main.cpp`、`src/app_assembly.cpp`、`src/app_sidebar.cpp`、`src/core/tray_menu_spec.h`、`src/core/router.*`。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| GA-01 | 冷启动首次运行 | 无数据根 → 双击启动 → `dslAppConfig` 静态初始化（建目录→开库→migrate→启动备份→装配→splash 2.3s→主界面） | 数据根建出、`devdesk.db` 就绪、侧边栏 6 项（agent 默认关）、日志 INFO「数据库已就绪」 | 磁盘只读/建库失败 → LOG_FATAL + abort（设计内硬停，非静默） | 〔有〕`main.cpp:123-207` | [单测]（test_app_config / test_db_backup / test_app_assembly）；真启动→splash→主界面链业务级未跑 |
| GA-02 | 二次启动单实例互斥 | 已有实例运行 → 再启动一次 | 后启进程 `acquireInstanceLock` 失败 → `notifyExistingInstance` → 前实例窗口唤起、后进程 exit(0)、零副作用 | 锁文件损坏/被外部进程持有 → 行为未核 | 〔未知〕`platform/single_instance_*` 无测试 | [未覆盖] |
| GA-03 | DB 损坏 → 备份还原 | 库文件被写坏 → 启动 | msgbox「用最近一次备份还原并继续启动？」选 Ok → 坏库改名隔离 + 还原 + 二开成功启动 | 用户选取消 → `_Exit(1)` 干净退出 | 〔有〕`main.cpp:70-120`（exclude 防对同一备份重复弹） | [单测]（test_db_backup；弹窗 UI 流未跑） |
| GA-04 | DB 损坏 → 无备份 | 库坏且备份目录空 → 启动 | msgbox「没有可用备份可还原，应用即将退出」后退出，不半启 | 备份目录扫描失败（权限）→ 单独文案，不冒充「无备份」 | 〔有〕`main.cpp:74-96` | [单测]（test_db_backup） |
| GA-05 | 还原后仍损坏 | 还原的二开 `quick_check` 仍失败 | msgbox「还原后的数据库仍无法通过完整性检查」后 `_Exit(1)`；循环上限 2 轮 | 还原中途断电 → 下一轮走 exclude 递补更早一份 | 〔有〕`main.cpp:129-176` | [单测] |
| GA-06 | 启动自动备份 | 正常启动（模块 onStart 完成后） | `performStartupBackupOnce` 成功，LOG「启动备份完成」；进程闩防重复 | 磁盘满/权限 → LOG_ERROR 不阻断启动 | 〔有〕`main.cpp:646-656` | [单测]（test_db_backup） |
| GA-07 | 托盘左键唤起 | 窗口最小化/隐藏 → 点托盘左键 | `onTrayMenu(leftButton=true)` → `ShowMainWindow` → 主窗口置前 | 托盘图标创建失败（系统托盘异常）→ 行为未核 | 〔未知〕平台层 | [未覆盖]（test_tray_icon 仅图标装载） |
| GA-08 | 托盘菜单「显示主窗口」 | 右键托盘 → styled 菜单（`trayMenuSpec()` 2 项）→ 点「显示主窗口」 | 窗口唤起置前 | styled 菜单构建失败 → 原生降级菜单同动作；菜单项点击越界 → 忽略 | 〔有〕`main.cpp:447-481`；降级数据同源 spec | [单测]（test_tray_dynamic 菜单结构；真派发未跑） |
| GA-09 | 托盘菜单「退出 DevDesk」 | 右键托盘 → 点「退出 DevDesk」 | `requestExit` → atexit：ghsync 退出补推 → stopAll（md 落盘）→ claude 还原 settings.json | atexit 阶段 Router 已析构 → exitHook 判 null 跳过 | 〔有〕`app_assembly.cpp:99-110`；序敏感注释 | [未覆盖]（退出整链无端到端测试） |
| GA-10 | Ctrl+B 折叠/展开侧边栏 | 任意页面（非编辑器聚焦）→ Ctrl+B | 侧边栏 208→72/72→208 即切，持久化 `sidebarCollapsed` | 编辑器聚焦时 Ctrl+B 被编辑器消费（加粗），不触全局——须核真派发栈 | 〔有〕`main.cpp:614-626`（依赖上游焦点派发） | [单测]（test_app_sidebar:342/:416 状态+持久化）；真键派发未跑 |
| GA-11 | 侧边栏折叠开关点击 | 点 `app.sidebar.toggle` | 同 GA-10（含持久化 + 失败仅 LOG_WARN） | DB 未就绪 → 不持久化但本会话生效 | 〔有〕`app_sidebar.cpp:32-47` | [单测]；module_toggle_sidebar_probe 覆盖同区域模块开关真点击 |
| GA-12 | 侧边栏导航 | 点 `app.sidebar.item.<PageId>.hit` | `navigateTo(page)`；当前页高亮；模块关的项不渲染 | 点被注销页（不可能，项已消失）；Router null → LOG 后静默 | 〔有〕`app_sidebar.cpp:149-202` | [单测]（test_router）；真点击未跑 |
| GA-13 | 打开 .md 文件 | Finder 双击 / `argv` 传入 `xx.md` | `captureFileIntoNotesDir` 拷入笔记目录 → `navigateTo(Markdown, "open:<rel>")` → 打开该笔记 | 文件无法纳入目录（权限/重名）→ LOG_WARN 仅切页 | 〔有〕`app_assembly.cpp:153-171` | [单测]（test_md_file_open）；真 OS 事件未跑 |
| GA-14 | 打开 .json 文件 | argv 传入 `xx.json` | `dispatchPageForFile` → 切 ClaudeProxy 页（无负载） | markdown 模块关闭且目标是 md → 仍切页但 payload 无人消费（已知降级） | 〔未知〕切页后无提示 | [单测] |
| GA-15 | 打开 .txt/.js/.html 文本 | argv 传文本文件 | 按 Markdown 模块查看（拷入+打开） | 同 GA-13 | 〔有〕 | [单测] |
| GA-16 | 打开未知后缀文件 | argv 传 `xx.zip` | 仅切到 Home 页 | — | 〔有〕`dispatchPageForFile` 默认 Home | [单测] |
| GA-17 | 退出链收尾 | 正常退出（托盘「退出」/系统关闭） | atexit 序：ghsync 退出补推（≤5s）→ `stopAll()`（md 3 次重试落盘、todo 存视图、agent join run 线程）→ claude `syncRestore` 还原 settings.json | md 落盘 3 次仍失败 → LOG_ERROR「未落盘的编辑已丢失」（上游无退出拦截 API） | 〔有〕`markdown_module.cpp:268-280`；ghsync detach 窗口风险已注 `settings_module.cpp:698` | [单测]（各 onStop 分别有）；退出**整链**未跑 |
| GA-18 | 启动恢复用户偏好 | 之前设过主题/字号/效果/折叠 → 再启动 | 主题/字号/窗口效果/侧边栏折叠/窗口尺寸全部恢复（最小窗 1330×845 逻辑，经工作区换算） | 工作区放不下 → 启动即最大化；配置坏值 → sanitized 钳制 | 〔有〕`main.cpp:549-576` + `settings_config.h` sanitized | [单测]（test_settings / test_window_layout）；真机恢复未跑 |

**通知链**（横切）：`platform::notify::initialize()` 在装配点预热（macOS 装 DidBecomeActive 观察者 + 异步授权查询），消费方 = Agent 模块（GA 层只预热、无独立用户场景，计入 AG-17）。

---

## 2. Home 首页（`src/module_home`）

入口：侧边栏「首页」；快捷卡点击、快捷按钮跳转、余量卡；模块恒开。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| H-01 | 查看仪表盘 | 启动后进首页 / 从别页切回 | 五统计卡（`home.stat.tokens/notes/todo/overdue/api`）真实数字 + 日期问候 + sparkline | DB 空 → 数字 0；getStats 失败 → 数字 `--` + badge「加载失败」 | 〔有〕`home_module.cpp:399-425`（失败不冒充空态） | [probe]（home_probes 组 A/G/M：统计网格逐字、spark 几何、按钮等宽）+ [单测] |
| H-02 | 点统计卡跳转 | 点 `home.stat.<kind>.press` | 跳对应页（tokens/api→ClaudeProxy；notes→Markdown；todo/overdue→Todo） | 目标模块已关 → 卡已隐藏不可点（`moduleRegistered` 过滤）；Router null → LOG_WARN | 〔有〕`home_module.cpp:159-163` | [未覆盖]（点击跳转链无断言） |
| H-03 | 手动刷新 | 点 `home.btn.refresh` | `refreshStats()` 数字即时更新，无 toast | service 失败 → 卡变「加载失败」不半更新 | 〔有〕 | [未覆盖] |
| H-04 | 跳设置页 | 点 `home.btn.settings` | 切 Settings 页 | — | 〔有〕navigateTo | [未覆盖] |
| H-05 | Token 速率卡三态 | 有成功调用 / 无调用 / 查询失败 / 流式中 | 分别显示速率数字 / `--`「暂无成功调用」/「加载失败」/ 流式主色 +「● N 路生成中」+ 1s timer 流末重查 | lastRate 查询失败独立于整页失败（不拖垮整卡也不静默） | 〔有〕`home_module.cpp:275-293/:409-425` | [单测]（test_home:190/:265/:302） |
| H-06 | 快捷新建笔记 | 点 `home.act.newnote` | 跳 Markdown 页并带 payload `action:new_note` → 弹「新建笔记」弹窗 | markdown 模块关 → 按钮隐藏（模块关=不可达） | 〔有〕表驱动过滤 | [未覆盖]（gap G4 相关面；payload 消费端 M-25 有单测，跳转链无） |
| H-07 | 快捷新建任务 | 点 `home.act.newtodo` | 跳 Todo 页带 `action:new_task` → 新建任务弹窗；Todo 无列 → toast「请先创建至少一列」 | todo 模块关 → 隐藏 | 〔有〕`todo_module.cpp:537-553` | [未覆盖]（跳转链） |
| H-08 | 切换 Claude Profile | 点 `home.act.claude` | 跳 ClaudeProxy 页（无 payload） | claude 模块关 → 隐藏 | 〔有〕 | [未覆盖] |
| H-09 | 今日 Token 用量图 | 有 API 调用的当天 → `home.api.chart` | 自绘柱状图：去头去尾、峰值归一、柱顶数值、x 轴「N时」 | 全天无数据 → 空态「今日暂无 API 调用」不画假柱；claude 关 → 整块隐藏 | 〔有〕`home_module.cpp:587-756` | [probe]（组 A wrap 几何）+ [单测]（hourlyDataRange 纯函数） |
| H-10 | 供应商余量卡 | quotaEnabled 开 + claude 模块开 → 余量卡 | 各 profile 余量行 + [刷新]→`requestQuotaImmediateRefresh`（后台真查询）+ 点行 → `action:quota_config:<id>` 去配置 | quotaEnabled 关 → 整卡隐藏；读库失败 → 卡上显形错误文案（非「暂无」） | 〔有〕`home_module.cpp:558-585/:818-845` | [probe]（组 G quota 真数据落图）+ [单测]（test_home_quota_card 五态） |
| H-11 | 余量卡版本缓存 | 后台 QuotaRefresher 写库 → 回首页 | writeVersion 变 → 一次查询更新快照；未变帧零查询零锁 | onStop 后快照作废（防旧 seenVersion 误命中） | 〔有〕`home_module.cpp:818-845` | [单测]（M4 修复组） |
| H-12 | 模块开关联动显隐 | 设置里关模块 → 回首页 | 归属卡/按钮整行隐藏、剩余宽度均分；三模块全关 → 空态「所有工具模块已关闭」 | — | 〔有〕表驱动统计 | [单测]（test_module_gating:214-281 统计卡）；**actions 行联动 = gap G4 未覆盖** |
| H-13 | 矮窗滚动 | 窗口高度不足 → 首页 | 整页挂 `home.scroll`：图表可压、余量卡照画、超出滚动条吸收；任何屏高 `drawsQuotaCard` 为真 | — | 〔有〕`home_quota_card.cpp:246` 滚动分支 | [probe]（组 K 三档 845/805/700 + min_window 变体） |
| H-14 | 进页重查 | 在 Todo 增任务 → 切回首页 | `onPageEnter` → `refreshStats()` 数字已变 | service 未就绪 → onStart 已置错误文案 | 〔有〕`home_module.cpp:252` | [未覆盖]（onPageEnter 重查无断言） |

---

## 3. Markdown 笔记本（`src/module_markdown`，模块可关）

快捷键：Ctrl+Shift+P 切视图 / Ctrl+N 新建 / Ctrl+S 保存 / Esc 关浮层 / 编辑器内 Tab、Enter、Ctrl+B/I/K、（实时态）Ctrl+Z/Y、方向键跨块。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| M-01 | 打开笔记 | 点树行 `md.tree.node.<i>.row` / `md.tree.item.<i>` | `saveEditor()` 后读文件 → 编辑器载入、状态栏「✓ 已保存」、标签栏载入、mtime 基准更新 | 文件读失败 → toast「读取失败」不动现文；切换前保存失败且有未存改动 → 阻止切换 + toast（保数据优先） | 〔有〕`markdown_module.cpp:2842-2883` | [单测]（test_markdown:541） |
| M-02 | 编辑 + 自动保存 | 在 `md.editor.input` 打字 → 停 400ms | `flushEditorIfIdle` 落盘 + 索引 + FTS + git 自动提交触发；状态栏 ● 未保存 → ✓ 已保存 HH:MM | 保存失败 → ✕「保存失败，自动重试中」+ 指数退避 500ms→60s，重试期只弹一次 toast | 〔有〕`:2933-2987`；**状态栏失败态零断言 = gap G2** | [单测]（test_markdown / test_md_note_git）；G2 [未覆盖] |
| M-03 | Ctrl+S 手动保存 | 有改动 → Ctrl+S | 立即落盘；冲突 toast 开着 → 保存成功即按「保留本地」收掉冲突 | 冲突未决时自动链路不写（防用旧缓冲盖外部新内容） | 〔有〕`:2976-2981` | [单测]（md_module_conflict_ctrl_s_resolves_keep_local） |
| M-04 | 编辑器键盘流 | Tab/Shift+Tab 缩进；Enter 列表续行；Ctrl+B `**`、Ctrl+I `*`、Ctrl+K 链接骨架 | 纯函数改 buffer → commit 回 InputState（进 undo、打脏、滚动跟光标） | IME 组字中不拦截（防撕裂 preedit）；无变化只消费不动 undo 栈 | 〔有〕`:1044-1106` | [单测]（test_md_keyboard:291/:386/:461 真编辑器 wiring） |
| M-05 | 右键插入表格 | 编辑器右键 → `md.editor.ctx` 级联（1-6 行 × 1-8 列）→ 选规格 | 光标处插表格模板，可 undo、可回撤聚焦 | 预览态无编辑器 → 不弹（`find(kEditorInputHitId)==null` 守卫） | 〔有〕`:3414-3423` | [单测] |
| M-06 | 四态视图切换 | `md.mode` segmented（编辑/实时/双栏/预览）或 Ctrl+Shift+P 循环 | 布局正确切换、视图状态持久化；进预览对齐编辑器位置；进实时态状态群复位 | — | 〔有〕`setMode`/`enterLiveState` | [单测]（test_markdown:541；markdown_probes 已删=业务级未跑） |
| M-07 | 实时态编辑 | Live 态打字（`md.live.input` 单输入随活动块搬移） | 活动块源码态、其余渲染态；光标越块自动切块；高度贴合无内部滚动（0.7 视口封顶除外） | 组字在飞拒绝切块 + 补发聚焦；失焦提交未上行文本（修订号对齐判据防旧文本灌回） | 〔有〕`:2044-2059`；贴合失败有 over 探针 WARN | [单测]（test_md_live_edit / test_md_live_spike:428/:487 / test_md_block_*） |
| M-08 | 实时态块边界键 | Live 态 ↑/↓ 跨块、块首 Backspace / 块尾 Delete | 跨块=光标迁移到邻块同列；边界删除=删块间接缝换行 | 首块↑/末块↓ 返内置；有选区交回内置 | 〔有〕`:2121-2153` | [单测]（md_live_edit 纯函数） |
| M-09 | 实时态撤销/重做 | Live 态 Ctrl+Z / Ctrl+Y（/Ctrl+Shift+Z） | 模块级快照栈整篇恢复 + 光标落点；跨块/换笔记基线换代 | >2MB 文档撤销栈降级 LOG_WARN；无快照静默返回（键仍消费） | 〔有〕`:2189-2258` | [单测]（test_md_live_edit） |
| M-10 | 双栏同步滚动 | Split 态滚动编辑器 | 预览按几何表反向映射跟随（`computeSyncOffset`）；编辑只重测脏块 | 空文档 → offset 0 | 〔有〕`:2262-2290` | [单测]（test_md_sync_map） |
| M-11 | 预览点击定位 | Split 态点 `md.preview.blk.<id>.hit` | 反查行首字节 → 编辑器光标落该行 + 聚焦 | 行内列不映射（布局不同源，行首为唯一无歧义落点——设计取舍非缺陷） | 〔有〕`:2294-2313` | [单测]（md_block_interact） |
| M-12 | 预览态阅读 | Preview 态滚轮 | 手动滚动（previewOffset 回写）；760px 居中列 | 进态对齐编辑器位置一次 | 〔有〕`:1360-1399` | [单测] |
| M-13 | 大纲面板 | `md.outline.toggle` 开 → 点 `md.outline.item.<i>.row` | 跳标题行（编辑/双栏写光标；预览滚预览；实时走 setLiveCursor）；当前节高亮 | 无标题/未开笔记 → 空态文案 | 〔有〕`:2613-2661` | [单测]（test_markdown:541；zIndex 断言 :1831） |
| M-14 | FTS 搜索 | `md.search` 输入 | 树展平为命中文件列表，逐键刷新；空输入恢复目录树 | 搜索无果 → 「没有匹配的笔记」；列表查询失败 → `md.tree.error` 红字 + 「每 5 秒自动重试」——**该错误态零断言 = gap G1** | 〔有〕`:596-637` | [单测]（test_markdown:851/:7416 空态与搜索）；G1 [未覆盖] |
| M-15 | 标签添加 | `md.tags.add` 回车 / `md.tags.addbtn` ＋ | trim 后落库、chip 出现、左栏筛选区计数 +1、发 `md.tags_changed`（同步 dirty 源） | 空白名静默忽略；重名不重复添加；落库失败 → 回滚 chip + toast | 〔有〕`:2791-2819` | [单测]（test_markdown_service / test_markdown_tools） |
| M-16 | 标签删除 / 筛选 | 点 `md.tags.chip.<i>.del` × / 点 `md.tags.filter.<i>` | 删除即时落库；筛选只显含该标签笔记；筛选标签失效自动取消 | 删除失败 → toast；筛选标签已无笔记 → 自动清筛选 | 〔有〕`:2746-2768` | [单测] |
| M-17 | 新建笔记 | `md.btn.new` / `md.tree.new` / Ctrl+N → `md.dialog.new.input` 输名 → 「创建」 | 落 `选中目录/名字.md`；打开新笔记并聚焦编辑器；发 `md_created` | 空名 → 「无标题笔记」自动编号（>999 拒绝）；显式重名 → 直接打开；`C:`/`..`/绝对路径 → 内嵌红字不关弹窗 | 〔有〕`:3155-3220` + `isSafeNewNotePath` | [单测]（test_markdown 新建组） |
| M-18 | 新建文件夹 | `md.tree.newfolder` → 选父目录下拉 + `md.dialog.newfolder.input` → 创建 | 空目录立即入树（磁盘实时枚举）；新目录成为选中目录 | 空名红字；父目录已被外部删 → 回落根目录 + WARN | 〔有〕`:3285-3327` | [单测] |
| M-19 | 树折叠/选中目录 | 点 `md.tree.node.<i>.toggle`（箭头）/ 点文件夹行 | 折叠记忆跨会话；选中目录决定新建落点；打开笔记不上溯选中 | 选中目录被外部删 → 新建回落根目录 + WARN | 〔有〕`selectDir`/`newItemParentDir` | [单测]（test_view_state roundtrip） |
| M-20 | 重命名/移动笔记 | 右键树行 → 「重命名」→ `md.dialog.rename.input` 填 `工作/新名` → 确认 | 磁盘 rename + 索引 UPDATE + FTS 重建，**id 不变**（标签保留）；当前笔记路径跟随 | 空名红字；非法路径红字弹窗不关；重名/失败红字保留弹窗 | 〔有〕`:3555-3604` | [单测]（test_markdown:2133 右键删链） |
| M-21 | 删除笔记 | 右键 → 「删除笔记」→ `md.dialog.del`「删除」 | 删文件 + 索引 + FTS + 发 `md_deleted`；删当前笔记 → 编辑器清空、dirty 清零（不复活） | 失败 → toast「删除失败」；文件已不在 → — | 〔有〕`:3522-3553` | [单测] |
| M-22 | 外部修改（无本地编辑） | 其他编辑器改当前笔记 → 5 秒轮询/watcher | 静默重载 + toast「检测到外部修改，已自动重载」；Live 态按内容身份重定位活动块 | mtime 读取失败（文件被删）走删除分支 | 〔有〕`:3015-3131`；MD-01 重载块映射修复组 | [单测]（md_module_md01_reload_* 六例 + capture_race） |
| M-23 | 外部修改冲突（有编辑） | 有未存改动 + 外部改文件 | 冲突 toast（不自动关）二选一：「重载磁盘版本」丢弃本地 / ✕ 保留本地（mtime 基准推进） | 冲突期间自动保存暂停（防旧缓冲盖新内容）；退出时未决 → 按保留本地落盘 + WARN 留痕 | 〔有〕`:2513-2530/:268-271` | [单测]（:6784/:6865）——真文件系统 + 真 5s 轮询链业务级未跑 |
| M-24 | 外部删除当前笔记 | 文件被外部删除 | 无编辑 → 清空编辑器 + toast「笔记已被删除」+ 视图状态同步清；有编辑 → toast「保存时重新创建」 | lastKnownMtime=-1 防重复 toast | 〔有〕`:3018-3035` | [单测]（部分：MD-01 reload_to_empty） |
| M-25 | 跨模块跳转负载 | Home `+新建笔记` / 文件打开分发 → payload `action:new_note`/`open:<path>` | 逐帧消费一次性打开弹窗/笔记；未识别 payload → LOG_WARN | payload 无效 → 不崩 | 〔有〕`consumeNavPayload :2665-2684` | [单测]（test_markdown 负载组） |
| M-26 | 库导入重载 | GitHub Sync 拉取完成（`db_imported`） | 列表重载；当前笔记**不**无条件重载（防假冲突盖掉未存编辑，走 mtime 判定） | 早退在 exchange 之前（DB 未就绪不消费丢标记） | 〔有〕`harvestDbImport :2692-2699` | [单测]（md_module_db_import_reloads_list:430） |
| M-27 | Esc 浮层栈 | 依次开 7 层浮层 → 连按 Esc | 逆 z 序逐层关闭（rename→delete→editorCtx→ctx→newFolder(连带下拉)→newNote） | 全关后 Esc 返 false（透传） | 〔有〕`closeTopmostPopup :355-368` | [单测]（test_markdown:3712） |
| M-28 | 视图状态恢复 | 重启应用 | 上次笔记/四态模式/折叠目录/选中目录恢复；笔记已删 → 跳过 + INFO | 坏值回默认；库未就绪 save 失败/ load 默认 | 〔有〕`loadViewState/saveViewState` + test_view_state sanitized | [单测]（test_view_state 四例） |
| M-29 | 退出模块落盘 | 关模块开关 / 退出应用（md onStart→onStop） | 未存改动 3 次重试落盘（200ms 间隔）；冲突未决按保留本地 + WARN | 3 次仍失败 → LOG_ERROR「未落盘的编辑已丢失」（上游无退出拦截——已知上限） | 〔有〕`:268-280` | [未覆盖]（onStop 落盘链无断言） |
| M-30 | 笔记 git 自动提交 | 保存成功 → `noteGit_->onFileSaved` | <5 分钟节流提交进笔记目录本地 git（历史兜底） | 无 git 环境 → 探测降级（设置卡灰字同源）；并发保存节流合并 | 〔有〕md_note_git | [单测]（test_md_note_git） |
| M-31 | 笔记目录只读展示 | 设置→模块 tab→目录卡 | 路径展示 + 「打开目录」（`platform::file_open::openDirectory`） | 目录不存在 → 「（尚未创建）」；打开失败 LOG_ERROR | 〔有〕settings_ui_cards:413-500 | [单测]（test_settings） |

（M-31 归属设置页卡片，列此为 md 数据面闭环。）

---

## 4. Todo 看板（`src/module_todo`，模块可关）

快捷键：Ctrl+↑/↓ 移动选中任务、Esc 关弹窗栈；右键任务/列菜单。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| T-01 | 加载看板 | 启动/进页 | 列 + 卡片按 position 渲染；选中任务/列滚动恢复 | 列或任务读失败 → `loadError_` 错误态 + 重试按钮（`todo.error.retry`），不拿旧数据伪装 | 〔有〕`todo_module.cpp:88-118/:164-180` | [单测]（test_todo_ui:202/:1819） |
| T-02 | 搜索 | `todo.search` 输入 | 命中集高亮/过滤（title/description）；Esc 清除 | 搜索失败 → toast「搜索失败」保留上次命中集 | 〔有〕`refreshSearch :120-132` | [单测] |
| T-03 | 逾期查看 | `todo.btn.overdue`（badge>0 时） | 逾期弹窗列表（`todo.dialog.overdue`）；badge 计数同步 | 查询失败 → 不开弹窗 + toast「逾期任务加载失败」（不当空结果开） | 〔有〕`openOverdueDialog :146-161` | [单测]（test_todo_ui:2364-2468） |
| T-04 | 顶栏新建任务 | `todo.btn.add` | 新建任务弹窗默认第一列 | 无列 → toast「请先创建至少一列」 | 〔有〕`boardActions.addTaskClicked` | [单测] |
| T-05 | 列内新建任务 | 列头「＋」(`addTaskToColumn`) | 新建任务弹窗锁定该列 | 同上 | 〔有〕 | [单测] |
| T-06 | 新建任务提交 | `todo.dialog.newtask.input` 回车/确认 | 建卡追加列尾 + toast + 发 `todo_created`；空标题 → 弹窗内红字「任务标题不能为空」弹窗不关 | service 失败 → 红字「新建任务失败：…」不关弹窗 | 〔有〕`doCreateTask :184-196` | [单测]（test_todo_ui + test_todo_service） |
| T-07 | 新建列 | `todo.btn.addcol` → `todo.dialog.newcol.input` | 新列追加末尾 + 发 `todo_column_changed` | 空标题红字；失败红字不关 | 〔有〕`:198-208` | [单测] |
| T-08 | 任务详情编辑 | 点卡片 → `todo.dialog.detail.input.title/desc`、优先级 `todo.dialog.detail.prio.<i>`、`due.pick` 日期、`tag.input` → 保存 | 全字段整行 update + toast「已保存」+ 发 `todo_updated` | 标题空？→ service 层校验；保存失败 → toast 不关弹窗 | 〔有〕`doUpdateTask`（返回 bool，失败中止动作保留弹窗） | [单测]（test_todo_ui:462/:542/:721/:1898） |
| T-09 | 完成/取消完成 | 详情 `todo.dialog.detail.toggle` / 右键菜单 | `completed_at` 置位/清空 + 迁到 kind='done'/'todo' 列尾 + 事件 | 语义列缺失 → toast 点名「任务留在原列」不静默；列迁移失败 → toast「已标记完成，但移入列失败」 | 〔有〕`finishCompletionMove :247-272` | [单测]（test_todo_service + test_todo_ui） |
| T-10 | 详情删除 | `todo.dialog.detail.delete` → 确认弹窗 | 删卡 + toast + `todo_deleted` | DB 失败 → toast | 〔有〕`doDeleteTask` | [单测] |
| T-11 | 详情未存改动守卫 | 详情有改动 → Esc | 第一次 Esc 只警告不关（守卫），第二次真关 | 关后改动丢弃（无草稿持久化——已知设计） | 〔有〕`requestClose` 守卫 | [单测]（test_todo_ui esc 组） |
| T-12 | 右键任务菜单 | 卡片右键 `cardMenu` | 菜单：编辑 / 上移 / 下移 / 移到第 i 列 / 删除（确认） | 无其他列时菜单自适应；运行数据变 → findTask null 直接返 | 〔有〕`onContextSelect :627-673` | [单测]（test_todo_ui:4151-4333 wiring） |
| T-13 | 右键列菜单 | 列头右键 | 重命名 / 左移 / 右移 / 改色（8 预设子菜单）/ 删除列（确认） | 重名「该列名已存在」红字；边界列移 → toast「已经在最左边/右边了」；删除列含全部任务在确认文案 | 〔有〕`doRenameColumn/doShiftColumn/doSetColumnColor` | [单测] |
| T-14 | 列头改名 | 点 ✎ | 重命名弹窗预填 | 同 T-13 | 〔有〕 | [单测] |
| T-15 | 拖拽跨列 | 卡片 `cardPressed` → `cardDragged`（超阈值）→ 目标列悬停占位条 → `cardReleased` 落列内 | `moveTask` 事务重排双侧 position + 占位条预示插位 + 跟手 ghost 副本 | 松手在板外 → 取消回原位；搜索态拖拽 → 同列取消、跨列追加列尾；拖拽中任务被外部删除 → DragState 复位 | 〔有〕`:461-501`；dpi 探针告警一次 | [单测]（纯几何 test_todo_ui:1374/:1479 + 帧驱动）——**OS 级真指针刻意不测（2026-10-03 裁决），业务级未跑** |
| T-16 | 列内拖拽重排 | 同列拖到目标行 | computeReorderRow 换算插位；插回原位 no-op（不写库） | 归档列拖拽 → 无效（按 archived_at 排序，不写 position）；卡片已不在该列 → 取消 | 〔有〕`finishDrag` 归档/ownIdx 守卫 | [单测]（同上，真指针未跑） |
| T-17 | 键盘移动 | 选中卡（最近点击）→ Ctrl+↑/↓ | 上/下移一位 + toast「已上移/下移」 | 未选中 → toast「先点击卡片选中任务」；边界 → 「已经在最上面/下面了」；归档列 → 禁移提示 | 〔有〕`moveTaskByKeyboard :504-522` | [单测]（test_todo_ui:302） |
| T-18 | Esc 弹窗栈 | 开多弹窗 → Esc | 栈顶先关（detail 有守卫） | 全关透传 | 〔有〕`closeTopmostPopup` | [单测]（:3712/:3843/:3953） |
| T-19 | 库导入重载 | GitHub Sync 拉取完成 | `dbImportedDirty` → compose 收割 `reloadAll`；拖拽锚点失效复位 | 导入线程只置原子不碰 UI（闸 + 退订序） | 〔有〕`:675-680` | [单测]（导入事件组） |
| T-20 | 归档列只读 | 点归档列卡片 | 详情弹窗只读态（不改 position、Ctrl+↑/↓ 拒绝并提示） | 退出归档列 → 恢复可编辑 | 〔有〕`openDetailDialog` readOnly + moveTaskByKeyboard 拒绝 | [单测]（test_todo_ui archive 命中） |
| T-21 | 视图状态持久化 | 滚列 + 选卡 → 退出重启 | 列滚动按列 id 恢复（列删自动 prune）+ 选中任务恢复（校验存在） | 坏值/库未就绪回默认；任务已删 → 不恢复 | 〔有〕`restoreViewState :385-397` | [单测]（test_view_state 四例） |
| T-22 | 智能体工具写库 | agent run 线程调 `todo.create/move/complete/...` | 工具经 TodoService 落库；off→on 期间 service 复用防 UAF | service 被关时工具已注销（onStop detach 先于 reset） | 〔有〕attach/detach 序 | [单测]（test_todo_tools）+ [live]（agent_live 场景含 todo 工具腿） |
| T-23 | 跨模块负载 | Home `+新建任务` → `action:new_task` | 弹新建任务弹窗（无列 → toast） | 未识别 payload → LOG_WARN | 〔有〕`consumeNavPayload :537-553` | [单测]（payload 到达路径） |
| T-24 | 空看板空态 | 无列无任务 | 「+ 列」引导空态 | — | 〔有〕board 渲染 | [单测]（test_todo_ui:1320） |

---

## 5. Claude 配置切换器（`src/module_claude_proxy`，模块可关）

快捷键：Ctrl+N 新建 / Ctrl+S 保存表单 / Ctrl+Enter 启停代理 / Esc 关弹窗栈。模块关 = ClaudeService 不装配、代理不监听、**智能体直连与余量也失效**。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| C-01 | 列表加载 | 进 Claude 页 | profile 列表 + 激活徽章 + 最近调用 + 接管状态行；快照缓存（脏标记重查） | 服务未装配 → 一次性 toast「本会话内开启的模块需重启…」+ 保持脏；查询失败显形非假空态 | 〔有〕`claude_module.cpp:112-130` | [单测]（test_claude_ui:1691 等五处） |
| C-02 | 启动代理 | 点 `claude.btn.proxy` / Ctrl+Enter（未运行时） | toast「正在校验」（后台 2s 探测激活 profile + 1-token ping）→ 过 → UI 线程 `startProxy` bind 127.0.0.1 + `syncTakeOver` 接管 settings.json → toast「代理已启动 监听 :port」+ 引导条消失 | 校验失败 → toast 报错、**零副作用**（不 bind 不接管）；校验中重复点 → 「操作进行中」拒绝 | 〔有〕`doToggleProxy/harvestTestResult :516-549/:1025-1048`；前置校验契约 `claude_service.h:150-157` | [单测]（test_claude service 层 + test_settings_sync 接管）；**真网启动 + 真接管未跑**（业务级未跑） |
| C-03 | 停止代理 | 点 `claude.btn.proxy`（运行中） | `stopProxy`：cancelActiveStreams → server stop → sidecar 快照按键级还原 settings.json → toast「代理已停止」 | 停止瞬间在途流被取消（设计）；还原失败 LOG_WARN 不阻断 | 〔有〕`claude_service.h:163-174` | [单测]（test_settings_sync 还原） |
| C-04 | 启动崩溃补还原 | 上次接管没走完还原（sidecar 残留）→ 再启动 | `recoverPendingSettingsRestore` 先还原，先于任何 startProxy（防残留键被误快照） | — | 〔有〕`main.cpp:404-417` | [单测]（test_settings_sync）；真机链未跑 |
| C-05 | 新建 Profile | `claude.profiles.new` / Ctrl+N → 表单（`claude.dialog.form.name/url/key` + 槽位 `claude.form.slot.*` + authfield 下拉 + 高级：方言/共享片段/备用 profile 下拉 + `claude.form.advanced.json` + env 行）→ 保存 | 落库（key 分平台加密入 `api_key_encrypted`）；无激活时可选自动激活；toast | 表单校验错误 → 弹窗内红字不关；非法 dialect/authField 归一 | 〔有〕表单层 + service 归一 | [单测]（test_claude_ui:505 tabs roundtrip） |
| C-06 | 编辑 Profile | 右键「编辑」/ `claude.act.edit` | 回填全字段；key 留空 = 不覆盖凭据 | key 改动 → 三列全量更新（check 判据） | 〔有〕`updateProfile` 契约 | [单测] |
| C-07 | 1M 上下文开关 | 详情 `claude.slot.onem.<i>` | 模型名追加/剥离 `[1M]` 归一落库 + toast；幂等（无变化不写） | 每次重读防同帧连点互覆；越界 slot 防御 return | 〔有〕`doSetOneM :565-605` | [单测]（test_claude_ui / 星锐网关拒 [1M] 教训——方言剥前缀归 upstream_dialect） |
| C-08 | 高级 JSON 全屏编辑 | 表单内 `claude.form.advanced.json.fs` → `claude.jsonfs.editor` | 格式化 / 应用（生效 JSON = profile ⊕ 片段，片段赢）/ Esc=放弃恢复落库值 | 解析失败 → 「（JSON 解析失败，接管时将忽略）」注记 | 〔有〕`discardJsonFullscreenEdits` + note 区分 | [单测]（test_claude_ui:2970/:3239） |
| C-09 | env 行编辑 | `claude.form.env.add` + 行内 input | 键值对叠加进接管 env（守卫键不写） | 溢出截断提示；坏 JSON 导出时 LOG_WARN 缺失部分 | 〔有〕doCopyConfig 同口径 WARN | [单测] |
| C-10 | 导入当前配置 | `claude.profiles.import` → `claude.form.import.text` → 解析 → 应用 | 解析 ~/.claude/settings.json → 预填表单（apply 前可见 diff warn） | 文件不存在/无 env → 预填空 + 提示；解析失败不落半态 | 〔有〕import 路径 | [单测] |
| C-11 | cc-switch 导入 | `claude.profiles.import.ccswitch` → 导入弹窗（列表/全选/导入） | 读 cc-switch 库 → 列出可导 profile → 选择导入落库 + 结果行 | 无内容可导入 → 空态；导入失败 toast——**整个弹窗 UI 零断言 = gap G3** | 〔有〕claude_ui_ccswitch:253/:280（空态已实现） | [未覆盖]（G3：纯逻辑有 test_ccswitch_import，UI 呈现层无） |
| C-12 | 删除 Profile | 右键「删除」/ `claude.act.enable` 旁 → 确认弹窗 → 确认 | 清凭据三列 → 删行；删的是激活且代理跑 → 自动停代理 + 还原 + 特殊 toast | 凭据清理失败 → 中止删行可重试 | 〔有〕`deleteProfile` 契约 | [单测]（test_claude 删除组） |
| C-13 | 设为激活 | 右键「设为当前」/ `claude.act.enable` | `setActiveProfile`（事务）+ 反向同步 settings.json env → 激活且代理跑 → re-takeover + toast | 无激活/读密钥失败 → fail toast；并发切（switchMutex_ 单写锁） | 〔有〕`claude_service.h:269-272` | [单测]（test_claude + test_settings_sync） |
| C-14 | 右键菜单五项 | 列表行右键 `claude.list.ctx` | 编辑/建副本/复制配置/删除/设为当前 | 回调按值拷贝 profile（防下帧悬垂——已修） | 〔有〕:737-768 | [单测] |
| C-15 | 建副本 | 右键「建副本」 | 名加「副本」后缀（重名递增）+ key 复制 + 不激活；二段写失败 best-effort 回滚 | 源缺 key → fail | 〔有〕`duplicateProfile` 契约 | [单测] |
| C-16 | 复制配置 | 右键「复制配置」 | 导出脱敏 JSON（key 占位）入剪贴板 + toast | 明文密钥命中待写文本 → **中止导出** + LOG_ERROR + toast；坏 JSON → WARN 缺失部分（不静默丢键）；剪贴板无回读 → 固定 LOG_WARN 键数留痕 | 〔有〕`doCopyConfig :674-745` 导出脱敏守卫 | [单测] |
| C-17 | 测试连接 | `claude.act.test` | 后台 15s HTTP POST /v1/messages → 2xx「连通成功」/ 401/403/404 定向提示 / 其他异常 | 上一轮未完 → 「操作进行中」；网络不可达 → 超时报错不崩 | 〔有〕`doTestConnection :983-1021` | [单测]（service 层 probeProfile）；真网 live 由 agent_live 的连通腿覆盖 |
| C-18 | 直连运行会话 | `claude.act.run` → 原生目录选择 → 确认 | 生成临时 settings（0600）→ 终端启动 `claude --settings` 新会话；零全局改动；目录记忆 `claude.run.lastDir` | 用户取消 → 静默返回；落盘/启动失败 → 撤 DB 记录 + toast | 〔有〕`runProfile` 契约 + forgetRunSettings | [单测]（test_claude 终端注入缝） |
| C-19 | 调用日志弹窗 | `claude.calls.viewall` → `claude.dialog.calls` | 最近调用表（时间/状态/耗时/tok 速率/路径）+ 搜索 `claude.dialog.calls.search` + 状态码下拉 + 翻页 prev/next | 空态/查询失败文案区分；分页越界禁用 | 〔有〕claude_ui_logs 三态 | [单测]（test_claude_ui:5693/:5743 + test_claude_calls_latency） |
| C-20 | 共享片段 CRUD | `claude.shared.new` / 右键编辑 | 片段名 + JSON 编辑（格式化按钮）+ 保存；改/删即时 re-takeover；删除同事务解绑 N 个 profile → toast 计数 | JSON 非法 → 弹窗红字不保存；删除确认弹窗 | 〔有〕`doDeleteShared :768-793` | [单测]（test_claude_ui shared 组） |
| C-21 | 余量配置弹窗 | `claude.act.quota` / Home 余量卡行 → `claude.dialog.quota` | 供应商下拉（opencode go/minimax/deepseek/volcano）→ 字段动态生成 → cc-switch 引用加载 → 测试连接（后台）→ 保存落 QuotaStore + toast（后台 60s 内自动查） | 手改字段 → 脱钩 cc-switch 标记；测试并发 → 「上一次仍在进行」；保存失败 → 弹窗内错误不关 | 〔有〕quotaHooks :948-981 | [单测]（test_claude_quota_dialog:7945/:8239）+ [live]（quota_live_probe 真机出网验收 + fixtures） |
| C-22 | 余量后台刷新 | quotaEnabled 开 + claude 开 | QuotaRefresher 真后台线程周期查询四家供应商 → QuotaStore 两表 → Home/详情读取 | DB 导入 begin/end → 暂停/恢复（原子闸防复活）；关开关 → 停线程（join） | 〔有〕onStart 订阅组 :322-345 | [单测]（test_claude_quota_store/lifecycle/minimax/volcano）+ [live] |
| C-23 | 引导条 | 本会话从未启动代理且未运行 | 顶部引导条（一次性内存态 `proxyEverSeen_`） | 运行过一次后永久消失 | 〔有〕:94-96 | [单覆盖缺口? 单测 test_claude_ui 骨架] [单测] |
| C-24 | 端口切换重启（设置页联动） | 设置→代理端口输入→应用（运行中） | stop→start 同端口重启 + 状态栏提示；快照就地刷新 | 非法输入红字（见 S-11）；重启失败 → 「代理已停止，可换端口重试」 | 〔有〕`applyProxyPort settings_module.cpp:406-453` | [单测]（test_settings 端口组） |
| C-25 | 接管状态可见 | 设置→代理卡 | 「● 已接管 <path>」/「○ 未接管」+ 警告行（最多 2 条，如 cc-switch 覆盖冲突） | 接管写失败 → 警告行显形（不以「已接管」冒充） | 〔有〕settings_ui_cards:592-642 | [单测]（test_settings） |
| C-26 | 流式转发（代理运行中） | Claude Code 指向 127.0.0.1:port 发流式请求 | SSE 透传 + 方言注入（UA/1M/x-opencode-session）+ failover 备用 profile 解析 + 调用日志落库 + 速率统计喂 Home | 上游断连 → 流取消不挂幽灵；方言方言错误 → 400 修正；命中备用 → 一跳切换 | 〔有〕upstream_dialect/proxy_router/resolveFallbackProfile | [单测]（test_streaming_proxy/test_streaming_stress/test_upstream_dialect/test_proxy_router）+ [live]（agent_live） |
| C-27 | 模块关闭下线 | 设置关 claude → onStop | 停余量刷新 → 注销工具 → 停代理还原；`prepareClaudeServiceBeforeModules` 不装配（端口不监听）；智能体页报「LLM 配置不可用」 | off→on 会话内 → 惰性装配（autoStart 才拉起）；重复 on 二次 bind 防御 | 〔有〕`main.cpp:383-425` + dispatchModuleToggle | [单测]（test_module_gating/test_module_toggle_bridge）+ [probe]（module_toggle_sidebar_probe 真点击） |

---

## 6. Agent 智能体（`src/module_agent`，模块可关、**默认关**）

触发类型六种：手动 / 定时（cron）/ 常驻 / 事件（todo.created、todo.completed、md.saved）/ 每天 / 闹钟（once）。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| A-01 | 启动清扫 | 模块 onStart | 上次会话悬挂 running run 清成 failed（`sweepStaleRuns`）；注册三自带工具（app.stats/app.notify/hot.list） | 清扫失败 WARN 不阻断 | 〔有〕`agent_module.cpp:262-266` | [单测]（test_agent_store） |
| A-02 | 打开任务列表 | 进智能体页 | 任务列表（名称/状态点/徽章：配置异常、限次、已停用/下一步时间） | 库未就绪 → 「数据库未就绪」错误行；空列表 → 空态引导 | 〔有〕compose :1745-1770 | [单测]（test_agent_ui:242/:1667） |
| A-03 | 新建任务表单 | `agent.page.newTask` → 弹窗（goal / 触发下拉 / cron+模板下拉 / 事件开关 / 闹钟输入 / 模型槽位 / 迭代上限 / 破坏性开关 / 只跑一次 / 执行记忆 / 自动记录）→ 保存 | 全字段落库 + 列表刷新 + 弹窗关 | 空 goal → 红字「请先填写任务目标」；坏 cron → 红字「cron 表达式非法」不关；事件全不勾 → 红字；闹钟过去时刻/格式错 → 红字；迭代 clamp 1-50 | 〔有〕`saveTaskDialog :781-877`（预览与保存同一份解析函数，预览能存进才存得进） | [probe]（agent_dialog_probe:173-240 真键入+模板下拉；agent_e2e_probe:546-587 真弹窗建任务）+ [单测]（test_agent_ui:398/:853/:896） |
| A-04 | cron 预览/模板 | cron 输入逐键 | 实时预览「下次触发：YYYY-MM-DD HH:MM」；模板下拉回填 | 非法 → 红字；一年无匹配 → 「一年内无触发时间」 | 〔有〕`refreshCronPreview :646-671` | [单测]（test_agent_cron）+ [probe] |
| A-05 | 事件/每天/常驻配置 | 触发下拉切型 | 各型只渲染对应控件（几何契约不变）；限次开关仅 schedule/event | 坏 trigger_config 库值 → 「配置异常」徽章 + 编辑回填点名不回落 manual | 〔有〕`openTaskEditor :701-777` | [单测]（test_agent_trigger_config / test_agent_ui） |
| A-06 | AI 生成草稿 | 任务弹窗 `agent.dialog.task.aichip` → 子弹窗输入一句话 → `agent.dialog.task.draft.generate` | 后台 LLM 生成 goal/cron/槽位草稿预填表单（保存仍由用户确认）；警告降级行 | 网关拒模型名/解析失败 → 弹窗内错误行；生成中重复点 → 禁用；关闭作废在途结果（epoch） | 〔有〕草稿组 :2356-2479；**双层 z 序无判据 = gap G5** | [单测]（test_agent_draft:4076-4419 全路径）；G5 [未覆盖]（probe 级） |
| A-07 | 编辑任务 | 行 `agent.task.<id>.edit` → 表单回填 | 库为源回填（含坏配置点名）；保存走 updateTask | 读库失败 → 页内错误行不开弹窗 | 〔有〕:701-711 | [单测] |
| A-08 | 启动任务 | 行 `agent.task.<id>.run` | ensureLlm 惰性装配 → ActiveRun 线程跑 AgentLoop → 自动开详情弹窗实时刷新（uiPolling） | run 中再点 → 按钮已禁（运行中…）；LLM 未装配 → 按钮禁 + 页内错误行；线程创建失败 → 摘除占位不标运行中 | 〔有〕`startTask :523-590` | [probe]（agent_e2e_probe:676-772 真点 .run 启动）+ [单测]（test_agent_loop） |
| A-09 | 停止任务 | 详情弹窗「停止」chip（仅有在跑 run 时渲染） | 置 cancel → 当前轮结束生效 → 状态 cancelled | 无 ActiveRun（DB 残留 running 行）→ 不渲染停止（不骗点击）；请求落空 WARN | 〔有〕`requestStopRun :425-437` + detail dialog:124-138 | [单测] |
| A-10 | 删除任务 | 行 `.delete` → `agent.dialog.delete`「确认删除」 | 单事务级联删 runs/messages；详情正显示被删任务 → 一并关 | 确认时刻 run 已起（scheduler 派发）→ 二道闸拒绝「请先停止后再删除」；运行中行内 chip 禁用 | 〔有〕`performDeleteTask :2537-2568` | [单测]（test_agent_store/UI） |
| A-11 | 启停任务 | 行 `.toggle` 禁用/启用 | `setTaskEnabled` 取反；禁用任务 Scheduler 跳过 | 失败 → 页内错误行 | 〔有〕`toggleTaskEnabled :880-891` | [单测] |
| A-12 | 常驻入队 | 常驻任务行 `.enqueue` | `scheduler_->enqueueResident` 手动跑一轮 | Scheduler 未装配 → 置灰 + WARN | 〔有〕:1910-1922 | [单测]（test_agent_scheduler） |
| A-13 | 详情弹窗浏览 | 行点 / `.detail` → `agent.dialog.detail` | 三摘要卡 + 配置头（可折叠）+ runlist（20 条/天分组）+ 选中 run 消息区（默认折叠） | 消息读失败 → 红字不拿旧消息伪装；无 run → 空态 | 〔有〕refreshData 详情段 :1291-1327 | [probe]（agent_dialog_probe:310-359 详情错误行+复制 chip）+ [单测]（test_agent_detail_layout 三档几何） |
| A-14 | 消息展开/复制 | 消息头 `.toggle` 展开 / `.copy` 复制全文 | 块级渲染（text/工具名/工具输出错误态）；复制 toast；最近 assistant 默认展开 | 空消息 → 「（空消息）」；手动收起不被每帧顶回 | 〔有〕`renderMessageRow :1381-1528` | [单测]（test_agent_ui / test_message_blocks） |
| A-15 | 复制最终回复 | 消息区头部 `.msgs.copyfinal` | 最后一条 assistant 纯文本入剪贴板 + toast | 剪贴板 void 无回读 → LOG 留痕 | 〔有〕setClipboardText + LOG | [probe]（agent_dialog_probe 复制 chip） |
| A-16 | 计划 vs 实际 | 详情选中 schedule run | 「计划 vs 实际」三行（previousCronMatch 回扫缓存键控） | 坏配置 → verdict 显形 | 〔有〕`updateDetailPlanCache :952-960` | [单测]（test_agent_time_text） |
| A-17 | 运行结束通知 | run running→failed/limited（页面不可见也测） | app 级 toast「智能体任务失败/受限」+ **OS 系统通知**（双通道同文案）+ `requestUiUpdate` 主动要帧 | completed/cancelled 静默；同 runId 只发一次（notifiedRunIds）；系统通知未受理 → LOG 不重试；授权阻塞 ≤1s → 发送挪后台线程 | 〔有〕refreshData 通知段 :1209-1290 + 装配层 pollRunEnd（app_sidebar.cpp:243-251） | [单测]（detectRunEndNotifications 纯函数 + test_notify_tool） |
| A-18 | 授权请求 | 首次有窗口可见帧 | `requestAuthorization` 只请求一次（macOS 授权框不在无前台窗时弹） | 拒绝授权 → 系统通道降级（app toast 仍达） | 〔有〕`requestNotificationAuthorizationOnce :1005-1009` | [未覆盖]（授权流无断言；平台层 test 未覆盖授权 UI） |
| A-19 | 页内错误 + 去引导 | run 失败「LLM 配置不可用…」前缀 | 页内错误行 + 「去 Claude 页 →」引导跳转 | 普通网络失败不上页内行（区分口径）；已看过的 run 错误不回弹 | 〔有〕refreshData pageError 段 :1173-1208 | [单测]（test_agent_ui:1541/:2869）+ [probe]（agent_dialog_probe:310-359） |
| A-20 | 可用工具区块 | `agent.tools.header` 点击展开 | 注册表快照列出（name/描述/参数）+ 数量；展开强制重装反映模块启停 | 空注册表 → 只头部 | 〔有〕`ensureToolsCache :995-999` | [单测]（test_agent_tools） |
| A-21 | 工具真实执行 | run 过程中调 md.*/todo.*/claude.*/app.*/hot.list | 工具经 ToolRegistry 真执行落库/真 HTTP（hot.list 真出网+限频） | hot.list 网络失败 → 工具报错进 run error 不崩；app.notify → 入队 app toast | 〔有〕工具各文件 | [live]（agent_live：LLM 腿全链真跑，LLM 不可用记「未跑」四态结论）+ [单测]（test_markdown_tools/todo_tools/claude_tools/hot_tool/notify/stats） |
| A-22 | 悬挂 run 兜底轮询 | 外部直写库插入 running run | 15s 轻查询命中 → refreshData 进入常规观测 → 终态通知 | 查询失败 WARN 下周期重试；短命 run（<15s 起止都在检查点间）零通知——已登记 backlog 不修 | 〔有〕`pollRunEndFallback :1044-1060`（已知洞注释） | [单测]（回补审计组） |
| A-23 | 库导入刷新 | GitHub Sync 拉取 → `db_imported` | tasksDirty → 重查（不碰 UI 成员） | 闸 + 退订序防悬垂 | 〔有〕onStart 订阅组 | [单测] |
| A-24 | 迭代/限次终态 | run 跑满迭代上限 / max_runs | 状态 limited；限次任务跑满自动停用 + 徽章「一次性/限N次」 | 迭代输入非法字符忽略、>1000 截断；保存 clamp 1-50 | 〔有〕parseIterInput + saveTaskDialog | [单测]（test_agent_loop/test_agent_ui） |

---

## 7. Settings 设置页（`src/module_settings`，恒注册）

四 Tab：通用（外观/编辑器字号）、模块（模块开关/余量开关/笔记目录/代理卡）、日志（设置+tail）、同步（GitHub Sync）。

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| S-01 | 读配置 | 启动 / 进页 | sanitized 载入（越界钳制/未知枚举收敛/模块 ID 过滤） | 库未就绪/表缺失 → 默认值 + LOG_ERROR（loadChecked 如实报） | 〔有〕`SettingsStore` 契约 | [单测]（test_settings/test_app_config） |
| S-02 | 主题模式 | `settings.card.appearance.theme.dd`（自动/浅色/深色） | 即时切深浅 + 持久化 + 状态行；auto 进页重解析跟随系统 | 无系统回调 → 只能回页重解析（已知上游限制） | 〔有〕`applyThemeMode/onPageEnter` | [单测]（test_settings:759/:2088-2215） |
| S-03 | 主题色 | `settings.card.appearance.accent.sw.0-3`（Claude 橙/雾紫/玫瑰粉/QQ 蓝） | 即时全 UI 换色 + 持久化 | — | 〔有〕`applyAccentTheme` | [单测]（test_theme_accents） |
| S-04 | 字号缩放 | `settings.card.appearance.font.dd`（85-130%） | token 层全局缩放下一帧生效 + 持久化 | 越界 sanitized 钳制 | 〔有〕`applyFontPreset` | [单测]（test_theme_fontscale） |
| S-05 | 窗口效果 | `settings.card.appearance.effect.dd`（关/半透明/磨砂） | 三档即时生效；降级提示行（⚠ 图标+省略号） | 系统不支持磨砂 → 每帧回查显示降级提示不崩 | 〔有〕`windowEffectDegradedHint` | [单测]（test_settings + 降级提示已接 Ellipsis） |
| S-06 | 编辑器字号 | `settings.card.editor.dd`（10-32px） | Markdown 编辑器下一帧生效 + 持久化 | sanitized 钳制 | 〔有〕`applyEditorFontPreset` | [单测]（test_editor_font） |
| S-07 | 模块开关×4 | `settings.card.modules.sw.0-3`（markdown/todo/claude/agent） | **即时生效**：注册/注销 Router + onStart/onStop + 侧边栏当帧增删；关当前页 → 回首页 + toast「XX 已关闭」 | 重复点击同值不写盘；未知 ID LOG 忽略；claude 开 → 惰性装配（autoStart 才起代理） | 〔有〕`applyModuleEnabled` + `dispatchModuleToggle :223-261` | [probe]（module_toggle_sidebar_probe:211-253 真点击→当帧出现/消失/回首页+toast）+ [单测]（test_module_gating 全关/损坏回退、test_module_toggle_bridge、test_app_sidebar:342/:416） |
| S-08 | 余量总开关 | `settings.card.quota.sw` | 即时生效：首页卡/配置入口显隐 + 后台刷新启停；关不删数据 | claude 模块关时开 quota → 刷新装配缺服务 WARN 不启 | 〔有〕`applyQuotaEnabled` + startQuotaRefresh 守卫 | [单测]（test_settings + quota 门控组） |
| S-09 | 笔记目录卡 | `settings.card.directory.open`「打开目录」 | 系统 Finder/Explorer 打开 markdownDir | 目录不存在 → 路径文案「（尚未创建）」；打开失败 LOG_ERROR | 〔有〕settings_ui_cards:413-498 | [单测]（test_settings） |
| S-10 | 随应用启动代理 | `settings.card.proxy.autostart` 开关 | 下次启动自动拉起代理；持久化 | 当前会话不立即起（文案说明「下次启动生效」） | 〔有〕`applyAutoStart` | [单测] |
| S-11 | 代理端口 | `settings.card.proxy.port.input` → 回车 / `.port.apply`「应用」/ Esc 丢弃 | 范围内纯数字 → 保存 + 运行中自动 stop→start 重启 + 状态行；代理未运行 → 「下次启动/启用时生效」 | 非法（越界/带符号/非纯数字）→ 红字「端口无效：请输入 1024-65535 的纯数字」不保存；Esc=丢弃草稿不触发应用；重启失败 → 红字「代理已停止，可换端口重试」 | 〔有〕`applyProxyPort :406-453` + `parseProxyPortInput` 纯函数 | [单测]（test_settings:1914/:1982/:2184 esc 草稿/ctrl+S/下拉） |
| S-12 | Ctrl+S | 端口草稿非空且≠已应用 → 应用端口；否则回「已保存」 | 状态行反馈 | — | 〔有〕onKeyEvent :250-275 | [单测] |
| S-13 | Esc 关下拉 | 任一下拉开 → Esc | 关最上层下拉（7 下拉互斥单槽） | 全关 → 透传 | 〔有〕`setDropdownOpen` | [probe]（settings_dropdown_z_probe z 序）+ [单测] |
| S-14 | 日志保留 | `settings.card.log.dd`（永久/7/30/90/180/365 天） | 切档立即清理一次 + 回显条数；持久化 | 0=永久跳过清理；DB 未就绪 → 「重启后生效」；清理失败红字 | 〔有〕`applyRetentionPreset :352-382` | [单测]（test_settings + pruneCallLogs 单测） |
| S-15 | 日志级别 | `settings.card.log.level.dd`（DEBUG/INFO/WARN/ERROR） | `core::log::setLevel` 即时生效 + tail 面板即时跟随（logTailDirty） | 未知值收敛 INFO | 〔有〕`applyLogLevelPreset` | [单测] |
| S-16 | 运行日志 tail | `settings.tab.log` 卡：`settings.tab.log.refresh` 刷新 + 滚动 | 后台线程读末 500 行 + 按级别过滤 + 版本化 contentKey 重测 | 空文件/低于级别 → 「（暂无日志内容…）」；读盘失败不闪空（快照） | 〔有〕refreshLogTailCache 快照模式 | [单测]（test_settings:1367/:1571） |
| S-17 | GitHub Sync 启用 | `settings.card.github_sync.enabled.sw` | 配置齐全 → 首次方向判定（远端有数据→拉取覆盖；空→首次上传）；进表单 | 配置不齐 → 静默跳过（不在拨开关瞬间弹错——用户手动操作自会得到报错） | 〔有〕`maybeStartFirstGithubSync :534-544` | [单测]（test_github_sync_logic） |
| S-18 | 配置输入 | `.url.input` / `.branch.input` / `.token.input` | URL/分支变更清首次标记（换仓重新判方向）+ git 探测置脏；Token 会话内落库 | 坏 URL → 测试连接报错不崩 | 〔有〕cardActions 1108-1125 | [单测]（test_github_sync_card） |
| S-19 | 立即同步 | `.btn.push`（运行中三者禁用） | 后台线程跑引擎：dump 整库（skip 清单除外）→ commit → push；进度行「① …」；成功清 dirty/未同步黄字 | 首次标记缺失 → 自动改跑 FirstSync（含三步校验）；repoUrl 空 → 即时错误 + 详情弹窗；Token 空 → 「未配置 Token」 | 〔有〕`githubSyncPush/startGithubSyncAction :555-823` | [单测]（test_github_sync_engine）+ [live]（github_sync_live 真网逐场景） |
| S-20 | 测试连接 | `.btn.test` | 后台校验连通性（仓/分支/token） | 404/401/网络错 → 错误行 + 弹窗变体定向 | 〔有〕 | [单测] + [live] |
| S-21 | 拉取 | `.btn.pull` | 远端整库 dump 导入 → DROP 重建 → 发 `db_imported` → md/todo/agent 重载 + 配置重载 | 拉取覆盖期间 claude 余量刷新暂停（begin/end 事件） | 〔有〕引擎 + 各模块订阅 | [单测] + [live]；**「拉取→三模块缓存重载→编辑不丢」整链业务级未跑** |
| S-22 | 自动推送 | `.autopush.sw` 开 | dirty 事件（md 五种 + todo 七种写入事件订阅）→ 节流 5 分钟自动 push | 漏订一类 = 永不同步（守卫注释钉死）；关自动推 + dirty → 退出置「未同步」黄字不静默 | 〔有〕订阅清单 :172-183 + `runExitSyncPush :661-684` | [单测]（test_github_sync_logic 事件组） |
| S-23 | 409 冲突弹窗 | push 撞远端新提交 | 冲突变体弹窗：「拉取后重推」需**二段确认**（conflictConfirmArmed）→ PullThenPush | 单击不执行；关闭 = 放弃 | 〔有〕git_error_dialog 变体纯函数 + armed 闸 | [单测]（test_github_sync_engine / 变体单测） |
| S-24 | 401 失败弹窗 | token 失效 | 定向变体：「更新 Token」按钮关弹窗回卡片 + toast 引导；错误行保留 | — | 〔有〕`updateToken` 动作 :952-958 | [单测] |
| S-25 | 通用失败弹窗 | 其他失败（手动操作） | 详情弹窗：摘要 + 请求日志翻看 + 复制详情 + 重试（按上次动作） | 自动同步失败只留卡片红字不弹窗（不打断）；ValidateFirstSync 失败同静默 | 〔有〕harvest 分支 :930-937 | [单测]（test_github_sync_card） |
| S-26 | 退出补推 | 退出应用时 dirty + 自动推送开 | atexit 推送，≤5s 完成即收；到点未完 → detach + 置未同步黄字（下次启动可见） | 已有任务在飞 → 不等（waitOrDetach）；断网 → 5s 放行不拖退出 | 〔有〕`waitOrDetachSyncThread :698-717` | [单测]（test_github_sync_logic） |
| S-27 | 本地 git 探测 | 进同步卡 | 后台探测本地 git（macOS 只查不执行防弹 CLT 安装框）；不可用 → 灰字 + 一次性 toast 警告 | 探测在途 → 不重复起；配置变更重探 | 〔有〕`maybeRefreshLocalGitProbe :637-654` | [单测]（test_local_git_probe） |
| S-28 | public 仓警告 | 测试连接发现 public 仓 | 黄字「仓库为公开（public）：笔记、整库快照与其中的密钥将公开可见」（提示不硬拦） | — | 〔有〕card :327-336（口径与描述行同源 sync-02） | [单测]（test_github_sync_card） |
| S-29 | 未同步黄字 | 上次退出未推完 | 「有未同步改动…建议点『立即同步』」，推送成功即清 | — | 〔有〕 | [单测] |

---

## 8. About 关于页（恒注册，纯展示）

| ID | 场景 | 操作序列 | 正向期待 | 反向设计 | 反向现状 | 当前覆盖 |
|---|---|---|---|---|---|---|
| B-01 | 静态信息展示 | 进关于页 | 两卡（版本/Git commit/构建时间/编译器/平台/C++ 标准；GUI 框架/EUI-NEO rev/数据库/HTTP/TLS/字体）+ 页脚；内容区 900px 居中 | 宏缺失回退 "dev"/"unknown"；极矮窗 max 护栏不重叠 | 〔有〕about_module.cpp:26-49/:280-348 | [单测]（test_ui_modern:1030-1205；about_probe 已删=业务级未跑） |
| B-02 | 光晕动画 | 页面空闲 / 鼠标移动 | 30fps 光斑漂移+呼吸+聚光扫过；指针移动唤醒/续期，空闲超时休眠零重绘 | — | 〔有〕awake()/onTimer 门控 | [单测]（:1236 六光斑参数） |

---

## 9. 并入的缺口清单（与 ui-test-gap-map 对账）

`docs/superpowers/plans/2026-10-19-ui-test-gap-map.md` 的 G1–G5 已并入上表：
- G1（markdown 树列表加载失败态）→ M-14 [未覆盖]
- G2（markdown 状态栏保存失败态）→ M-02 [未覆盖]
- G3（cc-switch 导入弹窗 UI）→ C-11 [未覆盖]
- G4（home.actions 模块开关联动）→ H-06/07/08/12 [未覆盖]
- G5（agent 草稿子弹窗双层 z 序）→ A-06 [未覆盖:probe 级]

轻量化删掉的 8 probe 面（2026-10-03 裁决）在本文口径下的新增欠账（新标准要求业务流程 probe 回补）：
- **todo 拖拽**（T-15/16）——用户主链、写库操作，真指针从未跑过（最危险 #1）
- **markdown 编辑/弹窗全链**（M-01~M-29）——440 处 harness 但零真窗口
- **claude profile 表格/表单/日志**（C-01/05/19）——518 处 harness 但零真窗口
- **settings 卡片几何**（S-02~S-16 中 compose 面）——dropdown z probe 仅覆盖下拉一层
- **agent 列表**（A-02 列表面）——agent_dialog/e2e probe 覆盖弹窗与启动，列表行点击链部分
- about 布局、claude calls 延迟、markdown 树 z——纯展示面，回补优先级最低

## 10. 补全优先级建议（业务级 probe 排序）

1. Todo 拖拽真指针 probe（T-15/16）——数据写错位风险 + 每日主链
2. GitHub Sync 拉取→重载→编辑保持整链（S-21 + M-26 + T-19 + A-23）——丢数据风险
3. Claude 代理启停 + 接管/还原真机链（C-02/03/04/25）——破坏用户 Claude Code 环境
4. G1/G2/G3/G4/G5（ui-test-gap-map 已排，S 级成本）
5. markdown 编辑器真窗口键盘流 probe（M-04/07/08 IME/组字路径无真输入测试）
