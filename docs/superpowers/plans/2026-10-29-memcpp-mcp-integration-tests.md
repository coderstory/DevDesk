# 测试清单卡：memcpp MCP 集成 P0 批次（qa 实操验收）

- 日期：2026-10-29。PRD：`2026-10-29-memcpp-mcp-integration.md` §8（A1-A7）；方案卡：`2026-10-29-memcpp-mcp-integration-tech.md`。
- 范围：A1-A4、A6-A7（A5 = F-08 真连 Claude Code，P1 缓置不在本批）。
- 证据物根目录：`DevDesk/evidence/2026-10-29-memcpp-mcp-qa/`（git-ignore）。
- 已知体系缺口：**点击类 UI 交互无法自主执行**（无法点开关/导航页面），相关链路如实标注「交互链路未验证」，以无头 compose 断言 + curl 线上验证替代。
- **截图采集缺口（本次实测发现）**：本验收进程宿主无 macOS「屏幕录制」TCC 授权（`screencapture -x` 报 could not create image from display，项目 evidence wrapper 同样走 screencapture），截图证据未能采集，如实标注。
- 验收环境：隔离数据根（`DEVDESK_DATA_ROOT` 指向 `evidence/2026-10-29-memcpp-mcp-qa/dataroot/`，预置 sqlite `app_config` 行 enabledModules 含 mcp+memory；用户真实数据根 `~/DevDesk` 零触碰）。
- **启动口径警示**：`build/DevDesk.app/` 是 Oct 19 陈旧 bundle（脚本常规构建不重组 bundle，产物是 `build/DevDesk` 裸二进制）——验收必须启动 `build/DevDesk`；启动旧 bundle 会得到 10 天前代码（日志特征：`enabledModules 含未知模块 ID "mcp"/"memory"/"release"`）。

| F-ID | 流程（验证什么能力） | 验证方法 | 证据物 | 结果 |
|---|---|---|---|---|
| A1（F-01/F-03） | 启动 app（MCP 模块开）→ MCP 页出现「记忆」模块行、开关默认开、工具网格含 memory_search 卡 | app 启动（隔离数据根）+ MCP server 监听 + tools/list 含 memory_search（协议面同一数据源）；UI 页面导航/截屏无法自主执行 → 交互链路未验证；布局面以 `test_mcp_page` 14/14 无头 compose 断言替代 | `dataroot/logs/devdesk.log`（HTTP server listening on 127.0.0.1:8931 ×2 次启动）、`curl-tools-list.json`、`test-mcp-page.log` | 已验证（协议+compose 面）；UI 真机面/截图 = 未验证（点击交互 + TCC 截图权限双缺口） |
| A2（F-01/F-06） | 引擎 Running + 已入库记忆：试一下命中/空 query 两态 | 引擎从未安装（`~/DevDesk/memory` 不存在）+ 需 25MB 模型下载 + venv + 页面点击（试一下），超出自主能力 → 未验证；复跑命令见下 | — | 未验证（引擎未装 + 点击交互缺口） |
| A3（F-04/F-05） | 拨开关关：工具卡消失 + tools/list 不含 memory_search；拨开恢复；重启保持 | 拨开关 = 点击交互 → **交互链路未验证**；已验半边：默认开注册（tools/list 含 memory_search）+ 重启后注册态保持（第二次启动 tools/list 仍含）；开关往返逻辑面由单测「持久化往返 + setSearchEnabled 开关联动注册态」覆盖 | `curl-tools-list.json`、`curl-restart-tools-list.json`、`curl-restart-check.txt`、`test-memory-mcp-search-tool.log`（10/10） | 部分验证（默认开 + 重启保持已验；拨关/拨开交互链路未验证） |
| A4（F-06） | 引擎停止态 tools/call memory_search → isError:true + §6.3 文案，server 不崩 | curl：initialize → tools/call（合法 query，8421 已核实空闲）→ 0.015s 返回 isError=true 文案与 §6.3 逐字一致；参数错态（缺 query）逐字一致；降级后再 initialize 200 正常（不崩）；另验未注册工具名 → -32602 协议既有行为 | `curl-init-meta.txt`、`curl-call-degraded.txt`、`curl-call-bad-args.txt`、`curl-alive-after.txt`、`curl-unknown-tool.txt` | 已验证 |
| A5（F-08） | Claude Code 真连调用 | P1 缓置，不在本批 | — | 范围外 |
| A6（F-09） | 红线未破：全量绿 + 既有记忆/MCP 测试全数通过 | 独立复跑（不信开发者汇报）三连：全量 + `--filter memory` + `--filter mcp`；tests 二进制经 strings 核验含 memory_search/mcpSearchEnabled（确证跑的是本批代码） | `full-suite-output.log`（Total: 1927 Passed: 1921 Failed: 0）、`filter-memory.log`（55/54/0，1 skip=Windows 压测既有态）、`filter-mcp.log`（148/148/0） | 已验证 |
| A7（F-02/F-05） | 模块隔离守卫退出码 0 | `bash scripts/check-module-isolation.sh` 独立跑 + 随脚本自动跑均过 | `module-isolation.log`（退出码 0） | 已验证 |
| REG-1 | 默认开持久化位：panel_settings.json 缺键/缺文件 = 默认 true | 隔离数据根无 panel_settings.json 启动 → memory_search 已注册（tools/list 实证）→ 重启仍注册；且全程文件未生成（main.cpp 仅 setSearchEnabled 落盘时写该文件，缺键路径成立） | `curl-restart-check.txt`、`dataroot/` 快照 | 已验证（缺文件默认 true 行为路径）；写盘态（拨开关生成文件）未验证（点击缺口） |
| REG-2 | 既有功能零改动（hook 注入/面板/采集/卡片） | 全量绿 + `--filter memory` 55 条 0 失败（含 memory hook 契约、面板用例） | `filter-memory.log` | 已验证 |
| BUG-n | 新发现缺陷登记 | 无功能缺陷；2 项验收环境观察见报告（陈旧 bundle 陷阱 / TCC 截图缺口，非本批代码问题） | — | — |

## 结果回写区（2026-10-29 qa 实操完成）

- A4 / A6 / A7 / REG-1 / REG-2：已验证（证据物路径见表）。
- A1：协议面 + compose 断言面已验证；真机 UI 面（页面导航 + 截屏）未验证。
- A2：未验证（引擎未安装 + 点击交互缺口）。复跑：按 `docs/superpowers/plans/2026-10-28-memcpp-e2e-verify.md` 拉起 llama-server(11435)+telemem(8421) 后，在 MCP 页 `试一下` 或 `curl tools/call`。
- A3：部分验证（默认开注册 + 重启保持已验；拨关/拨开交互链路未验证）。复跑：真机 MCP 页拨开关 + 前后 `curl tools/list`。
