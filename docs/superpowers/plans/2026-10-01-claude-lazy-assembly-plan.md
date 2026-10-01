# Claude 惰性装配 + auto-start 会话内恢复 + 侧边栏表序插入 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 活门控（live module toggle）的三个收尾项：A) claude 模块会话内开启时**惰性装配** ClaudeService + 按 `claudeProxyAutoStart` 恢复代理自启（不再「需重启」）；B) `Router::registerModule` 重注册按 **PageId 表序插入**（不再 append 到末尾）；C) 全部改动 TDD 先红后绿 + 既有契约零回归。

**Architecture:** 三项共用一条缝——main.cpp 把 `prepareClaudeServiceBeforeModules` 的装配段拆成幂等的 `assembleClaudeServiceNow()`，经 `module_toggle_bridge.h` 新增第二个桥函数 `app::toggle::ensureClaudeServiceAssembled(bool autoStart)`（**函数指针缝 + setter 注入**，照 `setToggleHandler` 既有模式）供 `dispatchModuleToggle` 的 on 路径调用；Router 侧 registerModule 从 push_back 改为按 `Module::pageId()` 升序插入（core 层零 app 依赖）。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（REGISTER_TEST + DOCTEST_CHECK/REQUIRE，fail-fast）。

**Spec:** 无独立 spec 文件——控制器设计（2026-10-01 会话内转写）为本计划权威口径；背景规格见 `docs/superpowers/specs/2026-10-01-live-module-toggle-design.md`（活门控主线，已交付）。**所有锚点已于 2026-10-01 在 dev 分支（HEAD `8dc93d5`）逐一核实**，实际 file:line 见各 Task；与控制器原话有出入的锚点已在「Self-Review 记录」末尾的「锚点核实差异」段列出。

## Global Constraints

