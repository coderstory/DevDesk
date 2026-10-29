# 测试清单卡：B2 运行历史抽屉接线 + DST 真跑 + memcpp 审核工具条 合并实操验收

- 日期：2026-10-29
- 角色：测试（qa）
- 范围（三组合并验收）：
  1. 运行历史抽屉接线（schedule_ui_list 按钮 → 抽屉打开/关闭/空态，2 条新用例）
  2. DST 用例真实覆盖（test_schedule_rule 两条 DST 用例从假绿转真跑，ScopedTz）
  3. memcpp 审核工具条窄窗重叠修复（1200/1000/1600 三档布局断言，3 条新用例）
  附带：引擎链路实操稳定性（R1 修复代理并行改 schedule_engine.* / action_runner.cpp 期间）。
- 证据根：`DevDesk/evidence/20261029-sched-b2-qa/`（git-ignore）
- **并行状态声明（重要）**：修复代理 R1 在验收窗口内并行修改 `src/module_schedule/schedule_engine.*`
  与 `action_runner.cpp`。时间线：首轮全量/定向/运行时实操（10:02–10:18）跑在 R1 早间改动（≤08:13）
  之上；R1 于 **10:23–10:26 又落地一批改动**，本 QA 等其安静 224s 后于 10:30 **重跑全量**（总数
  1937→1943，R1 落 6 条新用例），并将三轮定向、DST 直跑、引擎运行时实操（instance6）全部
  **刷新到 R1 最终二进制**。所有「已验证」判定均以最终态证据为准；首轮证据保留作过程记录。
  瞬时红：未出现（两轮全量均 0 失败，无需触发重跑判据）。
- 硬纪律：验收硬标准 = 启动程序实操 + 证据物；无证据判「未验证」。

| F-ID | 流程（验证什么能力） | 验证方法与证据物 | 结果 |
|---|---|---|---|
| F-01 | 全量独立复跑：构建零错 + 套件整体绿（预期 ≥1937 / 0 失败） | 首轮 `bash scripts/build-test-run.sh --no-run`：`Total:  1937 Passed: 1931 Failed: 0`，exit 0（`f01-full-run.log`）；**R1 落地后收口轮**：`Total:  1943 Passed: 1937 Failed: 0`，exit 0（`f01-full-run-final.log`）——0 失败硬标准在收口时点成立 | **已验证** |
| F-02 | 历史抽屉定向轮：接线 2 条（list：开抽屉装载两表/关闭收起 + 空态不崩）+ 装配 4 条（history）= 6/6 | 脚本轮 `--filter 历史抽屉` → `Total:  6 Passed: 6 Failed: 0` exit 0（`f02-filter-drawer.log`）；直跑逐用例 6 PASS + `Skipped: 0`（`f02-drawer-direct-run.log`）；**R1 最终态复刷** 6/6/0 全 PASS（`f02f03f04-final-direct-runs.log`） | **已验证** |
| F-03 | DST 定向轮：rule 2 条 + core.time 1 条 + agent_draft 1 条 = 4/4 且 Skipped: 0 | 脚本轮 `--filter DST` → `Total:  4 Passed: 4 Failed: 0` exit 0（`f03-filter-dst.log`）；直跑逐用例 4 PASS + **`Skipped: 0`**（`f06-dst-direct-run.log`）；**R1 最终态复刷** 4/4/0 + `Skipped: 0`（`f02f03f04-final-direct-runs.log`） | **已验证** |
| F-04 | 审核工具条定向轮：1200/1000/1600 三档布局断言 = 3/3 | 脚本轮 `--filter 审核工具条` → `Total:  3 Passed: 3 Failed: 0` exit 0（`f04-filter-toolbar.log`）；直跑逐用例 3 PASS + `Skipped: 0`（`f04-toolbar-direct-run.log`）；**R1 最终态复刷** 3/3/0（`f02f03f04-final-direct-runs.log`） | **已验证** |
| F-05 | 启动实操 + 引擎链路：隔离数据根 + 裸二进制 `build/DevDesk`（非 .app bundle）；60s interval 探针规则（remind / channel_ids=[]），schedule_run 产生行、planned_at 间隔 60s、status=success、零外发；日志无异常堆栈/terminate；退出码干净 | 共 6 实例：instance1（预 R1-final，1 行 success，next_fire−last_fire=60s）；**instance4**（预 R1-final，**3 行 run @10:16/10:17/10:18:00，实算 delta=60/60s，全 success，存活 160s，SIGTERM exit 143，零崩溃行**，`f05-runtime-summary-inst4.log`）；instance2/3/5 被外部优雅关停（exit 0、模块有序 stop、零崩溃行——判真人关窗，见 BUG 表下方环境备注）；**instance6（R1 最终二进制）：2 行 run @10:40/10:41:00，实算 delta=60s，全 success「已投递 0 个渠道」，存活全程，exit 143，零崩溃行**（`f05-runtime-summary-inst6-final.log`）。启动行 `schedule 模块启动（渠道 5 / 系统来源 0，调度引擎 1Hz 已挂泵）` | **已验证**（终态 = R1 最终二进制） |
| F-06 | DST 真跑证明：两条 DST 用例 PASS 原文行 + Skipped: 0；对比 HEAD 假绿形态 | 直跑原文：`[PASS] schedule: DST 春季缺口在最早存在时刻触发（§9.10 春季） [3ms]`、`[PASS] schedule: DST 秋季回拨重复墙钟只第一次（§9.10 秋季） [1ms]`，Summary `Skipped: 0`（`f06-dst-direct-run.log`）；HEAD(242e03bd) 假绿形态佐证：`if (!zoneHasDst()) return true;` 等静默 return true 共 7 处（`f06-head-fakegreen.txt`，git show 只读）；工作区版改为 `ScopedTz("America/New_York")` 真跑 + `DOCTEST_SKIP` 显式跳过（test_schedule_rule.cpp:364-366/393-395）——假绿洞已闭 | **已验证** |
| F-07 | 审核工具条无 GUI 目检：memory_probe 真窗渲染截屏，目检 1200/1600 逻辑档 chips 不重叠、批量开关右锚 | `build/tests/memory_probe --size <W>x<H> --out …`：首轮误用物理像素口径（见 BUG-1）后改传 `2400x2000`/`3200x2000`（=1200/1600 逻辑×2x Retina）真窗截屏，probe 结构断言 PASS exit 0；目检 `f07-logical-1200/memory-review.png`、`f07-logical-1600/memory-review.png`：chips 全部单行（全部/合并/失效/遗忘/新增/全部来源/Claude 对话/整理/遗忘扫描/待审/已处理），批量模式+开关右锚，无重叠无裁切；png 右侧白边为 FrameCapture 读满 framebuffer 的捕获伪影，不影响布局判定 | **已验证**（自动化截屏档；人手拖窗真检归 F-08） |
| F-08 | 交互链路：抽屉真实点击开/关、真窗手工缩放目检工具条 | 点击/拖拽类不可自动化 | **未验证（交互链路）**。替代证据已绿：接线/空态/装配 6 用例（F-02）+ 三档几何断言 3 用例（F-04）+ probe 真窗截屏（F-07）。复跑命令：`cd /Users/coderstory/dev/gui/DevDesk && bash scripts/build-test-run.sh --filter 历史抽屉`、`--filter 审核工具条`；端到端点验需用户真机操作 |
| F-09 | 附带：模块隔离静态守卫（R1 动过引擎/渠道文件后的回归保险） | `bash scripts/check-module-isolation.sh` → exit 0「✅ 模块隔离守卫通过」→ `f09-isolation.log` | **已验证** |

