# DevDesk MCP 服务骨架设计（Streamable HTTP + 可插拔能力接口）

> 日期：2026-10-26 · 状态：设计已拍板 · 基线：`feat/mcp-server-skeleton`（基于 dev `5c952d7e`）
> 适用平台：Windows 11 与 macOS 同一套代码。

## 0. 一句话与术语

DevDesk 新增一个**默认关闭**的后台模块 `module_mcp`：在本机 `127.0.0.1` 上提供 MCP 服务，把 DevDesk 的能力以「工具」形式开放给本机 MCP 客户端。第一个客户端是 Claude Code（HTTP 接入），第一个用途是抓网页并转成 Markdown。

术语（首次出现给一句人话，后文不再解释）：

- **MCP（Model Context Protocol）**：让 AI 客户端调用本机程序能力的开放协议；客户端先握手，再列出并调用「工具」。
- **工具（tool）**：一个可被客户端调用的函数，自带名字、一句说明和参数结构说明（schema）。
- **JSON-RPC**：用 JSON 文本表达「调用哪个方法、参数是什么、结果是成功还是错误」的请求/响应格式。
- **Streamable HTTP**：MCP 规定的 HTTP 传输方式；客户端用一条 POST 发出 JSON-RPC 消息，服务端用一条 JSON 应答（本轮不做事件流）。
- **会话（session）**：握手成功后服务端发的一个随机 id，客户端后续请求带着它；id 不认识 = 会话已失效。
- **SSE（Server-Sent Events）**：服务端主动推送的事件流。本轮不提供。
- **通知（notification）**：没有 `id` 的 JSON-RPC 消息，只表达「发生了某事」，服务端不应答。
- **批处理（batch）**：把多条 JSON-RPC 消息塞进一个 JSON 数组一次发送；2025-06-18 起已从协议移除。
- **桥（bridge）**：用函数指针把上层实现注入给下层调用的接缝，避免两个模块互相 include。
- **单例**：进程内唯一实例，通常经 `globalXxx()` 访问。
- **atexit**：进程正常退出前执行的回调；后注册的先执行。
- **HTML→Markdown**：把网页的 HTML 结构转成纯文本标记，便于模型阅读。

## 1. 背景与目标

### 1.1 目标

1. 新增独立模块 `module_mcp`，默认关闭；设置 → 模块卡标题右侧出现「MCP 服务」开关（零高度增量），打开后本机服务启动。
2. 设置 → 通用里出现「MCP 服务」配置卡（端口、运行状态、Claude Code 接入命令）；该卡由模块开关门控，模块关闭时整卡隐藏。
3. 首个内置工具 `web_fetch_markdown`：抓取网页并转 Markdown，供 Claude Code 检索互联网信息。
4. 能力接缝放 core（依赖反转）：core 定义接口与注册表，各模块自行实现并注册；`module_mcp` 只做协议、会话、HTTP 与生命周期，不持有业务能力。
5. 不自动修改 Claude Code 配置，只在设置卡里展示接入命令。
6. 平台复用现有 `platform::http::Server`（cpp-httplib）与 `platform::http::send`（libcurl），零新外部依赖。

### 1.2 已锁定决策

| 决策 | 结论 | 含义 |
|---|---|---|
| 模块形态 | 后台服务型模块，**不做侧边栏页面** | 不进 Router 页面表，不做 `compose` 页面 |
| 默认状态 | **默认关闭** | 不加入 `defaultEnabledModules()` |
| 启用入口 | 设置 → 模块卡标题右侧「MCP 服务」开关 | 零高度增量，复用 `applyModuleEnabled` 即时生效链路 |
| 监听地址 | 固定 `127.0.0.1` | 仅本机可访问；端口可配置 |
| 接入方式 | Claude Code 走 HTTP | 不写 Claude Code 配置文件，只展示命令 |
| 平台 | Windows 11 + macOS 同一套代码 | 无平台宏，平台差异只经 CMake 与既有 platform 层 |

## 2. 架构与依赖

```
main.cpp（装配：注册内置工具 / 注入桥 / 按开关启动 / atexit stop）
   │
   ├── module_mcp（静态库 devdesk_module_mcp）
   │      McpService ── platform::http::Server（127.0.0.1:<port>/mcp）
   │      McpHttpHandler ── McpProtocol ── McpSessionStore + core::mcp::McpToolRegistry
   │      mcp_tools_builtin（web_fetch_markdown / devdesk_server_info）
   │      html_to_markdown（自研，零新依赖）
   │
   ├── core::mcp（接口 + 注册表 + 端口常量，不依赖 platform）
   │      IMcpTool / McpToolRegistry / globalMcpTools() / mcp_port.h
   │
   ├── 各 module_*（未来）实现 IMcpTool 并在 onStart/onStop 注册/注销
   │
   └── app 层桥：src/mcp_service_bridge.h（函数指针缝，模式同 module_toggle_bridge）
          settings 模块只经桥读状态/发指令，不 include module_mcp
```

硬性依赖规则：

- `devdesk_module_mcp` 只链接 `devdesk_core` 与 `devdesk_platform`；**不依赖 `ui_kit`、不依赖任何兄弟 `module_*`**。
- `core` 不依赖 `platform`、不依赖任何模块。因此 schema 合法性校验放在 `module_mcp`，不在 core。
- `module_settings` 不 include `module_mcp`；两侧只共享 core 常量与 app 层桥。
- 与现有 `core/tool.h` 的 `ToolRegistry` 同款思路：core 定义抽象、模块实现并注册，双方都只依赖 core。

## 3. 能力接缝（core）

新增 `src/core/mcp_tool.h` / `src/core/mcp_tool.cpp`。精确接口：

```cpp
namespace core::mcp {

struct ToolResult {
    bool isError = false;   // true = 工具业务失败；仍以文本形式回给客户端
    std::string text;       // 纯文本结果
};

class IMcpTool {
public:
    virtual ~IMcpTool() = default;
    virtual std::string name() const = 0;             // 全局唯一；内置工具一律下划线命名（规避点号被客户端规范化）
    virtual std::string description() const = 0;      // 一句话说明
    virtual std::string inputSchemaJson() const = 0;  // JSON 对象文本（MCP inputSchema）
    // argumentsJson 恒为 JSON 对象文本；返回 fail 表示基础设施/实现错误（映射为 -32603），
    // 业务失败请返回 ToolResult{isError=true}
    virtual Result<ToolResult> execute(const std::string& argumentsJson) = 0;
};

class McpToolRegistry {
public:
    Result<void> registerTool(std::shared_ptr<IMcpTool> tool);  // 空指针/非法名/重名 → fail(ValidationError)
    void unregisterTool(const std::string& name);
    Result<std::shared_ptr<IMcpTool>> find(const std::string& name) const;  // 不存在 → fail(NotFound)；返回 keep-alive
    std::vector<std::shared_ptr<IMcpTool>> list() const;        // name 字典序（std::map 序遍历）
    std::size_t size() const;
};

McpToolRegistry& globalMcpTools();  // 进程级单例访问器
}
```

契约：

- 注册表内部单锁串行化，只保证**查找/注册/注销**线程安全。
- `execute` 在 HTTP worker 线程执行：实现方自行保证线程安全，**禁止触 UI**，长任务自行限时。
- `globalMcpTools()` 采用与 `core::ToolRegistry::instance()` 相同的「永不析构」写法：`static auto& g = *new McpToolRegistry();`，避免退出钩子访问已析构的 map/mutex。
- core 不依赖 `platform`，所以 `inputSchemaJson()` 的合法性（能否解析为 JSON 对象）由 `module_mcp` 组装 `tools/list` 时校验；非法 schema 的单个工具**跳过并 LOG_WARN**，不拖垮整个服务。

新增端口常量头 `src/core/mcp_port.h`（纯常量头，无 .cpp，镜像 `core/proxy_port.h`）：

```cpp
namespace core {
inline constexpr int kDefaultMcpPort = 8931;
inline constexpr int kMinMcpPort = 1;
inline constexpr int kMaxMcpPort = 65535;
}
```

