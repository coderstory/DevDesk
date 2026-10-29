# memcpp 记忆功能 MCP 集成 PRD

- 日期：2026-10-29。状态：待方案（矩阵为唯一推进状态源，初值全部「待方案」）。
- 需求来源：`TODO.md` §A memcpp 线 P0 第一条（用户 2026-10-28 明确要求「下一步先做」）。
- 上游 spec：`2026-10-28-memcpp-overview`（已交付面，本集成是其纯增量）。注：旧 spec 无 F-ID 体系，本文引用其章节号（`2026-10-28-memcpp-overview#§4` 形式）；MCP 侧机制引用 `2026-10-26-mcp-server-skeleton-design`（下称「骨架 spec」）。
- 红线（原文照抄）：**既有功能（hook 注入/面板/采集/卡片）一行不动**。本文所有改动点均按此过滤。

## 0. 一句话与术语

**一句话需求**：让 Claude Code 能通过 DevDesk 统一 MCP server 的 `memory_search` 工具查询本机长期记忆，并在 MCP 页面提供「记忆」模块行开关（关 = 工具不注册/运行时下线），既有记忆功能零改动。

**用户价值一句话**：Claude Code 不用另接任何记忆服务，经已有的 DevDesk MCP 入口随手查 DevDesk 沉淀的记忆；不想开放时一处开关随时关掉。

术语（首现解释，后文不再重复）：

- **MCP 工具**：MCP 客户端（首个 = Claude Code）可调用的函数，自带名字、一句说明、参数 schema。DevDesk 侧机制见骨架 spec §3（`core::mcp::IMcpTool` + `McpToolRegistry` + `globalMcpTools()` 单例）。
- **memory_search**：本文新增的 MCP 工具名，走既有检索接口缝 `IMemoryRetriever::search`（`2026-10-28-memcpp-overview#§4-gateway`）。
- **检索接口缝**：`src/module_memory/gateway/telemem_client.h` 的 `IMemoryRetriever`，调用方不直连 telemem 引擎；hook 注入通道已在用。
- **模块行**：MCP 页面新增的 UI 区块，每个「向 MCP server 贡献工具的模块」一行（本批只有「记忆」一行），行内含开关。

## 1. 背景与现状

### 1.1 已有面（不动的部分）

| 面 | 现状 | 出处 |
|---|---|---|
| 检索能力 | `IMemoryRetriever::search(query, limit, timeoutMs)` 已落地，telemem MCP-HTTP 转发实现 + 会话缓存 + 引擎重启自动重取 | `2026-10-28-memcpp-overview#§4-gateway`；`src/module_memory/gateway/telemem_client.h` |
| hook 注入 | `devdesk.exe memory hook` 子命令，每次调用现构造 `TelememHttpRetriever`（无共享单例） | `src/main.cpp:779` |
| MCP server | Streamable HTTP，`tools/list` 每次从注册表现查 `list()`；`registerTool`/`unregisterTool` 线程安全；注销瞬间在途 execute 由 shared_ptr keep-alive 保活 | `src/core/mcp_tool.h` 契约注释；骨架 spec §5.6 |
| 内置工具注册模式 | main.cpp MCP 装配段一次性调 `module::mcp::registerBuiltinMcpTools(globalMcpTools())`（ServerInfoTool / WebFetchMarkdownTool 两个先例） | `src/main.cpp:403`；`src/module_mcp/mcp_tools_builtin.cpp:828` |
| MCP 页面 | 服务状态行 + 工具卡网格 + 详情弹窗 + 「试一下」；页面注释明说「本页不提供开关，开关在设置页」（指 MCP 服务总开关） | `src/module_mcp/mcp_page_module.cpp` |
| 记忆面板设置持久化 | `MemoryPanelSettings`（JSON，落 `memoryDataDir()/panel_settings.json`，随 `DEVDESK_DATA_ROOT` 隔离），现有 sanitizeEnabled 等字段 | `src/module_memory/panel/settings_store.h` |
| 引擎生命周期 | **页面关闭 ≠ 引擎停止：引擎是应用级常驻服务**（`MemoryModule::onStop` 注释钉死） | `src/module_memory/memory_module.cpp:199-202` |
| 模块桥先例 | app 层函数指针缝（`mcp_service_bridge` / `module_toggle_bridge`）：默认桩 + main.cpp 装配期注入，桥 .cpp 只依赖 core | `src/mcp_service_bridge.cpp`、`src/module_toggle_bridge.cpp` |

