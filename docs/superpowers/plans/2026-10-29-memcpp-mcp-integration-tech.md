# 方案卡：memcpp 记忆功能 MCP 集成（P0 批次）

- 日期：2026-10-29。PRD：`docs/superpowers/specs/2026-10-29-memcpp-mcp-integration.md`。
- 范围：F-01~F-07、F-09（F-08 P1 不在本批）。

## 0. PRD 技术前提核验结论（读码实证，2026-10-29）

| PRD 前提 | 核验 | 结论 |
|---|---|---|
| `core/mcp_tool.h` 有 `IMcpTool` / `McpToolRegistry` / `globalMcpTools()` / `unregisterTool` | 已读 | 一致。`unregisterTool` 为 void、不存在即 no-op（map::erase 语义），§5.2 幂等要求零新增机制 |
| `registerBuiltinMcpTools` 装配先例 main.cpp:403 | 已读 | 一致，且带桥注入 `app::mcp::setHandlers`（404）同段 |
| hook 同款构造 `TelememHttpRetriever retriever;`（main.cpp:779） | 已读 | 一致；类无成员态、会话缓存是带锁全局，execute 可并发 |
| `IMemoryRetriever::search(query, limit, timeoutMs)` | 已读 | 一致（telemem_client.h:26） |
| `parseSearchResponse` 空段落=无命中 | 已读 | 一致（telemem_client.cpp:273-280） |
| `MemoryPanelSettings` / panel_settings.json | 已读 | 一致；`savePanelSettings` 现为 void 且吞写失败（见偏差 D2） |
| `mcp_service_bridge` 桥模式（函数指针缝+默认桩+static_assert） | 已读 | 一致，照抄 |
| 降级四态可由 Result 区分 | **部分不符** | `platform::http::send` 把一切 CURLcode 传输错误统一折成 `NetworkUnreachable`（http_client.cpp:199），`NetworkTimeout` 从未被返回——超时与拒连在 Result 层不可分。修正见偏差 D1 |

架构级无冲突，未触发「停下来汇报」条件；以下偏差均为局部实现口径。

## 1. F-ID → 技术方案

### F-01 工具本体（新增 `src/module_memory/mcp/search_tool.h/.cpp`）

- `class MemorySearchTool final : public core::mcp::IMcpTool`，构造注入 `std::shared_ptr<gateway::IMemoryRetriever>`（生产路径 `registerSearchTool` 内 `make_shared<TelememHttpRetriever>()` 自持；测试注入假检索器——注入先例 = WebFetchMarkdownTool 的 Fetcher 缝，测试有效性同理：不注入则 limit 钳制/成功/空命中分支全测不了）。
- 常量：`kMcpSearchTimeoutMs=3000`、`kMaxOutputChars=20000`、`kDefaultLimit=5`、`kMaxLimit=10`（文件局部，不导出）。
- execute：`platform::json::parse` → 非 JSON 对象 → isError「参数必须是 JSON 对象」；query 缺失/空串/非字符串 → isError「缺少 query（必须是非空字符串）」且不触检索；limit 非整数忽略取 5，钳 1..10；成功非空 → text = 各 hit 正文以 `\n` 连接（telemem 信封形态 = 单 hit 融合段原样，无二次加工）；空 → 「未命中任何记忆」；输出超 20000 字符按 UTF-8 码点边界截断 + 附注「（已截断到 20000 字符）」；最终过 `platform::utf8::sanitize`（web_fetch 同款收口）。
- 错误映射：`errorCode()==NetworkTimeout` → 超时文案；其余 → 连接失败文案 + errorMessage 透传。
- UTF-8 截断函数 `truncateByCharsUtf8` 为本文件局部复制（web_fetch 的同名函数是 module_mcp 文件局部；module 隔离禁 include 兄弟模块；core/str_util 无现成 utf8 截断——第二处需求出现再提升 core，先例不成立的抽象是负资产）。

### F-02 注册/注销接线（同文件导出，namespace `module::memory::mcp`）

- `inline constexpr char kMemorySearchToolName[] = "memory_search";`（main.cpp 状态实现按名查表用）。
- `registerSearchTool(registry)`：构造工具（自持 TelememHttpRetriever）→ `registry.registerTool`，重名 fail 原样上抛（main.cpp LOG_ERROR，不致命）。
- `unregisterSearchTool(registry)`：`registry.unregisterTool(kMemorySearchToolName)`（不存在 = no-op 既有语义）。
- `setSearchEnabled(registry, on)`：单点规则 = load settings → 改 `mcpSearchEnabled` → save（Result 化）；save fail → 返回 fail、注册态不动（§6.2 写盘失败态）；save ok → on ? register : unregister，注册结果原样返回。
- `searchEnabledBySetting()`：`loadPanelSettings().mcpSearchEnabled`（main.cpp 启动装配读一次）。

