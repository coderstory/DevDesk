# TIN-06 测试 scaffold 迁移（收尾波）：10 文件残留 ~165 用点 → test_support::

> 状态：**设计稿**。主会话 review + 独立审计 + 8 项【待拍板】拍板后再派开发。只动测试 scaffold，不动用例语义、不动生产代码。
> 前置：TIN-06（首批）+ TIN-06b（第二批）已把共享头 `tests/test_support.h` 收编七件套，且已拍板「构造失败即抛 + ready 兼容」失败口径（`test_support.h:21-36`，受影响的 test_home / test_todo_service / test_todo_ui / test_theme_hierarchy 本批已复跑全绿）。本波是其遗留清理 + 三份逐字重复 frame 驱动合流。

## 1. 问题（实证）

`tests/test_support.h` 七件套已共享（setEnvVar/SrcopedEnv、TempDir、AppDataEnv、ScopedAppRouter、tempDbPath/TestDb/TestEnv、MdHeadlessEnv、writeTextFile/tempConfigFile/cleanupProfileKey），但 10 个测试文件仍有 ~165 用点裸 scaffold（用点口径见下表注；原计数把 def/ctor/dtor/copy 定义体与注释行算进用点，审计三已重算）：

| 文件 | 残留 scaffold（file:行） | 用点（实测 grep -c） | 性质 |
|------|--------------------------|----------------------|------|
| `test_markdown.cpp` | FocusEnv :2599 | 2（1 def + 1 用 :2626） | headless 帧驱动 |
| `test_markdown.cpp` | MdLiveEnv :2833 | 33（32 用） | headless 帧驱动 |
| `test_md_live_spike.cpp` | SpikeEnv :97 | 8（7 用） | headless 帧驱动 |
| `test_md_live_spike.cpp` | SpikeScrollPage :392 | 3（2 用） | 单文件专属 |
| `test_settings.cpp` | PruneEnv :75 | 25（21 用） | cfg + 库 + appDb + 环境变量 |
| `test_streaming_proxy.cpp` | TestEnv :286 | 11（8 用） | `:memory:` 库 + ClaudeService glue |
| `test_streaming_proxy.cpp` | AppDbScope :304 | 7（2 用） | appDatabase scope guard |
| `test_streaming_stress.cpp` | StressEnv :343 | 5（4 用） | `:memory:` 库 + ClaudeService glue（静默降级 :347） |
| `test_claude_ui.cpp` | TestEnv :53 | 25（24 用） | TestDb 基座 + ClaudeService glue（基座已共享） |
| `test_claude_ui.cpp` | ScopedDataRoot :73 | 6（1 用） | DEVDESK_DATA_ROOT 重定向 |
| `test_claude_service.cpp` | ScopedDataRoot :117 | 9（4 用） | 同上，第二份 |
| `test_todo_service.cpp` | TestEnv :21 | 26（25 用） | 已 subclass 共享 TestEnv，剩 TodoService glue |
| `test_todo_ui.cpp` | TestEnv :40 | 34（33 用） | 已 subclass 共享 TestEnv，剩 TodoService + statePath glue |
| `test_pet.cpp` | 本地 writeTextFile :1356 | 4（3 用 :1417/:1435/:1439） | 共享头已有同款（:363） |

> 注：用点计数口径 = 待改写的真实构造/访问点（def/ctor/dtor/copy 定义体、注释行、迁移后无需改写的成员访问如 `root.dir` 不计；N 为实测 grep -c）。原计数把虚计算进用点，审计三重算：ScopedDataRoot 8→4、claude_ui ScopedDataRoot 5→1、AppDbScope 6→2、streaming TestEnv 10→8、PruneEnv 24→21。

三个结构性问题：

