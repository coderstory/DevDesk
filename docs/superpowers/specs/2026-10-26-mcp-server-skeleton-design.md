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

1. 新增独立模块 `module_mcp`，默认关闭；设置 → 模块里出现「MCP 服务」开关，打开后本机服务启动。
2. 设置 → 通用里出现「MCP 服务」卡：端口输入、运行状态、Claude Code 接入命令。
3. 首个内置工具 `web.fetch_markdown`：抓取网页并转 Markdown，供 Claude Code 检索互联网信息。
4. 能力接缝放 core（依赖反转）：core 定义接口与注册表，各模块自行实现并注册；`module_mcp` 只做协议、会话、HTTP 与生命周期，不持有业务能力。
5. 不自动修改 Claude Code 配置，只在设置卡里展示接入命令。
6. 平台复用现有 `platform::http::Server`（cpp-httplib）与 `platform::http::send`（libcurl），零新外部依赖。

### 1.2 已锁定决策

| 决策 | 结论 | 含义 |
|---|---|---|
| 模块形态 | 后台服务型模块，**不做侧边栏页面** | 不进 Router 页面表，不做 `compose` 页面 |
| 默认状态 | **默认关闭** | 不加入 `defaultEnabledModules()` |
| 启用入口 | 设置 → 模块「MCP 服务」开关 | 复用 `applyModuleEnabled` 即时生效链路 |
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
   │      mcp_tools_builtin（web.fetch_markdown / devdesk.server_info）
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
    virtual std::string name() const = 0;             // 全局唯一，符合 ^[A-Za-z0-9_.-]{1,128}$
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
| `src/module_mcp/CMakeLists.txt` | 静态库 `devdesk_module_mcp`；链接 core/platform；注入 `DEVDESK_VERSION` / `DEVDESK_BUILD_TYPE` / `DEVDESK_OS_NAME`（Windows=Windows、macOS=macOS，由 CMake 变量给值；业务代码零平台宏） |
| `mcp_service.h/.cpp` | 服务生命周期：`McpService` + `globalMcpService()`；绑定 127.0.0.1、Host 白名单、端口 0 支持 |
| `mcp_session_store.h/.cpp` | 线程安全会话表：32 位十六进制 id，最多 64 个，超出淘汰最旧；`create/has/remove`；构造函数可注入 id 生成器 |
| `mcp_protocol.h/.cpp` | 纯逻辑 JSON-RPC 分发（不碰 socket，可单测）：输入 body + session id + 协议版本头，输出 `HttpOutcome` |
| `mcp_http_handler.h/.cpp` | `RequestCtx`/`ResponseWriter` 适配、Origin 校验、方法路由（POST/GET/DELETE/OPTIONS `/mcp`） |
| `mcp_tools_builtin.h/.cpp` | `registerBuiltinMcpTools(registry, fetcher)`，注册两个内置工具 |
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
    McpService();
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

McpService& globalMcpService();
}
```

实现约束：

- `start` 构造**新的** `platform::http::Server`（`Server` 实例 stop 后不复用，与 `ProxyRuntime` 同款：`stop` 后 `reset` 再新建）。
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

### 5.10 JSON 构造约束

`platform::json` 只有对象 `set` 与只读数组访问，没有数组追加 API。`tools/list` 的 tools 数组与 `tools/call` 的 content 数组按现有惯例处理：拼 JSON 文本片段后用 `platform::json::parse` 校验/规范化（同 `agent_loop.cpp` 的 `parseArrayOrDie` 思路），**不在本批新增 JSON 能力**。

## 6. 内置能力（v1 两个）

`registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry, Fetcher fetcher = platform::http::send)`：

```cpp
using Fetcher = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;
```

### 6.1 `web.fetch_markdown`

输入 schema：

```json
{
  "type": "object",
  "properties": {
    "url": { "type": "string", "description": "http/https 网页地址" },
    "max_chars": { "type": "integer", "description": "输出字符上限，默认 60000，范围 1000..200000" }
  },
  "required": ["url"]
}
```

行为：

1. 只接受 `http` / `https`；其他 scheme（`file:`、`ftp:` 等）→ `isError=true`，不触网。
2. GET，每次请求 10 秒超时（`platform::http::Request::timeoutMs=10000`；每跳独立计时，不设总时限）。
3. **手动**跟随 301/302/303/307/308，最多 5 跳（`platform::http::send` 不跟随重定向）：
   - `Location` 缺失 → `isError=true`「重定向缺少 Location」。
   - `Location` 解析：绝对 `http(s)` 直接使用；`//host/path` 补当前 scheme；`/path` 补当前 scheme+host+port；相对路径相对当前 URL 的目录解析；解析出非 http(s) → `isError=true`。
   - 超过 5 跳 → `isError=true`「重定向次数过多」。
