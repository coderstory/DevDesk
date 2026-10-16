// tests/test_modules/test_claude_quota_lifecycle.cpp
// 余量「测试连接」后台线程的生命周期覆盖：① 点击确实起线程并把结果送回 UI；② 点完立刻 onStop + 析构，不崩不 terminate。
//
// 跨文件前提（三条测试纪律，破了本文件就不再零副作用）：
//   · 零真实网络：种子 profile 的 api_key_encrypted 列留空 → loadApiKey 按设计返回 NotFound
//     （「没有密钥」与「有但解不开」必须可分辨，见 profile_repository）→ 弹窗 profileApiKey 为空
//     → provider 的「空 key 是确定性失败且不发请求」早退生效（deepseek_provider.cpp）。断言钉住的
//     就是那条早退文案，只有走了早退分支才会得到它，于是「没出网」本身也成了断言的一部分。
//   · 后台刷新器同样不发请求：种子配置行 enabled = false → isDue 第一道闸就 false → onStart() 起的那条
//     真刷新线程一轮都查不出去。
//   · 不碰用户 Keychain：ClaudeService 显式传测试专用 service 名；种子 profile 没有任何一行存过密钥
//     → 孤儿清理护栏③直接整体跳过，连 list 都不会发生。
#include "doctest.h"
#include "test_support.h"

#include "app_registry.h"
#include "claude_service.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "quota_home_bridge.h"  // devdesk::quota_bridge::setQuotaFeatureEnabled
#include "quota/quota_store.h"
#include "core/event_bus.h"       // kDbImportBegin / kDbImportEnd 注入
#include "quota/quota_dialog_config.h"  // quotaProviderOptions（下拉预填文案单点）

#include <eui/dsl.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace cq = module::claude::quota;

constexpr const char* kDbPrefix = "devdesk_test_quota_lifecycle_";

// 测试专用 Keychain service 名：显式传它，cleanupOrphanKeychainEntries（onStart 第一步）就绝不碰
// 用户真实数据根派生的那个命名空间（DEVDESK_DATA_ROOT 并不隔离 Keychain）。
constexpr const char* kKeychainService = "DevDeskTestClaudeQuotaLifecycle";

// 测试里选中的供应商：必须是注册表里真实存在的一行（findProvider 命中才会起线程），且必须是
// 「空 key 即早退、不发请求」的那类。
constexpr const char* kProviderId = "deepseek";

// 弹窗「测试连接」按钮 / 结果行的节点 id（claude_ui_quota.cpp 的文案单点）。
constexpr const char* kTestButtonId = "claude.dialog.quota.test.bg";
constexpr const char* kResultNodeId = "claude.dialog.quota.result";

int64_t insertProfileWithoutCredential(core::data::SqliteDb& db,
                                       const std::string& name) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    // 刻意不写 api_key_encrypted：取表默认值 '' → loadApiKey 按设计返回 NotFound
    // （「没有密钥」与「有但解不开」必须可分辨）→ 弹窗拿不到 key → provider 早退。
    if (!db.exec("INSERT INTO claude_profiles (name, base_url, api_key_hint, "
                 "api_key_check, model, created_at, updated_at) VALUES ('" +
                 name + "', 'https://example.invalid', 'sk-***', 'chk', 'm', " +
                 std::to_string(now) + ", " + std::to_string(now) + ")")
             .isOk()) {
        throw std::runtime_error("生命周期用例：profile 种子插入失败");
    }
    auto st = db.prepare("SELECT last_insert_rowid() AS id");
    if (!st.isOk()) throw std::runtime_error("生命周期用例：last_insert_rowid 失败");
    auto rows = st.value().fetchAll();
    if (!rows.isOk() || rows.value().size() != 1) {
        throw std::runtime_error("生命周期用例：读不回种子 profile 的 id");
    }
    return rows.value()[0].getInt64("id").value();
}