1. **三份逐字重复的 frame 驱动**：FocusEnv / MdLiveEnv / SpikeEnv body 一字不差（`core::dsl::Runtime rt` + `eui::Ui* ui` + `frame()` 调 `rt.compose(pageId, 1600.0f, 1000.0f)`），仅 pageId 不同（`"mdfocus"` / `"mdlive"` / `kPage`=`"mdspike"`）。pageId 不是装饰：state / 焦点 / 元素 id 都以 pageId 为命名空间前缀，换名即换命名空间（`test_markdown.cpp:2830-2832` 注释）——三合一必须把 pageId 参数化。
2. **ScopedDataRoot 两份析构语义已漂移**：claude_ui 版析构`setEnvVar("DEVDESK_DATA_ROOT", "")`（清空，:83-87）；claude_service 版析构`hadPrev ? prev : ""`（快照还原，:133-137）。两份构造逐字相同（`tempDbPath(kDbPrefix, tag)+".data"` + remove_all + setEnvVar + create_directories）。同一语义两套行为 = TIN-07 温床（头注释 :2-5 的靶子本身）。
3. **三处静默降级与 TIN-06b 已定口径冲突**：PruneEnv 构造 `if (r.isFail()) return;` / `migrate 失败 db.reset() + return`（:88-93，对象存在但 db 为 null）；streaming TestEnv 构造 `if (dbResult.isOk())`（:291-296，失败 service 为 null，用例靠 `DOCTEST_REQUIRE(env.service != nullptr)` 判红）；StressEnv::start `if (dbResult.isFail()) return false;`（test_streaming_stress.cpp:347，失败 service 为 null）。TIN-06b 已统一「构造失败即抛」（`test_support.h:21-36`）——存量三处只剩这三处没收敛。

## 2. 方案

### 2.1 迁移原则（沿用既有约定）

- 只删本地重复脚手架 + 改 include / 调用，**不动用例语义**（`test_support.h:18` 迁移约定）；每个分支逐字保留原样（同 `test_support.h` 头部「原 XXX 同款」出处注释惯例）。
- 失败口径：新 helper 全部「构造即抛」（TIN-06b 既定 policy 的执行，不是新决策）；保留 ready 兼容成员，既有 `DOCTEST_REQUIRE(env.ready)` 断言行零改动（`test_support.h:35-36, 295-297`）。「用例判红」→「构造抛异常」记账等价（`test_support.h:30-34`）。
- 文件名/名前缀不变量保留（如 PruneEnv 的 `devdesk_test_settings_` 前缀、ScopedDataRoot 的 `kDbPrefix`），便于辨认残骸归属（MdHeadlessEnv 合流注释同款惯例，`test_support.h:320-321`）。
- 新 helper 落位不扩张 include 面（§2.4 决策 1/2/3/5）。

### 2.2 新增 helper 设计（4 个新 + 1 个三合一）

**A. `HeadlessFrame`（tests/frame_env.h，新建 header-only）** —— FocusEnv/MdLiveEnv/SpikeEnv 三合一：

```cpp
// 无头帧驱动：唯一入口是 Runtime::compose（requestFocus 的帧末消费端在
// Runtime::compose，plain Ui 不消费挂起请求）。pageId 恒定——state/焦点/
// 元素 id 都以 pageId 起头做命名空间，换名等于换命名空间。
struct HeadlessFrame {
    core::dsl::Runtime rt;
    eui::Ui* ui = nullptr;
    std::string pageId;
    explicit HeadlessFrame(std::string pageId) : pageId(std::move(pageId)) {}
    template <typename Fn>
    void frame(Fn&& body) {
        rt.compose(pageId, 1600.0f, 1000.0f,
                   [&](eui::Ui& u, const eui::Screen&) {
                       ui = &u;
                       body(u);
                   });
    }
};
```

替换：`FocusEnv env2;` → `test_support::HeadlessFrame env2("mdfocus");`；SpikeEnv 用点传 `kPage` 字面量。frame_env.h 独立于 test_support.h（§2.4 决策 1）。

**B. `ScopedAppDatabase`（test_support.h）** —— AppDbScope 收编，ScopedAppRouter 镜像（`test_support.h:229-237` 同构）：

```cpp
// RAII：作用域内登记/release 进程级 appDatabase（与 ScopedAppRouter 同构）。
// 原 test_streaming_proxy.cpp 的 AppDbScope 收编。
class ScopedAppDatabase {
public:
    explicit ScopedAppDatabase(core::data::SqliteDb& db) {
        ::core::registerAppDatabase(&db);
    }
    ~ScopedAppDatabase() { ::core::registerAppDatabase(nullptr); }
    ScopedAppDatabase(const ScopedAppDatabase&) = delete;
    ScopedAppDatabase& operator=(const ScopedAppDatabase&) = delete;
};
```

**C. `DataRootEnv`（test_support.h）** —— ScopedDataRoot 两份合流，AppDataEnv（:185-220）的 DEVDESK_DATA_ROOT 变体；析构语义统一为**快照还原**（claude_service 版，§2.4 决策 3）：