### 1.2 关键机制事实（2026-10-29 读码核实）

1. `McpToolRegistry::unregisterTool` 已存在且为设计内用法（`core/mcp_tool.h` 头注释：「各 module_* 实现并在 onStart/onStop 注册/注销」）——**运行时下线零新增基础设施**。
2. 骨架 spec §9「扩展指南」：新能力接入 = 任意 module_* 实现 `IMcpTool` → 注册进 `globalMcpTools()` → 不碰 module_mcp。**module_memory 实现、app 层装配，即符合既有架构，无需 module↔module include，也无需新增服务生命周期桥**。
3. `capabilities.tools.listChanged = false`（骨架 spec §5.5）：不推送工具变更通知。工具注销后，**已连接的 MCP 客户端要重新拉 `tools/list`（新会话自动）才见工具消失**；期间调已注销工具名 → `-32602`（协议层既有行为）。
4. telemem 引擎侧唯一检索形态：`search_memories` 返回 `{"query":…,"memories":"<融合段落>"}` 信封，`parseSearchResponse` 已把融合段落单条化（空段落 = 无命中）——`2026-10-28-memcpp-overview#§4-gateway` 解析契约。

## 2. 目标 / 非目标

### 2.1 目标

1. 新增 MCP 工具 `memory_search`：按查询文本检索本机长期记忆，返回融合段落文本；走既有 `IMemoryRetriever` 缝，不直连引擎。
2. 工具实现与注册/注销 API 落在 `module_memory`（新 `mcp/` 子目录）；注册进 `core::mcp::globalMcpTools()`；module_memory 与 module_mcp **零 include**（架构硬约束：module_* 互不依赖）。
3. MCP 页面新增「记忆」模块行 + 开关：关 = 立即注销工具（运行时下线）+ 持久化；开 = 立即注册；重启后开关态保持。
4. 开关持久化：`MemoryPanelSettings` 新增字段 `mcpSearchEnabled`（默认 **true**），落记忆面板既有 `panel_settings.json`。
5. 降级可诊断：引擎未运行 / 检索失败 / 超时 / 空命中，四种情形工具都不崩、返回明确中文文案（`isError` 语义按骨架 spec §5.7）。

### 2.2 非目标（明确不做，90% 原则）

| # | 不做 | 缘由 |
|---|---|---|
| 1 | **写入/删除类 MCP 工具**（memory_add / memory_forget 等） | 查询入口是本需求核心价值。`addDirect`/`setMemoryForgotten` 是卡片代理层的内部执行面（`2026-10-28-memcpp-overview#§4-proposals`），直接暴露给客户端 = 绕过「新增必审」人审卡与撤销链，且引入误写/误删面 |
| 2 | 列举/全清类工具 | 引擎侧无列举缝（`2026-10-28-memcpp-overview#§5-限制1`），有缝也属写入类出口 |
| 3 | MCP 页多模块行框架泛化（为 todo 等未来模块预留抽象） | 第二个贡献工具的模块接入时再抽；现在只有一个模块行，先例不成立的抽象是负资产 |
| 4 | MCP 检索入消耗账本（usage_ledger） | 本地检索零云端成本，账本口径以云端 token 为准；hook 通道入账是因含 inject 记账需求。若要补，随 infer=true 云端线一起接（`TODO.md` §A P1） |
| 5 | 记忆页与 MCP 行的页面门控联动置灰 | 「记忆页关闭 ≠ 引擎停止」是既有事实（§1.1），MCP 查询可用性真实判据是引擎在不在，不是页面在不在。页面关闭后引擎无人拉起时工具自然走降级文案，行为自洽 |
| 6 | 记忆面板配置页同步展示此开关 | 需求只要求 MCP 页行；同一开关两处展示引入双写一致性问题，零增量价值 |