// 种子：profile（无凭据）+ 一行余量配置（选了 provider，但未启用）
void seedQuotaConfig(core::data::SqliteDb& db, int64_t profileId) {
    cq::QuotaStore store(db);
    cq::QuotaConfigRow cfg;
    cfg.profileId = profileId;
    cfg.providerId = kProviderId;
    cfg.enabled = false;   // ← isDue 第一道闸：后台刷新器一轮都查不出去
    cfg.autoRefresh = true;
    cfg.intervalMinutes = 15;
    if (!store.saveConfig(cfg).isOk()) {
        throw std::runtime_error("生命周期用例：余量配置种子落库失败");
    }
}

// RAII：注册 / 摘掉 ClaudeService 全局单例。
//
// 不能手写 `setGlobal(&svc)` … `setGlobal(nullptr)`：DOCTEST_REQUIRE 宏自带 `return false;`，任何一条中途
// 断言不过就会提前返回，末尾那句摘牌永远执行不到——全局指针就此指向栈上已析构的 svc，后面每一条用例都在
// 读悬垂指针（症状会漂到毫不相干的用例上，难查）。RAII 摘牌不挑返回路径。
struct ScopedGlobalClaudeService {
    explicit ScopedGlobalClaudeService(module::claude::ClaudeService& svc) {
        module::claude::ClaudeService::setGlobal(&svc);
    }
    ~ScopedGlobalClaudeService() { module::claude::ClaudeService::setGlobal(nullptr); }
    ScopedGlobalClaudeService(const ScopedGlobalClaudeService&) = delete;
    ScopedGlobalClaudeService& operator=(const ScopedGlobalClaudeService&) = delete;
};

// RAII：用例期间打开余量功能总开关，出作用域必还原 false。
// 开关关时 onStart() 不启动余量刷新器、也不创建 quotaStore_ → openQuotaDialog 的 `if (quotaStore_)`
// 落空 → 种子的余量配置行读不回 → 弹窗 providerId 为空，弹不到「未配置 API 密钥」。不能手写在用例末尾
// 还原：DOCTEST_* 宏自带 `return false;`，断言早退时那行执行不到，开关会泄漏给同进程后续用例。
struct ScopedQuotaFeatureEnabled {
    ScopedQuotaFeatureEnabled() { devdesk::quota_bridge::setQuotaFeatureEnabled(true); }
    ~ScopedQuotaFeatureEnabled() { devdesk::quota_bridge::setQuotaFeatureEnabled(false); }
    ScopedQuotaFeatureEnabled(const ScopedQuotaFeatureEnabled&) = delete;
    ScopedQuotaFeatureEnabled& operator=(const ScopedQuotaFeatureEnabled&) = delete;
};

void composeFrame(::core::Module& mod, eui::Ui& ui, eui::Screen screen,
                  const char* treeId) {
    ui.begin(treeId);
    mod.compose(ui, screen);
    ui.end();
}

bool clickById(eui::Ui& ui, const std::string& id) {
    const auto* node = ui.find(id);
    if (node == nullptr || node->onClick == nullptr) return false;
    node->onClick();
    return true;
}

bool dialogVisible(eui::Ui& ui) {
    const auto* panel = ui.find("claude.dialog.quota.panel");
    return panel != nullptr && panel->opacity > 0.5f;
}

// 端到端走一遍「onStart → 弹窗打开 → 点【测试连接】」。必须经 Router 负载而不是直接调内部函数：
// `runQuotaTestConnection` 是 ClaudeModule 的私有成员，唯一外部入口就是弹窗按钮的 onClick，这里走的正是
// 生产那条路（consumeQuotaConfigPayload → openQuotaDialog → drawQuotaDialog → 按钮 onClick → runQuotaTestConnection）。
//
// 返回 bool（而不是就地断言）：DOCTEST_* 宏自带 `return false;`，在 void 函数里会触发 -Wreturn-mismatch
// （=-Werror）。调用方用 DOCTEST_REQUIRE_MESSAGE 收口，报错信息留在调用点。
bool openDialogAndClickTest(::core::Router& router, ::core::Module& mod,
                            int64_t profileId, int treeIndex) {
    router.registerModule(&mod);
    router.navigateTo(::core::PageId::ClaudeProxy,
                      "action:quota_config:" + std::to_string(profileId));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(mod, ui, screen,
                 ("quota_lifecycle_open_" + std::to_string(treeIndex)).c_str());
    if (!dialogVisible(ui)) return false;   // 弹窗没真打开
    return clickById(ui, kTestButtonId);    // 按钮不存在 / 没接 onClick
}

}  // namespace

