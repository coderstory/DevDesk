# 供应商余量（quota）删除普查报告（只读调研，2026-10-20）

基线：dev `d5df6b63`（行号钉在该版；见文末漂移警告）。全仓 quota 命中 = git 跟踪文件 51 个（源码 32 + 测试 9 + 脚本 4 + 工具 1 + 文档 2 + CMake 登记）。`module_agent` 的「余量」仅是像素留白/注释（agent_module.cpp:154、:1396、agent_detail_dialog.cpp:327、agent_module.h:181），无功能引用。

## 一、删除清单表

| # | 文件 | 动作 | 理由 |
|---|---|---|---|
| 1 | `src/module_claude_proxy/quota/quota_store.{h,cpp}` (453/99 行) | 整删 | 落库唯一口，两表全功能专属 |
| 2 | `quota/quota_refresh.{h,cpp}` (239/99) | 整删 | 后台定时刷新线程 |
| 3 | `quota/quota_registry.{h,cpp}` (64/42) | 整删 | provider 显式注册表 |
| 4 | `quota/quota_volcano.{h,cpp}` (726/74) | 整删 | 火山方舟 provider |
| 5 | `quota/provider_minimax_cn.{h,cpp}` (296/13) | 整删 | MiniMax provider |
| 6 | `quota/deepseek_provider.{h,cpp}` (176/13) | 整删 | DeepSeek provider |
| 7 | `quota/opencode_go_provider.{h,cpp}` (220/13) | 整删 | OpenCode Go provider |
| 8 | `quota/quota_provider.h` (103) | 整删 | 契约层类型 |
| 9 | `quota/quota_credential.{h,cpp}` (116/64) | 整删 | 凭据来源缝 |
| 10 | `quota/quota_dialog_config.{h,cpp}` (320/96) | 整删 | 弹窗配置逻辑层 |
| 11 | `quota/quota_error_text.{h,cpp}` (48/15) | 整删 | 错误文案 |
| 12 | `quota/quota_percent.h` (14) | 整删 | 百分比裁剪 |
| 13 | `quota/quota_home_view.cpp` (182) | 整删 | 桥实现（listQuotaHomeRows 等） |
| 14 | **`quota/ccswitch_source.{h,cpp}` (251/58) + `quota/ccswitch_sqlite.h` (118)** | **【已拍板：迁出 quota/ 至 src/module_claude_proxy/ 根，namespace quota→ccswitch】** | 有非 quota 消费方：`ccswitch_import.cpp:6-7,26-34,58`（cc-switch 导入功能）与 `tests/live/agent_live.cpp:52,690-696`（真机测试凭据回落） |
| 15 | `src/module_claude_proxy/claude_ui_quota.{h,cpp}` (286/64) | 整删 | 弹窗绘制层 |
| 16 | `src/module_home/home_quota_card.{h,cpp}` (571/98) | 整删 | 首页余量卡 |
| 17 | `src/quota_home_bridge.h` (112) | 整删 | 中立桥 + devdesk::quota_bridge 总开关 |
| 18 | `src/module_claude_proxy/claude_module.cpp` | 删块：:13,26-29,33(include)；:58,66-74(开关联动)；:226(dv.quotaEnabled)；:276-280(画弹窗/收割)；:319-321(onStart 启刷新)；:324-343(onStop 恢复)；:357-358(停机序)；:388(modal 冲突判据)；:417(Esc 关)；:495(wiring)；:796-824(start/stopQuotaRefresh)；:828-876(openQuotaDialog+consume payload)；:879-946(测试连接/收割/保存)；:948-977(quotaHooks)；:1066-1085(成员) | 装配点 |
| 19 | `claude_ui_state.h` | 删块 :8(include),:196,:206 | QuotaDialogState 成员 |
| 20 | `claude_ui_detail.cpp` | 删块 :896-914（if (v.quotaEnabled){...} 整段） | 详情页入口按钮 |
| 21 | `claude_ui_detail.h` | 删块 :44(quotaEnabled),:67(quota 回调) | 入参/回调 |
| 22 | `src/module_home/home_module.cpp` | 删块：:12,14(include)；:229-231(缓存复位)；:527-552(整卡绘制段+hooks)；:579-584(图表高度改回 kHomeChartH)；:790-816,834-837(refreshQuotaRows+成员)。**【已拍板：矮窗滚动模式保留——min 窗即将降 1000×640 仍需滚动；computeHomeQuotaBudget 简化为与 quota 无关的纯高度预算】** | 首页装配点 |
| 23 | `src/module_settings/settings_module.cpp` | 删块：:40(include)；:96-97(卡高注册表 "settings.card.quota" Fixed 72.0f)；:204(apply 开关)；:381-387(applyQuotaEnabled)；:1097(wiring)；:1401 注释顺手清 | 设置页装配点 |
| 24 | `settings_config.cpp/.h` | 删块 cpp:35(kKey),404-408(读),464(写)；h:76(quotaEnabled) | quotaEnabled 字段 |
| 25 | `settings_ui_cards.cpp/.h` | 删块 cpp:750-797(drawQuotaCard 整函数)；h:61,:104 | 设置余量卡 UI |
| 26 | `core/schema.h` | 删块 :167-223（两表 DDL 与注释）。**【已拍板：选方案 B——删 DDL + applySchema 流程内新增幂等 DROP TABLE IF EXISTS profile_quota_config / profile_quota_result 一次性清理】** | 两表建表语句 |
| 27 | `core/sql_dump.cpp` :172 / `core/event_bus.h` :92 | 仅注释引用，顺手改 | 无代码依赖 |
| 28 | `scripts/check-quota-write-paths.sh`、`check-quota-shutdown-order.sh`、`check-probe-gate-labels.sh`、`run-quota-live-probe.sh` | 整删 | quota 专属守卫/入口 |
| 29 | `tools/quota_live_probe.cpp` | 整删 | 真机出网探针 |
| 30 | 文档：CLAUDE.md(12 处)、CHANGELOG.md(1 处) | 按口径更新 | 见第六节 |

