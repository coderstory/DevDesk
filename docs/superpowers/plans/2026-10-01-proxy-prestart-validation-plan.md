# 代理启动前置校验（激活 profile + 联通测试）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 「开启代理」这个功能（启动自启 / 会话内开 claude 模块 / Claude 页手动按钮）在执行前先做两项校验——① 有激活且可读的 profile；② 该 profile 连通测试通过——**两项全过才 bind 端口、才接管 settings.json**；任一不过 = 零副作用失败（不 bind、不写 settings.json、不建 sidecar），并把原因送到用户可见面（手动路径 toast / 会话内 notice / 启动期 LOG）。

**Architecture:** 校验体落 `ClaudeService::validateProxyPreconditions(int probeTimeoutMs)`（门面新公开方法，可无头单测——main.cpp 不在 `devdesk_tests` 链接面）——检查 1 走既有 `getActiveProfile()`（一条只读 SQL，零网络），检查 2 **复用**既有 `testConnection` 探测通道（同上游方言装配，2xx 判定口径与 Claude 页「测试连通」逐字一致），仅把硬编码的 15s 超时通过新增私有 `probeProfile(profile, timeoutMs)` 拆出短超时口子（公开 43 签名不动）。三条生产启动路径各自在 `startProxy` **之前**显式调用校验：main.cpp 的 `startClaudeProxy()`（启动自启 + 会话内惰性装配共用同一函数 = 一处改动覆盖两条路径）、claude 页 `doToggleProxy()`（**只有联网校验段**改后台线程 + compose 收割，与「测试连通」同款不冻结 UI；`startProxy` 本体留在 UI 线程、移到收割点执行——见 T2 Step 5 的 A1 说明）。`startProxy` 保持低层原语语义（不做校验）——放进去会连带打破 `test_settings_sync.cpp:744-758` 的「无激活 profile 仍接管」既有契约与全部直接起代理的用例。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（`REGISTER_TEST` + `DOCTEST_CHECK/REQUIRE`，fail-fast——CHECK 失败即 return false，RAII 清理放栈对象析构）；`platform::http::Server` 本地 mock 上游。

**Spec:** 无独立 spec 文件——**本计划即权威口径**，来源 = 用户 2026-10-01 需求原文「开启代理这个功能，先执行校验：1. 有激活的 profile；2. profile 联通测试 ok —— 没问题才能开启」+ 同日事故（会话内开模块带自启 → 空 profile store 上起了代理 → 绑 8888 → 全部请求 503「没有激活的 Claude profile」→ 本机自身推理路径（走 `127.0.0.1:8888`）被打死）。背景规格：`docs/superpowers/specs/2026-09-27-provider-config-design.md`（接管协议）、`docs/superpowers/plans/2026-10-01-claude-lazy-assembly-plan.md`（惰性装配，本计划的前置交付）。**全部锚点已于 2026-10-01 在 dev 分支（HEAD `2c21ee7`）逐一核实**，与控制器原话有出入的锚点列在「Self-Review 记录 → 锚点核实差异」。

## Global Constraints

- **不抛异常**：错误一律 `core::Result<T>`；模块隔离不破（本计划不新增跨模块 include：改的是 `module_claude_proxy` 内部 + app 层 main.cpp + 既有 app_assembly 桥）。
- **零副作用失败是硬契约**：校验不过 ⇒ 不 bind 端口、不接管 settings.json、不建 sidecar、不动激活行。落点依据：`syncTakeOver` 只在 `startProxy` 内、且只在 `proxyRuntime_.startProxy` 成功之后调用（claude_service.cpp:507-517）；而**无激活 profile 时 `syncTakeOver` 仍会接管**（claude_service.cpp:850-852 的既有设计「接入路径先就位，转发层报 503」——正是本次事故的机制，本需求从上层推翻它：三条生产路径不得再走到那里）。
- **`startProxy` 公开签名与行为零变化**：43 签名口径（R10 交付）不动；`startProxy` 自身**不**做前置校验（既有直接起代理的用例 + `test_settings_sync.cpp:744-758` 的「无激活 profile 仍接管 + 警告」契约保持不变）。校验是调用方的显式前置步骤。
- **`testConnection` 公开签名零变化**：超时经新增**私有** `probeProfile(profile, timeoutMs)` 参数化，`testConnection(profile)` 恒传 15000（与今日逐字一致，含全部方言头用例）。
- **UI 线程不得跑联网探测**：既有回归钉子 `claude_module_test_connection_async`（test_claude_ui.cpp:276-346）与 claude_module.cpp:753-755 的注释口径——手动启动同款处理（后台线程 + 原子标志 + compose 每帧收割）。**反向同样成立（2026-10-01 审计 A1，必须项）：`startProxy` 不得进后台线程**——`settings_takeover.h:6-7` 的契约是「仅 UI 线程」，`startProxy` 成功即 `syncTakeOver`（写 settings.json + sidecar + `warnings_`），与 `onStop → stopProxy → syncRestore`（claude_module.cpp:364-374，同为 UI 线程且**不 join** `testThread_`）及 settings_module 的 `lastSyncWarnings()`（settings_module.cpp:681）/`isClaudeCodeManaged()`（:1037）读并发 = 竞态。手动路径的线程**只跑** `validateProxyPreconditions()`；`startProxy` 在 `harvestTestResult()`（claude_module.cpp:781-787，UI/compose 线程、join 之后）执行。
- **启动期不做无界阻塞**：校验有界（`kProxyPreStartProbeTimeoutMs` = 2000ms，`CURLOPT_TIMEOUT_MS` 封顶，http_client.cpp:67），失败**只记 LOG**——**理由订正（审计 A3）**：不是「无 notice 宿主」（`app::showAppNotice` 是进程级状态，**不是页级**，见 app_sidebar.cpp:69-71 与 Task 2 的 D-1），真实取舍是 UX（启动自启是后台动作，开窗即弹无上下文失败窗属打扰）；与 main.cpp:362-364 / 376-377 的启动期日志口径一致。设计取舍见 Task 2 的「决策 D-1」。
- **中文文案统一**：失败前缀固定「代理启动前置校验未通过（1/2 激活 Profile）」/「（2/2 连通测试）」——日志、toast、notice 三处共用同一句，用户看到的原因可原样去日志里搜。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化；若某用例首跑即绿，做变异验证（临时回退实现 → 必须红 → 还原）并在报告记录。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`；全量基线**以最近一次全量运行输出为准**（用例总数不在文档写死），验收口径 `Failed: 0`；probe 另跑 `(cd build && ctest -L probe)`。
- **提交**：原子提交、显式 pathspec、禁 `git add -A`；提交前 `git status --porcelain` 复查（仓库根出现 `NUL` 即删）。实现任务在 worktree 隔离执行，主工作树禁跑构建；T3 收尾由主会话在合并后执行。
- **平台**：本机 macOS（arm64）为当前开发平台；本计划无平台分支代码（探测走跨平台 `platform::http`）。

## Review Focus（五类最易咬人的输入）

1. **校验体被绕过 / 只接了一条路径**：三条生产启动路径必须逐条咬住——(a) 启动自启（main.cpp:373-374）、(b) 会话内惰性装配（main.cpp:414-419）、(c) Claude 页手动按钮 / Ctrl+Enter（claude_module.cpp:535-555）。(a)(b) 共用 `startClaudeProxy()` 一处改动即覆盖，必须显式验证这一点（不是"改了两处"）。钉子：T2 的会话内用例（空库 dispatch on → `isProxyRunning()==false` + `g_asmStartCalls==0`）+ T2 的手动按钮用例（空库点按钮 → 不起代理 + toast 给原因）。
2. **失败仍产生副作用**（本次事故本体）：不 bind、不接管 settings.json、不建 sidecar、激活行不动。钉子：T1 用例 ② 用临时 settings.json 逐字比对（`readFileToString` 相等 + sidecar 不存在 + `proxyPort()==0`）。
3. **UI 线程被联网探测冻结**（09-27 回归的重演）：手动按钮走校验后仍是同步调用 = 最坏 2s 白屏。钉子：T2 手动按钮用例的「点击立即返回 + compose 轮询收割」形态（照 test_claude_ui.cpp:326-341 的 20ms×250 轮询）+ 既有 `claude_module_test_connection_async` 零回归。**修反方向也咬（审计 A1）**：后台线程只许跑探测——钉子 = T2 Step 6 用例 3c（慢上游校验在飞时 `isProxyRunning()==false`，代理只在 compose 收割帧之后才起来）。
4. **超时没接上（静默回落 15s）**：`probeProfile` 的 `timeoutMs` 若在重构中被漏传，校验会静默变成 15s 阻塞，功能"看起来对"。钉子：T1 用例 ⑤（mock 上游挂 1500ms，`probeTimeoutMs=150` → 必须 fail；漏接则请求在 1.5s 后成功、断言红）——断言是 ok/fail 二值，不依赖墙钟测量。
5. **既有用例的语义随需求反转**：`assembly_dispatch_claude_lazy_assemble_autostart`（test_app_assembly.cpp:923-984）今天断言的是**空 profile store 也能起代理**（= 事故语义）。它必须按新契约更新（先种激活 profile + mock 上游），并在报告里逐条列出「改了哪条断言、为什么」，不得悄悄放宽。钉子：T2 Step 3 的更新清单 + 全量零回归。

---

### Task 1: `ClaudeService::validateProxyPreconditions`（两查 + 零副作用）+ TDD

**Files:**
- Modify: `src/module_claude_proxy/claude_service.h`（新增公开方法 + 超时常量；新增私有 `probeProfile`）
- Modify: `src/module_claude_proxy/claude_service.cpp`（`testConnection` 函数体拆入 `probeProfile`；实现校验）
- Test: `tests/test_modules/test_claude_service.cpp` 追加（新区段「18. 代理启动前置校验」）

**已核实锚点（实现前无需再猜）：**

| 锚点 | 实际位置 | 语义 |
|---|---|---|
| `getActiveProfile` | claude_service.h:126 / claude_service.cpp:250-252 → profile_repository.cpp:746-774 | 一条 `SELECT ... WHERE is_active = 1 LIMIT 1`；无激活 → fail `NoActiveProfile` + 文案「没有激活的 Claude profile，请在 DevDesk 的 Claude 页选中 Profile 并点击「启用」」（:765-767，503 body 同源）。**零网络、零文件** |
| `testConnection` | 声明 claude_service.h:184；定义 claude_service.cpp:429-466 | **同步** `platform::http::send`，`req.timeoutMs = 15000`（:453 字面量）；无 key 也发（未授权请求拿 401）；经 `UpstreamDialect::applyHeaders`（:450-452）。今日唯一调用点 = claude_module.cpp:758（**后台线程**） |
| `startProxy` | 声明 claude_service.h:227-228；定义 claude_service.cpp:507-517 | `proxyRuntime_.startProxy`（bind，proxy_runtime.cpp:50-85）成功后才 `syncTakeOver()`（:514）——**校验放它之前 = 天然零接管** |
| `syncTakeOver` 的无 profile 分支 | claude_service.cpp:845-873（「无激活 profile：仍接管 BASE_URL/api key 占位」在 :850-852） | 既有设计，本计划不删（`test_settings_sync.cpp:744-758` 钉着它）；由上层不再走到 |
| HTTP 错误码口径 | platform/http_client.cpp:104-109 | **所有** curl 失败（含 `CURLE_OPERATION_TIMEDOUT`）一律映射 `ErrorCode::NetworkUnreachable`——断言不要写 `NetworkTimeout` |
| 测试 mock 模式 | test_claude_service.cpp:952-968（200 / `/v1/models`）、:994-1010（401）、:1177（不可达 `http://127.0.0.1:1`）、:1458（`startProxy("127.0.0.1", 0)` 端口 0） | 直接照抄 |