## 3. 功能矩阵（唯一推进状态源）

| ID | 功能点 | 优先级 | 状态 | 卡片（一句话） |
|---|---|---|---|---|
| F-01 | `memory_search` 工具本体（实现 + schema + 出参格式 + 内部超时常量） | P0 | 已实现（自证：单测含 schema 契约/limit 钳制含浮点形态 3.7→5、2.0→2/降级分支/截断，全绿；复审 R1-R3 已修） | module_memory 新 `mcp/search_tool.*`：实现 `IMcpTool`，内部自持 `TelememHttpRetriever`（hook 子命令同款构造），不读全局注册表、不触 UI |
| F-02 | 注册/注销接线（module_memory 公共 API + main.cpp 装配段调用点） | P0 | 已实现（自证：注册表往返单测绿；main.cpp 装配段与 `registerBuiltinMcpTools` 并排，不挂 MemoryModule） | `registerSearchTool` / `unregisterSearchTool` / `setSearchEnabled` 三个模块级函数；main.cpp MCP 装配段按开关注册（与 `registerBuiltinMcpTools` 并排） |
| F-03 | MCP 页「记忆」模块行 UI（行 + 开关 + 状态反馈） | P0 | 已实现（自证：`test_mcp_page` 14/14 绿，新增无头 compose 断言 = 行盒 1136×64/标题/说明/开关读桥/失败红字回弹/成功点亮；真机截图归 qa A1） | `mcp_page_module.cpp` 状态行与工具区之间插入模块行区；`components::toggleSwitch`，点击命中 `.hit`（设置页模块卡先例）；既有元素零改动 |
| F-04 | 开关持久化（`MemoryPanelSettings.mcpSearchEnabled` 默认 true） | P0 | 已实现（自证：持久化往返单测含旧文件缺键默认 true 与写盘失败不注册分支，绿；复审 R2 已修短写/冲刷漏报） | `panel/settings_store.h` 加字段；旧文件缺键 = 默认 true；不改 settings_config 的 19 键体系 |
| F-05 | 开关运行时下线（app 层桥 `memory_mcp_bridge` + main 注入） | P0 | 已实现（自证：桥默认桩/注入转发/复位单测绿；CMake 六处登记齐全，Windows 交叉编译守卫 371 文件过） | 函数指针缝（模式 = `mcp_service_bridge`）：`status()` / `setEnabled(bool)`；MCP 页只经桥操作，桥真实现转发 F-02 API |
| F-06 | 降级与错误文案（引擎未跑/失败/超时/空命中四态） | P0 | 已实现（自证：超时/连接失败/空命中/参数错四分支单测各一，绿；超时可分依赖 http_client 超时码修正，见方案卡 D1） | 见 §6.3 文案表；全程 Result，不抛异常 |
| F-07 | 单测（schema 合法性 / execute 参数与降级分支 / 注册表注册-注销往返 / 持久化往返） | P0 | 已实现（自证：`test_memory_mcp_search_tool.cpp` 11 用例 + `test_mcp_page` 模块行用例，假检索器 + 局部注册表） | 新 `tests/test_modules/test_memory_mcp_search_tool.cpp`；测试用局部注册表（内置工具先例，防全局串味） |
| F-08 | 真连验收通道（真实 Claude Code 调通 memory_search） | P1 | 后置（第二批 P1，与 S5 收官 UAT 同场；见 §9 批次规划） | 复用 `tests/live/mcp_live.cpp` `--serve` 口径（骨架 spec §10 批次 4）；前置 = 引擎 Running + 已入库记忆，不满足则 skip 并记录（B4 真连 E2E 是另一线欠账，不阻塞本线注册接线） |
| F-09 | 红线回归护栏（既有记忆测试全数仍绿 + MCP 既有测试仍绿） | P0 | 已实现（自证：全量 `Total: 1927 Passed: 1921 Failed: 0`；`--filter memory` 55 条 0 失败（1 skip = Windows 专属压测既有态，含 memory hook 契约 4 用例全绿）；`--filter mcp` 147/147；模块隔离守卫过） | 见 §8 A6；不是新功能，是每批收口的硬判据 |