### F-03 MCP 页模块行（`src/module_mcp/mcp_page_module.cpp` + `.h`）

- 插入点：`mcp.status` 行 `.build()` 之后、`mcp.section` 之前，新增 `mcp.module.memory` 全宽行 stack：bg rect（surface + borderDefault + radiusLarge）→ 图标位 40×40（primarySoftBg 底 + FA7 `0xF5DC` fa-brain，与 app_registry 记忆行同码点）→ 标题「记忆」+ 说明「将本机长期记忆以 memory_search 工具开放给 MCP 客户端查询」→ 右侧 `components::toggleSwitch`（id `mcp.module.memory.toggle`，尺寸 44×24，`checked(app::memory_mcp::status().switchOn)`，onChange → 桥 setEnabled）→ 反馈红字（仅失败帧渲染）。
- 反馈态：`.h` 新增成员 `std::string saveError_;`；setEnabled ok → 清空，fail → 存 errorMessage（下帧红字「保存失败：<原因>」）。toggleSwitch 无内部态、每帧按 checked 重绘 → 写盘失败自动回弹，零额外逻辑。
- 既有元素（状态行/区头/计数 chip/工具网格/详情弹窗）零改动；工具网格数据源本就是每帧 `list()` 快照，注册态变化同帧生效。每个 builder 链以 `.build()` 收尾。

### F-04 持久化（`panel/settings_store.h` + `.cpp`）

- `MemoryPanelSettings` 加 `bool mcpSearchEnabled = true;`；load 加 `getBool("mcpSearchEnabled", true)`（旧文件缺键 = true），save 加对应 set。不动 settings_config 19 键体系。

### F-05 运行时下线桥（新增 `src/memory_mcp_bridge.h/.cpp`）

- 照抄 `mcp_service_bridge` 模式：`namespace app::memory_mcp { struct LiveStatus { bool switchOn = false; }; using StatusHandler = LiveStatus(*)(); using SetEnabledHandler = core::Result<void>(*)(bool); setHandlers(...); status(); setEnabled(bool); }` + 4 条 static_assert 签名哨兵。默认桩：status → `{false}`；setEnabled → fail(Internal,「记忆 MCP 桥未装配」)。
- main.cpp 注入（无捕获函数指针，mcpStatusImpl 同款）：
  - `memoryMcpStatusImpl` = `globalMcpTools().find(kMemorySearchToolName).isOk()`（读注册表实况，不读文件）；
  - `memoryMcpSetEnabledImpl` = 转发 `setSearchEnabled(globalMcpTools(), on)`。
- CMake：`memory_mcp_bridge.cpp` 照 `mcp_service_bridge.cpp` 同清单加入 DevDesk exe / devdesk_tests / probe 两分支 / github_sync_live / agent_live / mcp_live（mcp_live 链 devdesk_module_mcp，其 mcp_page_module.cpp 新增引用本桥符号，不编则 mcp_live 链接失败）。
- tests/test_main.cpp 复位段（16) 后）加 `::app::memory_mcp::setHandlers(nullptr, nullptr);` + include。

### F-06 降级文案（§6.3 全表照抄）

- 连接失败：`记忆引擎未运行或连接失败：<底层原因>。可在 DevDesk「记忆」页主控台启动引擎后重试`
- 超时：`记忆检索超时（3 秒）。引擎可能正在启动或负载高，稍后重试`
- 参数错：`缺少 query（必须是非空字符串）`；空命中（isError=false）：`未命中任何记忆`
- 全程 Result，不抛异常。

### F-07 单测（新增 `tests/test_modules/test_memory_mcp_search_tool.cpp`）

- 假检索器（实现 `IMemoryRetriever`，记录入参、可编 result）+ 每用例局部 `McpToolRegistry`（内置工具先例，防全局串味）。
- 用例：schema 契约 / 坏参数四态 / limit 钳制与默认（观测假检索器入参）/ 成功融合段原样 / 空命中 / 超时文案 / 连接失败文案 / 超长截断（20000 码点 + 附注）/ 注册表注册-注销往返（重注销 no-op）/ 持久化往返（含旧文件缺键默认 true、setSearchEnabled 开↔关与注册态联动）/ 桥默认桩与注入转发。
- tests/CMakeLists.txt devdesk_tests 源表登记（TIN-10 差分自检会核对磁盘实况）。

### F-09 红线回归

