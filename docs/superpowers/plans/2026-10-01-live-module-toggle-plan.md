# 模块开关即时生效（Live Module Toggle）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 四个 Router 页面模块（markdown/todo/claude_proxy/agent）的设置页开关拨动后，侧边栏与导航当帧生效，无需重启。

**Architecture:** 路线 A「注册表活门控」（spec §1）：模块对象常驻（main.cpp 装配段不动），toggle 经中立层桥（照 module_pet_bridge.h 先例）触发 app 层 `dispatchModuleToggle`——off = onStop → Router::unregisterModule；on = registerModule → onStart；关闭正在浏览的页 → navigateTo(Home) + showAppNotice。启动期门控（buildGatedEntries）零语义变化。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（REGISTER_TEST + DOCTEST_CHECK/REQUIRE，fail-fast——CHECK 失败即 return false，RAII 清理放栈对象析构）。

**Spec:** `docs/superpowers/specs/2026-10-01-live-module-toggle-design.md`

## Global Constraints

- **不抛异常**：错误一律 `core::Result<T>`；模块隔离铁律——module_settings 不得 include 兄弟模块头，桥头落 **src 根**（`module_pet_bridge.h` 同级同模式）；改完跑 `bash scripts/check-module-isolation.sh`（settings 不 link 兄弟模块库的断言必须继续绿）。
- **启动期契约零变化**：`buildGatedEntries` 及其全部既有测试（test_app_assembly 的 registry/gated 用例）不得改语义；两个门控路径共用同一谓词 `module::settings::moduleEnabled`（settings_config.h:148）。
- **ToolRegistry 既有语义**（src/core/tool.cpp:11）：同名同指针注册 = no-op ok；同名不同指针 = fail。重入实现必须在此语义下工作。
- **Router 既有语义**（src/core/router.cpp）：`navigateTo` 对未注册页已回退 Home（LOG_WARN）；`isRegistered(PageId)` 已存在；`stopAll` 按注册序（todo/markdown 先于 agent——UAF 修复 555a728 的前提，不得改序）。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`；全量基线 **948/942/0**（+probe 9/9），每任务提交前全量不得回归。
- **提交**：原子提交、显式 pathspec、禁 `git add -A`、提交前 `git status --porcelain`（仓库根 NUL 出现即删）。
- 本计划在 **dev 主检出**直接执行（既有 CI 修复线，非 worktree 新分支；与 CI keychain 修复线串行）。

## Review Focus（五类最易咬人的输入）

1. **双重 onStart**（连点开关/桥重复触发）：工具引用的 service 被替换 → 悬垂；或同名不同指针重复注册 → fail 被吞。钉子：Task 4 各模块 double-onStart no-op 用例。
2. **关闭正在浏览的页**：当前页停留注册表外 → 空白页（fallback compose）。钉子：Task 5 dispatchModuleToggle 重定向 + toast 用例。
3. **桥未注入**（启动早期拨开关 / 测试环境）：toggle 写盘成功但即时效果静默丢失。钉子：Task 2 handler-null 用例（写盘仍成功 + LOG_WARN + 不崩）。
4. **registerModule 重复**（on→on 边界）：modules_ 双条目 → 侧边栏同页双入口。钉子：Task 1 Router 幂等用例。
5. **agent 运行中关模块**：run 线程在跑时 onStop → 必须取消并 join 后才反注册（T12 已钉的时序在活门控下必须原样成立）。钉子：Task 4 agent 重入用例复跑 T12 断言 + 既有 onstop_cancels_and_joins 不回归。

---

### Task 1: Router::unregisterModule + registerModule 幂等化

**Files:**
- Modify: `src/core/router.h`（声明）+ `src/core/router.cpp`（实现）
- Test: `tests/test_core/test_router.cpp` 追加

**Interfaces:**
- Produces: `void Router::unregisterModule(Module* module);`——从 modules_ 移除首个匹配指针；nullptr / 未注册 = 幂等 no-op。`registerModule` 增加幂等：modules_ 内已存在同一指针 = no-op（不再双条目）。
- 不改：`stopAll` 顺序、`navigateTo` 回退行为、`isRegistered`。

- [ ] **Step 1: 写失败测试**（test_router.cpp 既有模式：局部 `RecordingModule` 风格桩——参照文件内既有桩或 test_app_sidebar.cpp:26-48 的 `RecordingModule : ::core::Module`）：

```cpp
REGISTER_TEST(router_unregister_and_idempotent_register,
              "router: unregisterModule 移除注册；register/unregister 双向幂等", {
    ::core::Router router;
    ModuleStub a;  // pageId 各异的三个桩（复用文件内既有桩类型）
    ModuleStub b;
    router.registerModule(&a);
    router.registerModule(&a);            // 重复注册
    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(1));  // 幂等：不双条目
    router.registerModule(&b);
    DOCTEST_CHECK(router.isRegistered(a.pageId()));
    DOCTEST_CHECK(router.isRegistered(b.pageId()));

    router.unregisterModule(&a);
    DOCTEST_CHECK_FALSE(router.isRegistered(a.pageId()));
    DOCTEST_CHECK(router.isRegistered(b.pageId()));
    router.unregisterModule(&a);          // 再卸 = no-op
    router.unregisterModule(nullptr);     // null = no-op
    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(1));
    // 注销当前页后 currentPage 不自动改（重定向是 app 层职责，Task 5），
    // 但 navigateTo 到其他页仍正常
    return true;
})
```

- [ ] **Step 2: 红**（unregisterModule 不存在 → 编译失败即 RED 证据）
- [ ] **Step 3: 实现**：

```cpp
// router.cpp
void Router::registerModule(Module* module) {
    if (module == nullptr) return;
    for (auto* m : modules_) {
        if (m == module) return;   // 活门控（live-toggle）：重复注册 = no-op，
    }                              // 侧边栏不再出现同页双入口
    modules_.push_back(module);
}

