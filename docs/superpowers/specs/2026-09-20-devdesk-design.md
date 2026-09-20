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

| 维度 | 目标 | 备注 |
|------|------|------|
| 启动时间 | < 5 秒（macOS）| 含 SQLite 启动、4 模块初始化、OpenGL 上下文创建；首次冷启 |
| 内存占用 | < 250 MB RSS | 含 Cocoa/GL 驱动框架（实测 EUI-NEO 示例 ~120 MB）|
| 帧率 | UI 交互 ≥ 60 FPS | 拖拽期间需 ≥ 30 FPS 即可接受；Markdown 长文档预览允许临时降帧 |
| 编译时间 | 全量冷编译 < 120 秒 | EUI-NEO 自带 md4c/FreeType/yyjson 等，首次编译就接近 60 秒 |
| 二进制大小 | < 35 MB（Release）| 含 EUI-NEO 全静态链；不含 EUI-NEO 单独 1.6 MB |
| 跨平台 | macOS 主线承诺；Windows **实验性** | 见 §15.2 |

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
| HTTP 服务器 | cpp-httplib（header-only）| 本地代理用，单文件，零依赖；**需 OpenSSL/LibreSSL 启用 HTTPS** |
| JSON | yyjson（**复用 EUI-NEO 自带**）| 高性能、轻量 |
| Markdown 渲染 | md4c（**复用 EUI-NEO 自带**，独立 C 库，非标准库）| 轻量、C 标准 C99 |
| TLS | OpenSSL / LibreSSL | cpp-httplib 启用 HTTPS 必需 |
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
    // 安全取值（错误时返回默认值，不退出进程）
    T valueOr(T defaultValue) const;
    // 显式取值（错误时 LOG_FATAL + 抛 std::logic_error，调用前必须 isOk）
    // 适合已确认成功的内部代码路径
    const T& value() const;
    // 类 Rust expect()：错误时 LOG_FATAL + 抛异常，附带自定义上下文
    const T& expect(const std::string& contextMessage) const;
    // 显式取值（不检查错误，调用前必须确认 isOk；性能敏感场景用）
    const T& unsafeValue() const;
    // 取错误
    ErrorCode errorCode() const;
    const std::string& errorMessage() const;
    // 链式：若 ok 应用 fn，否则传播错误
    template<typename Fn>
    auto map(Fn fn) -> Result<decltype(fn(std::declval<T>()))>;
    // 链式：若 ok 应用返回 Result 的 fn
    template<typename Fn>
    auto andThen(Fn fn) -> Result<decltype(fn(std::declval<T>()).value())>;
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

// SQLite 支持的标量类型（替代 std::any，类型安全）
using SqlValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// 一行结果：按列名索引到值
struct SqlRow {
    std::vector<std::string> columnNames;
    std::vector<SqlValue> values;

    // 按列名取值（类型安全的访问器）
    Result<int64_t> getInt64(const std::string& col) const;
    Result<double> getDouble(const std::string& col) const;
    Result<std::string> getString(const std::string& col) const;
    Result<std::vector<uint8_t>> getBlob(const std::string& col) const;
    bool isNull(const std::string& col) const;
};

// 线程模型：单连接 + 全局互斥锁（write 串行化，read 可并发但实现简单起见也走同一锁）
// 后续如性能不够，可改为 connection-per-thread 池
class SqliteDb {
public:
    // 打开数据库（不存在则创建）
    static Result<SqliteDb> open(const std::filesystem::path& file);
    ~SqliteDb();

    // 执行无返回值的 SQL
    Result<void> exec(const std::string& sql);

    // 准备语句（每个 Statement 内部使用 shared_ptr<sqlite3_stmt>）
    class Statement {
    public:
        // 绑定参数（按索引）
        Result<void> bind(int index, int64_t value);
        Result<void> bind(int index, double value);
        Result<void> bind(int index, const std::string& value);
        Result<void> bind(int index, std::nullptr_t);
        Result<void> bind(int index, const std::vector<uint8_t>& blob);

        // 执行一步
        Result<bool> step();    // 返回 true = 有一行可读；false = 没有更多行
        // 取当前行（必须在 step() 返回 true 后调用）
        Result<SqlRow> currentRow() const;

        // 一次性取出所有行
        Result<std::vector<SqlRow>> fetchAll();
    };

    Result<Statement> prepare(const std::string& sql);

    // 事务（自动 commit/rollback；包在 mutex 内）
    Result<void> transaction(std::function<Result<void>()> fn);

    // 运行迁移（幂等；包在事务里）
    Result<void> runMigrations(const std::vector<Migration>& migrations);

    // 当前 schema 版本
    int schemaVersion() const;

    // 内部：取互斥锁供 Statement 内部使用
    std::recursive_mutex& mutex();

private:
    sqlite3* handle_;
    std::recursive_mutex mutex_;
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

// 颜色 token（基于方案 B：温暖专业，Claude.ai 风格）
struct ColorTokens {
    eui::Color background;        // 主背景
    eui::Color surface;          // 卡片表面
    eui::Color surfaceElevated;  // 浮层表面
    eui::Color hover;            // hover 背景
    eui::Color active;           // 选中 / 按下态

    eui::Color primary;          // Claude Clay #D97757 — 主品牌色
    eui::Color primaryHover;     // #E0896B
    eui::Color primaryPressed;   // #B85F3F
    eui::Color primarySoftBg;    // 强调态背景 #3A2620 (dark) / #F5E6DE (light)

    eui::Color textPrimary;
    eui::Color textSecondary;
    eui::Color textTertiary;
    eui::Color textDisabled;

    eui::Color borderDefault;
    eui::Color borderMuted;