// ① 「测试连接」确实起线程并把结果送回 UI
//
// 这条是 ② 的**非空转前提**：若「点按钮 → 起线程 → 结果回 UI」这条链哪天断了（findProvider 查不到、按钮
// onClick 没接上、线程没跑到 harvest），这里先红，而不是让 ② 靠「反正没崩」持续报绿。
REGISTER_TEST(claude_quota_lifecycle_test_connection_round_trip,
              "claude quota 生命周期: 点【测试连接】起后台线程并把结果送回 UI", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "roundtrip", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();

    const int64_t profileId = insertProfileWithoutCredential(sq, "余量生命周期-回环");
    seedQuotaConfig(sq, profileId);

    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    ScopedQuotaFeatureEnabled quotaOn;   // 必须在 onStart() 前开：开关关时 onStart 不建 quotaStore_，弹窗读不到种子配置行
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    router.registerModule(mod.get());
    router.navigateTo(::core::PageId::ClaudeProxy,
                      "action:quota_config:" + std::to_string(profileId));

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "quota_lifecycle_rt_open");
    DOCTEST_REQUIRE(dialogVisible(ui));
    DOCTEST_REQUIRE_MESSAGE(clickById(ui, kTestButtonId),
                            "【测试连接】必须存在且带 onClick");

    // 收割是 compose 每帧尾部的一次轮询（harvestQuotaTestResult），结果行又在下一帧才画出来
    // ——有界轮询等它落地，绝不靠固定长睡。
    std::string result;
    for (int i = 0; i < 400; ++i) {   // 上界 400 × 5ms = 2s
        eui::Ui frame;
        composeFrame(*mod, frame, screen, "quota_lifecycle_rt_poll");
        const auto* node = frame.find(kResultNodeId);
        if (node != nullptr && !node->text.empty()) {
            result = node->text;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 这串文案**只可能**来自 provider 体内「空 key = 确定性失败且不发请求」那一行
    //（deepseek_provider.cpp）：线程真的跑到了 fetch 并把结果发了出来（不是空转），且走的是发请求
    // 之前的早退（本用例零真实出网）。不可放宽成「只要有非空文案」——那会放过空转与早退被绕过两种退化。
    DOCTEST_CHECK_MESSAGE(!result.empty(),
                          "点【测试连接】后 2 秒内 UI 上没出现任何结果文案："
                          "后台线程要么没起、要么没把结果送回 UI");
    DOCTEST_CHECK_MESSAGE(result.find("未配置 API 密钥") != std::string::npos,
                          "结果文案应为「未配置 API 密钥」（种子 profile 无凭据 → "
                          "provider 发请求前早退），实到：" + result);

    mod->onStop();
    return true;
})

