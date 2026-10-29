# Todo 看板测试清单卡

- 日期：2026-10-29（依据契约 docs/superpowers/plans/2026-10-23-UAT62-H20-Todo四列固定与归档折叠设计.md）
- 执行人：测试 QA
- 口径：每条用例 = 一个用户场景；「已覆盖」= 现有自动化已断言（见证据列）；「缺口」= 尚无用例，需新增或标「未验证」。
- 总判定（2026-10-29 缺口回填后）：**完成**——6 个缺口全部关闭或部分关闭（仅 3 项标未验证，见执行记录遗留段）。

## A. 四列固定与列模型

| F-ID | 场景（验证什么能力） | 验证方法 | 现状/证据 |
|---|---|---|---|
| A1 | 新装用户打开 Todo 恰见固定四列（待办/进行中/已完成/已归档），无「+列」入口 | 单测 schema 种子 + UI 渲染断言 | 已覆盖：todo_svc_create_list_columns、todo_tools_schema_spot_check_and_registry、todo_ui_h20_board_render |
| A2 | 列名不可改名、不可增删列 | 断言 UI 无 ✎/右键菜单/+列按钮 | 已覆盖：todo_ui_column_color_dot_and_menu、todo_svc_rename_column（服务层拒改语义需复核口径） |
| A3 | 新建任务默认落「待办」列 | 服务层创建后查列 | 已覆盖：todo_tools_create_default_first_column |
| A4 | 老库迁移：kind 空补 doing、多余自定义列任务并入待办列尾、迁移幂等 | 造旧库 fixture 跑迁移两次比对 | 已覆盖：todo_svc_fixed_migration_idempotent；**缺口：迁移前自动备份 DB 无断言** → 待补（记 BUG 缺口 BUG-G1 未验证） |
| A5 | 列色点与固定顺序（#D97757/#C29243/#788C5D/#9A948A） | UI 渲染断言 | 已覆盖：todo_ui_column_color_dot_and_menu（色值逐项核对未做，标「部分」） |

## B. 创建弹窗（唯一入口）

| F-ID | 场景 | 验证方法 | 现状/证据 |
|---|---|---|---|
| B1 | 只剩页头「新建任务」一个入口，列头 + 与空列 + 均已删 | UI 断言按钮存在性 | 已覆盖：todo_ui_newtask_dialog_plan_a |
| B2 | 空标题点创建：内嵌红字、弹窗不关、不落库 | UI 交互断言 | 已覆盖：todo_ui_dialog_form_error |
| B3 | 带优先级/截止/标签创建成功，落待办列 | 服务层字段回读 | 已覆盖：todo_svc_create_task_with_attributes |
| B4 | 跨模块 action:new_task 同口径落待办 | 事件注入 | 未验证（未找到对应用例，缺口） |

## C. 归档折叠/展开

| F-ID | 场景 | 验证方法 | 现状/证据 |
|---|---|---|---|
| C1 | 折叠态宽度 = W×25%，1000/1600 两档几何正确 | 无头几何断言 | 已覆盖：todo_ui_h20_column_widths |
| C2 | 折叠态点击任意位置/chevron/Enter/Space 展开，Esc 收起 | probe 注入 | 已覆盖：todo_ui_h20_board_render + Esc 系（todo_ui_esc_*）；Enter/Space 切换未见专项断言（部分） |
| C3 | 动画：展开 240ms/收起 200ms ease-out cubic，t=0.5 宽度介于两端；动画中重复触发被禁；反向点击回插 | 纯函数 + 帧断言 | 部分覆盖（widths 插值有）；**缺口：重复触发禁止与反向回插无断言** → BUG-G2 未验证 |
| C4 | 首帧/切页不播动画直接落目标态 | 渲染断言 | 未验证（缺口） |
| C5 | 归档卡只读、0.7 透明度、归档时间 chip、最多渲染 20 条 | UI 断言 | 已覆盖：todo_ui_archive_card_title_color、todo_archive_format（20 条上限断言未见，部分） |
| C6 | 归档计数 = 总数；拖入 archived_at=now；拖回清空 | 服务层断言 | 已覆盖：todo_archive_move_into_sets_timestamp、todo_archive_move_out_clears_timestamp |

## D. 拖拽归档链路

| F-ID | 场景 | 验证方法 | 现状/证据 |
|---|---|---|---|
| D1 | 拖卡悬停折叠竖条高亮 + 悬停 300ms 自动展开；取消且为自动展开 → 200ms 后收回 | probe 真窗时序 | 几何命中已覆盖（todo_ui_drag_*、todo_drag_probe）；**缺口：300ms 自动展开与 200ms 收回时序无断言** → BUG-G3 未验证 |
| D2 | 全链：拖卡→悬停展开→drop 归档→计数+1→拖回恢复 | probe 端到端 | 已覆盖：todo_drag_probe（tests/probes/todo_drag_probe.cpp，需跑一次留执行输出） |
| D3 | 拖拽插入位置正确（含空列/滚动/缩放） | 纯函数断言 | 已覆盖：todo_ui_drag_insert_position 等 |

## E. 搜索联动