**已取舍登记**（不做但被讨论过）：见 §2.2 全表。

## 4. 关键设计决策（含依据）

### 4.1 工具集 = 单工具 `memory_search`，不做 recall/search 双工具

- **本软件一致性**：检索接口缝只有一条 `search`（`telemem_client.h`）；"recall" 与 "search" 在 memcpp 线同义（`2026-10-28-memcpp-overview#§3.4` 触发线口径里 recall_eval 评测的就是 search）。拆两个工具 = 同一能力双入口，客户端困惑、零增量价值。
- **同类产品先例**：mem0 / 各类 memory MCP server 的最小可用集都是单个检索工具起步；写入类靠人审通道（本项目即提案卡）。
- 名称 `memory_search` 与引擎侧 `search_memories`、检索缝 `search` 同族，命名可追溯。**用户若偏好 `memory_recall` 仅是改名，不影响任何设计**（列开放问题 1）。

### 4.2 桥接形态 = 无 module↔module 桥；module_memory 实现 + app 层装配，MCP 页开关经一条新 app 桥

- **为什么不是「app 层桥缝合 module_mcp ↔ module_memory」**：依赖反转在 core 已完成——`core::mcp` 定义接口与注册表，module_memory 与 module_mcp 双方都只依赖 core（骨架 spec §2 硬性依赖规则 + §9 扩展指南）。新增 app 桥来「缝合」两模块反而是多余一层。
- **为什么不需要服务生命周期桥**：注册表注册独立于 MCP 服务运行态（服务停着，工具照样注册在表内；服务启动即服务于全表）。memory_search 不需要知道 server 在不在。
- **为什么 module_memory 实现**：工具需要 `TelememHttpRetriever`（module_memory 内部类）。由 module_memory 实现并导出注册 API，main.cpp（装配层，已 include module_memory，`main.cpp:779` hook 先例）调用——与 main.cpp 直调 `module::mcp::registerBuiltinMcpTools` 完全同构（**本软件一致性**）。
- **MCP 页开关为什么仍要一条 app 桥**：MCP 页（module_mcp）要读写「记忆面板自有设置」并触发注册/注销，两件事都属 module_memory，不能互相 include。按 `mcp_service_bridge` 先例造 `src/memory_mcp_bridge.h/.cpp`（函数指针缝 + 默认桩 + main.cpp 装配期注入真实现转发）——**同类先例**：`app::mcp` 四槽桥、`app::toggle` 两槽桥。
- **红线面**：`memory_module.cpp` 一行不动（注册点在 main 装配段，不在 MemoryModule 生命周期钩子——依据见 §4.4）。

### 4.3 开关语义 = 运行时下线（即时生效），持久化走记忆面板自有 JSON

- **启动时不注册 vs 运行时下线，选后者**。理由：
  1. **基础设施已支持**：`unregisterTool` 存在且注销瞬间在途 execute 由 keep-alive 保活（§1.2-1），零新增机制；选「仅启动时生效」反而要解释「拨了没反应」。
  2. **本软件一致性**：设置页所有模块开关（MCP 服务、页面注销）均即时生效；记忆面板内脱敏闸开关也是即时生效。
  3. 已知边界如实登记：`listChanged=false` 不推送通知，已连接客户端需重新 `tools/list` 才见变化（§1.2-3）——写进验收口径与用户文档，不算缺陷。
