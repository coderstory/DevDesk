# 智能体系统通知（`app.notify`）与平台通知层 — 设计文档

- 日期：2026-10-18
- 状态：草案（待用户拍板，未实施）
- 分支：`dev`
- 关联决策：`docs/superpowers/specs/2026-10-agent-roadmap-decisions.md` §2（M3 通知形态）
- 关联代码：`src/platform/notification.h`、`src/platform/notification_macos.mm`、`src/platform/notification_win.cpp`、`src/platform/notification_posix.cpp`、`src/module_agent/agent_module.*`、`src/app_sidebar.cpp`
- 关联上游：`EUI-NEO/core/platform/tray_bridge.{h,c}`、`EUI-NEO/3rd/tray/tray.h`

> **只读调研结论**：本文不修改任何源码。工作区当前已存在一组**未提交**的通知层草案
> （`notification.h` / `notification_macos.mm` / `notification_win.cpp` stub /
> `notification_posix.cpp` / `test_notification.cpp` 及 CMake 接线），macOS 真实路径已跑通
> 到「系统受理」层，Windows 仍是诚实 stub。本文把它记为 **T1a 基线**，在其上设计
> `app.notify` 与 Windows 真实后端，不把已有工作当成不存在。

---

## 0. 结论速览

| 编号 | 结论 |
|---|---|
| C-1 | 新增平台层 `platform::notify`，统一 macOS `UNUserNotificationCenter` 与 Windows 托盘气泡；业务代码零平台宏。 |
| C-2 | 新增智能体工具 `app.notify`，只负责「把一条通知交给平台层」，不做 UI、不碰页面。 |
| C-3 | **不推翻 M3 决策**：run 结束通知仍走 `AgentModule::pendingNotices_` → `app::showAppNotice` 应用级 toast；`app.notify` 是新的、独立的触发源。 |
| C-4 | 平台层负责文本清洗/截断、限速、去重、授权状态；工具层只做参数校验、状态映射与降级入队。 |
| C-5 | macOS 必须先做 `.app` bundle guard，再触碰 `UNUserNotificationCenter`；否则裸二进制会抛 ObjC 异常。 |
| C-6 | Windows 复用 EUI-NEO 现有 Win32 托盘，新增 `eui_tray_show_notification()`，用 `NOTIFYICONDATAW` + `NIF_INFO`；不引入 WinRT/AUMID/快捷方式。 |
| C-7 | Windows 11 的托盘气泡是**临时横幅**、不进通知中心；v1 接受，作为已知限制。 |
| C-8 | macOS 现有 `.app` 签名无效（`Identifier=DevDesk`、`Info.plist=not bound`、`codesign --verify` 失败）；新增 ad-hoc 重签脚本，固定 `--identifier com.devdesk.app`。 |
| C-9 | 失败一律降级到应用内 toast 队列；系统通知失败不再重试、不持久化、不阻塞 UI。 |
| C-10 | 分阶段 T1b–T1f，每阶段独立可回滚；T1a 草案先收口再演进。 |

---

## 1. 背景与问题

### 1.1 已有能力

- 智能体任务跑完后，`AgentModule` 通过 `detectRunEndNotifications()` 检测
  `running → failed/limited` 迁移，把 `(title, message)` 放进 `pendingNotices_`
  （`agent_module.h:296-298`）。
- `app_sidebar::compose()` 每帧 `pollRunEnd()` 并 `takeRunEndNotice()`，取走后调用
  `app::showAppNotice()` 显示**应用级 toast**（`app_sidebar.cpp:243-250`）。
- M3 决策（`2026-10-agent-roadmap-decisions.md` §2）已明确：run 结束用应用级 toast，
  **不推上游补系统通知 API**。理由是该场景只在 app 进程活着时发生，窗口/任务栏通常已是
  可见面，系统通知的增量只剩「窗口最小化且任务栏被遮挡」这一窄场景。

### 1.2 新需求

现在要让**智能体主动**通知用户，而不是只在 run 结束时被动提示。典型场景：

- 长任务在后台跑完，用户已切到别的应用；
- 智能体需要用户回来做一个决定；
- 定时任务的结果需要用户注意。

这类通知由智能体**显式调用工具**触发，触发时机、内容、级别都由模型决定，和 M3 的
run-end 迁移检测不是同一条链路。因此新增 `app.notify` 工具是合理的，但必须：

1. 不改变 run-end 的既有语义；
2. 不让模型无限制地刷通知；
3. 在系统通知不可用时仍有可见的降级出口；
4. 在 macOS/Windows 两端都不引入新的外部依赖或安装负担。

### 1.3 为什么不能只靠应用内 toast

应用内 toast 只在窗口可见、compose 正在跑时才有意义。用户最小化到托盘、切到别的桌面、
或者窗口被遮挡时，toast 看不见。系统通知是唯一能跨窗口状态触达用户的通道。

---

## 2. 目标 / 非目标 / anti-goals

### 2.1 目标

| 编号 | 目标 |
|---|---|
| G-1 | 提供 `platform::notify` 平台抽象：macOS/Windows 真实实现，其它 POSIX 诚实降级。 |
| G-2 | 提供 `app.notify` 工具：参数化 title/body/sound/level/id，返回结构化 JSON 给 LLM。 |
| G-3 | 通知失败可降级到应用内 toast；降级不影响智能体 run 的成败。 |
| G-4 | 文本安全：非法 UTF-8、控制字符、超长文本在进入系统 API 前被清洗/截断。 |
| G-5 | 防刷：全局限速 + 相同内容去重；不因模型循环调用而刷屏。 |
| G-6 | 授权可观测：macOS 未决定/拒绝/允许三态可查询；拒绝后不反复弹窗。 |
| G-7 | 线程安全：工具在 AgentLoop 工作线程调用，不要求调用方切 UI 线程。 |
| G-8 | 可测试：所有平台分支都能用注入后端测试，不需要真实弹通知。 |
| G-9 | 双平台原生：macOS `.app` 与 Windows 单 exe 都不新增外部 DLL/运行时。 |

### 2.2 非目标

- 不做通知中心历史同步、已读状态、消息列表。
- 不做通知点击后的页面路由/任务定位；v1 点击只激活 app。
- 不做通知内的按钮、回复、图片、自定义声音、分组/线程。
- 不做 Linux/其它 POSIX 的原生通知（`notify-send`/D-Bus 都不做）。
- 不做 Windows WinRT `ToastNotificationManager`、AUMID、开始菜单快捷方式。
- 不做用户级通知偏好设置页；v1 只依赖系统设置。
- 不做通知持久化、失败重试队列、离线补发。
- 不把 run-end 通知迁移到系统通知；M3 决策保持不变。
- 不做多语言文案；工具描述与错误信息用中文。

### 2.3 Anti-goals（明确禁止）

| 编号 | 禁止 |
|---|---|
| A-1 | 在裸二进制/测试进程里直接调用 `[UNUserNotificationCenter currentNotificationCenter]`；必须先过 bundle guard。 |
| A-2 | 平台后端在未接入时返回成功（假绿）；必须返回 `BackendUnavailable` / `NotImplemented` 语义。 |
| A-3 | 在 UI 主线程阻塞等待系统通知展示结果；`send()` 只等「系统受理」。 |
| A-4 | 在业务代码里写 `#ifdef _WIN32` / `#ifdef __APPLE__`；平台差异只放 `notification_*` 文件。 |
| A-5 | 让 `module_agent` include 兄弟 module，或让 `platform` 依赖任何 `module_*`。 |
| A-6 | 使用 C++ 异常；错误走 `Result<T>`。 |
| A-7 | 新增外部依赖/DLL；Windows 只复用已有 Shell32/EUI-NEO 托盘。 |
| A-8 | 把 title/body 原文写进日志；日志只记状态、id、长度。 |
| A-9 | 为了系统通知去改 run-end 的 `pendingNotices_` 语义或 UI 文案。 |
| A-10 | 对 `NIM_MODIFY` 失败做无限重试；失败即降级。 |

---

## 3. 现状盘点（2026-10-18，工作区未提交）

### 3.1 已存在的通知草案（T1a 基线）

| 文件 | 现状 |
|---|---|
| `src/platform/notification.h` | `namespace platform::notify`；`Notification{id,title,body,sound}`；`Authorization{NotDetermined,Denied,Authorized}`；虚 `Backend`；`send()` 返回 `Result<void>`；`setBackendForTest` / `resetBackendForTest`。 |
| `src/platform/notification_macos.mm` | 真实 `UNUserNotificationCenter`；bundle guard；强引用 delegate；前台 `Banner|List|Sound`；授权请求/缓存；`send` 异步入队后立即返回 ok。 |
| `src/platform/notification_win.cpp` | **stub**：`send` 返回 `NotImplemented`，授权恒 `NotDetermined`。 |
| `src/platform/notification_posix.cpp` | `NotImplemented` 诚实降级。 |
| `tests/test_platform/test_notification.cpp` | 抽象层转发、空 title/body、注入缝、裸进程 guard 共 6 条用例。 |
| `src/platform/CMakeLists.txt` | 已按平台选源；macOS 已链 `UserNotifications.framework`，`.mm` 单独 `-fobjc-arc`。 |
| 顶层 `CMakeLists.txt` | 已 `enable_language(OBJCXX)`（APPLE）。 |
| `tests/CMakeLists.txt` / `tests/test_main.cpp` | 已登记 `test_notification.cpp`；`resetCaseGlobals()` 已调 `resetBackendForTest()`。 |

