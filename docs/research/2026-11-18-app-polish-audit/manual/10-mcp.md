# 10 MCP

> 手册走查底稿。以源码实况为准（核对基准日 2026-11-18）。

## 一句话

本机 MCP hub 服务：把内置工具（memory_search / md.* 笔记检索读写 / web_fetch_markdown / app.read_settings / project_info 等）经 `http://127.0.0.1:<端口>` 以标准 MCP 协议开放给 Claude Code 等客户端，页面可启停、试跑工具、一键安装到 Claude Code。

## 入口与前置（模块开关/门控）

- 侧边栏「MCP」，受模块开关控制（模块卡描述「MCP 服务器接入配置与管理」，settings_ui_cards.cpp:401）。
- 服务**默认是否自启未核**；设置页「MCP 服务」卡与本页均可启停/改端口（settings_ui_cards.cpp:808-866）。
- 记忆查询口径：MCP 页的「记忆」卡开关只控制 `memory_search` 是否出现在工具清单（USER_GUIDE §3.8）。

## 主流程步骤（编号：操作→预期可见结果）

1. 进入页面→「服务」区（mcp_page_module.cpp:533）：端口与运行状态（设置页同款胶囊：「运行中 127.0.0.1:<端口> / 已停止 / 启动失败：<原因>」，settings_ui_cards.cpp:816-819）；「MCP 端口」输入框改后回车或点「应用」，运行中自动重启（settings_ui_cards.cpp:825-866）。
2. 工具卡列表：每卡带类别 chip「内置 / 服务」（mcp_page_module.cpp:776、791）、hover 浮现「查看详情」CTA（mcp_page_module.cpp:552）。
3. 点卡→工具详情弹窗：工具名/描述、「参数」表单（按 inputSchema 生成，必填占位「必填」/可选「可选」，mcp_page_module.cpp:823、871）、「参数 schema（JSON）」原文（mcp_page_module.cpp:898）、「试一下」钮→「运行中…」→「运行结果」区显示输出（mcp_page_module.cpp:927、1015）。
4. 「安装到 Claude Code」按钮（未安装时）/「从 Claude Code 卸载」（已安装时，mcp_page_module.cpp:701）：写入 `~/.claude.json` 的 mcpServers 键（写前备份），成功提示「已写入，重启 Claude Code 后生效」（mcp_page_module.cpp:688）。
5. 「记忆」卡：开关+说明「将本机长期记忆以 memory_search 工具开放给 MCP 客户端查询」；保存失败红字「保存失败：<原因>」（mcp_page_module.cpp:620-642）。
6. MCP 客户端接入：server 地址 `http://127.0.0.1:<端口>`（见服务区）；JSON-RPC 标准：方法错 `-32601 未知方法`、参数错 `-32602`、内部错 `-32603`、解析/请求非法 HTTP 400 + `-32700/-32600`（mcp_protocol.cpp:120-141、226-290）。

## 弹窗·菜单·浮层清单

| 名称 | 触发 | 内容 | 关闭方式 |
|---|---|---|---|
| 工具详情弹窗 | 点工具卡 | 参数表单 + schema JSON +「试一下 / 运行中…」+「运行结果」；按钮「试一下」「关闭」；无参数工具不显示表单（schema 框兜底） | 「关闭」钮 / backdrop 点击 / Esc 三通道（mcp_page_module.cpp:733） |

## 空态·错误态

- 未检测到 Claude Code：提示「未检测到 Claude Code（~/.claude.json 不存在）」（mcp_page_module.cpp:986）。
- 服务未启动时试装：提示「请先启动 MCP 服务」（mcp_page_module.cpp:988）。
- 参数校验失败红字：「缺少必填参数：<名>」「参数 <名> 需为整数：当前「<值>」」「参数 <名> 需为数字：…」「入参序列化失败」（mcp_page_module.cpp:1211-1241）。
- 工具已注销时点详情：不开空弹窗（mcp_page_module.cpp:1073）。

## 设置项联动

- 设置页「MCP 服务」卡与 MCP 页同源：端口/启停/运行状态/「启动失败」原因（settings_ui_cards.cpp:808-819）。
- 「记忆」卡 ↔ 记忆模块开放口径（USER_GUIDE §3.8：记忆引擎内部 8421 端点是内部实现细节，接入一律走 hub）。
- 工具清单随各模块开关联动（mcp.health 等探活工具注册，mcp_page_module.cpp:1051）。

## 异常与边界场景表