// ② 点完立刻停：join / 析构 / 重启一轮
//
// 守的正是 stopQuotaRefresh() 开头那行 `if (quotaTestThread_.joinable()) quotaTestThread_.join();`。
// 删掉它 → 线程成员析构时仍 joinable → std::terminate → **进程 abort**。这就是「崩」在本测试框架里的
// 表现形式——没有断言可写，崩溃本身就是那条断言。
//
// 与 ① 的分工：① 收割结果（join 在 harvestQuotaTestResult 里发生），本条**刻意不收割**——被测的正是
// stopQuotaRefresh 那条 join 路径，若先收割，线程早被别的路径 join 掉，本条就测不到 stopQuotaRefresh 了。
// 因此「点击确实起了线程」这件事由 ① 负责背书，这里只负责「起了就得收干净」。
REGISTER_TEST(claude_quota_lifecycle_test_connection_then_immediate_stop,
              "claude quota 生命周期: 点【测试连接】后立刻 onStop + 析构不崩不 terminate", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "stop", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();

    const int64_t profileId = insertProfileWithoutCredential(sq, "余量生命周期-急停");
    seedQuotaConfig(sq, profileId);

    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    ScopedQuotaFeatureEnabled quotaOn;   // 覆盖下面全部轮次（含「停了又起」）：onStart 只在开关开时建 quotaStore_

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;

    // 三轮：起模块 → 点测试 → 立刻停 → 析构。重复是为了压时序抖动
    // （线程可能还没跑完就被要求收尾——那正是真实用户「点完马上退出」的场景）。
    for (int round = 0; round < 3; ++round) {
        std::unique_ptr<::core::Module> mod(create_claude_module());
        mod->onStart();
        DOCTEST_REQUIRE_MESSAGE(openDialogAndClickTest(router, *mod, profileId, round),
                                "余量弹窗未打开或【测试连接】不可点（详见 ①）");
        // 不收割、不等待：立刻停机，把「在途测试线程」交给 stopQuotaRefresh。
        mod->onStop();
        mod.reset();   // ← 析构里还会再走一次 stopQuotaRefresh（析构兜底路径）
    }

    // 走完三轮还活着 = 三次「起线程 → 收线程」都干净。
    // 再补一轮「停了又起」，验证 stopQuotaRefresh 之后模块仍可复用
    // （store / credentials 被 reset 过，起得来说明没有留下半初始化的残骸）。
    {
        std::unique_ptr<::core::Module> mod(create_claude_module());
        mod->onStart();
        DOCTEST_REQUIRE_MESSAGE(openDialogAndClickTest(router, *mod, profileId, 99),
                                "余量弹窗未打开或【测试连接】不可点（详见 ①）");
        mod->onStop();
        mod->onStart();   // off → on 循环：余量刷新器必须能重新装起来
        mod->onStop();
    }

    // 恒真断言：只为失败时留下本用例的名字，崩溃本身由框架报告（走到这里即未 terminate）
    DOCTEST_CHECK_MESSAGE(true, "三轮「点【测试连接】→ 立刻 onStop → 析构」未崩溃");
    return true;
})