**结论**：T1a 已经把 macOS 的「能不能发」验证到系统受理层；缺的是统一状态/限速/去重/截断、
Windows 真实后端、`app.notify` 工具、签名与真机 UAT。本文不要求推倒重来。

### 3.2 现有签名问题（实测）

```text
$ codesign -dv --verbose=2 build/DevDesk.app
Identifier=DevDesk
Signature=adhoc
Info.plist=not bound
$ codesign --verify --strict build/DevDesk.app
build/DevDesk.app: code has no resources but signature indicates they must be present
```

即：链接器 ad-hoc 签名的 identifier 与 `Info.plist` 的 `CFBundleIdentifier=com.devdesk.app`
不一致，且 bundle 资源未被封印。系统通知/TCC 对 bundle 身份敏感，出包前必须重签。

### 3.3 现有 run-end 通知路径（不改）

- 入队：`agent_module.cpp:1053-1083`。
- 取走：`app_sidebar.cpp:243-250`。
- 语义：completed/cancelled 静默，failed/limited 必响；同 runId 只通知一次。
- 应用级 toast 单实例：同帧多条后发顶替先发（已接受）。

---

## 4. 分层与数据流

```
┌─────────────────────────────────────────────────────────────┐
│ module_agent                                                │
│   AgentLoop (worker thread)                                 │
│     └─ AppNotifyTool::execute()                             │
│          ├─ platform::notify::send()                        │
│          └─ fallback: AgentModule::enqueueFallbackNotice()  │
│                                                             │
│   AgentModule::onStart() 注册 app.notify / app.stats         │
│   app_sidebar::compose() 每帧 drain fallback → app toast     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ platform::notify（facade，notification.cpp）                 │
│   清洗/截断 → 授权 → 去重 → 限速 → Backend::send            │
│   结构化 Status / Receipt；Result::fail 只给参数非法          │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
notification_macos.mm  notification_win.cpp  notification_posix.cpp
UNUserNotificationCenter  EUI-NEO tray bridge    NotImplemented
                          Shell_NotifyIconW(NIF_INFO)
```

依赖方向：

- `module_agent` → `core::ITool` + `platform::notify`；不依赖兄弟 module。
- `platform::notify` → `core/result.h`、`platform/utf8.h`；不依赖任何 module。
- `notification_win.cpp` → `EUI-NEO/core/platform/tray_bridge.h`（平台层调用框架平台能力，
  与 `tray_icon_*` 同模式）；不依赖 DSL/UI。
- `app_sidebar` → `module_agent::AgentModule`（现有装配层依赖，保持不变）。

---

## 5. `platform::notify` API

> 目标 API 在 T1a 草案上做三点收敛：① `send()` 返回 `Result<Receipt>` 而非 `Result<void>`；
> ② `Notification` 增加 `level`；③ 清洗/限速/去重进 facade。当前零生产调用方，破坏面只有
> `test_notification.cpp`。若用户要求保留草案签名，退路见 §13 D-7。

### 5.1 类型

```cpp
// src/platform/notification.h
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include <core/result.h>

namespace platform::notify {

enum class Level { Info, Success, Warning, Error };

// Unknown       = 无法查询（裸 bundle / 后端未就绪 / 查询超时）
// NotDetermined = macOS 尚未询问用户
// Denied        = 用户已拒绝
// Authorized    = 已允许（macOS Provisional 归入此档）
enum class Authorization { Unknown, NotDetermined, Denied, Authorized };

enum class Status {
    Accepted,              // 系统受理；不保证用户看到（Focus/勿扰可能抑制）
    Deduped,               // 相同 id+内容在 2s 内重复
    RateLimited,           // 超过 3 条突发 / 1 条每秒
    NotBundled,            // macOS 裸二进制，没有有效 .app bundle
    PermissionDenied,      // macOS 用户拒绝通知授权
    NotDetermined,         // macOS 尚未决定；已异步发起授权请求
    BackendUnavailable,    // 托盘未就绪 / NIM_MODIFY 失败 / 平台未接入
};

struct Notification {
    std::string id;        // 可选；空 = 由内容派生稳定 id
    std::string title;     // 必填，单行
    std::string body;      // 必填，可多行
    bool sound = true;
    Level level = Level::Info;
};

struct Receipt {
    Status status = Status::BackendUnavailable;
    std::string detail;        // 中文说明，给工具/日志
    std::string effectiveId;   // 清洗/派生后的实际 id
    bool titleTruncated = false;
    bool bodyTruncated = false;
    int retryAfterMs = 0;      // 仅 RateLimited 有意义
};

const char* statusToString(Status status);
const char* authorizationToString(Authorization auth);

// 主线程调用一次；幂等。macOS 安装 delegate/激活观察者、查询授权并在
// NotDetermined 时异步请求（首次启动弹窗）。Windows/POSIX no-op。
void initialize();

// 任意线程；异步、幂等。macOS 内部封送主线程；Windows/POSIX no-op。
void requestAuthorization();

// 任意线程；macOS 可能阻塞 ≤1s 查询设置（有缓存），Windows 读原子。
Authorization authorizationStatus();

// 任意线程。清洗/截断 → 授权 → 去重 → 限速 → 后端。
// Result::fail 只用于参数非法（清洗后 title/body 为空）或内部错误；
// 环境状态一律 ok(Receipt.status)。
core::Result<Receipt> send(const Notification& notification);

// ---- 测试缝 ----
struct Backend {
    virtual ~Backend() = default;
    virtual void initialize() {}
    // 后端只应返回 Accepted / NotBundled / PermissionDenied /
    // NotDetermined / BackendUnavailable；Deduped/RateLimited 由 facade 产生。
    // Result::fail 只表示后端内部错误，facade 会映射成 BackendUnavailable。
    virtual core::Result<Status> send(const Notification&) = 0;
    virtual Authorization authorizationStatus() = 0;
    virtual void requestAuthorization() = 0;
};

Backend& defaultBackend();
Backend* setBackendForTest(Backend* backend);  // nullptr = 恢复默认
void resetBackendForTest();

using NowFn = std::function<std::chrono::steady_clock::time_point()>;
void setClockForTest(NowFn now);   // 空 = steady_clock::now
void resetForTest();               // limiter/dedupe/clock + resetBackendForTest

} // namespace platform::notify
```

### 5.2 函数语义与线程模型

| 函数 | 调用线程 | 阻塞 | 语义 |
|---|---|---|---|
| `initialize()` | 主线程 | 否 | 幂等；macOS 安装 delegate、注册 `NSApplicationDidBecomeActiveNotification`、异步查授权；若 `NotDetermined` 则请求授权。Windows/POSIX no-op。 |
| `requestAuthorization()` | 任意 | 否 | macOS 封送主队列；已决定时系统不会再弹。 |
| `authorizationStatus()` | 任意 | macOS ≤1s | 优先缓存；缓存 `NotDetermined` 时同步查 settings（信号量 1s），超时返回缓存。 |
| `send()` | 任意 | 通常 <1s；Windows ≤1.5s | 入口做清洗/授权/去重/限速，再调后端。不等待用户看到。 |
| `setBackendForTest()` | 测试 | 否 | 原子指针交换；不要在并发 send 时换后端。 |
| `resetForTest()` | 测试 | 否 | 清限速/去重/时钟并恢复默认后端。 |

**为什么 `send()` 允许 worker 线程**：`AppNotifyTool::execute()` 跑在 AgentLoop 工作线程。
平台层必须自己处理线程亲和，不能要求调用方 `dispatch`/`PostMessage`。

### 5.3 文本清洗与截断

所有清洗在 facade 内完成，后端拿到的永远是良构、有界文本。

#### 5.3.1 UTF-8

- 用 `platform::utf8::sanitize()`：非法字节逐字节替换为 U+FFFD（`EF BF BD`）。
- 不因为非法 UTF-8 直接失败；通知内容可读部分保留。
- 清洗后 title/body 为空 → `Result::fail(ValidationError, "通知标题/正文不能为空")`。

#### 5.3.2 控制字符

| 字段 | 规则 |
|---|---|
| title | `\r\n`/`\r`/`\n`/`\t` → 空格；其余 C0（0x00-0x1F）与 DEL（0x7F）→ 空格；连续空格折叠为一个；首尾 trim。 |
| body | 行尾统一 `\n`；`\t` → 一个空格；NUL 与其它 C0（除 `\n`）→ 空格；连续 3 个以上 `\n` 折叠为 2 个；首尾 trim。 |

#### 5.3.3 截断

| 字段 | 上限 | 规则 |
|---|---|---|
| title | 63 UTF-16 code unit | 超限时保留前 62 unit + `…`（U+2026，1 unit），`titleTruncated=true`。 |
| body | 255 UTF-16 code unit | 超限时保留前 254 unit + `…`，`bodyTruncated=true`。 |