    eui::Color success;          // Claude Sage Green #788C5D
    eui::Color warning;          // Notion 黄 #C29243
    eui::Color error;            // Cursor 玫红 #CF2D56
    eui::Color info;             // Claude Dusty Blue #6A9BCC
};

// 尺寸 token
struct SizeTokens {
    float spacing;             // 8.0f
    float spacingSmall;        // 4.0f
    float spacingLarge;        // 16.0f
    float spacingXLarge;       // 24.0f
    float radius;              // 10.0f（按钮 / 输入框）
    float radiusSmall;         // 6.0f（chip / tag）
    float radiusLarge;         // 12.0f（卡片）
    float fontSizeSmall;       // 12.0f
    float fontSizeBody;        // 14.0f
    float fontSizeLarge;       // 18.0f
    float fontSizeTitle;       // 24.0f
    float fontSizeDisplay;     // 32.0f
};

struct Tokens {
    ColorTokens colors;
    SizeTokens sizes;
};

// 当前主题（默认深色 = 方案 B Dark）
const Tokens& current();
// 切换主题
void setDark();
void setLight();
// 获取当前主题名（用于持久化）
std::string currentThemeName();

} // namespace core::theme
```

### 6.5.1 Design Tokens（方案 B：温暖专业）

**决策**：2026-09-20 选定方案 B（warm professional），与 Claude.ai / Cursor / Notion 风格同源。

#### Dark Mode 主色板

| Token | Hex | 用途 | 备注 |
|-------|-----|------|------|
| `--bg-base` | `#1A1816` | 主背景 | 暖墨底（深于 Cursor `#26251E`）|
| `--bg-surface` | `#232120` | 卡片 / 面板 | +5% 亮度 |
| `--bg-elevated` | `#2C2A28` | 浮层 / 菜单 / 弹窗 | +5% 亮度 |
| `--bg-hover` | `#353231` | hover 态 | |
| `--bg-active` | `#3E3B39` | 选中 / 按下态 | |
| `--accent` | `#D97757` | Claude Clay — 主品牌色 | 唯一鲜艳色 |
| `--accent-hover` | `#E0896B` | | |
| `--accent-pressed` | `#B85F3F` | | |
| `--accent-soft-bg` | `#3A2620` | 强调态背景（按钮二级态、激活态）| |
| `--text-primary` | `#FAF9F5` | 正文 | Claude Ivory（15.2:1 AAA）|
| `--text-secondary` | `#C9C5BD` | 次要 | |
| `--text-tertiary` | `#9A948A` | 弱化 / 时间戳 | |
| `--text-disabled` | `#6B6660` | 禁用 | |
| `--border-default` | `#3A3735` | 默认边框 | |
| `--border-muted` | `#2A2826` | 弱边框 | |
| `--success` | `#788C5D` | Claude Sage Green | |
| `--warning` | `#C29243` | Notion 黄 | |
| `--error` | `#CF2D56` | Cursor 玫红 | |
| `--info` | `#6A9BCC` | Claude Dusty Blue | |

#### Light Mode 主色板

| Token | Hex | 备注 |
|-------|-----|------|
| `--bg-base` | `#FAF9F5` | Claude Ivory 暖白 |
| `--bg-surface` | `#FFFFFF` | |
| `--bg-elevated` | `#FFFFFF` | |
| `--bg-hover` | `#F5F2EC` | |
| `--bg-active` | `#EBE7DF` | |
| `--accent` | `#D97757` | 跨模式同色（强化品牌识别）|
| `--accent-soft-bg` | `#F5E6DE` | |
| `--text-primary` | `#1F1D1A` | 非纯黑（暖墨）|
| `--text-secondary` | `#6B6660` | |
| `--text-tertiary` | `#9A948A` | |
| `--text-disabled` | `#B5B0A8` | |
| `--border-default` | `#E8E6DC` | |
| `--border-muted` | `#F0EEE6` | |
| `--success` | `#5E7048` | |
| `--warning` | `#9A6B30` | |
| `--error` | `#B82850` | |
| `--info` | `#4A7AAB` | |

#### 字体搭配

```cpp
// EUI-NEO 通过 FreeType 加载 TTF 文件
// 路径：src/core/fonts/

namespace core::theme {
struct FontTokens {
    std::string sansFamily;       // "Inter"
    std::string sansFilePath;     // "fonts/Inter-Regular.ttf"
    std::string sansBoldFilePath; // "fonts/Inter-Bold.ttf"（**仅打包 Regular + Bold 两份**）
    std::string monoFamily;       // "JetBrains Mono"
    std::string monoFilePath;     // "fonts/JetBrainsMono-Regular.ttf"
    std::string chineseFamily;    // macOS: PingFang SC; Windows: 微软雅黑
    float bodyLineHeight;         // 1.6（英文）/ 1.8（中文）
    float chineseLetterSpacing;   // 0.02em
};
}
```

字体加载策略：
- **主字体**：开源 Inter（**Regular + Bold 两份**，OFL 协议），文件 ~200 KB × 2
- **等宽字体**：开源 JetBrains Mono（**Regular 一份**），文件 ~250 KB
- **中文 fallback**：不打包字体，使用系统字体（macOS 自动苹方，Windows 自动微软雅黑，EUI-NEO 自动 fallback）
- **版权合规**：Inter 与 JetBrains Mono 均为 OFL 协议，可商用打包

**⚠️ EUI-NEO 真实能力警告**（来自审计）：
- `fontWeight(700)` 数值传入但 **runtime 不切换字重 face**
- 因此不打包 SemiBold/Medium 等中间字重，避免浪费
- 真正的"半粗"视觉改用：Bold 字 + opacity 0.85，或 Bold 字 + 1px text stroke
- 在 spec 中所有提到"Medium / SemiBold"的位置替换为"Bold"或"Regular + opacity"

#### 圆角系统

| 元素 | 圆角 |
|------|------|
| 按钮 | 10 px |
| 输入框 | 10 px |
| Chip / Tag | 999 px (pill) |
| 小卡片 | 8 px |
| 卡片 | 12 px |
| 大卡片 / 弹窗 | 14 px |
| macOS 27 窗口 | 18 px（系统默认）|