```cpp
// RAII：DEVDESK_DATA_ROOT 重定向到临时 .data 目录（core/paths.cpp 的统一覆盖
// 入口，绝不碰真实 %APPDATA%/DevDesk）。析构删目录 + 还原进入前值。
// 原 test_claude_ui.cpp / test_claude_service.cpp 两份 ScopedDataRoot 合流，
// 析构语义统一为快照还原（claude_ui 版原为清空，见 §1）。
// 等价前置：外部环境未预设非空 DEVDESK_DATA_ROOT（预设时「清空」与「还原」不等价）。
struct DataRootEnv {
    std::string dir;
    std::string prev;
    bool hadPrev = false;
    explicit DataRootEnv(const std::string& prefix, const std::string& tag) {
        dir = tempDbPath(prefix, tag).string() + ".data";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (const char* p = std::getenv("DEVDESK_DATA_ROOT")) {
            hadPrev = true;
            prev = p;
        }
        setEnvVar("DEVDESK_DATA_ROOT", dir);
        std::filesystem::create_directories(dir, ec);
    }
    ~DataRootEnv() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        setEnvVar("DEVDESK_DATA_ROOT", hadPrev ? prev : "");
    }
    DataRootEnv(const DataRootEnv&) = delete;
    DataRootEnv& operator=(const DataRootEnv&) = delete;
};
```

**D. `ScopedConfigPath`（test_support.h）** —— PruneEnv 的 cfg 段（:78, :95, :99）抽出的通用 RAII（§2.4 决策 4）：

```cpp
// RAII：DEVDESK_CONFIG_PATH 指向临时 config.json 路径，析构清空环境变量 +
// 删文件（cfg 文件由用例自行写内容，同原 PruneEnv 语义）。原 test_settings.cpp
// PruneEnv 的 cfg 段收编。
struct ScopedConfigPath {
    std::filesystem::path cfgPath;
    explicit ScopedConfigPath(std::filesystem::path p) : cfgPath(std::move(p)) {
        setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());
    }
    ~ScopedConfigPath() {
        setEnvVar("DEVDESK_CONFIG_PATH", "");
        std::error_code ec;
        std::filesystem::remove(cfgPath, ec);
    }
    ScopedConfigPath(const ScopedConfigPath&) = delete;
    ScopedConfigPath& operator=(const ScopedConfigPath&) = delete;
};
```

**E. `InMemoryDb`（落位见 §2.4 决策 5）** —— streaming TestEnv 的 `:memory:` 库形态收编；SqliteDb 构造失败即抛、需 unique_ptr 持有形态（TestDb 注释 :259-262 同款关系）：

```cpp
// RAII：SqliteDb::open(":memory:") 的内存库（供不需要 migrate 的纯内存
// 形态——如 ClaudeService 测试）。构造失败即抛。原 test_streaming_proxy.cpp
// TestEnv 的 dbResult 段收编。
struct InMemoryDb {
    std::unique_ptr<core::data::SqliteDb> db;
    InMemoryDb() {
        auto r = core::data::SqliteDb::open(":memory:");
        if (r.isFail()) {
            throw std::runtime_error("SqliteDb::open(:memory:) failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::data::SqliteDb>(std::move(r.value()));
    }
    core::data::SqliteDb& handle() { return *db; }
};
```

`handle()` 命名对齐 TestDb::handle()（`test_support.h:287`）；streaming 7 个 `dbResult.value()` 站点（:1382/:1387/:1388/:1414/:1462/:1463/:1489，其中 :1382/:1459 为 AppDbScope 构造实参）→ `env.db.handle()`；9 处 `env.svc()`（:731/:774/:815/:857/:1070/:1217/:1249/:1396/:1471）成员名随 glue 原样保留；8 处 `env.service != nullptr` 为显式改写项（§3，失败语义不变）。

### 2.3 收编边界（keep-local 白名单，§2.4 决策 6）

共享头禁依赖 `module_*` 头（依赖规则：module 之间不可互依、共享头同理）。以下 glue 就地保留并加 `// 模块 glue，keep-local（TIN-06）` 注释标注：

