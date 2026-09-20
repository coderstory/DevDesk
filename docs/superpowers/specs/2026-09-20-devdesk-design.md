# DevDesk 设计文档

> **项目代号**：DevDesk
> **类型**：跨平台桌面 GUI 应用（个人瑞士军刀）
> **目标平台**：macOS 27+、Windows 11
> **作者**：纯 AI 开发（用户不熟悉 C++）
> **创建日期**：2026-09-20

---

## 1. 项目概述

DevDesk 是一个面向开发者的桌面工具集，集三大常用工具于一身：

| 工具 | 解决的问题 |
|------|-----------|
| **Markdown 笔记本** | 本地化的笔记系统，文件可直接用外部编辑器打开 |
| **Todo 看板** | 任务管理（看板 + 列表）|
| **Claude 配置切换器** | 多套 Claude API 配置（base URL / token / 模型）的管理 + 一键切换 + 本地 HTTP 代理 |

**为什么这个组合**：
- 笔记、任务、API 切换是开发者日常最高频的三个痛点
- Claude 切换器有强差异化（市面上几乎没有同类 GUI 工具）
- 三者都能用同一个 GUI 框架（EUI-NEO）+ 同一个本地存储后端（SQLite）实现，最大化代码复用

**预期效果**：用户启动 DevDesk 后，左侧侧边栏切换三个工具，右侧主区显示对应界面。所有数据存本地，无云依赖，无网络上报。

---

## 2. 目标与约束

### 2.1 功能性目标

1. ✅ Markdown 笔记本支持文件树浏览、多行编辑、实时预览、全文搜索、标签
2. ✅ Todo 看板支持多列管理、卡片拖拽、优先级、截止日期、标签
3. ✅ Claude 配置切换器支持多 profile 管理、一键切换、连通测试、本地 HTTP 代理（含流式转发）、调用日志、API key 加密存储
4. ✅ 跨平台（macOS 27+、Windows 11）
5. ✅ Home 仪表盘（今日统计、最近笔记）
6. ✅ 设置页面（主题切换、目录配置）
7. ✅ 中文文档 + 中文日志 + 用户操作手册

### 2.2 非功能性目标

| 维度 | 目标 |
|------|------|
| 启动时间 | < 2 秒（macOS） |
| 内存占用 | < 200 MB RSS |
| 帧率 | UI 交互 ≥ 60 FPS |
| 编译时间 | 全量编译 < 60 秒 |
| 二进制大小 | < 5 MB（不含 EUI-NEO）|

### 2.3 约束

- **技术栈**：C++17 + EUI-NEO（不可更换）
- **开发者**：用户不熟悉 C++，纯 AI 开发
- **开发模式**：不需要考虑开发时间，但每个功能完成后必须**运行 + 截屏 + 测试通过**
- **第三方依赖**：尽可能复用 EUI-NEO 自带的库（yyjson、md4c 等）
- **代码风格**：中文注释、详细命名、避免高级 C++ 黑魔法

---

## 3. 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| 语言 | C++17 | 与 EUI-NEO 对齐 |
| GUI 框架 | EUI-NEO | 项目基础 |
| 窗口后端 | GLFW（默认）| 已在 EUI-NEO 中 |
| 渲染后端 | OpenGL（默认 macOS），Vulkan 可选 | macOS 27 可能移除 OpenGL，需保留 Vulkan 切换能力 |
| 构建系统 | CMake 3.14+ | EUI-NEO 已用 |
| 本地数据库 | SQLite3 | 单文件、零配置、跨平台 |
| HTTP 客户端 | libcurl | 跨平台标准 |
| HTTP 服务器 | cpp-httplib（header-only）| 本地代理用，单文件，零依赖 |
| JSON | yyjson（**复用 EUI-NEO 自带**）| 高性能、轻量 |
| Markdown 渲染 | md4c（**复用 EUI-NEO 自带**）| C 标准库、轻量 |
| 单元测试 | doctest（header-only）| 比 GoogleTest 快、单文件 |
| 字体渲染 | FreeType（**复用 EUI-NEO 自带**）| - |
| 加密 | macOS Keychain / Windows DPAPI | 平台原生密钥管理 |
| 截屏 | macOS `screencapture` / Windows PowerShell | 系统自带 |

---

## 4. 架构

### 4.1 架构图

```
                ┌─────────────┐
                │   main.cpp  │
                └──────┬──────┘
                       │
       ┌───────────────┼───────────────┐
       │               │               │
       ▼               ▼               ▼
┌─────────────┐ ┌─────────────┐ ┌─────────────┐
│ module_home │ │module_markdown│ │ module_todo │ ┌─────────────┐
└──────┬──────┘ └──────┬──────┘ └──────┬──────┘ │ module_claude│
       │               │               │        │   _proxy    │
       └───────────────┴───────────────┴────────┴──────┬──────┘
                       │                              │
                       ▼                              ▼
              ┌─────────────────┐         ┌─────────────────┐
              │   core (lib)    │◄────────│   platform(lib) │
              │ - Result        │         │ - http_client   │
              │ - paths         │         │ - http_server   │
              │ - sqlite_db     │         │ - keychain      │
              │ - theme         │         │ - json          │
              │ - router        │         └────────┬────────┘
              │ - logging       │                  │
              └────────┬────────┘                  │
                       │                          │
                       └──────────┬───────────────┘
                                  ▼
                       ┌─────────────────┐
                       │     EUI-NEO     │
                       └─────────────────┘
```

### 4.2 模块依赖规则

- ✅ `main` 依赖所有 `module_*`
- ✅ 所有 `module_*` 依赖 `core` 和 `platform`
- ✅ `platform` 可依赖 `core`（仅依赖通用工具如 `Result`，不依赖业务逻辑）
- ✅ `core` 不依赖 `platform`（core 必须能在无 GUI 环境运行，便于单元测试）
- ✅ `module_*` 之间**不可互相依赖**（必须通过 `core` 的服务层通信）
- ❌ `core` 不可依赖 `platform` 或 `EUI-NEO`
- ❌ `module_*` 不可直接依赖 `EUI-NEO`（通过 `core` 间接使用，保持模块可独立测试）

---

## 5. 顶层目录结构

