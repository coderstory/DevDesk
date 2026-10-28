// tests/test_modules/test_channels.cpp
// 调度通知渠道层（波 S5）：注册表分发契约 + 四个内置渠道行为。
// 每条用例对应一个业务场景并注明防什么事故；不覆盖 platform::notify facade
// 内部的截断/限速（那是 test_platform/test_notification.cpp 的契约面）。
#include "doctest.h"

#include "module_schedule/channel_registry.h"
#include "platform/notification.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using module::schedule::Channel;
using module::schedule::Delivery;
using module::schedule::Receipt;

Delivery makeDelivery() {
    Delivery d;
    d.title = "提醒标题";
    d.body = "提醒正文";
    d.level = 2;
    d.deeplink = "schedule://home";
    d.runId = 42;
    d.dedupeKey = "rule:7@planned:1700000000";
    return d;
}

// 记录型假渠道：断言分发顺序/次数/载荷透传
class RecordingChannel final : public Channel {
public:
    explicit RecordingChannel(std::string id) : id_(std::move(id)) {}
    std::string id() const override { return id_; }
    core::Result<Receipt> deliver(const Delivery& d) override {
        received.push_back(d);
        return core::Result<Receipt>::ok(Receipt{"ok", "fake"});
    }
    std::vector<Delivery> received;

private:
    std::string id_;
};

// 可配置失败结果的假渠道
class FailingChannel final : public Channel {
public:
    std::string id() const override { return "failing"; }
    core::Result<Receipt> deliver(const Delivery&) override {
        return core::Result<Receipt>::fail(core::ErrorCode::Internal, "fake fail");
    }
};

// 假平台通知后端：拦截 facade 发往后端的载荷（facade 清洗/去重/限速仍生效）
class FakeNotifyBackend final : public platform::notify::Backend {
public:
    void initialize() override {}
    core::Result<platform::notify::Status> send(
        const platform::notify::Notification& n) override {
        sent.push_back(n);
        return core::Result<platform::notify::Status>::ok(status);
    }
    platform::notify::Authorization authorizationStatus() override {
        return platform::notify::Authorization::Authorized;
    }
    void requestAuthorization() override {}

    std::vector<platform::notify::Notification> sent;
    platform::notify::Status status = platform::notify::Status::Accepted;
};

}  // namespace

// 防：重复注册被静默覆盖——已上线的渠道实现被悄悄换成另一个，行为漂移
REGISTER_TEST(channelsRegistryRegisterFindAndDuplicate, "schedule 渠道注册表：注册/查找/重复 id 拒绝", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();

    auto first = std::make_unique<RecordingChannel>("alpha");
    RecordingChannel* firstRaw = first.get();
    DOCTEST_CHECK(registry.registerChannel(std::move(first)).isOk());
    DOCTEST_CHECK_EQ(registry.find("alpha"), static_cast<const Channel*>(firstRaw));
    DOCTEST_CHECK_EQ(registry.find("ghost"), static_cast<const Channel*>(nullptr));

    auto dup = registry.registerChannel(std::make_unique<RecordingChannel>("alpha"));
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK_EQ(dup.errorCode(), core::ErrorCode::AlreadyExists);
    DOCTEST_CHECK_EQ(registry.find("alpha"), static_cast<const Channel*>(firstRaw));
    registry.resetForTest();
    return true;
});

// 防：分发乱序 / 漏发 / 多发——多渠道时每条渠道都必须且只收到一份载荷
REGISTER_TEST(channelsRegistryDispatchOrderAndReach, "schedule 渠道注册表：按 channel_ids 顺序逐渠道分发", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    auto a = std::make_unique<RecordingChannel>("a");
    auto b = std::make_unique<RecordingChannel>("b");
    auto c = std::make_unique<RecordingChannel>("c");
    RecordingChannel* aRaw = a.get();
    RecordingChannel* bRaw = b.get();
    RecordingChannel* cRaw = c.get();
    registry.registerChannel(std::move(a));
    registry.registerChannel(std::move(b));
    registry.registerChannel(std::move(c));

    Delivery d = makeDelivery();
    auto report = registry.deliver({"c", "b", "a"}, d);
    DOCTEST_CHECK(report.isOk());
    const auto& entries = report.value().entries;
    DOCTEST_CHECK_EQ(entries.size(), 3u);
    DOCTEST_CHECK(entries[0].receipt.isOk());
    DOCTEST_CHECK_EQ(entries[0].channelId, std::string("c"));
    DOCTEST_CHECK_EQ(entries[1].channelId, std::string("b"));
    DOCTEST_CHECK_EQ(entries[2].channelId, std::string("a"));
    DOCTEST_CHECK_EQ(cRaw->received.size(), 1u);
    DOCTEST_CHECK_EQ(bRaw->received.size(), 1u);
    DOCTEST_CHECK_EQ(aRaw->received.size(), 1u);
    registry.resetForTest();
    return true;
});

