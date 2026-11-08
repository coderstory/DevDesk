# OpenCode 宿主 setup + 验收证据（need#58 · DevDesk 真项目）

> 日期：2026-11-06｜宿主：OpenCode v2.2.5（macOS arm64）｜devflow 0.39.0
> 项目：`/Users/coderstory/dev/DevDesk`

## 一、setup 产物

| 产物 | 结果 |
|---|---|
| `opencode.json` | 新建（原无）：`mcp.servers.devflow-pool` + `plugins: ["<devflow>/hosts/opencode/plugin"]`，POOL_DB/POOL_REPORT 指向 `docs/pool/` |
| `.opencode/devflow-root` | 写入 devflow 源仓绝对路径 |
| `.opencode/devflow.json` | 四键：setupVersion 0.39.0 / maxConcurrentSubagents 3 / peakWindowEnabled true / poolDir docs/pool/ |
| `.opencode/skills/` | 13 个 skills，级别 **symlink**（`opencode-links.sh` 输出：链接 13 / 副本 0 / 跳过 0） |
| `.opencode/agents/` | 7 角色派生（hosts/opencode/agents 覆盖头 + agents/*.md 正文），`mode: subagent` 齐 |
| `.opencode/armory/evidence-capture.sh` | 自包含 wrapper，EVIDENCE_ROOT=项目根 |
| `.opencode/lessons/` | 目录已建 |
| 门禁脚本 | 复用既有 `.zcode/armory/gates/pre-tool-use.sh`（双宿主不落第二份，适配层按 .zcode→.opencode→.claude 取第一份） |

门禁单测：bash-gate 载荷退出码 2；path-gate 载荷（build/）退出码 2。

## 二、四项实测（`opencode run --standalone --auto`，真实会话）

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | skills 可载 | ✅ | `skill` 工具载入 `pool-usage`，模型原样回读首行 `# Skill: pool-usage` |
| 2 | devflow-pool 工具可调 | ✅ | Code Mode 调 `pool_stats({})` 返回 `{total:137, open:16, ...}`（DevDesk 池真实数据） |
| 3 | subagent 缺池标记被拦 | ✅ | 派发 developer（无池标记）→ `Error: [devflow 门禁] 派发简报缺池条目 id：必含「[pool:need:<id>]」或「[pool:bug:<id>]」标记…` |
| 4 | 未入池改码被拦 | ✅（修复后） | 需求类发言打 pending 桩 → `Write src/zz_gate_probe2.txt` 被拦：`本轮发言后的任务尚未入池登记（B+ 口径）…`；文件未创建 |

适配层日志 `~/.zcode/devflow-gate/opencode-adapter.log`：`adapter 已加载`（projectDir=DevDesk），warn 计数 0。
附带实证：本会话内一条含 `cmake -B` 的 shell 调用被 bash-gate 实时阻断。

## 三、验收发现的缺陷（已修复 + 回归）

### B1（阻断验收）相对路径 fail-open

- 现象：真实 `write` 工具传 **项目相对路径**（`src/a.txt`）；适配层原样交给 `path-gate`/`execute-gate`，bash 门禁按项目根绝对路径前缀判定 → 判为「项目外」→ 放行。首次实测未入池改码**未被拦**。
- 根因：`hosts/opencode/plugin/src/server.ts` 的 `filePathOf()` 未做路径归一。
- 修复：相对路径 `resolve(projectDir, p)` 后再送门禁（`isAbsolute` 判断）。
- 回归：同一提示重跑，`write` 被拦且无部分写入（见上表 #4）。

### B2（同批修正）OpenCode 单宿主读不到 peak 开关

- 现象：`gates/agent-gate.sh` 只读 `.zcode/devflow.json`；纯 OpenCode 项目按分册写 `.opencode/devflow.json` 时 `peakWindowEnabled=false` 不生效。
- 修复：按 `.zcode` → `.opencode` → `.claude` 取第一份存在的 devflow.json（双宿主仍 .zcode 优先）。
- 回归：`devflow/tests/gate-test.sh` 全绿（含 need235 峰窗用例）。

## 四、能力边界（如实）

- 不支持：auto-continue（OpenCode 无 turn-end 钩子）、monitor 面板（~/.zcode 数据源）、peak-hours 18:00 定时恢复。
- 本次未测：池写清桩（execute.after → pool-write-clear.sh）会话级；单元链已由 gate-test.sh 覆盖。