```
DevDesk/
├── CMakeLists.txt                  # 顶层 CMake
├── README.md                       # 中文项目说明
├── LICENSE
├── .gitignore
├── cmake/
│   ├── Dependencies.cmake          # 找 SQLite3, CURL
│   └── CompilerWarnings.cmake
├── 3rd/
│   ├── EUI-NEO/        # git submodule，指向 ../EUI-NEO（开发时）或上游仓库
│   ├── cpp-httplib/    # git submodule，指向 https://github.com/yhirose/cpp-httplib
│   └── doctest/        # git submodule，指向 https://github.com/doctest/doctest（单头文件）
├── src/
│   ├── main.cpp                    # 应用入口
│   ├── core/                       # 共享核心（所有模块依赖）
│   │   ├── CMakeLists.txt
│   │   ├── result.h / .cpp
│   │   ├── paths.h / .cpp
│   │   ├── sqlite_db.h / .cpp
│   │   ├── logging.h / .cpp
│   │   ├── theme.h / .cpp
│   │   ├── router.h / .cpp
│   │   └── module_interface.h      # Module 抽象接口
│   ├── platform/                   # OS 能力抽象
│   │   ├── CMakeLists.txt
│   │   ├── http_client.h / .cpp
│   │   ├── http_server.h / .cpp
│   │   ├── keychain.h / .cpp       # macOS Keychain / Windows DPAPI
│   │   └── json.h / .cpp
│   ├── module_markdown/
│   │   ├── CMakeLists.txt
│   │   ├── markdown_module.cpp
│   │   ├── markdown_view.cpp
│   │   ├── markdown_editor.cpp
│   │   ├── markdown_renderer.cpp
│   │   ├── markdown_service.cpp
│   │   └── markdown_search.cpp
│   ├── module_todo/
│   │   ├── CMakeLists.txt
│   │   ├── todo_module.cpp
│   │   ├── todo_view.cpp
│   │   ├── todo_board.cpp
│   │   ├── todo_column.cpp
│   │   ├── todo_card.cpp
│   │   └── todo_service.cpp
│   ├── module_claude_proxy/
│   │   ├── CMakeLists.txt
│   │   ├── claude_module.cpp
│   │   ├── claude_view.cpp
│   │   ├── claude_profile_card.cpp
│   │   ├── claude_profile_editor.cpp
│   │   ├── claude_service.cpp
│   │   ├── proxy_server.cpp
│   │   ├── proxy_router.cpp
│   │   ├── upstream_client.cpp
│   │   └── call_logger.cpp
│   └── module_home/
│       ├── CMakeLists.txt
│       └── home_module.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── doctest.h                   # 测试框架单文件
│   ├── test_main.cpp
│   ├── test_core/
│   ├── test_markdown/
│   ├── test_todo/
│   └── test_claude_proxy/
├── scripts/
│   ├── dev.sh                      # build + run
│   ├── test.sh                     # 跑全部测试
│   ├── screenshot.sh               # 自动截屏
│   ├── reset-db.sh                 # 删除 SQLite db
│   └── package.sh                  # 打包（可选）
├── docs/
│   ├── ARCHITECTURE.md             # 架构详细说明（中文）
│   ├── USER_GUIDE.md               # 用户操作手册（中文）
│   ├── CROSS_PLATFORM.md           # 跨平台说明
│   └── screenshots/                # AI 自动截屏存放
└── logs/                           # 运行日志（运行时生成，gitignore）
```

---

## 6. Core 模块详细设计

### 6.1 `result.h` - 错误处理统一约定

```cpp
// src/core/result.h
namespace core {

// 错误码枚举
enum class ErrorCode {
    Ok = 0,
    FileNotFound,
    FilePermissionDenied,
    DatabaseError,
    NetworkError,
    ParseError,
    ValidationError,
    NotFound,
    AlreadyExists,
    Internal,
};

// 通用 Result 容器
template<typename T>
class Result {
public:
    // 构造成功结果
    static Result ok(T value);
    // 构造错误结果
    static Result fail(ErrorCode code, std::string message);
    // 判断是否成功
    bool isOk() const;
    bool isFail() const;
    // 取值（错误时调用会 LOG+FATAL，调用前必须 isOk）
    const T& value() const;
    T valueOr(T defaultValue) const;
    // 取错误
    ErrorCode errorCode() const;
    const std::string& errorMessage() const;
};

} // namespace core
```

**使用约定**：
- 所有可能失败的函数返回 `Result<T>`
- 不抛 C++ 异常（避免对非 C++ 用户不友好的栈追踪）
- 错误信息用中文 + 上下文

### 6.2 `paths.h` - 跨平台路径

```cpp
// src/core/paths.h
namespace core::paths {

// 应用数据目录
//   macOS:   ~/Library/Application Support/DevDesk/
//   Windows: %APPDATA%\DevDesk\
std::filesystem::path appDataDir();

// 配置文件（JSON）
std::filesystem::path configFile();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// Markdown 笔记根目录
std::filesystem::path markdownDir();

// 日志目录
std::filesystem::path logsDir();

// 调用日志文件（Claude 切换器专用）
std::filesystem::path callLogFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths
```

**实现细节**：
- macOS 通过 `NSHomeDirectory()` 或 `std::getenv("HOME")` + 拼接
- Windows 通过 `SHGetKnownFolderPath(FOLDERID_RoamingAppData)` 或 `std::getenv("APPDATA")`

### 6.3 `sqlite_db.h` - SQLite 封装

```cpp
// src/core/sqlite_db.h
namespace core::data {

// 单条迁移
struct Migration {
    int version;
    std::string description;
    std::string sql;  // 多条用 ; 分隔
};

class SqliteDb {
public:
    // 打开数据库（不存在则创建）
    static Result<SqliteDb> open(const std::filesystem::path& file);
    ~SqliteDb();

    // 执行无返回值的 SQL
    Result<void> exec(const std::string& sql);

    // 准备语句
    class Statement {
    public:
        Result<void> bind(int index, int value);
        Result<void> bind(int index, int64_t value);
        Result<void> bind(int index, double value);
        Result<void> bind(int index, const std::string& value);
        Result<void> bind(int index, std::nullptr_t);
        // 执行 + 取出所有行
        Result<std::vector<std::vector<std::any>>> fetchAll();
        // 单步执行（用于批量插入）
        Result<void> step();
    };

    Result<Statement> prepare(const std::string& sql);

    // 事务（自动 commit/rollback）
    Result<void> transaction(std::function<Result<void>()> fn);

    // 运行迁移（幂等）
    Result<void> runMigrations(const std::vector<Migration>& migrations);

    // 当前 schema 版本
    int schemaVersion() const;
};

} // namespace core::data
```

### 6.4 `logging.h` - 日志

```cpp
// src/core/logging.h
namespace core::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

// 设置日志级别
void setLevel(Level level);
// 添加文件输出
void addFileOutput(const std::filesystem::path& file);
// 添加控制台输出
void addConsoleOutput();

// 宏
#define LOG_TRACE(msg) ::core::log::log(::core::log::Level::Trace, msg, __FILE__, __LINE__)
#define LOG_DEBUG(msg) ::core::log::log(::core::log::Level::Debug, msg, __FILE__, __LINE__)
#define LOG_INFO(msg)  ::core::log::log(::core::log::Level::Info,  msg, __FILE__, __LINE__)
#define LOG_WARN(msg)  ::core::log::log(::core::log::Level::Warn,  msg, __FILE__, __LINE__)
#define LOG_ERROR(msg) ::core::log::log(::core::log::Level::Error, msg, __FILE__, __LINE__)
#define LOG_FATAL(msg) ::core::log::log(::core::log::Level::Fatal, msg, __FILE__, __LINE__)

} // namespace core::log
```

**日志格式**：
```
[2026-09-20 14:32:15.123] [INFO ] [module_claude_proxy] 启用 profile: 官方 (https://api.anthropic.com)
```

### 6.5 `theme.h` - 主题 token