- 计数按 UTF-8 解码后的码点折算 UTF-16：BMP 码点 1 unit，补充平面码点 2 unit。
- **不得切开代理对**：剩余容量不足以容纳下一个码点时停止，再补 `…`。
- 不按字素簇切分（emoji ZWJ 组合可能被截断），v1 接受；工具描述建议模型自己控制长度。
- Windows `NOTIFYICONDATAW` 的 `szInfoTitle` 64 wchar / `szInfo` 256 wchar 是硬上限；
  facade 的 63/255 是跨平台统一口径，EUI-NEO 侧再用 `StringCchCopyW` 防御。

#### 5.3.4 id

- 用户提供 `id`：先做 UTF-8 清洗、trim；长度 >64 code unit 时用 FNV-1a 64 位哈希替换。
- 未提供：`effectiveId = "devdesk-" + fnv1a64(level + "\n" + title + "\n" + body)`。
- `effectiveId` 用于：
  - macOS `UNNotificationRequest.identifier`（同 id 替换系统里的同 id 通知）；
  - 去重键；
  - 日志（只记 id，不记内容）。
- Windows 托盘气泡没有 id 概念，`effectiveId` 只用于去重/回执。

### 5.4 限速与去重

facade 内进程级状态，mutex 保护；`resetForTest()` 清零。

| 机制 | 参数 | 行为 |
|---|---|---|
| 去重 | 窗口 2s，缓存上限 64 条 | 键 = `effectiveId + '\x1f' + level + '\x1f' + title + '\x1f' + body + '\x1f' + sound`。命中 → `Status::Deduped`，不消耗令牌、不调后端。 |
| 限速 | 突发 3 条，之后 1 条/秒 | 令牌桶；无令牌 → `Status::RateLimited`，`retryAfterMs = ceil(不足令牌所需毫秒)`。不调后端。 |

处理顺序：

```
参数清洗 → 授权检查 → 去重 → 限速 → 后端
```

- 授权检查在前：已拒绝时返回 `PermissionDenied` 比 `RateLimited` 更有信息量。
- 去重在前：重复内容不消耗令牌，避免模型重试把桶打空。
- 限速是**全局**的，不按 id/level 分桶；v1 不接受按任务配置。

### 5.5 授权状态机

```
                 initialize()
                      │
        ┌─────────────┴─────────────┐
        ▼                           ▼
   macOS 有 bundle             无 bundle / Windows / POSIX
        │                           │
        ▼                           ▼
  getNotificationSettings      Unknown（Windows 托盘未就绪）
        │                           │
   ┌────┼─────────┐                 └─ send() 继续走后端
   ▼    ▼         ▼
Authorized Denied NotDetermined
   │       │         │
   │       │         └─ requestAuthorization()（异步）
   │       │              └─ 本次 send 返回 NotDetermined
   │       └─ send 返回 PermissionDenied，不再弹窗
   └─ send 继续
```

- `initialize()` 只在 `NotDetermined` 时请求授权；`Denied` 不重复请求。
- macOS 用户可能在运行中改系统设置：注册 `NSApplicationDidBecomeActiveNotification`，
  激活时把缓存置回 `NotDetermined`，下次 `authorizationStatus()` 重新查询；**不**因此
  再次自动弹授权框（系统本身也不会再弹）。
- Windows 没有 app 级通知授权：托盘就绪 → `Authorized`，未就绪 → `Unknown`。
- POSIX → `Unknown`。
- `Authorization::Provisional`（macOS 12+）归入 `Authorized`。

### 5.6 失败语义

- `Result::fail` 只用于：
  - 清洗后 title/body 为空（`ValidationError`）；
  - facade 内部不变量错误（`Internal`）。
- 所有环境状态（无 bundle、拒绝、未决定、后端不可用、限速、去重）都是
  `Result::ok(Receipt{status,...})`。
- 后端 `Result::fail` 被 facade 映射为 `BackendUnavailable`，detail 保留错误消息并
  `LOG_WARN`；不把后端内部错误直接抛给 LLM。

---

## 6. 失败矩阵

| # | 场景 | 层 | 返回 | `app.notify` 输出 | 降级 |
|---|---|---|---|---|---|
| 1 | title 缺失/清洗后为空 | facade | `fail(ValidationError)` | `{"error":"title 不能为空"}`，`isError=true` | 无 |
| 2 | body 缺失/清洗后为空 | facade | `fail(ValidationError)` | `{"error":"body 不能为空"}`，`isError=true` | 无 |
| 3 | title >63 / body >255 unit | facade | `ok(Accepted)` + `*Truncated=true` | `status=accepted`，`truncated=true` | 无 |
| 4 | 非法 UTF-8 | facade | 清洗后继续 | `status=accepted` | 无 |
| 5 | 相同 id+内容 2s 内重复 | facade | `ok(Deduped)` | `status=deduped`，`isError=false` | 无（避免刷屏） |
| 6 | 超过 3 条突发 / 1 条每秒 | facade | `ok(RateLimited)` | `status=rate_limited`，`retryAfterMs` | 无 |
| 7 | macOS 裸二进制 / 无 `.app` | macOS backend | `ok(NotBundled)` | `status=fallback`，`reason=not_bundled` | 应用内 toast |
| 8 | macOS 用户拒绝授权 | macOS backend / facade | `ok(PermissionDenied)` | `status=fallback`，`reason=permission_denied` | 应用内 toast |
| 9 | macOS 尚未决定 | macOS backend / facade | `ok(NotDetermined)`（并发起授权） | `status=fallback`，`reason=not_determined` | 应用内 toast |
| 10 | macOS `addNotificationRequest` 异步报错 | macOS backend | 已返回 `Accepted`，仅 `LOG_WARN` | `status=accepted`（无法事后改口） | 无；文档声明「受理≠展示」 |
| 11 | macOS Focus/勿扰/通知中心关闭 | 系统 | `Accepted`，无法检测 | `status=accepted` | 无；文档声明 |
| 12 | Windows 托盘未初始化 | Win backend | `ok(BackendUnavailable)` | `status=fallback`，`reason=backend_unavailable` | 应用内 toast |
| 13 | Windows `NIM_MODIFY` 返回 FALSE | Win backend | `ok(BackendUnavailable)` | 同上 | 应用内 toast |
| 14 | Windows Explorer 重启后托盘消失 | Win backend | `BackendUnavailable` | 同上 | 应用内 toast；v1 不重注册 |
| 15 | Windows UI 线程 >1.5s 无响应 | Win backend | `BackendUnavailable`（超时） | 同上 | 应用内 toast（若 UI 仍卡则 toast 也看不到） |
| 16 | Windows Focus Assist/通知关闭 | 系统 | `Accepted`，无法检测 | `status=accepted` | 无；文档声明 |
| 17 | Linux/其它 POSIX | POSIX backend | `ok(BackendUnavailable)` | `status=fallback` | 应用内 toast |
| 18 | 降级队列也失败（模块已停） | module_agent | `fail(Internal)` | `{"error":"..."}`，`isError=true` | 无 |
| 19 | 非主线程调用 `initialize` | facade | 记录 `LOG_WARN`，不崩 | 不影响 send | 无 |
| 20 | `id` 超长/非法 UTF-8 | facade | 清洗/哈希后继续 | `id` 为派生值 | 无 |

---

## 7. `app.notify` 工具契约

### 7.1 名称与定位

- 名称：`app.notify`（与 `app.stats` 同前缀，短、动词）。
- 位置：`src/module_agent/app_notify_tool.{h,cpp}`，由 `AgentModule::onStart()` 注册。
- 非破坏性：`isDestructive() == false`。
- 执行线程：AgentLoop 工作线程；工具内部不碰任何 UI 对象。

### 7.2 schema

```json
{
  "name": "app.notify",
  "description": "向用户发送系统通知。适用于：长任务完成、需要用户回来处理的提醒。不要用于进度汇报、闲聊或高频重复内容。标题单行、正文简短；不要包含密钥、令牌等敏感信息。返回 JSON 说明系统是否受理，以及是否降级为应用内提示。",
  "input_schema": {
    "type": "object",
    "properties": {
      "title": {
        "type": "string",
        "description": "通知标题，单行，建议不超过 30 个汉字"
      },
      "body": {
        "type": "string",
        "description": "通知正文，可多行，建议不超过 120 个汉字"
      },
      "sound": {
        "type": "boolean",
        "description": "是否播放提示音，默认 true"
      },
      "level": {
        "type": "string",
        "enum": ["info", "success", "warning", "error"],
        "description": "通知级别，默认 info"
      },
      "id": {
        "type": "string",
        "description": "可选稳定标识；相同 id + 内容在 2 秒内去重；相同 id 会替换 macOS 中未展示/已展示的同 id 通知"
      }
    },
    "required": ["title", "body"],
    "additionalProperties": false
  }
}
```

### 7.3 参数校验

