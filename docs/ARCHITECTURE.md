# DevDesk 架构设计文档

> **目的**：回答"项目架构设计了吗？OOP 设计模式使用了？"
> **基于**：spec §4 / §6.5 / docs/EUI-NEO-CAPABILITIES.md
> **日期**：2026-09-21

---

## 1. 整体架构（4 层模块化）

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 0: Application Entry                                     │
│   src/main.cpp                                                │
│   - 初始化 paths / logging / sqlite                           │
│   - 注册 5 个页面模块到 Router + 直启桌宠子窗（startPetWindow） │
│   - 启动 EUI-NEO 事件循环                                     │
└────────────────┬────────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 1: Feature Modules（6 个独立模块）                       │
│   src/module_home/         - 首页仪表盘                       │
│   src/module_markdown/     - Markdown 笔记本                   │
│   src/module_todo/         - Todo 看板                         │
│   src/module_claude_proxy/ - Claude 切换器                    │
│   src/module_settings/     - 设置页                           │
│   src/module_pet/          - 桌宠（非 Router 页面模块，独立子窗）│
│   每个模块一个 static lib                                    │
└────────────────┬────────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 2: Shared Core（基础设施）                             │
│   src/core/                                                  │
│   - Result<T>     错误处理（不抛异常）                        │
│   - SqliteDb      数据库 + mutex                             │
│   - Logging       线程安全日志                                │
│   - Paths         跨平台目录                                  │
│   - Theme         设计 token                                  │
│   - Router        模块调度                                    │
│   - Module        模块抽象接口                                 │
└────────────────┬────────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 3: Platform Abstraction（OS 能力封装）                  │
│   src/platform/                                              │
│   - HttpClient    libcurl 同步 + 流式                         │
│   - HttpServer    cpp-httplib                                │
│   - Keychain      Keychain / DPAPI                            │
│   - Json          yyjson                                      │
└────────────────┬────────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 4: EUI-NEO GUI Framework                                │
│   ../EUI-NEO/                                                │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 模块依赖规则（强制）

| ✅ 允许 | ❌ 禁止 |
|--------|--------|
| `main` → `module_*` | `module_*` → `module_*`（互相依赖）|
| `module_*` → `core` | `module_*` → `EUI-NEO`（通过 core 间接）|
| `module_*` → `platform` | `core` → `platform`（保持 core 纯净）|
| `platform` → `core`（用 Result）| `core` → `EUI-NEO` **运行时行为**（窗口/渲染/事件循环/组件；类型桥接例外见注 ①）|
| `core` → `EUI-NEO` **仅 header-only 类型桥接**（注 ①）| `module_*` → `EUI-NEO`（必须经 core 适配）|

> **注 ①（core → EUI-NEO 类型桥接例外，2026-09-23 审计 [6] 转正）**：core 仅可引用 EUI-NEO 的**类型**——
> `eui::Color` 值类型 token、`eui::Ui`/`eui::Screen` 签名参数透传、`component_theme.h` 的 header-only
> `ThemeColorTokens` 映射；`devdesk_core` PUBLIC 链接 `eui_neo` 是为向 `module_*`/tests 传播 include
> usage requirements（静态库链接不强制解析符号），core 目标文件不含 EUI 运行时调用。「core 保持可单测」
> 的本意不变：core 逻辑测试不构造 GUI 环境。完整边界见 spec §4.2.1。

---

## 3. 使用的 OOP 设计模式

### 3.1 Strategy 模式（策略模式）

**位置**：`core/router.h` + `module_interface.h`

**目的**：每个模块用不同策略渲染自己，Router 只负责调度