#### 阴影（macOS 上 vibrancy 替代实色阴影）

```cpp
struct ShadowTokens {
    // 弱阴影，用于普通浮层
    ShadowTokens small = {
        .offsetY = 1.0f,
        .blur = 3.0f,
        .color = eui::Color(0, 0, 0, 0.04f)
    };
    // 中阴影，用于弹窗
    ShadowTokens medium = {
        .offsetY = 4.0f,
        .blur = 12.0f,
        .color = eui::Color(0, 0, 0, 0.06f)
    };
};
// 注：macOS 上优选 vibrancy（Liquid Glass），Windows 上用实色阴影
```

#### 动画

| 场景 | 时长 | 缓动函数 |
|------|------|----------|
| 按钮 hover / 按下 | 150 ms | `ease-out` |
| 页面切换 | 200 ms | `ease-out` |
| 弹窗出现 | 200 ms | `cubic-bezier(0.32, 0.72, 0, 1)` (spring) |
| Toast 进出 | 250 ms | `ease-out` |
| 拖拽跟随 | 即时 | - |
| Markdown 预览 debounce | 300 ms | - |

#### 与 EUI-NEO 落地的注意事项

- ✅ EUI-NEO 支持任意 hex `eui::Color`，所有 token 直接可用
- ✅ EUI-NEO 支持 FreeType 字体加载（见 `3rd/EUI-NEO/3rd/freetype/`）
- ✅ EUI-NEO 支持阴影 API（见 `core/render/opengl/opengl_backend.cpp`）
- ✅ EUI-NEO 支持 transition / animation API（见 `core/runtime/runtime_animation.h`）
- ⚠️ EUI-NEO **不支持** web 风格 CSS 变量；token 在 C++ 中硬编码到 `theme.cpp`
- ⚠️ EUI-NEO **不支持** shadcn 风格组件；用 EUI-NEO 自带 `components/` 下的 button / input / chip
- ⚠️ macOS 27 Liquid Glass 需要 EUI-NEO 上游支持（截至审计未实现），先用实色 + 阴影替代

### 6.5.2 大气设计原则（Generous Design）

**决策**：2026-09-20 添加。**反对"咪咪小"设计** —— DevDesk 是桌面应用，用户在 1080P / 2K / 4K 显示器前长时间使用，需要舒展、有呼吸感的视觉。

#### 窗口尺寸规范

| 项 | 数值 | 说明 |
|----|------|------|
| 默认窗口大小 | **1600 × 1000** | 占 1080P 屏幕 83% × 93%，2K 屏 75% × 83% |
| 最小窗口 | 1200 × 720 | 三栏布局的最小可读尺寸 |
| 最大窗口 | 全屏 | 用户可最大化（macOS 绿色按钮 / Windows 最大化）|
| HiDPI | **默认开启** | `DslAppConfig{}.hiDpi(true)` |
| 窗口位置 | 居中显示 | 首次启动屏幕中心；之后记住上次位置 |
| 窗口持久化 | `config.json` | 记录 size + position，下次启动恢复 |

#### 间距系统（基于 8 px 网格）

| Token | 值 | 用途 |
|-------|-----|------|
| `--spacing-xs` | 4 px | 紧凑（如 chip 内 padding） |
| `--spacing-sm` | 8 px | 元素内 padding（小按钮）|
| `--spacing-md` | 16 px | 标准间距（按钮 padding）|
| `--spacing-lg` | 24 px | 卡片内 padding |
| `--spacing-xl` | **40 px** | 内容区四周 padding |
| `--spacing-2xl` | **56 px** | section 之间留白 |
| `--spacing-3xl` | 80 px | 页面顶部留白 |

**原则**：宁可多留 8 px，也不要挤 4 px。

#### 字号系统（"大气"档）

| Token | 值 | 用途 |
|-------|-----|------|
| `--font-xs` | 12 px | chip 标签、辅助说明 |
| `--font-sm` | 13 px | 次要元数据、时间戳 |
| `--font-body` | **15 px** | 正文（默认）|
| `--font-lg` | **18 px** | 强调文本 |
| `--font-title` | **24 px** | 卡片标题 |
| `--font-page` | **32 px** | 页面大标题 |
| `--font-display` | **48 px** | 统计数字（Home 大数字）|

**关键**：正文 15 px（不是 14），页面标题 32 px（不是 24），统计 48 px（不是 28）。

#### 行高

| 类型 | 行高 |
|------|------|
| 英文正文 | 1.5 |
| 中文正文 | **1.7**（中文需要更多行高）|
| 大标题 | 1.2 |

#### 按钮尺寸（"大气"档）

| 元素 | 高度 | padding |
|------|------|---------|
| 主按钮 | **40-44 px** | 16 px / 20 px |
| 次按钮 | **40 px** | 16 px / 20 px |
| 小按钮 | 32 px | 8 px / 12 px |
| 图标按钮 | 40 × 40 px | - |

**关键**：主按钮 40 px 高（不是 32 px），触感和点击区域都更舒适。

#### 输入框尺寸

| 元素 | 高度 |
|------|------|
| 输入框 | **40 px** |
| 多行编辑器 | 最小高度 320 px |
| 搜索框 | 40 px |

#### 卡片系统

| 类型 | 内 padding | 圆角 |
|------|-----------|------|
| 统计卡片 | 24 px | 14 px |
| 列表卡片 | 20 px | 12 px |
| 普通卡片 | 20-24 px | 12 px |
| 大卡片（Profile、Home） | **28-32 px** | **14 px** |
| section 之间间距 | 40 px | - |

#### 侧边栏

- 宽度：**260 px**（不是 200-220）
- 内 padding：上下 24 px，左右 16 px
- 导航项高度：40 px
- 导航项之间 gap：4 px

#### 图标尺寸