## 二、调用图

```
home_module.cpp:527 ─→ quota_home_bridge.h ─→ quota_home_view.cpp(listQuotaHomeRows)
home_quota_card.cpp ──┘       ↑  feature flag(in quota_home_bridge.h:97-112)
                              │  requestQuotaImmediateRefresh → quota_refresh.cpp
claude_module.cpp:796-824 ─→ QuotaStore(quota_store.cpp) ← profile_quota_config/result 两表
        │                  ─→ ProfileCredentialSource(quota_credential.cpp) ─→ ccswitch_source.cpp(读 cc-switch)
        │                  ─→ QuotaRefresher(quota_refresh.cpp) ─→ quota_registry.cpp
        │                                              ├→ quota_volcano / deepseek / opencode_go / minimax providers
        └:828-977 弹窗链 ─→ claude_ui_quota.cpp ─→ quota_dialog_config / quota_error_text / quota_percent
settings_module.cpp:204,381 ─→ quota_bridge::setQuotaFeatureEnabled ← settings_config(quotaEnabled)
ccswitch_import.cpp:6-7 ─→ quota/ccswitch_sqlite.h + ccswitch_source.h   ←【非 quota 消费，保留（迁移）】
tests/live/agent_live.cpp:690-696 ─→ quota::listCcSwitchProviders        ←【非 quota 消费，保留（迁移）】
tools/quota_live_probe.cpp ─→ providers + quota_store                    【删】
```

## 三、测试影响清单

**整文件删（10 个）**：`test_claude_quota{,_ccswitch,_dialog,_minimax,_store,_volcano,_live_fixtures}.cpp`、`test_claude_quota_lifecycle.cpp`（内含 1 个 DOCTEST_SKIP :230）、`test_home_quota_card.cpp`、`test_probe_gate_labels.cpp`。
**删块**：

| 文件 | 位置 | 内容 |
|---|---|---|
| test_claude_ui.cpp（91 处命中） | :12,26,27 include；:205-222、:5702-5721 按钮几何断言；:7931-8277 quota 弹窗整段（helper :7946 + 7 个 REGISTER_TEST） | 弹窗/按钮 UI 断言 |
| test_settings.cpp | :305-366（quota 开关 3 用例）；:2994,3026,3070,3080（"settings.card.quota" 注册表断言） | 开关+卡注册 |
| test_schema.cpp | :81-82（表清单）；:660-729（schema_quota_tables_columns 用例） | 两表结构断言 |
| test_sql_dump.cpp | :28,43,124-126 —— isFtsShadowName 回归锚是通用逻辑测试，换表名保留（如 models_config） | 影子表误判回归 |
| test_main.cpp | :16,85,179-182（全局 scaffold bridge flag reset 第 15 项） | 测试基建 |
| tests/CMakeLists.txt | :88-96,98,111（源清单）；:242 注释；:354-357（quota_live_probe add_test+live 标签）；:361,416 注释 | 构建登记 |
| probe_harness.h | :242-245,260,278-279（--quota-fixture 参数） | probe 基建 |
| home_probes.cpp | 组 G(:697)/H(:1374)/I(:1082)/J(:799)/K(:1458)/L(:1590)/M(:1809) + quota 种子段 :337-483 部分 + :1437,:1801 收尾 DELETE；剩组 A-F 非 quota | probe 裁剪 |
| tests/live/agent_live.cpp | :52,690-696 | 依赖 ccswitch_source（迁移后更新 include/namespace） |