**Interfaces:**

```cpp
// claude_service.h —— 「---- 代理 server 生命周期 ----」段（startProxy 声明之前，:219-228）
    // 代理启动前置校验（2026-10-01 事故：无激活 profile 起代理 → 8888 全量 503，
    // 本机推理路径被劫持）。「开启代理」前两项必须全过，任一不过 → fail 且
    // **零副作用**（不 bind 端口、不接管 settings.json、不建 sidecar、不动激活行）：
    //   1) 有激活且可读的 profile（getActiveProfile，一条只读 SQL，零网络）
    //   2) 该 profile 连通测试通过（GET <baseUrl>/v1/models 返回 2xx）
    // 探测通道复用 testConnection 的探测体（同上游方言装配：UA / 鉴权头 /
    // opencode 会话头——否则会出现「探测按 A 方言、转发按 B 方言」错位）。
    // 2xx 判定与 Claude 页「测试连通」逐字一致（claude_module.cpp:764-768）：
    // 2xx = 连通成功；其余状态码（含 401/403/5xx）= 连通异常一律 fail——没有
    // 可用 key 的 profile 会拿到 401，本条同时覆盖「Keychain 里没有 key」。
    // 本函数是「开启代理」动作的前置步骤，联网段**不得在 UI 线程同步调用**
    //（手动路径走后台线程 + compose 收割，见 claude_module.cpp doToggleProxy）。
    core::Result<void> validateProxyPreconditions(
        int probeTimeoutMs = kProxyPreStartProbeTimeoutMs);

    // 前置校验的探测超时（毫秒）。取值依据：探测只发一个 GET /v1/models，
    // 正常上游 RTT 百毫秒量级；2s 覆盖慢链路；黑洞上游（SYN 丢弃）由
    // CURLOPT_TIMEOUT_MS 封顶（http_client.cpp:67）。短于 testConnection 的
    // 15s：校验失败要用户可感知地快。
    static constexpr int kProxyPreStartProbeTimeoutMs = 2000;
```

```cpp
// claude_service.h —— private 段（syncTakeOver / syncRestore 声明旁，:345-361）
    // testConnection 的探测体（可配超时）：testConnection(profile) 恒传
    // kTestConnectionTimeoutMs(15000)，前置校验传短超时。公开签名零变化
    //（R10 交付的「43 签名」不动——超时是内部量，不进公开面）
    core::Result<platform::http::Response> probeProfile(const Profile& profile,
                                                        int timeoutMs);
    static constexpr int kTestConnectionTimeoutMs = 15000;
```

```cpp
// claude_service.h —— :156（`reapplyTakeOver`，公开的零门控接管入口；今日无调用方）
    // 立即用当前激活 profile（含所绑片段）重写 settings.json（设计 §2.5：片段
    // 保存后必须当场生效）。通道未注入 / 接管开关关 → no-op 成功。
    // ⚠️ 2026-10-01 审计（A2）：本入口**不经任何前置校验**——无激活 profile 时
    // 同样会走 syncTakeOver 的无 profile 分支（claude_service.cpp:845-873）接管。
    // **调用方必须先过 validateProxyPreconditions 校验**（或明确只做「片段保存 /
    // 切 profile 后的重写」——此时接管本就在位）。本计划不改行为，只补口径注释。
    core::Result<void> reapplyTakeOver() { return syncTakeOver(); }
```

- [ ] **Step 1: 写失败测试**（test_claude_service.cpp 末尾追加新区段；桩与工具全部复用本文件既有件——`test_support::TestDb`（test_support.h:330）、**文件内匿名命名空间**的 `TempSyncPaths`（:85-107）、`readFileToString`（:109-113）、`test_support::cleanupProfileKey`（**共享实现**，test_support.h:517；本文件 :45-49 已有 `using test_support::…` 段，:172 起即用此名））：