- **持久化键与默认值**：`MemoryPanelSettings` 加 `bool mcpSearchEnabled = true;`。不走 `settings_config` 的 `enabledModules`，依据：
  1. **本软件一致性**：记忆面板的能力开关（脱敏闸 `sanitizeEnabled`）就在 panel_settings.json，这是第二枚同类开关；
  2. 不动 settings_config 的 save/load/sanitized/键数注释四处（19 键体系），改动面最小，红线友好；
  3. 模块隔离：module_memory 不依赖 module_settings（现状即如此）。
- **默认 true 的依据**：MCP 服务本体默认关（用户开了 MCP 就是要用工具）；记忆内容对 MCP 客户端的暴露面 = hook 注入通道既有暴露面（同是本机 Claude Code 读记忆正文），默认开不扩大暴露面。**行业惯例**：能力默认可用、留关闭出口。

### 4.4 注册点 = main.cpp 装配段（不挂 MemoryModule onStart/onStop）

- 骨架 spec §9 字面模式是「模块 onStart/onStop 注册/注销」，但那是为「工具能力随模块实例存活」的场景设计的。记忆的特殊事实：**引擎是应用级常驻服务，页面关闭不停引擎**（`memory_module.cpp:200`）。若挂页面生命周期：页关 → 工具消失，而引擎还活着、检索本来可用——语义不自洽，还白白多两个改动点。
- 装配段注册（与 `registerBuiltinMcpTools` 并排）+ 桥翻转，共**两个维护点**，注册不变式简单：**注册 ⟺ `mcpSearchEnabled`**。
- 边界（接受并登记）：记忆页在设置页被关（kModuleIdMemory off）不影响本开关与工具注册态；引擎无人拉起时工具执行走降级文案（§6.3）。用户想整体关掉记忆对外查询 = 拨本开关。

### 4.5 出参不脱敏（与 hook 通道同口径）

- 脱敏闸的定义域是「**出机**（发给云端）前清洗」（`2026-10-28-memcpp-overview#§1-需求3`）；hook 注入向本机 Claude Code 发记忆正文不经脱敏。MCP 查询同为本机回环通道（server 只绑 127.0.0.1，骨架 spec §5.9），同口径不脱敏。**本软件一致性**。登记为已知边界，防后续误当漏洞修。

## 5. 工具契约（F-01）

### 5.1 `memory_search`

- **描述文案**：`检索 DevDesk 本机长期记忆（构建约定、环境事实、项目结论等已沉淀条目），返回按相关度融合的记忆段落`
- **入参 schema**：

```json
{
  "type": "object",
  "properties": {
    "query": { "type": "string", "description": "检索词，用自然语言描述要找的记忆" },
    "limit": { "type": "integer", "description": "返回条数上限，默认 5，范围 1..10" }
  },
  "required": ["query"]
}
```

- **行为**：
  1. `query` 缺失/空串/非字符串 → `isError=true`「缺少 query（必须是非空字符串）」。
  2. `limit` 缺省 5；非整数忽略取默认；钳制 1..10。
  3. 调 `IMemoryRetriever::search(query, limit, kMcpSearchTimeoutMs)`；超时常量建议 **3000ms**（新常量，独立于 hook 的 `kHookSearchTimeoutMs=800`——hook 的 800ms 是「不阻断用户提问」特例，MCP 是主动查询可放宽；实现期可调）。默认实现 = `TelememHttpRetriever`（工具实例自持一个，构造一次随注册表存活；会话缓存与引擎重启重取由该类既有契约承担）。
  4. 成功非空 → `isError=false`，text = 融合段落原文（`parseSearchResponse` 产物，不二次加工）。
  5. 成功空 → `isError=false`，text =「未命中任何记忆」。
  6. 输出硬顶：成功结果超 **20000 字符** → 按 UTF-8 边界截断 + 附注「（已截断到 20000 字符）」（web_fetch 的截断同款口径；固定值不参数化——limit 上限 10 × 单条 4KB 分段的极端和仍可控，防「超长段落烧爆客户端上下文」即可）。
  7. 检索 fail（含引擎未运行/连接失败/超时）→ `isError=true`，文案见 §6.3。