```cpp
// src/core/theme.h
namespace core::theme {

// 颜色 token
struct ColorTokens {
    eui::Color background;      // 主背景
    eui::Color surface;        // 卡片表面
    eui::Color surfaceAlt;     // 备选表面
    eui::Color primary;        // 主色（按钮、强调）
    eui::Color primaryHover;
    eui::Color danger;         // 错误/危险
    eui::Color success;        // 成功
    eui::Color warn;           // 警告
    eui::Color textPrimary;    // 主文本
    eui::Color textSecondary;  // 次文本
    eui::Color textMuted;      // 弱化文本
    eui::Color border;         // 边框
    eui::Color borderStrong;
};

// 尺寸 token
struct SizeTokens {
    float spacing;             // 8.0f
    float spacingSmall;        // 4.0f
    float spacingLarge;        // 16.0f
    float radius;              // 6.0f
    float fontSizeSmall;       // 12.0f
    float fontSizeBody;        // 14.0f
    float fontSizeLarge;       // 18.0f
    float fontSizeTitle;       // 24.0f
};

struct Tokens {
    ColorTokens colors;
    SizeTokens sizes;
};

// 当前主题（默认深色）
const Tokens& current();
// 切换主题
void setDark();
void setLight();

} // namespace core::theme
```

### 6.6 `router.h` 和 `module_interface.h` - 模块系统

```cpp
// src/core/module_interface.h
namespace core {

// 页面标识
enum class PageId {
    Home,
    Markdown,
    Todo,
    ClaudeProxy,
};

// 模块接口（每个模块一个实现）
class Module {
public:
    virtual ~Module() = default;

    // 模块标识
    virtual PageId pageId() const = 0;

    // 显示名（侧边栏用）
    virtual std::string displayName() const = 0;

    // 图标字符（用 Unicode 符号）
    virtual std::string iconGlyph() const = 0;

    // 渲染入口
    virtual void compose(eui::Ui& ui, const eui::Screen& screen) = 0;

    // 模块启动时调用一次
    virtual void onStart() {}

    // 模块关闭时调用一次
    virtual void onStop() {}
};

// 路由
class Router {
public:
    Router();

    // 注册模块
    void registerModule(Module* module);

    // 当前页面 Signal
    eui::Signal<PageId>& currentPage();

    // 渲染侧边栏 + 当前模块
    void compose(eui::Ui& ui, const eui::Screen& screen);

    // 启动所有模块
    void startAll();

    // 停止所有模块
    void stopAll();

private:
    std::vector<Module*> modules_;
    eui::Signal<PageId> currentPage_;
};

} // namespace core
```

---

## 7. Platform 模块详细设计

### 7.1 `http_client.h` - libcurl 封装

```cpp
// src/platform/http_client.h
namespace platform::http {

struct Request {
    std::string method;                              // "GET" / "POST"
    std::string url;
    std::map<std::string, std::string> headers;     // {"Content-Type": "application/json"}
    std::string body;
    long timeoutMs = 30000;
    bool verifyTls = true;
};

struct Response {
    int statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    long elapsedMs = 0;
    bool networkError = false;       // true 表示网络层失败（非 HTTP 错误）
    std::string errorMessage;        // 网络错误信息
};

// 同步发送
Result<Response> send(const Request& req);

// 带重试（指数退避）
Result<Response> sendWithRetry(const Request& req, int maxRetries = 3);

// 流式发送（SSE 场景）
class StreamingResponse {
public:
    bool readChunk(std::string& chunk);   // 读到返回 true，流结束返回 false
    int statusCode() const;
    std::string errorMessage() const;
};

Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req);

} // namespace platform::http
```

### 7.2 `http_server.h` - cpp-httplib 封装

```cpp
// src/platform/http_server.h
namespace platform::http {

struct RequestCtx {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string clientAddress;
};

class ResponseWriter {
public:
    void setStatus(int code);
    void setHeader(const std::string& key, const std::string& value);
    void write(const std::string& data);       // 多次调用 = 流式
    void end();
};

using RouteHandler = std::function<void(const RequestCtx&, ResponseWriter&)>;

class Server {
public:
    Server(std::string bindAddress, int port);

    void route(const std::string& method, const std::string& path,
               RouteHandler handler);

    // 阻塞监听（应放在工作线程）
    Result<void> listen();

    // 异步启动（在工作线程）
    Result<void> startAsync();

    void stop();

    bool isRunning() const;

    int actualPort() const;     // 用于获取随机分配的端口
};

} // namespace platform::http
```

### 7.3 `keychain.h` - 平台原生密钥管理

```cpp
// src/platform/keychain.h
namespace platform::keychain {

// 存储密钥（明文 → 加密）
Result<void> storeSecret(const std::string& service, const std::string& account,
                         const std::string& secret);

// 读取密钥（加密 → 明文）
Result<std::string> loadSecret(const std::string& service, const std::string& account);

// 删除密钥
Result<void> deleteSecret(const std::string& service, const std::string& account);

} // namespace platform::keychain
```

**实现**：
- macOS：使用 `Security.framework` 的 `SecKeychainAddGenericPassword` / `SecKeychainFindGenericPassword`
- Windows：使用 `CryptProtectData` / `CryptUnprotectData` (DPAPI)

### 7.4 `json.h` - yyjson 薄封装

```cpp
// src/platform/json.h
namespace platform::json {

// 解析
Result<JsonValue> parse(const std::string& text);

// 序列化
Result<std::string> stringify(const JsonValue& value, bool pretty = false);

class JsonValue {
public:
    std::string asString() const;
    int asInt() const;
    double asDouble() const;
    bool asBool() const;
    bool isString() const;
    bool isInt() const;
    bool isObject() const;
    bool isArray() const;

    // 对象操作
    bool has(const std::string& key) const;
    JsonValue operator[](const std::string& key) const;
    std::vector<std::string> keys() const;

    // 数组操作
    int size() const;
    JsonValue operator[](int index) const;

    // 构造
    static JsonValue object();
    static JsonValue array();
    static JsonValue string(const std::string& s);
    static JsonValue number(int n);
    static JsonValue number(double n);
    static JsonValue boolean(bool b);
    static JsonValue null();

    JsonValue& set(const std::string& key, JsonValue v);
    void append(JsonValue v);
};

} // namespace platform::json
```

---

## 8. 功能模块详细设计

### 8.1 模块：Markdown 笔记本

#### 数据模型

```sql
-- 索引表（正文存在文件系统）
CREATE TABLE markdown_index (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT UNIQUE NOT NULL,            -- 相对路径，如 "inbox/ideas.md"
    title TEXT,
    size_bytes INTEGER NOT NULL,
    modified_at INTEGER NOT NULL,         -- Unix 时间戳
    created_at INTEGER NOT NULL
);

-- 标签表
CREATE TABLE markdown_tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
);

-- 笔记-标签关联
CREATE TABLE markdown_note_tags (
    note_id INTEGER NOT NULL,
    tag_id INTEGER NOT NULL,
    PRIMARY KEY (note_id, tag_id),
    FOREIGN KEY (note_id) REFERENCES markdown_index(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id) REFERENCES markdown_tags(id) ON DELETE CASCADE
);

-- 全文搜索（FTS5）
CREATE VIRTUAL TABLE markdown_fts USING fts5(
    path UNINDEXED,
    title,
    content,
    tokenize = 'unicode61 remove_diacritics 2'
);
```

#### 文件存储

- 默认根目录：`~/Library/Application Support/DevDesk/notes/` (macOS) 或 `%APPDATA%\DevDesk\notes` (Windows)
- 子目录用文件夹层级表达
- 文件名用 UTF-8，扩展名 `.md`

#### 服务层 API