```cpp
// ---- 18. validateProxyPreconditions（2026-10-01 事故：无 profile / 不连通不得起代理）----

REGISTER_TEST(claude_pv_no_active_profile,
              "claude_service: 前置校验① 无激活 profile → fail（NoActiveProfile）且零副作用", {
    test_support::TestDb tdb(kDbPrefix, "pv_noactive");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto r = svc.validateProxyPreconditions();
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NoActiveProfile);
    DOCTEST_CHECK(r.errorMessage().find("前置校验未通过（1/2 激活 Profile）") !=
                  std::string::npos);
    DOCTEST_CHECK(r.errorMessage().find("没有激活的 Claude profile") !=
                  std::string::npos);          // 可操作指引随错误码原样透传
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());  // 不 bind
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);
    return true;
})

REGISTER_TEST(claude_pv_unreachable_no_takeover,
              "claude_service: 前置校验② 上游不可达 → fail（NetworkUnreachable）且 settings.json 未被接管", {
    test_support::TestDb tdb(kDbPrefix, "pv_conn");
    TempSyncPaths sync("pv_conn");                   // 文件内桩：临时 settings.json + sidecar
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(sync.paths());
    svc.setSettingsTakeOverEnabled(true);            // 接管开关默认即 true，此处显式钉住
    const std::string kOriginal = R"({"env":{"FOO":"bar"}})";
    sync.writeSettings(kOriginal);

    // 端口 1 = 几乎必然拒绝连接（既有用例同款，:1177）
    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-pv-7777", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());  // 首次激活：G0 不读文件

    auto r = svc.validateProxyPreconditions(500);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NetworkUnreachable);
    DOCTEST_CHECK(r.errorMessage().find("前置校验未通过（2/2 连通测试）") !=
                  std::string::npos);
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);
    // 事故本体断言：settings.json 一字未改、无 sidecar（= 没走到 syncTakeOver）
    DOCTEST_CHECK_EQ(readFileToString(sync.settings), kOriginal);
    DOCTEST_CHECK_FALSE(std::filesystem::exists(sync.sidecar));
    DOCTEST_CHECK(svc.lastSyncWarnings().empty());
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_non_2xx_fails,
              "claude_service: 前置校验③ 上游 401 → fail（无/坏 key 不得起代理）", {
    platform::http::Server upstream401("127.0.0.1", 0);   // 模式照 :994-1010
    upstream401.route("GET", "/v1/models",
                      [](const platform::http::RequestCtx&,
                         platform::http::ResponseWriter& w) {
                          w.setStatus(401);
                          w.setHeader("Content-Type", "application/json");
                          w.write(R"({"error":{"type":"authentication_error"}})");
                          w.end();
                      });
    DOCTEST_REQUIRE(upstream401.start());

    test_support::TestDb tdb(kDbPrefix, "pv_401");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "NoKey", "http://127.0.0.1:" + std::to_string(upstream401.actualPort()),
        "sk-ant-pv-401", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto r = svc.validateProxyPreconditions(2000);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::HttpError);
    DOCTEST_CHECK(r.errorMessage().find("401") != std::string::npos);
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upstream401.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_ok_then_start_proxy,
              "claude_service: 前置校验④ 通过（2xx）→ 校验只探一次，startProxy 仍可用", {
    platform::http::Server upstream("127.0.0.1", 0);       // 模式照 :952-968
    int hitCount = 0;
    std::string lastPath;
    upstream.route("GET", "/v1/models",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       ++hitCount;
                       lastPath = ctx.path;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());

    test_support::TestDb tdb(kDbPrefix, "pv_ok");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "Good", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-pv-ok-8888", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto v = svc.validateProxyPreconditions(2000);
    DOCTEST_CHECK(v.isOk());
    DOCTEST_CHECK_EQ(hitCount, 1);
    DOCTEST_CHECK_EQ(lastPath, std::string("/v1/models"));
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());   // 校验本身绝不 bind

    // 通过后 startProxy 照常（端口 0 = OS 分配）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK(svc.proxyPort() > 0);
    // 不变式：校验不在 startProxy 内部（否则这里会变成 2——且 startProxy 的
    // 「无激活 profile 仍接管」既有契约会被打破，见 test_settings_sync.cpp:744-758）
    DOCTEST_CHECK_EQ(hitCount, 1);
    svc.stopProxy();
    upstream.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_timeout_honored,
              "claude_service: 前置校验⑤ 探测超时按入参生效（不静默回落 15s）", {
    // mock 上游挂 1500ms 才回 200：probeTimeoutMs=150 → 必须 fail。
    // 若超时参数没接上（回落 testConnection 的 15000），请求会在 1.5s 后成功
    // → 本用例红（断言是 ok/fail 二值，不依赖墙钟测量；10× 余量保证负载抖动
    // 下也不会误判。收尾 slow.stop() 会 join 上游 worker，用例总耗时 ≈1.5s）
    platform::http::Server slow("127.0.0.1", 0);
    slow.route("GET", "/v1/models",
               [](const platform::http::RequestCtx&,
                  platform::http::ResponseWriter& w) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                   w.setStatus(200);
                   w.setHeader("Content-Type", "application/json");
                   w.write(R"({"data":[]})");
                   w.end();
               });
    DOCTEST_REQUIRE(slow.start());

    test_support::TestDb tdb(kDbPrefix, "pv_timeout");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "Slow", "http://127.0.0.1:" + std::to_string(slow.actualPort()),
        "sk-ant-pv-slow", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto r = svc.validateProxyPreconditions(150);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    slow.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})
```

（`TempSyncPaths` 是 test_claude_service.cpp 匿名命名空间的**文件内桩**（:85-107，构造参数只有 tag），成员 `settings` / `sidecar` / `writeSettings()` / `paths()`；`lastSyncWarnings()` 声明在 claude_service.h:314。若既有成员名有出入，按实际改写测试，不改语义。）

- [ ] **Step 2: 红**（`validateProxyPreconditions` 不存在 → 编译失败即 RED 证据）

- [ ] **Step 3: 实现**——先把 `testConnection`（claude_service.cpp:429-466）函数体**逐字**搬进 `probeProfile`，唯一改动是 `req.timeoutMs = 15000;`（:453）→ `req.timeoutMs = timeoutMs;`：

```cpp
// claude_service.cpp —— 「---- 连通性测试 ----」段（:427 起）

core::Result<platform::http::Response>
ClaudeService::probeProfile(const Profile& profile, int timeoutMs) {
    // 原 testConnection 函数体逐字搬入（日志前缀保持 "testConnection: "，
    // 既有日志口径不变）；唯一改动 = req.timeoutMs 取入参
    auto keyResult = loadApiKey(profile.id);
    std::string key;
    if (keyResult.isOk()) {
        key = keyResult.value();
    } else {
        LOG_WARN("testConnection: 无法读取 API key (" +
                 keyResult.errorMessage() + ")，将发未授权请求");
    }

    platform::http::Request req;
    req.method = "GET";
    req.url = stripTrailingSlash(profile.baseUrl) + "/v1/models";
    const UpstreamDialect dialect =
        UpstreamDialect::forBaseUrl(profile.baseUrl, profile.dialectOverride);
    dialect.applyHeaders(req.headers, key, authFieldFromText(profile.authField));
    req.timeoutMs = timeoutMs;          // ← 唯一改动（原字面量 15000）

    auto resp = platform::http::send(req);
    if (resp.isFail()) {
        LOG_WARN("testConnection 网络失败: " + resp.errorMessage());
        return core::Result<platform::http::Response>::fail(
            resp.errorCode(), resp.errorMessage());
    }

    LOG_INFO("testConnection: " + req.url +
             " -> " + std::to_string(resp.value().statusCode) +
             " (" + std::to_string(resp.value().elapsedMs) + "ms)");
    return core::Result<platform::http::Response>::ok(std::move(resp).value());
}

core::Result<platform::http::Response>
ClaudeService::testConnection(const Profile& profile) {
    return probeProfile(profile, kTestConnectionTimeoutMs);   // 15s 口径不变
}

core::Result<void> ClaudeService::validateProxyPreconditions(int probeTimeoutMs) {
    // 检查 1：激活 profile（零网络，微秒级）。失败原样透传错误码 + 可操作
    // 指引（文案出自 profile_repository.cpp:765-767，503 body 同源），前缀
    // 标明是本校验挡下的
    auto active = getActiveProfile();
    if (active.isFail()) {
        LOG_WARN("代理启动前置校验未通过：无激活 profile - " + active.errorMessage());
        return core::Result<void>::fail(
            active.errorCode(),
            "代理启动前置校验未通过（1/2 激活 Profile）：" + active.errorMessage());
    }
    const Profile prof = std::move(active).value();

    // 检查 2：连通测试（联网，最长 probeTimeoutMs）。探测失败 / 非 2xx 都算不过
    auto resp = probeProfile(prof, probeTimeoutMs);
    if (resp.isFail()) {
        LOG_WARN("代理启动前置校验未通过：连通测试失败（profile id=" +
                 std::to_string(prof.id) + "）- " + resp.errorMessage());
        return core::Result<void>::fail(
            resp.errorCode(),
            "代理启动前置校验未通过（2/2 连通测试）：" + resp.errorMessage());
    }
    const int code = resp.value().statusCode;
    if (code < 200 || code >= 300) {
        LOG_WARN("代理启动前置校验未通过：上游返回 HTTP " + std::to_string(code) +
                 "（profile id=" + std::to_string(prof.id) + "）");
        return core::Result<void>::fail(
            core::ErrorCode::HttpError,
            "代理启动前置校验未通过（2/2 连通测试）：上游返回 HTTP " +
                std::to_string(code) + "（检查 API Key 或 Base URL）");
    }
    LOG_INFO("代理启动前置校验通过：profile「" + prof.name + "」(id=" +
             std::to_string(prof.id) + ") 连通 OK（HTTP " + std::to_string(code) + "）");
    return core::Result<void>::ok();
}
```

