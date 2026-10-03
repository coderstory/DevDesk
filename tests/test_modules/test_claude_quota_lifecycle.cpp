// tests/test_modules/test_claude_quota_lifecycle.cpp
// 余量链的**生命周期**覆盖（静态审计 M5 的剩余一半，2026-10-03）
//
// 上一批补的 `scripts/check-quota-shutdown-order.sh` 守的是「代码里那几行的
// 顺序对」——**纯源码形态不变式**。它证明不了「这个模块真被构造、启动、
// 触发后台线程、停止、析构时不崩」。审计员的实测：把
// `if (quotaTestThread_.joinable()) quotaTestThread_.join();` 整行删掉
// （留下 joinable thread → 析构时 std::terminate）→ 全量 1259 例**零反应**。
//
// 本文件就是那条缺失的运行时覆盖：
//   ① `test_connection_result_round_trip` —— 「测试连接」确实起了后台线程、
//      线程跑完把结果送回 UI（顺带证明点击路径**非空转**，否则 ② 是假绿）。
//   ② `test_connection_then_immediate_stop` —— 点完**立刻** onStop + 析构，
//      不崩、不 terminate；重复三轮把时序抖动也压一遍。
//
// ── 三条测试纪律 ───────────────────────────────────────────────────
//   · **零真实网络**：种子 profile 的 `api_key_encrypted` 列留空 →
//     `loadApiKey` 按设计 fail → 弹窗 `profileApiKey` 为空 → provider 的
//     「空 key 是确定性失败且不发请求」早退生效（deepseek_provider.cpp）。
//     断言②钉住结果文案**就是**那条早退文案——只有走了早退分支才会得到它，
//     于是「没出网」本身也成了断言的一部分（而不是靠嘴说）。
//   · **后台刷新器同样不发请求**：种子配置行 `enabled = false` → `isDue`
//     第一道闸就 false → `onStart()` 起的那条真刷新线程一轮都查不出去。
//   · **不碰用户 Keychain**：ClaudeService 显式传测试专用 service 名；
//     且种子 profile 没有任何一行存过密钥 → 孤儿清理护栏③直接整体跳过，
//     连 list 都不会发生。
#include "doctest.h"
#include "test_support.h"

#include "app_registry.h"
#include "claude_service.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "quota/quota_store.h"

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

// 测试专用 Keychain service 名。显式传它，`cleanupOrphanKeychainEntries`
// （onStart 第一步）就绝不碰用户真实数据根派生出来的那个命名空间
// （DEVDESK_DATA_ROOT 不隔离 Keychain 的前科）。
constexpr const char* kKeychainService = "DevDeskTestClaudeQuotaLifecycle";

// 测试里选中的供应商：**必须是注册表里真实存在的一行**（findProvider 命中
// 才会起线程），且必须是「空 key 即早退、不发请求」的那类。
constexpr const char* kProviderId = "deepseek";

// 弹窗「测试连接」按钮 / 结果行的节点 id（claude_ui_quota.cpp 的文案单点）。
constexpr const char* kTestButtonId = "claude.dialog.quota.test.bg";
constexpr const char* kResultNodeId = "claude.dialog.quota.result";

int64_t insertProfileWithoutCredential(core::data::SqliteDb& db,
                                       const std::string& name) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    // **不写 api_key_encrypted** → 该列取表默认值 '' → loadApiKey 按设计返回
    // NotFound（「没有密钥」与「有但解不开」必须可分辨，见 profile_repository）。
    // 于是弹窗拿不到 key，provider 早退，本文件零出网。
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