```cpp
// src/module_markdown/markdown_service.h
namespace module::markdown {

struct NoteMeta {
    int64_t id;
    std::filesystem::path relativePath;
    std::string title;
    int64_t sizeBytes;
    std::time_t modifiedAt;
    std::vector<std::string> tags;
};

class MarkdownService {
public:
    explicit MarkdownService(core::data::SqliteDb& db,
                            const std::filesystem::path& rootDir);

    // 扫描根目录，建立/更新索引
    Result<void> rescan();

    // 列出所有笔记（按修改时间倒序）
    Result<std::vector<NoteMeta>> listAll();

    // 按路径前缀列子目录
    Result<std::vector<NoteMeta>> listInDirectory(
        const std::filesystem::path& dir);

    // 读取笔记内容
    Result<std::string> read(const std::filesystem::path& relativePath);

    // 写入笔记
    Result<void> write(const std::filesystem::path& relativePath,
                       const std::string& content);

    // 新建笔记
    Result<void> create(const std::filesystem::path& relativePath);

    // 删除笔记
    Result<void> remove(const std::filesystem::path& relativePath);

    // 全文搜索
    Result<std::vector<NoteMeta>> search(const std::string& query);

    // 标签管理
    Result<std::vector<std::string>> listAllTags();
    Result<void> setTags(const std::filesystem::path& relativePath,
                         const std::vector<std::string>& tags);

private:
    // 内部辅助
    Result<void> rebuildFtsIndex();  // 全量重建 FTS
    Result<void> updateIndex(const std::filesystem::path& file);
};

} // namespace module::markdown
```

#### UI 布局

```
┌────────────────────────────────────────────────────┐
│ DevDesk / Markdown 笔记本          [⚙] [↻ 重扫]   │
├────────────┬─────────────────┬─────────────────────┤
│ 文件树      │ 编辑器           │ 预览                │
│            │                 │                     │
│ ▾ 📁 inbox │ # 你好          │ 你好                │
│   📄 ideas │                 │ ─────               │
│   📄 todo  │ 这是段落...     │                     │
│ ▾ 📁 work  │                 │ 这是段落...         │
│   📄 设计稿 │                 │                     │
│            │                 │ ```                  │
│ [+ 新建]   │                 │ 代码块              │
│ [🔍 搜索]   │                 │ ```                  │
│            │                 │                     │
│ 标签:      │                 │                     │
│ #工作      │                 │                     │
│ #学习      │                 │                     │
└────────────┴─────────────────┴─────────────────────┘
```

**用户操作**（USER_GUIDE.md 写法）：
1. 点击左下「+ 新建」→ 输入文件名 → 自动打开编辑器
2. 在中间编辑 Markdown，右侧实时渲染
3. `Cmd+S`（macOS）/ `Ctrl+S`（Windows）保存
4. 顶部搜索框输入关键词 → 显示匹配的文件列表
5. 点击文件树的文件 → 加载到编辑器
6. 点击标签 → 过滤显示同标签的文件
7. 文件存在系统目录中，可用任意外部编辑器直接打开

### 8.2 模块：Todo 看板

#### 数据模型

```sql
CREATE TABLE todo_columns (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    position INTEGER NOT NULL,
    color TEXT,                              -- 列颜色 token 名称
    created_at INTEGER NOT NULL
);

CREATE TABLE todo_tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    column_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    description TEXT,
    priority INTEGER NOT NULL DEFAULT 0,    -- 0=无 1=低 2=中 3=高
    due_at INTEGER,
    tags TEXT,                               -- JSON 数组
    position INTEGER NOT NULL,               -- 列内排序
    completed_at INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY (column_id) REFERENCES todo_columns(id) ON DELETE CASCADE
);

CREATE INDEX idx_tasks_column ON todo_tasks(column_id, position);
CREATE INDEX idx_tasks_due ON todo_tasks(due_at) WHERE due_at IS NOT NULL;
```

#### 默认数据

应用首次启动时自动创建默认列：
1. 待办
2. 进行中
3. 已完成
4. 已归档

#### 服务层 API

```cpp
// src/module_todo/todo_service.h
namespace module::todo {

enum class Priority { None = 0, Low = 1, Medium = 2, High = 3 };

struct Column {
    int64_t id;
    std::string title;
    int position;
    std::optional<std::string> color;
};

struct Task {
    int64_t id;
    int64_t columnId;
    std::string title;
    std::optional<std::string> description;
    Priority priority;
    std::optional<std::time_t> dueAt;
    std::vector<std::string> tags;
    int position;
    std::optional<std::time_t> completedAt;
    std::time_t createdAt;
    std::time_t updatedAt;
};

class TodoService {
public:
    explicit TodoService(core::data::SqliteDb& db);

    // 列操作
    Result<std::vector<Column>> listColumns();
    Result<Column> createColumn(const std::string& title);
    Result<void> renameColumn(int64_t columnId, const std::string& newTitle);
    Result<void> deleteColumn(int64_t columnId);  // 级联删除任务
    Result<void> reorderColumns(const std::vector<int64_t>& orderedIds);

    // 任务操作
    Result<std::vector<Task>> listTasks(int64_t columnId);
    Result<std::vector<Task>> listAllTasks();  // 含所有列
    Result<Task> createTask(int64_t columnId, const std::string& title);
    Result<void> updateTask(const Task& task);
    Result<void> deleteTask(int64_t taskId);
    Result<void> moveTask(int64_t taskId, int64_t toColumnId, int newPosition);
    Result<void> completeTask(int64_t taskId);

    // 查询
    Result<std::vector<Task>> searchTasks(const std::string& query);
    Result<std::vector<Task>> listOverdueTasks();
};

} // namespace module::todo
```

#### UI 布局

```
┌────────────────────────────────────────────────────────────┐
│ DevDesk / Todo 看板  [⌘N 新建] [🔍 搜索]    [+ 列] [⚙]   │
├──────────┬──────────────┬──────────────┬──────────────────┤
│ 待办 (3) │ 进行中 (2)   │ 已完成 (5)   │ 已归档            │
│ ┌──────┐ │ ┌──────────┐ │ ┌──────────┐ │                  │
│ │写文档 │ │ │修复bug#42│ │ │初次部署  │ │                  │
│ │🔴高   │ │ │          │ │ │          │ │                  │
│ │⏰明天 │ │ │🟡中      │ │ │✓ 9/15    │ │                  │
│ └──────┘ │ └──────────┘ │ └──────────┘ │                  │
│ ┌──────┐ │ ┌──────────┐ │              │                  │
│ │会议纪要│ │ │性能优化  │ │              │                  │
│ └──────┘ │ └──────────┘ │              │                  │
└──────────┴──────────────┴──────────────┴──────────────────┘
```

**用户操作**：
1. 进入页面看到 4 个默认列
2. `Cmd+N` / `Ctrl+N` 快速新建任务（在第一列）
3. 鼠标拖拽卡片跨列移动
4. 单击卡片打开详情面板（编辑标题、描述、优先级、截止日期、标签）
5. 右键卡片 → 快捷菜单（编辑/删除/移动到...）
6. 点击列标题 → 重命名 / 改色 / 删除

### 8.3 模块：Claude 配置切换器

#### 数据模型