- [ ] **Step 3b: 第二处改动（审计 A2，注释 only、零行为）**——`claude_service.h:156` 的 `reapplyTakeOver()`（`return syncTakeOver();`，公开的**零门控接管入口**，今日全仓无调用方）按 Interfaces 段的 A2 代码块补注释：明写「本入口不经任何前置校验，无激活 profile 时同样走 syncTakeOver 的无 profile 分支接管（claude_service.cpp:845-873）；**调用方必须先过 `validateProxyPreconditions`**（或明确只做「片段保存 / 切 profile 后的重写」——此时接管本就在位）」。不改签名、不改行为、不加门控（本计划范围外；残差风险 ④ 登记）。
- [ ] **Step 4: 绿** + 定向复跑既有连通性用例（`./build/tests/devdesk_tests claude_service` —— `claude_service_test_connection*` / `claude_service_proxy_lifecycle` / 反向同步与接管全段必须零回归，证明 `probeProfile` 拆分是等价重构）
- [ ] **Step 5: Commit** `git commit -m "feat(claude): validateProxyPreconditions 启动前置校验（激活 profile + 联通测试，零副作用失败）"`（显式 pathspec：`src/module_claude_proxy/claude_service.h src/module_claude_proxy/claude_service.cpp tests/test_modules/test_claude_service.cpp`）

### Task 2: 三条启动路径接线 + 手动按钮改后台线程 + 用例（含既有用例按新契约更新）

**Files:**
- Modify: `src/main.cpp`（`startClaudeProxy` 加校验 + 失败提示开关；会话内路径改真）
- Modify: `src/module_claude_proxy/claude_module.cpp`（`doToggleProxy` 改后台线程（**只跑校验**）+ `harvestTestResult` 新增收割点起代理（**A1**）+ 新成员 `testResultStartProxy_`；忙碌 toast 文案统一）
- Test: `tests/test_modules/test_app_assembly.cpp` 追加 + **既有用例按新契约更新**；`tests/test_modules/test_claude_ui.cpp` 追加

**已核实锚点（实现前无需再猜）：**

| 锚点 | 实际位置 | 语义 |
|---|---|---|
| `startClaudeProxy` | main.cpp:328-347（`isProxyRunning` 早退 :334-337；`startProxy` 调用 :341） | **两条路径的唯一汇聚点**：(a) 启动自启 main.cpp:373-374（`prepareClaudeServiceBeforeModules` 内）、(b) 会话内惰性装配 main.cpp:414-419（`ensureClaudeServiceAssembledImpl`）。改一处 = 覆盖两条 |
| 会话内路径的用户可见面 | main.cpp:414-419 + app_assembly.cpp:270-281（dispatch on 分支） | 桥签名 `ClaudeAssemblyHandler = void(*)(bool)`（module_toggle_bridge.h:25、实现 .cpp:30-42）**无返回通道**；用户可见反馈由 main.cpp 侧经 `app::showAppNotice`（main.cpp 已 include app_sidebar.h:7） |
| 设置页拨开关的旧文案 | settings_module.cpp「模块已开启」status（活门控 Task 2 落点） | 模块确实开启成功；代理未起属本模块之外的附加动作，反馈走 notice（不误报"模块开启失败"） |
| `doToggleProxy` | claude_module.cpp:535-555（UI 线程同步 `startProxy` :548） | 手动按钮（claude_ui_list.cpp:416-428 的 `claude.btn.proxy`）+ Ctrl+Enter（claude_module.cpp:415-419）共用 |
| 既有后台线程模式 | claude_module.cpp:744-787（`doTestConnection` / `harvestTestResult`）+ compose 每帧收割 :317 + 析构 join :59-61 + 成员 :802-808 | `testThread_` / `testDone_` / `testResultTitle_/Msg_/IsError_`——手动启动复用同一套（见下方"是否新增线程成员"注；**A1** 另加一个结果成员 `testResultStartProxy_`）。**A1 注**：既有线程今日只跑 `testConnection`（纯 HTTP + 只读，线程内不碰 settings.json / 接管状态）＝本来就安全；手动启动是**本仓第一处**把「跨线程共享状态 / 接管写」放到线程外的调用点，故本计划额外把 `startProxy` 钉回 UI 线程收割点（Step 5 的 A1 说明 + Step 6 用例 3c） |
| 设置页端口重启 | settings_module.cpp:419-465（`startProxy` :448；`isProxyRunning` 早退 :440-446，`setStatus`+`LOG_INFO` :441-445） | **本计划范围外**，理由见 T2 末段 |
| app 层用例底座 | test_app_assembly.cpp:834-921（`assemblyHandlerStub` 镜像 main.cpp + `LazyAssemblyGuard` + `writeLazyAssemblyConfig`）、:923-984（autostart 用例）、:986-1020（no-autostart 用例）、notice 断言 :800-803 | 桥 handler 是桩，**真 main.cpp 不在 devdesk_tests 链接面**（该文件 :829-831 已如实注明） |
| 手动路径用例底座 | test_claude_ui.cpp:60-72（`TestEnv`：TestDb + ClaudeService + setGlobal）、:276-346（异步点击 + 20ms×250 轮询 + `claude.toast.title` 断言） | 直接照抄轮询形态 |

**决策 D-1（启动期口径，必须写进报告）**：启动自启走**同一个同步校验**（有界 ≤2000ms），失败**只记 LOG_WARN**，不弹 notice。**理由订正（2026-10-01 审计 A3，原稿不成立）**：原稿写「启动期未出首帧、notice 无宿主」——**错**。`app::showAppNotice` 是**进程级**状态、不是页级宿主（app_sidebar.cpp:69-71：`g_appNotice.show(...)`，渲染归 app 层，启动期调用同样能显示），所以「没有宿主」这条论据被推翻。**真实的取舍是 UX**：启动自启是**后台动作**（用户没点任何东西、也不知道什么是「代理」），一开窗就弹一个无上下文的失败窗 = 打扰；用户此刻也无法把提示与任何操作关联，更无从处置。会话内路径才是用户动作（点开关 / 按 Ctrl+Enter），故 `notifyFailure=true` 弹 notice（T2 Step 4）。**失败不静默**：LOG_WARN 有完整原因（含 1/2 与 2/2 前缀），与既有启动期日志口径一致（main.cpp:362-364 / 376-377），T3 Step 3 的真机冒烟按日志验收。**不采纳后台线程方案**，理由：① 线程持有 + 退出 join 必须与 exit hook 的「还原 settings.json 先于进程结束」序对齐（main.cpp:248-255 的永不析构堆持有注释正是为这条），而 **main.cpp 不在测试链接面**（tests/CMakeLists.txt 源表无 main.cpp）——不可测机制进不可测文件与 TDD 纪律冲突；② 受影响人群 = 显式打开 `claudeProxyAutoStart` 的用户（默认 false，settings_config.h:125）；③ 事故形态（无激活 profile）耗时 ~0ms，最坏（上游黑洞）由 `CURLOPT_TIMEOUT_MS` 封顶 2s（实测口径见 T3 Step 3 的黑洞主机名测法）。**后备路径**（若审查判定 2s 启动阻塞不可接受）：把自启推迟到 claude 模块首帧 compose，复用模块既有的后台线程 + 收割模式——记录在案，不在本计划实施。

- [ ] **Step 1: 写失败测试**（test_app_assembly.cpp）：先**更新**既有 autostart 用例、再**追加**空库用例。