| 用途 | 大小 |
|------|------|
| 侧边栏导航 | 20 px |
| 按钮内 icon | 18 px |
| 大标题旁 icon | 24 px |
| 状态指示器 | 8 px（点）或 16 px（小图标）|

#### 内容区最大宽度（防止超宽屏文字过长）

```css
max-width: 1200px;  /* 主内容容器 */
margin: 0 auto;      /* 居中 */
```

#### 应用示例

```cpp
// 主按钮（大气版）
components::button(ui, "save")
    .text("保存")
    .height(44.0f)             // 不是 32
    .padding(16.0f, 20.0f)     // 不是 8, 12
    .radius(10.0f)
    .fontSize(15.0f)           // 不是 13
    .build();

// 卡片
ui.column("card")
    .padding(28.0f)            // 不是 16
    .gap(20.0f)                  // 而不是 12
    .radius(14.0f)              // 而不是 8
    .build();

// 统计数字
ui.text("stat.value")
    .text("142")
    .fontSize(48.0f)           // 不是 32
    .fontWeight(700)           // Bold face 唯一支持的"粗"
    .color(textPrimary())
    .build();
```

#### 与 EUI-NEO 兼容性

- ✅ 所有间距 / 字号 / padding 数值 EUI-NEO 完全支持
- ✅ 圆角 14 px EUI-NEO 支持（不限值）
- ✅ HiDPI 通过 GLFW 自动缩放，无需代码改动
- ✅ 窗口尺寸 / 最小尺寸 DslAppConfig 完整支持

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

// 流式响应的 chunk 状态
enum class ChunkResult {
    More,    // 还有数据，chunk 有效
    End,     // 正常结束
    Error,   // 网络错误或上游断开
};

// 流式发送（SSE 场景）
class StreamingResponse {
public:
    // 读下一个 chunk
    //   返回 More + 填充 chunk = 有数据
    //   返回 End = 流正常结束
    //   返回 Error + errorMessage = 出错
    ChunkResult readChunk(std::string& chunk, std::string& errorMessage);

    int statusCode() const;
    std::string finalErrorMessage() const;     // 整个流关闭后的最终错误

    // 主动取消（中断 libcurl transfer）
    void cancel();

    ~StreamingResponse();
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
    bool authenticated = false;       // 认证通过的标记（由 Server 内部检查后设置）
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
    // 构造（authToken 留空表示不要求认证，仅用于开发模式）
    Server(std::string bindAddress, int port, std::string authToken = "");

    // 注册路由
    void route(const std::string& method, const std::string& path,
               RouteHandler handler);

    // 阻塞监听（应放在工作线程）
    Result<void> listen();

    // 异步启动（在工作线程）
    Result<void> startAsync();

    void stop();

    bool isRunning() const;

    int actualPort() const;     // 用于获取随机分配的端口