// ─── db.import 暂停/恢复 × 余量功能总开关门控（claude_module onStart 的 kDbImport* 订阅段） ───
//
// 被测行为：end handler 受双闸门控——订阅存活闸（dbImportHandlersEnabled_，onStop 先关再退订）
// × 功能总开关（quota_bridge::isQuotaFeatureEnabled，默认关）。开关关时导入结束不得复活刷新；
// 开关开时恢复触发 startQuotaRefresh。startQuotaRefresh 的幂等早退（quotaRefresher_ 已在即
// return）无外部观测量（预填缝对双起/不双起同像，变异实测不可判异），不做行为断言。
// 观察缝（行为级，非内部布尔）：quotaStore_ 全模块唯一写点是 startQuotaRefresh()（claude_module.cpp）。
// 余量配置弹窗的供应商预填读 quotaStore_->loadConfig——刷新真的起过 ⇒ 预填种子的 DeepSeek；
// 没起 / 已被拆 ⇒ 预填落「不配置（关闭余量查询）」。断言只认下拉选中文案，不碰任何成员。
// 事件注入复用 test_github_sync_card 的 store 层事件序（begin → end，end 无论成败都发）。
namespace {

// emit 同步派发：返回时全部 handler 已跑完，可直接观察副作用
void emitDbImportBegin() { ::core::event::bus().emit(::core::event::kDbImportBegin); }
void emitDbImportEnd() { ::core::event::bus().emit(::core::event::kDbImportEnd); }

std::string quotaProviderDisplayName(const std::string& id) {
    for (const cq::QuotaProviderOption& o : cq::quotaProviderOptions()) {
        if (o.id == id) return o.displayName;
    }
    return {};
}

// RAII：总开关设为指定值，出作用域恢复**进入前值**（方向不限，防开关值跨用例泄漏）。
// 不能手写还原：DOCTEST_* 宏自带 `return false;`，断言早退时还原行执行不到。
struct ScopedQuotaFeatureFlag {
    bool prev;
    explicit ScopedQuotaFeatureFlag(bool v)
        : prev(devdesk::quota_bridge::isQuotaFeatureEnabled()) {
        devdesk::quota_bridge::setQuotaFeatureEnabled(v);
    }
    void set(bool v) { devdesk::quota_bridge::setQuotaFeatureEnabled(v); }
    ~ScopedQuotaFeatureFlag() { devdesk::quota_bridge::setQuotaFeatureEnabled(prev); }
    ScopedQuotaFeatureFlag(const ScopedQuotaFeatureFlag&) = delete;
    ScopedQuotaFeatureFlag& operator=(const ScopedQuotaFeatureFlag&) = delete;
};

// RAII：离开用例前必 onStop。开关开的 end 路径会起真 QuotaRefresher 后台线程（线程持有
// appDatabase 引用），TestEnv 析构会先注销 appDatabase——线程不先收就是悬垂。onStop 幂等，
// 且持 unique_ptr 指针：用例内 reset() 后析构自动跳过。
struct ScopedModuleStop {
    std::unique_ptr<::core::Module>* mod;
    explicit ScopedModuleStop(std::unique_ptr<::core::Module>* m) : mod(m) {}
    ~ScopedModuleStop() {
        if (mod != nullptr && mod->get() != nullptr) (*mod)->onStop();
    }
    ScopedModuleStop(const ScopedModuleStop&) = delete;
    ScopedModuleStop& operator=(const ScopedModuleStop&) = delete;
};

// 走生产路径打开余量配置弹窗（Router 负载 → compose 消费 → openQuotaDialog），返回供应商
// 下拉的当前选中文案。返回空串 = 弹窗没打开（观察缝失效，调用方 REQUIRE 收口）。
std::string quotaDialogProviderLabel(::core::Router& router,
                                     std::unique_ptr<::core::Module>& mod,
                                     int64_t profileId, int seq) {
    router.registerModule(mod.get());
    router.navigateTo(::core::PageId::ClaudeProxy,
                      "action:quota_config:" + std::to_string(profileId));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(*mod, ui, screen,
                 ("quota_gate_label_" + std::to_string(seq)).c_str());
    const auto* panel = ui.find("claude.dialog.quota.panel");
    if (panel == nullptr || panel->opacity <= 0.5f) return std::string();
    const auto* label = ui.find("claude.dialog.quota.provider.dd.label");
    return label != nullptr ? label->text : std::string();
}

}  // namespace

// ① 总开关关（生产默认态）：db.import_end 不复活刷新
REGISTER_TEST(claude_quota_gate_import_end_blocked_when_switch_off,
              "claude quota 总开关: 开关关→db.import_end 不复活刷新（门控兜底）", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "gateoff", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();
    const int64_t profileId = insertProfileWithoutCredential(sq, "余量门控-关");
    seedQuotaConfig(sq, profileId);

    ScopedQuotaFeatureFlag quotaOff(false);
    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ScopedModuleStop stopAtExit(&mod);
    mod->onStart();

    emitDbImportBegin();
    emitDbImportEnd();

    const std::string none = quotaProviderDisplayName("");
    DOCTEST_REQUIRE(!none.empty());
    const std::string label = quotaDialogProviderLabel(router, mod, profileId, 1);
    DOCTEST_REQUIRE_MESSAGE(!label.empty(),
                            "余量弹窗未打开：观察缝失效（Router 负载路径断了），本用例测不到门控");
    DOCTEST_CHECK_MESSAGE(label == none,
                          "开关关时 db.import_end 不得启动余量刷新（quotaStore_ 不存在 → "
                          "预填应为「不配置」），实到：" + label);
    return true;
})

