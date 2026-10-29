# 置顶 BUG 修复（全屏提醒弹窗门控）定向回归清单卡

- 日期：2026-10-29
- 被测：dev `09d8aa62`（17 文件）——上线提醒弹窗总开关默认关 + fullscreen 渠道默认关 + 先红后绿守卫两条
- BUG 背景：老板真机复现「主窗无法最小化 + 置顶」；根因=上线提醒窗全屏置顶盖住主窗红绿灯拦截点击（非窗口状态坏）
- 派单人：coderstory

| F-ID | 流程 | 验证方法与证据物 | 结果 |
|------|------|------------------|------|
| F-G1 | 默认配置启动（开关关）不弹全屏提醒窗 | 隔离 DATA_ROOT 启动 + DEVDESK_RELEASE_DEMO=red + 日志关键行 | 绿（日志级；截屏环境受限未取得，如实记录） |
| F-G2 | 拨开关后开窗/关窗 + 5 分钟冷却 | DB 注入 releaseReminderPopup=true 重启 + 日志；冷却逻辑由用例覆盖复跑核验，5 分钟全时长未实测（如实记录） | 绿（开窗路径核验） |
| F-G3 | fullscreen 渠道默认不弹 | 守卫用例 `channelsFullscreenPopupGate` 复跑 | 绿（gate 定向 4/4） |
| F-G4 | 回归行：release + schedule 域定向用例 | `--filter release` / `--filter schedule` 复跑 Total 行 | 绿（60/60、86/86） |
| F-G5 | fullscreen 渠道设置入口未做（遗留项） | 记账不追（coderstory 拍板） | 记账 |

## 结果记录（2026-10-29 17:35 收口）

- **F-G1**：隔离 `/tmp/dd-qa-rel-g1` + `DEVDESK_RELEASE_DEMO=red` 启动（PID 33093），日志 `logs/devdesk.log:35`、`:42` 两条「Release 提醒：弹窗开关关闭，跳过开窗（到期项 4 条）」（60 秒节流，`release_reminder.cpp:495`）——演示模式 4 条到期项仍不开窗，门控生效。截屏因 agent 会话无屏幕录制 TCC 授权未取得（`could not create image from display`），以日志铁证判绿。
- **F-G2**：`app_config` 注入 `config.json:releaseReminderPopup=true` 后重启（PID 33225）：「跳过开窗」行不再出现、零「句柄丢失/开窗请求超过」WARN（fail-dead 路径未触发）= 开窗请求正常发起且句柄确认成功。冷却逻辑由 `shouldOpenReminder`（`release_reminder_logic`）单测与守卫覆盖，5 分钟全时长未实测。
- **F-G3/F-G4**：`--filter gate` 4/4、`--filter release` 60/60、`--filter schedule` 86/86，全部 exit 0（job `bash-c7ba763abd2f46e1` / `bash-ede26b3074af4edb`）。
- **总判定**：完成——BUG-1（主窗无法最小化+置顶）随 `09d8aa62` 修复，回归全绿，**销账**。

## BUG 台账

- BUG-1：主窗无法最小化+置顶（老板真机）→ **已修复，回归绿，销账**（`09d8aa62`）。