// 防：空渠道列表被误报为失败（无渠道 = 无投递，静默动作的合法形态），
// 或被误触发了任何渠道
REGISTER_TEST(channelsRegistryEmptyIds, "schedule 渠道注册表：空 channel_ids = ok 且零分发", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    auto a = std::make_unique<RecordingChannel>("a");
    RecordingChannel* aRaw = a.get();
    registry.registerChannel(std::move(a));

    auto report = registry.deliver({}, makeDelivery());
    DOCTEST_CHECK(report.isOk());
    DOCTEST_CHECK(report.value().entries.empty());
    DOCTEST_CHECK_EQ(aRaw->received.size(), 0u);
    registry.resetForTest();
    return true;
});

// 防：一个未注册/已移除的渠道 id 阻断整条分发链（规则里残留旧渠道配置时
// 其余渠道必须照常投递）
REGISTER_TEST(channelsRegistryUnknownIdIsolated, "schedule 渠道注册表：未知 id 只影响自身 entry", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    auto a = std::make_unique<RecordingChannel>("a");
    RecordingChannel* aRaw = a.get();
    registry.registerChannel(std::move(a));

    auto report = registry.deliver({"ghost", "a"}, makeDelivery());
    DOCTEST_CHECK(report.isOk());
    const auto& entries = report.value().entries;
    DOCTEST_CHECK_EQ(entries.size(), 2u);
    DOCTEST_CHECK(entries[0].receipt.isFail());
    DOCTEST_CHECK_EQ(entries[0].receipt.errorCode(), core::ErrorCode::NotFound);
    DOCTEST_CHECK(entries[1].receipt.isOk());
    DOCTEST_CHECK_EQ(aRaw->received.size(), 1u);
    DOCTEST_CHECK(!report.value().allOk());
    registry.resetForTest();
    return true;
});

// 防：单个渠道失败被短路成"整体失败"从而掩盖其它渠道的成功投递
REGISTER_TEST(channelsRegistryPerChannelFailure, "schedule 渠道注册表：单渠道失败不短路分发链", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    registry.registerChannel(std::make_unique<FailingChannel>());
    auto a = std::make_unique<RecordingChannel>("a");
    RecordingChannel* aRaw = a.get();
    registry.registerChannel(std::move(a));

    auto report = registry.deliver({"failing", "a"}, makeDelivery());
    DOCTEST_CHECK(report.isOk());
    DOCTEST_CHECK(report.value().entries[0].receipt.isFail());
    DOCTEST_CHECK(report.value().entries[1].receipt.isOk());
    DOCTEST_CHECK_EQ(aRaw->received.size(), 1u);
    registry.resetForTest();
    return true;
});

// 防：静默渠道被误加副作用/失败路径——静默执行类动作的"零打扰"承诺被破坏
REGISTER_TEST(channelsSilentAlwaysOk, "schedule 静默渠道：任意载荷恒成功零副作用", {
    auto channel = module::schedule::makeBuiltinChannel(module::schedule::kChannelSilent);
    DOCTEST_CHECK(channel != nullptr);
    DOCTEST_CHECK_EQ(channel->id(), std::string(module::schedule::kChannelSilent));
    Delivery d = makeDelivery();
    d.title.clear();
    d.body.clear();
    auto result = channel->deliver(d);
    DOCTEST_CHECK(result.isOk());
    DOCTEST_CHECK_EQ(result.value().status, std::string("ok"));
    return true;
});

// 防：分发时载荷字段截断/丢失/错位——记录层依赖 runId/dedupeKey 落
// schedule_delivery，渠道层收到什么必须与调用方发出的一致
REGISTER_TEST(channelsDeliveryFieldPassthrough, "schedule 分发：Delivery 全字段透传", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    auto a = std::make_unique<RecordingChannel>("a");
    RecordingChannel* aRaw = a.get();
    registry.registerChannel(std::move(a));

    Delivery d = makeDelivery();
    auto report = registry.deliver({"a"}, d);
    DOCTEST_CHECK(report.isOk());
    DOCTEST_CHECK_EQ(aRaw->received.size(), 1u);
    const Delivery& got = aRaw->received[0];
    DOCTEST_CHECK_EQ(got.title, std::string("提醒标题"));
    DOCTEST_CHECK_EQ(got.body, std::string("提醒正文"));
    DOCTEST_CHECK_EQ(got.level, 2);
    DOCTEST_CHECK(got.deeplink.has_value());
    DOCTEST_CHECK_EQ(*got.deeplink, std::string("schedule://home"));
    DOCTEST_CHECK_EQ(got.runId, 42);
    DOCTEST_CHECK_EQ(got.dedupeKey, std::string("rule:7@planned:1700000000"));
    registry.resetForTest();
    return true;
});