4. 非 2xx → `isError=true`，文本含状态码与响应体前 200 字符（UTF-8 边界安全）。
5. 响应头按**大小写不敏感**查表（`platform::http::send` 返回的头键保留服务端原始大小写，只有 Server 侧入站头被小写化）。
6. Content-Type 判定（取 `;` 前主类型，小写）：
   - `text/html`、`application/xhtml+xml` → 转 Markdown；
   - `text/plain`、`text/markdown`、`application/json`、`text/xml`、`application/xml` → 原文返回；
   - 缺失 Content-Type → 按文本；
   - 其他 → `isError=true`「不支持的内容类型: <值>」。
7. 原始响应体超过 2 MiB：先截断到 2 MiB 再转换/返回，并在输出末尾附注「（原始响应超过 2 MiB，已截断）」。
8. 输出超过 `max_chars`（默认 60000，钳制 1000..200000）：按 UTF-8 边界截断并附注「（已截断到 N 字符）」。
9. HTML 转换后没有可见文本 → **非错误**，返回「页面未提取到文本（可能是 JS 渲染页面）」。
10. 网络失败 → `isError=true`，文本为可读中文 + 底层错误信息。

### 6.2 `devdesk.server_info`

- 无参数；schema 为 `{"type":"object","properties":{}}`。
- 返回 Markdown 列表（顺序固定）：
  - DevDesk 版本（`DEVDESK_VERSION`，宏缺失回退 `dev`）；
  - 构建类型（`DEVDESK_BUILD_TYPE`，缺失回退 `unknown`）；
  - 平台名（`DEVDESK_OS_NAME`，缺失回退 `unknown`）；
  - 支持的 MCP 协议版本：`2025-06-18`、`2025-03-26`；
  - 已注册工具数 + 工具名（字典序）。
- 不读其它模块的数据（模块隔离）；工具清单来自 `core::mcp::globalMcpTools()`。

## 7. HTML→Markdown 转换器

接口：`std::string htmlToMarkdown(const std::string& html);`（输入上限 2 MiB，超出先截断）。

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

### 8.2 设置 → 模块「MCP 服务」卡

- 在模块 Tab 的壁纸卡之后新增**全宽卡**；不改现有 2×2 模块开关格的数量、尺寸与卡高预算。
- 卡内容与壁纸卡同布局：标题「MCP 服务」+ 一行说明 + 右侧 44×24 开关。
- 元素根 `settings.card.mcp_toggle`；开关 `<id>.sw`（命中元素 `<id>.sw.track`）。
- 开关初值 = `moduleEnabled(cfg_, kModuleIdMcp)`；拨动走既有 `applyModuleEnabled(kModuleIdMcp, on)`，不新增链路。
- 说明文案：「本机 MCP 服务，供 Claude Code 等客户端调用工具」。最终文案实现时可微调，不改语义。
- 卡高常量 `kMcpToggleCardHeight = 128.0f`（与壁纸卡同源推导：`roundFs(68) + roundFs(44) + roundFs(16)`，100% 档 = 128）。

### 8.3 设置 → 通用「MCP 服务」卡

- 全宽卡，**恒显**，不受模块开关门控（`CardSpec.gate = nullptr`），排在代理卡之后。
- 元素根 `settings.card.mcp`。卡内行序：
  1. 标题「MCP 服务」（16/22）+ 说明（40/16）。
  2. 端口行：`ui_kit::formRow`，label「MCP 端口」，行 y = `roundFs(68)`、行高 = `logFieldHeight()`（36）。控件组右锚：输入 96×28（`settings.card.mcp.port.input`，placeholder `8931`）、间距 8、「应用」按钮 64×28（`settings.card.mcp.port.apply`）；剩余宽给说明列「改后回车或点「应用」；运行中自动重启」。
  3. 状态行：label「运行状态」（定宽 110）+ 值（`settings.card.mcp.status`），行 y = 端口行盒底，行高 `roundFs(18)`。文案三态：
     - 运行中 → `运行中 127.0.0.1:<实际端口>`
     - 已停止 → `已停止`
     - 启动失败 → `启动失败：<lastError>`
  4. 接入命令行（`settings.card.mcp.command`）：`claude mcp add --transport http devdesk http://127.0.0.1:<端口>/mcp`。端口取「运行中实际端口」，未运行取已配置端口。行 y = 状态行底 + `roundFs(6)`，行高 `roundFs(16)`，12px 三级文字色；放不下按既有 `fitControlLabel` 口径截断。