    // 认证相关
    bool requiresAuth() const;
    const std::string& authToken() const;       // 用于日志/UI 显示

private:
    // 内部：每个请求前调用，检查 Authorization: Bearer <token>
    bool checkAuth(const RequestCtx& ctx) const;
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
2. 在中间编辑 Markdown，右侧实时渲染（**带 300ms debounce**，避免每键重渲染）
3. `Cmd+S`（macOS）/ `Ctrl+S`（Windows）保存 → 触发外部文件变更检测时自动让 DevDesk 重读
4. 顶部搜索框输入关键词 → 显示匹配的文件列表
5. 点击文件树的文件 → 加载到编辑器
6. 点击标签 → 过滤显示同标签的文件
7. 文件存在系统目录中，可用任意外部编辑器直接打开

**EUI-NEO 限制说明**（已知约束，已审计源码确认）：
- **没有文件拖入 API**（onDrop）：文件需通过 UI 新建按钮添加，不支持拖文件进窗口
- **没有 tree 组件**：需自实现 `MarkdownTreeView`（约 150 行）：
  - 基于 `components::virtualList` 渲染节点
  - 用 `▶ / ▼` Unicode 字符图标
  - 状态机：`isExpanded` 状态 + 父子节点链表
  - 节点类型：folder（可展开）/ file（不可展开）
  - P4 阶段实现
- **没有真正的代码/Markdown 编辑器组件**：MVP 使用 `components::input.multiline(true)` 普通多行文本框
  - 不支持：语法高亮、行号、括号匹配、代码折叠、缩进
  - 支持：多行输入、IME、复制粘贴、撤销重做、选区
- **实时预览每次步骤**：解析 md4c → 重建 Element 树 → 触发 dirty rect
  - 长文档（> 5000 字）下需 debounce（300ms）+ 分段渲染
  - 否则帧率会从 60FPS 跌到 20FPS 以下
- **外部编辑器冲突处理**：
  - 启用 `std::filesystem::last_write_time` 轮询（每 5 秒） + kqueue/inotify/ReadDirectoryChangesW 平台 watcher
  - 若检测到磁盘文件 mtime > DevDesk 编辑器 mtime，弹 toast：「文件已被外部修改，是否重新加载？」三选项：覆盖 / 重新加载 / 取消

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
3. **拖拽**（P7b 阶段）：鼠标按下卡片 → 拖动 → 跨列放置，详见 `DragController` 设计
4. **键盘/菜单移动**（P7a 阶段，必交付）：选中卡片 + `Cmd+↑/↓` 上下移动；右键菜单 → 「移动到...」选列
5. 单击卡片打开详情面板（编辑标题、描述、优先级、截止日期、标签）
6. 右键卡片 → 快捷菜单（编辑/删除/移动到...）
7. 点击列标题 → 重命名 / 改色 / 删除

#### 拖拽框架设计（DragController）

**背景**：EUI-NEO 只提供 `onDrag`（指针拖动）回调，无 drop target / 跨元素移动语义。所有拖拽需自实现。

```cpp
// src/core/drag_controller.h
namespace core::drag {

struct DragContext {
    std::string dragId;          // 由调用方生成的拖拽唯一标识
    void* payload;               // 拖拽的数据（如 taskId）
    eui::Vec2 startPosition;     // 起始位置
    eui::Vec2 currentPosition;   // 当前光标位置
};

class DragController {
public:
    // 注册可拖拽元素（鼠标按下时调用）
    //   onDragStart: 拖拽开始，可视化反馈（半透明、阴影）
    //   onDragMove: 光标移动
    //   onDragEnd: 拖拽结束（含是否在有效 drop target 上）
    void registerDraggable(
        const std::string& elementId,
        std::function<void(const DragContext&)> onDragStart,
        std::function<void(const DragContext&)> onDragMove,
        std::function<void(const DragContext&, bool droppedOnValidTarget)> onDragEnd);

    // 注册 drop target（鼠标移入/移出时检测）
    void registerDropTarget(
        const std::string& targetId,
        eui::Rect bounds,
        std::function<bool(const DragContext&)> acceptPredicate,
        std::function<void(const DragContext&)> onDragEnter,
        std::function<void(const DragContext&)> onDragLeave,
        std::function<void(const DragContext&)> onDrop);

    // 接收 EUI-NEO 的指针事件（应在主 UI compose 中调用）
    void onPointerDown(eui::Vec2 pos);
    void onPointerMove(eui::Vec2 pos);
    void onPointerUp(eui::Vec2 pos);

private:
    DragContext current_;
    bool dragging_ = false;
};

}
```

**实现要点**：
- DragController 是单例，由 Router 创建并注入到需要拖拽的模块
- 鼠标按下时通过 hit-test 找到 draggable 元素
- 鼠标移动时更新 current_，通知 draggable 的 onDragMove
- 同时 hit-test 找 drop target，触发 enter/leave
- 鼠标抬起时调用 onDragEnd，参数告诉调用方是否落在有效 target
- 失败回退：若 P7b 拖拽体验不达 60 FPS 或频繁崩溃，**直接降级为只保留 P7a 键盘/菜单移动**，不阻塞交付

### 8.3 模块：Claude 配置切换器

#### 数据模型

```sql
-- API key 不存在 SQLite，**只存平台原生 Keychain/DPAPI**；本表存引用
CREATE TABLE claude_profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL,                -- "官方" / "第三方A" / "测试"
    base_url TEXT NOT NULL,                    -- "https://api.anthropic.com"
    api_key_hint TEXT NOT NULL,                -- 仅显示用：明文末3位（如 "***xyz"），便于用户区分多个 key
    api_key_check TEXT NOT NULL,               -- 明文前 8 字符的 SHA256 hex，用于 profile 删除时验证选错 key
    keychain_account TEXT NOT NULL,            -- Keychain/DPAPI 中的账号名（"DevDesk:claude:profile:<id>"）
    model TEXT NOT NULL,                       -- 默认 "claude-sonnet-4-5"
    is_active INTEGER NOT NULL DEFAULT 0,      -- 全局唯一
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
    std::string apiKeyPlain;       // **仅在内存中存在**，从不落盘 SQLite
    std::string apiKeyHint;        // "***xyz" 形式，用于 UI 显示
    std::string apiKeyCheck;       // 前 8 字符的 SHA256 hex，用于防止误操作
    std::string keychainAccount;   // Keychain/DPAPI 中的账号
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
    // getProfile 返回的 Profile.apiKeyPlain **不填充**（空字符串），需显式 loadApiKey
    Result<Profile> getProfile(int64_t id);

    // 创建（API key 入 Keychain，引用入 SQLite）
    Result<Profile> createProfile(const std::string& name,
                                   const std::string& baseUrl,
                                   const std::string& apiKey,
                                   const std::string& model);

    Result<void> updateProfile(const Profile& profile);  // profile.apiKeyPlain 为空则不改 key
    Result<void> deleteProfile(int64_t id);             // 同时清理 Keychain 条目

    // 显式加载明文 API key（仅在需要发请求时调用，用完即丢弃）
    Result<std::string> loadApiKey(int64_t profileId);

    // 激活
    Result<Profile> getActiveProfile();
    Result<void> setActiveProfile(int64_t id);

    // 连通测试（发送 GET /v1/models）
    Result<platform::http::Response> testConnection(const Profile& profile);
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
│   claude-sonnet-4-5  │ Token: sk-ant-***xyz (已脱敏)        │
│                    │ 模型: claude-sonnet-4-5                 │
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
   - 模型（必填，默认 `claude-sonnet-4-5`）
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
3. **Markdown 编辑器字体大小**：数字输入框（默认 14）
4. **Markdown 编辑器字体**：下拉（系统等宽 / 系统无衬线 / 自定义路径）
5. **Claude 代理端口**：数字输入框（默认 8888；改后自动重启代理）
6. **Claude 代理认证 Token**：只读显示当前 token +「复制」按钮 +「重新生成」按钮
7. **调用日志保留天数**：数字输入框（默认 30；保留策略见 11.5）
8. **API key 提示脱敏粒度**：下拉（仅末 3 位 / 仅末 6 位 / 完全隐藏）
9. **日志级别**：下拉框（Trace / Debug / Info / Warn / Error）
10. **关于**：版本号、许可证、依赖列表

**安全说明**（写入文档）：
- 代理地址固定为 `http://127.0.0.1:<port>`，**不暴露对外网**
- 调用 Claude Code 时需在配置中加 `Authorization: Bearer <token>`
- 设置页面提供 token 复制按钮，避免用户手动输入错误

---

## 9. 跨平台策略

### 9.1 平台差异抹平点

| 关注点 | macOS | Windows | EUI-NEO 真实能力 |
|--------|-------|---------|------------------|
| 应用数据目录 | `~/Library/Application Support/DevDesk/` | `%APPDATA%\DevDesk\` | ✅ |
| 配置目录 | 同上 | 同上 | ✅ |
| 快捷键修饰键 | `Cmd`（`key.modifiers.shortcut()` 自动识别）| `Ctrl`（同上）| ✅ |
| 路径分隔符 | `/` | `\`（代码统一用 `std::filesystem::path`）| ✅ |
| 字体 | 系统默认（San Francisco / PingFang SC）| Segoe UI / Microsoft YaHei | ✅ EUI-NEO 自动 fallback |
| 密钥存储 | Keychain | DPAPI | ✅ 通过 platform/keychain 抽象 |
| **系统托盘** | 菜单栏图标 | 系统托盘 | ⚠️ **菜单项硬编码为 Show/Exit**，不能加自定义项；iconPath 可设 |
| HiDPI | 自动（Retina）| 自动（4K）| ✅ 通过 `DslAppConfig{}.hiDpi(true)` 开启 |
| 窗口默认大小 | 1600 × 1000 | 1600 × 1000 | ✅ `DslAppConfig{}.windowSize(1600, 1000)` |
| 窗口最小大小 | 1200 × 720 | 1200 × 720 | ✅ `DslAppConfig{}.minWindowSize(1200, 720)` |
| 全局 OS hotkey | 不支持 | 不支持 | ❌ 窗口失焦时收不到 key 事件，需先聚焦窗口 |

**EUI-NEO 不支持的事项**（实现时绕开）：
- ❌ OS 文件拖入窗口 → 用"+ 新建"按钮 + `core::platform::openFileDialog`
- ❌ 跨元素拖放 → 自实现 `DragController`（P7a 键盘 / 菜单先行）
- ❌ macOS 27 Liquid Glass → 实色 + 阴影替代
- ❌ 字体 weight runtime 切换 → 只打包 Regular + Bold 两份
- ❌ onResize / onMove 回调 → 通过 `DslWindowRequest::compose(ui, screen)` 的 `screen` 参数变化触发自适应

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

### 10.4 一次 Claude API 代理调用（流式）

#### 线程模型（三线程 + 队列）

```
[Thread A: cpp-httplib worker]                 [Thread B: libcurl]                 [Thread C: UI main]
                                                                                   ┐
  接收 POST /v1/messages                      ── 无工作 ──                        │ 信号通知
  检查 Authorization: Bearer <token> ✓                                            │ 状态变化
  检查请求大小 (避免 OOM, >10MB 拒绝)                                              │
  构造 ResponseWriter (chunked provider)                                          │
       │                                                                              │
       ▼                                                                              │
  从 cpp-httplib 内部队列 <queue_> 取数据 ────► 若有 ──► 写入 HTTP chunk ──► [客户端] 
       ▲                                                                              │
       │                                                                              │
  [Thread B: libcurl connection thread]                                              │
       │                                                                              │
       ▼                                                                              │
  sendStreaming(req)                                                                  │
  libcurl CURLOPT_WRITEFUNCTION 回调 ──► 把 chunk 推入 <queue_> ◄─────────────┘
                                                                                      