```sql
CREATE TABLE claude_profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL,            -- "官方" / "第三方A" / "测试"
    base_url TEXT NOT NULL,                -- "https://api.anthropic.com"
    api_key_encrypted BLOB NOT NULL,       -- 加密后的 token
    api_key_hint TEXT NOT NULL,            -- 显示用，如 "sk-ant-***xyz"
    model TEXT NOT NULL,                   -- "claude-sonnet-5"
    is_active INTEGER NOT NULL DEFAULT 0,  -- 全局唯一
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

-- 保证 is_active 唯一（部分索引）
CREATE UNIQUE INDEX idx_claude_active
    ON claude_profiles(is_active) WHERE is_active = 1;

CREATE TABLE claude_call_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL,
    method TEXT NOT NULL,
    path TEXT NOT NULL,
    status_code INTEGER,
    latency_ms INTEGER,
    request_bytes INTEGER,
    response_bytes INTEGER,
    error TEXT,
    started_at INTEGER NOT NULL,
    FOREIGN KEY (profile_id) REFERENCES claude_profiles(id) ON DELETE CASCADE
);

CREATE INDEX idx_call_logs_started ON claude_call_logs(started_at DESC);
CREATE INDEX idx_call_logs_profile ON claude_call_logs(profile_id, started_at DESC);
```

#### 服务层 API

```cpp
// src/module_claude_proxy/claude_service.h
namespace module::claude {

struct Profile {
    int64_t id;
    std::string name;
    std::string baseUrl;
    std::string apiKeyPlain;       // 仅在使用时存在于内存
    std::string apiKeyHint;
    std::string model;
    bool isActive;
    std::time_t createdAt;
    std::time_t updatedAt;
};

class ClaudeService {
public:
    explicit ClaudeService(core::data::SqliteDb& db,
                          platform::keychain::Keychain& keychain);

    // Profile CRUD
    Result<std::vector<Profile>> listProfiles();
    Result<Profile> getProfile(int64_t id);
    Result<Profile> createProfile(const std::string& name,
                                   const std::string& baseUrl,
                                   const std::string& apiKey,
                                   const std::string& model);
    Result<void> updateProfile(const Profile& profile);
    Result<void> deleteProfile(int64_t id);

    // 激活
    Result<Profile> getActiveProfile();
    Result<void> setActiveProfile(int64_t id);

    // 连通测试（发送 GET /v1/models）
    Result<platform::http::Response> testConnection(const Profile& profile);

    // 加密辅助
    Result<std::string> encryptApiKey(const std::string& plainKey);
    Result<std::string> decryptApiKey(const std::string& encrypted);

private:
    // 内部：加载时把加密字段解密到内存（apiKeyPlain）
    Result<Profile> loadProfileFromRow(const SqliteRow& row);
};

} // namespace module::claude
```

#### 代理服务器架构

```
┌──────────────────┐
│ Claude Code       │
│ (或其他客户端)    │
└────────┬─────────┘
         │ HTTP POST http://127.0.0.1:8888/v1/messages
         │ Headers: x-api-key: sk-ant-xxx, anthropic-version: 2023-06-01
         │ Body: JSON {model, messages, stream: true, ...}
         ▼
┌──────────────────────────────────────────────┐
│ DevDesk 代理 (cpp-httplib, 工作线程)         │
│                                              │
│ proxy_router.cpp:                            │
│   1. 接收请求                                │
│   2. 调用 ClaudeService::getActiveProfile()  │
│   3. 用 profile 信息改写请求：                │
│      - URL: base_url + path                  │
│      - Header: x-api-key: profile.apiKey     │
│   4. upstream_client.cpp 转发                │
│   5. 流式 chunk 透传回客户端                  │
│   6. call_logger.cpp 记录调用                │
└────────┬─────────────────────────────────────┘
         │ HTTP POST https://api.anthropic.com/v1/messages
         │ Headers: x-api-key: <active token>
         ▼
┌──────────────────┐
│ Claude API 上游   │
└──────────────────┘
```

#### UI 布局

```
┌────────────────────────────────────────────────────────────┐
│ DevDesk / Claude 切换器  🟢运行中 :8888  [⏸ 停止] [⚙]    │
├────────────────────┬───────────────────────────────────────┤
│ Profile 列表         │ 当前 Profile 详情                     │
│                    │                                       │
│ ● 官方 [active]    │ 名称: 官方                            │
│   sk-ant-***xyz    │ Base URL: https://api.anthropic.com   │
│   claude-sonnet-5  │ Token: sk-ant-***xyz (已脱敏)        │
│                    │ 模型: claude-sonnet-5                 │
│ ○ 第三方A           │                                       │
│   sk-***abc         │ [启用] [测试连通] [编辑] [删除]       │
│   claude-opus-5    │                                       │
│                    │ ─────────────────────────              │
│ ○ 测试账号          │                                       │
│   sk-***test       │ 最近调用                              │
│   claude-haiku-5   │                                       │
│                    │ 14:32 POST /v1/messages 200 1247ms 1.2K│
│ [+ 新建 Profile]   │ 14:30 POST /v1/messages 200 832ms  0.8K│
│                    │ 14:25 POST /v1/messages 429 312ms  -  │
│                    │ 14:20 POST /v1/messages 200 1102ms 1.1K│
│                    │                                       │
│                    │ [查看全部日志]                         │
└────────────────────┴───────────────────────────────────────┘
```

**用户操作**：
1. 首次进入 → 点击「+ 新建 Profile」→ 填写：
   - 名称（必填）
   - Base URL（必填，默认 `https://api.anthropic.com`）
   - API Key（必填，密码框）
   - 模型（必填，默认 `claude-sonnet-5`）
2. 点击「测试连通」→ 发送 `GET /v1/models` → 显示状态码 + 耗时
3. 点击「启用」→ 该 Profile 高亮，代理自动重启读取新配置
4. Claude Code 配置：`ANTHROPIC_BASE_URL=http://127.0.0.1:8888`
5. 所有调用实时显示在「最近调用」列表
6. 点击「查看全部日志」→ 弹出完整日志面板（支持过滤、搜索）

### 8.4 模块：Home 仪表盘

#### 数据获取

不存独立数据，从其他模块聚合：

```cpp
namespace module::home {

struct DashboardStats {
    int todayMarkdownCount;
    int weekMarkdownCount;
    int pendingTodoCount;
    int overdueTodoCount;
    int totalClaudeCalls;
    int todayClaudeCalls;
    std::vector<module::markdown::NoteMeta> recentNotes;  // 最近 5 个
};

class HomeService {
public:
    HomeService(module::markdown::MarkdownService& md,
                module::todo::TodoService& todo,
                module::claude::ClaudeService& claude);

    Result<DashboardStats> getStats();
};

}
```

#### UI 布局

```
┌──────────────────────────────────────────────────────────┐
│ DevDesk                                              [⚙] │
│                                                          │
│ 欢迎回来                                                  │
│ 今天是 2026 年 9 月 20 日 星期六                          │
│                                                          │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐    │
│ │ 今日笔记  │ │ 待办任务  │ │ 逾期任务  │ │ API 调用  │    │
│ │    3     │ │    7     │ │    1     │ │   142    │    │
│ └──────────┘ └──────────┘ └──────────┘ └──────────┘    │
│                                                          │
│ 最近编辑                                                  │
│ • 设计稿v2.md (2 小时前)                                 │
│ • 周会纪要.md (昨天)                                     │
│ • bug分析.md (3 天前)                                   │
│                                                          │
│ 快捷操作                                                  │
│ [+ 新建笔记] [+ 新建任务] [📋 切换 Claude Profile]        │
└──────────────────────────────────────────────────────────┘
```