- 卡高常量 `kMcpCardHeight = 160.0f`（100% 档：端口行 68+36，状态行 18，命令行间隔 6 + 16，下边距 16）。
- 行几何常量与卡高同源，统一放 `settings_ui_logic.h`：

```cpp
inline constexpr float kMcpToggleCardHeight = 128.0f;
inline constexpr float kMcpCardHeight = 160.0f;
inline float mcpPortRowY() { return roundFs(68.0f); }
inline float mcpStatusRowHeight() { return roundFs(18.0f); }
inline float mcpCommandRowY() {
    return mcpPortRowY() + logFieldHeight() + mcpStatusRowHeight() + roundFs(6.0f);
}  // 100% = 128
inline float mcpCommandRowHeight() { return roundFs(16.0f); }
```

- 端口输入是「草稿 + 提交」语义（与代理端口行相同，无独立 PersistOnCommit 抽象）：
  - 输入只改草稿（`mcpPortDraft_`），不落库；
  - 回车或点「应用」→ 解析；非法 → 既有 `setStatus(..., true)` 红字提示，**不落库**；合法且变化 → 写 `cfg_.mcpPort` → `persist()` → `app::mcp::applyPort(port)`；未变化 → 状态行提示未变化；
  - Esc → 丢弃草稿（回落到已应用端口）。
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
   - `std::atexit([] { module::mcp::globalMcpService().stop(); });`（晚于单例构造注册，LIFO 保证先于单例析构执行）。
2. `src/app_assembly.cpp` 的 `dispatchModuleToggle` 增加 mcp 特例，**必须放在注册表遍历之前**（mcp 不是 Router 页面模块，遍历永远命中不了它）：
   - `on`：从 `SettingsStore` 读当前端口 → `app::mcp::start(cfg.mcpPort)`；失败 → `app::showAppNotice("MCP 服务未启动", 原因)`；
   - `off`：`app::mcp::stop()`；
   - 之后 `return`，不进入页面模块分支。

### 8.6 线程与停机

- `platform::http::Server` 自带 httplib 工作线程；`McpService` 不另起线程。
- 工具 `execute` 在 HTTP worker 执行；设置页读状态是加锁快照（每帧 O(1)）。
- `stop()` 会 join httplib 工作线程；在途工具调用不会被强制中断，长任务会拖慢停止——工具实现必须自行限时（写入接口契约）。
- 退出顺序：mcp `atexit stop` 与 Router `stopAll` 互不依赖；mcp 先于单例析构即可。

## 9. 扩展指南（新能力接入 ≤4 步）

硬判据：接入一个新能力不超过 4 步；超过即接口退化，先修接口。

1. 在任意 `module_*` 内实现 `core::mcp::IMcpTool`（name/description/inputSchemaJson/execute）。
2. 该模块 `onStart` 里 `core::mcp::globalMcpTools().registerTool(std::make_shared<XxxTool>())`，`onStop` 里 `unregisterTool("xxx")`。
3. 为 schema 与 execute 写单测（含参数非法、业务失败 `isError=true`）。
4. 不碰 `module_mcp`。

最小伪代码（仅示意，不进真实源码）：