## 4. module_mcp 文件与职责

| 文件 | 职责 |
|---|---|
| `src/module_mcp/CMakeLists.txt` | 静态库 `devdesk_module_mcp`；链接 core/platform；照 `module_about` 注入 `DEVDESK_VERSION="${PROJECT_VERSION}"` / `DEVDESK_BUILD_TYPE="$<CONFIG>"` / `DEVDESK_PLATFORM="${CMAKE_SYSTEM_NAME}"`（业务代码零平台宏；不设 `DEVDESK_OS_NAME`） |
| `mcp_service.h/.cpp` | 服务生命周期：`McpService` + `globalMcpService()`；绑定 127.0.0.1、Host 白名单、端口 0 支持 |
| `mcp_session_store.h/.cpp` | 线程安全会话表：32 位十六进制 id，最多 64 个，超出淘汰最旧；`create/has/remove`；构造函数可注入 id 生成器 |
| `mcp_protocol.h/.cpp` | 纯逻辑 JSON-RPC 分发（不碰 socket，可单测）：输入 body + session id + 协议版本头，输出 `HttpOutcome` |
| `mcp_http_handler.h/.cpp` | `RequestCtx`/`ResponseWriter` 适配、Origin 校验、方法路由（POST/GET/DELETE/OPTIONS `/mcp`） |
| `mcp_tools_builtin.h/.cpp` | `registerBuiltinMcpTools(registry, fetcher)`，注册两个内置工具；工具持有注入的注册表引用，不读全局 |
| `html_to_markdown.h/.cpp` | 自研 HTML→Markdown 转换器，零新依赖 |

### 4.1 服务生命周期接口

```cpp
namespace module::mcp {

enum class ServiceState { Stopped, Running, Failed };

struct ServiceStatus {
    ServiceState state = ServiceState::Stopped;
    int port = 0;                 // 运行中 = 实际监听端口；其余状态 0
    std::string lastError;        // Failed 时非空；stop 成功时清空
};

class McpService {
public:
    explicit McpService(
        core::mcp::McpToolRegistry& tools = core::mcp::globalMcpTools());
    ~McpService();
    McpService(const McpService&) = delete;
    McpService& operator=(const McpService&) = delete;

    core::Result<int> start(int port);   // 0 = 临时端口（测试用）；已运行 = 幂等返回当前端口
    void stop();                         // 幂等；清空会话表
    core::Result<int> restart(int port); // stop() → start(port)
    bool isRunning() const;
    int port() const;
    ServiceStatus status() const;        // 加锁快照
};

McpService& globalMcpService();  // 永不析构：static auto& s = *new McpService();（同 core::ToolRegistry）
}
```

实现约束：

- `start` 每次构造**新的** `platform::http::Server`。这是「换端口必须换监听器」的设计选择；同端口停止后重启理论上可复用旧实例，但统一「每次新建」实现更简单、状态更干净（不是「Server 不能复用」）。与 `ProxyRuntime` 的 stop→reset→新建同款。
- 构造注入注册表：默认 `globalMcpTools()`；测试用局部 `McpToolRegistry` 实例化，工具注册互不串味。
- `globalMcpService()` 采用与 `core::ToolRegistry::instance()` 相同的「永不析构」写法：`static auto& s = *new McpService();`。退出钩子里的 atexit `stop` 因此不会访问已析构对象。
- `setAllowedHosts(platform::http::hostWhitelistForBind("127.0.0.1"))`：回环绑定放行三种回环拼写。
- `Server::start()` 只返回 bool，没有失败原因；失败文案统一为「端口 X 被占用或无法监听」，同时记 `lastError` 并返回 `fail(Internal, 同文案)`。
- 状态字段由 `std::mutex` 保护；`status()` 返回拷贝，不暴露内部对象。

### 4.2 会话表

```cpp
namespace module::mcp {

class McpSessionStore {
public:
    using IdGenerator = std::function<std::string()>;
    explicit McpSessionStore(IdGenerator generator = defaultSessionIdGenerator);

    std::string create();                 // 32 位十六进制；超出 64 个先淘汰最旧
    bool has(const std::string& id) const;
    void remove(const std::string& id);
};
}
```

- id 只含可见 ASCII（0x21–0x7E），满足协议对 session id 字符的要求。
- 会话只在内存；服务停止或重启即全部失效（旧 id 一律 404，客户端重新 initialize）。
- 生成器注入仅供测试做确定性 id；生产实现用两次 `std::random_device` 派生的 64 位随机数拼成 32 位十六进制。

### 4.3 协议分发接口

```cpp
namespace module::mcp {

struct HttpOutcome {
    int status = 200;             // 建议的 HTTP 状态码
    std::string body;             // 响应体（JSON-RPC 文本；通知为空）
    bool notificationOnly = false; // true = 202 且无 body、不设 Content-Type
};

class McpProtocol {
public:
    McpProtocol(McpSessionStore& sessions, core::mcp::McpToolRegistry& tools,
                std::string serverVersion);

    // newSessionIdOut：仅在 initialize 成功时写入新 session id，由 handler 写响应头；
    // 其余情况保持空串。sessionId/protocolVersionHeader 传空串 = 请求未带对应头。
    // （HttpOutcome 字段已拍板不含响应头，故新 session id 走 out 参数交给 handler。）
    HttpOutcome handlePost(const std::string& body,
                           const std::string& sessionId,
                           const std::string& protocolVersionHeader,
                           std::string& newSessionIdOut);
};
}
```

### 4.4 HTTP 适配器

```cpp
namespace module::mcp {

class McpHttpHandler {
public:
    explicit McpHttpHandler(McpProtocol& protocol);
    void registerRoutes(platform::http::Server& server);  // POST/GET/DELETE/OPTIONS "/mcp"
};
}
```

- 请求头键已由 `platform::http` 统一小写（`content-type` / `origin` / `host` / `mcp-session-id` / `mcp-protocol-version`），适配器按小写键读取。
- JSON 响应统一设 `Content-Type: application/json`（可带 `; charset=utf-8`）。
- 通知/空 body 的响应不设 Content-Type。

## 5. 协议行为

### 5.1 端点与传输

- 端点：`http://127.0.0.1:<port>/mcp`，方法 POST / GET / DELETE / OPTIONS。
- POST 的 `Content-Type` 必须是 `application/json`（允许 `; charset=...` 后缀，大小写不敏感）；缺失或不符 → **415**（空 body，不进分发）。
- 请求体上限沿用 `platform::http::Server` 的 10MB；超限由 cpp-httplib 直接回 **413**，handler 不可见。
- `Accept` 头本轮不校验（骨架不提供 SSE，客户端按规范会带 `application/json` 与 `text/event-stream`，不校验不影响接入）。
- 响应体统一为 JSON-RPC 文本；本骨架不做服务端主动 SSE 流。

### 5.2 会话

| 情形 | 行为 |
|---|---|
| initialize 成功 | 生成 session id，响应头 `Mcp-Session-Id: <id>` |
| 请求带 `Mcp-Session-Id` 但不在表中 | **404**（空 body） |
| 请求不带 `Mcp-Session-Id` | 按无状态放行（本机单用户服务，便于排障） |
| DELETE 带已知 session id | 移除该会话，**200** |
| DELETE 不带 session id | **200** |
| 服务停止/重启 | 会话表清空，旧 id 一律 404 |

有意宽松：协议建议「需要会话的服务在缺失 id 时应回 400」，本服务选择放行，属于已拍板的排障优先策略。

### 5.3 `MCP-Protocol-Version` 头

- 支持版本：`2025-06-18`（主）与 `2025-03-26`（兼容）。
- 非 initialize 请求带该头且值不在支持列表 → **400**（空 body）。
- 不带该头 → 放行（与规范 SHOULD 假设 `2025-03-26` 不同，见 §14）。

### 5.4 消息分类与错误映射

判定顺序（自上而下，先命中先返回）：

