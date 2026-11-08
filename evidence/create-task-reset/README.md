# BUG 212 before/after 证据（创建任务弹层重开残留旧日期）

- 复现/回归用例：`tests/test_modules/test_release_task_ui.cpp` 的
  `release_new_task_reopen_resets_stale_session`（三条路径：新建→编辑 B、
  创建完→再新建、选日期→取消→再新建）。
- before-red.txt：仅还原 `src/module_release/release_dialogs.cpp` 修复（git stash push），
  定向跑用例 → 红。红点 = 重开弹层后 `dialog.planDate` 残留上一次会话值
  `2020-06-02`，再点「创建任务」触发同日复用（`findTaskByDate` 命中旧任务 →
  OpenExisting → openTask 打开上一个任务执行页）。
- after-green.txt：恢复修复后 `--dev --filter release` 全量定向 → 112/112 全绿
  （含本用例与既有同日复用、作废重建等用例，无回归）。
- 修复：`release_dialogs.cpp` 打开沿（open false→true）把日期锚点重置为今天
  （setToday），替代原先「保留调用方预填 planDate」的分支（现无调用方预填，
  全仓 grep 证实）。环境选择/确认态/缓存本就在打开沿清空，不变。
- 复现时间：2026-11-07（macOS arm64，构建入口 bash scripts/build-test-run.sh --dev）。