  客户端断开 / 网络错误 / 完成:
     ◄─── cancel() / 自然结束 ────
     ◄─── 从队列读 End 标记 ────
     ◄─── ResponseWriter.end() ────
     ◄─── call_logger 写入 ────
```

**关键设计**：
- `queue_`：线程安全的 `std::deque<std::string>` + `std::mutex` + `std::condition_variable`
- libcurl 回调只做"队列推入"，不做 IO
- cpp-httplib 的 chunked_content_provider 阻塞在队列 pop
- 出错时从队列推入 Error 标记，cpp-httplib 停止读取 → 关闭连接
- 超时保护：cpp-httplib 端设 5 分钟无新 chunk 则主动 close（防止挂死）
- backpressure：队列上限 100 chunk（约 1 MB），超过则 libcurl 端 `curl_easy_pause` 暂停

#### 请求路径

```
Claude Code: POST http://127.0.0.1:8888/v1/messages
            Headers: Authorization: Bearer <token>,
                     x-api-key: sk-ant-xxx,
                     anthropic-version: 2023-06-01,
                     Content-Type: application/json
            Body: JSON {model, messages, stream: true, ...}
  ↓
cpp-httplib Server 接收
  ↓
proxy_router.cpp: 路由 /v1/messages
  ├─ 1. 验证 Authorization（auth_token 一致 → 通过；不一致 → 401）
  ├─ 2. 读取 RequestCtx（method, path, body, headers）
  ├─ 3. 记录开始时间戳
  ├─ 4. claudeService.getActiveProfile() → Profile
  ├─ 5. claudeService.loadApiKey(profileId) → 明文 token（用完即丢弃）
  ├─ 6. 构造转发请求：
  │   - url = profile.baseUrl + path
  │   - headers = 原始 headers（去掉 x-api-key） + x-api-key: 明文 token
  │   - body = 透传
  └─ 7. upstream_client.cpp: sendStreaming(req) → 启动三线程流式转发
  ↓
上游 Claude API 流式响应
  ↓
chunks 通过队列流回 Claude Code
  ↓
流结束
  ├─ call_logger.cpp: 写入 claude_call_logs 表
  │   ├─ profile_id, method, path, status_code, latency_ms, request_bytes, response_bytes
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

### 12.3 测试覆盖目标与度量

**覆盖率工具**：
- macOS: `llvm-cov`（Xcode Command Line Tools 自带）+ `gcovr` 生成报告
- Windows: `OpenCppCoverage` 或 `llvm-cov`（通过 LLVM 安装）
- 集成到 CMake：在 Debug 构建中加 `--coverage` 编译标志

**覆盖率目标**：

| 层级 | 目标 |
|------|------|
| 核心工具类（`Result`、`paths`、`sqlite_db`、`logging`）| 行覆盖 ≥ 90% |
| Service 层（各模块）| 行覆盖 ≥ 80% |
| UI 层 | 不强制 |
| Platform 层（`http_client`、`http_server`、`keychain`）| 行覆盖 ≥ 70%（mock 上游） |

**报告产出**：`build/coverage/coverage.html` 单文件报告，每个 P 阶段验证覆盖率不下降。

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
# 找 SQLite3（必须带 FTS5）
find_package(SQLite3 REQUIRED)
if(NOT SQLite3_VERSION_SYM OR NOT SQLite3_VERSION_SYM MATCHES "FTS5")
    message(WARNING "SQLite3 编译时未启用 FTS5，Markdown 全文搜索功能将不可用")
endif()

# 找 CURL（上游 Claude API 必需）
find_package(CURL REQUIRED)

# 找 OpenSSL（cpp-httplib HTTPS 支持必需）
find_package(OpenSSL REQUIRED)

# Threads
find_package(Threads REQUIRED)

# cpp-httplib（header-only，直接放 3rd/）
# 注意：必须定义 CPPHTTPLIB_OPENSSL_SUPPORT 才能用 HTTPS
add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE ${CMAKE_SOURCE_DIR}/3rd/cpp-httplib)
target_compile_definitions(cpp_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT=1)
target_link_libraries(cpp_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)

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
| **SSE 流式转发（cpp-httplib + libcurl）** | 🔴 高 | P0a 单独 PoC 验证三线程 + 队列架构；P10a 先做 buffer 模式；P10b 加流式；必须有 mock 上游 + 长连接回归测试 |
| **EUI-NEO 无 drop-target 拖拽 API** | 🔴 高 | 自实现 DragController；P7a 先交付键盘/菜单移动；P7b 鼠标拖拽如不达 60 FPS 或频繁崩溃，**直接降级为只保留 P7a** |
| **macOS 27 OpenGL 移除 + Vulkan 选项不可用** | 🔴 高 | 已审计：EUI-NEO 无 MoltenVK/Metal 配置，Vulkan 在 macOS 上**跑不通**。长期需 fork EUI-NEO 加 Metal 后端，或迁 SDL2+Metal 直写；MVP 仅承诺 OpenGL，macOS 27 升级节点再决策 |
| **SQLite 多线程并发** | 🔴 高 | SqliteDb 内部 `std::recursive_mutex`，所有操作走同一把锁；Statement 引用 db 锁；call_log 写入也走同一锁。后续可改 connection-per-thread |
| **代理无认证 → 本机进程可盗用 API key** | 🔴 高 | 启动时生成随机 token（32 字节 base64），要求 `Authorization: Bearer <token>`；UI 显示 token 供复制 |
| Markdown 实时预览长文档帧率 | 🟡 中 | 300ms debounce + 分段渲染；长文档（>5000 字）做懒加载预览 |
| EUI-NEO 无代码编辑器组件 | 🟡 中 | MVP 用 `components::input.multiline(true)`；语法高亮等 v2 |
| EUI-NEO 无文件拖入 API | 🟢 低 | UI 加「+ 新建」按钮代替；用户用拖入的预期需在文档说明 |
| macOS 27+ 实际可达性 | 🟡 中 | 已要求"macOS 27+"，但实际只在本机（macOS 26 Tahoe，2025）测过；具体 27 兼容性待发布后再验 |
| FTS5 在 Windows SQLite 不可用 | 🟡 中 | vcpkg 用 `sqlite3[fts5]` feature；运行时检测，无 FTS5 降级到 LIKE |
| md4c 复杂 Markdown 边角 case | 🟢 低 | 标准 Markdown 语法，md4c 成熟 |
| 中文文件名编码 | 🟢 低 | 全程 UTF-8，std::filesystem 原生支持 |
| 外部编辑器与 DevDesk 同时编辑同一文件 | 🟡 中 | mtime 轮询 + 平台 watcher，冲突时弹 toast 三选项 |
| **OpenSSL 缺失 → HTTPS 无法连接** | 🟡 中 | 平台包作为硬依赖（macOS: brew install openssl；Windows: vcpkg install openssl），CMake find_package(OpenSSL REQUIRED) |