| 输入 | HTTP | 响应体 |
|---|---|---|
| Content-Type 不合法 | 415 | 空 |
| Origin 存在且不允许 | 403 | 空 |
| 带未知 session id | 404 | 空 |
| 非 initialize 且协议版本头不受支持 | 400 | 空 |
| body 不是合法 JSON | 400 | `-32700`（id 为 null） |
| JSON 是数组（批处理） | 400 | `-32600`（id 为 null） |
| JSON 合法但不是合法 JSON-RPC 请求（无 `method`、或带 id 的响应对象） | 400 | `-32600`（id 为 null） |
| 通知（无 `id` 成员，含未知通知） | 202 | 空，`notificationOnly=true` |
| 未知方法 | 200 | `-32601` |
| 参数非法（protocol 层解析不出必需字段/类型） | 200 | `-32602` |
| 工具内部错误（`execute` 返回 fail） | 200 | `-32603` |
| 其他 | 200 | 正常 `result` |

- `id` 必须原样回显（字符串或数字；缺失按上面的通知行处理）。
- 错误对象形状：`{"jsonrpc":"2.0","id":<id|null>,"error":{"code":<code>,"message":"<中文或英文短句>"}}`。
- `initialize` 不校验 params 内容完整性（骨架无会话依赖），除传输/JSON 错误外总是成功。
- `ping` → `result: {}`。

### 5.5 initialize

请求方法 `initialize` 成功时 `result` 固定包含：

```json
{
  "protocolVersion": "2025-06-18",
  "capabilities": { "tools": { "listChanged": false } },
  "serverInfo": { "name": "devdesk", "version": "<DEVDESK_VERSION>" },
  "instructions": "本服务运行在本机，提供网页抓取等工具。"
}
```

- 请求 `params.protocolVersion` 属于支持列表 → 原样回显；不支持/缺失/非字符串 → 回主版本 `2025-06-18`。
- 响应头 `Mcp-Session-Id` 仅在本条响应出现。
- `initialize` 若是通知（无 id）→ 202，不建会话。

### 5.6 tools/list

- `result`: `{"tools":[{"name":..., "description":..., "inputSchema":<对象>}, ...]}`，按注册表 name 字典序。
- 单个工具的 `inputSchemaJson()` 解析失败或不是 JSON 对象 → 跳过该工具 + `LOG_WARN`，其余照常返回。
- `listChanged=false`：本轮不推送工具变更通知。

### 5.7 tools/call

- `params` 必须是对象，`params.name` 必须是非空字符串；否则 `-32602`。
- `params.arguments` 缺失或为 null → 按 `{}` 处理；非对象 → `-32602`。
- 工具名不存在 → `-32602`（message 含工具名）。
- `execute(argumentsJson)` 返回 ok：
  - `isError=false` → `{"content":[{"type":"text","text":"<text>"}],"isError" 字段不出现}`
  - `isError=true` → 同上，但出现 `"isError":true`
- `execute` 返回 fail → `-32603`（工具业务失败必须用 `isError=true` 表达，不能返回 fail）。

### 5.8 方法与路径

| 请求 | 行为 |
|---|---|
| `GET /mcp` | 405，响应头 `Allow: POST, DELETE`（不提供 SSE；会话规则不参与方法级拒绝） |
| `DELETE /mcp` | 见 §5.2：带已知会话 → 移除 + 200；带未知会话 → 404；不带 → 200 |
| `OPTIONS /mcp` | 405（显式注册路由；Allow 同上） |
| 其他路径 | 404（cpp-httplib 默认行为，不额外注册） |

### 5.9 安全

- 只绑定 `127.0.0.1`；连接级来源即本机。
- `Host` 头经 `platform::http::Server::setAllowedHosts` 校验，白名单由绑定地址推导（`127.0.0.1` / `localhost` / `[::1]`），其余 403。
- `Origin` 头存在时：解析 host 部分（忽略 scheme 与端口），仅允许 `127.0.0.1`、`localhost`、`[::1]`、`::1`，否则 403；`Origin: null` 或无法解析同样 403。无 Origin 头放行（非浏览器客户端）。
- 无鉴权 token：本机信任模型，只防「本机其它网页借道伪造请求」，不防本机恶意进程。此点在文档与设置卡说明中明写。

### 5.10 JSON 构造约束（硬约束）

所有进入响应体的字符串——工具正文、`error.message`、工具 `description`、`id` 字符串等——一律经 `platform::json::stringify(platform::json::makeString(...))` 或 JSON 工厂构造，**禁止裸拼接未转义文本**。`tools/list` 的 tools 数组与 `tools/call` 的 content 数组仍按现有惯例拼 JSON 文本片段后用 `platform::json::parse` 校验/规范化（同 `agent_loop.cpp` 的 `parseArrayOrDie` 思路，不在本批新增 JSON 能力），但每个字符串值必须先由上述工厂产出转义后的 JSON 串再接进片段。`id` 原样回显用**解析后的 `JsonValue` 再序列化**，不拿原始字节拼串；JSON-RPC 错误对象同理。任何字符串混入非法 UTF-8 时，先 `platform::utf8::sanitize`（见 §6.1-8），保证 `stringify` 与后续 `parse` 自检都能通过。

## 6. 内置能力（v1 两个）

`registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry, Fetcher fetcher = platform::http::send)`：registry 由调用方注入（生产 = 全局注册表，测试 = 局部注册表）；两个内置工具都持有该引用，**不读 `globalMcpTools()`**，保证测试隔离。

```cpp
using Fetcher = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;
```

### 6.1 `web_fetch_markdown`

输入 schema：

```json
{
  "type": "object",
  "properties": {
    "url": { "type": "string", "description": "http/https 网页地址" },
    "max_chars": { "type": "integer", "description": "输出字符上限，默认 60000，范围 1000..200000" },
    "useProxy": { "type": "boolean", "description": "是否自动探测本地代理（127.0.0.1:7897→10808→51295，取首个可连通者；全部不可连通则直连并附注）。默认 true" },
    "proxy": { "type": "string", "description": "显式代理 host:port（仅 IP:port 或 localhost:port），提供时强制使用；预检失败报错，且不可与 useProxy=false 同给" },
    "main_content": { "type": "boolean", "description": "是否只提取正文主干（剔除导航/页脚/面包屑等样板）。默认 true；false 返回全量内容（与提取功能引入前行为一致）" }
  },
  "required": ["url"]
}
```

**本地代理参数（代理参数批追加，主会话拍板口径）**：

- `useProxy`（boolean，默认 true）：true → 按优先级序探测 `127.0.0.1:7897` → `127.0.0.1:10808` → `127.0.0.1:51295`（TCP 连通探测，单候选阻塞 ≤300ms，探测结果同一次调用内缓存），首个可连通者作为 HTTP 代理；全部不可连通 → 直连，输出末尾追加一行「未检测到本地代理,已直连」。false → 不用代理、不探测、无附注。
- `proxy`（string，可选）：格式仅 `IP:port` 或 `localhost:port`（控 SSRF 面——代理主机是 curl 实连对象）。提供时强制走该代理：先 TCP 预检，失败 → `isError=true`「显式指定的代理 <host:port> 不可用」（不回落直连）；与 `useProxy=false` 同给 → 参数冲突错误（-32602 口径）。
- `main_content`（boolean，默认 true）：true → HTML 转换走正文主干提取（§7 规则 R1–R4，剔除导航/页脚/面包屑等样板）；false → 规则全关断，输出与提取功能引入前基线逐字节一致（保底契约，专项测试锁死）。仅对 HTML 响应生效。
- SSRF 口径不变：代理地址本身（回环）是传输通道，不进抓取目标 deny 表；抓取目标的字面 SSRF 校验与重定向每跳复检完全不变（走代理时亦然）；DNS 层风险仍按 §14.2 接受。

行为：

