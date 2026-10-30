# probe 残留黑窗事件 结案卡

- 日期：2026-10-29 晚
- 定性：流程缺陷（非产品 bug、非框架 bug）
- 结案依据：码农 Swift CGWindowList 全程监测实锤——黑窗=QA `ctest -L probe` 循环重启的真窗 probe（agent_e2e_probe 等 LLM 响应长挂、主进程退出子窗不收）；DevDesk 主窗与 EUI 框架 layer=0 无辜
- QA 侧处置：22:4x 清零（ctest 8498/8499 + probe 8505 已杀，pgrep 复核全净）；纪律立「probe 跑完即 pgrep 复核清进程」
- 守卫自动化：老板娘拍板派元气小萝莉（build-test-run.sh 测试段尾部 pgrep 复核）

## 守卫判据（终稿 v1.1，2026-10-29 23:2x）

1. 检查点：build-test-run.sh 测试段（含 probe 段）结束尾部，函数名建议 `guard_probe_residue`
2. 判据：`pgrep -f "build/tests/[a-z_]*_probe"`（macOS/Linux）/ `tasklist | findstr _probe`（Windows Git Bash）；命中 → 打印残留 PID + `kill <pid>`（Windows `taskkill /F /PID <pid>`）→ exit 1（FAIL，不静默绿）
3. 反证两条：①正常全绿路径（无残留）守卫必须静默过、零误报；②人为挂一个 probe（如 SIGSTOP 挂起一个 probe 进程）→ 守卫必须红——两条都跑过才算验收
4. Windows 分支同判据双平台对称；脚本内实现不受业务码 #ifdef 禁令约束
5. 附带：agent_e2e_probe 改隐藏窗/离屏前，凡含它的 probe 段跑完守卫可能红——属预期行为（真有残留），不是误报

## BUG-7 定性（agent_e2e 首跑挂/复跑绿）：环境性 flake

- 现象：agent_e2e_probe 首跑长挂（22:3x 事件挂到需手杀），复跑绿（76-86s 通过，job `bash-3a9a8f35d5ec4b3e`/`bash-c104fa78561d417c` 两轮实证）
- 复现条件：agent_e2e 是 live 型真窗探针（真调 LLM 网关端到端），耗时 1~90s 波动；首跑挂=冷启动叠加（GL 上下文 + 真网关握手 + LLM 响应慢），网关高峰即撞
- 定性：**环境依赖 flake（真网关延迟敏感），非代码回归**——mock 层 agent 域 133/133 稳绿可证逻辑无误
- 改进建议（供拍板，不阻塞）：①加显式超时预算（如 120s）超时 FAIL 带日志而非无限挂；②或照 memory_e2e_probe 先例挪 `live` 标签、挪出默认门禁
- 取证限制：老板在机禁开有窗 probe 急规，未跑复现，基于既有 job 证据静态定性

- 裁决（老板娘 2026-10-29 23:3x 定）：**选「挪 live 标签」**——agent_e2e_probe 加 live 标签从常规全量门禁摘出，零代码风险；超时预算不做（1-90s 是真实网特性，砍预算会掩盖问题）。归档于本卡，TODO 对应条目同步；实现随守卫单并给小萝莉（同一脚本段）。

## 结果（QA 终验 2026-10-29 23:5x：**通过，关单**）

- G-1 修复（`3a0d2dea`）复验：红证过——SIGSTOP 态（STAT T）假 probe 被守卫抓到且 -9 兜底清掉、exit 1；绿证过——环境净后 `--filter gate` exit 0、gate 4/4、守卫静默零命中、5 秒后无残留
- 中途插曲：绿证一度被小萝莉 launchd 保活任务（launchctl submit 残留，每几秒拉新 fake_probe）干扰连报红——守卫行为全程正确（真有残留就红），源头 launchctl remove 后绿证即过
- **债-1 验收通过**：守卫（1666a2cb + 3a0d2dea）+ agent_e2e_probe 挪 live，准予合 dev；合入后按流水线在 dev 树复跑一次守卫段确认
