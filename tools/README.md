# tools/ 目录工具注记

## pool_mcp.py — 三池 MCP server

独立 Python 进程 MCP stdio server（决策文档 `docs/specs/2026-10-29-三池mcp决策文档.md`）。
数据 `docs/pool/pool.db`（出 git，不入库）；报告 `docs/pool/pool-report.html` 每次写操作自动重生成。

### 装机（每台开发机各一份 venv，两步）

```bash
# macOS / Linux
python3 -m venv tools/.pool-mcp-venv && tools/.pool-mcp-venv/bin/pip install mcp

# Windows（Git Bash / PowerShell）
python -m venv tools/.pool-mcp-venv && tools/.pool-mcp-venv/Scripts/pip install mcp
```

### Bot 会话接入（MCP stdio server 一条）

- command：`tools/.pool-mcp-venv/bin/python`（Windows：`tools/.pool-mcp-venv/Scripts/python.exe`）
- args：`tools/pool_mcp.py`
- cwd：仓库根（db/report 路径按脚本位置推导，环境变量 `POOL_DB` / `POOL_REPORT` 可覆盖）

### 工具面

`pool_add`（入池）/ `pool_fetch`（按 P 级/状态/池捞取）/ `pool_update`（状态机校验+溯源+子表）/
`pool_search`（FTS5 检索，CJK 已处理）/ `pool_trace`（溯源链）。

### 验收与迁移

- 验收：`tools/.pool-mcp-venv/bin/python tools/pool_mcp_acceptance.py`（F-01~F-06，临时库，不碰真实池）
- 旧表迁移：`tools/.pool-mcp-venv/bin/python tools/pool_migrate_legacy.py`
  （pool_items 21 条 → 五表全量重建，幂等可双机复跑；pool_items 保留作审计参照）

## 运维与跨机部署（tools/pool_ops.py）

```bash
# 备份：docs/pool/pool-backup-<时间戳>.sql（SQL 文本，落项目内）
tools/.pool-mcp-venv/bin/python tools/pool_ops.py backup
# 还原：默认取时间戳最新一份；还原前自动留 pre-restore 保险备份
tools/.pool-mcp-venv/bin/python tools/pool_ops.py restore [file.sql]
# venv 修复/重装（跨机部署第二步的脚本化形态）
tools/.pool-mcp-venv/bin/python tools/pool_ops.py reinstall-venv
```

跨机迁移三步：克隆仓库 → `python3 tools/pool_ops.py reinstall-venv` → 拷 `pool.db` 文件或
`restore` 快照/SQL 备份。实现注记：纯 stdlib 零依赖（venv 坏了 restore 仍可跑）；
contentless FTS5 不能被 iterdump 直接重放（无列名 INSERT 必炸），备份时滤掉 FTS 行、
尾部按 needs 全量重建——无独立数据，重建无损。

## zcode 供应商自动切换（tools/zcode_provider_switch.py，需-16）

「探测即判断」：最小真实 zcode 回合探测当前档位实际可用性；可用=白嫖到底不切，不可用=自动切另一档；失败重试 1 次防抖，全程记 `~/.zcode/v2/provider_switch.log`，异常桌面通知+非零退出。

```bash
python3 tools/zcode_provider_switch.py --window free   # 23:05 免费窗入口：切 Start Plan 并探测，不可用自动切回付费
python3 tools/zcode_provider_switch.py --window paid   # 9:05 出口：可用维持，不可用切回付费
python3 tools/zcode_provider_switch.py status          # 只读当前档位
python3 tools/zcode_provider_switch.py --probe-only    # 只探测不决策
ZCODE_PROBE_FORCE_FAIL=1 python3 tools/zcode_provider_switch.py --window free  # 注入失败（验收用）
```

双 cron 点已注册 DS Bot 调度（23:05/9:05，任务 id task-7807c9a2/fd51824e）。切换点=`~/.zcode/v2/provider_config.json` 的 `defaultModelSelection.providerId`，原子写（tmp+rename）；付费档 id 从 credentials 的 `<providerId>:identity` 键现读，解析失败回落常量并告警。