1. 只接受 `http` / `https`；其他 scheme（`file:`、`ftp:` 等）→ `isError=true`，不触网。
2. **字面地址防护（SSRF）**：URL host 为 `localhost`（大小写不敏感）或字面 IP 命中 `127.0.0.0/8`、`0.0.0.0/8`、`::1`、`[::ffff:127.0.0.1]`、`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`、`169.254.0.0/16`、`fe80::/10`、`fc00::/7` → `isError=true` 并说明原因，不触网；重定向的每一跳目标同样过此检查。DNS 重绑定不在防护内（§14.2 明写接受风险）。
3. GET，**所有重定向跳合计总时限 10 秒**：每跳 `timeoutMs = min(剩余总时限, 10000)`；总时限耗尽 → `isError=true`。
4. **手动**跟随 301/302/303/307/308，最多 5 跳（`platform::http::send` 不跟随重定向）：
   - `Location` 缺失 → `isError=true`「重定向缺少 Location」。
   - `Location` 解析：绝对 `http(s)` 直接使用；`//host/path` 补当前 scheme；`/path` 补当前 scheme+host+port；相对路径相对当前 URL 的目录解析；解析出非 http(s) → `isError=true`。
   - 超过 5 跳 → `isError=true`「重定向次数过多」。
5. 非 2xx → `isError=true`，文本含状态码与响应体前 200 字符（UTF-8 边界安全）。
6. 响应头按**大小写不敏感**查表（`platform::http::send` 返回的头键保留服务端原始大小写，只有 Server 侧入站头被小写化）。
7. Content-Type 判定（取 `;` 前主类型，小写）：
   - `text/html`、`application/xhtml+xml` → 转 Markdown；
   - `text/plain`、`text/markdown`、`application/json`、`text/xml`、`application/xml` → 原文返回；
   - 缺失 Content-Type → 按文本；
   - 其他 → `isError=true`「不支持的内容类型: <值>」。
8. **编码**：charset 白名单 = `utf-8` / `utf8` / `us-ascii` / `ascii`（去除空白与引号、大小写归一后比较）；白名单外任何显式 charset → `isError=true`「不支持非 UTF-8 编码」，不尝试转码；charset 缺失或白名单内 → 先 `platform::utf8::sanitize`（非法字节换 U+FFFD）再转换/返回。所有输出文本（含错误文本、截断附注）返回前必须 sanitize，保证进入 JSON 前是良构 UTF-8。
9. 原始响应体超过 2 MiB：先截断到 2 MiB 再转换/返回，并在输出末尾附注「（原始响应超过 2 MiB，已截断）」。
10. 输出超过 `max_chars`（默认 60000，钳制 1000..200000）：按 UTF-8 边界截断并附注「（已截断到 N 字符）」。
11. HTML 转换后没有可见文本 → **非错误**，返回「页面未提取到文本（可能是 JS 渲染页面）」。
12. 网络失败 → `isError=true`，文本为可读中文 + 底层错误信息。
13. **TLS 信任根覆盖口（CA 批，平台层口径）**：Windows 默认用系统 ROOT 存储（`CURLSSLOPT_NATIVE_CA`）。覆盖口 = 环境变量 `DEVDESK_CA_BUNDLE`（DevDesk 自有约定，指向 CA bundle 文件）：设置且文件存在 → 改用该文件完整校验（`CURLOPT_CAINFO`，不叠加 NATIVE——换信任根，不弱化校验）；设置但路径不存在 → 请求失败并报「DEVDESK_CA_BUNDLE 指向的文件不存在: <路径>」；未设 → 默认策略不变。注意 libcurl **库**不读 curl CLI 的 `CURL_CA_BUNDLE` 约定（实测 8.x 无效果），覆盖口由代码读 env 显式下发。适用于 `send` 与 `sendStreaming` 全部出站请求（含重定向各跳）。

### 6.2 `devdesk_server_info`

- 无参数；schema 为 `{"type":"object","properties":{}}`。
- 返回 Markdown 列表（顺序固定）：
  - DevDesk 版本（`DEVDESK_VERSION`，宏缺失回退 `dev`）；
  - 构建类型（`DEVDESK_BUILD_TYPE`，缺失回退 `unknown`）；
  - 平台名（`DEVDESK_PLATFORM`，缺失回退 `unknown`）；
  - 支持的 MCP 协议版本：`2025-06-18`、`2025-03-26`；
  - 已注册工具数 + 工具名（字典序）。
- 不读其它模块的数据（模块隔离）；工具清单来自**构造时注入的注册表引用**（生产 = 全局注册表，测试 = 局部注册表），不直接调用 `globalMcpTools()`。

## 7. HTML→Markdown 转换器

接口：`std::string htmlToMarkdown(const std::string& html, bool extractMainContent = true);`（输入上限 2 MiB，超出先截断）。

处理规则：

- 剔除：`script`、`style`、`head`（仅保留 `title`）、HTML 注释。
- 块级：`h1`–`h6` → `#`…`######`；`p`、`div` → 段落；`br` → 换行；`hr` → `---`。
- 列表：`ul`/`ol` → `-` / `1.`，嵌套按 2 空格缩进；`li` 内的段落折叠为同一行。
- 引用：`blockquote` → `> ` 前缀（嵌套重复前缀）。
- 代码：`pre` → 围栏代码块（```）；`code` → 行内反引号；围栏语言无法稳定识别时不写语言。
- 链接与图片：`a` → `[text](href)`；`img` → `![alt](src)`；相对地址原样保留。
- 强调：`strong`/`b` → `**`；`em`/`i` → `*`。
- 表格：`table` 基础行列转 pipe 表（首行为表头）；`colspan`/`rowspan` 不解析，按普通单元格拍平；单元格内 `|` 转义为 `\|`。
- 实体：常见命名实体（`&amp;` `&lt;` `&gt;` `&quot;` `&nbsp;` `&#39;` 等）与数字实体（十进制/十六进制）解码。
- 空白：连续空白折叠为单个空格（`pre` 内除外）；最多保留一个空行；行尾空白去掉。
- 标题兜底：`<title>` 非空且正文没有 `h1` 时，在最前面加 `# <title>`。
- 正文主干提取（`extractMainContent`，默认开；规则/词表/阈值出处 = 蓝图 `docs/superpowers/research/2026-10-27-mcp-content-extraction-review.md` §3，照抄不改值）：
  - **R1 语义标签整块剔除**：`header/footer/nav/aside/form/noscript/iframe/svg/template/dialog/button/select`（`Mode::Suppress` 抑制栈，嵌套按层计数；未闭合双保险 = 遇 `article`/`main`/`h1`–`h3` 信号释放 + 抑制区 64 KiB 字节上限硬释放；表格单元格与 `pre` 内不启动）；
  - **R1b class/id 关键词剔除**：负面词表（Readability unlikelyCandidates 高置信子集）命中且无正词（`article/content/main/docs/code` 等保险丝，词表冲突存疑不删）→ 对 `div/section/ul/ol/table/p` 整块抑制；
  - **R2 链接密度**：块 ≤150 字符且链接文字占比 >0.5 且无图 → 整块丢弃；锚链接（`href="#…"`）文字按 0.3 计重（保 TOC）；丢弃随块复位码 span/链接状态并回退有序列表编号；`<main>`/`<article>` 锚定区内暂停；
  - **R3 锚定保护**：`<main>`/`<article>` 进入锚定态——区内暂停 R2，其出现触发 R1 信号释放；
  - **R4 无障碍属性**：容器 `role` 命中词表（menu/menubar/complementary/navigation/alert/alertdialog/dialog/banner/contentinfo/search）或 `aria-hidden="true"`/`hidden`/`display:none`/`visibility:hidden` → 整块抑制（以必有闭合配对的容器为闸，void/自闭合标签不启动防栈滞留）；
  - **关断保底**：`extractMainContent = false` 时以上规则全部短路，输出与引入本参数前的基线**逐字节一致**（差分金样测试锁死，防回归）。

容错与局限（明写）：