## 四、守卫面（不同步删必红）

- `check-quota-write-paths.sh` 本体当前 0 违规；构建日志 4 条「违规」是 build-test-run.sh:693-706 守卫自检段 `--self-test` 的**预期红输出**（坏树①2+③1+④1=4 条，:169-249）。删除 = 连删此脚本 + build-test-run.sh:621-627 调用 + :696 自检列表项 + **:694 `_GUARD_SELF_COUNT=7` 改 4**（check-probe-gate-labels.sh 也在自检环同批删，7−3=4）。
- `check-quota-shutdown-order.sh`：删 build-test-run.sh:641-647 + :697 自检项。
- `check-probe-gate-labels.sh`：断言「恰好 1 条 add_test(NAME quota_live_probe」→ 删 probe 后必红，同批删（build-test-run.sh:629-635 + :698）。
- `check-module-isolation.sh` D 段（:326-373）：删除后 home 自洽，断言仍绿但依据消失——按普查处置：断言与依据注释同步清，`src/module_home/CMakeLists.txt:16-31` 大段「不自洽」注释清理。

## 五、DB 与旧库（已拍板 B）

事实：schema 机制无版本化迁移链（schema.h:1-10：单份幂等 DDL、CREATE TABLE IF NOT EXISTS、无 schema_version 表；唯一补列机制 addMissingAgentColumns 仅 agent 三表）。**拍板：方案 B**——schema.h 删两表 DDL；applySchema 应用流程内新增小函数做幂等 `DROP TABLE IF EXISTS profile_quota_config; DROP TABLE IF EXISTS profile_quota_result;`（无 FK 指向、无 reader，老库启动即清）。

## 六、文档面

CLAUDE.md 12 处（项目概况「含供应商余量卡」、当前状态「供应商余量链全链闭环 G0-G8」整段、probe 段 home_probes 定位、已知欠账 3 条余量条目、.clip() 欠账条）；CHANGELOG.md 1 处。历史 specs/plans（2026-10-02 两份）与历史审计留痕**不回改**。

## 七、构建面

- `src/module_claude_proxy/CMakeLists.txt`：:11 claude_ui_quota.cpp；:19-30 quota/ 12 个 .cpp（ccswitch_source.cpp 迁出后原行改指新路径）
- `src/module_home/CMakeLists.txt`：:3 home_quota_card.cpp + :16-31 注释段
- `tests/CMakeLists.txt`、顶层 `CMakeLists.txt`（链接序断言依据消失，随 isolation D 段处置）

## 八、风险点

①ccswitch_source 跨用（已拍板迁移）；②守卫网连锁（第四节清单，漏改全量门禁必红）；③首页布局连带（已拍板：滚动保留、预算与 quota 解耦）；④链接顺序不变式作废（home 自洽，随 D 段清理）；⑤schema 兼容（已拍板 B）；⑥event_bus 无产品级 quota 事件（仅注释），无悬空 emit。

## ⚠ 行号漂移警告（主会话注）

普查行号钉在 d5df6b63；当前 dev=8dc6a8aa 已合入 Act（动了 CLAUDE.md、tests/CMakeLists.txt、home_probes.cpp、test_settings.cpp（+26 行 gating 断言在 :1167-1192，其后行号全漂）、test_claude_ui.cpp、home_module.cpp、markdown_module.cpp、app_sidebar.cpp、main.cpp）与同步双列（github_sync_card.cpp、settings_ui_logic.h、test_github_sync_card.cpp）。**一切以 grep 实际定位为准，禁止按本文行号盲删。**

## 建议批次

一个 worktree 原子批（改动强耦合，无法按层拆批），2 个 commit：①src + CMake + 4 守卫脚本 + build-test-run.sh + 全部测试/probe 裁剪；②CLAUDE.md/CHANGELOG 口径更新。约 45 文件。