- **出参格式说明**：telemem 实测唯一形态是融合段落信封（§1.2-4），无逐条结构可复用；逐条化（附日期/分数）待引擎暴露结构化列举缝后再议（挂 `2026-10-28-memcpp-overview#§5-限制1` 的缝补齐）。
- **消耗记账**：不入账（§2.2-4）。

### 5.2 注册 API（F-02，签名意向，实现期以 Result 纪律收口）

```cpp
// src/module_memory/mcp/search_tool.h（namespace module::memory::mcp）
core::Result<void> registerSearchTool(core::mcp::McpToolRegistry& registry);
void unregisterSearchTool(core::mcp::McpToolRegistry& registry);
core::Result<void> setSearchEnabled(core::mcp::McpToolRegistry& registry, bool on); // 写盘+注册/注销单点
bool searchEnabledBySetting();  // 读 panel_settings.json 快捷
```

- `setSearchEnabled` 是开关状态规则的**单点维护处**（写 `panel_settings.json` + 按新值注册/注销）；main.cpp 装配段与桥 handler 都只转发到它/注册函数。
- 幂等要求：重复注册（重名 fail）只 LOG_ERROR 不致命；注销不存在的名 = no-op（`unregisterTool` 既有语义）。快速连拨 on→off→on 不崩、终态正确。
- 模块隔离守卫：module_memory 不 include 任何兄弟模块；`mcp/` 子目录只 include core 与 module_memory 自有头（`check-module-isolation.sh` 过）。

## 6. 用户操作流程与完整状态集

### 6.1 操作流程（验收照走）

1. 启动 DevDesk（记忆模块默认开、MCP 模块开）→ 打开 MCP 页：状态行下方出现「记忆」模块行，开关为开；工具网格出现 `memory_search` 卡（与既有卡同族，`试一下` 可用）。
2. 记忆页主控台启动引擎 → 主控台评测灌入或提案接受若干记忆 → 回 MCP 页 `memory_search` 卡「试一下」输入 query → 结果区显示「成功：」+ 记忆段落。
3. Claude Code 实连（F-08）：`claude mcp add --transport http devdesk http://127.0.0.1:<端口>/mcp` → 提问触发工具调用 → 返回记忆内容。
4. 拨「记忆」行开关为关：工具卡立即从网格消失；新开 Claude Code 会话 `tools/list` 不含 `memory_search`。
5. 重启 DevDesk：开关仍为关，`tools/list` 仍无该工具；拨回开：立即恢复。

### 6.2 状态集（每个界面/操作：空态、错误、边界）

| 态 | 表现 |
|---|---|
| 首次启动（无 panel_settings.json） | 开关 = 默认开；工具已注册 |
| MCP 服务停止 | 模块行照常渲染、开关可拨（注册态照变）；工具卡状态点「未启用」（既有态，不新增逻辑） |
| 引擎未安装 / 未运行 | 行与工具卡一切正常；`试一下` 与客户端调用返回降级文案（§6.3） |
| 引擎运行中、无命中 | `isError=false`「未命中任何记忆」 |
| 写盘失败（磁盘满/权限） | 开关视觉回弹到原值，行内红字「保存失败：<原因>」；不崩、注册态不变 |
| 快速连拨开关 | 幂等收敛，终态 = 最后一次拨动值；注册表无重复项 |
| 记忆页在设置页被关 | 模块行不受影响（§4.4 边界）；引擎无人拉起时工具走降级文案 |
| 在途调用时拨关（注销竞态） | 在途 execute 由 keep-alive 跑完正常返回；后续调用 `-32602`（协议既有行为）；UI 不悬垂 |
| 工具名冲突（理论态） | 注册 fail + LOG_ERROR，服务与其余工具不受影响（注册表既有语义） |