- 不是完整 HTML5 解析器；非法/未闭合标签按文本处理，**不抛异常**。
- JS 渲染页面（正文由脚本生成）提取不到内容，返回空文本由工具层转成「未提取到文本」提示。
- 复杂布局（多级表格、`display:flex` 语义、CSS 隐藏元素）不还原视觉结构。

## 8. 设置接入与生命周期

### 8.1 settings_config 增量

`src/module_settings/settings_config.h`：

```cpp
#include "core/mcp_port.h"

inline constexpr const char* kModuleIdMcp = "mcp";
constexpr int kDefaultMcpPort = ::core::kDefaultMcpPort;
constexpr int kMinMcpPort = ::core::kMinMcpPort;
constexpr int kMaxMcpPort = ::core::kMaxMcpPort;

struct SettingsConfig {
    // …既有字段…
    int mcpPort = kDefaultMcpPort;   // 非法值回落默认 8931
};

bool parseMcpPortInput(const std::string& text, int& out);  // 纯数字且在范围内才 true
```

`src/module_settings/settings_config.cpp`：

- `kKnownModuleIds` 加入 `kModuleIdMcp`（否则下次启动被 sanitized 当未知项清掉）。
- `defaultEnabledModules()` **不加入** mcp（默认关闭）。
- `sanitized`：`mcpPort` 越界/0/负数 → 回落 `kDefaultMcpPort`（与 `proxyPort` 同口径，不钳到边界）。
- `save`/`load` 新增顶层键 `mcpPort`：写入行数 17 → 18，并同步该处注释。
- `parseMcpPortInput` 与 `parseProxyPortInput` 同实现口径：只接受范围内纯数字；带符号/空格/非数字 → false。

### 8.2 设置 → 模块卡「MCP 服务」开关（零高度增量）

- **不新增开关卡、不新增卡高常量**：在现有「模块」卡（`settings.card.modules`）标题行右侧新增 `components::toggleSwitch`，与外观卡「动态背景」同款标题右贴先例；不改 2×2 四格数量/尺寸与 `kModulesCardHeight`。
- 开关 id `settings.card.modules.mcp`，文案「MCP 服务」；可点击元素 `<id>.hit`（`EUI-NEO/components/switch.h` 把 onClick 挂在 `.hit`，`.track` 只是视觉轨道；probe 与单测按 `.hit` 定位）。开关盒宽固定 150（130% 字号下「MCP 服务」放得下，与外观卡「动态背景」同款 `kFluidToggleW = 150`）。标题盒按既有做法收窄（`w - 56 - 150`），与开关盒相接不叠印。
- 开关初值 = `moduleEnabled(cfg_, kModuleIdMcp)`；拨动走既有 `applyModuleEnabled(kModuleIdMcp, on)`，不新增链路。

### 8.3 设置 → 通用「MCP 服务」配置卡（模块门控）

- 全宽卡，`CardSpec.gate = kModuleIdMcp`，排在代理卡之后：**模块关闭时整卡隐藏**（不建、不占高度）。默认关闭态通用 Tab 零滚动预算逐值保持（现有 0/1/2 条代理警告三档内容高 338/356/372，可用 372）。模块开启后该卡出现、通用 Tab 允许滚动——这是**已裁决的接受项**（§14.3-1），不新增零滚动断言。
- **滚动内容高缓存键必须签 MCP 门控态**：`settings_module.cpp` 的 `components::scrollView("settings.cards.scroll").contentKey(...)` 现有签名为 `"settings.cards.t" + tab + "." + int(proxyH) + (claudeOn ? "C" : "c") + ".c" + int(pairColW + 0.5f)`；新增 MCP 卡后必须追加 `(mcpOn ? "M" : "m")`（`mcpOn = moduleEnabled(cfg_, kModuleIdMcp)`）。否则「模块 Tab 打开 MCP → 回通用 Tab」会命中旧内容高缓存（默认关闭态 338 高），滚动条不渲染、514 高的开启态内容被裁剪。
- 元素根 `settings.card.mcp`。卡内行序：
  1. 标题「MCP 服务」（16/22）+ 说明（40/16）。
  2. 端口行：`ui_kit::formRow`，label「MCP 端口」，行 y = `mcpPortRowY()`（= `proxyPortRowY()`，100% = 68）、行高 = `logFieldHeight()`（36）。控件组右锚：输入 96×28（`settings.card.mcp.port.input`，placeholder `8931`）、间距 8、「应用」按钮 64×28（`settings.card.mcp.port.apply`）；剩余宽给说明列。
     - 说明列兼作**反馈位**：静态提示「改后回车或点「应用」；运行中自动重启」；空草稿点「应用」→ 红字「请输入端口（1-65535）」；解析失败 → 红字原因；合法但未变化 → 三级字「端口未变化」；成功后回静态提示。输入内容变化即清错（红字回静态提示）。反馈直接由卡片状态渲染，不经 `setStatus`（设置模块的 `setStatus` 已无页头渲染面，不作为用户反馈通道）。
  3. 状态行：label「运行状态」（定宽 110）+ 值（`settings.card.mcp.status`），行 y = 端口行盒底，行高 `mcpStatusRowHeight()`（100% = 18）。文案三态：
     - 运行中 → `运行中 127.0.0.1:<实际端口>`
     - 已停止 → `已停止`
     - 启动失败 → `启动失败：<lastError>`
  4. 接入命令行（`settings.card.mcp.command`）：`claude mcp add --transport http devdesk http://127.0.0.1:<端口>/mcp`。端口取「运行中实际端口」，未运行取已配置端口。行 y = 状态行底 + `roundFs(6)`，行高 `roundFs(16)`，12px 三级文字色；放不下按既有 `fitControlLabel` 口径截断。
- 行几何与卡高同源，统一放 `settings_ui_logic.h`，**卡高不写死 160**：

```cpp
inline float mcpPortRowY() { return proxyPortRowY(); }           // 100% = 68
inline float mcpStatusRowHeight() { return roundFs(18.0f); }     // 100% = 18
inline float mcpCommandRowY() {
    return mcpPortRowY() + logFieldHeight() + mcpStatusRowHeight() + roundFs(6.0f);
}                                                                // 100% = 128
inline float mcpCommandRowHeight() { return roundFs(16.0f); }    // 100% = 16
inline float mcpCardHeightFor() {                                // 100% = 160
    // 下边距下限 16：85% 档 roundFs(16) = 14，不能直接 roundFs(16)
    return mcpCommandRowY() + mcpCommandRowHeight() + std::max(16.0f, roundFs(16.0f));
}
```

- 85%/100%/130% 三档下保持卡内行盒不重叠、下边距 ≥16；允许在该约束内微调。模块开关卡零高度（§8.2），不参与此卡几何。
- 卡高注册**不得**用 `CardHeightKind::Fixed + height = 160`：Fixed 分支会再 `roundFs(160)`（85% 档 = 136，下边距只剩 7）。必须在 `CardHeightKind` 新增 `McpCard`，`cardHeight()` 的 switch 直接 `return mcpCardHeightFor();`（照 `ProxyWarnings` 分支写法）。
- 端口输入是「草稿 + 提交」语义（与代理端口行相同，无独立 PersistOnCommit 抽象）：
  - 输入只改草稿（`mcpPortDraft_`），不落库；
  - 回车或点「应用」→ 解析；空/非法 → 反馈位红字，**不落库**；合法且变化 → 写 `cfg_.mcpPort` → `persist()` → `app::mcp::applyPort(port)`；未变化 → 反馈位三级字「端口未变化」；
  - Esc → 丢弃草稿（回落到已应用端口）；
  - Ctrl+S 显式保存同段提交 mcp 端口草稿：草稿非空且 ≠ 已应用端口 → 走上述「应用」路径；否则与代理端口一样落到 `persist()` + 「已保存」回显。
- 端口修改的生效语义：服务运行中 → 桥内 `restart(newPort)`；未运行 → 只保存，下次启动生效。

### 8.4 app 层桥