```cpp
// Strategy 接口
class Module {
public:
    virtual PageId pageId() const = 0;
    virtual void compose(Ui& ui, const Screen& screen) = 0;
    virtual void onStart() {}
    virtual void onStop() {}
};

// Context
class Router {
    std::vector<Module*> modules_;
public:
    void compose(Ui& ui, const Screen& screen) {
        for (auto* m : modules_) {
            if (m->pageId() == currentPage_) {
                m->compose(ui, screen);  // 多态分发
            }
        }
    }
};

// 具体策略
class MarkdownModule : public Module { ... };
class TodoModule : public Module { ... };
```

**好处**：加新模块 = 加一个 Module 子类，不动 Router

---

### 3.2 RAII（资源获取即初始化）

**位置**：`core/sqlite_db.h`、`platform/keychain.cpp`

**目的**：自动管理资源生命周期，避免泄漏

```cpp
class SqliteDb {
public:
    static Result<SqliteDb> open(path);  // 构造时打开
    ~SqliteDb() { sqlite3_close(handle_); }  // 析构时关闭
    
    // 禁止拷贝（防止 double-close）
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    
    // 允许移动（所有权转移）
    SqliteDb(SqliteDb&&) noexcept;
};
```

```cpp
// macOS Keychain RAII 包装
class KeychainHandle {
    SecKeychainItemRef ref_ = nullptr;
public:
    ~KeychainHandle() { if (ref_) CFRelease(ref_); }
};
```

---

### 3.3 Pimpl（Pointer to Implementation）

**位置**：`platform/http_server.h`（计划）

**目的**：隐藏实现细节，减少编译依赖

```cpp
// 头文件
class Server {
public:
    Server(string addr, int port);
    ~Server();
    void route(string method, string path, RouteHandler h);
private:
    struct Impl;        // 前向声明
    Impl* impl_;        // 实现细节藏在 cpp
};

// cpp 文件
struct Server::Impl {
    map<string, RouteHandler> handlers;
    thread worker;
    atomic<bool> stop;
};
```

**好处**：改 `Impl` 不触发包含此头文件的代码重编译

---

### 3.4 Singleton 单例模式

**位置**：`core/logging.h`（简化版）

**目的**：全局日志系统唯一

```cpp
namespace core::log {
    // 全局静态变量 + setLevel/addFileOutput 接口
    // 隐式单例（不强制）
}
```

**注意**：完全单例 + 线程安全保证留给后续 P3 实现

---

### 3.5 Observer 模式（简化版）

**位置**：EUI-NEO 内部（`Signal<T>`）

**目的**：状态变化自动通知 UI

```cpp
eui::Signal<int> currentPage;  // 当前页面 ID
eui::Signal<bool> darkMode;     // 主题模式
eui::Signal<std::string> status; // 代理状态

// UI 绑定 Signal，状态变自动更新
ui.bindSignal(currentPage, [&] { ... });
```

**好处**：数据/UI 解耦，状态改变无需手动通知

---

### 3.6 Template Method 模板方法

**位置**：`module_interface.h` 的 Module 接口

**目的**：定义算法骨架，子类填充细节

```cpp
class Module {
    // 模板方法（用户不能 override 的固定流程）
    void runLifecycle() {
        onStart();              // 钩子 1
        // ... 主循环
        onStop();               // 钩子 2
    }
    
    // 钩子（子类实现）
    virtual void onStart() {}
    virtual void onStop() {}
};
```

---

### 3.7 Dependency Injection 依赖注入

**位置**：所有 Service 类

**目的**：可测试性（注入 mock）

```cpp
class HomeService {
public:
    // 依赖通过构造函数注入
    HomeService(SqliteDb& db) : db_(db) {}
    // 不在内部 new 或找全局
};
```

**好处**：测试时可注入 mock SqliteDb

---

### 3.8 Factory Method 工厂方法

**位置**：`src/main.cpp`（模块创建）

**目的**：隐藏构造细节

```cpp
// 每个模块提供 C 风格工厂函数
extern "C" Module* create_markdown_module();

// main.cpp 调用
auto m = create_markdown_module();
router.registerModule(m);
```