```cpp
// (1) 既有用例按新契约更新：`assembly_dispatch_claude_lazy_assemble_autostart`
//     （:923-984）今天在**空 profile store** 上断言代理已起——那正是 2026-10-01
//     事故语义。新契约下必须：先起 mock 上游 + 种激活 profile，再 dispatch。
//     改动点（逐条，报告里列明）：
//     - 用例开头起 mock 上游（200 /v1/models，模式照 test_claude_service.cpp:952-968）
//     - 「on」之前先装配服务并种激活 profile：
//         asmEnsureService();                       // 从 stub 抽出的构造段（见下）
//         auto p = g_asmSvc->createProfile("asm", "http://127.0.0.1:" +
//                       std::to_string(upstream.actualPort()), "sk-ant-asm-0001", "m");
//         DOCTEST_REQUIRE(p.isOk());
//         DOCTEST_REQUIRE(g_asmSvc->setActiveProfile(p.value().id).isOk());
//     - 断言面不变（global 非空 / isProxyRunning / seam 序 / 幂等 / off / 非 claude）
//     - 收尾 cleanupProfileKey + upstream.stop()

// (2) 新增（本计划核心回归 = 事故现场）：
REGISTER_TEST(assembly_dispatch_claude_toggle_blocked_by_preconditions,
              "assembly: 会话内开 claude + autoStart，空 profile store → 前置校验挡下（代理不起/不接管）", {
    resetLazyAssemblyLedger();
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_pv", "noactive"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(scoped.cfgPath, true));   // autoStart=true

    test_support::TestDb asmDb("assembly_pv", "noactive_db");       // 空库 = 事故现场
    g_asmDb = &asmDb;
    app::toggle::setClaudeAssemblyHandler(&assemblyHandlerStub);

    ::core::Router router;
    router.registerModule(&g_toggleHome);

    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        true, router, &toggleLookupStub);

    auto* svc = module::claude::ClaudeService::global();
    DOCTEST_CHECK(svc != nullptr);            // 服务仍装配（模块要能进页，死页缓解不回归）
    DOCTEST_CHECK_EQ(g_asmStartCalls, 0);     // startProxy 一次都没跑（不 bind）
    if (svc != nullptr) {
        DOCTEST_CHECK_FALSE(svc->isProxyRunning());
        DOCTEST_CHECK_EQ(svc->proxyPort(), 0);
    }
    DOCTEST_CHECK(router.isRegistered(::core::PageId::ClaudeProxy));  // 模块本身照常开启
    DOCTEST_CHECK_EQ(g_claudeStarts, 1);      // onStart 照常
    // 用户可见面（桩镜像 main.cpp 的 showAppNotice 分支；真 main.cpp 路径归 T3 冒烟）
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK(app::appNoticeTitle() == std::string("代理未启动"));
    DOCTEST_CHECK(app::appNoticeMessage().find("前置校验未通过") != std::string::npos);
    return true;
})
```

- [ ] **Step 2: 红**（当前 stub 无校验 → 空库也 `startProxy` → `g_asmStartCalls == 1`、`isProxyRunning() == true`，两条断言红；notice 断言也红）

- [ ] **Step 3: 改 stub 镜像 main.cpp 的新逻辑**（test_app_assembly.cpp:834-868；`assemblyHandlerStub` 抽出一段 `asmEnsureService()` 供用例预置 profile 用）：

```cpp
// 与 main.cpp startClaudeProxy 同逻辑（含前置校验）；notifyFailure=true 分支
// 镜像 main.cpp 的 showAppNotice（真 main 路径不在本链接面，见 :829-831）
void asmEnsureService() {
    if (g_asmSvc == nullptr) {
        g_asmSvc = std::make_unique<module::claude::ClaudeService>(
            g_asmDb->handle(), "DevDeskTestClaudeService");
        module::claude::ClaudeService::setGlobal(g_asmSvc.get());
    }
}

void assemblyHandlerStub(bool autoStart) {
    ++g_asmHandlerCalls;
    g_asmLastAutoStart = autoStart;
    g_asmSeamSeq = ++g_seqCounter;
    asmEnsureService();
    const module::settings::SettingsStore store(::core::paths::configFile());
    const auto cfg = store.load();
    g_asmSvc->setConfiguredProxyPort(cfg.proxyPort);
    g_asmSvc->recoverPendingSettingsRestore();
    if (!autoStart) return;
    if (g_asmSvc->isProxyRunning()) return;                  // main 的早退语义
    auto v = g_asmSvc->validateProxyPreconditions();          // ← 新增：前置校验
    if (v.isFail()) {
        app::showAppNotice("代理未启动", v.errorMessage());    // ← 新增：用户可见面
        return;                                              // 零副作用失败
    }
    auto r = g_asmSvc->startProxy("127.0.0.1", 0);            // 端口 0 = OS 分配
    ++g_asmStartCalls;
    g_asmStartOk = r.isOk();
}
```

（`g_asmStartCalls` 语义收窄为「真正执行了 startProxy」——用例注释同步更新。`resetLazyAssemblyLedger()` 已清 notice，无需改。）

- [ ] **Step 4: main.cpp 接线**（`startClaudeProxy` :328-347 + 会话内调用点 :414-419）：

```cpp
// 启动本地 Claude 代理（全局唯一实例）。
// 调用方：prepareClaudeServiceBeforeModules（claudeProxyAutoStart=true 时，启动期）
//        ensureClaudeServiceAssembledImpl(autoStart=true)（会话内开模块）
//        notifyFailure=true（会话内 = 用户动作）→ 校验失败额外弹应用级 notice；
//        启动期（默认 false）只记 LOG——启动自启是后台动作，弹无上下文失败窗
//        UX 差（决策 D-1；审计 A3：showAppNotice 是进程级、启动期能弹，不弹是
//        取舍不是限制）
void startClaudeProxy(bool notifyFailure = false) {
    auto* svc = getClaudeService();
    if (!svc) {
        LOG_ERROR("startClaudeProxy: ClaudeService 初始化失败");
        return;
    }
    if (svc->isProxyRunning()) {
        LOG_INFO("Claude 代理已在运行 port=" + std::to_string(svc->proxyPort()));
        return;
    }

    // 启动前置校验（2026-10-01 事故）：无激活 profile / 连通不过 → 不起代理，
    // 零副作用（不 bind 8888、不接管 settings.json）。校验失败是预期内结果，
    // 记 LOG_WARN 不记 ERROR
    auto v = svc->validateProxyPreconditions();
    if (v.isFail()) {
        LOG_WARN("Claude 代理未启动（前置校验未通过）: " + v.errorMessage());
        if (notifyFailure) {
            app::showAppNotice("代理未启动", v.errorMessage());
        }
        return;
    }

    // 端口读设置页配置（审计 2026-09-23 [14]，默认 8888）；若被占用则 bind
    // 失败，调用方在日志里可见
    auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
    if (r.isFail()) {
        LOG_ERROR("Claude 代理启动失败: " + r.errorMessage());
        return;
    }
    // 启动成功日志由 ClaudeService::startProxy 统一输出一次，这里不重复记录
}

// 桥真实现（:409-419 既有段）——唯一改动 = 传 notifyFailure=true（会话内 = 用户动作）
void ensureClaudeServiceAssembledImpl(bool autoStart) {
    assembleClaudeServiceNow();
    if (autoStart) {
        startClaudeProxy(/*notifyFailure=*/true);
    }
}
```

（`prepareClaudeServiceBeforeModules` :373-374 的 `startClaudeProxy();` 一字不改 —— 默认参数即启动期口径。）

- [ ] **Step 5: 手动按钮改后台线程（A1：线程只跑校验，`startProxy` 回 UI 线程收割点）**（claude_module.cpp `doToggleProxy` :535-555 + `harvestTestResult` :781-787 + 结果成员 :802-808）：

> **A1 说明（2026-10-01 审计，必须项）——为什么 `startProxy` 不能留在线程里**
>
> `settings_takeover.h:6-7` 的契约是「**仅 UI 线程**（现状同款）」。`startProxy` 成功即调 `syncTakeOver`（写 `settings.json` + sidecar + `warnings_`）；若在后台线程执行，它与**同为 UI 线程**的 `onStop → stopProxy → syncRestore`（claude_module.cpp:364-374——注意它**不 join `testThread_`**，不会等这条线程）以及 settings_module 的 `lastSyncWarnings()` 读（settings_module.cpp:681）/ `isClaudeCodeManaged()` 读（:1037）并发 = 文件级重叠写 + 读到半截状态。旧稿把 `startProxy` 放进线程 lambda 就是这个形态。
>
> **修法**：线程**只跑** `validateProxyPreconditions()`（联网 + 只读 SQL，零共享写）；`startProxy` 移到 `harvestTestResult()`（compose 每帧调、**join 之后**、UI 线程）执行——bind + 接管写是**亚毫秒**级（端口 bind + 一次文件写），不会冻结界面；冻结的来源是联网探测，那一段已在线程里。
>
> **既有 `doTestConnection` 线程为何安全**：它只跑 `testConnection`（纯 HTTP + 只读，线程内不碰 settings.json / 接管状态）——所以这条约束此前从未被违反过。本计划的手动启动是**本仓第一处**「后台线程 + 共享状态写（接管）」共处的调用点（写法：线程只探测、接管写回到 UI 收割点），不是既有模式的自然延伸，必须显式钉住（钉子 = T2 Step 6 用例 3c）。