新增 `src/mcp_service_bridge.h` + `src/mcp_service_bridge.cpp`，模式照抄 `module_toggle_bridge`（函数指针缝 + 默认桩 + static_assert 签名哨兵）：

```cpp
namespace app::mcp {

struct LiveStatus {
    bool running = false;
    int port = 0;
    std::string lastError;
};

using StartHandler = core::Result<void> (*)(int port);
using StopHandler = void (*)();
using ApplyPortHandler = core::Result<void> (*)(int port);
using StatusHandler = LiveStatus (*)();

// 每个参数传 nullptr = 该槽恢复默认桩；四个槽一起安装（装配期一次调用）。
void setHandlers(StartHandler start, StopHandler stop,
                 ApplyPortHandler applyPort, StatusHandler status);

core::Result<void> start(int port);
void stop();
core::Result<void> applyPort(int port);
LiveStatus status();

}  // namespace app::mcp
```

默认桩（未装配）：

- `start` / `applyPort` → `fail(Internal, "MCP 服务未装配")`；
- `stop` → no-op；
- `status` → `{false, 0, ""}`。

`mcp_service_bridge.cpp` 只依赖 core（`Result`）与标准库，不链接 `module_mcp`——这样设置模块与 probe 编译它时不会产生模块边。

### 8.5 装配与生命周期

`src/main.cpp`：

1. 装配段（`getRouter()` 的 initialized 块内）：
   - `module::mcp::registerBuiltinMcpTools(core::mcp::globalMcpTools());`（一次；内置工具常驻注册表）；
   - `app::mcp::setHandlers(&mcpStartImpl, &mcpStopImpl, &mcpApplyPortImpl, &mcpStatusImpl);`（无捕获函数指针，转发 `globalMcpService()`）；
   - 若 `moduleEnabled(gatingConfigSnapshot(), kModuleIdMcp)` → `globalMcpService().start(cfg.mcpPort)`；启动失败只 `LOG_ERROR`（启动期不弹窗，与代理自启同口径）；
   - `std::atexit([] { module::mcp::globalMcpService().stop(); });`（单例永不析构，退出钩子访问安全）。
2. `src/app_assembly.cpp` 的 `dispatchModuleToggle` 增加 mcp 特例，**必须放在注册表遍历之前**（mcp 不是 Router 页面模块，遍历永远命中不了它）：
   - `on`：从 `SettingsStore` 读当前端口 → `app::mcp::start(cfg.mcpPort)`；失败 → `app::showAppNotice("MCP 服务未启动", 原因)`；
   - `off`：`app::mcp::stop()`；
   - 之后 `return`，不进入页面模块分支。

### 8.6 线程与停机

- `platform::http::Server` 自带 httplib 工作线程；`McpService` 不另起线程。
- 工具 `execute` 在 HTTP worker 执行；设置页读状态是加锁快照（每帧 O(1)）。
- `stop()` 会 join httplib 工作线程；在途工具调用不会被强制中断。抓取工具总时限 10 秒（所有重定向跳合计，§6.1），故 stop/restart 的最坏等待 ≈ 总时限（10 秒）；工具实现必须自行限时（写入接口契约）。
- 退出顺序：mcp `atexit stop` 与 Router `stopAll` 互不依赖；单例永不析构（§4.1），atexit 回调只要求服务对象仍可使用。

## 9. 扩展指南（新能力接入 ≤4 步）

硬判据：接入一个新能力不超过 4 步；超过即接口退化，先修接口。

1. 在任意 `module_*` 内实现 `core::mcp::IMcpTool`（name/description/inputSchemaJson/execute）；工具名一律下划线命名（如 `todo_add`），规避点号在客户端被规范化。
2. 该模块 `onStart` 里 `core::mcp::globalMcpTools().registerTool(std::make_shared<XxxTool>())`，`onStop` 里 `unregisterTool("xxx")`。
3. 为 schema 与 execute 写单测（含参数非法、业务失败 `isError=true`）。
4. 不碰 `module_mcp`。

最小伪代码（仅示意，不进真实源码）：

```cpp
class TodoAddTool final : public core::mcp::IMcpTool {
public:
    std::string name() const override { return "todo_add"; }
    std::string description() const override { return "新增一条待办"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{"title":{"type":"string"}},"required":["title"]})";
    }
    core::Result<core::mcp::ToolResult> execute(const std::string& argsJson) override {
        // 解析 argsJson；业务失败返回 ToolResult{true, "标题不能为空"}
    }
};
// 模块 onStart：
// core::mcp::globalMcpTools().registerTool(std::make_shared<TodoAddTool>());
```

## 10. 测试计划（按批，每批最多编译一次）

通用规则：

- 测试一律 `port 0`（OS 分配临时端口），**禁止固定端口**。
- 定向命令：`bash scripts/build-test-run.sh --filter mcp --no-run`；用例描述含 ASCII 子串 `mcp` 才可被筛到。
- 每条用例注明防的事故；不报 Total/Passed 数字当交付证明。

### 批次 1：core 接缝 + 协议骨架

新增用例文件：`tests/test_core/test_mcp_tool_registry.cpp`、`tests/test_modules/test_mcp_session_store.cpp`、`tests/test_modules/test_mcp_protocol.cpp`、`tests/test_modules/test_mcp_tools_builtin.cpp`（仅 server_info 部分）。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| 注册表 | 非法名/空指针/重名失败；find 未找到；unregister；list 字典序；size；并发 tools/call 期间 unregister（已取得的 shared_ptr keep-alive 不悬垂） | 工具名冲突静默覆盖、退出时访问已析构单例、注销瞬间 use-after-free |
| 会话表 | 32 位十六进制；create/has/remove；65 个时最旧被淘汰；注入生成器 | 会话无限增长、旧会话复活 |
| 协议分发 | initialize 版本协商（支持/不支持/缺失）；session id 仅在 initialize 成功时产生；未知 session 404；无 session 放行；协议版本头不支持 400、缺失放行；通知/未知通知 202；批处理数组 400；非法 JSON -32700；非法请求 -32600；未知方法 -32601；参数非法 -32602；ping；tools/list 字典序 + 非法 schema 跳过；tools/call 成功/isError/未知工具；正文含引号/反斜杠/换行/控制字符的 JSON 转义；id 原样回显 | 客户端握手失败、错误码错位、批处理被误吞、单个坏 schema 拖垮整表、未转义文本造非法 JSON |
| server_info 工具 | 文本含版本/构建类型/平台（`DEVDESK_PLATFORM`）/两个协议版本/工具数与工具名；无参数也可调用；局部注册表注入后只列该表工具 | 工具清单与实际注册脱节、版本号写死、测试串用全局注册表 |

一次构建 + 定向测试。

### 批次 2：抓取能力

新增用例文件：`tests/test_modules/test_mcp_html_to_markdown.cpp`、`tests/test_modules/test_mcp_fetch_tool.cpp`。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| HTML→MD | h1-h6/p/div/br/hr；嵌套 ul/ol 缩进；blockquote；pre/code；a/img；strong/em；基础 table；命名与数字实体；注释/script/style 剔除；title 兜底；未闭合标签；空白折叠；2 MiB 截断 | 把整页标签噪声喂给模型、转换崩溃、表格/列表结构丢失 |
| fetch 工具 | mock fetcher：HTML→MD；max_chars 钳制与 UTF-8 边界截断；301 相对 Location 跟随；超过 5 跳报错；非 2xx 文本含状态码；不支持内容类型；缺失 Content-Type 按文本；二进制拒绝；`file:` scheme 不触网；2 MiB 截断附注；空正文提示；charset=gbk → isError；charset=ascii/utf8 放行；charset 缺失/utf-8 时非法字节被 sanitize；总时限 10 秒（多跳累计）超时 isError；SSRF 字面量（localhost/127.0.0.1/0.0.0.0/::1/[::ffff:127.0.0.1]/10.x/172.16.x/192.168.x/169.254.x/fe80::/fc00::）拒绝且不触网 | 半个 UTF-8 字符导致客户端 400、非 UTF-8 字节混入 JSON、重定向死循环、总时限失控拖慢 stop、本机/内网地址被读、超长正文烧爆上下文 |