### 6.3 降级文案表（F-06）

| 情形 | 返回（isError=true） |
|---|---|
| 引擎未运行/连接失败 | `记忆引擎未运行或连接失败：<底层原因>。可在 DevDesk「记忆」页主控台启动引擎后重试` |
| 检索超时 | `记忆检索超时（3 秒）。引擎可能正在启动或负载高，稍后重试` |
| 引擎会话失效重取后仍失败 | 同连接失败文案（底层原因已含 404/重取细节） |
| 参数错 | `缺少 query（必须是非空字符串）` |

文案原则：可诊断（底层原因透传，与 web_fetch 的错误文案口径一致）、给出口（指向记忆页主控台）、不泄露记忆正文与密钥（telemem_client 密钥红线：本通道不带任何凭据）。

## 7. UI 契约（F-03，可截图对照）

- **插入位置**：`mcp_page_module.cpp` compose 内，`mcp.status` 状态行之后、`mcp.section`（注册的工具区头）之前，新增模块行区。既有元素（状态行/区头/计数 chip/工具网格/详情弹窗）**零改动**。
- **行结构**（根 id `mcp.module.memory`，全宽轻量行卡——状态行已是全宽先例；不用 340 网格卡，为将来多模块行自然堆叠留形）：
  1. 图标位：FA7 `0xF5DC` fa-brain（与 `app_registry.h` 记忆行同码点），尺寸/圆角/底色对齐工具卡图标位 token；
  2. 标题「记忆」+ 说明一行：「将本机长期记忆以 memory_search 工具开放给 MCP 客户端查询」；
  3. 右侧 `components::toggleSwitch`（id `mcp.module.memory.toggle`，点击命中 `.hit`——设置页模块卡同款口径），初值 = 桥 `status().switchOn`；
  4. 反馈位：常态不渲染；写盘失败时行内红字（§6.2）。
- **交互**：拨开关 → 调桥 `setEnabled` → 成功：工具网格同帧增减 `memory_search` 卡（数据源本就是每帧 `list()` 快照，零额外刷新逻辑）；失败：回弹 + 红字。
- **视觉**：沿用本页 theme token（`componentTokensWeChat` / `t.sizes.*`），不新增常量族；85%~130% 字号档行盒不重叠。
- **明确不做**：demo 里的页内服务开关与关闭置灰（骨架 spec §15 已裁决不照抄）；模块行的 pulse 动画。

## 8. 验收标准（qa 实操，标 F-ID）

| # | 标准 | 证据物 |
|---|---|---|
| A1 | 启动 app（MCP 模块开）→ MCP 页出现「记忆」模块行、开关默认开、工具网格含 `memory_search` 卡；详情弹窗 schema 与 §5.1 一致（F-01/F-03） | 截图：MCP 页整页 + 详情弹窗 |
| A2 | 引擎 Running + 已入库记忆：MCP 页 `试一下` 输入命中词 → 结果区「成功：」+ 段落含目标记忆正文；输入空 query → 「失败：缺少 query…」（F-01/F-06） | 截图：试一下结果区两态 |
| A3 | 拨开关关：工具卡立即消失；`curl` 新会话 initialize+tools/list 不含 `memory_search` 且其余工具仍在；拨开：恢复。重启 app 开关态保持（F-04/F-05） | curl 前后输出 + 截图 |
| A4 | 引擎停止态 `tools/call memory_search` → `isError:true` + §6.3 文案，server 不崩（后续 ping 正常）（F-06） | curl 输出 + 服务存活证据 |
| A5 | Claude Code 真连调用一次 memory_search 返回记忆内容（F-08；引擎前置不满足则 skip 并记录，不阻塞 A1-A4 判收） | `tests/live/mcp_live` 口径运行记录 |
| A6 | **红线未破**：`bash scripts/build-test-run.sh --no-run` 全量输出中，既有记忆测试（`test_memory_*` 全部文件 + `memory_probe`）与 MCP 既有测试（`test_mcp_*`）全数通过；`devdesk.exe memory hook` 契约用例（61 项）仍绿；记忆面板三视图与改动前截图对照无差异（F-09） | 构建脚本 Total/Passed 输出 + 三视图对照截图 |
| A7 | 模块隔离守卫通过：`bash scripts/check-module-isolation.sh` 退出码 0（module_memory/module_mcp 零兄弟 include）（F-02/F-05） | 脚本输出 |

