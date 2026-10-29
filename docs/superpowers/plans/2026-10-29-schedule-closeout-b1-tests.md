# 测试清单卡：调度线收口批次 1（三工作流合并）实操验收

- 日期：2026-10-29
- 角色：测试（qa）
- 范围：TODO §B 第 1/3/4/5/6 条（A=引擎接线+全局令牌桶；B=toast 桥+wecom 注册；C=设置页定时开关卡）。
  **不含**第 2 条（运行历史抽屉）、第 7 条（DST 用例）。
- 证据根：`DevDesk/evidence/20261029-sched-b1-*`（f08/f09f10 为 evidence wrapper 目录，其余为归档日志，均 git-ignore）
- 全量基线：**Total: 1932 Passed: 1926 Failed: 0**（2026-10-29 本机实测，exit 0）——与预期一致
- 硬纪律：验收硬标准 = 启动程序实操 + 证据物；无证据判「未验证」。

| F-ID | 流程（验证什么能力） | 验证方法与证据物 | 结果 |
|---|---|---|---|
| F-01 | 全量独立复跑：构建零错 + 测试套整体绿（含本批 5 条新用例进套） | `bash scripts/build-test-run.sh --no-run` → `evidence/20261029-sched-b1-f01-full-run.log`；原文 `Total:  1932 Passed: 1926 Failed: 0`，exit=0；UI probe 21 项中 5 项 wallpaper 家族设计内跳过 | **已验证** |
| F-02 | A 域 3 条新用例真实存在且绿 | `--filter 调度引擎` → 13/13/0（`evidence/20261029-sched-b1-f02-filter-engine.log`）；逐条单核（harness 直跑，各 1/1/0）：`全局投递桶把同刻`（全局桶节奏 18 条/分钟）、`不连坐`（run_task 不受桶累）、`挂泵驱动引擎`（接线启停幂等） | **已验证** |
| F-03 | B 域 2 条新用例真实存在且绿 | `--filter 渠道` → 15/15/0（`evidence/20261029-sched-b1-f03-filter-channels.log`）；逐条单核各 1/1/0：`五内置 id 就位`（`channelsFactoryKnownAndUnknownIds` 内 wecom 工厂非空断言）、`未装配零 UI`（`channelsToastDefaultSinkViaBridge` 未装配零 UI / 装配转发） | **已验证** |
| F-04 | schedule 生命周期与 gating 定向轮 | `--filter dispatchModuleToggle` 3/3/0（f04a）；`--filter gating` 14/14/0（f04b）；`--filter 模块管理卡` 1/1/0（f04c，= `settings_modules_card_renders` 7 行）；`gatingKey` 单核 3/3（含 `assembly_registry_table_shape` 14 行注册表） | **已验证** |
| F-05 | 启动实操：隔离数据根 + 裸二进制（非 .app bundle）；onStart 生效 + 引擎 1Hz 泵在跑；≥30s 无崩溃；无 wecom 外发 | 4 次启动 `DEVDESK_DATA_ROOT=$(mktemp -d)` + `build/DevDesk`：启动行 `schedule 模块启动（渠道 4 / 系统来源 0，调度引擎 1Hz 已挂泵）`（schedule_module.cpp:60）；**引擎行为实锤**：插 60s interval 探针规则 → 80s 内 schedule_run 2 行（间隔精确 60s，status=success，run_count=2）；35s/80s 存活；wecom/qyapi 四份日志 0 命中 → `evidence/20261029-sched-b1-f05-app*.log` + `f05f07-runtime-summary.log` | **已验证** |
| F-06 | MCP server 健康（8931 口径） | curl initialize → HTTP 000（connection refused）；根因（静态）：MCP 默认关闭（settings_config.h:54）且 `app::mcp::start` 唯一调用点在 `dispatchModuleToggle`（app_assembly.cpp:309，设置页拨开关），**无启动期自启路径**——数据通道写 enabledModules 含 mcp 也只注册页面不启服务。非本批缺陷，设计内默认态 | **未验证**（需 UI 拨开关；复跑：设置页开 MCP 后 `curl -X POST http://127.0.0.1:8931/` initialize + tools/list）→ `evidence/20261029-sched-b1-f06-mcp-probe.log` |
| F-07 | 退出码干净：无 terminate/崩溃行 | 单 shell 内启动→SIGTERM→wait：exit=143（128+15 SIGTERM 常规终止，非 crash）；四份日志 terminate/abort/Segmentation 共 0 行 → `evidence/20261029-sched-b1-f07-app3-exit-code.log` | **已验证** |
| F-08 | 模块隔离静态守卫全过 | `bash scripts/check-module-isolation.sh` → 退出码 0，守卫绿 → `evidence/20261029-sched-b1-f08-isolation/` | **已验证** |
| F-09 | B 反向依赖修复真伪 | `grep -rn app_sidebar src/module_schedule/` 零命中（exit 1）；`toast_channel.cpp:19` include `toast_notice_bridge.h`（渠道层不再依赖 app 层）→ `evidence/20261029-sched-b1-f09f10-static/` | **已验证** |
| F-10 | 「flushAllToMainThread 此前零调用点、现 tick 末尾补」真伪 | grep src/：声明 channel_registry.h:103、定义 :88、**唯一调用点 schedule_engine.cpp:152**（tick 末尾，fireRule 循环 + dueFires_.clear() 之后），与声称一致 → 同上证据目录 | **已验证（声称属实）** |
| F-11 | 默认态安全：schedule 表空 + 无外发 | 隔离数据根 sqlite：schedule_rule / schedule_channel / schedule_run / schedule_delivery 全 0 行（探针插入前）；探针投递零外发（channel_ids=[]，wecom 0 命中）→ `evidence/20261029-sched-b1-f11-default-state.log` | **已验证** |
| F-12 | 交互/UI：设置页「定时」开关卡两态端到端 | 点击/输入类不可自动化 | **未验证（交互链路）**；替代证据（无头）已绿：`settings_modules_card_renders`（7 行）1/1 + `assembly_registry_table_shape`（schedule 行含 kModuleIdSchedule toggle id）3/3。复跑命令：`bash scripts/build-test-run.sh --filter 模块管理卡`；端到端需用户真机点验 |

## 缺陷登记

| BUG-n | 现象 | 关联 F-ID | 状态 |
|---|---|---|---|
| BUG-1 | `schedule_module.cpp:60` 启动日志硬编码「渠道 4」，实际 registerBuiltinChannels 注册 5 渠道（fullscreen/toast/os/silent/**wecom**，`channelsFactoryKnownAndUnknownIds` 五内置 id 断言绿）；wecom 接线后文案未更新，误导排障 | F-05 | **开放（移交 developer）**：一行文案修复（「渠道 5」或动态计数），修复后回归 `--filter 调度引擎` + 启动日志复查 |

## 结论

**完成（就本批 5 条 TODO 项的验收而言）**：F-01~F-05、F-07~F-11 共 10 项已验证带证据物；F-06（MCP server 需 UI 拨开关，非本批改动域）、F-12（点击交互链路，具无头替代证据）2 项如实未验证并附复跑命令。BUG-1 为一般级文案缺陷，开放待修，不阻塞本批功能验收，但按纪律悬空 bug 未终态前迭代收口需挂账。