```cpp
    // 说明：手动启动 = 前置校验（联网，最长 kProxyPreStartProbeTimeoutMs）+ bind +
    // 接管。**只有联网校验段进后台线程**——bind + syncTakeOver 必须回 UI 线程
    // （审计 A1：settings_takeover 契约「仅 UI 线程」，理由见上方 A1 说明）。
    // 与「测试连通」共用同一条后台线程 + 每帧收割模式
    //（:744-787，回归见 test_claude_ui.cpp:272-276）。
    // Ctrl+Enter 快捷键走同一入口（:415-419）
    void doToggleProxy() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            showToast("代理操作失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        if (svc->isProxyRunning()) {
            svc->stopProxy();
            showToast("代理已停止", "本地代理已停止监听", false);
            return;
        }
        harvestTestResult();   // 先收上一轮（页面不可见时 compose 不跑）
        if (testThread_.joinable()) {
            showToast("操作进行中", "上一次校验/测试仍在进行，请稍候", false);
            return;
        }
        showToast("正在校验",
                  "检测激活 Profile 与上游连通性（最长 " +
                      std::to_string(ClaudeService::kProxyPreStartProbeTimeoutMs / 1000 + 1) +
                      " 秒）…", false);
        testThread_ = std::thread([this, svc] {
            // 线程里**只**跑探测：联网 + 只读 SQL，零共享写。
            // startProxy 绝不在这里执行（A1）——它由 harvestTestResult 在
            // join 之后、UI 线程里调
            auto v = svc->validateProxyPreconditions();      // 检查 1 + 检查 2
            testResultStartProxy_ = v.isOk();   // 收割点据此决定是否起代理
            if (v.isFail()) {
                testResultTitle_ = "启动代理失败";
                testResultMsg_ = v.errorMessage();
                testResultIsError_ = true;
            }
            testDone_.store(true, std::memory_order_release);
        });
    }

    // 收割后台结果：线程跑完后 join（回到 UI/compose 线程）再弹 toast。
    // **代理启动也在这里**——join 之后已无并发写者，bind + syncTakeOver
    // 回到 UI 线程（A1；settings_takeover.h:6-7 契约）
    void harvestTestResult() {
        if (!testThread_.joinable()) return;
        if (!testDone_.load(std::memory_order_acquire)) return;  // 还在跑
        testThread_.join();
        testDone_.store(false);                                  // ← 既有
        if (testResultStartProxy_) {          // 校验通过 → 这时才 bind + 接管
            testResultStartProxy_ = false;
            ClaudeService* svc = ClaudeService::global();
            if (svc != nullptr) {             // 模块已停/已析构 → 不起（零副作用）
                auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
                if (r.isFail()) {
                    testResultTitle_ = "启动代理失败";
                    testResultMsg_ = r.errorMessage();
                    testResultIsError_ = true;
                } else {
                    testResultTitle_ = "代理已启动";
                    testResultMsg_ = "监听 http://127.0.0.1:" +
                                     std::to_string(r.value());
                    testResultIsError_ = false;
                }
            }
        }
        showToast(testResultTitle_, testResultMsg_, testResultIsError_);  // ← 既有收尾后移到此
    }
```

（**新增成员**：`bool testResultStartProxy_ = false;`，落在 :802-808 那组「结果经 `testDone_` 原子标志回 compose 收割」的成员里——**单写者 = 后台线程（只在置 `testDone_` 之前写）、单读者 = join 之后的 UI 线程**，join 即 happens-before，与既有 `testResultTitle_/Msg_/IsError_`（同为普通成员、同款发布序，:802-803 注释已写明）完全一致，故**不需要 atomic**。`doTestConnection` :744-756 的忙碌 toast 文案同步改成同一句「操作进行中 / 上一次校验/测试仍在进行，请稍候」——两条路径共用 `testThread_`，单一文案才不会误导；`test_claude_ui.cpp:323` 断言的「测试中」是**发起后**的 toast（:756），保持不动。是否新增独立**线程**成员：**不新增**——共用 `testThread_` + `testDone_` 即可，析构 join（:59-61）已覆盖，第二套成员只会引入两份竞态面。**析构等待上限**：本路径线程最长 = 探测超时 2s，短于既有 `testConnection` 路径的 15s。**未收割 = 不起代理**：线程跑完但页面再未 compose（模块被关掉）时 `harvestTestResult` 不会被调 → 代理不会"偷偷"起来，与零副作用口径一致；用户重新开页时首帧即收割。**测试环境红线**：用例里 `configuredProxyPort` 必须先置 0（`setConfiguredProxyPort(0)`，OS 分配端口）——**绝不许让用例绑 8888**（本机开发会话的推理链路正走 `127.0.0.1:8888`，绑上去会打死自己；T2 Step 6 两个用例都已显式 `setConfiguredProxyPort(0)`）。）

- [ ] **Step 6: 新增手动按钮用例（3b 校验挡下 + 3c A1 收割点才 bind）**（test_claude_ui.cpp，紧邻既有 async 用例 :276 之后；形态照抄 :296-346）：

```cpp
// ---- 3b. 前置校验：无激活 profile 点「启动代理」→ 不冻结 UI、不起代理、给原因 ----
REGISTER_TEST(claude_module_start_proxy_requires_active_profile,
              "claude_module: 无激活 profile 点「启动代理」→ 后台校验挡下（不起代理 + toast 给原因）", {
    TestEnv env;                       // 空库 = 事故现场；svc 已 setGlobal
    env.svc->setConfiguredProxyPort(0);   // 红线：用例绝不绑 8888（开发会话推理链路）；校验失败路径本不 bind，显式钉住防回归
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_pv");
        mod->compose(ui, screen);
        ui.end();
    };

    {   // 点击「启动代理」→ 立即返回（toast 切到「正在校验」）
        eui::Ui ui;
        compose(ui);
        const auto* btn = ui.find("claude.btn.proxy");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("正在校验") != std::string::npos);
    }

    // 轮询 compose（每帧收割）直到失败结果 toast（空库路径零网络，通常 1-2 帧）
    bool gotResult = false;
    for (int i = 0; i < 250 && !gotResult; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        const auto* msg = ui.find("claude.toast.message");
        if (title != nullptr && msg != nullptr &&
            title->text.find("启动代理失败") != std::string::npos &&
            msg->text.find("前置校验未通过") != std::string::npos) {
            gotResult = true;
        }
    }
    DOCTEST_CHECK(gotResult);
    DOCTEST_CHECK_FALSE(env.svc->isProxyRunning());   // 不 bind
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), 0);
    mod->onStop();
    return true;
})

// ---- 3c. A1 反方向钉子：startProxy 只在 UI 收割帧执行（校验在飞 / 已回但未收割都不 bind）----
// 判别式：上游 500ms 后才回 200；点击后在**不 compose** 的情况下静置 500ms ——
//   · 正解（startProxy 在 harvestTestResult 里）→ 线程早已跑完，但没人收割 → 仍不 bind；
//   · 旧稿（startProxy 在线程里）→ 上游一回包线程立刻 bind → 循环里的 CHECK_FALSE 红。
REGISTER_TEST(claude_module_start_proxy_binds_only_at_harvest,
              "claude_module: startProxy 只在 UI 收割帧执行（校验在飞时不 bind，A1）", {
    TestEnv env;
    env.svc->setConfiguredProxyPort(0);   // 红线：绝不绑 8888；startProxy 端口由 OS 分配
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("GET", "/v1/models",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       std::this_thread::sleep_for(std::chrono::milliseconds(500));
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    auto a = env.svc->createProfile(
        "慢校验", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-pv-harvest", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_pv_harvest");
        mod->compose(ui, screen);
        ui.end();
    };

    {   // 点「启动代理」→ 立即返回（不冻结 UI 由 3b 钉；这里钉「不立刻 bind」）
        eui::Ui ui;
        compose(ui);
        const auto* btn = ui.find("claude.btn.proxy");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
    }

    // 校验在飞 + 已回包但**尚无 compose 帧**的整个窗口：一律不 bind
    // （25×20ms = 500ms，覆盖上游 500ms 延迟；这段刻意不 compose）
    for (int i = 0; i < 25; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        DOCTEST_CHECK_FALSE(env.svc->isProxyRunning());   // A1：绑定必须晚于收割帧
    }
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), 0);

    // 一帧 compose = 一次收割 → 这时才 bind（UI 线程内的亚毫秒操作）
    bool started = false;
    for (int i = 0; i < 50 && !started; ++i) {
        eui::Ui ui;
        compose(ui);
        started = env.svc->isProxyRunning();
        if (!started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    DOCTEST_CHECK(started);
    DOCTEST_CHECK(env.svc->proxyPort() > 0);
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("代理已启动") != std::string::npos);
    }
    env.svc->stopProxy();                 // 用完还原（onStop 也会停；显式更清楚）
    mod->onStop();
    upstream.stop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})
```