**好处**：避免暴露模块内部类型，编译期解耦

---

### 3.9 Builder 链式调用

**位置**：EUI-NEO DSL（外部框架）

```cpp
ui.column("root")
    .size(800, 600)
    .padding(24)
    .gap(12)
    .content([&] { ... })
    .build();
```

DevDesk 继承并使用此模式

---

### 3.10 Result/Either 模式（错误处理）

**位置**：`core/result.h`

**目的**：替代异常，对非 C++ 用户友好

```cpp
Result<int> result = someOperation();
if (result.isOk()) {
    int value = result.value();
} else {
    LOG_ERROR("失败: " + result.errorMessage());
}
```

**好处**：
- 错误处理是类型系统一部分（编译期检查）
- 中文错误信息
- 无 C++ 异常（避免栈追踪混乱）

---

## 4. SOLID 原则遵循情况

| 原则 | 遵循情况 | 说明 |
|------|---------|------|
| **S**ingle Responsibility | ✅ | 每个 module/类单一职责 |
| **O**pen-Closed | ✅ | 加新模块不修改 Router |
| **L**iskov Substitution | ✅ | Module 子类可替换 |
| **I**nterface Segregation | ✅ | Module 接口最小化 |
| **D**ependency Inversion | ✅ | 依赖抽象（Module、Result）而非具体实现 |

---

## 5. 错误处理设计模式

### 5.1 多层错误传播

```
UI 层：收到 Result → 显示 Toast
       ↓
Service 层：返回 Result<T>，错误带上下文
       ↓
Core 层：返回 Result<T>，错误带 ErrorCode
       ↓
Platform 层：libcurl 错误 → 映射到 ErrorCode
```

### 5.2 错误码分类

```cpp
enum class ErrorCode {
    Ok = 0,
    FileNotFound, FilePermissionDenied, FileWriteFailed,
    DatabaseOpenFailed, DatabaseCorrupted,
    NetworkUnreachable, NetworkTimeout, HttpError,
    JsonParseError, ProfileNotFound, ProfileAlreadyExists,
    InvalidApiKey, ValidationError, NotFound, AlreadyExists,
    Internal,
};
```

---

## 6. 之前的方案调研 — 是否做了？

### 6.1 ✅ 已做（基于 spec + audit 报告）

| 调研项 | 文档位置 | 结论 |
|--------|---------|------|
| EUI-NEO 真实能力盘点 | docs/EUI-NEO-CAPABILITIES.md（13 节）| ✅ 39 组件 + 5 硬限制 + 5 陷阱 |
| 2026 主流桌面应用配色 | ui-ux-pro-max search 调用 | ✅ 13 个标杆产品 + 3 个候选方案 |
| 浏览器渲染验证 | ui-ux-pro-max UX 指引搜索 | ✅ Modal/Toast/可发现性等 |
| SQLite 线程模型 | spec §6.3 | ✅ recursive_mutex 方案 |
| SSE 流式架构 | spec §10.4（已审计修正）| ✅ 三线程 + 队列 + backpressure 归属 |
| Keychain 跨平台 | spec §7.3 + keychain.cpp | ✅ Keychain/DPAPI/桩文件 |
| API key 存储 | spec §7.3（已审计修正）| ✅ 仅 Keychain，无 SQLite BLOB |
| 代理认证 | spec §6.2.2（已审计修正）| ✅ Bearer token + 127.0.0.1 |
| 多维度 spec 审计 | spec commit 2c7447e | ✅ 8/10 严重问题已修 |
| 拖拽可行性 | spec §8.2 DragController | ✅ 自实现 ~200 行（W8 后作为死代码删除，见 CHANGELOG「Todo 卡片拖拽真 drop 语义」条目；拖拽现走 EUI-NEO 元素级 onPress/onDrag/onRelease） |

### 6.2 ⏳ 部分（待继续）