## 9. 关联影响

| 面 | 影响 |
|---|---|
| `main.cpp` | MCP 装配段追加：按 `searchEnabledBySetting()` 注册 + `app::memory_mcp::setHandlers(...)`（无捕获函数指针转发，`mcpStartImpl` 同款） |
| `src/memory_mcp_bridge.h/.cpp` | 新增（模式照 `mcp_service_bridge`：默认桩 + `static_assert` 签名哨兵） |
| CMake / 测试登记 | `module_memory/CMakeLists.txt` 加 `mcp/` 两文件；`memory_mcp_bridge.cpp` 照 `mcp_service_bridge.cpp` 同清单加入可执行/tests/probe/live 目标源集；`tests/test_main.cpp` 每用例出厂复位加桥复位（防桩残留） |
| `module_mcp` | 仅 `mcp_page_module.cpp` 插入模块行区；协议/service/handler/session_store/tools_builtin **冻结不动**（骨架 spec §15 冻结清单） |
| `module_memory` | 新增 `mcp/search_tool.*` + `panel/settings_store.h` 加一字段；`memory_module.cpp` 与既有五个子目录**一行不动** |
| 平台 | 零平台分支（回环 HTTP + JSON 全走既有 platform 层，双平台同源）。macOS 侧引擎托管实测仍挂既有 P2 欠账（`TODO.md` §A P2），非本线新增 |
| 文档 | `docs/EUI-NEO-CAPABILITIES.md` 不涉及；`docs/USER_GUIDE.md` MCP 段补一句记忆查询能力（收口批） |

## 10. 优先级建议

1. **第一批（P0 核心）**：F-01 + F-02 + F-04 + F-06 + F-07（工具、注册、持久化、降级、单测）→ F-03 + F-05（UI 行与桥）→ F-09 随批跑。全部完成即可判收（A1-A4、A6-A7）。
2. **第二批（P1）**：F-08 真连，与引擎 Running 的真机环境合并做（可与 S5 收官 UAT 同场）。
3. 依赖：无前置阻塞项；B4 真连 E2E（另一线欠账）不阻塞 F-01~F-07、F-09。

## 11. 开放问题

| # | 问题 | 现处置 |
|---|---|---|
| 1 | 工具命名 `memory_search` vs `memory_recall` | 已按 §4.1 拍 `memory_search`（同名族可追溯）；仅改名不影响设计，用户可推翻 |
| 2 | MCP 检索是否入消耗账本 | 已取舍不入（§2.2-4，本地零云端成本）；用户若要消耗页可见，随 infer=true 云端线一起接 |
| 3 | 默认开（装 MCP 即得记忆查询）是否符合隐私预期 | 已按 §4.3 拍 true（暴露面 = hook 通道既有面）；用户若要默认关，改一字段默认值即可 |

---

*本文只记「当前决定 + 依据」；实现细节（文件名/签名微调/常量值）以卡片推进时 developer 与主会话裁决为准，冲突时回本表矩阵状态。*