| 场景 | 预期用户所见 | 现状判定 | 代码锚点 |
|---|---|---|---|
| 端口被占用 / 启动失败 | 设置卡与状态胶囊「启动失败：<原因>」（Server::start 仅返回 bool，原因统一拼「端口 <port> …」） | 已处理 | src/module_mcp/mcp_service.cpp:46-51；src/module_settings/settings_ui_cards.cpp:816-819 |
| 客户端→hub 工具调用超时 | 未知（工具在请求线程同步执行，未见超时机制——慢工具会挂住该请求） | 未知 | src/module_mcp/mcp_protocol.cpp:195-290（执行段无 timeout 参数） |
| 返回值序列化/解析失败（老板点名「返回解析失败」） | 客户端收到 `-32603` JSON-RPC 错误响应（errorResponseText），DevDesk 页面无感知 | 已处理（协议侧有兜底） | src/module_mcp/mcp_protocol.cpp:280-282、216、282 |
| 客户端请求 JSON 非法 / 批处理非法 | HTTP 400 + `-32700/-32600` 错误响应 | 已处理 | src/module_mcp/mcp_protocol.cpp:120-141 |
| 参数缺失/类型错 | `-32602`（「缺少必填参数」等） | 已处理 | src/module_mcp/mcp_protocol.cpp:226-261 |
| 未知方法 | `-32601 未知方法: <method>` | 已处理 | src/module_mcp/mcp_protocol.cpp:290 |
| server 进程崩溃 | 不适用——hub 是 DevDesk 应用内组件，无独立 server 进程；应用退出经 atexit stop 收尾 | 已处理（结构性不适用） | src/module_mcp/mcp_service.cpp:106 |
| 「安装到 Claude Code」时 ~/.claude.json 非法/非对象 | 拒绝写入保护原值：「~/.claude.json 的 mcpServers 是非对象形态（数组/字符串），拒绝写入以免吞掉原值；请先手工修正该键后再安装」 | 已处理 | src/module_mcp/claude_mcp_config.cpp:30-31 |
| 重复安装（重复 server 名） | 幂等：已存在同配置则 no-op（「一字节不动」），不产生重复项 | 已处理 | src/module_mcp/claude_mcp_config.cpp:96、130、173 |
| 卸载时配置不存在 | 幂等 no-op，不报错 | 已处理 | src/module_mcp/claude_mcp_config.cpp:130 |
| 写 ~/.claude.json 失败 | Result 错误透传（「读取 <路径> 失败」等），写入前先备份 | 已处理（备份在写前） | src/module_mcp/claude_mcp_config.cpp:65、166、177 |
| 试跑 object/array 类型参数 | 表单整体回退（不可简单渲染），走 schema 框兜底 | 已处理 | src/module_mcp/mcp_page_module.cpp:1182 |
| 改端口后旧客户端 | 客户端需手工改地址；应用无推送 | 半处理（重启代理自动生效，客户端侧需人工同步） | src/module_settings/settings_ui_cards.cpp:825-866 |

## 演练卡壳点

1. 「安装到 Claude Code」断言：~/.claude.json 属用户真实文件，走查环境若共用真实数据根会污染配置——建议临时数据根跑（ui_driver 默认临时根，但真实 app 手测需注意）。
2. 「试一下」是后台线程 + 收割（tryThread_ join），脚本断言「运行结果」出现需留 wait（mcp_page_module.cpp:1101、1249）。
3. hub 工具调用超时无机制——「老板点名」的返回解析失败在协议侧有 -32603 兜底，但**工具本身挂死会让客户端请求挂死**，走查时可用慢工具验证一次并记录实测表现。
4. 记忆卡开关联动 memory_search 出现/消失，需重开工具详情弹窗刷新清单才可见（列表读失败静默返回，mcp_page_module.cpp:1073）。

## 2026-11-18 打磨后状态

> 对账口径：本章正文 = 批次前基线（commit 64378979）；本节为打磨专项增量，依据池台账与 `git log 63900b3e..HEAD`，零发明。

### 行为变化（变化前 → 后）

1. **工具调用超时总闸**：工具在请求线程同步执行、无超时机制（慢工具挂死该请求）→ 外层超时总闸，默认 600s，env `MCP_TOOL_TIMEOUT_SECONDS` 可调（need#290，`a0ed6d99`）。异常表「客户端→hub 工具调用超时=未知」行升级「已处理」。
2. **弃置 worker 计数**：总闸弃置 worker 泄漏无上限 → 弃置计数 + LOG_WARN 含在途弃置数（need#301，bug#28 观测面落地；并发上限/同 tool+session 单飞拒绝待真实泄漏数据，保持 P2 观测）。
3. **记忆行玻璃**：MCP 页记忆行玻璃样式（need#296 D5）——视觉层微调。

### 卡壳点对账

1. 卡壳点 1（~/.claude.json 真实文件污染风险）：仍在。
2. 卡壳点 2（试跑断言留 wait）：仍在。
3. 卡壳点 3（工具挂死连带请求挂死）：**已消解**——600s 总闸落地（need#290）；弃置 worker 泄漏上限仍 P2 观测（bug#28）。
4. 卡壳点 4（记忆卡联动需重开弹窗刷新）：仍在。