- `test_claude_ui.cpp:53-65` TestEnv —— 基座已是 `test_support::TestDb`，剩 ClaudeService 构造 + `setGlobal`/析构解除（:56-64）
- `test_todo_service.cpp:21-28` TestEnv —— 共享 TestEnv 基座 + TodoService 构造
- `test_todo_ui.cpp:40-56+` TestEnv —— 共享 TestEnv 基座 + TodoService + statePath 环境变量注入（:44-52）
- `test_md_live_spike.cpp:392` SpikeScrollPage —— 单文件专属（2 用点）
- `test_streaming_proxy.cpp` StreamEnv —— 本地 TestEnv（:286）收编后剩形：InMemoryDb 基座 + ClaudeService 构造 + `service`/`svc()`/`db` 存取器。成员名是 8+9+7 处用点的契约，不得改名：
```cpp
// 模块 glue，keep-local（TIN-06）：InMemoryDb 基座 + ClaudeService 构造。
struct StreamEnv {
    test_support::InMemoryDb db;
    std::unique_ptr<module::claude::ClaudeService> service;
    StreamEnv() {
        service = std::make_unique<module::claude::ClaudeService>(db.handle());
    }
    module::claude::ClaudeService& svc() const { return *service; }
};
```
用点映射：8 处构造（:727/:770/:811/:853/:1066/:1213/:1380/:1457）`TestEnv env;` → `StreamEnv env;`；9 处 `env.svc()`（:731/:774/:815/:857/:1070/:1217/:1249/:1396/:1471）原样；8 处 `env.service != nullptr` 断言删除（service 构造即抛恒非空，判红等价上移，`test_support.h:30-34`）；7 处 `env.dbResult.value()` → `env.db.handle()`。
- `test_streaming_stress.cpp:343` StressEnv —— 同型驻留 glue（InMemoryDb 基座 + ClaudeService + ProxyRouter 构造），4 用点（:714/:788/:845/:930）原样；`start()` 的 :347 静默降级（`if (dbResult.isFail()) return false;`）随 InMemoryDb 构造即抛收敛（§2.4 决策 8）。

### 2.4 待拍板 8 项（推荐在前，主会话拍板）

| # | 决策点 | 推荐 | 备选 |
|---|--------|------|------|
| 1 | HeadlessFrame 落位 | **独立 `tests/frame_env.h`**——test_support.h 收它只需新增 `core/dsl_runtime.h` 一个头（`eui::Ui` 已由既有传递链存在于 test_support.h 面：test_support.h:42 `core/router.h` → `module_interface.h:6-7` `#include <eui/dsl.h>`，无需为 eui::Ui 再增头）；但 dsl_runtime.h 重、带 platform/input/render/runtime 全家，include 面仍扩张（research 倾向同此）；header-only，tests/ 已在 target_include_directories（`test_support.h:7-8` 同款），零 CMake 改动 | 塞 test_support.h（收敛一处但 include 面扩大） |
| 2 | ScopedAppDatabase | **进 test_support.h**——`core::registerAppDatabase` 声明已在 `core/database.h`（test_support.h:41 已 include），零新增头；与 ScopedAppRouter 对称命名 | 塞 frame_env.h（语义不符） |
| 3 | DataRootEnv 析构语义 | **统一「快照还原」**（claude_service 版）——嵌套安全、与 AppDataEnv 的还原惯例一致；claude_ui 1 用点（:2409 构造）行为从清空变还原；等价前置：外部环境未预设非空 DEVDESK_DATA_ROOT（shell/CI 预设时「清空」把预设值丢成空串、「还原」归还预设值——两版不等价；正常开发机未预设）+ 用例间无嵌套 DEVDESK_DATA_ROOT，行为等价 | 统一「清空」（claude_ui 版，破坏其他用例预置的 DATA_ROOT） |
| 4 | PruneEnv 重构 | **组合式**：`TestEnv(prefix, tag, /*registerAppDb=*/true)` + `ScopedConfigPath(tempConfigFile(...))`，本地类删除；「构造失败不抛」按 TIN-06b 既定 policy 收敛到抛（§2.1） | 本地瘦身（PruneEnv 缩成 3 行 glue 保留类名——21 用点不动但本地类仍在，与验收「零裸 scaffold」冲突） |
| 5 | InMemoryDb 落位 | **进 test_support.h**——SqliteDb 头面已具备（同决策 2）；ClaudeService 内存库形态后续测试可复用；构造即抛无状态风险 | 留在 test_streaming_proxy.cpp keep-local（:memory: 仅单文件用，保守） |
| 6 | 收编边界 | **§2.3 白名单 keep-local**（module glue ×3 + streaming/stress 驻留 glue StreamEnv/StressEnv + SpikeScrollPage），验收 grep 排除标注项 | 全量收编（破坏共享头禁 module_* 依赖，不成立） |
| 7 | test_pet writeTextFile | **机械迁移**：删本地 void 版（:1356），3 用点换 `test_support::writeTextFile`（:363，bool 返回）；实现时核对调用点是否忽略返回值（void→bool 兼容） | 保留本地版（与共享头重复，违背收编目标） |
| 8 | 口径统一 | **新 helper 全部「构造即抛」**（TIN-06b 已拍板的 policy，本波执行）；存量三处静默降级（PruneEnv :88-93、streaming TestEnv :291-296、StressEnv :347）一并收敛到抛；ready 兼容保留（`test_support.h:35-36`） | 保留 isOk() 降级形态（维持「存在但不可用」态，与 policy 冲突） |

