# 静态扫描·四面补维（错误吞没 / 样式硬编码 / 平台纪律 / 文案一致性）

- 池条目：need 284｜日期：2026-11-18｜角色：code-reviewer｜只读静态扫描（grep 全量→窄读锚点确认，未整文件通读；未改代码、未跑构建/应用、未联网）
- 检出分支：feat/notepad-batch1（notepad 在建未合主干，相关发现标「在建模块」，不计存量债）
- 前置事实（扫描前实测）：src/core/result.h 的 Result 类**未标 [[nodiscard]]**（:73-74 主模板、:134-135 void 特化）——丢弃 Result 零编译告警；故错误吞没面按实情扫 (void) 显式丢弃 + 空错误分支两条路。

## 0. 总览计数

| 面 | P0 | P1 | P2 | P3 | 小计 |
|---|---|---|---|---|---|
| a 错误吞没 | 0 | 2 | 1 | 0 | 3 |
| b 样式硬编码 | 0 | 0 | 2 | 3 | 5 |
| c 平台纪律 | 0 | 1 | 2 | 1 | 4 |
| d 文案一致性 | 0 | 0 | 1 | 3 | 4 |
| 合计 | 0 | 3 | 6 | 7 | 16 |

P0 为零的依据：唯一接近 P0 的数据面（退出保存丢稿、迁移失败无弹窗）走查一已登记（§2.4/§2.6），不重复；schedule 域丢弃判 P1 而非 P0——可恢复重放（重启后重算/重投），非不可逆损毁，依据见 a-2。

**Top 3**：① result.h 缺 [[nodiscard]]（吞没面根因，全站）② schedule 域 store 写操作 30 处级 fire-and-forget（重复投递/规则状态复活）③ app_sidebar.cpp 业务层平台宏 ×2（§三铁律违例）。

---

## a. 错误吞没面（3 条）

- **[P1] 错误吞没 Result 类未标 [[nodiscard]]，全站丢弃返回值无编译告警** | src/core/result.h:73-74、:134-135 | 影响面：全站 | 建议方向：主模板与 void 特化补 [[nodiscard]]；告警生效后现存 (void) 点转为「显式决策点」，逐点补理由注释或改接 UI 反馈。If we skip it：后续新增调用点继续无告警静默丢弃，吞没面只增不减，本轮清点无法靠编译器长期守住。
- **[P1] 错误吞没 schedule 域 store 写操作系统性 fire-and-forget（(void) 丢弃 30 处级）** | src/module_schedule/action_runner.cpp:209/226/237/247/260/275/360/365/372、src/module_schedule/schedule_engine.cpp:61/149/200/236/248/282/347/377/418/518/567/571/630/660/665/672 | 影响面：单模块（schedule 全域） | 建议方向：先分类再收口——投递状态迁移（updateDelivery）与规则启停/归档（updateRule/archiveRule）失败至少 LOG_WARN + 状态行提示；同文件 insertDelivery（action_runner.cpp:198）严检 vs update 全丢的不对称说明并非有意豁免。If we skip it：进程崩溃/重启后 delivery 行停在 retry/pending，重启触发重复投递与重复提醒；规则启停态回退，用户已关掉的规则复活。
- **[P2] 错误吞没 记忆评测结果落库失败静默（无日志无 UI）** | src/module_memory/panel/panel_state.cpp:664 | 影响面：单模块 | 建议方向：save 失败补 LOG_WARN + 写 snap_.lastEvalError（与 :668-669 评测失败分支对称）；评测是花钱的 LLM 调用，结果丢了连痕迹都没有。

**正面确认（非发现）**：catch 块全站 20 处全部为线程入口/第三方边界兜底且均 LOG 非静默（scheduler.cpp:172-228 三线程体、ticker.cpp:108、event_bus.h:110、agent_builtin_tools.cpp:257/319/387、agent_module_schedule.cpp system_error ×3、platform file_watcher/http_client），不违「不抛异常」纪律；空 else / 空错误分支扫描 0 命中；已知锚点直接引用不重查：笔记 git 自动提交失败仅日志（md_note_git.h）、迁移失败无用户弹窗（main.cpp:210-212，走查一 §2.6）、toast 桥仅 main/home/schedule 接入（toast_notice_bridge.h:13-18，超时矩阵）。

---

## b. 样式硬编码面（5 条）