### 8.5 设置页面

通过侧边栏底部齿轮图标进入。

**设置项**：
1. **主题**：深色 / 浅色 / 跟随系统
2. **笔记目录**：路径输入框 + 「打开目录」按钮
3. **Claude 代理端口**：数字输入框
4. **Claude 代理地址**：文本输入框（用于给 Claude Code 配置）
5. **日志级别**：下拉框（Trace / Debug / Info / Warn / Error）
6. **关于**：版本号、许可证、依赖列表

---

## 9. 跨平台策略

### 9.1 平台差异抹平点

| 关注点 | macOS | Windows |
|--------|-------|---------|
| 应用数据目录 | `~/Library/Application Support/DevDesk/` | `%APPDATA%\DevDesk\` |
| 配置目录 | 同上 | 同上 |
| 快捷键修饰键 | `Cmd` | `Ctrl` |
| 路径分隔符 | `/` | `\`（代码中统一用 `std::filesystem::path`） |
| 字体 | 系统默认（San Francisco） | 系统默认（Segoe UI） |
| 密钥存储 | Keychain | DPAPI |
| 系统托盘 | 菜单栏图标 | 系统托盘 |
| 进程启动 | `posix_spawn` | `CreateProcess` |

### 9.2 平台抽象层

所有平台特定代码集中在 `src/platform/` 下，业务代码（`src/core/`、`src/module_*/`）通过接口调用，**不直接使用 `#ifdef __APPLE__` 之类的宏**。

### 9.3 测试矩阵

| 测试类型 | macOS | Windows |
|----------|-------|---------|
| 编译 | ✅ 开发机本地 | ⚠️ 通过 CI 或用户手测 |
| 单元测试 | ✅ 本地跑 | ⚠️ 远程跑（GitHub Actions） |
| 集成测试 | ✅ 本地跑 | ⚠️ 远程跑 |
| UI 截屏 | ✅ `screencapture` | ⚠️ PowerShell + Selenium-like |

**Windows 限制**：开发者只有 macOS，Windows 验证通过 CI（GitHub Actions windows-latest runner）+ 用户手测反馈。

---

## 10. 数据流

### 10.1 应用启动

```
main()
  │
  ├─► core::paths::ensureDirectoriesExist()
  │     ├─ 创建 Application Support/DevDesk/
  │     ├─ 创建 notes/
  │     └─ 创建 logs/
  │
  ├─► core::log::addFileOutput(logs/devdesk.log)
  ├─► core::log::addConsoleOutput()
  │
  ├─► core::data::SqliteDb::open(sqliteDbFile())
  │     └─► runMigrations(ALL_MIGRATIONS)
  │
  ├─► 创建所有模块实例
  │     ├─ HomeModule(homeService, todoService, markdownService, claudeService)
  │     ├─ MarkdownModule(markdownService)
  │     ├─ TodoModule(todoService)
  │     └─ ClaudeProxyModule(claudeService, proxyServer)
  │
  ├─► Router::registerModule(* each module)
  ├─► Router::startAll()
  │     └─ ClaudeProxyModule::onStart() → 启动代理工作线程
  │
  ├─► eui_neo::run(app::dslAppConfig(), [&](ui, screen) {
  │       router.compose(ui, screen);
  │     })
  │
  └─► 退出时：
        ├─ Router::stopAll()
        ├─ 停止代理
        ├─ 关闭 db
        └─ flush 日志
```

### 10.2 一次 Markdown 编辑保存

```
用户在编辑器按 Cmd+S
  ↓
EUI-NEO 触发 keypress 事件
  ↓
MarkdownEditor.cpp: 保存回调
  ↓
MarkdownService::write(relativePath, content)
  ├─ 写文件到磁盘
  ├─ 更新 markdown_index 表
  ├─ 更新 markdown_fts 索引
  └─ 返回 Result<void>
  ↓
UI 显示「已保存」提示（3秒后消失）
  ↓
LOG_INFO("保存笔记: inbox/ideas.md, 1234 字节")
```

### 10.3 一次 Todo 拖拽

```
用户鼠标按下卡片
  ↓
EUI-NEO 触发 mouseDown
  ↓
TodoCard.cpp: 记录拖拽开始
  ├─ 保存源 column_id, task_id
  ├─ 显示半透明拖拽预览
  └─ 注册 mouseMove / mouseUp
  ↓
用户拖动到目标列
  ├─ 实时显示目标列高亮
  └─ 显示放置位置预览
  ↓
用户释放鼠标
  ├─ TodoService::moveTask(taskId, targetColumnId, newPosition)
  │   ├─ UPDATE todo_tasks SET column_id, position
  │   └─ 调整其他任务的 position
  ├─ 刷新 UI
  └─ LOG_INFO("移动任务: id=42 → column=3, position=1")
```

### 10.4 一次 Claude API 代理调用

```
Claude Code: POST http://127.0.0.1:8888/v1/messages
  ↓
cpp-httplib Server 接收
  ↓
proxy_router.cpp: 路由 /v1/messages
  ├─ 读取 RequestCtx（method, path, body, headers）
  ├─ 记录开始时间戳
  ├─ claudeService.getActiveProfile() → Profile
  ├─ 构造转发请求：
  │   - url = profile.baseUrl + path
  │   - headers = 原始 headers + 重写 x-api-key
  ├─ upstream_client.cpp: sendStreaming(req)
  │   └─ libcurl 流式发起
  ↓
上游响应流式到达
  ├─ chunk by chunk 写入 ResponseWriter
  ├─ ResponseWriter.write(chunk) → 通过 cpp-httplib 流式发给 Claude Code
  ├─ 累计接收字节数
  └─ 错误时立即关闭连接 + LOG_ERROR
  ↓
流结束
  ├─ call_logger.cpp: 写入 claude_call_logs 表
  │   ├─ profile_id, method, path, status_code, latency_ms, bytes
  │   └─ 错误时 error 字段记录
  └─ ResponseWriter.end()
```

---

## 11. 错误处理约定

### 11.1 统一规则

1. **不抛 C++ 异常**：所有可能失败的函数返回 `core::Result<T>`
2. **错误必有中文消息**：`Result::fail(ErrorCode, "具体的上下文错误信息")`
3. **错误必须 LOG**：调用方收到错误时必须 `LOG_ERROR` 一条
4. **错误必须上抛 UI**：UI 层收到错误必须显示 toast 或对话框
5. **不可恢复错误 LOG_FATAL**：例如数据库无法打开

### 11.2 错误码分类

```cpp
enum class ErrorCode {
    Ok = 0,

    // 文件系统
    FileNotFound,              // 文件不存在
    FilePermissionDenied,      // 权限不足
    FileWriteFailed,           // 写入失败（磁盘满、IO 错误）
    FileReadFailed,

    // 数据库
    DatabaseOpenFailed,        // db 文件打不开
    DatabaseCorrupted,         // db 文件损坏
    DatabaseMigrationFailed,

    // 网络
    NetworkUnreachable,        // 主机不可达
    NetworkTimeout,            // 超时
    NetworkTlsFailed,          // TLS 握手失败
    HttpError,                 // HTTP 错误（4xx/5xx）

    // 解析
    JsonParseError,
    MarkdownParseError,

    // 业务
    ProfileNotFound,
    ProfileAlreadyExists,
    NoActiveProfile,           // Claude 切换器：没有激活的 profile
    InvalidApiKey,             // API key 格式不对

    // 通用
    ValidationError,           // 输入校验失败
    NotFound,
    AlreadyExists,
    NotImplemented,
    Internal,                  // 内部错误
};
```