void Router::unregisterModule(Module* module) {
    if (module == nullptr) return;
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if (*it == module) {
            modules_.erase(it);
            return;                // 只移除首个匹配；重复调用幂等
        }
    }
}
```

- [ ] **Step 4: 绿** + 全量（router 契约用例 core_pageid_order 等零回归）
- [ ] **Step 5: Commit** `git commit -m "feat(router): unregisterModule + register/unregister 幂等（活门控基座）"`

### Task 2: module_toggle_bridge（中立层桥头 + 可注入实现缝）

**Files:**
- Create: `src/module_toggle_bridge.h`（src 根，module_pet_bridge.h 同级）
- Modify: `src/module_settings/settings_module.cpp`（applyModuleEnabled 尾段）
- Test: `tests/test_modules/test_module_toggle_bridge.cpp` 新建 + `tests/CMakeLists.txt` 登记进 `add_executable(devdesk_tests ...)`

**Interfaces:**
- Produces（settings 侧调用、app 侧 Task 3 实现）:

```cpp
// src/module_toggle_bridge.h —— 设置页 ↔ 应用层的模块开关即时生效桥。
// 模式照 module_pet_bridge.h（中立层桥头 + 链接期解析 + static_assert 哨兵）：
// settings 不 include 任何 app/兄弟模块头；符号在可执行文件链接期解析。
// 实现 = 函数指针缝（默认 no-op + LOG_WARN）：app 装配段注入真实现（Task 3），
// 测试注入 recording 桩。handler 为空时 onModuleToggled 只记日志不崩——
// 启动早期/测试环境拨开关，写盘（settings 侧已 persist）不受影响。
#pragma once
#include <string>

namespace app::toggle {

using ToggleHandler = void (*)(const std::string& moduleId, bool on);
void setToggleHandler(ToggleHandler handler);   // nullptr = 恢复默认 no-op
void onModuleToggled(const std::string& moduleId, bool on);

}  // namespace app::toggle

static_assert(std::is_same_v<decltype(&app::toggle::onModuleToggled),
                             void (*)(const std::string&, bool)>,
              "module_toggle_bridge: onModuleToggled 签名漂移");
static_assert(std::is_same_v<decltype(&app::toggle::setToggleHandler),
                             void (*)(app::toggle::ToggleHandler)>,
              "module_toggle_bridge: setToggleHandler 签名漂移");
```

（实现落 `src/app_assembly.cpp`：函数指针表 + 默认 no-op；`<type_traits>` 哨兵所需 include 由头自带。）

- [ ] **Step 1: 写失败测试**（新文件）：

```cpp
#include "doctest.h"
#include "module_toggle_bridge.h"
#include <string>

namespace {
app::toggle::ToggleHandler g_prev = nullptr;
struct Recording {
    std::string lastId; bool lastOn = false; int calls = 0;
    app::toggle::ToggleHandler self = nullptr;
};
Recording g_rec;
void record(const std::string& id, bool on) {
    g_rec.calls++; g_rec.lastId = id; g_rec.lastOn = on;
}
} // namespace

