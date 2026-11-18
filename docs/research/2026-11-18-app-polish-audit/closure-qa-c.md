# 批次 C 收口 QA（closure-qa-c）

- 日期：2026-11-18｜角色：qa-tester｜池条目：need:294｜本机 macOS
- 命令：`bash scripts/build-test-run.sh --no-run`（全量档，未带 --dev）

## 总判定：阻断（增量相关 1 红，转 developer 修复后重跑全量再收口）

## 一、全量数

| 门禁 | 结果 | 对照基线（今日上一批收口） |
|---|---|---|
| doctest 全量 | Total 2462 / Passed 2452 / **Failed 1** | 2462 / 2453 / 0（总数持平 ✓，新增 1 红 ✗） |
| UI probe（ctest -L probe，脚本中止后补跑） | 100% passed out of 52；跳过 6：b1_material、memory_install、wallpaper_desktop / image_rotation / rotation / yield | 52/52（5 设计内跳过）；本轮列到 6 条，名单如实记录，差 1 待下次收口核口径 |
| 守卫链·模块隔离 | EXIT=0 | 全绿 ✓ |
| 守卫链·一致性检查 | OK / EXIT=0 | 全绿 ✓ |

脚本链在 doctest 红处按设计中止（probe/守卫段未随跑），两项由 qa 按 build-test-run.sh 同口径补跑，非历史数据。

## 二、唯一红（归属=阻断）

- 用例：`ui_kit_empty_state_minimal_text_with_nearby_action`（「ui_kit emptyState: 文字灰(#666666) + 就近按钮 Secondary 级」）
- 归属：**与 C2（130e5136）直接相关**。C2 把 Secondary 常态底 hover→surfaceElevated（`src/ui_kit/button.cpp` Secondary 分支「normal=surfaceElevated / hover=hover / pressed=active」）；断言 `tests/test_modules/test_ui_kit_feedback.cpp:170` 仍钉旧契约 `t.colors.hover`（注释「Secondary bg = hover #F3F4F6」）。
- 为什么 C2 定向绿没拦住：该用例名不含 "button"，C2 定向 filter（button 22 例）扫不到——定向 filter 按描述匹配的已知缝隙。
- 分类：测试缺陷（断言未随 kit 契约同步）。修复=断言改钉 `t.colors.surfaceElevated`，一行；归 developer（qa 禁修）。

## 三、专项 1：白卡 Secondary 常态可见性（亮主题 4 页判读）

**判定：消失态确认**——`surfaceElevated=#FFFFFF`（`src/core/theme.cpp:83`）与白卡同值，常态底不可辨。

| 页面 | 钮 | 宿主 | 判定 |
|---|---|---|---|
| 记忆·页头 | 首次安装向导 | 白卡条 | **消失态**：仅文字+极弱圆角可感知 |
| 记忆·脱敏间卡 | 跑评测 | 白卡 | **消失态**，同上 |
| 壁纸·暂停横幅卡 | 下一个 | 白卡 | **消失态**，同上 |
| 上线任务·空态 | 新建任务（Secondary） | 页面浅色底（非白卡） | 可辨（白钮落淡绿底有对比） |

- 附注：todo 看板列空态为**纯文字变体**（无按钮露出，非消失态；页头「新建任务」为 Primary 不涉）。
- 处置（转 developer）：消费方局部加 1px borderDefault 描边，不动 kit。锚点：module_memory 页头「首次安装向导」、memory 脱敏间卡「跑评测」、module_wallpaper 横幅卡「下一个」。empty_state 全站钮仅在宿主为白卡时受影响（empty_state.cpp:122）。

## 四、专项 2：暗色 Secondary/Danger 三态

- 判定：**部分验证**。驱动无 hover/pressed 原语（`tests/ui_driver/driver_steps.h` 原语清单：click/type/key/scroll/drag/wait/assert/screenshot/dump/quit），按简报降级为常态对照。
- 暗档 18 页已落盘（`UAT62_DARK=1`）；已判读 dark/04-todo：暗色渲染生效，该页无 Secondary/Danger 露出（空态列纯文字）。Secondary/Danger 暗档常态图 dark/08-memory、dark/13-release_tasks 落盘未逐图判读（预算截止），如实标注。
- 暗档 settings tab1/tab3 两帧「白底空帧」判据失败：探针已知静态 init 竞态（`tests/probes/uat62_page_shots_probe.cpp:236` 注释，「批7 raw-dark 实拍为亮色即此因」）——测试侧环境限制，非 app 缺陷。

## 五、证据物

- 根：`evidence/2026-11-18-need294-closure/`
  - `light/`：亮主题 18 页（04-todo / 08-memory / 09-wallpaper / 13-release_tasks 已判读）
  - `dark/`：暗主题 18 页（04-todo 已判读；15-settings-tab1/tab3 附 -debug.png）
- 日志：doctest 全量输出裁尾见构建日志（Total 2462 / Failed 1；FAIL 摘要单行）；模块隔离 `/tmp/iso.log`、一致性 `/tmp/cons.log`（本轮会话临时件）
- 工具调用：35 / 35（预算打满，到点停手）

## 结论一行

全量 2452/2462——唯一红=emptyState 旧断言（钉 hover 未随 C2 改 surfaceElevated 同步，定向 filter 缝隙），阻断转 developer 一行修复后重跑全量；专项 1 白卡消失态确认（memory×2、wallpaper×1，消费方描边处置）；守卫链双绿、probe 52/52；专项 2 部分验证（驱动无 hover 原语，常态图已落盘）。