### 11.3 错误显示规范

```cpp
// 在 UI 层统一处理
auto result = service.someOperation();
if (!result.isOk()) {
    // 1. 显示 toast
    showToast("操作失败: " + result.errorMessage(),
              ToastLevel::Error);
    // 2. 记录日志（已在服务层做过，但 UI 层也可加上下文）
    LOG_WARN("UI: 操作失败 [" + context + "]: " + result.errorMessage());
    return;
}
```

---

## 12. 测试策略

### 12.1 测试框架选择

**doctest**（header-only，单文件）：
- `tests/doctest.h` 直接拷贝
- 比 GoogleTest 快 5-10 倍
- 比 Catch2 API 更简洁

### 12.2 测试层级

| 层级 | 范围 | 工具 |
|------|------|------|
| 单元测试 | `core::Result`、`paths`、`sqlite_db`、`theme`、各 `service` | doctest |
| 集成测试 | SQLite 迁移、`MarkdownService` + 文件系统、proxy 转发 | doctest |
| 端到端 | 应用启动、UI 交互 | AI 手动验证 + 自动截屏 |

### 12.3 测试覆盖目标

- 核心工具类：`Result`、`paths`、`sqlite_db` 覆盖率 ≥ 90%
- 各 Service 层：覆盖率 ≥ 80%
- UI 层：不强制（依赖框架，人工验证）

### 12.4 Mock 策略

- **HTTP 上游 mock**：启动一个本地 Python/Node mock server（用 httpbin 替代品）用于测试代理
- **文件系统 mock**：使用 `std::filesystem::temp_directory_path()` + 唯一子目录
- **SQLite 测试**：每个测试用独立的临时 db 文件，测试结束删除

### 12.5 测试目录结构

```
tests/
├── CMakeLists.txt
├── doctest.h                       # 单文件框架
├── test_main.cpp                   # doctest 入口
├── test_core/
│   ├── test_result.cpp
│   ├── test_paths.cpp
│   ├── test_sqlite_db.cpp
│   └── test_logging.cpp
├── test_markdown/
│   ├── test_markdown_service.cpp
│   ├── test_markdown_renderer.cpp
│   └── test_markdown_search.cpp
├── test_todo/
│   ├── test_todo_service.cpp
│   └── test_todo_crud.cpp
├── test_claude_proxy/
│   ├── test_claude_service.cpp
│   ├── test_proxy_router.cpp       # 需要 mock 上游
│   └── test_streaming_proxy.cpp
└── test_platform/
    ├── test_http_client.cpp        # 用 httpbin.org 或 mock
    ├── test_http_server.cpp
    └── test_keychain.cpp
```

---

## 13. 文档策略

由于用户不熟悉 C++，文档是**用户能 review 的唯一产物**。每完成一个功能模块必须产出：

| 文档 | 内容 | 维护者 |
|------|------|--------|
| `README.md` | 项目是什么、怎么跑、目录结构、依赖说明 | AI |
| `docs/ARCHITECTURE.md` | 架构图、模块职责、数据流（中文） | AI |
| `docs/USER_GUIDE.md` | 每个功能的操作手册（中文，截图辅助） | AI |
| `docs/CROSS_PLATFORM.md` | macOS / Windows 差异说明 | AI |
| `docs/screenshots/` | AI 自动截屏（每完成一个模块新增） | AI 自动生成 |
| 源码内注释 | 每个文件顶部中文说明 + 关键函数 JSDoc 风格 | AI |
| 日志 | 中文日志（用户能直接看的运行记录） | AI 自动生成 |

### 13.1 README.md 模板

```markdown
# DevDesk

跨平台的开发者桌面工具集：Markdown 笔记本 + Todo 看板 + Claude 配置切换器。

## 快速开始

### macOS

\`\`\`bash
# 1. 克隆
git clone <repo>
cd DevDesk
git submodule update --init --recursive

# 2. 构建
./scripts/dev.sh

# 3. 运行
./build/DevDesk
\`\`\`

### Windows

\`\`\`powershell
# PowerShell
git clone <repo>
cd DevDesk
git submodule update --init --recursive
.\scripts\dev.ps1
.\build\Release\DevDesk.exe
\`\`\`

## 功能

- 📝 **Markdown 笔记本**：[详细说明](docs/USER_GUIDE.md#markdown笔记本)
- ✅ **Todo 看板**：[详细说明](docs/USER_GUIDE.md#todo看板)
- 🔄 **Claude 配置切换器**：[详细说明](docs/USER_GUIDE.md#claude配置切换器)

## 架构

详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 开发

详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。
```

---

## 14. 构建与开发

### 14.1 顶层 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(DevDesk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

include(cmake/Dependencies.cmake)
include(cmake/CompilerWarnings.cmake)

# EUI-NEO 子项目
add_subdirectory(3rd/EUI-NEO)

# 核心库
add_subdirectory(src/core)
add_subdirectory(src/platform)

# 功能模块
add_subdirectory(src/module_home)
add_subdirectory(src/module_markdown)
add_subdirectory(src/module_todo)
add_subdirectory(src/module_claude_proxy)

# 测试
enable_testing()
add_subdirectory(tests)

# 应用入口
add_executable(DevDesk src/main.cpp)
eui_neo_configure_app(DevDesk)
target_link_libraries(DevDesk PRIVATE
    devdesk_core
    devdesk_platform
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_module_claude_proxy
    eui_neo
)
target_include_directories(DevDesk PRIVATE src)

# 拷贝 resources（如果有）
# ...
```

### 14.2 cmake/Dependencies.cmake

```cmake
# 找 SQLite3
find_package(SQLite3 REQUIRED)

# 找 CURL
find_package(CURL REQUIRED)

# Threads
find_package(Threads REQUIRED)

# cpp-httplib（header-only，直接放 3rd/）
add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE ${CMAKE_SOURCE_DIR}/3rd/cpp-httplib)

# yyjson / md4c / FreeType 复用 EUI-NEO 自带
add_library(yyjson INTERFACE)
target_include_directories(yyjson INTERFACE
    ${CMAKE_SOURCE_DIR}/3rd/EUI-NEO/3rd/yyjson-0.12.0/src)

add_library(md4c INTERFACE)
target_include_directories(md4c INTERFACE
    ${CMAKE_SOURCE_DIR}/3rd/EUI-NEO/3rd/md4c/src)
```

### 14.3 开发脚本

```bash
# scripts/dev.sh
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== 配置 CMake ==="
cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "=== 编译 ==="
cmake --build "$PROJECT_DIR/build" --parallel

echo "=== 启动应用 ==="
"$PROJECT_DIR/build/DevDesk"
```

```bash
# scripts/test.sh
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

ctest --test-dir "$PROJECT_DIR/build" --output-on-failure --parallel
```

```bash
# scripts/screenshot.sh
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

PAGE_NAME="${1:-home}"

# 启动应用
"$PROJECT_DIR/build/DevDesk" > /dev/null 2>&1 &
APP_PID=$!
sleep 3