REGISTER_TEST(toggle_bridge_handler_roundtrip,
              "toggle_bridge: 注入 handler 后 onModuleToggled 路由到它；复位后回默认", {
    g_prev = nullptr;  // 缝无 get accessor——测试直接 set/恢复（见实现注）
    app::toggle::setToggleHandler(&record);
    app::toggle::onModuleToggled("agent", true);
    DOCTEST_CHECK_EQ(g_rec.calls, 1);
    DOCTEST_CHECK_EQ(g_rec.lastId, std::string("agent"));
    DOCTEST_CHECK(g_rec.lastOn);
    app::toggle::setToggleHandler(nullptr);   // 复位默认 no-op：不崩即通过
    app::toggle::onModuleToggled("agent", false);
    DOCTEST_CHECK_EQ(g_rec.calls, 1);         // no-op 不再进 record
    return true;
})
```

（用例间 handler 复位纪律：每用例结束 `setToggleHandler(nullptr)`，防泄漏到后续用例——fail-fast 下放栈外，靠用例末行保证；若断言在复位前失败，后续用例首行 set 新 handler 覆盖，无悬垂风险——handler 不捕获对象，纯函数指针。）

- [ ] **Step 2: 红**（头不存在 → 编译失败）→ **Step 3: 实现**头 + `app_assembly.cpp` 的缝：

```cpp
// app_assembly.cpp 末尾（或独立小节）
namespace app::toggle {
namespace { ToggleHandler g_handler = nullptr; }
void setToggleHandler(ToggleHandler handler) { g_handler = handler; }
void onModuleToggled(const std::string& moduleId, bool on) {
    if (g_handler == nullptr) {
        LOG_WARN("模块开关桥未装配（启动早期/测试环境），仅写盘: " + moduleId);
        return;
    }
    g_handler(moduleId, on);
}
}  // namespace app::toggle
```

- [ ] **Step 4: settings 侧接线**（settings_module.cpp `applyModuleEnabled` :559-578）：pet 分支后、原「重启生效」status 处改为：

```cpp
    #include "module_toggle_bridge.h"   // 文件头 include 区
    // …… pet 分支保持不变 ……
    app::toggle::onModuleToggled(moduleId, on);   // 即时生效（桥内处理反馈）
    setStatus((on ? "模块已开启：" : "模块已关闭：") + moduleId);
    LOG_INFO("Settings: 模块开关 " + moduleId + " → " + (on ? "启用" : "关闭"));