一次构建 + 定向测试。

### 批次 3：设置与生命周期

新增用例文件：`tests/test_modules/test_mcp_service.cpp`、`tests/test_modules/test_mcp_settings.cpp`。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| 配置 | `parseMcpPortInput` 边界/符号/空格；默认 8931；默认关闭（不在 defaultEnabledModules）；sanitized 越界回落；未知 ID 被过滤；save/load 往返（键数 18） | 旧库缺键读不出、开关被当未知项清掉、端口脏数据 |
| 桥 | 默认桩 start/applyPort 返回 fail、status 全空、stop 不崩；注入 recording 桩后 start/stop/applyPort/status 路由与计数；注入后复位；`tests/test_main.cpp` 每用例出厂复位清空四槽 | 未装配时设置页崩溃、测试间桩残留串扰 |
| 设置卡 | 模块卡标题右侧开关存在且默认关（点击命中 `settings.card.modules.mcp.hit`，`.track` 仅为视觉轨道）；通用 Tab 配置卡在模块关闭时整卡不存在、开启时存在；状态三态文案；端口反馈位四态（空草稿/非法/未变化/成功）；非法端口不落库；命令串端口来源；85%/100%/130% 三档行盒不重叠且下边距 ≥16 | 卡片挤坏现有布局、错误状态不可见、端口脏数据落库、命令端口与实际不一致 |
| 服务 | port 0 启动返回实际端口；isRunning/port/status；stop 幂等；restart 换端口；端口占用时 fail + lastError；Host 白名单与 Origin 拒绝；在途工具调用时 stop（fake 工具 sleep 300ms）不崩且等待收敛 | 固定端口冲突、重启后旧监听残留、失败无提示、在途调用被硬杀 |

一次构建 + 定向测试。

### 批次 4：集成与收口

新增/修改：`tests/live/mcp_live.cpp`（`DEVDESK_BUILD_LIVE_TOOLS` 门控，Windows/macOS 都编；`--check` / `--serve` 双模式）、`scripts/run-mcp-live.sh`、`tests/CMakeLists.txt`、`README.md`、本 spec 的口径同步。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| 服务级往返（`--check`） | 进程内真 HTTP：initialize（取 session id）→ tools/list → tools/call（`devdesk_server_info`）→ DELETE；未知 session 404；Content-Type 415；GET 405 且 `Allow: POST, DELETE`；OPTIONS 405；通知 202；重启后旧 session 404 | 单测自洽但真实客户端接不上、响应头丢失、方法级拒绝错位 |
| live 抓取 | 真网页 `web_fetch_markdown`（环境不可用则 skip 并记录） | 假 fetcher 全绿、真外网必炸 |
| Claude Code 实连（硬门禁） | `--serve` 起服务（stdout 打印 `PORT=<n>` 后 `fflush(stdout)` 再阻塞）→ `scripts/run-mcp-live.sh` 在**临时空目录**（`mktemp -d`）作为 cwd 运行真实 `claude -p --strict-mcp-config --mcp-config <临时json> --allowedTools mcp__devdesk__web_fetch_markdown --output-format text`（2.1.307+ 已支持 `--strict-mcp-config`，只加载临时配置）发一条明确要求调用抓取工具的提示 → 断言输出含目标网页内容 → 杀 serve；临时 mcp json 的 server key 固定 `devdesk`（与 allowedTools 前缀对齐）；**不设** `CLAUDE_CONFIG_DIR`（认证依赖用户现有环境）；全程临时文件，不碰用户 `~/.claude.json` | 命令文案错、协议方言不兼容、宣称完成但客户端根本连不上 |

一次构建 + 定向测试；最后主会话统一跑全量门禁一次。413 由 `platform::http::Server` 既有用例覆盖，本批不重复。

## 11. 文件清单

新增：

| 文件 |
|---|
| `src/core/mcp_port.h` |
| `src/core/mcp_tool.h`、`src/core/mcp_tool.cpp` |
| `src/module_mcp/CMakeLists.txt` |
| `src/module_mcp/mcp_service.h/.cpp` |
| `src/module_mcp/mcp_session_store.h/.cpp` |
| `src/module_mcp/mcp_protocol.h/.cpp` |
| `src/module_mcp/mcp_http_handler.h/.cpp` |
| `src/module_mcp/mcp_tools_builtin.h/.cpp` |
| `src/module_mcp/html_to_markdown.h/.cpp` |
| `src/mcp_service_bridge.h`、`src/mcp_service_bridge.cpp` |
| `tests/test_core/test_mcp_tool_registry.cpp` |
| `tests/test_modules/test_mcp_session_store.cpp`、`test_mcp_protocol.cpp`、`test_mcp_tools_builtin.cpp`、`test_mcp_html_to_markdown.cpp`、`test_mcp_fetch_tool.cpp`、`test_mcp_service.cpp`、`test_mcp_settings.cpp` |
| `tests/live/mcp_live.cpp`（门控，默认不构建；`--check` / `--serve` 双模式） |
| `scripts/run-mcp-live.sh` |

修改（最小面）：

| 文件 | 动作 |
|---|---|
| `src/core/CMakeLists.txt` | `devdesk_core` 源码加 `mcp_tool.cpp` |
| `CMakeLists.txt` | `add_subdirectory(src/module_mcp)`；DevDesk 链接 `devdesk_module_mcp`；可执行文件源加 `src/mcp_service_bridge.cpp`；覆盖率清单加 `devdesk_module_mcp`；`DevDesk` 的 `target_include_directories` 补 `src/module_mcp`（与其它 module_* 同款） |
| `tests/CMakeLists.txt` | 登记全部新用例文件（漏登记会触发 TIN-10 的 configure 期 FATAL）；`devdesk_tests` 源加 `src/mcp_service_bridge.cpp`；链接 `devdesk_module_mcp`；`devdesk_tests` 与 probe 公共源集的 include 目录补 `src/module_mcp`；probe 公共源集合同步加 `src/mcp_service_bridge.cpp`（否则 settings 模块引用桥符号链接失败）；新增**独立** `if(DEVDESK_BUILD_LIVE_TOOLS)` 块构建 `mcp_live`（Windows/macOS 都编，LABELS live，默认不构建），目标源集含 `live/mcp_live.cpp` + `src/mcp_service_bridge.cpp`（settings/app_assembly 引用其符号）；现有 `if(... AND UNIX)` 的 `github_sync_live` 与 `agent_live`（均链接 `devdesk_module_settings`）源集**各补** `src/mcp_service_bridge.cpp`——settings_module.cpp/app_assembly.cpp 新增 `app::mcp::*` 引用后缺桥符号会 undefined |
| `scripts/check-windows-cross-compile.sh` | `build_incs()` 增加 `-I "${ROOT}/src/module_mcp"`（硬编码 include 清单，不补则 module_mcp 源被记成编不过） |
| `tests/test_main.cpp` | 每用例出厂复位新增桥复位：`app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr)`（四槽全清，防用例桩残留串扰） |
| `src/main.cpp` | 注册内置工具、注入桥、按开关启动、atexit stop |
| `src/app_assembly.cpp` | `dispatchModuleToggle` 增加 mcp 特例（置于注册表遍历前） |
| `src/module_settings/settings_config.h/.cpp` | mcp 模块 ID、端口字段与解析、合法 ID 清单、sanitized、save/load 新键（17→18） |
| `src/module_settings/settings_ui_logic.h` | MCP 配置卡行几何与 `mcpCardHeightFor()`（由 `proxyPortRowY()`/`logFieldHeight()` 求和，不写死）；`mcpCardHeightFor()` 的下边距用 `std::max(16.0f, roundFs(16.0f))`（85% 档 `roundFs(16)=14` 不满足 ≥16）；不新增开关卡常量 |
| `src/module_settings/settings_ui_cards.h/.cpp` | `drawModulesCard` 标题右侧加 MCP 开关（盒宽 150、点击命中 `.hit`）；`drawMcpCard`；`SettingsCardsIn`/`SettingsCardActions` 增加 MCP 字段与回调——新字段一律**追加末尾**（带默认值，兼容既有位置初始化） |
| `src/module_settings/settings_module.cpp` | 通用 Tab 配置卡登记（`gate = kModuleIdMcp`）；模块卡内开关接线（不新增卡）；`mcpPortDraft_`；`applyMcpPort`；Ctrl+S 同段提交；卡内反馈位状态；桥状态快照刷新；`CardHeightKind` 增加 `McpCard`（枚举在本文件匿名命名空间），`cardHeight()` 的 switch 增加 `case CardHeightKind::McpCard: return mcpCardHeightFor();`（照 `ProxyWarnings`）；`scrollView("settings.cards.scroll").contentKey(...)` 加入 `(mcpOn ? "M" : "m")` 签名 |
| `README.md` | 能力清单与模块开关口径同步 |

