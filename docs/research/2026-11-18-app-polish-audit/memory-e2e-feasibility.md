# 记忆引擎 E2E 本机可自动跑通性 —— 可行性简报

> **老板令冻结，探查中止于现状**（收到停止指令的时点：本机依赖探查 + 一轮 e2e 已跑完、进程已清场；此后未再拉起任何引擎进程、未再做新的探查动作）。本文只记录冻结前已完成的事实，不含后续计划性内容。

- 池条目：need:315（qa-tester，可行性探查）
- 日期：2026-11-18；主机：macOS arm64
- 铁律遵守：未碰真实数据根（未写 `~/DevDesk` 任何数据文件，仅读取其依赖路径是否存在）；未改 `src/`；未落盘任何真实 LLM key；未联网下载模型。

## 结论

本机**可自动跑通**记忆引擎 e2e 主体链路（造数据 → 起引擎 → 采集/脱敏/提案 → 入库/检索/撤销 → hook 注入 → 冻结评测 → 红线 grep → 清场），一键脚本单条命令可完成，**已实跑一轮：46 条断言 42 PASS / 4 FAIL，4 条全部集中在 S8 提炼段（dummy key 预期内）**。缺口 **3 条**，其中 1 条是 must-fix（key 通道迁移后脚本未覆盖）。

## 依赖状态（本机实测存在，无需联网下载）

| 依赖 | 状态 |
|---|---|
| llama-server | `/opt/homebrew/bin/llama-server` → llama.cpp 1.0.0 (build 12359, Darwin arm64) |
| embedding 模型 | `~/DevDesk/memory/models/bge-small-zh-v1.5-Q8_0.gguf`，26,472,640 B |
| telemem venv | `~/DevDesk/memory/venv/bin/python` → python3.12，`import telemem` = **1.10.1** |
| 构建产物 | `build/tests/memory_e2e_probe`、`build/DevDesk` 均在位（11-18 18:51 构建） |
| 端口 8421 / 11435 | 跑前空闲，跑后已清场（`pgrep telemem.mcp\|llama-server` 空） |
| 配方脚本 | `scripts/run-memory-e2e.sh`（临时根 + 伪造 3 会话 + 起引擎 + 探针 + hook 三问 + fail-open + 红线 grep + 清场） |

## 已跑一轮的结果（冻结前完成，非本轮冻结后新跑）

命令：`bash scripts/run-memory-e2e.sh --keep`

- PASS：造环境 / 双引擎探活（llama `/health`=200、telemem `initialize`=200）/ 探针 S3–S7 / hook 改写问 ×2 命中 / 负对照 0 注入 / fail-open（stdout 空 + exit 0）/ 红线 grep 零命中（phone+secret+ipv4）
- FAIL 4 条，全在 S8：`runInferStep 真调成功`、`清单含补标产物`、`计账非零`、`队列已出队`
- FAIL 真因（`telemem.log` 实证）：`POST https://open.bigmodel.cn/api/anthropic/v1/messages → HTTP/1.1 401 Unauthorized`，`anthropic.AuthenticationError: {'message': '令牌已过期或验证不正确'}` —— dummy key 预期行为，非代码回归，属 fail-closed 通道有效
- 证据：`/Users/coderstory/dev/DevDesk/evidence/2026-11-18-memory-e2e/`（`run.log` / `probe.log` / `telemem.log` / `llama-server.log` / hook 输出）

## 缺口清单

| # | 缺口 | 证据 | 建议谁补 |
|---|---|---|---|
| G1 | **key 通道未覆盖（must-fix）**：脚本把 dummy/真 key 直接塞 telemem 子进程 env（`ANTHROPIC_API_KEY`），yaml 中 `api_key: ""`；探针也不触 key 通道 —— need305 迁 `app_secrets` 后的「DB 预置 → 引擎取 key → 起服」整段当前 **零覆盖**，脚本仍会假绿 | `scripts/run-memory-e2e.sh:70,217`；`src/module_memory/engine/telemem_engine.cpp:429`（`readLlmKey()`）；`src/core/secrets_store.cpp:118`（需 `appDatabase()` 已注册） | developer 改脚本：沙箱 `devdesk.db` 预写 `app_secrets(service='memory.llm', account='default')` 后改走 `TelememEngine::startAsync()` 拉起；或 qa 在清单加一条 DB 预置行交 developer 实现 |
| G2 | `memory_install_probe key-e2e` 迁移后失效：子命令直接 `saveLlmKeyToKeychain`，但探针 未 `registerAppDatabase` → `appDatabase()` 为 null → 必失败（「appDatabase 未注册」） | `tests/probes/memory_install_probe.cpp:340`；`src/core/secrets_store.cpp:118-123` | developer（探针属测试代码，qa 可改但需构建） |
| G3 | S8 无真 key 时**恒 4 红**，脚本不给「跳段」出口 —— 无 key 机器上无法得到全绿，只能读到「42/46」并人工判读 | 本轮实测 4 FAIL 全在 S8 | developer：`E2E_LLM_KEY` 未设时把 S8 段落显式标 SKIP（打印跳过行），而不是期望它绿 |

## 费用与风险

- **费用**：S3–S7 + hook + 评测全走本机 embedding，**零 API 费**；仅 S8 提炼需要真 LLM key（约 1 次短调用，量级可忽略）。
- **风险**：① 8421 残留 telemem 会被复用致假 PASS —— 脚本已内置端口预检，实测生效；② 阈值灰带（S7「8421」/「红烧肉怎么做」）分数贴 0.4，换模型或跨平台可能翻红，脚本已把它降为信息行 + 常量自洽断言；③ 沙箱口径偏差：本轮沿用脚本自带临时根 `/tmp/mem-e2e.QSxQaP`（`--keep` 保留），**未使用**简报指定的 `/tmp/need315-sandbox`；两者均不碰真实数据根，隔离性等价。

## 工具调用

约 39 次 / 预算 ≤45；耗时在 40 分钟窗内。