| 参数 | 规则 |
|---|---|
| `title` | 必填字符串；trim 后非空；超过 63 UTF-16 unit 由平台层截断。 |
| `body` | 必填字符串；trim 后非空；超过 255 unit 由平台层截断。 |
| `sound` | 可选 bool；缺省 true。 |
| `level` | 可选 enum；缺省 `info`；未知值 → 工具错误（不静默降级成 info）。 |
| `id` | 可选字符串；空 = 派生；长度 >64 或非法 UTF-8 由平台层处理。 |
| 未知字段 | `additionalProperties:false`；解析层忽略未知字段，不报错（与现有工具一致）。 |

错误 JSON（`ToolResult.isError = true`）：

```json
{"error":"title 不能为空"}
```

### 7.4 输出 JSON

成功（系统受理）：

```json
{
  "status": "accepted",
  "deliveredVia": "system",
  "id": "devdesk-8f3a1c0d9e2b4a71",
  "truncated": false
}
```

去重/限速（不算失败）：

```json
{"status":"deduped","deliveredVia":"none","id":"devdesk-..."}
{"status":"rate_limited","deliveredVia":"none","retryAfterMs":640}
```

降级：

```json
{
  "status": "fallback",
  "deliveredVia": "app_toast",
  "reason": "permission_denied",
  "detail": "用户未授权系统通知",
  "id": "devdesk-..."
}
```

- `deliveredVia` 取值：`system` / `app_toast` / `none`。
- `reason` 取值：`not_bundled` / `permission_denied` / `not_determined` /
  `backend_unavailable`。
- `isError`：
  - `accepted` / `deduped` / `rate_limited` / `fallback` → `false`；
  - 参数错误 / 降级也失败 → `true`。
- `truncated`：title 或 body 任一被截断即为 true；可选附 `titleTruncated`/`bodyTruncated`。
- 不把 `detail` 里的系统路径/内部错误堆栈回喂给 LLM；只给用户可理解的中文。

### 7.5 注册与生命周期

`AgentModule` 新增：

```cpp
// agent_module.h
std::shared_ptr<::core::ITool> notifyTool_;
std::mutex fallbackNoticeMu_;
std::deque<std::pair<std::string, std::string>> fallbackNotices_;

bool enqueueFallbackNotice(const std::string& title, const std::string& body);
std::optional<std::pair<std::string, std::string>> takeFallbackNotice();
```

`onStart()`（在 `app.stats` 之后）：

```cpp
notifyTool_ = makeAppNotifyTool(
    [this](const std::string& title, const std::string& body) {
        return enqueueFallbackNotice(title, body);
    });
auto regNotify = ::core::ToolRegistry::instance().registerTool(notifyTool_);
if (regNotify.isFail()) {
    LOG_ERROR("Agent: app.notify 注册失败: " + regNotify.errorMessage());
}
```

`onStop()`（join 之后，与 `app.stats` 同段）：

```cpp
::core::ToolRegistry::instance().unregisterTool("app.notify");
::core::ToolRegistry::instance().unregisterTool("app.stats");
notifyTool_.reset();
statsTool_.reset();
// 清空降级队列（join 后不会再有生产者）
{
    std::lock_guard<std::mutex> lock(fallbackNoticeMu_);
    fallbackNotices_.clear();
}
```

- 注册失败只 `LOG_ERROR`，不中断模块（与 `app.stats` 同口径）。
- `onStart` 的 `db == nullptr` 早退路径不会注册任何工具；此时智能体本身不可用，`app.notify`
  不存在是合理的。
- 工具持有的是 `FallbackFn`，不持有 `AgentModule&`；`onStop` join 后再 reset，
  不存在工具执行中模块析构的路径。

### 7.6 降级队列

- 独立于 `pendingNotices_`：后者是 run-end 专用、仅 UI 线程；新队列有 mutex，生产者是
  AgentLoop 工作线程。
- 容量 8 条；满时丢最旧（不无限增长）。
- `app_sidebar::compose()` 在现有 run-end drain 之后追加：

```cpp
while (auto notice = agent->takeFallbackNotice()) {
    showAppNotice(notice->first, notice->second);
}
```

- 同帧既有 run-end 又有 fallback：toast 单实例，后发顶替先发；fallback 后 drain 所以
  fallback 显示。可接受（与 M3 单实例语义一致）。
- 窗口隐藏到托盘/最小化时，`compose` 是否运行取决于 EUI-NEO 的窗口循环；若不可见，
  fallback toast 也不可见。这是「系统通知失败后的最后兜底」，不承诺必达。

### 7.7 工具层为什么不做限速/去重

限速/去重放平台层，原因：

1. 所有触发源共享同一配额（未来 run-end 若改用系统通知也不会绕过）；
2. Windows/macOS 后端不需要各自复制一份；
3. 工具层只做语义映射，测试更简单。

---

## 8. macOS 实现细节

### 8.1 Bundle guard（最高优先级）

`UNUserNotificationCenter.currentNotificationCenter` 在无 bundle identifier 的裸进程里会抛
`NSInternalInconsistencyException`，不是 C++ 可捕获异常。所有入口第一步：

```objc
bool hasUsableAppBundle() {
    NSBundle* bundle = [NSBundle mainBundle];
    return bundle.bundleIdentifier.length > 0 &&
           [[bundle.bundleURL pathExtension] isEqualToString:@"app"];
}
```

- `initialize()`：guard 失败直接返回；不安装 delegate、不查授权。
- `send()`：guard 失败返回 `Status::NotBundled`。
- `authorizationStatus()`：guard 失败返回 `Unknown`。
- `requestAuthorization()`：guard 失败只 `LOG_WARN`，不调用系统 API。

### 8.2 Delegate 与前台展示

- `UNUserNotificationCenter.delegate` 是 **weak**；必须由文件内强引用单例持有
  （`dispatch_once` 创建），否则 delegate 立刻释放，前台通知静默不展示。
- `willPresentNotification`：

```objc
completionHandler(UNNotificationPresentationOptionBanner |
                  UNNotificationPresentationOptionList |
                  UNNotificationPresentationOptionSound);
```

- 必须及时调用 `completionHandler`，不能漏。
- `didReceiveNotificationResponse`：
  - v1 只记录 id（不记内容）；
  - `dispatch_async(dispatch_get_main_queue(), ^{ ... })` 后
    `[NSApp activateIgnoringOtherApps:YES]`；若 `[NSApp isHidden]` 先 `unhideWithoutActivation`；
  - 调用 `completionHandler()`；
  - 不做页面路由/任务定位（见 §13 D-3）。

### 8.3 授权

- `initialize()` 在主线程调用；用 `getNotificationSettingsWithCompletionHandler` 异步查询，
  避免启动期阻塞。
- 若 `UNAuthorizationStatusNotDetermined`：
  - `dispatch_async(dispatch_get_main_queue(), ^{ requestAuthorization(); })`，
    让应用先完成启动；
  - 首次启动会出现系统授权弹窗（见 §13 D-1）。
- 请求选项：`UNAuthorizationOptionAlert | UNAuthorizationOptionSound`；不请求 Badge，
  不请求 Provisional。
- 结果缓存到 `std::atomic<Authorization>`；`Denied` 后不重复请求。
- 注册 `NSApplicationDidBecomeActiveNotification`：激活时把缓存置回 `NotDetermined`，
  下次查询重新读系统设置；不自动再次弹授权框。
- `authorizationStatus()` 的同步查询最多等 1s；超时返回缓存并 `LOG_WARN`。
- `UNAuthorizationStatusProvisional`（macOS 12+）映射为 `Authorized`。

### 8.4 投递

- 内容：`content.title`、`content.body`、`content.sound = defaultSound`（仅 `sound=true`）。
- `content.userInfo` 写入 `{"level":"warning"}`，为将来点击路由留字段；v1 不消费。
- `UNNotificationRequest identifier = receipt.effectiveId`，`trigger = nil`（立即投递）。
- `addNotificationRequest:withCompletionHandler:` 异步入队；返回 `Accepted` 只代表系统受理。
- completion 里的 `NSError` 只 `LOG_WARN`，不改变已经返回的回执。
- 同一 id 的再次发送会替换系统里同 id 的通知（macOS 语义）；工具文档已说明。
- Focus/勿扰/通知中心关闭会抑制展示，平台层无法可靠检测；`Accepted ≠ 用户看到`。

### 8.5 签名与打包

#### 8.5.1 为什么必须重签

- 现有 `.app` 是链接器 ad-hoc 签名：`Identifier=DevDesk`，与
  `CFBundleIdentifier=com.devdesk.app` 不一致；`Info.plist=not bound`；
  `codesign --verify --strict` 直接失败。
- 系统通知/TCC 以 bundle 身份为键；签名不合法会导致授权行为不稳定（每次重建都可能重新询问）。
- 目标不是通过 Gatekeeper（需要 Developer ID + notarization），而是让 bundle 身份自洽。

#### 8.5.2 新增 `scripts/sign-macos-app.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

APP="${1:?usage: sign-macos-app.sh <path/to/DevDesk.app>}"
if [ ! -d "$APP" ]; then
  echo "❌ 不是 .app bundle: $APP" >&2
  exit 1
fi

# 只签 bundle 根：DevDesk 没有嵌套 framework/dylib，不需要 --deep。
# 不用 --options runtime：本机 ad-hoc 分发不做硬化运行时/公证。
codesign --force --sign - --identifier com.devdesk.app "$APP"
codesign --verify --strict "$APP"

