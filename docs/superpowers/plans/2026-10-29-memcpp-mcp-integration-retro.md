# memcpp MCP 集成 P0 批次 · 收口纪要（retro）

- 日期：2026-10-29。slug：`2026-10-29-memcpp-mcp-integration`（三卡：spec + `-tech` + `-tests`）。
- 流程：product-manager（PRD+矩阵）→ developer（实现+自证）→ code-reviewer（1 P1 打回）→ developer（R1-R4 修复）→ reviewer 定点复核（通过）→ qa（实操验收）。主会话全程只分派与物理验证。

## F-ID 终态统计

| 终态 | 数量 | 明细 |
|---|---|---|
| 已实现（自证） | 8 | F-01~F-07、F-09（F-01/F-04 经复审 R1-R3 修复增强） |
| 后置 | 1 | F-08 真连通道（P1 第二批，与 S5 收官 UAT 同场，理由见矩阵） |

## bug 终态（悬空必须为 0）

- **修复 4**：R1 limit 浮点形态（P1，json Int/Double 互斥陷阱）、R2 fwrite/fclose 短写漏报、R3 截断附注硬编码、R4 130% 档行盒重叠——全部回归绿 + 复核通过。
- **忽略 0；悬空 0。**
- **转待办 2（非本批代码 bug）**：① `build/DevDesk.app` 陈旧 bundle 陷阱（工具链，TODO 🟣 新增）；② qa 宿主无屏幕录制 TCC（体系缺口，已有登记）。

## AI 沉淀清单

- **经验 4**：developer 2（json 形态陷阱 / 写盘失败查全 fwrite+fclose）、qa 2（产物新鲜度核验 / TCC 缺口如实标注）——落 `.zcode/lessons/{developer,qa-tester}.md`（目录首建）。
- **回改 0**：无达安装门槛的新机制（安装前三问全不过）。
- **评测 2**：B6（JSON 多形态数值解析）、C4（启动验收先核产物新鲜度）——入 `docs/research/agent-eval-tasks.md`。

## 整顿清单

- TODO 销账 3：A.P0-1 MCP 集成 ✓、A.P1 memory_e2e_probe 门禁注入通道 ✓（改 live 标签处置）、C 线整批零构建实测 ✓（波次 A/B/C 覆盖）。
- TODO 新债 1：陈旧 bundle 陷阱（待用户选修法）。
- 证据归档：`evidence/2026-10-29-memcpp-mcp-qa/`（curl 实操 6 件 + 独立复跑全量/定向 3 件 + 隔离守卫 1 件）。
- 审查遗留：P2「提交卫生」——收口 squash 时按方案卡文件清单摘干净，勿卷入 `proposal_store.h`/`memory_module.cpp` 等他批文件。

## 下次准备度

- F-08 第二批：需引擎 Running + 已入库记忆（模型 25MB 需重新下载，`tools/memcpp-integration/spike/`）；与 S5 收官 UAT 同场。
- 真机 UI 交互链路（A1 截图/A2 两态/A3 拨开关）仍属体系缺口（无点击自动化 + TCC），真机 UAT 时人工补。
- git 状态：全部未提交（未授权）。建议的收口 squash：本批文件 = 方案卡/PRD/tests 卡清单 + `src/module_memory/mcp/`×2 + `src/memory_mcp_bridge.*` + `panel/settings_store.*` + `mcp_page_module.*` + `main.cpp` + `http_client.cpp` + 3×CMakeLists + `test_main.cpp` + `test_memory_mcp_search_tool.cpp` + `test_mcp_page.cpp`；他批文件（schedule/todo/markdown/process_host/proposal_store 等）不得混入。