| 调研项 | 状态 |
|--------|------|
| cpp-httplib 流式 API 详细文档 | ⏳ P0a PoC 阶段验证 |
| EUI-NEO 字体加载兼容性（macOS/Windows） | ⏳ P0 实测 |
| Markdown 实时预览性能（>5000 字） | ⏳ P4 实测 |

### 6.3 ❌ 未做（明确延后到 v2）

| 调研项 | 原因 |
|--------|------|
| macOS 27+ Metal 后端迁移 | 用户决定：当前 OpenGL，先跑通 |
| Windows DPAPI 完整实现 | 实验性平台，P14 才做 |
| 打包 .dmg/.msi | YAGNI |
| 自定义窗口标题栏 | EUI-NEO 不支持 |

---

## 7. 架构决策记录（ADR）

### ADR-001：4 层模块化 + 严格依赖规则

**状态**：采纳
**理由**：
- 让 `core` 完全不依赖 EUI-NEO，单测可在 CI 跑
- `module_*` 之间无依赖，加新模块零摩擦
- `platform` 可独立测试 HTTP/Keychain

### ADR-002：不抛 C++ 异常，用 Result<T>

**状态**：采纳
**理由**：
- 用户不熟 C++，异常栈追踪不友好
- 中文错误信息更直观
- 编译期类型检查（必须处理 Result）

### ADR-003：API key 仅存 Keychain/DPAPI

**状态**：采纳
**理由**（已审计发现的问题）：
- 双存储（Keychain + SQLite）是攻击面
- 平台原生密钥管理更安全
- spec §7.3 + §8.3 已实施

### ADR-004：127.0.0.1 + Bearer token（代理认证）

**状态**：采纳
**理由**：
- 0.0.0.0 会暴露 API key 到外网
- 强随机 token 防本机其他进程盗用
- TLS 在 loopback 收益小

### ADR-005：DragController 自实现

**状态**：已被取代（W8 后 `core::drag::DragController` 经审计确认零调用方，作为死代码删除 .h/.cpp 与 CMake 条目；拖拽改用 EUI-NEO 元素级 `onPress`/`onDrag`/`onRelease` 回调）
**理由**（EUI-NEO 审计）：
- EUI-NEO 没有 drop target API
- 自实现 ~200 行可控可测
- P7a 键盘/菜单先行，P7b 鼠标拖拽可选

### ADR-006：单 Git 仓库 + EUI-NEO 子项目

**状态**：采纳
**理由**：
- 用户已在 `/Users/coderstory/Dev/gui/EUI-NEO/` 克隆 EUI-NEO
- `add_subdirectory(EXCLUDE_FROM_ALL)` 集成
- 便于直接修改 EUI-NEO 并测试

---

## 8. 测试策略

| 类型 | 工具 | 覆盖目标 |
|------|------|---------|
| 单元测试 | doctest（tests/doctest.h） | core 90%, service 80% |
| 集成测试 | doctest + 临时 db | module 关键路径 |
| UI 截屏 | screencapture | 视觉一致性 |
| 跨平台 | GitHub Actions | macOS 主线 |

---

## 9. 接下来要写的实际 UI 代码

按依赖顺序：

1. **Router 修复**：完成 compose dispatch（已基本完成）
2. **HomeModule UI**：4 统计卡 + 最近编辑（用 `ui.column` + `ui.row`）
3. **MarkdownModule UI**：三栏布局（用 `ui.row` + nested `ui.column`）
4. **TodoModule UI**：4 列看板（用 `ui.flow` 或 4 个固定列）
5. **ClaudeModule UI**：左右两栏
6. **统一测试**：编译 + 启动 + 截图对比 HTML demo

每个模块的 UI 写完后立即编译验证，截图与 HTML demo 对比。

---

**架构设计文档 v1.0** · 与 spec / EUI-NEO-CAPABILITIES.md / UI-UX-DESIGN.md 三份文档保持一致