- 全量 `bash scripts/build-test-run.sh --no-run` + `--filter memory` + `--filter mcp` 两轮定向输出留证；`check-module-isolation.sh` 随脚本自动跑。

## 2. 局部偏差登记（自行决定，未动架构）

| # | 偏差 | 依据 |
|---|---|---|
| D1 | `platform/http_client.cpp` `send()`：CURLE_OPERATION_TIMEDOUT 映射 `NetworkTimeout`（其余 CURLcode 仍 NetworkUnreachable）；`sendStreaming` 不动 | F-06 要求超时/拒连可分，而 NetworkTimeout 定义至今从未被返回（死码）；全部 send() 调用方只读 errorMessage 不分支该码（grep 证实：telemem/web_fetch/github_api 均透传文案），行为面仅错误码枚举变精确。错误码语义本就归 platform 层定 |
| D2 | `savePanelSettings` 返回值 `void` → `core::Result<void>`（内部 fopen/序列化失败上报）；settings_store.cpp 除新键两行外新增失败上报 | §6.2「写盘失败 → 回弹 + 红字」需要失败信号，现实现吞进 LOG_WARN 无法上报；5 处现有调用点（panel_state.cpp）全为语句调用忽略返回值，零改动零行为变化 |
| D3 | 桥 status 实现读注册表实况（find memory_search）而非 `searchEnabledBySetting()` 读文件 | MCP 页 compose 每帧调 status()，每帧读 JSON 文件不可取；注册⟺开关不变式下注册表实况即开关态，文件只是启动期事实源 |
| D4 | `panel/settings_store.cpp` 必须随 `.h` 同改（PRD §9 只列了 .h） | load/save 实现体在 .cpp，不同步加键则字段永不落盘，F-04 往返测试必红 |

### 复审修复登记（2026-10-29 审查回单，R 编号）

| # | 修复 | 内容 |
|---|---|---|
| R1 | search_tool.cpp limit 整数提取 | Int/Double 双形态分路：`asInt()!=0` ⟺ Int 直取；否则走 `asDouble()` 且 `floor(d)==d` 才采纳（json.cpp 的 JsonImpl Int/Double 字段互斥写入，Double 形态 asInt() 恒 0，公开 API 不区分形态）。3.7 → 默认 5、2.0 → 2、1e20 → 钳 10，钳制在 int64/double 域完成后收窄，不经 impl-defined 截断。用例 +3（3.7/2.0/1e20） |
| R2 | settings_store.cpp fwrite/fclose 返回值检查 | 短写或 fclose 冲刷失败 → fail(FileWriteFailed,"写入不完整")——磁盘满最常在短写/冲刷显形；半截 JSON 会让下次 load 整体落默认、被关工具重启复活。用例：数据根指向普通文件 → save 必败 → setSearchEnabled fail 且注册态不动 |
| R3 | search_tool.cpp 截断附注去硬编码 | `"（已截断到 20000 字符）"` → `std::to_string(kMaxOutputChars)`（web_fetch mcp_tools_builtin.cpp:812 同款），改常量不再漂移文案 |
| R4 | mcp_page_module.cpp 说明行 y 偏移随字号缩放 | `kPad + 22.0f` 固定像素 → `kPad + core::theme::scaled(22.0f)`，消除 130% 字号档与标题盒约 1.4px 重叠（PRD §7「85%~130% 档行盒不重叠」） |

## 3. 涉及文件

| 动作 | 文件 |
|---|---|
| 新增 | `src/module_memory/mcp/search_tool.h` / `search_tool.cpp`、`src/memory_mcp_bridge.h` / `memory_mcp_bridge.cpp`、`tests/test_modules/test_memory_mcp_search_tool.cpp`、本卡 |
| 修改 | `src/module_memory/panel/settings_store.h/.cpp`（F-04+D2）、`src/module_mcp/mcp_page_module.h/.cpp`（F-03）、`src/main.cpp`（装配段+handlers+include）、`src/platform/http_client.cpp`（D1 一处）、`CMakeLists.txt`（DevDesk exe + bridge）、`src/module_memory/CMakeLists.txt`（mcp/ 两文件）、`tests/CMakeLists.txt`（桥 6 处 + 测试登记）、`tests/test_main.cpp`（桥复位） |
| 冻结 | `memory_module.cpp`、collector/engine/gateway/hook/pipeline/proposals/sanitize、module_mcp 协议/service/session_store/tools_builtin、settings_config |

## 4. POC

无需：全部机制均为既有先例照抄（IMcpTool 实现 / 函数指针桥 / JSON 设置字段 / toggleSwitch），无非平凡未知点。