（节点 id 取自 `ui_kit::drawToast` 的固定拆分（ui_kit/toast.h:60-65：`<id>` / `<id>.icon` / `<id>.title` / `<id>.message` / `<id>.close`），claude 页的 id 前缀 = `claude.toast`（claude_module.cpp:525），故正文节点 = `claude.toast.message`。**3b/3c 的断言口径按 A1 的收割点形态**：代理是否已起一律看 `env.svc->isProxyRunning()`（= `startProxy` 的真实效果），而**不是**看线程是否跑完——3b 钉「校验不过 ⇒ 永不 bind + toast 给原因」，3c 钉「校验通过但未收割 ⇒ 仍不 bind，收割帧后才 bind」（= 反向咬「`startProxy` 又被塞回线程」的变异：把 Step 5 的 `startProxy` 挪回 lambda，3c 静置段的 `CHECK_FALSE` 必红、3b 则不变红——两条合起来才能同时锁住两个方向）。3c 收尾 `stopProxy()` + `mod->onStop()` 双保险，避免代理跨用例残留占端口。）

- [ ] **Step 7: 绿** + 定向（`./build/tests/devdesk_tests claude` + `./build/tests/devdesk_tests assembly`）+ 全量零回归（含 `test_settings_sync` 的「无激活 profile 仍接管」契约 :744-758 —— 必须**不动**）
- [ ] **Step 8: Commit** `git commit -m "feat(claude,assembly): 三条启动路径接前置校验（启动 LOG / 会话内 notice / 手动按钮后台线程 + toast）"`（显式 pathspec：`src/main.cpp src/module_claude_proxy/claude_module.cpp tests/test_modules/test_app_assembly.cpp tests/test_modules/test_claude_ui.cpp`）

**范围外（写明理由，不实施）**：设置页改端口触发的 `stopProxy → startProxy` 重启（settings_module.cpp:419-465，`stopProxy` :447 / `startProxy` :448）。理由：该路径的前提是代理**已经在跑**（= 已经过一次校验、已经接管），重启不会制造新的劫持；且插入 2s 探测会给「改端口」这个本就即时生效的操作引入无谓阻塞与新的失败模式。留 backlog 一行即可。

### Task 3: 收尾验证 + 文档口径（主会话在合并后执行）

- [ ] **Step 1: 全量构建 + 测试**：`bash scripts/build-test-run.sh --no-run` → `Failed: 0`（总数 ≥ 前基线 + 本计划新增 **7** 条用例：T1×5 + T2×2（3b 校验挡下 / 3c A1 收割点））；`(cd build && ctest -L probe)` 全绿（本计划不动 UI 几何，probe 期望零改动）
- [ ] **Step 2: 隔离守卫** `bash scripts/check-module-isolation.sh`（本计划未新增跨模块 include，必须仍绿）
- [ ] **Step 3: 启动期冒烟（决策 D-1 的真机验收 + 「≤2s 上界」的实测数字）**：独立 `DEVDESK_DATA_ROOT` 起 app，配置 `claudeProxyAutoStart=true`，**分两轮**：
  - **轮 1（事故形态，零网络）**：**空 profile store** → ① 日志出现 `Claude 代理未启动（前置校验未通过）` + `前置校验未通过（1/2 激活 Profile）`；② `lsof -nP -iTCP:8888 -sTCP:LISTEN` 无输出（8888 未监听）；③ `~/.claude/settings.json` 内容与启动前逐字相同（`ANTHROPIC_BASE_URL` 未变成 `http://127.0.0.1:8888`）、无 sidecar 残留；④ app 正常起窗不卡死。
  - **轮 2（最坏形态 = 黑洞上游，**实测** ≤2s 上界，审计 A5）**：同一隔离根里先种一个激活 profile，`baseUrl` 指向**黑洞主机名**（`http://<不存在的域名或丢弃 SYN 的主机名>`）——**必须用主机名、不要用 IP**：IP 只覆盖 TCP SYN 丢包一条路径，而 libcurl 对**同步 DNS 解析器**是否施加 `CURLOPT_TIMEOUT_MS` 是实现细节（解析挂在解析器里时定时器可能不生效，耗时会长于 2s）；用主机名才能把这条路径真正暴露出来，否则「2s 有界」只是纸面结论。**测量口径（要产出数字，不是"应该 <2s"）**：日志行带毫秒时间戳（core/logging.cpp:52-58，`%Y-%m-%d %H:%M:%S.mmm`）——取 `Claude 代理未启动（前置校验未通过）` 行时间戳 **减去紧邻其前的最后一行日志时间戳** = 本次校验引入的阻塞（两行之间只有这个同步校验）；再跑一次 `claudeProxyAutoStart=false` 的基线做对照。**判据**：实测值 ≤ 2000ms（`kProxyPreStartProbeTimeoutMs`）；测试机上有代理环境变量时注意 `http_client.cpp:72` 的 NOPROXY 只覆盖回环地址，黑洞主机名可能走系统代理——测量时清掉 http_proxy/ALL_PROXY 再跑，两次结果都记。**破界的处置**：>2s 即 D-1 的「有界同步」论据不成立，不得含糊过去——升级走 D-1 后备路径（自启推迟到 claude 模块首帧 compose，复用后台线程 + 收割），或按实测值重定常量并说明依据。**两轮实测数字写进 T3 Step 7 报告 + CHANGELOG**（验收证据）。**严禁 `pkill -f DevDesk`——只杀自己起的 PID**
- [ ] **Step 4: 手动路径冒烟**：同一隔离数据根起 app → Claude 页点「启动代理」（空库）→ 出现「正在校验」→ 1 帧内转「启动代理失败 + 前置校验未通过（1/2 激活 Profile）」，界面全程可交互（点侧边栏切页验证不冻结）；再建 profile + 启用（若可达上游）→ 点按钮 → 「代理已启动 监听 http://127.0.0.1:8888」
- [ ] **Step 5: 文档口径**（审计 A4：**唯一落点 = `CHANGELOG.md`**）：新增 `## [ProxyPreStart] 2026-10-01` 段——① 事故背景（空 profile store 起代理 → 8888 全量 503 → 本机推理链路被打死）；② 三层落点（启动自启 / 会话内惰性装配 / 手动按钮；含 **A1**：`startProxy` 在 `harvestTestResult` 的 UI 线程收割点执行）；③ 校验口径（两项检查 + 零副作用失败 + 2000ms 上界 + T3 Step 3 两轮**实测数字**）；④ 实测 `Total / Passed` 数字；⑤ 已知边界：(a) 启动期校验为有界同步阻塞，实测 ≤2s；(b) 设置页改端口重启不做校验（范围外，理由见 T2 末段）；(c) `reapplyTakeOver()` 仍是零门控接管入口（注释口径，见 A2 / 残差风险 ④）。
  **原稿另指向 `docs/KNOWN_LIMITATIONS.md`——该文件在 dev 上不存在，此步骤删除**（2026-10-01 核实：`ls docs/KNOWN_LIMITATIONS.md` → No such file；`docs/` 实际只有 ARCHITECTURE.md / UI-UX-DESIGN.md / USER_GUIDE.md / EUI-NEO-CAPABILITIES.md 等）。**不新建该文件**（新建一份只有本计划条目的限制清单 = 假文档）；用户可见的已知边界一律随 `CHANGELOG.md` 段落走，本计划的边界登记在上条 ⑤。
- [ ] **Step 6: 装机**：合并进 dev 后按「编译 = 编译 + 复制到 `/Applications`」重建安装（主会话执行）
- [ ] **Step 7: 汇报**：Total/Passed 对比基线 + **7 条新用例**逐条列名 + Review Focus 5/5 落钉结果 + 锚点差异（含 A6 回填）+ 决策 D-1 + **T3 Step 3 两轮实测启动阻塞数字**（含 ≤2s 判定）

## Self-Review 记录