```

（旧文案「重启 DevDesk 后生效」删除——本任务起不再成立；settings_module.cpp:559 的「重启生效」设计注释同步改写为「即时生效经 app::toggle 桥，本函数只写盘 + 发意图」。）

- [ ] **Step 5: 绿** + 全量 + `bash scripts/check-module-isolation.sh`（settings 不 link 兄弟库断言必须仍绿——桥头是 src 根头，非模块头）
- [ ] **Step 6: Commit** `git commit -m "feat(settings): 模块开关接即时生效桥（中立层桥头 + 可注入缝）"`

### Task 3: app 层 dispatchModuleToggle + main.cpp 注入

**Files:**
- Modify: `src/app_assembly.h` / `src/app_assembly.cpp`（dispatchModuleToggle 实现，桥缝旁）
- Modify: `src/main.cpp`（getRouter() 内安装 handler）
- Test: `tests/test_modules/test_app_assembly.cpp` 追加

**Interfaces:**
- Consumes: Task 1 `Router::unregisterModule`、Task 2 缝、`app::registry::kModuleRegistry`（ModuleEntry{page, gatingKey, shortName, icon, factory}，app_registry.h:44）、`module::settings::moduleEnabled`、`app::showAppNotice(title, message)`（app_sidebar.h:49）、`core::Router`（navigateTo/isRegistered/currentPage）
- Produces:

```cpp
// app_assembly.h
namespace app::assembly {
// 模块开关即时生效的执行体（Task 2 桥的实现Target）：
// on  = registerModule → onStart（经 gateLookup 现查使能态，双保险）
// off = onStop → unregisterModule；若 router 当前页 == 该页 → navigateTo(Home)
//       + showAppNotice("模块已关闭", "<shortName> 已关闭")
// moduleId 不在表内 / 表行无 gatingKey（home/settings 恒开）→ LOG_WARN + no-op
// lookup(page) 返回该页模块对象指针（main.cpp 的 moduleForPage 语义）
using ModuleLookup = ::core::Module* (*)(::core::PageId page);
void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup);
}
```

- [ ] **Step 1: 写失败测试**（test_app_assembly.cpp 追加；桩模块照文件内既有 stub 提供者形态，可开关的页用 todo/agent 对应 PageId）：

```cpp
REGISTER_TEST(assembly_dispatch_module_toggle_lifecycle,
              "assembly: dispatchModuleToggle on/off 驱动注册与生命周期", {
    ::core::Router router;
    // stub 模块：记录 onStart/onStop 次数（照本文件既有 stub 提供者）
    app::assembly::dispatchModuleToggle(
        module::settings::kModuleIdAgent, true, &router, lookupStub);
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Agent));
    DOCTEST_CHECK_EQ(stubStarts, 1);
    DOCTEST_CHECK_EQ(stubStops, 0);
    app::assembly::dispatchModuleToggle(
        module::settings::kModuleIdAgent, false, &router, lookupStub);
    DOCTEST_CHECK_FALSE(router.isRegistered(::core::PageId::Agent));
    DOCTEST_CHECK_EQ(stubStops, 1);
    // off 当前页 → 回 Home
    router.navigateTo(::core::PageId::Agent);   // 已注销 → 回退 Home（既有语义）
    // 再开再关时先把 Agent 设为当前页（register 后 navigate）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true, &router, lookupStub);
    router.navigateTo(::core::PageId::Agent);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Agent);
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false, &router, lookupStub);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);   // D-1 重定向
    // 恒开模块（gatingKey nullptr）→ no-op
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdHome /*或任意恒开 id*/, true, &router, lookupStub);
    DOCTEST_CHECK_EQ(stubStarts, 2);   // 未再增
    return true;
})
```

（`lookupStub` = 测试提供的 `ModuleLookup`，把 PageId::Agent 映到桩对象、其余 nullptr；toast 断言——showAppNotice 是 UI 副作用，测试观察 router.currentPage() 即可，notice 文案进 Review Focus 人工核对项。）

- [ ] **Step 2: 红** → **Step 3: 实现**（app_assembly.cpp）：

```cpp
void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup) {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.gatingKey == nullptr || moduleId != e.gatingKey) continue;
        ::core::Module* mod = lookup(e.page);
        if (mod == nullptr) {
            LOG_WARN("模块开关: " + moduleId + " 无实例，跳过即时生效");
            return;
        }
        if (on) {
            router.registerModule(mod);
            mod->onStart();
        } else {
            mod->onStop();
            router.unregisterModule(mod);
            if (router.currentPage() == e.page &&
                ::core::Router().isRegistered(::core::PageId::Home)) {  // 见注
                router.navigateTo(::core::PageId::Home);
                app::showAppNotice("模块已关闭",
                                   std::string(e.shortName) + " 已关闭，已返回首页");
            }
        }
        return;
    }
    LOG_WARN("模块开关: 未知或恒开模块 " + moduleId + "，忽略");
}
```

（注：`isRegistered(Home)` 是 router 实例方法——直接 `router.isRegistered(::core::PageId::Home)`，上面的 `::core::Router()` 示意写法是错的，实现时用实例调用。Home 恒注册，防御分支照 navigateTo 既有风格。）

- [ ] **Step 4: main.cpp 注入**（getRouter() 的 initialized 块末尾，moduleForPage 可见域内）：

```cpp
        app::toggle::setToggleHandler(
            [&router, &moduleForPage](const std::string& moduleId, bool on) {
                app::assembly::dispatchModuleToggle(moduleId, on, router,
                    [](::core::PageId p) -> ::core::Module* {
                        // moduleForPage 是 getRouter() 内 lambda——不能裸取地址；
                        // 这里经 main.cpp 文件级静态表转发（Task 实现时把
                        // moduleForPage 提为 getRouter() 文件内静态辅助函数，
                        // 持有集 unique_ptr 已是 static，作用域足够）
                        return moduleForPageImpl(p);
                    });
            });