// ② 总开关开：同一事件触发恢复（startQuotaRefresh 路径被真走到）
REGISTER_TEST(claude_quota_gate_import_end_fires_when_switch_on,
              "claude quota 总开关: 开关开→db.import_end 触发余量刷新恢复", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "gateon", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();
    const int64_t profileId = insertProfileWithoutCredential(sq, "余量门控-开");
    seedQuotaConfig(sq, profileId);

    ScopedQuotaFeatureFlag flag(false);   // onStart 时保持关：起不起只能归因给 end 事件
    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ScopedModuleStop stopAtExit(&mod);
    mod->onStart();

    const std::string none = quotaProviderDisplayName("");
    const std::string deepseek = quotaProviderDisplayName("deepseek");
    DOCTEST_REQUIRE(!none.empty());
    DOCTEST_REQUIRE(!deepseek.empty());

    // 对照基线：开关还关，同样的 end 事件必须不起
    emitDbImportEnd();
    const std::string before = quotaDialogProviderLabel(router, mod, profileId, 1);
    DOCTEST_REQUIRE_MESSAGE(before == none,
                            "开关关时 end 事件不应起刷新（基线被破，后续断言失去归因），实到：" + before);

    // 用户在导入进行中打开总开关 → 下一次 end 恢复刷新
    flag.set(true);
    emitDbImportBegin();   // 暂停（此时本就没起，no-op）
    emitDbImportEnd();
    const std::string after = quotaDialogProviderLabel(router, mod, profileId, 2);
    DOCTEST_REQUIRE_MESSAGE(!after.empty(), "余量弹窗未打开：观察缝失效");
    DOCTEST_CHECK_MESSAGE(after == deepseek,
                          "开关开后 db.import_end 必须触发 startQuotaRefresh（quotaStore_ 建立 → "
                          "预填 DeepSeek），实到：" + after);
    return true;
})

// ③ 导入中途开开关 + begin/end 往返：end 起刷新、begin 真暂停、end 真恢复。
// 刻意不做「幂等不双起」断言：预填缝对幂等早退不敏感（删 quotaRefresher_ 守卫预填同像），
// 双起在本实现里无外部观测量，断了就是恒真假绿——重复 end 只验「不断链仍健康」。
REGISTER_TEST(claude_quota_gate_pause_resume_cycle_mid_import_toggle,
              "claude quota 总开关: 导入中途开开关→end 起刷新；begin 真暂停、end 真恢复", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "gatecycle", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();
    const int64_t profileId = insertProfileWithoutCredential(sq, "余量门控-往返");
    seedQuotaConfig(sq, profileId);

    ScopedQuotaFeatureFlag flag(false);
    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ScopedModuleStop stopAtExit(&mod);
    mod->onStart();

    const std::string none = quotaProviderDisplayName("");
    const std::string deepseek = quotaProviderDisplayName("deepseek");
    DOCTEST_REQUIRE(!none.empty());
    DOCTEST_REQUIRE(!deepseek.empty());

    emitDbImportBegin();       // 导入开始（暂停：本就没起，no-op）
    flag.set(true);            // 导入中途开开关
    emitDbImportEnd();         // 第一次 end：起刷新
    const std::string first = quotaDialogProviderLabel(router, mod, profileId, 1);
    DOCTEST_REQUIRE_MESSAGE(first == deepseek,
                            "第一次 end 必须起刷新（预填 DeepSeek），实到：" + first);

    // 重复 end：不断链（刷新器仍可用、预填仍对）。幂等早退本身在此缝不可判异（见上）。
    emitDbImportEnd();
    const std::string second = quotaDialogProviderLabel(router, mod, profileId, 2);
    DOCTEST_CHECK_MESSAGE(second == deepseek,
                          "重复 end 后刷新链不得断（预填仍 DeepSeek），实到：" + second);

    // begin → end 一整个往返：begin 真拆 store（预填回落「不配置」），end 建新实例再起
    //（stopAll 后旧实例不可重启，startQuotaRefresh 每次建新 QuotaRefresher）
    emitDbImportBegin();
    const std::string paused = quotaDialogProviderLabel(router, mod, profileId, 3);
    DOCTEST_CHECK_MESSAGE(paused == none,
                          "begin 暂停必须真拆刷新器（quotaStore_ reset → 预填回落「不配置」），实到：" + paused);
    emitDbImportEnd();
    const std::string resumed = quotaDialogProviderLabel(router, mod, profileId, 4);
    DOCTEST_CHECK_MESSAGE(resumed == deepseek,
                          "恢复 end 必须能重新装起刷新器（新 QuotaRefresher → 预填 DeepSeek），实到：" + resumed);
    return true;
})