- **不抛异常**：错误一律 `core::Result<T>`；module_settings 不 include 兄弟模块头（本计划不触碰 settings 模块源码，隔离守卫必须保持全绿）。
- **启动期契约零变化**：`prepareClaudeServiceBeforeModules` 重构后外部可观察行为逐字不变——gate 不过 = 不创建 service / 不 bind 端口（gate LOG_INFO 文案按下方 T1 修订更新——「需重启」话术随惰性装配失效）；`claudeProxyAutoStart=false` = 不 startProxy（LOG_INFO 原文保留）。preStart 时序（assemble 早于模块 onStart，W3 事故点）不得回退。
- **桥实现必须走缝，不许直接定义在 main.cpp**：app_assembly.cpp 被三处链接（DevDesk exe / devdesk_tests，tests/CMakeLists.txt:4；module_toggle_sidebar_probe，tests/CMakeLists.txt:190），**三处都不链 main.cpp**——dispatchModuleToggle 若直呼 main.cpp 符号 = 测试与 probe 链接即炸。新桥函数照 `setToggleHandler` 模式：声明在 `module_toggle_bridge.h`、缝落 `module_toggle_bridge.cpp`（默认 no-op + LOG_WARN）、真实现由 main.cpp 文件级函数经 setter 注入。
- **Router 既有语义**：幂等 no-op 分支（router.cpp:14-16）保留；`stopAll` 按注册序的 UAF 前提（555a728）在新插入序下仍成立——表序 Todo(2) < Agent(5)，todo/markdown 先于 agent 的既有保证不弱化；`navigateTo` 回退、`isRegistered`、`unregisterModule` 不动。
- **core 层零 app 依赖**：registerModule 的插入比较只准用 `Module::pageId()`（module_interface.h header-only 透传），不得引入 registry / settings 头。
- **PageId 枚举序 == kModuleRegistry 表序**是双契约钉死的既有事实（`core_pageid_order`，tests/test_core/test_router.cpp:451；`assembly_registry_table_shape`，tests/test_modules/test_app_assembly.cpp:493），表序插入直接消费它。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化；若某用例首跑即绿，必须做变异验证（临时回退实现 → 红 → 还原）并在报告记录。
- **测试进程全局态纪律**：`ClaudeService::setGlobal` / `ServiceLocator` / toggle 桥 handler 都是进程级——每个用例收尾恢复原值（handler 置 nullptr、setGlobal 还原），防泄漏到后续用例。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`；全量基线**以最近一次全量运行输出为准**（用例总数不在文档写死），验收口径 `Failed: 0` 且 Total ≥ 前基线 + 本计划新增用例数；probe 全绿（数量以 `ctest -L probe` 输出为准）。
- **执行隔离**：涉及编译/测试的实现任务（T1/T2）在 worktree 分支执行，原子提交、显式 pathspec、禁 `git add -A`；主工作树禁跑构建。T3 收尾由主会话在合并后执行。

## Review Focus（五类最易咬人的输入）

1. **直接定义在 main.cpp 的桥符号**：devdesk_tests / sidebar probe 不链 main.cpp，直呼即 undefined symbol。钉子：T1 Step 2 的红必须来自断言而非链接错误；改完全量 + probe 必须能链。
2. **装配时序**：`ensureClaudeServiceAssembled` 必须严格先于 claude 模块 `mod->onStart()`（claude_module.cpp compose 期读 `ClaudeService::global()`，:119 起的 `!svc` 分支是唯一防线）。钉子：T1 用例记录 seam 调用与 onStart 的先后序号断言。
3. **重复 on 的幂等**：off→on→on 连拨——getClaudeService 的 static 初始化守卫（main.cpp:235-253）保证同指针；startClaudeProxy 的 isProxyRunning 早退（main.cpp:313-316）保证不重绑端口。钉子：T1 幂等用例断言同指针 + 运行态不变。
4. **表序插入破坏既有 y 断言**：`sidebar_live_toggle_reflects_registration`（test_app_sidebar.cpp:361，注册序 home→md→agent 恰为表序）与 probe 的 Agent y=Settings+44 断言（module_toggle_sidebar_probe.cpp:281-290，注册集 {home, settings, agent} 表序不变）在插入序下**应保持绿**——若红，说明插入实现有误而非测试该改。钉子：T2 Step 4 逐条复跑。
5. **autoStart=false 误起代理**：on 路径读了 settings 的 `claudeProxyAutoStart` 传给桥，桥内为真才 startProxy——false 时只装配不起代理。钉子：T1 第二条用例断言 isProxyRunning()==false 且装配已完成（global 非空）。

---

### Task 1: claude 惰性装配 + auto-start 会话内恢复（A 项：拆函数 + 第二桥函数 + dispatch 接线 + 测试）

**Files:**
- Modify: `src/main.cpp`（拆 `assembleClaudeServiceNow` + 注入桥 handler）
- Modify: `src/module_toggle_bridge.h` + `src/module_toggle_bridge.cpp`（第二桥函数声明 + 缝）
- Modify: `src/app_assembly.cpp`（dispatchModuleToggle on 路径接线）
- Modify: `src/module_claude_proxy/claude_module.cpp`（死页 toast 分支注释口径修正，零行为变化）
- Test: `tests/test_modules/test_module_toggle_bridge.cpp` 追加 + `tests/test_modules/test_app_assembly.cpp` 追加

**已核实锚点（实现前无需再猜）：**

| 锚点 | 实际位置 | 语义 |
|---|---|---|
| `prepareClaudeServiceBeforeModules` | main.cpp:333-368（前向声明 :129-132，preStart 接线 :176-178） | gate(:337-343) → getClaudeService(:348) → setConfiguredProxyPort(:352) → recoverPendingSettingsRestore(:357-359) → autoStart 分支(:362-367) |
| `getClaudeService` | **main.cpp:234-256（main 文件级，非 claude_service.cpp）** | static unique_ptr + static bool initialized（:235-236）——首次：openAppDatabase → 构造 service → setSettingsSyncPaths → setGlobal → ServiceLocator::set（:239-253）；重复调用返回同指针、零副作用。**幂等语义成立，assembleClaudeServiceNow 重复调用安全** |
| `startClaudeProxy` | main.cpp:307-326（main 文件级，无头文件声明——grep 全仓仅 main.cpp 引用） | isProxyRunning 早退（:313-316）→ startProxy("127.0.0.1", configuredProxyPort())（:320）。**自身幂等** |
| `dispatchModuleToggle` on 路径 | app_assembly.cpp:255-286（on 分支 :266-268：registerModule → onStart） | 接线点 = on 分支内、`router.registerModule(mod)` 之前 |
| `ClaudeService` 可观察面 | claude_service.h: isProxyRunning(:247) / startProxy(:227) / stopProxy(:234) / setConfiguredProxyPort(:276) / configuredProxyPort(:275) / recoverPendingSettingsRestore(:318)；global()/setGlobal 落 claude_service.cpp:126-127 | 测试断言全部走这些面 |
| 死页 toast 分支 | claude_module.cpp:119-131（`if (!svc)` 起 :119，toast :127-131） | `serviceNotReadyWarned_` 单次 toast「……需重启后才能提供代理服务」；:123-126 注释自注「真修 = 惰性创建服务，越界记 backlog」——本任务即该真修 |
| 桥现状 | module_toggle_bridge.h（src 根，:14-20 seam 声明 + :22-27 static_assert）；缝实现 module_toggle_bridge.cpp（独立 TU：probe 只链 module_* 库，app_assembly.cpp 不在其链接面） | 照此模式加第二个桥函数 |

**Interfaces:**

```cpp
// src/module_toggle_bridge.h 追加（namespace app::toggle 内）
using ClaudeAssemblyHandler = void (*)(bool autoStart);
void setClaudeAssemblyHandler(ClaudeAssemblyHandler handler);  // nullptr = 恢复默认 no-op
void ensureClaudeServiceAssembled(bool autoStart);
// 文件末尾追加两条 static_assert（照 :22-27 既有哨兵）：
//   ensureClaudeServiceAssembled ↔ void (*)(bool)
//   setClaudeAssemblyHandler ↔ void (*)(app::toggle::ClaudeAssemblyHandler)
```

```cpp
// src/main.cpp：prepareClaudeServiceBeforeModules 之后新增文件级函数（前向声明
// 照 :129-132 模式提到 prepare 之前，因 prepare 内部要调它）
void assembleClaudeServiceNow() {
    // 逐字搬运 prepare 的 :344-359 段：getClaudeService → setConfiguredProxyPort
    // （端口自读：函数内 SettingsStore(::core::paths::configFile()).load()
    //   取 proxyPort，不依赖调用方传入）→ recoverPendingSettingsRestore。
    // 幂等依据：getClaudeService 的 static 守卫（main.cpp:235-253）——
    // 重复调用 = 返回同指针 + 重复 setConfiguredProxyPort/recover 均 no-op 安全
    //（recoverPendingSettingsRestore 无残留 sidecar 时即空操作，main.cpp:354-356
    //  既有注释口径）
}
// prepareClaudeServiceBeforeModules 的 :344-359 段替换为一句 assembleClaudeServiceNow();
// gate 分支与 autoStart 分支（:337-343 / :360-367）逐字不动
```

```cpp
// src/main.cpp：桥真实现（文件级静态函数，无捕获可转函数指针）+ 注入
void ensureClaudeServiceAssembledImpl(bool autoStart) {
    assembleClaudeServiceNow();
    if (autoStart) {
        startClaudeProxy();   // 自身幂等：isProxyRunning 早退（main.cpp:313-316）
    }
}
// getRouter() initialized 块内、setToggleHandler 注入（:192-196）旁边：
app::toggle::setClaudeAssemblyHandler(&ensureClaudeServiceAssembledImpl);
// 注册顺序在装配之后 = 运行期拨开关才会触达，启动期行为零变化（同 :190 既有口径）
```

```cpp
// src/app_assembly.cpp dispatchModuleToggle on 分支（:266-268）改为：
if (on) {
    if (moduleId == module::settings::kModuleIdClaudeProxy) {
        // 惰性装配（本计划 A）：启动期 claude 关 → 会话内开启时在此补装配。
        // 必须先于 registerModule/onStart——claude 模块 compose 期读 global()。
        // autoStart 从持久配置现读（与会话内改设置后拨开关的语义一致）
        const module::settings::SettingsStore store(::core::paths::configFile());
        app::toggle::ensureClaudeServiceAssembled(store.load().claudeProxyAutoStart);
    }
    router.registerModule(mod);
    mod->onStart();
}
// off 路径零改动（service/代理进程级存活，off 不拆服务）
// include 增量：module_toggle_bridge.h + module_settings/settings_config.h +
// core/paths.h——链接面已核：app_assembly.cpp 的三个消费方（exe / devdesk_tests
// / module_toggle_sidebar_probe）全部链接 devdesk_module_settings
//（tests/CMakeLists.txt:159-168 与 :203-212）
```

- **claude_module.cpp 注释口径修正（零行为）**：:123-126 的「真修 = 惰性创建服务，越界记 backlog」改为「真修 = 惰性装配（2026-10-01 计划 T1）已落地：dispatch on 路径先装配服务，本分支理论不可达，保留为防御（零成本）」；toast 文案与 `serviceNotReadyWarned_` 逻辑**逐字不动**（防御分支保留）。

- **main.cpp gate 日志文案修正（零行为）**：:339-342 的「重新启用需改设置并重启」改为「重新启用可在设置页开启，会话内即时生效（惰性装配）」——纯文案，gate 行为（不创建 service / 不 bind）逐字不变。

- [ ] **Step 1: 写失败测试**（两条腿）：

  1a. `tests/test_modules/test_module_toggle_bridge.cpp` 追加（照文件内 `toggle_bridge_handler_roundtrip` 既有形态 + 收尾复位纪律）：

  ```cpp
  REGISTER_TEST(toggle_bridge_claude_assembly_seam,
                "toggle_bridge: ensureClaudeServiceAssembled 缝注入/路由/复位；默认 no-op 不崩", {
      // 注入 recording handler（void(*)(bool)）→ ensureClaudeServiceAssembled(true)
      // → 记录 calls==1、lastAutoStart==true；setClaudeAssemblyHandler(nullptr)
      // → 再调 → calls 不再增（默认 no-op + LOG_WARN，不崩即通过）
      return true;
  })
  ```

  1b. `tests/test_modules/test_app_assembly.cpp` 追加两条（照 `assembly_dispatch_module_toggle_lifecycle` :737 的桩模块/lookupStub 形态；配置隔离照 test_module_gating.cpp:90-111 的 `SettingsStore::save` + test_support.h:313 `ScopedConfigPath` 模式）：

  ```cpp
  // 用例一：assembly_dispatch_claude_lazy_assemble_autostart
  //   前置：ScopedConfigPath + SettingsStore::save 写入「claude 门控关 +
  //   claudeProxyAutoStart=true」的配置；seam 注入**真服务 handler**——
  //   照 main.cpp ensureClaudeServiceAssembledImpl 的等价逻辑驱动真
  //   ClaudeService（devdesk_tests 链 devdesk_module_claude_proxy，可真构造）：
  //   首次 new ClaudeService（TestDb 库）→ setGlobal → setConfiguredProxyPort
  //   → 若 autoStart 且 !isProxyRunning 则 startProxy（端口选择照
  //   test_claude_service.cpp 既有起代理用例，避开 8888/28888）；
  //   handler 记录：每次调用的 svc 指针、autoStart 入参、调用序号。
  //   断言：
  //   1) dispatchModuleToggle(kModuleIdClaudeProxy, true, …) →
  //      ClaudeService::global() 非空 && isProxyRunning()==true；
  //   2) 时序：seam 调用序号 < 桩模块 onStart 序号（Review Focus #2）；
  //   3) 再拨一次 on → 幂等：handler 记录的 svc 指针与首次相同
  //      （getClaudeService static 守卫语义）、isProxyRunning() 仍 true、
  //      startProxy 实际执行次数 == 1（无二次绑定副作用）；
  //   4) off → 不触 seam（off 路径零改动）；
  //   5) 非 claude id（如 agent）on → 不触 seam。
  //   收尾：stopProxy + setGlobal(原值/nullptr) + setClaudeAssemblyHandler(nullptr)
  //   + clearAppNotice（进程全局态全还原）。

  // 用例二：assembly_dispatch_claude_lazy_assemble_no_autostart
  //   同上但配置 claudeProxyAutoStart=false →
  //   global() 非空（已装配）&& isProxyRunning()==false（代理未起）。
  //   收尾同用例一。
  ```

  （注：main.cpp 的 static unique_ptr 持有段不在 devdesk_tests 链接面（无 main.cpp），「真 getClaudeService 被触发」只能在 T3 的 app 级冒烟覆盖；headless 层钉的是 dispatch→seam 契约 + 真 service 的可观察幂等语义——控制器验收项 1 的「以 service 可观察面为准」按此落。）

- [ ] **Step 2: 红（两段红口径，消解与 Review Focus #1 的措辞冲突）**：先只落 module_toggle_bridge.h/.cpp 的缝声明（默认 no-op）→ 编译通过、1a/1b 断言红（seam 未接线 = dispatch 不触发 handler / 服务未装配）；接线后补验幂等断言（用例一断言 3）。链接错误不是 RED 证据——Review Focus #1 以断言红为准。
- [ ] **Step 3: 实现**——module_toggle_bridge.h/.cpp 缝 → main.cpp 拆函数 + 注入 → app_assembly.cpp 接线 → claude_module.cpp 注释修正 → main.cpp gate 日志文案（本 bullet）（按 Interfaces 各代码块）
- [ ] **Step 4: 绿** + 定向（`./build/tests/devdesk_tests toggle_bridge` / `devdesk_tests dispatch` / `devdesk_tests claude`）+ 全量（启动契约用例零回归）+ `bash scripts/check-module-isolation.sh`
- [ ] **Step 5: Commit** `feat(claude): 惰性装配 + auto-start 会话内恢复（第二桥函数 + dispatch 接线）`

### Task 2: Router 重注册按表序插入（B 项）

**Files:**
- Modify: `src/core/router.cpp`（registerModule :12-18）
- Test: `tests/test_core/test_router.cpp` 追加 + `tests/test_modules/test_app_sidebar.cpp` 追加

**已核实锚点：**

| 锚点 | 实际位置 | 语义 |
|---|---|---|
| `Router::registerModule` | router.cpp:12-18（nullptr 守卫 :13；同指针幂等 no-op :14-16；push_back :17） | 只改插入策略，幂等分支逐字保留 |
| PageId 枚举序契约 | tests/test_core/test_router.cpp:451 `core_pageid_order`（Home=0<Markdown=1<Todo=2<ClaudeProxy=3<Settings=4<Agent=5<About=6，值也钉死） | 插入比较的序来源 |
| 表序镜像契约 | tests/test_modules/test_app_assembly.cpp:493 `assembly_registry_table_shape`（kModuleRegistry 行序==枚举序） | 「表序」与「枚举序」同一件事 |
| 既有 sidebar 用例 | tests/test_modules/test_app_sidebar.cpp:361 `sidebar_live_toggle_reflects_registration`（注册序 home→md→agent；Agent y=188 三段断言） | 注册序恰为表序 → 插入序下 y 断言**应原样保持绿**（Review Focus #4） |
| probe y 锚 | tests/probes/module_toggle_sidebar_probe.cpp:277-290（注册序 home, settings, agent；Agent y = Settings y + 44） | 注册集的表序相对位不变 → **应原样保持绿** |
| 「Agent 回末位」用例 | **不存在**——控制器所指 test_app_sidebar.cpp 无此用例（文件止于 :445 前后）；实况相关物 = :411 注释「顺序 = 注册表序，仍在末位」 | 本任务**新建**该语义用例（见 Step 1） |

**Interfaces:**
- Produces: `registerModule` 插入策略 = 在 modules_ 中找到**第一个 `pageId()` 更大**的元素之前插入；全表更小/等值则 push_back（稳定序）。比较只用 `m->pageId()`，core 层零 app 依赖。效果：off→on 重注册回到表序原位，不再被 append 到末尾。
- 不改：幂等分支、unregisterModule、navigateTo、isRegistered、stopAll。

- [ ] **Step 1: 写失败测试**（两条）：

  1a. `tests/test_core/test_router.cpp` 追加（照文件内既有桩模块形态）：

  ```cpp
  REGISTER_TEST(router_register_inserts_table_order,
                "router: 乱序注册仍得表序（Agent 先注册、Markdown 后注册 → modules() 序为表序）", {
      // 先 registerModule(agent桩) 再 registerModule(markdown桩)（可再补 home 桩）→
      // 断言 modules() 的 pageId 序严格按 PageId 枚举升序（Markdown 在 Agent 前，
      // 尽管 Agent 先注册）；重复注册仍幂等（modules().size() 不变，既有语义）
      return true;
  })
  ```

  1b. `tests/test_modules/test_app_sidebar.cpp` 追加「Agent 回末位」（照 `sidebar_live_toggle_reflects_registration` :361 的 RecordingModule + ScopedAppRouter + compose 形态）：

  ```cpp
  REGISTER_TEST(sidebar_re_register_lands_table_order,
                "sidebar: Agent 先注册、Home/Markdown 后注册 → compose 序仍表序（Agent y=188 回末位）", {
      // 1) 只注册 Agent → compose → Agent y=100（唯一条目）；
      // 2) 再注册 Home、Markdown → re-compose → Agent y==188（表序末位，
      //    Home=100 / Markdown=144 插到它前面）——push_back 旧序下 Agent 恒 100，
      //    此断言先红后绿（Review Focus / TDD 红源）；
      // 3) unregister(Agent) → 再 register(Agent) → y 仍 188（off→on 回原位）
      return true;
  })
  ```

- [ ] **Step 2: 红**（两条用例在旧 push_back 下断言失败——1a Markdown 排 Agent 后、1b Agent y 恒 100）
- [ ] **Step 3: 实现**（router.cpp:12-18）：

  ```cpp
  void Router::registerModule(Module* module) {
      if (module == nullptr) return;
      for (auto* m : modules_) {
          if (m == module) return;   // 活门控（live-toggle）：重复注册 = no-op，
      }                              // 侧边栏不再出现同页双入口
      // 表序插入（2026-10-01 计划 T2）：PageId 枚举序 == kModuleRegistry 表序
      //（core_pageid_order / assembly_registry_table_shape 双契约钉死）——
      // off→on 重注册回表序原位，不再 append 末尾
      for (auto it = modules_.begin(); it != modules_.end(); ++it) {
          if ((*it)->pageId() > module->pageId()) {
              modules_.insert(it, module);
              return;
          }
      }
      modules_.push_back(module);
  }
  ```

- [ ] **Step 4: 绿** + 既有回归逐条复跑（Review Focus #4）：`core_pageid_order`、`sidebar_live_toggle_reflects_registration`、`assembly_registry_table_shape`、`assembly_build_gated_entries_real_table`、`assembly_dispatch_module_toggle_lifecycle`、`ctest -L probe`（module_toggle_sidebar_probe 的 Agent y 锚）——**任何一条红 = 插入实现有误，回 Step 3，不得改测试迁就**
- [ ] **Step 5: Commit** `feat(router): registerModule 按 PageId 表序插入（off→on 重注册回原位）`

### Task 3: 收尾验证 + 文档口径（主会话执行）

- [ ] **Step 1: 全量构建 + 测试**：`bash scripts/build-test-run.sh --no-run` → `Failed: 0`，Total ≥ 前基线 + 本计划新增 4 条用例；报告贴 `Total / Passed / Failed` 行
- [ ] **Step 2: probe**：`(cd build && ctest -L probe --output-on-failure)` 全绿（含 module_toggle_sidebar_probe）
- [ ] **Step 3: 隔离守卫**：`bash scripts/check-module-isolation.sh` 退出码 0（settings 模块本计划零改动，必须保持）
- [ ] **Step 4: 启动契约复核**：test_app_assembly 的 buildGatedEntries 系列 + test_module_gating 全绿零改动
- [ ] **Step 5: app 级冒烟（真 getClaudeService 链路，headless 测不到的那段）**：隔离 `DEVDESK_DATA_ROOT` 启动（禁碰用户真实数据根与 `/Applications/DevDesk.app`，禁 `pkill -f DevDesk`）——配置写入「claude 关 + claudeProxyAutoStart=true」→ 启动确认 8888 未监听、`Claude 模块已关闭` LOG → 设置页拨 claude 开 → 确认代理起（LOG「Claude 代理已在运行/startProxy 成功」）+ Claude 页可用（不再是死页 toast）→ off→on→on 连拨无重复绑定报错 →⑤ 拨开后退出：无崩溃 + onStop/stopProxy 还原日志正常 + settings.json 已还原（sidecar 清）（终审 高: atexit LIFO 退出时序；根修 = getClaudeService 永不析构堆持有）
- [ ] **Step 6: CHANGELOG**：根目录 `CHANGELOG.md` 追加条目（惰性装配 + auto-start 会话内恢复 + 侧边栏表序插入）；`docs/KNOWN_LIMITATIONS.md` 经审计确认不存在（2026-10-01 实查），无登记项需同步——仅更新 CHANGELOG.md
- [ ] **Step 7: 汇报** Total/Passed 对比基线 + 冒烟截图路径；合并与安装到 `/Applications/DevDesk.app` 由主会话执行（四律阶段 5，不在本任务内）

## Self-Review 记录

- **设计覆盖**：A 项（T1：拆 assembleClaudeServiceNow + 双 static_assert 桥函数 + dispatch 接线 + 死页 toast 保留为防御 + off 路径零改动 + prepare 行为逐字不变）、B 项（T2：表序插入 + core 零 app 依赖 + 「Agent 回末位」用例）、C 项（T1/T2 的 4 条 TDD 用例 + T3 回归清单：T12 生命周期用例经 assembly_dispatch_module_toggle_lifecycle 的 stopWhileRegistered 断言、core_pageid_order、buildGatedEntries 系列、活门控全链 sidebar probe）。
- **占位符扫描**：T1 1b 的「端口选择照 test_claude_service.cpp 既有用例」是显式授权的实现期核实点（避免写死端口重蹈 28888 flake），非 TBD；其余锚点全部有 file:line。
- **类型一致性**：`ensureClaudeServiceAssembled(bool)` / `ClaudeAssemblyHandler` / `setClaudeAssemblyHandler` 三处（头声明、缝实现、main 注入、dispatch 调用）签名一致，static_assert 钉死；`assembleClaudeServiceNow()` 无参（端口自读 settings，与控制器「函数内自读 SettingsStore」一致）。
- **幂等口径（已核）**：getClaudeService 重复调用 = 同指针零副作用（main.cpp:235-253 static 守卫）；startClaudeProxy 重复调用 = isProxyRunning 早退（main.cpp:313-316）→ ensureClaudeServiceAssembledImpl 重复调用安全，dispatch 重复 on 安全。
- **Review Focus 落钉**：5/5（T1#1 Step 2、T1#2 用例一断言 2、T1#3 用例一断言 3、T2#4 Step 4、T1#5 用例二）。

### 锚点核实差异（与控制器原话的出入，均已按实况转写）

1. `getClaudeService` 定义在 **main.cpp:234-256**（main 文件级 static 守卫），控制器原文「读 src/module_claude_proxy/claude_service.cpp 的 getClaudeService」——claude_service.cpp 只有 `setGlobal/global`（:126-127）。幂等语义结论不变（成立）。
2. `prepareClaudeServiceBeforeModules` 实际在 **main.cpp:333-368**（非 341-374）；`dispatchModuleToggle` 在 **app_assembly.cpp:255-286**（非 252-280）；死页 toast 分支在 **claude_module.cpp:119-131**（`if (!svc)` 起 :119）。内部结构与控制器描述逐项吻合。
3. 控制器所指「tests/test_app_sidebar.cpp 新用例『Agent 回末位』（约 :1593）」——**该路径不存在**（实为 `tests/test_modules/test_app_sidebar.cpp`，且无同名既有用例、无 :1593 区域）；T2 按「新建该语义用例」落。
4. 「新桥函数由 main.cpp 文件级实现」按链接面实况收窄为：**缝落 module_toggle_bridge.cpp（默认 no-op + setter），真实现是 main.cpp 文件级函数经 setter 注入**——app_assembly.cpp 被 devdesk_tests（tests/CMakeLists.txt:4）与 module_toggle_sidebar_probe（:190）链接且都不链 main.cpp，直呼 main.cpp 符号即链接失败。与「照 module_toggle_bridge.h 既有模式」的裁定一致（既有缝就是 setter 模式，实现 TU 是 module_toggle_bridge.cpp 而非 app_assembly.cpp）。
5. 验收项 1 的端到端断言（dispatch → global() 非空 + isProxyRunning）在 headless 层以「seam 注入真 ClaudeService handler」达成；main.cpp 的 static unique_ptr 持有段不在测试链接面，由 T3 Step 5 app 级冒烟补盖。
6. 方案独立审计（2026-10-01，dev `1e676fb`）裁定**可执行**，3 点修订已并入（gate 日志话术 / 两段红口径 / KNOWN_LIMITATIONS 引用）；审计确认幂等语义、链接面、表序插入对 stopAll 影响方向均与实况吻合。
7. 终审（2026-10-01）发现并修复退出时序 UAF：`~ClaudeService` 清 global（claude_service.cpp），防惰性装配反转 atexit LIFO；冒烟补「拨开后退出」一步。
8. 修复轮 2（2026-10-01）根修同一根因的另一半：`getClaudeService` 的 static `unique_ptr` 改**永不析构堆持有**（`static auto* holder = new std::unique_ptr<...>`，main.cpp）——exit hook 恒见到活服务 → onStop 走 stopProxy + 还原，settings.json 在退出时正常还原；`~ClaudeService` 摘牌保留作测试卫生。冒烟 ⑤ 措辞随根修收敛为「无崩溃 + 还原日志正常 + settings.json 已还原（sidecar 清）」。