```

（实现注：把 getRouter() 内的 `moduleForPage` lambda 提为同文件静态函数 `moduleForPageImpl(PageId)`——持有集 unique_ptr 已是函数内 static，提函数后可见。include `module_toggle_bridge.h`。）

- [ ] **Step 5: 绿** + 全量（启动期契约用例零回归）+ 隔离守卫
- [ ] **Step 6: Commit** `git commit -m "feat(assembly): dispatchModuleToggle（on/off 驱动注册与生命周期 + 当前页重定向）"`

### Task 4: 四模块 onStart/onStop 重入幂等（本计划核心风险面）

**Files:**
- Modify: `src/module_markdown/markdown_module.cpp`、`src/module_todo/todo_module.cpp`、`src/module_claude_proxy/claude_module.cpp`、`src/module_agent/agent_module.cpp`
- Test: 各模块既有测试文件追加重入循环用例（test_markdown_tools.cpp / test_todo_tools.cpp / test_claude_service.cpp 或 claude 模块测试所在文件 / test_agent_tools.cpp）

**Interfaces:**
- 统一模式（四模块一致）：每模块加 `bool started_ = false;`——`onStart()` 首行 `if (started_) return;`（double-onStart = no-op，防 service 替换悬垂 + 同名不同指针重复注册），置 `started_ = true`；`onStop()` 首行 `if (!started_) return;`（未启动的 off = no-op），末尾 `started_ = false;`。
- 已知语义锚：ToolRegistry 同名同指针 = no-op（tool.cpp:18）；markdown onStop 已 reset markdownTools_（off→on 循环安全）；todo detachTools 已 reset todoTools_（同）；agent onStop 已 unregister + 成员析构序安全（T12）。

- [ ] **Step 1: 写失败测试**（每模块一条，形态统一——以 agent 为例，其余三个照抄结构换模块与工具名）：

```cpp
REGISTER_TEST(agent_module_onstart_reentry_idempotent,
              "agent_module: onStart→onStop→onStart 重入 + 双 onStart no-op", {
    test_support::TestDb tdb("agenttools", "reentry");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentModule mod({&tdb.handle(), &proxy, nullptr});

    mod.onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    mod.onStart();   // 双 onStart：no-op（不重复注册、不重建成员）
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    mod.onStop();    // 双 onStop：no-op
    mod.onStart();   // off→on 循环：工具恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    return true;
})
```

（markdown 用例断言 `ToolRegistry::instance().find("md.search")` 同型循环；todo 断言 `todo.list`；claude 模块若 onStart 无外部可见副作用，则断言重入不崩 + LOG 无重复接线（可用既有 claude 模块测试的观察面——以实际为准，最低要求：双 onStart/双 onStop 循环不崩、服务状态一致）。）

- [ ] **Step 2: 红**（现状：markdown/todo/agent 的双 onStart 会重建 service/成员——markdown 双 onStart 时新 MarkdownTools registerAll 撞同名不同指针 fail（被 LOG 吞）→ 旧 tools 引用旧 service 悬垂；agent 同理 statsTool_ 重建）→ **Step 3: 实现**（四模块 started_ 守卫，形态照 Interfaces；agent 的 onStart 既有 sweepStaleRuns 保持在后）
- [ ] **Step 4: 绿** + 既有 T12 生命周期用例（agent_module_registers_own_tool / onstop_cancels_and_joins）零回归 + 全量
- [ ] **Step 5: Commit** `git commit -m "fix(modules): 四页面模块 onStart/onStop 重入幂等（double-onStart no-op 守卫）"`

### Task 5: 侧边栏实时反映（无头）+ 真 probe

**Files:**
- Test: `tests/test_modules/test_app_sidebar.cpp` 追加
- Test: `tests/probes/settings_cards_probes.cpp` 追加（或新建 `tests/probes/module_toggle_probe.cpp`——以既有 probe 结构最小改动者为准）

**Interfaces:**
- Consumes: Task 1 Router、Task 3 dispatchModuleToggle、`app::compose`（真 app 层 compose + 桩 Router，test_app_sidebar.cpp:66-72 的 `makeStubRouter` 模式）

- [ ] **Step 1: 无头失败测试**（test_app_sidebar.cpp 追加）：

```cpp
REGISTER_TEST(sidebar_live_toggle_reflects_registration,
              "sidebar: 注册/反注册后 compose 当帧反映（活门控）", {
    // makeStubRouter 模式（:66-72）：注册 home + 可开关桩模块
    // 1) compose → find("app.sidebar.item.Agent")（pageIdToString 命名）
    //    存在且 y 随条目数排布；
    // 2) router.unregisterModule(&agentStub) → re-compose → 条目消失；
    // 3) registerModule 再 compose → 条目回来；
    // 4) 断言其余条目（Home）位置稳定（不受反注册挤压错位——条目 y 由
    //    compose 循环变量决定，天然重排，断言 Home 在、Agent 不在即可）
    return true;
})
```

（断言键：`"app.sidebar.item." + pageIdToString(page)`——app_sidebar.cpp:187 的既有 id 拼法。）

- [ ] **Step 2: 红**（现状 unregister 后 compose 已天然反映——本用例可能首跑即绿；若绿，变异验证：临时注释 unregister 的 erase 使其 no-op → 用例必须红 → 还原。记录进报告）→ **Step 3: probe**——真窗口：

```
tests/probes/ 内新增或扩展：probe_harness.h 真窗口（glfwInit）+
composeModuleFrame 驱动【真 app::compose + 桩 Router + 桩可开关模块】
（app::compose 的链接桩照 about_probe.cpp:49-56 的 app::stub 模式——
若 include <eui/detail/dsl_app_impl.h> 才需要）；
1) composeModuleFrame → find("app.sidebar.item.Agent") 存在；
2) core::queuePointerButton 点击设置卡的 agent 开关（或直接调
   dispatchModuleToggle——probe 钉「compose 当帧反映」这一层即可，
   开关点击路径已由无头层钉）→ composeModuleFrame → 条目消失；