// ④ onStop 后迟到 end：原子闸挡住在途 handler；退订后再发 = no-op；析构无悬空
REGISTER_TEST(claude_quota_gate_late_event_after_on_stop,
              "claude quota 总开关: onStop 后迟到 db.import_end 被原子闸挡住不复活刷新", {
    ::core::Router router;
    test_support::ScopedAppRouter scopedRouter(&router);
    test_support::TestEnv env(kDbPrefix, "gatestop", /*registerAppDb=*/true);
    core::data::SqliteDb& sq = env.db->db();
    const int64_t profileId = insertProfileWithoutCredential(sq, "余量门控-急停");
    seedQuotaConfig(sq, profileId);

    ScopedQuotaFeatureFlag on(true);
    module::claude::ClaudeService svc(sq, kKeychainService);
    ScopedGlobalClaudeService globalSvc(svc);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ScopedModuleStop stopAtExit(&mod);

    // 先于 onStart 订阅：emit 按订阅序拷出 handler，观察者先跑 → 在观察者里 onStop。
    // 模块自己的 end handler 已被拷出（在途），随后执行——正是「先关原子闸再退订」要挡的
    // 迟到事件；闸若失效，它会复活刷新器（可从预填观测）。
    auto stopInsideEmit = ::core::event::bus().subscribe(
        ::core::event::kDbImportEnd,
        [&mod](const std::string&) { mod->onStop(); });
    (void)stopInsideEmit;
    mod->onStart();

    const std::string none = quotaProviderDisplayName("");
    const std::string deepseek = quotaProviderDisplayName("deepseek");
    DOCTEST_REQUIRE(!none.empty());
    DOCTEST_REQUIRE(!deepseek.empty());

    // 基线：开关开 + onStart ⇒ 刷新已起（预填 DeepSeek）。没有这条，「不复活」断言是空转。
    const std::string base = quotaDialogProviderLabel(router, mod, profileId, 1);
    DOCTEST_REQUIRE_MESSAGE(base == deepseek,
                            "基线：开关开时 onStart 必须起余量刷新（预填 DeepSeek），实到：" + base);

    // 事件到达：观察者先 onStop（关闸 + 退订 + 拆刷新器），模块在途 handler 随后到——必须被闸挡住。
    emitDbImportEnd();
    const std::string afterStop = quotaDialogProviderLabel(router, mod, profileId, 2);
    DOCTEST_REQUIRE_MESSAGE(!afterStop.empty(), "余量弹窗未打开：观察缝失效");
    DOCTEST_CHECK_MESSAGE(afterStop == none,
                          "onStop 后到达的 end 不得复活刷新（闸关闭 → 不建 quotaStore_，"
                          "预填应回落「不配置」），实到：" + afterStop);

    // 退订后再发：无订阅者 = no-op；模块对象仍在也不得复活
    emitDbImportEnd();
    const std::string afterUnsub = quotaDialogProviderLabel(router, mod, profileId, 3);
    DOCTEST_CHECK_MESSAGE(afterUnsub == none,
                          "退订后的 end 不得复活刷新，实到：" + afterUnsub);

    mod.reset();   // 析构兜底：刷新器若被复活且没收干净，这里 std::terminate（崩溃即断言）
    DOCTEST_CHECK_MESSAGE(true, "onStop 后迟到事件 + 析构全程未崩溃（无悬空线程）");
    return true;
})