// 防：去重键丢失——OS 通知 2s 去重靠 id=dedupeKey，键不传则同 (rule, planned_at)
// 的重复投递穿透为重复系统通知；同时锁 level 映射与状态串回执
REGISTER_TEST(channelsOsDedupeKeyAndStatus, "schedule OS 渠道：dedupeKey 透传 + 状态映射", {
    platform::notify::resetForTest();
    FakeNotifyBackend fakeBackend;
    platform::notify::setBackendForTest(&fakeBackend);

    auto channel = module::schedule::makeBuiltinChannel(module::schedule::kChannelOs);
    Delivery d = makeDelivery();
    auto result = channel->deliver(d);
    DOCTEST_CHECK(result.isOk());
    DOCTEST_CHECK_EQ(result.value().status, std::string("accepted"));
    DOCTEST_CHECK_EQ(fakeBackend.sent.size(), 1u);
    DOCTEST_CHECK_EQ(fakeBackend.sent[0].id, std::string("rule:7@planned:1700000000"));
    DOCTEST_CHECK_EQ(fakeBackend.sent[0].title, std::string("提醒标题"));
    DOCTEST_CHECK_EQ(fakeBackend.sent[0].body, std::string("提醒正文"));
    DOCTEST_CHECK(fakeBackend.sent[0].level == platform::notify::Level::Warning);

    // 环境类失败 → 状态串，不是 Result::fail
    fakeBackend.status = platform::notify::Status::PermissionDenied;
    auto denied = channel->deliver(makeDelivery());
    DOCTEST_CHECK(denied.isOk());
    DOCTEST_CHECK_EQ(denied.value().status, std::string("permission_denied"));

    // level 越界钳制（防：非法 level 越界读取）
    Delivery bad = makeDelivery();
    bad.level = 99;
    fakeBackend.status = platform::notify::Status::Accepted;
    auto clamped = channel->deliver(bad);
    DOCTEST_CHECK(clamped.isOk());
    DOCTEST_CHECK(fakeBackend.sent.back().level == platform::notify::Level::Error);

    platform::notify::resetForTest();
    return true;
});

// 防：非主线程直触 UI——deliver 只入队、flush 才是唯一落 UI 时机；
// 防：toast 槽位膨胀——单实例语义下多次投递只弹最新一条
REGISTER_TEST(channelsToastMainThreadMarshaling, "schedule toast 渠道：deliver 入队 + flush 单实例落 UI", {
    std::vector<std::pair<std::string, std::string>> shown;
    auto sink = [&shown](const std::string& t, const std::string& b) {
        shown.emplace_back(t, b);
    };
    auto channel = module::schedule::makeToastChannelForTest(sink);
    module::schedule::ChannelRegistry registry;
    registry.registerChannel(std::move(channel));

    Delivery first = makeDelivery();
    first.title = "第一条";
    Delivery second = makeDelivery();
    second.title = "第二条";

    auto r1 = registry.deliver({module::schedule::kChannelToast}, first);
    DOCTEST_CHECK(r1.isOk());
    DOCTEST_CHECK(shown.empty());  // 未 flush 不触 UI
    auto r2 = registry.deliver({module::schedule::kChannelToast}, second);
    DOCTEST_CHECK(r2.isOk());

    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(shown.size(), 1u);  // 单实例：只弹最新
    DOCTEST_CHECK_EQ(shown[0].first, std::string("第二条"));
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(shown.size(), 1u);  // 槽位已清，重复 flush 不重弹
    registry.resetForTest();
    return true;
});

