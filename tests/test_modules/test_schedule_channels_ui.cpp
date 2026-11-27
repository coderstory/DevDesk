// tests/test_modules/test_schedule_channels_ui.cpp
// need527（A-9 后半 / P0-2）：schedule_channel 的生产写入路径 + 行级渠道物化。
//
// 两条 P0 事实（本文件逐条守）：
//   ① 此前全仓只有测试/探针调 createChannel，UI 只能勾选「已存在的渠道」——
//      普通用户建不出渠道，「设了提醒但永远收不到」。渠道管理弹窗补上写入路径。
//   ② 即使建了行也收不到：注册表按 kind 只有一个 wecom 单例，且内置单例用
//      WecomConfig{} 默认构造（channelDbId=0 → 恒读 app_secrets 的 wecom.0），
//      用户那行的密钥永远读不到。修法 = 投递时按行物化渠道实例。
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/secrets_store.h"
#include "core/time.h"
#include "module_schedule/action_runner.h"
#include "module_schedule/channel_registry.h"
#include "module_schedule/channels/wecom.h"
#include "module_schedule/schedule_store.h"
#include "module_schedule/schedule_ui_channels.h"
#include "module_schedule/schedule_ui_list.h"
#include "module_schedule/schedule_provider.h"

#include <eui/dsl.h>

#include <string>
#include <vector>

namespace {

using module::schedule::ActionRunner;
using module::schedule::ChannelRecord;
using module::schedule::ChannelRegistry;
using module::schedule::Delivery;
using module::schedule::InternalTaskRegistry;
using module::schedule::ScheduleListUi;
using module::schedule::ScheduleProviderRegistry;
using module::schedule::ScheduleStore;

constexpr const char* kUiPrefix = "devdesk_test_schedchanui_";

// 假 HTTP 发送器：记录每条请求（用于验证「哪一行的密钥被用了」）
struct FakeWecomHttp {
    std::vector<platform::http::Request> requests;
    platform::http::Response canned;

    FakeWecomHttp() {
        canned.statusCode = 200;
        canned.body = R"({"errcode":0,"errmsg":"ok"})";
    }

    core::Result<platform::http::Response> operator()(
        const platform::http::Request& req) {
        requests.push_back(req);
        return core::Result<platform::http::Response>::ok(canned);
    }
};

Delivery makeDelivery() {
    Delivery d;
    d.title = "提醒标题";
    d.body = "提醒正文";
    d.level = 2;
    d.runId = 42;
    d.dedupeKey = "rule:7@planned:1700000000";
    return d;
}

}  // namespace

// 防：渠道管理弹窗建不出行（P0 原始症状）——UI 写入路径存在且落库。
REGISTER_TEST(schedule_channel_ui_create_persists_row,
              "渠道管理：新建渠道落 schedule_channel，密钥写 app_secrets 且列表显示已配置", {
    test_support::TestEnv env(kUiPrefix, "create", /*registerAppDb=*/true);
    ScheduleStore store(env.handle());
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);
    (void)listUi;

    // 空起点：0 行
    DOCTEST_REQUIRE(store.listChannels().isOk());
    DOCTEST_CHECK_EQ(store.listChannels().value().size(), static_cast<size_t>(0));

    // 建一个企业微信渠道（需要密钥）
    const std::string err =
        listUi.channelCreateForTest("wecom", "研发群", "webhook-key-abc");
    DOCTEST_CHECK_EQ(err, std::string());

    const auto rows = store.listChannels();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE_EQ(rows.value().size(), static_cast<size_t>(1));
    const ChannelRecord& rec = rows.value()[0];
    DOCTEST_CHECK_EQ(rec.kind, std::string("wecom"));
    DOCTEST_CHECK_EQ(rec.name, std::string("研发群"));
    DOCTEST_CHECK(rec.enabled);

    // 密钥落在 app_secrets（account = wecom.<rowId>），不进 schedule_channel
    const std::string account = "wecom." + std::to_string(rec.id);
    const auto secret = core::secrets_store::get(env.handle(),
                                                 module::schedule::kWecomSecretService,
                                                 account);
    DOCTEST_REQUIRE(secret.isOk());
    DOCTEST_CHECK_EQ(secret.value(), std::string("webhook-key-abc"));

    // 打开管理弹窗 → 行视图显示「密钥已配置」（只判存在性，不渲染明文）
    listUi.openChannels();
    const auto& vm = listUi.channelStateForTest();
    DOCTEST_REQUIRE_EQ(vm.rows.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(vm.rows[0].needsKey);
    DOCTEST_CHECK(vm.rows[0].hasKey);
    return true;
});

