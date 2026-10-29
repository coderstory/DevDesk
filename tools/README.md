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