# 截屏
mkdir -p "$PROJECT_DIR/docs/screenshots"
screencapture -x "$PROJECT_DIR/docs/screenshots/${PAGE_NAME}.png"

# 关闭
kill $APP_PID 2>/dev/null || true
echo "截图已保存: docs/screenshots/${PAGE_NAME}.png"
```

---

## 15. 风险与权衡

### 15.1 技术风险

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| cpp-httplib 流式转发稳定性 | 🔴 高 | 集成测试 + mock 上游充分验证 |
| EUI-NEO 拖拽 API 不支持/有限 | 🟡 中 | 先 spike 验证，必要时改用键盘菜单 |
| macOS 27 OpenGL 移除 | 🟡 中 | 提供 Vulkan 选项，但 macOS 主路径仍是 OpenGL |
| Keychain/DPAPI 跨平台差异 | 🟡 中 | 抽象层统一接口 |
| md4c 渲染复杂 Markdown 边角 case | 🟢 低 | Markdown 语法标准且 md4c 成熟 |
| 中文文件名编码 | 🟢 低 | 全程 UTF-8，std::filesystem 原生支持 |

### 15.2 项目风险

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| 范围过大 | 🟡 中 | 严格按 spec 实现，不做 spec 之外的功能 |
| 用户不熟 C++ 无法 review | 🟡 中 | 文档 + 截屏 + 测试是主要反馈手段 |
| Windows 验证难 | 🟡 中 | macOS 优先，Windows 通过 CI + 用户手测 |

### 15.3 权衡决策记录

| 决策 | 选择 | 备选 | 理由 |
|------|------|------|------|
| 渲染后端 | OpenGL（默认）| Vulkan | macOS 27 可能弃用 GL，但 Vulkan 性能损耗 + MoltenVK 复杂度不划算 |
| HTTP 服务器 | cpp-httplib | civetweb | 单文件、零依赖、API 简洁 |
| JSON | yyjson | nlohmann/json | 复用 EUI-NEO 自带，性能更高 |
| Markdown 渲染 | md4c | cmark | 复用 EUI-NEO 自带 |
| 测试框架 | doctest | GoogleTest / Catch2 | 单文件、速度快 |
| 拖拽交互 | EUI-NEO 原生（如有）| 自实现 | 优先使用框架能力 |
| 加密 | Keychain/DPAPI | 应用内 AES | 用平台原生能力，密钥管理更安全 |
| 主题 | 单深色 + 可切换浅色 | 多套主题 | YAGNI |
| 文件存储 | 文件 + SQLite 索引 | 纯 SQLite | 用户可直接用外部编辑器 |
| 部署 | 源码构建 | 打包 .dmg/.msi | 个人项目，本地构建足够 |

---

## 16. 实施里程碑

虽然用户不限制开发时间，但为方便跟踪进度，按以下顺序实施：

| 阶段 | 内容 | 可交付物 |
|------|------|---------|
| **P0：项目骨架** | 目录、CMake、依赖、空窗口启动 | 应用启动后显示空窗口 |
| **P1：Core 基础设施** | `Result`、`paths`、`logging`、`sqlite_db`、`theme` | 对应单元测试通过 |
| **P2：Platform 层** | `http_client`、`http_server`、`keychain`、`json` | 对应单元测试通过 |
| **P3：Router + Home** | 侧边栏 4 个模块入口 + Home 空状态 | 应用显示完整导航 |
| **P4：Markdown MVP** | 文件树 + 编辑器 + 预览 | 能写笔记 + 渲染 |
| **P5：Markdown 增强** | 搜索、标签 | 完整 Markdown 体验 |
| **P6：Todo MVP** | 4 列 + 卡片 CRUD | 能管理任务 |
| **P7：Todo 增强** | 拖拽、优先级、截止日期、标签 | 完整 Todo 体验 |
| **P8：Claude profile** | profile CRUD + active 切换 | 能管理配置 |
| **P9：Claude 代理（非流式）** | 代理转发 + 测试 + 日志 | Claude Code 能走通（buffer 模式）|
| **P10：Claude 流式转发** | SSE 流式 chunk 透传 | Claude Code 流式响应正常 |
| **P11：Claude 加密** | Keychain/DPAPI 存储 | API key 不再明文落盘 |
| **P12：Home 仪表盘数据** | 真实数据展示 | 首页不是空壳 |
| **P13：设置页面** | 主题、目录配置 | 用户可定制 |
| **P14：Windows 适配** | 路径、字体、CI | Windows 上可编译运行 |
| **P15：完善文档** | README、USER_GUIDE、ARCHITECTURE | 文档齐全 |

每个 P 阶段完成后：
1. ✅ 跑 `./scripts/test.sh`（单元测试全过）
2. ✅ 跑 `./scripts/dev.sh`（应用启动）
3. ✅ 跑 `./scripts/screenshot.sh`（自动截屏）
4. ✅ 更新 `docs/USER_GUIDE.md` 和 `docs/screenshots/`
5. ✅ git commit

---

## 17. 附录

### 17.1 第三方依赖完整列表

| 依赖 | 版本 | 来源 | 用途 |
|------|------|------|------|
| EUI-NEO | 0.6.0 | `3rd/EUI-NEO/` | GUI 框架 |
| GLFW | 3.x | EUI-NEO 自带 | 窗口 |
| glad | - | EUI-NEO 自带 | OpenGL loader |
| FreeType | - | EUI-NEO 自带 | 字体 |
| yyjson | 0.12.0 | EUI-NEO 自带 | JSON |
| md4c | - | EUI-NEO 自带 | Markdown 渲染 |
| miniaudio | - | EUI-NEO 自带 | 音频（暂未使用）|
| SQLite3 | 系统 | 系统包 / vcpkg | 本地数据库 |
| libcurl | 系统 | 系统包 / vcpkg | HTTP 客户端 |
| cpp-httplib | latest | `3rd/cpp-httplib/` | HTTP 服务器 |
| doctest | latest | `tests/doctest.h` | 单元测试 |

## 18. 初始化项目（开发者参考）

新克隆项目后：

```bash
# 1. 初始化子模块
git submodule update --init --recursive

# 2. macOS 安装系统依赖
brew install cmake sqlite curl

# 3. 配置 + 构建
./scripts/dev.sh

# 4. 运行测试
./scripts/test.sh

# 5. 运行应用
./build/DevDesk
```

**Windows**：

```powershell
# 1. 初始化子模块
git submodule update --init --recursive

# 2. 通过 vcpkg 安装依赖
vcpkg install sqlite3 curl:x64-windows

# 3. 配置 CMake（指定 vcpkg toolchain）
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake

# 4. 构建
cmake --build build --config Release --parallel

# 5. 运行
.\build\Release\DevDesk.exe
```

### 17.3 参考资料

- [EUI-NEO 官方文档](https://sudoevolve.github.io/EUI-NEO/)
- [EUI-NEO 示例代码](../EUI-NEO/examples/)
- [cpp-httplib GitHub](https://github.com/yhirose/cpp-httplib)
- [yyjson GitHub](https://github.com/ibireme/yyjson)
- [md4c GitHub](https://github.com/mity/md4c)
- [doctest GitHub](https://github.com/doctest/doctest)

---

**Spec 完成时间**：2026-09-20
**Spec 版本**：1.0
**下次更新**：实施过程中如有重大设计变更需更新此文档