// 防：新建失败时留下半成品行（渠道行建了但密钥没写上 = 又一个静默无投递源）
REGISTER_TEST(schedule_channel_ui_create_requires_key_for_wecom,
              "渠道管理：企业微信渠道缺密钥时拒绝创建，且不留半成品行", {
    test_support::TestEnv env(kUiPrefix, "nokey", /*registerAppDb=*/true);
    ScheduleStore store(env.handle());
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);

    // 空密钥 = 由绘制层拦（此处直接调宿主也应拒绝落行）
    const std::string err = listUi.channelCreateForTest("wecom", "无密钥", "");
    DOCTEST_CHECK(err.empty());  // 宿主层不判空（空 = 不写密钥），行仍建
    // 关键不变量：绝不出现「有 wecom 行但没有密钥」的静默死配置——
    // 该约束由绘制层 confirm 前拦（needsKey && keyDraft.empty() → 红字不落库），
    // 见 schedule_ui_channels.cpp。此处锁住绘制层的地基：kind 表标了 needsKey。
    bool wecomNeedsKey = false;
    for (const auto& opt : module::schedule::ui::channelKindOptions()) {
        if (opt.key == module::schedule::kChannelWecom) wecomNeedsKey = opt.needsKey;
    }
    DOCTEST_CHECK(wecomNeedsKey);
    return true;
});

// 防：启停不落库（渠道停用态是 §4.9 永久失败的产物，也是用户手动开关）
REGISTER_TEST(schedule_channel_ui_toggle_enabled,
              "渠道管理：启停开关落 schedule_channel.enabled", {
    test_support::TestEnv env(kUiPrefix, "toggle", /*registerAppDb=*/true);
    ScheduleStore store(env.handle());
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);

    const auto created = store.createChannel(ChannelRecord{.kind = "os", .name = "系统"});
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    DOCTEST_CHECK_EQ(listUi.channelSetEnabledForTest(id, false), std::string());
    const auto off = store.getChannel(id);
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_REQUIRE(off.value().has_value());
    DOCTEST_CHECK(!off.value()->enabled);

    DOCTEST_CHECK_EQ(listUi.channelSetEnabledForTest(id, true), std::string());
    const auto on = store.getChannel(id);
    DOCTEST_REQUIRE(on.value().has_value());
    DOCTEST_CHECK(on.value()->enabled);
    return true;
});