// 种子：profile（无凭据）+ 一行余量配置（选了 provider，但**未启用**）。
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
// 为什么不能手写 `setGlobal(&svc)` … `setGlobal(nullptr)`：DOCTEST_REQUIRE 宏
// 自带 `return false;`，任何一条中途断言不过就会**提前返回**，末尾那句摘牌
// 永远执行不到 —— 全局指针就此指向栈上已析构的 svc，后面每一条用例都在读
// 悬垂指针（症状会漂到毫不相干的用例上，难查）。RAII 摘牌不挑返回路径。
struct ScopedGlobalClaudeService {
    explicit ScopedGlobalClaudeService(module::claude::ClaudeService& svc) {
        module::claude::ClaudeService::setGlobal(&svc);
    }
    ~ScopedGlobalClaudeService() { module::claude::ClaudeService::setGlobal(nullptr); }
    ScopedGlobalClaudeService(const ScopedGlobalClaudeService&) = delete;
    ScopedGlobalClaudeService& operator=(const ScopedGlobalClaudeService&) = delete;
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

// 端到端走一遍「onStart → 弹窗打开 → 点【测试连接】」。
//
// 为什么必须经 Router 负载而不是直接调内部函数：`runQuotaTestConnection` 是
// ClaudeModule 的私有成员，**唯一**的外部入口就是弹窗按钮的 onClick。
// 这里走的是与生产完全同款的那条路（consumeQuotaConfigPayload → openQuotaDialog
// → drawQuotaDialog → 按钮 onClick → runQuotaTestConnection）。
//
// 返回 bool（而不是就地断言）：DOCTEST_* 宏自带 `return false;`，在 void 函数里
// 会触发 -Wreturn-mismatch（=-Werror）。调用方用 DOCTEST_REQUIRE_MESSAGE 收口，
// 报错信息留在调用点。
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

// ==================== ① 「测试连接」确实起线程并把结果送回 UI ====================
//
// 这条是 ② 的**非空转前提**：若「点按钮 → 起线程 → 结果回 UI」这条链哪天断了
// （findProvider 查不到、按钮 onClick 没接上、线程没跑到 harvest），这里先红，
// 而不是让 ② 靠「反正没崩」持续报绿。
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

    // 收割是 compose 每帧尾部的一次轮询（harvestQuotaTestResult），结果行又
    // 在下一帧才画出来 —— 有界轮询等它落地，绝不靠固定长睡。
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
    // 这串文案**只可能**来自 provider 体内「空 key = 确定性失败且不发请求」
    // 那一行（deepseek_provider.cpp）—— 它同时钉住两件事：
    //   · 线程真的跑到了 fetch 并把结果发了出来（不是空转）；
    //   · 走的是**发请求之前**的早退（本用例零真实出网）。
    DOCTEST_CHECK_MESSAGE(!result.empty(),
                          "点【测试连接】后 2 秒内 UI 上没出现任何结果文案："
                          "后台线程要么没起、要么没把结果送回 UI");
    DOCTEST_CHECK_MESSAGE(result.find("未配置 API 密钥") != std::string::npos,
                          "结果文案应为「未配置 API 密钥」（种子 profile 无凭据 → "
                          "provider 发请求前早退），实到：" + result);

    mod->onStop();
    return true;
})

// ==================== ② 点完立刻停：join / 析构 / 重启一轮 ====================
//
// 守的正是审计员变异 11 删掉的那一行：`stopQuotaRefresh()` 开头的
// `if (quotaTestThread_.joinable()) quotaTestThread_.join();`。
// 删掉它 → 线程成员析构时仍 joinable → std::terminate → **进程 abort**
// （门禁随之红；这正是「崩」在本测试框架里的表现形式——没有断言可写，
// 崩溃本身就是那条断言）。
//
// ⚠ 与 ① 的分工：① 收割结果（join 在 harvestQuotaTestResult 里发生），
// 本条**刻意不收割**——被测的正是 stopQuotaRefresh 那条 join 路径，
// 若先收割，线程早被别的路径 join 掉，本条就测不到 stopQuotaRefresh 了。
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

    DOCTEST_CHECK_MESSAGE(true, "三轮「点【测试连接】→ 立刻 onStop → 析构」未崩溃");
    return true;
})