不动：`src/app_registry.h`（无页面行）、`src/core/module_interface.h`（无新 PageId）、`src/platform/*`（无新平台实现）、EUI-NEO。

## 12. 非目标（明确不做）

- stdio 传输。
- 鉴权 token / 用户体系。
- 服务端主动 SSE 推送（工具变更通知、sampling、roots）。
- prompts / resources 能力（只做 tools）。
- 自动写 Claude Code 配置（只展示命令）。
- 侧边栏页面、Router 页面注册。
- 把现有 todo/markdown 能力直接暴露（留给后续模块自行注册）。
- JSON-RPC 批处理兼容（按新协议移除）。
- 安装包、远端推送。

## 13. 交付边界

- 本轮为**中间里程碑**：合入本地 dev + 构建 + **真实 Claude Code 实连验证**（握手 + 真实调用一次抓取工具）。
- **不出安装包、不 push 远端**（Windows Inno Setup 与 macOS DMG 均不改）。
- 0.5.0 安装包与真机 UAT 仍是项目总目标，不在本轮。
- macOS 仅 CI 编译 + 单测，无真机验证；真实客户端实连在 Windows 开发机完成。
- 真实 Claude Code 实连是**完成硬门禁**，不是可选项；做不到则本轮不得宣称完成。
- 硬门禁的固定执行口径：`claude -p --strict-mcp-config` 在临时空目录 cwd 运行，仅加载临时 `--mcp-config`（server key 固定 `devdesk`）；`--serve` 输出 `PORT=<n>` 后 `fflush(stdout)`；不设 `CLAUDE_CONFIG_DIR`，认证沿用用户现有环境。

## 14. 未验证项与风险 / 待确认

### 14.1 未验证项

1. macOS 真机未验证（仅 CI 编译与单测）。
2. 真实互联网抓取受公司网络与目标站点渲染方式影响；JS 渲染页面提取不到内容。
3. 会话 id 使用非密码学随机（`std::random_device` 派生）；在本机信任模型下只做会话标识，不承担鉴权职责。

### 14.2 已知风险

1. `platform::http::send` **不限制响应体大小**，2 MiB 只能下载完成后再截断：超大页面会整段进入内存。骨架接受此限制；后续可改为流式读取并中途截断。
2. `platform::http::send` 不跟随重定向，所有跳转必须手动处理（设计已含），实现漏一跳会误报 3xx 为成功/失败。
3. `platform::http::send` 返回的响应头键保留服务端原始大小写（如 `Content-Type`、`Location`），工具必须大小写不敏感查表；`platform::http::Server` 的入站头则是小写。
4. `Server::start()` 无失败细粒度原因，只能给出「端口 X 被占用或无法监听」。
5. `Server::stop()` 会等待 httplib 工作线程；在途工具调用不会被强制中断。抓取工具总时限 10 秒，故 stop/restart 最坏等待 ≈10 秒（工具须自行限时，接口契约已写）。
6. 无鉴权：本机任意进程都可调用本服务（本机信任模型，已知并接受）。
7. `MCP-Protocol-Version` 头缺失时放行，与规范「SHOULD 假设 2025-03-26」不一致；属有意宽松，若后续接入严格客户端需重新评估。
8. **DNS 重绑定不在防护内**：抓取工具只拒绝字面 loopback/私网/链路本地地址，域名解析到内网地址不拦。本机信任模型下接受此风险。

### 14.3 待确认与已裁决（不阻塞代码施工，需用户拍板或实现期核实）

1. **通用 Tab 零滚动预算冲突——已裁决（审计修订）**：采用「模块卡标题右侧开关（零高度增量）+ 通用 Tab 配置卡门控」。默认关闭态通用 Tab 零滚动预算逐值保持（0/1/2 条代理警告三档内容高 338/356/372，可用 372）；模块开启后配置卡出现、通用 Tab 出现滚动为**接受项**（不新增零滚动断言，只断言开启态卡存在）。
2. 设计原文中的「PersistOnCommit」在现有代码中无对应抽象；本 spec 按代理端口行的既有行为（输入草稿 + 回车/应用提交 + Esc 丢弃）落笔。若另有所指，需澄清。
3. 通用 Tab 卡高不再写死：`mcpCardHeightFor()` 由行几何求和（`mcpPortRowY()` 直接复用 `proxyPortRowY()`，状态行、命令行与下边距派生，100% = 160）；实现期以 `settings_ui_logic.h` 同源常量与 85%/100%/130% 三档几何断言为准，保持「行盒不重叠、下边距 ≥16」。模块开关卡零高度（不新增卡高常量）。
## 15. UI 批口径（2026-10-28 增补：MCP 菜单页）

本节是后增批次的口径登记，**覆盖前文与之冲突的表述**；未涉及处仍以上文为准。

1. **§12 非目标废止两条**：「侧边栏页面、Router 页面注册」自 UI 批起不再是目标外——MCP 成为 Router 页面模块。
2. **§11「不动」清单松绑**：`src/app_registry.h`（新增 `PageId::Mcp` 行）与 `src/core/module_interface.h`（`PageId::Mcp` 插 Wallpaper 后、Settings 前）自本批起改为「动，最小面」。`../EUI-NEO/**`、`module_settings/**`、B1/B2 冻结文件（service/protocol/http_handler/session_store/tools_builtin）、`mcp_service_bridge.*`（只读调用）冻结不变。
3. **接线不变量**：`dispatchModuleToggle` 的 mcp 分支自本批起**不提前返回**——先启停服务，再落入注册表遍历做页面注册/注销（mcp 既有「页面」与「服务」双重身份，缺任一半即破设置页「关闭 = 侧栏入口消失」的门控承诺）；`main.cpp` `moduleForPage` 补 `PageId::Mcp` 分支与静态实例（缺则页面在生产装配永远拿不到实例、被 `registerOne` 静默跳过）。
4. **页面口径**：页头 + 服务状态行（`app::mcp::status()` 只读，运行中显示 `127.0.0.1:port` / 已停止）；工具卡片网格（`ui.flow`，卡定宽 340——上游坑：flow 子元素必须显式 `size()`）+ 详情弹窗（`components::dialog`，关闭 = 按钮 + Esc + backdrop 三通道，`dialog.h` 的 backdrop `onClick(requestClose)` 已逐行核实）+「试一下」（`IMcpTool::execute` 一律后台线程 + compose 轮询收割，禁 UI 线程直调；结果区弹窗内展示，成功/失败前缀区分）。数据源 = `McpToolRegistry::list()` 快照（模块可注入注册表，生产工厂用全局单例）。空列表 = 页内空态提示；**MCP 关闭态 = 模块门控整页注销**（侧栏入口消失），页内不置灰、不放开关。视觉基准 = 桌面 `mcp-menu-demo.html`：设计 token 对齐、图标改 FA7 码点；demo 的页内开关与关闭置灰**不照抄**；pulse 动画与键盘焦点圈明确不做（后者为 EUI 上游缺口，已拍板砍）。