IDENT="$(codesign -dv --verbose=2 "$APP" 2>&1 | sed -n 's/^Identifier=//p' | head -1)"
if [ "$IDENT" != "com.devdesk.app" ]; then
  echo "❌ 签名 identifier 不是 com.devdesk.app: $IDENT" >&2
  exit 1
fi
echo "✅ ad-hoc 重签完成: $APP（Identifier=com.devdesk.app）"
```

#### 8.5.3 调用点（顺序即语义）

| 脚本 | 调用位置 | 说明 |
|---|---|---|
| `platform/macos/build-app.sh` | assets 拷贝之后（约 L133-142）、lsregister 之前 | 先让 bundle 内容完整，再封印资源。 |
| `scripts/package-macos.sh` | 二次 assets 拷贝/断言之后（约 L61-68）、`cp -R` 到 DMG stage（约 L86）之前 | DMG 里必须是已签名的 bundle。 |
| `scripts/build-test-run.sh --install` | assets 补拷/断言之后（约 L745-751）、`ditto` 到 `/Applications`（约 L777）之前 | 装到 `/Applications` 的副本继承签名；安装后再 `codesign --verify --strict` 断言。 |

- 三个调用点都必须失败即 `exit 1`，不允许「签名失败但继续出包」。
- 不修改 `Info.plist` 的 `CFBundleIdentifier`（已经是 `com.devdesk.app`）。
- 不需要新的 entitlement；本地通知不需要 App Sandbox 权限。
- `build-app.sh` 在 `DEVDESK_SKIP_LSREGISTER=1` 的打包路径下也会重签；lsregister 在签名后执行
  不会破坏签名（只写 LaunchServices 数据库）。

#### 8.5.4 验收与限制

- `codesign --verify --strict build/DevDesk.app` 退出码 0。
- `codesign -dv --verbose=2 build/DevDesk.app` 显示 `Identifier=com.devdesk.app`、
  `Signature=adhoc`。
- `spctl -a -vv build/DevDesk.app` **仍然会被拒绝**（ad-hoc、未公证）；DMG 继续保留
  「首次运行（去隔离）」说明。本文不承诺 Gatekeeper 通过。
- 每次重建签名都会变；macOS 可能把 dev 构建视作同一 bundle id 但不同签名。真机 UAT
  必须验证「重建后是否重复弹授权」。若重复弹，记录为已知限制，长期方案是 Developer ID。

---

## 9. Windows 实现细节

### 9.1 方案选择

| 方案 | 结论 | 理由 |
|---|---|---|
| WinRT `ToastNotificationManager` | ❌ v1 不做 | 需要 AUMID + 开始菜单快捷方式 + COM/WinRT 初始化；打包/安装链路复杂，单 exe 零 DLL 约束下成本高。 |
| 托盘气泡 `Shell_NotifyIconW(NIF_INFO)` | ✅ v1 | 复用 EUI-NEO 已有 Win32 托盘；无新依赖；Win10/11 可见。 |
| 自建 `CreateWindow` + 自绘 toast | ❌ | 窗口隐藏/最小化时不可见，失去系统通知意义。 |

接受 Windows 11 的托盘气泡是**临时横幅**、不进通知中心、用户错过就没了（见 §13 D-2）。

### 9.2 EUI-NEO 新增 C API

`EUI-NEO/core/platform/tray_bridge.h` 增加（仅 `_WIN32`）：

```c
#if defined(_WIN32)
/* 用现有托盘图标弹一条气泡通知。
 * - 可从任意线程调用；内部封送到创建托盘的 UI 线程。
 * - title/body 为 UTF-8；本函数负责转 UTF-16 并按 NOTIFYICONDATAW 上限截断。
 * - level: 0=info 1=success 2=warning 3=error；sound: 0/1。
 * - 返回 1=系统受理（Shell_NotifyIconW 返回 TRUE），0=托盘未初始化/失败/超时。
 * - 不重试、不排队；调用方（DevDesk platform 层）负责降级。 */
int eui_tray_show_notification(const char* title, const char* body,
                               int level, int sound);
#endif
```

- 只在 `EUI_TRAY_WINAPI` 分支实现；其它后端/无后端返回 0（保持头文件跨平台可包含）。
- 不改变现有 `eui_tray_init` / `eui_tray_poll` / 菜单 API 的语义。

### 9.3 UI 线程封送

托盘 `nid`/`hwnd` 是 `tray.h` 内部的静态量，必须只在创建它的 UI 线程访问。
`tray_bridge.c` 的 Win32 分支新增一个 **message-only window**：

- 在 `eui_tray_init()` 成功调用 `tray_init()` 后创建：
  `CreateWindowExW(0, L"EUI_TRAY_NOTIFY", L"", 0, 0,0,0,0, HWND_MESSAGE, NULL, hInstance, NULL)`。
- 注册窗口类 `EUI_TRAY_NOTIFY`，WndProc 处理自定义消息 `WM_EUI_TRAY_NOTIFY`（`WM_APP+1`）。
- `eui_tray_shutdown()` 里 `DestroyWindow(g_notify_hwnd)` 并置空。
- 未初始化时 `eui_tray_show_notification()` 直接返回 0。

调用方（工作线程）→ `SendMessageTimeoutW` → UI 线程 WndProc：

```c
typedef struct eui_tray_notify_payload {
    volatile LONG state;       /* 0=发送方持有 1=接收方持有 2=已取消 */
    wchar_t title[64];
    wchar_t body[256];
    UINT info_flags;
} eui_tray_notify_payload;
```

- 发送方堆分配 payload，填好 title/body/flags，`state=0`。
- `SendMessageTimeoutW(hwnd, WM_EUI_TRAY_NOTIFY, 0, (LPARAM)p,
  SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result)`。
- WndProc 入口用 `InterlockedCompareExchange(&p->state, 1, 0)` 认领：
  - 返回 0（发送方已取消）→ 不碰字段，直接返回 0；
  - 认领成功 → 构造 `NOTIFYICONDATAW`，调用 `Shell_NotifyIconW(NIM_MODIFY, &data)`，
    释放 payload，返回 `ok ? 1 : 0`。
- 发送方超时/失败后用 `InterlockedCompareExchange(&p->state, 2, 0)`：
  - 成功 → 自己释放；
  - 失败（接收方已认领）→ 接收方负责释放。
- 这样即使 `SendMessageTimeout` 超时，也不会出现「UI 线程稍后读已释放栈内存」的 UAF；
  最坏只是 UI 线程永久卡死时泄漏一个 payload（限速已把频率封顶）。

**为什么不用 `PostMessage` + 事件**：同样需要处理「超时后接收方仍可能读 payload」的生命周期，
引用计数并不能省；`SendMessageTimeout` 能直接拿到 `NIM_MODIFY` 的真实结果。

### 9.4 `NOTIFYICONDATAW` 与文本

WndProc 内使用**显式 W 版**，不依赖 `UNICODE` 宏：

```c
NOTIFYICONDATAW data;
memset(&data, 0, sizeof(data));
data.cbSize = sizeof(data);
data.hWnd = g_tray_hwnd;      /* tray.h 的 hwnd */
data.uID = 0;
data.uFlags = NIF_INFO;
data.dwInfoFlags = p->info_flags;
StringCchCopyW(data.szInfoTitle, 64, p->title);
StringCchCopyW(data.szInfo, 256, p->body);
BOOL ok = Shell_NotifyIconW(NIM_MODIFY, &data);
```

- 只设置 `NIF_INFO`，不碰 `NIF_ICON`/`NIF_MESSAGE`，避免覆盖托盘常驻状态。
- `level` 映射：

| `Level` | `dwInfoFlags` |
|---|---|
| Info | `NIIF_INFO` |
| Success | `NIIF_INFO` |
| Warning | `NIIF_WARNING` |
| Error | `NIIF_ERROR` |

- `sound == false` → 追加 `NIIF_NOSOUND`。
- 不使用 `NIIF_USER`（需要 `hIcon`/`NIF_ICON`，会把气泡图标换成应用图标，行为不稳定）。
- 标题 64 wchar / 正文 256 wchar，含结尾 NUL；facade 已按 63/255 截断，这里再防一次。
- `NOTIFYICONDATAW` 的 `uTimeout` 已废弃，不设置。

### 9.5 `notification_win.cpp`

```cpp
core::Result<Status> WinBackend::send(const Notification& n) {
    const int level = static_cast<int>(n.level);  // 0..3，与 EUI C API 对齐
    if (!eui_tray_show_notification(n.title.c_str(), n.body.c_str(),
                                    level, n.sound ? 1 : 0)) {
        return core::Result<Status>::ok(Status::BackendUnavailable);
    }
    return core::Result<Status>::ok(Status::Accepted);
}

Authorization WinBackend::authorizationStatus() {
    return eui_tray_is_initialized() ? Authorization::Authorized
                                     : Authorization::Unknown;
}

