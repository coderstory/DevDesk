# DevDesk 交接（2026-10-19 暂停版）

> 取代 `$TMPDIR/devdesk-handoff-2026-10-19-final.md`（tmp 文件，随系统清理消失）。
> 用户指令现状：**暂停开发，不再派新子任务**。恢复须等用户明说。

## 当前状态（本文件落笔时）

- **交付态已达成并实测**（2026-10-19 上午，重跑后台链）：dev @ `d33643f`（树净、无 stash、未 push 任何远端）
  - 全量 `Total: 1626 Passed: 1620 Failed: 0`（差值 = 平台条件 DOCTEST_SKIP，非失败）
  - UI probe 全绿；7 道守卫 + 守卫自检全过（坏树全红、好树绿）
  - 终装：`/Applications/DevDesk.app` 4.4.6，ad-hoc 签名有效，二进制与 build 产物逐字节一致，特征串 `hot.list` 命中 3 行
  - 链日志（临时件，可能已清）：`/tmp/devdesk-gate-final-20261019.log`
- **暂停前派过一个子任务（拆分+详情弹窗布局）随即被用户叫停**，agent 死于基线测试段，**未写任何代码**
- 残留恢复点：worktree `~/dev/gui/wt-agent-split-20261019`（分支 `agent-split-task-20261019`，= d33643f，净，零提交）。恢复时可直接在其上重派；确认弃用则 `git -C ~/dev/gui/DevDesk worktree remove ~/dev/gui/wt-agent-split-20261019 && git branch -D agent-split-task-20261019`

## 恢复开发后的队列（按序，单个子任务串行）

1. **上帝类拆分**（依据 `docs/superpowers/plans/2026-10-19-agent-batch-audit-arch.md` F1 HIGH）：`src/module_agent/agent_module.cpp` 4193 行 → ① `agent_task_dialog.{h,cpp}`（composeTaskDialog + edit* 草稿字段聚 TaskDialogState）② `agent_detail_dialog.cpp`（composeTaskDetailDialog，:2874 起）③ `task_draft_job.{h,cpp}`（DraftJob + startDraftGeneration/reapDraftJob/applyDraftPrefill，可独立单测）。硬契约：成员声明序 `scheduler_` 在 `loop_/llm_/store_` 之后（agent_module.h ~:256）；注释随代码走；常量断言守卫保绿
2. **智能体任务详情弹窗布局重做**（用户原话：布局不合理不美观；设计决策已拍板，未落码）：
   - 摘要三卡去 1px 边框改 `surfaceElevated` 色块 + `t.sizes.radius`；窄屏（wide<680×520）2+1 不对称改单列三行
   - 配置条 hint 现状被硬切（「触发：手动 · 模型：def…」）→ 改四枚小 chip（触发/cron或闹钟/模型/迭代）可折行；「任务配置」「展开/收起」与浮层行为不动
   - 停止 chip 主文案缩「停止」，「当前轮结束后生效」挪副文案/stopping 态
   - 左栏宽 clamp(240,282) → clamp(innerW*0.42, 300, 360)；耗时列右对齐
   - 消灭卡中卡双边框（嵌套只留最外层一个边框层级）；选中 primarySoftBg/hover 保留
   - ui id 前缀 `agent.dialog.detail.*` 不动（probe 按 id 断言）；只用现有 ui_kit/EUI-NEO 能力，不改数据层
   - 验收：全量+probe 绿 + `build/tests/agent_dialog_probe --shot-detail /tmp/x.png` 截图自查
3. 快照终更（`docs/superpowers/plans/2026-10-18-agent-progress-snapshot.md`）
4. 合并走串行 + 全量&probe 双门禁

## 用户晨检（别替做）

TCC 授权弹窗、肉眼看横幅（跑「今日热点 top10」）、闹钟改时间（现 10-21 12:00，任务 id 9）、Windows 11 真机 UAT、About 版本口径拍板（build 计数 vs configure 版本）、**推送顺序：EUI-NEO→fork 先、再 DevDesk**（DevDesk 用了 EUI-NEO `8df829a` API，反着推 CI 必红）

## 本会话新增教训（未入 memory 的照此办）

- `scripts/build-test-run.sh` 无 +x 位：必须 `bash scripts/` 调，直接执行报 Permission denied
- `tests/probes/agent_e2e_probe` **没有 --help**，裸跑/错参会直接跑全流程 E2E（72s、真发 LLM 请求走代理链）。想看用法只能读源码 `tests/probes/*.cpp`；`agent_dialog_probe` 的截图开关是 `--shot-detail <path>`（拒连端口 9，零真网）
- Agent 工具 `isolation:worktree` 在本仓失效（根目录非 git 仓）→ 派单让 agent 手动 `git worktree add` + `pwd` 自查
- Monitor 过滤器别吞整段 ✅ 流（本会话被限流压制 2 事件）——盯终态用窄过滤

## 硬纪律（浓缩，完整版见 DevDesk/CLAUDE.md + memory 索引）

主会话只拆单/派单/审计/合并；构建只走 `scripts/build-test-run.sh`（bash 调）；派单前 `scripts/dispatch-check.sh`；合并串行+双门禁；唤醒死 agent 用 SendMessage 不重派；Bash >3 分钟后台跑；禁 `git stash/reset`；不碰 8888；不读/回显密钥；不 push（推送顺序见晨检）。

## suggested skills

- `superpowers:verification-before-completion` — 宣称任何完成态前（本会话已用它核旧链被杀真相）
- `superpowers:requesting-code-review` — 拆分/布局两阶段合并前
- `caveman:cavecrew-investigator` — 代码定位省 context
- `caveman:caveman` — 用户全程 caveman，续会话保持
- `cs-retro:cs-retro` — 拆分完成后记「上帝类拆分」经验；本会话教训（见上节）也可入 memory