### 15.2 项目风险

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| **Windows 几乎无法验证** | 🔴 高 | 当前**没有 GitHub Actions 配置**；用户不熟 C++ 无法手测。MVP 诚实承诺 **macOS 为唯一主线**，Windows 标记为"实验性 — 编译通过 ≠ 可运行"；待补 CI workflow 后再升级承诺 |
| 范围过大 | 🟡 中 | 严格按 spec 实现，不做 spec 之外的功能；每个 P 阶段都跑测试 + 截屏 |
| 用户不熟 C++ 无法 review | 🟡 中 | 文档 + 截屏 + 测试 + 中文日志是主要反馈手段；自我诊断页面报告运行时状态 |
| 调用日志无限增长 | 🟡 中 | 30 天滚动保留 + 月度归档（SQLite DELETE WHERE started_at < now-30d）|
| 拖拽 / 流式 PoC 失败 | 🟡 中 | P7a 键盘/菜单 / P10a buffer 模式作为降级方案，确保核心功能仍可交付 |

### 15.3 权衡决策记录

| 决策 | 选择 | 备选 | 理由 |
|------|------|------|------|
| 渲染后端 | OpenGL（macOS 主线）| Vulkan | Vulkan 在 macOS 上跑不通（无 MoltenVK），长期需评估 Metal 后端 |
| HTTP 服务器 | cpp-httplib | civetweb | 单文件、零依赖、API 简洁；需 OpenSSL 支持 HTTPS |
| JSON | yyjson | nlohmann/json | 复用 EUI-NEO 自带，性能更高 |
| Markdown 渲染 | md4c | cmark | 复用 EUI-NEO 自带 |
| 测试框架 | doctest | GoogleTest / Catch2 | 单文件、速度快 |
| 拖拽交互 | 自实现 DragController | "等 EUI-NEO" | 已审计无 drop-target；P7a 键盘/菜单先做，P7b 鼠标拖拽可选 |
| API key 存储 | 仅 Keychain/DPAPI | SQLite AES BLOB | 消除双存储攻击面；平台原生密钥管理更安全 |
| 代理认证 | 随机 Bearer token | 不认证 / TLS 双向 | 127.0.0.1 + 强 token 足够；TLS 双向在 loopback 收益不大 |
| 主题 | 深色 + 浅色双主题 | 多套主题 / 单深色 | YAGNI 但浅色不可少（白天使用场景） |
| 文件存储 | 文件 + SQLite 索引 | 纯 SQLite | 用户可直接用外部编辑器 |
| Markdown 编辑器 | 普通多行 Input | 自实现组件 | EUI-NEO 无编辑器组件；MVP 接受无语法高亮 |
| 部署 | 源码构建 | 打包 .dmg/.msi | 个人项目，本地构建足够 |
| 平台支持 | macOS 主线 + Windows 实验性 | 双主线 | 用户只 macOS，Windows 验证手段缺 |
| 性能目标 | 启动 < 5s / 编译 < 120s / 二进制 < 35 MB | 更激进 | 实测后修订，符合 EUI-NEO 实际开销 |

