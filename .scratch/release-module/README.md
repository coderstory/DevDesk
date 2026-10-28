# 上线模块交付闸门（release-module）

## 文件

| 文件 | 作用 |
|------|------|
| `checklist.json` | 交付清单，唯一真相源。`items[]` 每项：`id` 稳定编号；`behavior` 用户视角的一句行为（不按文件/层次拆）；`status` = `done`（已验证可用）/ `todo`（未做）/ `degraded`（只能降级交付，仍算未完成、会阻断）；`evidence` 为 `done` 必填（怎么验的：命令、用例、sha、真机）；`note` 为 `degraded` 必填（缺哪一块、为什么） |
| `active` | 开关文件。存在 = 闸门生效；不存在 = 完全放行（Stop 静默、SessionEnd 只记「未启用」） |
| `build.marker` | 构建标记。**每轮验证通过后** `touch .scratch/release-module/build.marker`。它只证明「跑过」，不证明「跑对」 |
| `session-end-report.md` | 每次会话结束自动覆盖写。**下次开工先读它**，按其中未完成项接续 |

## 判定（Stop 与 SessionEnd 共用同一段逻辑）

1. `active` 不存在 → 放行 / 无待办。
2. 有 `status != done` 的项 → 阻断，逐条列 `id + behavior + 当前 status`（`degraded` 也算未完成）。
3. 有 `status == done` 但 `evidence` 为空 → 阻断，写明「N 项声称完成但没有证据」。
4. 有 `status == degraded` 但 `note` 为空 → 阻断。
5. `src/` 或 `tests/` 有文件 mtime 晚于 `build.marker`（或 marker 缺失）→ 阻断，提示「改了代码但没有更新构建标记」。
6. 全部通过 → 静默放行（exit 0）。

`checklist.json` 的字符串值不要含英文双引号 `"` 或花括号 `{}`——脚本用 bash 内建解析，以避开 hook 环境不保证有 `jq` 的限制。

## Stop 是闸门，SessionEnd 是存档

- **Stop**：唯一能阻断的事件。不通过 → 输出 `{"decision":"block","reason":"…"}` 让主会话继续做。`stop_hook_active: true` 时直接放行并提示「闸门已跳过（防循环）」（Claude Code 会在 Stop 后再次触发 Stop，不防会死循环）。
- **SessionEnd**：不能阻断，只把同一判定结果写进 `session-end-report.md`（时间、未完成项、未验证改动、降级未说明项）。**无论如何 exit 0**：清单缺失、JSON 损坏、脚本自身出错都只写一行说明，绝不卡住会话结束。
- 两者是分工不是备份：Stop 管「现在能不能收工」，SessionEnd 管「下次从哪继续」。

## 启用 / 关闭

```bash
touch .scratch/release-module/active   # 启用闸门
rm .scratch/release-module/active      # 关闭闸门
```