3) 反向再开 → 条目回来；
4) 收尾照 about_probe.cpp:449-453。
```

（probe 点击若受窗口坐标换算困扰，允许降级为「直接调 dispatchModuleToggle + composeModuleFrame 断言」——probe 的价值在真 Runtime 帧节拍与真实 DOM，不在重复无头已钉的点击语义。）

- [ ] **Step 4: 跑**：`(cd build && ctest -R settings_cards_probes --output-on-failure)`（或新 probe 名）+ `ctest -L probe` 全套绿
- [ ] **Step 5: Commit** `git commit -m "test(ui): 侧边栏活门控实时反映（无头 + probe 双层）"`

### Task 6: 收尾验证 + 文档口径

- [ ] **Step 1: 全量构建 + 测试**：`bash scripts/build-test-run.sh --no-run` → Failed: 0（基线 948 + 本计划新增用例）+ `(cd build && ctest -L probe)` 9/9+
- [ ] **Step 2: 隔离守卫** `bash scripts/check-module-isolation.sh`（settings 桥 include 不违规）
- [ ] **Step 3: 启动契约复核**：test_app_assembly 的 buildGatedEntries 系列 + test_module_gating 全绿零改动（或仅当语义确需扩展时逐条列出）
- [ ] **Step 4: 文档口径**：settings_module.cpp:559 设计注释已随 Task 2 改写；`docs/KNOWN_LIMITATIONS.md` 若登记过「模块开关需重启」条目则更新（grep 核实）
- [ ] **Step 5: 手动冒烟**（隔离 DEVDESK_DATA_ROOT 启动）：设置页拨 agent 开 → 侧边栏立即出现「智能体」→ 进页 → 拨关 → 自动回首页 + toast → 侧边栏条目消失
- [ ] **Step 6: 汇报 Total/Passed 对比基线**

## Self-Review 记录

- **Spec 覆盖**：§1 路线 A（T1/T3）、§2 数据流（T2 桥 + T3 执行体 + main 注入）、§3 Router（T1）、§4 重入审计（T4 四模块 + 统一验收用例）、§5 D-1 重定向+toast（T3）、D-2 agent onStop 复用（T4）、D-3 托盘（pageForTrayAction 无 Agent 动作，navigateFromTray 既有「模块已关闭」notice 走 router 注册态天然一致——T3 实现期核实并在报告注明）、D-4 连点短路（applyModuleEnabled 既有 + T1/T4 幂等双保险）、D-5 pet 不动（T2 仅改其余分支）、§7.1（T1）、§7.2（T4）、§7.3（T2）、§7.4（T5 无头）、§7.5（T5 probe）、§7.6（T6 Step 3）、§7.7（T2 handler-null）。
- **占位符扫描**：T4 claude 模块观察面「以实际为准」与 T5 probe 降级条款是显式授权的实现期核实点（锚点不足处已注明依据文件:行），非 TBD；其余全部有代码。
- **类型一致性**：`dispatchModuleToggle(moduleId, on, router, lookup)` 签名 T2/T3 一致；`ModuleLookup` 函数指针形态与 main.cpp lambda 转发（需提静态函数）已在 T3 注明；桥函数指针缝与 static_assert 哨兵照 pet 桥逐字模式。
- **Review Focus 落钉**：5/5（T1#4、T2#3、T3#2、T4#1+#5、T5 无）。