void WinBackend::requestAuthorization() {}
void WinBackend::initialize() {}
```

- 失败 detail 由 facade 填「Windows 托盘通知不可用（托盘未初始化、Explorer 重启或消息超时）」。
- 不在这里做 UTF-8 → UTF-16；EUI-NEO 的 `eui_tray_utf8_to_utf16` 统一处理。
- `notification_win.cpp` 只 include `platform/notification.h` 与
  `core/platform/tray_bridge.h`，不 include `<windows.h>`，保持薄。

### 9.6 Windows 已知限制

1. 托盘未初始化/Explorer 重启/`NIM_MODIFY` FALSE → `BackendUnavailable`，降级 toast。
   v1 不监听 `TaskbarCreated`、不重注册托盘（EUI-NEO 现有托盘也没有）。
2. Windows 11 气泡是临时横幅，不进通知中心；用户错过即丢失。
3. Focus Assist/通知设置可能静默抑制，`Shell_NotifyIconW` 仍返回 TRUE，无法检测。
4. UI 线程卡死超过 1.5s → 超时降级；此时 toast 也大概率不可见。
5. 点击气泡没有回调路由；v1 不处理 `NIN_BALLOONUSERCLICK`。
6. 通知标题/正文长度受 `NOTIFYICONDATAW` 硬限制，超出部分截断。

---

## 10. 测试策略

### 10.1 平台层单元测试（`tests/test_platform/test_notification.cpp`）

所有发送都走 fake backend；默认后端只在裸进程 guard 用例里调用，且不触发真实通知。

| 用例 | 覆盖 |
|---|---|
| `notification_send_forwards_fields` | 字段/level/sound/id 原样转发（清洗后）。 |
| `notification_backend_failure_maps_unavailable` | 后端 `Result::fail` → `BackendUnavailable`，不抛。 |
| `notification_empty_title_or_body_rejected` | 清洗后为空 → `ValidationError`，后端零调用。 |
| `notification_utf8_sanitized` | 非法 UTF-8 → U+FFFD，发送继续。 |
| `notification_title_body_truncated` | 63/255 unit、`…`、`truncated` 标志、代理对不切开。 |
| `notification_title_control_chars` | CR/LF/TAB/C0 清洗、连续空格折叠。 |
| `notification_body_newlines_preserved` | CRLF → LF、3+ 换行折 2、首尾 trim。 |
| `notification_id_derived_when_empty` | 空 id → `devdesk-<hash>`，稳定。 |
| `notification_dedupe_same_content` | 2s 内同 id+内容 → `Deduped`，后端只调用一次。 |
| `notification_dedupe_expires` | 假时钟前进 2s 后可再发。 |
| `notification_dedupe_different_content` | 同 id 不同内容不误去重。 |
| `notification_rate_limit_burst` | 3 条通过，第 4 条 `RateLimited`，`retryAfterMs > 0`。 |
| `notification_rate_limit_refill` | 假时钟前进 1s 后恢复 1 条。 |
| `notification_authorization_denied` | `Denied` → `PermissionDenied`，后端零调用。 |
| `notification_authorization_not_determined` | `NotDetermined` → 返回 `NotDetermined` 且调用 `requestAuthorization`。 |
| `notification_authorization_unknown_proceeds` | `Unknown` 仍尝试后端（Windows 托盘未就绪场景）。 |
| `notification_default_backend_in_bare_process` | macOS 裸进程 → `NotBundled` / 不崩；Windows/POSIX → `BackendUnavailable`。 |
| `notification_backend_seam_restores_default` | 注入/恢复/`resetForTest` 不悬垂。 |
| `notification_initialize_idempotent` | 连续调两次不重复初始化（fake 计数）。 |

### 10.2 工具测试（新增 `tests/test_modules/test_app_notify_tool.cpp`）

| 用例 | 覆盖 |
|---|---|
| `app_notify_schema_valid` | schema 可被 `platform::json` 解析，name/required/level enum 正确。 |
| `app_notify_success` | fake 后端 `Accepted` → `status=accepted`、`deliveredVia=system`、`isError=false`。 |
| `app_notify_permission_denied_fallback` | fake 返回 `PermissionDenied` → fallback 回调收到 title/body，输出 `status=fallback`。 |
| `app_notify_not_bundled_fallback` | 同上，reason `not_bundled`。 |
| `app_notify_not_determined_fallback` | 同上，reason `not_determined`；确认 `requestAuthorization` 被调。 |
| `app_notify_rate_limited_no_fallback` | `RateLimited` → 输出 `rate_limited`，fallback 零调用。 |
| `app_notify_deduped_no_fallback` | `Deduped` → 输出 `deduped`，fallback 零调用。 |
| `app_notify_missing_title` | 缺 title → `isError=true`，错误 JSON。 |
| `app_notify_invalid_level` | `level="fatal"` → `isError=true`。 |
| `app_notify_default_sound_true` | 缺省 sound → true。 |
| `app_notify_fallback_also_fails` | fallback 回调返回 false → `isError=true`。 |
| `app_notify_is_not_destructive` | `isDestructive()==false`。 |

### 10.3 模块/生命周期回归

| 文件 | 需要改的断言 |
|---|---|
| `tests/test_modules/test_agent_tools.cpp` | `onStart` 后 `find("app.notify")`/`find("app.stats")` 都 ok；`onStop` 后都 fail；双 onStart/双 onStop 不变。 |
| `tests/test_modules/test_agent_ui.cpp` | **约 41 处** `ToolGuard statsGuard{"app.stats"}` 扩成同时兜底 `app.notify`；每个用例起点的 `unregisterTool("app.stats")` 补 `unregisterTool("app.notify")`。 |
| `tests/test_modules/test_agent_ui.cpp` 工具区 | `registered` 从 1 变 2；`agent.tools.card.0.name` 由 `app.stats` 变 `app.notify`（字典序），card.1 是 `app.stats`；建议改成「两张卡名字集合 == {app.notify, app.stats}」，避免顺序脆断。 |
| `tests/live/agent_live.cpp` | 若该场景直接开 `AgentModule`，收尾补 `unregisterTool("app.notify")`（live 不进默认门禁，但别留污染）。 |
| `tests/test_main.cpp` | 已调 `resetBackendForTest()`；改为 `resetForTest()`（清限速/去重/时钟）。 |
| `tests/CMakeLists.txt` | 新增 `test_modules/test_app_notify_tool.cpp`；TIN-10 漏登记检查会守住。 |

**ToolGuard 推荐写法**（减少 41 处重复）：

```cpp
struct ToolGuard {
    std::vector<std::string> names;
    ToolGuard(std::initializer_list<std::string> n) : names(n) {}
    ~ToolGuard() {
        for (const auto& name : names) {
            core::ToolRegistry::instance().unregisterTool(name);
        }
    }
};
// 用法：
ToolGuard statsGuard{{"app.stats", "app.notify"}};
```

验收时用 `rg 'ToolGuard statsGuard\{"app\.stats"\}' tests/test_modules/test_agent_ui.cpp`
确认零残留。

### 10.4 真机 UAT（T1f）

#### macOS

| 场景 | 预期 |
|---|---|
| 首次启动 `.app` | 出现系统授权弹窗；允许后状态 Authorized。 |
| 拒绝授权后再次发送 | 不重复弹窗；工具返回 fallback/permission_denied；应用内 toast 可见。 |
| 前台发送 | 横幅 + 通知中心列表 + 声音（sound=true）。 |
| 最小化/切到别的应用 | 系统横幅可见；点击激活 DevDesk。 |
| 系统设置里关闭通知 | 发送返回 accepted，但无横幅；文档声明无法检测。 |
| Focus/勿扰 | 同上。 |
| 超长标题/正文 | 不崩溃；截断到 63/255；`truncated=true`。 |
| emoji/补充平面字符 | 不出现乱码/半个代理对。 |
| 非法 UTF-8（测试注入） | 替换为 U+FFFD，不崩溃。 |
| 连续快速发送 5 条 | 前 3 条受理，第 4/5 条 `rate_limited`。 |
| 相同内容 2s 内重复 | `deduped`。 |
| 点击通知 | app 激活；日志有 id；不跳页。 |
| 重建 `.app` 后再发 | 观察是否重复弹授权；记录结果。 |

#### Windows

| 场景 | 预期 |
|---|---|
| 托盘就绪后发送 | 气泡横幅出现；级别图标正确。 |
| app 最小化 | 气泡仍出现（托盘存在）。 |
| 托盘未初始化（启动早期） | 返回 fallback/backend_unavailable；应用内 toast。 |
| Explorer 重启后发送 | 返回 fallback；不崩溃；不无限重试。 |
| Focus Assist 开启 | 可能无横幅；返回 accepted；文档声明。 |
| sound=false | 无提示音。 |
| 超长/中文/emoji | 不崩溃；按 63/255 wchar 截断。 |
| UI 线程卡死（调试断点） | 1.5s 超时；返回 backend_unavailable；不 UAF。 |
| 打包单 exe | 无新增 DLL；`scripts/package-windows.sh` 仍通过。 |

### 10.5 验收命令

```bash
# macOS 单测（平台层）
bash scripts/build-test-run.sh --filter notification

# 工具/模块
bash scripts/build-test-run.sh --filter app_notify
bash scripts/build-test-run.sh --filter agent_module
bash scripts/build-test-run.sh --filter agent_ui
bash scripts/build-test-run.sh --filter agent_tools