## 3. 改动清单

| 文件 | 迁移动作 | 目标 helper |
|------|----------|-------------|
| `tests/frame_env.h`（**新建**） | 新增 HeadlessFrame（§2.2-A） | — |
| `tests/test_support.h` | 新增 ScopedAppDatabase / DataRootEnv / ScopedConfigPath（/ InMemoryDb，按决策 5） | — |
| `test_markdown.cpp` | 删 FocusEnv（:2599）、MdLiveEnv（:2833）；include `../frame_env.h`；1 + 32 用点换 HeadlessFrame（`"mdfocus"` / `"mdlive"`） | frame_env.h::HeadlessFrame |
| `test_md_live_spike.cpp` | 删 SpikeEnv（:97）；7 用点换 HeadlessFrame（`kPage`）；SpikeScrollPage（:392）keep-local 标注 | 同上 |
| `test_settings.cpp` | 删 PruneEnv（:75）；21 用点拆两行 `TestEnv env("devdesk_test_settings_", tag, true)` + `ScopedConfigPath cfg(tempConfigFile("devdesk_test_settings_", tag + "_cfg"))`；cfg 前缀不变量保留（:81-84）；:1184 注释「读 PruneEnv 指向的临时 config」更新为指向新组合（TestEnv + ScopedConfigPath，§2.1 出处注释惯例） | test_support::TestEnv（既有）/ ScopedConfigPath |
| `test_streaming_proxy.cpp` | 删本地 TestEnv（:286）与 AppDbScope（:304）；驻留 glue 更名 StreamEnv（§2.3，基座 `test_support::InMemoryDb`）；8 用点 `TestEnv env;` → `StreamEnv env;`；7 个 `dbResult.value()`（:1382/:1387/:1388/:1414/:1462/:1463/:1489）→ `env.db.handle()`；2 处 `AppDbScope appDb(...)`（:1382/:1459）→ `ScopedAppDatabase`；8 处 `env.service != nullptr` 断言删除（glue 构造即抛后恒真，判红语义等价上移，`test_support.h:30-34`）；9 处 `env.svc()` 原样 | InMemoryDb / ScopedAppDatabase（+ 驻留 StreamEnv） |
| `test_streaming_stress.cpp` | StressEnv（:343）keep-local 标注；`dbResult` 段收编为 `test_support::InMemoryDb`；:347 静默降级随构造即抛收敛（决策 8）；4 用点（:714/:788/:845/:930）原样 | InMemoryDb（+ 驻留 StressEnv） |
| `test_claude_ui.cpp` | 删 ScopedDataRoot（:73）；1 用点（:2409）换 DataRootEnv（保留 kDbPrefix）；:2498 `root.dir` 不变（DataRootEnv 保留同名 `dir` 成员）；TestEnv（:53）keep-local 标注 | DataRootEnv |
| `test_claude_service.cpp` | 删 ScopedDataRoot（:117）；4 用点（:2730/:2812/:2853/:2917）换 DataRootEnv（保留 kDbPrefix）；3 处 `root.dir`（:2775/:2844/:2929）不变（同名成员） | DataRootEnv |
| `test_todo_service.cpp` | TestEnv（:21）keep-local 标注，零代码改动 | — |
| `test_todo_ui.cpp` | TestEnv（:40）keep-local 标注，零代码改动 | — |
| `test_pet.cpp` | 删本地 writeTextFile（:1356）；3 用点（:1417/:1435/:1439）换 `test_support::writeTextFile`；:69 注释「test_settings.cpp PruneEnv 同款约定」改指新组合（TestEnv + ScopedConfigPath，原 PruneEnv 名已移除法） | test_support::writeTextFile（既有） |