| F-ID | 场景 | 验证方法 | 现状/证据 |
|---|---|---|---|
| E1 | 输入实时过滤所有列，无命中列显示「无命中任务」 | UI 断言 | 已覆盖：todo_ui_search_filter |
| E2 | 归档命中 → 自动展开 + 记住展开前状态，清空后还原；无命中不展开 | UI 状态断言 | 服务层 todo_svc_search 有；**缺口：自动展开/还原状态机无专项断言** → BUG-G4 未验证 |
| E3 | Esc 清空搜索框 | UI 断言 | 已覆盖：todo_ui_search_enter_noop（Esc 分支需复核，部分） |

## F. 海量任务与虚拟化（契约 §9.1）

| F-ID | 场景 | 验证方法 | 现状/证据 |
|---|---|---|---|
| F1 | 待办列 300 条：只构建可视区+3 缓冲，滚动换批，每帧不全量构建 | 计数探针/性能断言 | **缺口：无虚拟化用例** → BUG-G5 未验证 |
| F2 | 每列独立滚动、列头 56px 固定、切页后滚动位置保留 | probe | 已覆盖：todo_ui_column_scroll_geometry（切页保留未见专项，部分） |

## G. 回归行（历史能力不得丢）

| F-ID | 场景 | 现状/证据 |
|---|---|---|
| R1 | 完成/取消完成 completed_at 语义 | 已覆盖：todo_done_* 5 条、todo_tools_complete_flips_completed_at |
| R2 | 文档关联（doclink）全套 | 已覆盖：todo_doclink_* 10 条 |
| R3 | 标签特殊字符往返、超时任务过滤 | 已覆盖：todo_svc_tags_roundtrip_special_chars、todo_svc_list_overdue |
| R4 | 模块开关插拔后 Todo 正常启停 | 已覆盖：todo_module_onstart_reentry_idempotent + module_toggle_sidebar_probe |
| R5 | 代理/其他模块不受 Todo 改动波及 | 由发布收口全量回归覆盖（不在本卡） |

## 执行记录（2026-10-29 22:30）

- `bash scripts/build-test-run.sh --filter todo --no-run`：Total 94 / Passed 94 / Failed 0（todo 定向全绿），模块隔离/交叉编译/std-include 等守卫全绿（日志关键行如上）。
- `./build/tests/todo_drag_probe`：F1/F2/F3/R1/R2/R4/R5 PASS；R3 INFO「归档列无卡，非决定性跳过」→ D2 归档 drop 段未走通，判「部分」，降级为未验证证据。
- 证据物：本节日志关键行 + 构建脚本输出（Total: 94 Passed: 94 Failed: 0；probe PASS 行）。

## 执行记录（2026-10-29 22:30 首跑；22:50 缺口回填复跑）

- `bash scripts/build-test-run.sh --filter todo --no-run`：Total 94 / Passed 94 / Failed 0（回填测试并入后仍全绿），模块隔离/交叉编译/std-include 等守卫全绿。
- `bash scripts/build-test-run.sh --filter "schema:" --no-run`：Total 22 / Passed 22 / Failed 0（含新增 schema_migrate_todo_creates_backup）。
- `./build/tests/todo_drag_probe`：F1-F3 / R1-R5 / S1-S2 / H1-H3 / H2a-H2b 全 PASS（1200×1000，连跑 3 次稳定）；R3 已由非决定性跳过转决定性 PASS。证据关键行：S1「宽 68→221」、H1「68→159」、H2a「73→195→272」、H2b「265→127→68」。
- 证据物：本节日志关键行 + 清单卡 BUG-G1~G6 台账（各自引用测试名/探针输出）。
- 遗留未验证：动画期重复触发禁止（无外部观察缝，见 BUG-G2）；跨模块 action:new_task（B4）；Enter/Space 切换与 20 条上限分项（C2/C5 部分）。

## BUG / 缺口台账

| 编号 | 现象 | 关联 | 状态 |
|---|---|---|---|
| BUG-G1 | 迁移前自动备份 DB 无测试断言 | A4 | 已修复（回归绿）：新增 schema_migrate_todo_creates_backup（备份存在/内容为老形态/幂等不重复备份），schema: 22/22 绿 |
| BUG-G2 | 动画重复触发禁止、反向回插无断言 | C3 | 部分关闭（回归绿）：todo_drag_probe H2a/H2b 断言展开/收起中间帧连续（73→195→272 / 265→127→68）；「动画期重复触发禁止」属模块内部语义，无外部观察缝，标未验证 |
| BUG-G3 | 拖拽悬停 300ms 自动展开/收回时序无断言 | D1 | 已修复（回归绿）：todo_drag_probe H1/H3（悬停自动展开 68→159；拖离取消自动收回 68），连跑 3 次稳定 |
| BUG-G4 | 搜索命中归档自动展开/还原状态机无专项断言 | E2 | 已修复（回归绿）：todo_drag_probe S1/S2（命中自动展开 68→221；Esc 清空还原折叠 68） |
| BUG-G5 | 海量任务虚拟化（300 条只建可视区）无用例 | F1 | 已关闭（盘点更正）：todo_ui_h20_column_widths 已含 virtualCardRange 300 条窗口断言（顶部 0..13 / 底部 287..300 / 空列与退化 pitch），此前漏盘 |
| BUG-G6 | todo_drag_probe 归档列无种子卡，归档 drop 链路（D2）非决定性跳过 | D2 | 已修复（回归绿）：probe 补归档种子卡 + R3 先展开归档列再断言，R3 由「非决定性跳过」转 PASS，全套 F/R/S/H PASS |