- **[P2] 样式硬编码 语义提醒色三档硬编码 hex 绕 theme** | src/module_release/release_reminder.cpp:89-93（0xEF4444 / 0xF59E0B / 0xEAB308） | 影响面：单模块（上线提醒弹窗/横带/角标） | 建议方向：注释自证「票据指定色系」有出处，但 theme.h:42-54 已备 success/warning/error(+Text) 语义 token——Red→error、Orange→warning、Yellow→success 映射或补专用 token，亮暗主题自动适配。
- **[P2] 样式硬编码 估宽实现三套口径并存** | src/module_agent/agent_module.cpp:292（cachedTextWidth，weight 入参）vs src/module_terminal/terminal_page.cpp:135（textWidth，weight 恒 400）vs src/module_notepad/notepad_window.cpp:186（码点数×7px 粗估，**在建模块**） | 影响面：跨模块（agent/terminal/mcp/notepad） | 建议方向：ui_kit 出统一 cached measure 入口——bug248 后 agent/terminal 已各写一份同款缓存，mcp_page_module.cpp:302/306/419、agent_module_dialogs.cpp:2043 直调点一并迁移；第三次复制（notepad）发生前收口。
- **[P3] 样式硬编码 徽标/图标/品牌字 700 家系（豁免口径待裁决）** | src/module_todo/todo_ui_board.cpp:950（完成 ✓ 角标）、src/app_sidebar.cpp:417/470（标题栏品牌字 DevDesk）、src/ui_kit/splash.cpp:211（splash 品牌字）、git_views.cpp:677（已见于走查一 T8） | 影响面：全站 | 建议方向：与走查一 T8 合并裁决「图形/品牌类 700 豁免名单」，名单外归 600。
- **[P3] 样式硬编码 「大纲」面板标题 700 新锚点** | src/module_markdown/md_module_preview.cpp:57 | 影响面：单模块 | 建议方向：并入走查一 T8 / 走查二 §2 的「弹窗·面板标题 700 待过会」家系统一裁决，不单独修。
- **[P3] 样式硬编码 终端 ANSI 32 色调色板硬编码** | src/module_terminal/terminal_grid_widget.cpp:237-246 | 影响面：单模块（终端语义合理豁免候选） | 建议方向：登记豁免，或随亮暗主题出两套 palette；不阻塞打磨批次。

**字重全量盘点**：500 全站仅已知 2 处（git_views.cpp:174 已见 T2、md_assoc_card.cpp:181 已见走查二），**无新增**；700 新增锚点即上 2 条 P3（todo 角标、sidebar/splash 品牌字、md 大纲标题），其余 700 均已见于既有产物（wallpaper_ui.cpp:1193、about_module.cpp:276、release_detail_draw.cpp:875/2448、release_reminder.cpp:894、agent_detail_dialog.cpp:183、dialog_form.cpp:41 默认档）。
**正面确认**：业务代码 0x 色值全量扫描，真颜色违例仅 release_reminder 一处（其余命中为 hash 常量/codepoint 边界/平台合法文件）；裸字号走查一 §4 已盘（五模块无脱离 token），本轮未重扫、引用其口径。

---

## c. 平台纪律面（4 条）

- **[P1] 平台纪律 业务 UI 文件内 #ifdef __APPLE__ ×2** | src/app_sidebar.cpp:1388（回退态系统标题栏 inset 30）、:1441（macOS 窗口圆角 16 vs Windows DWM 8） | 影响面：全局壳 | 建议方向：注释自证「回退缝」且载明出处（need:140 F-03 / 审计 P1-1），但 AGENTS.md §三铁律无豁免条款；两常量迁入 window_layout 族访问器（window_layout_macos/posix/win 三平台文件已存在，零新拆分成本）。If we skip it：平台宏缝留在业务层成先例，此文件后续改动易复制同款写法，铁律事实上松动。
- **[P2] 平台纪律 模块内共享头 #if defined(_WIN32) ×2** | src/module_wallpaper/platform/mpv_controller.h:263/274 | 影响面：单模块（模块内 platform 拆分目录） | 建议方向：目录内已有 _win/_macos 拆分文件，头内分支抽成 inline 访问器或移入各平台实现文件。
- **[P2] 平台纪律 平台拆分映射表漂移（维护债）** | .zcode/cheatsheets/platform-file-map.md（2026-10-28 生成）vs 现文件树 | 影响面：跨仓协作纪律 | 建议方向：表缺 5 族（importer、mpv_controller、terminal_settings_platform、terminal_backend、local_git_probe 的 posix 列口径），且 image_probe/wallpaper_host/yield_watchers 实际已有 macos 侧文件（.mm）表仍标「仅 Windows」——按表自述规则 3「更新本表」补齐（改小抄属流程资产，另派处理）。
- **[P3] 平台纪律 编译器宏进业务文件（非平台宏，登记备查）** | src/module_toggle_bridge.cpp:71（__GNUC__）、src/module_memory/proposals/proposal_store.h:79/101（__GNUC__ && !__clang__） | 影响面：单点 | 建议方向：与「拆文件」精神同族；如需消音迁 utils，不阻塞。

**⚠ 族核对（对照 platform-file-map.md）**：file_watcher 无 posix 兜底（已知红，不重登记）、window_visibility 无 posix（表已标 ⚠）；表外实态 = local_git_probe 双端无 posix、terminal_backend_win 仅 win、image_probe/wallpaper_host/yield_watchers 已补 macos——**无新发现的无兜底业务族**，更新映射表即可。
**正面确认**：业务代码（module_*/core/，排除合法平台文件与 platform/ 目录）平台宏扫描，_WIN32/__APPLE__ 违例仅 app_sidebar 2 处；其余命中均为 DEVDESK_* 构建宏回退（about/mcp/settings，有意模式）或合法文件内部。