```cpp
class TodoAddTool final : public core::mcp::IMcpTool {
public:
    std::string name() const override { return "todo.add"; }
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
| 注册表 | 非法名/空指针/重名失败；find 未找到；unregister；list 字典序；size | 工具名冲突静默覆盖、退出时访问已析构单例 |
| 会话表 | 32 位十六进制；create/has/remove；65 个时最旧被淘汰；注入生成器 | 会话无限增长、旧会话复活 |
| 协议分发 | initialize 版本协商（支持/不支持/缺失）；session id 仅在 initialize 成功时产生；未知 session 404；无 session 放行；协议版本头不支持 400、缺失放行；通知/未知通知 202；批处理数组 400；非法 JSON -32700；非法请求 -32600；未知方法 -32601；参数非法 -32602；ping；tools/list 字典序 + 非法 schema 跳过；tools/call 成功/isError/未知工具 | 客户端握手失败、错误码错位、批处理被误吞、单个坏 schema 拖垮整表 |
| server_info 工具 | 文本含版本/构建类型/平台/两个协议版本/工具数与工具名；无参数也可调用 | 工具清单与实际注册脱节、版本号写死 |

一次构建 + 定向测试。

### 批次 2：抓取能力

新增用例文件：`tests/test_modules/test_mcp_html_to_markdown.cpp`、`tests/test_modules/test_mcp_fetch_tool.cpp`。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| HTML→MD | h1-h6/p/div/br/hr；嵌套 ul/ol 缩进；blockquote；pre/code；a/img；strong/em；基础 table；命名与数字实体；注释/script/style 剔除；title 兜底；未闭合标签；空白折叠；2 MiB 截断 | 把整页标签噪声喂给模型、转换崩溃、表格/列表结构丢失 |
| fetch 工具 | mock fetcher：HTML→MD；max_chars 钳制与 UTF-8 边界截断；301 相对 Location 跟随；超过 5 跳报错；非 2xx 文本含状态码；不支持内容类型；缺失 Content-Type 按文本；二进制拒绝；`file:` scheme 不触网；2 MiB 截断附注；空正文提示 | 半个 UTF-8 字符导致客户端 400、重定向死循环、本机文件被读、超长正文烧爆上下文 |

一次构建 + 定向测试。

### 批次 3：设置与生命周期

新增用例文件：`tests/test_modules/test_mcp_service.cpp`、`tests/test_modules/test_mcp_settings.cpp`。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| 配置 | `parseMcpPortInput` 边界/符号/空格；默认 8931；默认关闭（不在 defaultEnabledModules）；sanitized 越界回落；未知 ID 被过滤；save/load 往返（键数 18） | 旧库缺键读不出、开关被当未知项清掉、端口脏数据 |
| 桥 | 默认桩 start/applyPort 返回 fail、status 全空、stop 不崩；注入 recording 桩后 start/stop/applyPort/status 路由与计数；注入后复位 | 未装配时设置页崩溃、测试间桩残留串扰 |
| 设置卡 | 模块 Tab 存在开关卡且默认关；通用 Tab 卡恒显（模块关闭时仍可见）；状态三态文案；非法端口不落库；命令串端口来源；85%/100%/130% 三档行盒不重叠且不越卡框 | 卡片挤坏现有布局、错误状态不可见、命令端口与实际不一致 |
| 服务 | port 0 启动返回实际端口；isRunning/port/status；stop 幂等；restart 换端口；端口占用时 fail + lastError；Host 白名单与 Origin 拒绝 | 固定端口冲突、重启后旧监听残留、失败无提示 |

一次构建 + 定向测试。

### 批次 4：集成与收口

新增/修改：`tests/live/mcp_live.cpp`（受 `DEVDESK_BUILD_LIVE_TOOLS` 门控）、`tests/CMakeLists.txt`、`README.md`、本 spec 的口径同步。

| 用例组 | 覆盖 | 防的事故 |
|---|---|---|
| 服务级往返 | 真实 HTTP：initialize（取 session id）→ tools/list → tools/call（server_info）→ DELETE；未知 session 404；Content-Type 415 | 单测自洽但真实客户端接不上、响应头丢失 |
| live 抓取 | 真网页 fetch_markdown（环境不可用则 skip 并记录） | 假 fetcher 全绿、真外网必炸 |
| Claude Code 实连 | 用真实客户端执行接入命令并握手、调用一次工具（收口验证项） | 命令文案错、协议方言不兼容 |

一次构建 + 定向测试；最后主会话统一跑全量门禁一次。

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
| `tests/live/mcp_live.cpp`（门控，默认不构建） |

修改（最小面）：

| 文件 | 动作 |
|---|---|
| `src/core/CMakeLists.txt` | `devdesk_core` 源码加 `mcp_tool.cpp` |
| `CMakeLists.txt` | `add_subdirectory(src/module_mcp)`；DevDesk 链接 `devdesk_module_mcp`；可执行文件源加 `src/mcp_service_bridge.cpp`；覆盖率清单加 `devdesk_module_mcp` |
| `tests/CMakeLists.txt` | 登记全部新用例文件（漏登记会触发 TIN-10 的 configure 期 FATAL）；`devdesk_tests` 源加 `src/mcp_service_bridge.cpp`；链接 `devdesk_module_mcp`；probe 公共源集合同步加 `src/mcp_service_bridge.cpp`（否则 settings 模块引用桥符号链接失败）；live 段新增 `mcp_live`（门控） |
| `src/main.cpp` | 注册内置工具、注入桥、按开关启动、atexit stop |
| `src/app_assembly.cpp` | `dispatchModuleToggle` 增加 mcp 特例（置于注册表遍历前） |
| `src/module_settings/settings_config.h/.cpp` | mcp 模块 ID、端口字段与解析、合法 ID 清单、sanitized、save/load 新键（17→18） |
| `src/module_settings/settings_ui_logic.h` | MCP 两卡卡高与行几何常量 |
| `src/module_settings/settings_ui_cards.h/.cpp` | `drawMcpToggleCard`、`drawMcpCard`；`SettingsCardsIn`/`SettingsCardActions` 增加 MCP 字段与回调 |
| `src/module_settings/settings_module.cpp` | 两张卡登记；`mcpPortDraft_`；`applyMcpPort`；桥状态快照刷新；动作接线 |
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

- 合入本地 dev（feature worktree → 主会话串行合并）+ 构建与定向测试验证。
- **不 push 远端、不出安装包**（Windows Inno Setup 与 macOS DMG 均不改）。
- macOS 仅 CI 编译 + 单测，无真机验证。
- Claude Code 实连作为收口验证项；做不到则在汇报中明写「未验证」。

## 14. 未验证项与风险 / 待确认

### 14.1 未验证项

1. macOS 真机未验证（仅 CI 编译与单测）。
2. Claude Code 真实客户端握手与工具调用（收口验证；若环境不可用则明写未验证）。
3. 真实互联网抓取受公司网络与目标站点渲染方式影响；JS 渲染页面提取不到内容。
4. 会话 id 使用非密码学随机（`std::random_device` 派生）；在本机信任模型下只做会话标识，不承担鉴权职责。

### 14.2 已知风险

1. `platform::http::send` **不限制响应体大小**，2 MiB 只能下载完成后再截断：超大页面会整段进入内存。骨架接受此限制；后续可改为流式读取并中途截断。
2. `platform::http::send` 不跟随重定向，所有跳转必须手动处理（设计已含），实现漏一跳会误报 3xx 为成功/失败。
3. `platform::http::send` 返回的响应头键保留服务端原始大小写（如 `Content-Type`、`Location`），工具必须大小写不敏感查表；`platform::http::Server` 的入站头则是小写。
4. `Server::start()` 无失败细粒度原因，只能给出「端口 X 被占用或无法监听」。
5. `Server::stop()` 会等待 httplib 工作线程；在途工具调用不会被强制中断，长任务会拖慢停止/重启（工具须自行限时）。
6. 无鉴权：本机任意进程都可调用本服务（本机信任模型，已知并接受）。
7. `MCP-Protocol-Version` 头缺失时放行，与规范「SHOULD 假设 2025-03-26」不一致；属有意宽松，若后续接入严格客户端需重新评估。

### 14.3 待确认（不阻塞代码施工，需用户拍板或实现期核实）

1. **通用 Tab 零滚动预算冲突**：现有通用 Tab 在 0/1/2 条代理警告三档的内容高为 338/356/372，可用高 372，是「零滚动」硬约束。新增恒显 MCP 卡（约 160）后必然出现滚动条。可选项：接受滚动 / 把 MCP 卡改挂模块 Tab / 压缩现有卡。本 spec 按已拍板设计写在通用 Tab，布局归属待用户拍板。
2. 设计原文中的「PersistOnCommit」在现有代码中无对应抽象；本 spec 按代理端口行的既有行为（输入草稿 + 回车/应用提交 + Esc 丢弃）落笔。若另有所指，需澄清。
3. 通用 Tab 卡高 `160.0f` 与模块 Tab 卡高 `128.0f` 为 100% 档推导值；实现期以 `settings_ui_logic.h` 同源常量与 85%/100%/130% 三档几何断言为准，允许在保持「行盒不重叠、下边距 ≥16」的前提下微调。