- **需求覆盖**：用户原文两项 → 检查 1（T1 Step 1 用例 ①②）+ 检查 2（用例 ③④⑤）；「没问题才能开启」→ T2 三条路径接线；「不能接管 settings.json」→ 零副作用断言（T1 用例 ②，机制依据 claude_service.cpp:850-852 的无 profile 接管分支 + :507-517 的接管只在 bind 成功后）。
- **占位符扫描**：无 TBD、无「以实际为准」；唯一保留的实现期核对点 = `TempSyncPaths` 的成员名（已给文件:行与既定语义，且明示「按实际改写测试、不改语义」），其余全部有可编译的确切代码与锚点行号。
- **类型一致性**：`validateProxyPreconditions(int probeTimeoutMs = kProxyPreStartProbeTimeoutMs)` 在 T1 声明/实现/T2 三处调用点签名一致；`probeProfile(const Profile&, int)` 私有、`testConnection(const Profile&)` 公开签名不变；`startClaudeProxy(bool notifyFailure = false)` 的默认值保证启动期调用点零改动；T2 新增成员 `bool testResultStartProxy_ = false;`（与 `testResultTitle_/Msg_/IsError_` 同组、同发布序——后台线程单写、join 后 UI 线程读）。
- **Review Focus 落钉**：5/5（#1→T2 Step 1 用例 + T2 Step 4 的单点覆盖论证；#2→T1 用例 ②；#3→T2 Step 6 用例 3b（不冻结 UI + 不起代理）+ **3c（A1 反方向：不收割不 bind）** + 既有 async 用例零回归；#4→T1 用例 ⑤；#5→T2 Step 1 的更新清单 + Step 7 全量）。
- **残差风险（如实登记）**：① `startProxy` 保持不做校验 = 契约靠文档 + 调用点纪律，将来若有第四条起代理路径，编译器不会拦（缓解：claude_service.h 的 `startProxy` 注释写明「调用方必须先过 validateProxyPreconditions」；T3 Step 5 在 CHANGELOG 留痕）。② 真 main.cpp 的接线只由 T3 Step 3/4 的真机冒烟覆盖（devdesk_tests 不链 main.cpp 是既有限制，app 层用例钉的是同源桩）。③ 校验与 `startProxy` 之间非原子：极端情况下 profile 在校验通过后被删（同一进程内的并发写），代理仍会起——可接受（下一轮校验/切 profile 会收敛），不在本计划处理。④ **`reapplyTakeOver()` 是第二个零门控接管入口**（claude_service.h:156，函数体就是 `return syncTakeOver();`）：它不做任何前置校验，无激活 profile 时同样落到 syncTakeOver 的无 profile 分支（claude_service.cpp:845-873）接管。今日**全仓无调用方**（纯公开面），但入口已经存在且语义是「立即接管」——将来若有人拿它当「起代理 / 恢复代理」用，就绕开了本计划的三条路径。缓解：T1 **Step 3b 在声明处补口径注释**（调用方必须先过 `validateProxyPreconditions`，或明确只是在「片段保存 / 切 profile 后重写」——此时接管本就在位），T3 Step 5 的 CHANGELOG 记为已知边界；**不改行为 / 不加门控**（无调用方时加门控 = 无测试可钉的猜测式改动，且会误伤片段保存路径）。

### 锚点核实差异（相对控制器原话）

| 控制器原话 | 实际 | 影响 |
|---|---|---|
| `startClaudeProxy`「main.cpp:307-326」 | **main.cpp:328-347**（惰性装配计划已把装配段拆出，整段下移 ~21 行） | 无——语义判断不变（早退 + startProxy 两点） |
| `testConnection`「claude_service.cpp:421 附近」 | **claude_service.cpp:429-466**（`req.timeoutMs = 15000` 在 :453） | 无——但确认了硬编码 15s 必须拆 `probeProfile` 才能拿到短超时 |
| 「两条既有启动路径」 | 实际是**一个函数两个调用点**（main.cpp:373-374 / :414-419 都经 `startClaudeProxy`） | 接线量比预期小：一处改动覆盖两条；但**验证时必须显式证明覆盖了两条**（Review Focus #1） |
| 「failing connectivity → fail via injected failing transport」 | 仓内**无** HTTP transport 注入缝（`platform::http::send` 是自由函数，http_client.h 无 setter） | 测试用「baseUrl 指向拒绝连接端口」（:1177 既有模式）+ 本地 mock 上游（401 / 慢响应）表达失败与超时 |
| 「connectivity test ... on a background thread before startProxy」 | **部分采纳**：联网段进后台线程**仅限手动按钮**路径，且**只有校验体**进线程——`startProxy` 回 UI 线程收割点（2026-10-01 审计 **A1**，必须项，见下方回填表）。**启动期不采纳**（决策 D-1，理由含 main.cpp 不在测试链接面 + exit hook 时序） | 启动期 = 有界同步（≤2s）+ LOG-only；会话内 toggle 同理（同一函数）；手动 = 线程只跑探测，bind + 接管留在 UI 线程 |
| 设置页「随应用启动」开关 | 只写盘（settings_module.cpp:380-387「下次启动生效」），不是启动路径 | 不在三条路径内 |
| 未提及 | **`syncTakeOver` 无激活 profile 仍接管**（claude_service.cpp:850-852）是事故的真正落点之一，且被 `test_settings_sync.cpp:744-758` 钉着 | 决定**不把校验放进 startProxy**（否则打破既有契约 + 全部直接起代理的用例） |

**2026-10-01 方案审计修订回填（A1–A6；A1 = 必须项，其余 = 建议项）**——控制器对第一版计划的审计，逐条在 dev（HEAD `5a21264`）上复核后处置：

| 审计项 | 复核结论 | 处置落点 |
|---|---|---|
| **A1（必须）`startProxy` 不得进后台线程** | **成立**：`settings_takeover.h:6-7` 明写「仅 UI 线程（现状同款）」；`startProxy` 成功即 `syncTakeOver`（settings.json + sidecar + `warnings_`），与 `onStop → stopProxy → syncRestore`（claude_module.cpp:364-374，同为 UI 线程且**不 join `testThread_`**）及 settings_module 读 `lastSyncWarnings()`（:681）/`isClaudeCodeManaged()`（:1037）并发 = 文件级竞态。既有 `doTestConnection` 线程安全（只跑 `testConnection`：纯 HTTP + 只读）——手动启动是**第一处**「后台线程 + 接管写」共处的调用点（接管写留在 UI 收割点） | 改 T2 Step 5：线程**只**跑 `validateProxyPreconditions()`，`startProxy` 移入 `harvestTestResult()`（claude_module.cpp:781-787，join 之后）；新增成员 `testResultStartProxy_`（单写者/单读者，join 即 happens-before，无需 atomic）；Step 6 新增用例 3c 反向咬（静置段 `isProxyRunning()==false`，收割帧后才 bind）；Global Constraints / Review Focus #3 / Architecture / 锚点表同步改写 |
| A2 `reapplyTakeOver()` 零门控接管入口 | **成立**：claude_service.h:156 `return syncTakeOver();`，无校验、今日无调用方；无激活 profile 时同样接管（:845-873） | T1 Step 3b 补注释（不改行为）+ 残差风险 ④ + T3 Step 5 记已知边界 |
| A3 启动期「无 notice 宿主」不成立 | **成立**：`app::showAppNotice` 是**进程级**状态（app_sidebar.cpp:69-71），启动期调用同样能渲染，原论据为假 | D-1 理由改写为 **UX**（后台动作、无上下文失败窗 = 打扰）；Global Constraints 与 main.cpp 注释段同步订正；结论（LOG-only）不变 |
| A4 T3 Step 5 指向 `docs/KNOWN_LIMITATIONS.md` | **成立**：该文件在 dev 上**不存在**（`ls docs/KNOWN_LIMITATIONS.md` → No such file；`docs/` 只有 ARCHITECTURE / UI-UX-DESIGN / USER_GUIDE / EUI-NEO-CAPABILITIES / mockups / screenshots / superpowers 等） | 步骤改写为**唯一落点 `CHANGELOG.md`**，明写不新建该文件；边界随 CHANGELOG 段落登记 |
| A5 T3 Step 3 缺「≤2s」实证 | **成立**：原步骤未产出数字，且「IP 黑洞」测不到 DNS 那条路径 | 改用**黑洞主机名**（非 IP，暴露同步 DNS 解析器是否被 `CURLOPT_TIMEOUT_MS` 覆盖的未知面）+ 日志毫秒时间戳（core/logging.cpp:52-58）测「校验引入的阻塞」并与不校验基线对照；判据 ≤2000ms，破界即走 D-1 后备路径；实测数字入 T3 Step 7 报告与 CHANGELOG |
| A6 锚点漂移 | **成立**（逐条实测）：① `settings_module.cpp` 端口重启段实际 **:419-465**，`startProxy` 在 **:448**、`isProxyRunning` 早退在 **:440-446**（原稿 :395-441 / :424 均错；审计给的上界 :443-447 也差 3 行——`:443-444` 是早退分支内的 `LOG_INFO`，本计划按实测 :440-446 写）② `cleanupProfileKey` → **`test_support::cleanupProfileKey`**（test_support.h:517；test_claude_service.cpp :45-49 已 `using test_support::…`、:172 起即用此名）③ 桥类型别名在 **module_toggle_bridge.h:25**（原稿 :26-27 指的是 `setClaudeAssemblyHandler` 声明行） | 三处锚点就地改写（T2 锚点表 ×2 + T2 末段范围外理由 + T1 Step 1 工具清单），本条即审计留痕 |