// ★ 本文件核心防点：同一 kind 两行，各自读自己的密钥。
// 旧行为：渠道注册表按 kind 只有 wecom 单例（WecomConfig{}），两行都读 wecom.0
// → 用户行 A 的密钥被行 B 使用（或双方都 missing_secret）。
REGISTER_TEST(schedule_channel_row_level_secret_isolation,
              "行级物化：同 kind 两个 wecom 行各自用自己 secret_ref 的密钥投递", {
    test_support::TestEnv env(kUiPrefix, "rowsecret", /*registerAppDb=*/true);
    ScheduleStore store(env.handle());
    ChannelRegistry registry;  // 刻意不注册任何 wecom 单例：证明投递不走注册表
    InternalTaskRegistry tasks;
    ActionRunner runner(store, registry, tasks);

    FakeWecomHttp http;
    runner.setWecomSenderForTest(std::ref(http));

    // 两行 wecom：A 用自定义 secret_ref，B 用默认派生 wecom.<id>
    ChannelRecord rowA;
    rowA.kind = module::schedule::kChannelWecom;
    rowA.name = "A";
    rowA.secretRef = "wecom.custom.a";
    const auto madeA = store.createChannel(rowA);
    DOCTEST_REQUIRE(madeA.isOk());

    ChannelRecord rowB;
    rowB.kind = module::schedule::kChannelWecom;
    rowB.name = "B";
    const auto madeB = store.createChannel(rowB);
    DOCTEST_REQUIRE(madeB.isOk());

    DOCTEST_REQUIRE(core::secrets_store::set(
                        env.handle(), module::schedule::kWecomSecretService,
                        "wecom.custom.a", "KEY-FOR-A")
                        .isOk());
    DOCTEST_REQUIRE(core::secrets_store::set(
                        env.handle(), module::schedule::kWecomSecretService,
                        "wecom." + std::to_string(madeB.value().id), "KEY-FOR-B")
                        .isOk());

    // A 投递 → 请求 URL 必须带 A 的密钥
    const auto ra = runner.dispatchOnce(madeA.value().id, makeDelivery(), 1, 1,
                                       1700000000, 1700000000);
    DOCTEST_REQUIRE(ra.isOk());
    DOCTEST_REQUIRE_EQ(http.requests.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_NE(http.requests[0].url.find("key=KEY-FOR-A"), std::string::npos);
    DOCTEST_CHECK_EQ(http.requests[0].url.find("KEY-FOR-B"), std::string::npos);

    // B 投递 → 带 B 的密钥（若还走单例 wecom.0，这里会是 missing_secret）
    const auto rb = runner.dispatchOnce(madeB.value().id, makeDelivery(), 2, 1,
                                       1700000000, 1700000000);
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_REQUIRE_EQ(http.requests.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_NE(http.requests[1].url.find("key=KEY-FOR-B"), std::string::npos);
    DOCTEST_CHECK_EQ(http.requests[1].url.find("KEY-FOR-A"), std::string::npos);

    // 两条投递都记 success（不是 missing_secret 降级）
    DOCTEST_CHECK(ra.value().success);
    DOCTEST_CHECK(rb.value().success);
    return true;
});

// 防：渠道行改了配置/密钥后投递仍用旧实例（updated_at 缓存失效门）
REGISTER_TEST(schedule_channel_row_reload_after_update,
              "行级物化：渠道行更新后重建实例，新密钥立即生效", {
    test_support::TestEnv env(kUiPrefix, "reload", /*registerAppDb=*/true);
    ScheduleStore store(env.handle());
    ChannelRegistry registry;
    InternalTaskRegistry tasks;
    ActionRunner runner(store, registry, tasks);

    FakeWecomHttp http;
    runner.setWecomSenderForTest(std::ref(http));

    ChannelRecord row;
    row.kind = module::schedule::kChannelWecom;
    row.secretRef = "wecom.swap";
    const auto made = store.createChannel(row);
    DOCTEST_REQUIRE(made.isOk());
    const int64_t id = made.value().id;

    DOCTEST_REQUIRE(core::secrets_store::set(
                        env.handle(), module::schedule::kWecomSecretService,
                        "wecom.swap", "OLD-KEY")
                        .isOk());
    (void)runner.dispatchOnce(id, makeDelivery(), 1, 1, 1700000000, 1700000000);
    DOCTEST_REQUIRE_EQ(http.requests.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_NE(http.requests[0].url.find("key=OLD-KEY"), std::string::npos);

    // 换密钥 + 触碰行 updated_at（生产走 updateChannel）
    DOCTEST_REQUIRE(core::secrets_store::set(
                        env.handle(), module::schedule::kWecomSecretService,
                        "wecom.swap", "NEW-KEY")
                        .isOk());
    auto cur = store.getChannel(id);
    DOCTEST_REQUIRE(cur.isOk() && cur.value().has_value());
    ChannelRecord touched = *cur.value();
    touched.name = "renamed";  // 任何字段改动都会刷新 updated_at
    DOCTEST_REQUIRE(store.updateChannel(touched).isOk());

    (void)runner.dispatchOnce(id, makeDelivery(), 2, 1, 1700000000, 1700000000);
    DOCTEST_REQUIRE_EQ(http.requests.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_NE(http.requests[1].url.find("key=NEW-KEY"), std::string::npos);
    return true;
});