# 全量（以运行输出 Total 为准）
bash scripts/build-test-run.sh --no-run

# 模块隔离
bash scripts/check-module-isolation.sh

# 签名（T1e）
bash platform/macos/build-app.sh
codesign --verify --strict build/DevDesk.app
codesign -dv --verbose=2 build/DevDesk.app 2>&1 | grep '^Identifier='

# 打包（T1e）
bash scripts/package-macos.sh
```

Windows 侧对应：

```powershell
bash scripts/build-test-run.sh --filter notification
bash scripts/build-test-run.sh --filter app_notify
```

---

## 11. 分阶段实施

> 每个阶段独立提交、独立回滚。T1a 是工作区已有草案，建议先单独收口，避免和后续
> 接口演进混在一个提交里。

### T1a — 现有草案收口（基线，已完成待提交）

- **范围**：`notification.h` + macOS 后端 + Windows/POSIX stub + `test_notification.cpp`
  + CMake 接线 + `resetCaseGlobals`。
- **依赖**：无。
- **验收**：
  - `bash scripts/build-test-run.sh --filter notification` 绿；
  - 裸测试进程不崩；
  - `git status` 只包含通知相关文件。
- **回滚**：删除/还原这批未提交文件即可（不影响其它模块）。
- **备注**：如果用户不希望保留 `Result<void>` 草案接口，可直接进入 T1b 并在同一提交里
  完成签名收敛；但建议先让 T1a 可独立编译/测试。

### T1b — 平台 facade 收口（macOS 完成）

- **范围**：
  - `src/platform/notification.h`：`Level`、`Status`、`Receipt`、`initialize`、
    `resetForTest`、`setClockForTest`；
  - 新增 `src/platform/notification.cpp`：清洗/截断/派生 id/限速/去重/授权状态/后端槽；
  - `src/platform/notification_macos.mm`：适配新 `Backend::send` 返回 `Result<Status>`；
    `initialize()` 安装 delegate/观察者；点击激活；
  - `notification_win.cpp` / `notification_posix.cpp`：适配新接口，仍为 stub；
  - `src/platform/CMakeLists.txt`：加入 `notification.cpp`；
  - `tests/test_platform/test_notification.cpp`：新增 §10.1 用例；
  - `tests/test_main.cpp`：`resetForTest()`。
- **依赖**：T1a。
- **步骤**：
  1. 把 `send` 从 header inline 移到 `notification.cpp`，保留 `setBackendForTest` 原子槽。
  2. 实现清洗/截断/派生 id（§5.3），用 fake backend 断言最终传入的字符串。
  3. 实现令牌桶 + 去重（§5.4），注入假时钟。
  4. 实现授权状态机（§5.5）：`Denied`/`NotDetermined` 早退；`Unknown` 继续。
  5. macOS 后端：`send` 返回 `Status`；`initialize` 安装 delegate/激活观察者；
     `didReceive` 激活 app；`authorizationStatus` 保留 1s 超时缓存。
  6. 更新 stub 为 `BackendUnavailable`/`Unknown`。
  7. 更新测试与 `resetForTest`。
- **验收**：
  - `--filter notification` 绿；
  - `--filter agent` 现有用例不回归（工具尚未加入，计数不变）；
  - 裸进程 guard 用例绿；
  - 无生产调用方（`rg 'platform::notify::send' src` 为空）。
- **回滚**：`git revert` T1b；T1a 仍在。

### T1c — Windows 真实后端

- **范围**：
  - `EUI-NEO/core/platform/tray_bridge.h`：新增 `eui_tray_show_notification` 声明；
  - `EUI-NEO/core/platform/tray_bridge.c`：message-only window + WndProc +
    `SendMessageTimeoutW` + payload 引用计数 + `NOTIFYICONDATAW`；
  - `DevDesk/src/platform/notification_win.cpp`：真实实现；
  - `tests/test_platform/test_notification.cpp`：Windows 默认后端未初始化 → 
    `BackendUnavailable`，不崩。
- **依赖**：T1b。
- **步骤**：
  1. 在 `eui_tray_init` 成功后创建 message-only window；`shutdown` 销毁。
  2. 实现 `eui_tray_show_notification`（UTF-8→UTF-16、级别/声音、1.5s 超时）。
  3. WndProc 用局部 `NOTIFYICONDATAW` + `NIF_INFO`，不污染常驻 `nid`。
  4. `notification_win.cpp` 映射 `Accepted`/`BackendUnavailable`；授权按托盘状态。
  5. Windows 构建 + 单测。
- **验收**：
  - Windows `bash scripts/build-test-run.sh --filter notification` 绿；
  - `rg 'notification_win.cpp.*stub'` 不再出现；
  - `EUI-NEO` 单测/构建不回归（若 EUI-NEO 有 tray bridge 测试，补一条纯逻辑/不初始化返回 0）；
  - 手工：托盘就绪后调用（T1d 后）能看到气泡。
- **回滚**：还原 EUI-NEO 两个文件 + Windows backend 为 stub；DevDesk 其它层不受影响。
- **注意**：EUI-NEO fork 永不 push origin；本阶段提交留在本地 `main`，DevDesk 侧不 pin hash。

### T1d — `app.notify` 工具 + 降级队列

- **范围**：
  - 新增 `src/module_agent/app_notify_tool.{h,cpp}`；
  - `src/module_agent/agent_module.{h,cpp}`：`notifyTool_`、fallback 队列、
    `enqueueFallbackNotice`/`takeFallbackNotice`、onStart/onStop；
  - `src/module_agent/CMakeLists.txt`：加入 `app_notify_tool.cpp`；
  - `src/app_sidebar.cpp`：drain fallback；
  - 新增 `tests/test_modules/test_app_notify_tool.cpp`；
  - 更新 `test_agent_tools.cpp`、`test_agent_ui.cpp`、`tests/CMakeLists.txt`。
- **依赖**：T1b；Windows 真实路径依赖 T1c（没有 T1c 时工具仍可用，Windows 全部 fallback）。
- **步骤**：
  1. 定义 `FallbackFn` 与 `makeAppNotifyTool(FallbackFn)`；schema/校验/输出。
  2. `AgentModule` 注册/注销；fallback 队列（mutex + cap 8）。
  3. `app_sidebar` drain。
  4. 更新工具卡断言与 ToolGuard。
  5. 单测。
- **验收**：
  - `--filter app_notify` 绿；
  - `--filter agent_module` / `agent_ui` / `agent_tools` 绿；
  - `rg 'app\.notify' src` 显示仅工具/注册/测试预期位置；
  - `check-module-isolation.sh` 绿；
  - 工具不再直接调用 `app::showAppNotice`（`rg` 确认）。
- **回滚**：移除工具注册 + fallback drain；平台层保留。

### T1e — macOS 签名/打包接线

- **范围**：
  - 新增 `scripts/sign-macos-app.sh`；
  - `platform/macos/build-app.sh`、`scripts/package-macos.sh`、
    `scripts/build-test-run.sh --install` 三处调用；
  - 安装后 `codesign --verify --strict` 断言。
- **依赖**：T1d（需要真实 `.app` 发通知验证）；可与 T1d 并行开发，但验收在 T1d 后。
- **步骤**：
  1. 写脚本并本地试跑（对现有 `build/DevDesk.app`）。
  2. 三处调用点插入；失败即 exit 1。
  3. `--install` 段补签名验证。
  4. 打包验证。
- **验收**：
  - `codesign --verify --strict build/DevDesk.app` 退出 0；
  - `Identifier=com.devdesk.app`；
  - DMG 内 `.app` 同样通过；
  - `/Applications/DevDesk.app` 安装后通过；
  - `spctl` 仍拒绝，文档写明这是预期。
- **回滚**：移除调用与脚本；app 仍可运行，但系统通知授权可能不稳定。

### T1f — 真机 UAT + 文档

- **范围**：
  - `docs/USER_GUIDE.md`：通知权限、系统设置、限制。
  - `docs/EUI-NEO-CAPABILITIES.md`：补充「macOS 系统通知 / Windows 托盘气泡」能力与
    平台差异；注明 EUI-NEO 只提供 Windows 托盘 C API，macOS 由 DevDesk platform 实现。
  - `docs/superpowers/specs/2026-10-agent-roadmap-decisions.md`：追加一条 2026-10-18
    补充说明「`app.notify` 是新触发源，不改变 run-end toast 决策」。
  - 本 spec 回写 UAT 结果/已知限制。
- **依赖**：T1b–T1e。
- **验收**：
  - §10.4 矩阵逐项记录（通过/失败/已知限制）；
  - 全量测试绿；
  - 打包产物可安装、可发通知；
  - 文档中的路径/命令与实际一致。
- **回滚**：文档回滚不影响代码；UAT 失败时按失败项回退对应阶段。

---

## 12. 风险与未决点

### 12.1 风险

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R-1 | macOS 首次启动弹授权，用户未准备好 | 体验打扰/误点拒绝 | 见 D-1；可改为首次 `app.notify` 时再请求（待拍板）。 |
| R-2 | Windows 11 气泡不进通知中心 | 用户错过即丢失 | 见 D-2；v1 接受；若要持久化需 WinRT Toast + AUMID（大工程）。 |
| R-3 | 点击通知只激活 app，不定位任务 | 用户还要自己找 | 见 D-3；v1 接受；`userInfo` 已留 level，未来可加 route。 |
| R-4 | ad-hoc 签名仍过不了 Gatekeeper | 首次打开需去隔离 | 见 D-4；DMG 继续保留说明；长期需 Developer ID + 公证。 |
| R-5 | 重建后签名变化导致重复授权 | 开发期反复弹窗 | 真机验证；若严重，记录为已知限制；长期稳定签名。 |
| R-6 | Windows UI 线程卡死 1.5s | 工具线程阻塞/超时 | 限速 + 超时降级；payload 引用计数防 UAF。 |
| R-7 | Explorer 重启后托盘消失 | 通知失败 | 返回 `BackendUnavailable` + toast；v1 不重注册。 |
| R-8 | Focus/勿扰静默抑制 | 用户没看到但返回 accepted | 文档声明「受理≠展示」；工具描述提醒。 |
| R-9 | 模型刷通知 | 打扰用户 | 全局 3 burst/1 每秒 + 2s 去重；仍建议在工具描述里写「不要高频」。 |
| R-10 | 通知内容含敏感信息 | 泄露到通知中心/锁屏 | 工具描述警告；不写日志；用户可在系统设置关闭。 |
| R-11 | fallback toast 在窗口隐藏时不可见 | 最后兜底也失效 | 文档声明；不强制弹窗/抢焦点（避免更糟体验）。 |
| R-12 | 现有未提交草案被覆盖 | 丢失用户工作 | T1a 先收口；T1b 明确列出接口演进；不在本文实施。 |

### 12.2 需要用户拍板

| 编号 | 决策点 | 选项 | 推荐 |
|---|---|---|---|
| D-1 | macOS 授权弹窗时机 | A. 首次启动（`initialize()` 自动请求）；B. 首次调用 `app.notify` 时请求 | **A**（当前草案方向；用户第一次启动就知道有通知能力；但会打断启动） |
| D-2 | Windows 11 临时横幅 | A. 接受（v1）；B. 改用 WinRT Toast（AUMID + 快捷方式 + 安装器改造） | **A**（成本/收益） |
| D-3 | 点击通知行为 | A. 仅激活 app；B. 路由到智能体页/任务详情 | **A**（v1；B 需要 userInfo 路由 + Router 跨模块协调） |
| D-4 | ad-hoc 签名/Gatekeeper | A. 接受首次去隔离；B. 做 Developer ID + 公证 | **A**（本地/小范围分发；B 是发布工程） |
| D-5 | Windows worker 阻塞封送 | A. 最多等 1.5s 拿 `NIM_MODIFY` 结果；B. fire-and-forget 不等结果 | **A**（能区分失败并降级；1.5s 有上限） |
| D-6 | run-end 通知是否迁移到系统通知 | A. 保持 M3 应用级 toast；B. 一并迁移 | **A**（不推翻已拍板决策；`app.notify` 独立） |
| D-7 | 是否接受 T1b 改 `Result<void>` → `Result<Receipt>` | A. 接受（推荐；零生产调用方）；B. 保留 `Result<void>`，限速/状态上移工具层 | **A**（平台层统一状态；B 会让 Windows/macOS 各写一套） |
| D-8 | 限速参数 | A. 3 burst / 1 每秒 / 去重 2s；B. 更宽松/更严格 | **A**（先保守；UAT 后调） |
| D-9 | 通知点击是否播放声音/前台展示 | 固定为 Banner+List+Sound（sound=false 时不响） | 无需拍板，按 spec |

---

## 13. 附录

### 13.1 关键文件清单（目标态）

| 文件 | 动作 |
|---|---|
| `src/platform/notification.h` | 改：Level/Status/Receipt/initialize/resetForTest |
| `src/platform/notification.cpp` | 新增：facade、清洗、限速、去重 |
| `src/platform/notification_macos.mm` | 改：新 Backend 接口、initialize、点击激活 |
| `src/platform/notification_win.cpp` | 改：真实 EUI-NEO 托盘调用 |
| `src/platform/notification_posix.cpp` | 改：新接口，保持 Unavailable |
| `src/platform/CMakeLists.txt` | 改：加入 notification.cpp |
| `src/module_agent/app_notify_tool.h` | 新增 |
| `src/module_agent/app_notify_tool.cpp` | 新增 |
| `src/module_agent/agent_module.h` | 改：notifyTool_、fallback 队列 |
| `src/module_agent/agent_module.cpp` | 改：注册/注销、enqueue |
| `src/module_agent/CMakeLists.txt` | 改：加入工具源 |
| `src/app_sidebar.cpp` | 改：drain fallback |
| `src/main.cpp` | 改：启动调用 `platform::notify::initialize()` |
| `EUI-NEO/core/platform/tray_bridge.h` | 改：新增 Windows C API |
| `EUI-NEO/core/platform/tray_bridge.c` | 改：message-only window + 气泡 |
| `scripts/sign-macos-app.sh` | 新增 |
| `platform/macos/build-app.sh` | 改：调用签名 |
| `scripts/package-macos.sh` | 改：调用签名 |
| `scripts/build-test-run.sh` | 改：`--install` 调用签名 + 验证 |
| `tests/test_platform/test_notification.cpp` | 改：新用例 |
| `tests/test_modules/test_app_notify_tool.cpp` | 新增 |
| `tests/test_modules/test_agent_tools.cpp` | 改：双工具生命周期 |
| `tests/test_modules/test_agent_ui.cpp` | 改：ToolGuard/工具卡 |
| `tests/CMakeLists.txt` | 改：登记新用例 |
| `tests/test_main.cpp` | 改：`resetForTest()` |
| `docs/USER_GUIDE.md` / `docs/EUI-NEO-CAPABILITIES.md` | 改：使用说明/能力盘点 |

### 13.2 关键现状行号（约，随提交漂移）

| 事实 | 位置 |
|---|---|
| `app.stats` 注册 | `src/module_agent/agent_module.cpp:406-411` |
| `app.stats` 注销 | `src/module_agent/agent_module.cpp:440` |
| run-end 入队 | `src/module_agent/agent_module.cpp:1053-1083` |
| `pendingNotices_` | `src/module_agent/agent_module.h:296-298` |
| `takeRunEndNotice` | `src/module_agent/agent_module.cpp:934-941` |
| app toast drain | `src/app_sidebar.cpp:243-250` |
| `showAppNotice` | `src/app_sidebar.cpp:56-58` |
| 测试工具卡断言 | `tests/test_modules/test_agent_ui.cpp:1149`、`:1181-1186` |
| 模块生命周期断言 | `tests/test_modules/test_agent_tools.cpp:183-204` |
| `.app` 组装/assets | `platform/macos/build-app.sh:128-153` |
| 打包二次 assets | `scripts/package-macos.sh:59-86` |
| `--install` 段 | `scripts/build-test-run.sh:704-790` |
| Windows 托盘 `NOTIFYICONDATA` | `EUI-NEO/3rd/tray/tray.h:234`、`:331-337`、`:358-377` |
| Windows tray init | `EUI-NEO/core/platform/tray_bridge.c:1180-1197` |
| `Info.plist` bundle id | `platform/macos/Info.plist:9-10` |

### 13.3 与 M3 决策的关系（必须写进提交说明）

- M3 决策「run 完成用应用级 toast，不推上游系统通知」**继续有效**。
- 本文新增的是 `app.notify` 工具触发的系统通知，属于新的、用户/模型显式发起的通道。
- 不改 `pendingNotices_`、不改 `takeRunEndNotice()`、不改 `app::showAppNotice()` 的 run-end
  调用点；只在 `app_sidebar` 增加一条 fallback drain。
- 因此不需要修订 M3 决策，只需在路线图文档追加一条补充说明（T1f）。

### 13.4 开放问题（实施前需要确认）

1. `main.cpp` 的 `platform::notify::initialize()` 放在 `dslAppConfig` 初始化末尾是否足够早？
   若 macOS 授权弹窗在窗口出现前被系统吞掉，备选：在第一帧 `app::compose` 里调用（需
   避免测试进程触发；用 `if (!ui_kit::splashDisabledForTests())`? 不优雅）。T1b 先按
   `initialize()` 末尾调用验证。
2. `UNUserNotificationCenter` 在 ad-hoc 重签后的授权是否稳定？真机验证。
3. Windows `SendMessageTimeout` 的 1.5s 是否足够覆盖低端机冷启动？UAT 观察。
4. 限速 3 burst/1 每秒是否影响「一次 run 发多条」的合理场景？UAT 后调。
5. 是否需要在工具 schema 里限制 title/body 最大长度（JSON Schema `maxLength`）？
   v1 不在 schema 硬限制，靠平台截断；避免模型因 schema 校验失败而反复重试。
6. fallback toast 在 Windows 窗口隐藏时不可见；是否需要在降级时把窗口带回前台？
   v1 不做（抢焦点比丢通知更打扰）。