## 4. 验收

1. **零裸 scaffold**：`grep -rnE "struct (TestEnv|PruneEnv|AppDbScope|ScopedDataRoot|FocusEnv|MdLiveEnv|SpikeEnv|StreamEnv|StressEnv)\b" tests/test_modules/` 仅剩 keep-local 标注的 5 处（test_claude_ui:53 / test_todo_service:21 / test_todo_ui:40 的 TestEnv、test_streaming_proxy.cpp 的 StreamEnv、test_streaming_stress.cpp 的 StressEnv 定义，均含「keep-local（TIN-06）」注释）；`grep -rn "PruneEnv\|AppDbScope\|ScopedDataRoot\|FocusEnv\|MdLiveEnv\|SpikeEnv" tests/test_modules/` 命中 0（keep-local 标注例外：SpikeScrollPage / StreamEnv / StressEnv）。
2. **零回归**：定向测试各功能子串全绿（`devdesk_tests.exe markdown` / `settings` / `streaming` / `claude` / `todo` / `pet`）；既有 `DOCTEST_REQUIRE(env.ready)` 断言行零改动——范围限定：TIN-06b 四文件（test_home / test_todo_service / test_todo_ui / test_theme_hierarchy）+ keep-local 3 处 TestEnv（claude_ui:53 / todo_service:21 / todo_ui:40）；streaming 8 处 `env.service != nullptr` 为显式改写项（§3：删除——glue 内 service 构造即抛恒非空，失败语义不变）；全量收在主会话里程碑跑一次（基线 Total: 646）。
3. **include 面**：`test_support.h` 不新增 `core/dsl_runtime.h` 头（决策 1 生效态；`eui::Ui` 已由 test_support.h:42 → core/router.h → module_interface.h:6-7 传递链存在）；frame_env.h 只被 2 个 headless 文件 include（test_markdown.cpp + test_md_live_spike.cpp）。
4. **口径**：新 helper 构造失败即抛（无「对象在、ready=false」态）；用例语义零改动（分支逐字保留，diff 只含脚手架替换）。

## 5. 风险与未知

| # | 风险 | 验证 / 处置 |
|---|------|-------------|
| 1 | test_pet 本地 writeTextFile 是 void、共享版 bool——调用点若读返回值行为变 | 迁移时核对 3 用点（:1417/:1435/:1439）是否使用返回值；不用则直换等价 |
| 2 | PruneEnv「不抛」→「抛」改变失败呈现（判红 → 未捕获异常） | 记账等价（`test_support.h:30-34`）；test_main 逐用例 catch 不炸套件 |
| 3 | HeadlessFrame pageId 迁移笔误（mdlive↔mdspike 互串） | pageId=命名空间，串名元素 id 查找必 red（`test_markdown.cpp:2830-2832`）；测试自证 |
| 4 | DataRootEnv 统一快照还原改变 claude_ui 1 用点（:2409）析构语义；前置：外部环境未预设非空 DEVDESK_DATA_ROOT（预设时「清空」丢预设值、「还原」归还，不等价） | 正常开发机未预设 + 用例间无嵌套 DEVDESK_DATA_ROOT → 行为等价；若审计发现嵌套/预设场景再议 |
| 5 | InMemoryDb 与 TestEnv(registerAppDb=true) 并存时的 appDatabase 注册冲突（streaming 用点 :1382 只注册不迁移） | 不冲突：ScopedAppDatabase 是独立 scope，与 TestEnv 的 `appDbRegistered` 互不感知；用例内时序与现状一致 |
| 6 | `module::claude::ClaudeService` 构造是否要求已 migrate 的库（streaming TestEnv 是无 migrate `:memory:` 形态） | 现状已在跑（8 用点全绿），InMemoryDb 只收编不改语义；若审计有疑走 keep-local |

## 6. 流程

本 spec（设计稿）→ 主会话 review → 独立审计（重点核 §2.2 四个 helper 与三份 frame 的逐字对应、§2.4 推荐项合理性与备选代价）→ 用户/主会话拍板 8 项 → 派 **1 个开发子代理**（worktree 隔离；机械类约 60%：三合一 + AppDbScope + ScopedDataRoot×2 + writeTextFile；定向测试各功能子串；显式 pathspec 原子 commit）→ 主会话审计 diff + 复跑验收 → 串行合 master → 清 worktree → 全量测试主会话跑一次。