---

## 16. 实施里程碑

虽然用户不限制开发时间，但为方便跟踪进度，按以下顺序实施。**红线风险点（P7 拖拽、P10 流式）拆成两步 + 前置 PoC**。

| 阶段 | 内容 | 可交付物 | 依赖 |
|------|------|---------|------|
| **P0：项目骨架** | 目录、CMake、依赖、空窗口启动 | 应用启动后显示空窗口 | - |
| **P1：Core 基础设施** | `Result`、`paths`、`logging`、`sqlite_db`（含 mutex）、`theme` | 对应单元测试通过 | P0 |
| **P2：Platform 层** | `http_client`、`http_server`（含 auth）、`keychain`、`json` | 对应单元测试通过 | P1 |
| **P3：Router + Home** | 侧边栏 4 个模块入口 + Home 空状态 + DragController 占位 | 应用显示完整导航 | P1 |
| **P4：Markdown MVP** | 文件树 + 编辑器（Input 组件） + 预览（debounced）+ mtime watcher | 能写笔记 + 渲染 | P2, P3 |
| **P5：Markdown 增强** | 搜索、标签、外部编辑器冲突处理 | 完整 Markdown 体验 | P4 |
| **P6：Todo MVP** | 4 列 + 卡片 CRUD + 优先级/截止/标签 | 能管理任务 | P3 |
| **P7a：Todo 移动（必交付）** | 键盘（`Cmd+↑/↓`）+ 右键菜单「移动到...」+ 列间排序逻辑 | 任务移动不依赖拖拽 | P6 |
| **P7b：Todo 拖拽（可选）** | 自实现 DragController + 跨列拖拽 + 视觉反馈 | 鼠标拖拽可用 | P7a |
| **P8：Claude profile** | profile CRUD + Keychain 存储 + active 切换 + token UI | 能管理配置 | P2, P3 |
| **P0a：流式 PoC（前置 spike）** | 独立 console 程序：cpp-httplib + libcurl + mock 上游，验证三线程 + 队列架构 | 流式透传 demo 可跑 | P2 |
| **P9：Claude 代理（非流式）** | 代理转发（buffer 模式）+ 测试 + 日志 + 认证 token 集成 | Claude Code 能走通 | P0a, P8 |
| **P10a：Claude 流式（生产者-消费者）** | SSE 流式 chunk 透传，含 backpressure + 取消 + 超时 | 流式响应正常 | P9 |
| **P10b：Claude 流式（质量保证）** | 5 分钟长连接回归 + 大 body (10MB) 压测 + 异常断连恢复 | 边界情况稳定 | P10a |
| **P11：Claude 加密** | Keychain/DPAPI 存储（实际上 P8 已用，P11 确认加固）| API key 验证不落 SQLite | P8 |
| **P12：Home 仪表盘数据** | 真实数据展示 + 30 天日志清理任务 | 首页不是空壳 | P4, P6, P9 |
| **P13：设置页面** | 主题、目录、token UI、字体大小、日志保留策略 | 用户可定制 | P12 |
| **P14：Windows 实验性支持** | 路径、字体、CI workflow（windows-latest runner）、FTS5 vcpkg feature | 编译 + 单元测试过；功能不承诺 | P13 |
| **P15：完善文档** | README、USER_GUIDE、ARCHITECTURE + 自我诊断页面 + 已知限制 | 文档齐全 | P13 |
| **P16：长期维护评估** | 评估 EUI-NEO Metal 后端或 fork 需求；评估 Markdown 编辑器自实现 | macOS 27+ 升级准备 | P15 |

### 16.1 失败回退路径

| 阶段 | 失败表现 | 降级方案 |
|------|---------|---------|
| P7b 鼠标拖拽 | 帧率 < 30 FPS 或崩溃 | 砍掉鼠标拖拽，**只保留 P7a 键盘/菜单** |
| P10a 流式 | 长连接挂起/内存泄漏 | 退回到 P9 buffer 模式，**流式推迟到 v2** |
| P14 Windows | CI 编译失败 | 暂降为"已知不支持 Windows"，**修复优先 macOS** |
| P0a PoC | 三线程架构不可行 | 流式永久放弃，**始终 buffer 模式** |

### 16.2 验证规范（每个 P 阶段完成后必须做）

1. ✅ 跑 `./scripts/test.sh`（单元测试全过，含覆盖率报告）
2. ✅ 跑 `./scripts/dev.sh`（应用启动，5 秒内可见主界面）
3. ✅ 跑 `./scripts/screenshot.sh <page>`（自动截屏当前页）
4. ✅ 录屏验证拖拽/动画（仅 P7b、P10b 等含动画阶段）
5. ✅ 更新 `docs/USER_GUIDE.md` 和 `docs/screenshots/`
6. ✅ 更新 `docs/KNOWN_LIMITATIONS.md`（记录该阶段发现的边界）
7. ✅ git commit + 写 CHANGELOG.md 条目

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