## 缺陷登记

| BUG-n | 现象 | 关联 F-ID | 状态 |
|---|---|---|---|
| BUG-1 | `memory_probe` 的 `--size` 语义为**物理像素**（memory_probe.cpp:178-183 注释自述「窗口像素尺寸 = 物理像素」并除以 contentScale），而 probe_harness.h:282 的 pollOnce 注释声称「逻辑 = 物理」——两套口径并存易误用。实测 `--size 1200x1000` 在 2x Retina 机实际按 **600×500 逻辑宽**布局，低于 app 最小窗 1000×640，chips 出现叠字/裁切，险些误判为修复失效（首轮截图 `f07-toolbar-probe-1200/` 留证）。属测试基建缺陷（致误判），非被测功能缺陷 | F-07 | **开放（移交 developer，一般级）**：建议 `--size` 帮助文本/启动输出显式打印「请求 WxH → 实际逻辑 w×h」或统一口径；修复后回归：`./build/tests/memory_probe --size 2400x2000 --out <dir>` 目检逻辑宽=1200 |
| —— | 环境备注（非缺陷）：验收窗口内 3 个实例（10:08:26 / 10:11:16 / 10:36:40）在观察窗内被外部**优雅关停**（exit 0、窗体记忆保存、模块有序 stop、零 terminate/崩溃行；其中 10:36 时点 R1 已安静 10 分钟，排除并行构建 kill_by_exe_path 命中——本 QA 以相对路径 `./build/DevDesk` 启动亦不匹配 pkill 绝对路径判据）。判机器前真人关窗，不影响任何功能判定 | F-05 | **已忽略**（环境干扰，理由如左；复跑遇同象按同判据处理并重试一次） |
| —— | 跨批次核对：B1 清单卡 BUG-1（启动日志硬编码「渠道 4」与实际 5 渠道不符）现状：schedule_module.cpp:60 已改「渠道 5」，与 `channelsFactoryKnownAndUnknownIds` 断言的 5 内置渠道一致，不再误导（仍为字面量，非动态计数，可接受） | （B1 遗留） | **已修复**（文案核对一致；B1 卡可销账） |

## 结论

**完成（三组合并验收口径）**：F-01~F-07、F-09 共 9 项已验证带证据物；F-08（点击/拖拽交互链路）如实未验证并附复跑命令与替代证据。无致命/严重缺陷；BUG-1 为一般级测试基建缺陷，开放移交 developer，不阻塞本批功能验收。收口时点（R1 最终落地并安静 224s 后）：全量 `Total:  1943 Passed: 1937 Failed: 0`、exit 0，三轮定向 6/6、4/4（Skipped: 0）、3/3，引擎链路在 R1 最终二进制上实操复证（60s 间隔、success、零外发、退出码干净）。