// 防：启动期投递被吞 / 逐条开窗骚扰 / 关闭后立刻复弹 / 冷却到期不恢复
REGISTER_TEST(channelsFullscreenAggregateCooldownRepop, "schedule 全屏渠道：聚合开窗 + 5 分钟冷却 + 首 tick 补弹 + 窗开刷新", {
    struct Probe {
        int calls = 0;
        std::vector<std::vector<Delivery>> snapshots;
        std::function<void()> notifyClosed;
        void updater(const std::vector<Delivery>& items,
                     const std::function<void()>& onClosed) {
            ++calls;
            snapshots.push_back(items);
            notifyClosed = onClosed;
        }
    } probe;
    std::int64_t now = 1000;
    auto nowFn = [&now] { return now; };

    auto channel = module::schedule::makeFullscreenChannelForTest(
        [&probe](const std::vector<Delivery>& items,
                 const std::function<void()>& onClosed) { probe.updater(items, onClosed); },
        nowFn);
    module::schedule::ChannelRegistry registry;
    registry.registerChannel(std::move(channel));

    // 首 tick 补弹 + 聚合：flush 前到达的两条投递合并为一次开窗
    Delivery a = makeDelivery();
    a.title = "A";
    a.dedupeKey = "k1";
    Delivery b = makeDelivery();
    b.title = "B";
    b.dedupeKey = "k2";
    registry.deliver({module::schedule::kChannelFullscreen}, a);
    registry.deliver({module::schedule::kChannelFullscreen}, b);
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(probe.calls, 1);
    DOCTEST_CHECK_EQ(probe.snapshots[0].size(), 2u);

    // 窗打开期间新投递 → 刷新（不重新开窗）
    Delivery c = makeDelivery();
    c.title = "C";
    c.dedupeKey = "k3";
    registry.deliver({module::schedule::kChannelFullscreen}, c);
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(probe.calls, 2);
    DOCTEST_CHECK_EQ(probe.snapshots[1].size(), 3u);

    // 用户关窗 → 5 分钟冷却：期间新投递保持入队、不开窗
    DOCTEST_CHECK(probe.notifyClosed != nullptr);
    probe.notifyClosed();
    Delivery d = makeDelivery();
    d.title = "D";
    d.dedupeKey = "k4";
    registry.deliver({module::schedule::kChannelFullscreen}, d);
    now = 1100;  // 关窗后 100s，冷却中
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(probe.calls, 2);

    // 冷却到期（>300s）→ 补弹入队条目
    now = 1401;
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(probe.calls, 3);
    DOCTEST_CHECK_EQ(probe.snapshots[2].size(), 1u);
    DOCTEST_CHECK_EQ(probe.snapshots[2][0].title, std::string("D"));
    registry.resetForTest();
    return true;
});

// 防：同 (rule, planned_at) 重复投递被弹两次——dedupeKey 相同的条目必须合并
REGISTER_TEST(channelsFullscreenDedupeKeyMerge, "schedule 全屏渠道：同 dedupeKey 合并为一条", {
    struct Probe {
        int calls = 0;
        std::vector<Delivery> items;
        void updater(const std::vector<Delivery>& got,
                     const std::function<void()>&) {
            ++calls;
            items = got;
        }
    } probe;
    std::int64_t now = 1000;
    auto channel = module::schedule::makeFullscreenChannelForTest(
        [&probe](const std::vector<Delivery>& items,
                 const std::function<void()>& onClosed) { probe.updater(items, onClosed); },
        [&now] { return now; });
    module::schedule::ChannelRegistry registry;
    registry.registerChannel(std::move(channel));

    Delivery a = makeDelivery();
    a.title = "第一版";
    a.dedupeKey = "k1";
    Delivery a2 = makeDelivery();
    a2.title = "第二版";
    a2.dedupeKey = "k1";  // 同键：后到覆盖
    registry.deliver({module::schedule::kChannelFullscreen}, a);
    registry.deliver({module::schedule::kChannelFullscreen}, a2);
    registry.flushAllToMainThread();
    DOCTEST_CHECK_EQ(probe.calls, 1);
    DOCTEST_CHECK_EQ(probe.items.size(), 1u);
    DOCTEST_CHECK_EQ(probe.items[0].title, std::string("第二版"));
    registry.resetForTest();
    return true;
});

// 防：工厂拼错 id 映射——内置渠道 id 与 kChannel* 常量脱节导致分发永远 NotFound
REGISTER_TEST(channelsFactoryKnownAndUnknownIds, "schedule 渠道工厂：四内置 id 就位 + 未知 id 空", {
    DOCTEST_CHECK(module::schedule::makeBuiltinChannel(module::schedule::kChannelFullscreen) != nullptr);
    DOCTEST_CHECK(module::schedule::makeBuiltinChannel(module::schedule::kChannelToast) != nullptr);
    DOCTEST_CHECK(module::schedule::makeBuiltinChannel(module::schedule::kChannelOs) != nullptr);
    DOCTEST_CHECK(module::schedule::makeBuiltinChannel(module::schedule::kChannelSilent) != nullptr);
    DOCTEST_CHECK_EQ(module::schedule::makeBuiltinChannel("wecom"), nullptr);
    return true;
});