---

## d. 文案一致性面（4 条）

- **[P2] 文案一致性 错误反馈通道构造谱系不一** | src/module_todo/todo_module.cpp:586-589（setStatus→toast：错误详情挤在标题、正文恒空）vs src/module_markdown/md_module_editor.cpp:538、src/module_memory/memory_module.cpp:90 等（showToast：标题短语 + 正文详情分槽）vs src/module_settings/settings_module.cpp:676（状态行）与 :686（注释自证部分分支已迁 toast） | 影响面：跨模块 | 建议方向：定谱系「toast 标题=动作短语、正文=errorMessage」，todo 迁双槽；settings 状态行/toast 的分工边界成文。
- **[P3] 文案一致性 钥匙串失败 toast 丢弃底层错误详情** | src/module_memory/memory_module.cpp:90（正文写死「钥匙串写入失败，请重试」） | 影响面：单模块 | 建议方向：正文补 r.errorMessage()（keychain 错误链本就区分 NotFound/拒绝/用户取消，超时矩阵行 17 已证），排障可用。
- **[P3] 文案一致性 确认类按钮谱系离群值** | 主流谱系成立：表单「取消/保存/删除」（标准壳 cancel/confirm 槽）+ 只读弹窗 cancel 槽「关闭」（wallpaper_ui.cpp:1250、settings_module.cpp:1218、agent_module_dialogs.cpp:1393/2175、memory_module_modals.cpp ×4、mcp_page_module.cpp:1024）；离群锚点：src/module_release/release_detail_draw.cpp:2492「知道了」（全站唯一）、:832「移除」vs 全站「删除」、terminal_page.cpp:1379 与 memory_module_modals.cpp:227「完成」（向导尾步，语义可留） | 影响面：跨模块 | 建议方向：「知道了」归「关闭」档；「移除」若确指 detach 语义可留但写进口径，否则归「删除」。
- **[P3] 文案一致性 用户可见文案半角冒号混排** | src/module_settings/settings_module.cpp:488（「开启开机自启动失败: 」半角+空格）vs 全站全角「：」（todo_module.cpp:299/311/323/361/373、markdown/memory showToast 族）；src/module_settings/app_tools.cpp:84（agent 工具结果串）同半角 | 影响面：跨模块（2 处锚点） | 建议方向：用户可见串统一全角「：」；LOG_* 串半角冒号属日志约定（本次已按此前缀剔除）不算。

**正面确认**：ASCII "..." 混入中文文案 0 处；空错误分支 0 处；已知锚点引用不重查：托盘模块开关 toast 文案未定稿（走查一 §2.6）、md「● 未保存」红文案（走查一 §2.4）。

---

## 与既有走查产物的重合度说明

**完全重合，仅引用不登记**：字重已知 6 锚点（简报列名）；git_views.cpp:677 徽标 700（走查一 T8）；弹窗标题 700 家系（走查一 T8 + 走查二 §2，含 agent_detail_dialog.cpp:183 / release_detail_draw.cpp:2448 / release_reminder.cpp:894）；托盘 toast 文案（走查一 §2.6）；迁移失败无弹窗（走查一 §2.6）；笔记退出丢稿（走查一 §2.4）；toast 桥覆盖面（超时矩阵）；笔记 git 自动提交失败仅日志（简报已知）；memory chip emoji 估宽（走查二 §1.3）；release「+N」估宽（走查二 §2）；agent 药丸估宽 500/600（走查二 C1，简报已知）。

**部分交叉，本轮加维**：估宽问题——既有只报参数不一致单点，本轮升为「三套实现并存」的结构性发现（b-2）；release 上线提醒——既有只报标题 700，本轮补语义色绕 token（b-1）；dialog 标题 700 待裁决家系——本轮补「面板标题」新成员（b-4）。

**纯新增面**：错误吞没（[[nodiscard]] 缺失、schedule fire-and-forget 群、panel_state 评测落库）；平台纪律（app_sidebar 平台宏、mpv_controller.h 头内宏、映射表漂移）；文案谱系（toast 双槽构造、半角冒号混排、按钮离群值）。

---

### 方法与边界

rg 全量 grep（命中裁尾）→ 12 处窄读锚点确认（±25-85 行）；未整文件通读。Result 丢弃面以 (void) 显式丢弃 + 空错误分支为准；`.value()` 无守卫调用与 valueOr 误用未做调用图级核对（超静态扫描边界，登记为未核面，可交 QA/后续 role）。字重盘点覆盖 module_*/app_*/ui_kit 全量，grep 模式含 fontWeight 相邻 500/700。

Lean: 约 -50 lines possible.（估宽三套并一套 ≈-40、app_sidebar 宏缝迁访问器 ≈-8、release_reminder 迁 token ≈-4；schedule 域补检查为增行不在净省口径内。）
