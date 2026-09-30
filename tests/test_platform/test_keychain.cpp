#include "doctest.h"
#include "platform/keychain.h"

#include <core/paths.h>

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

// macOS 扫描路径测试缝（仅 __APPLE__ 有实现，见 keychain_macos_scan.h）：
// 只把枚举/删除两个边界动作换成可注入函数指针，决策循环不动 → 接线可断言
#if defined(__APPLE__)
#include "platform/keychain_macos_scan.h"
#endif

// macOS 平台守卫：本平台实现走 Security.framework，只能操作**真实 login keychain**
// （keychain_macos.cpp 用 SecKeychainAddGenericPassword + nullptr 默认钥匙串，没有
// 测试专用钥匙串的注入口）——会写条目 / 全库枚举的用例若照跑，会污染用户钥匙串、
// 可能弹授权框（deleteSecretsNotIn 还按 kSecMatchLimitAll 枚举整库）。
// Windows（DPAPI + %APPDATA% 文件）与 Linux（$HOME 文件）实现不碰系统凭据库，
// 行为与从前一字不差，照常执行。
#if defined(__APPLE__)
#define SKIP_ON_MACOS_KEYCHAIN()                       \
    DOCTEST_SKIP(                                      \
        "macOS login keychain 不可隔离（无测试注入口）：跳过以免写真实钥匙串")
#else
#define SKIP_ON_MACOS_KEYCHAIN() do { } while (0)
#endif

REGISTER_TEST(keychain_store_load, "keychain: store and load", {
    SKIP_ON_MACOS_KEYCHAIN();   // 会写入真实钥匙串 → macOS 跳过

    const std::string service = "DevDeskTest";
    const std::string account = "test_account";
    const std::string secret = "sk-ant-test-secret-12345";

    platform::keychain::deleteSecret(service, account);

    auto store = platform::keychain::storeSecret(service, account, secret);
    DOCTEST_REQUIRE(store.isOk());

    auto load = platform::keychain::loadSecret(service, account);
    DOCTEST_REQUIRE(load.isOk());
    DOCTEST_CHECK_EQ(load.value(), secret);

    auto del = platform::keychain::deleteSecret(service, account);
    DOCTEST_CHECK(del.isOk());
    return true;
})

REGISTER_TEST(keychain_load_nonexistent, "keychain: load nonexistent", {
    auto load = platform::keychain::loadSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(load.isFail());
    return true;
})

REGISTER_TEST(keychain_delete_nonexistent, "keychain: delete nonexistent ok", {
    auto del = platform::keychain::deleteSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(del.isOk());
    return true;
})

// 空 service / 空 account 在 macOS Keychain API 里是**通配**（serviceLength /
// accountLength 传 0 = 匹配任意值），于是「空 account」会被静默当成「随便给我
// 一条」——load 返回**别人**的密钥、delete 删掉**别人**的条目。本仓库空 account
// 有确切含义 =「该 profile 没有密钥」（反向同步「缺席即清空」会清空
// keychain_account），此时必须回答「没有这条」。见 keychain_macos.cpp 的
// 「空 service / 空 account 守卫」段。
// 断言顺序是**故意的**：store 放最前。守卫被摘掉时它先红（fail-fast），后面的
// load / delete 断言就不再执行 —— 否则 load 会真的从用户真实钥匙串里读出一条
// 任意密钥、delete 更会通配删掉真实条目（实测：摘掉守卫后 loadSecret("","")
// 返回 Ok，即整库任取一条）。摘守卫的代价只有一条：往
// DevDeskTestBlankIdentity 这个测试专用 service 写一条空 account 条目（缺陷本身
// 的行为），可在「钥匙串访问」里手工删除。
#if defined(__APPLE__)
REGISTER_TEST(keychain_blank_identity_no_wildcard,
              "keychain: 空 service/account 不通配（macOS）", {
    // store：无身份可写 → 显式拒绝（不通配删旧，也不写查不回来的孤儿条目）
    auto stored = platform::keychain::storeSecret("DevDeskTestBlankIdentity", "",
                                                  "sk-blank-identity");
    DOCTEST_CHECK(stored.isFail());

    // load：空 account / 空 service / 两者皆空 → 一律没这条，不能命中任意条目
    DOCTEST_CHECK(platform::keychain::loadSecret("DevDeskTestBlankIdentity", "")
                      .isFail());
    DOCTEST_CHECK(platform::keychain::loadSecret("", "claude:blank").isFail());
    DOCTEST_CHECK(platform::keychain::loadSecret("", "").isFail());

    // delete：无身份 = 没有这条 → 幂等 ok（不允许落成通配删除）
    DOCTEST_CHECK(
        platform::keychain::deleteSecret("DevDeskTestBlankIdentity", "").isOk());
    return true;
})
#endif

REGISTER_TEST(keychain_delete_secrets_not_in,
              "keychain: deleteSecretsNotIn 只留 keep 集合", {
    SKIP_ON_MACOS_KEYCHAIN();   // 写 3 条 + 枚举整库 → macOS 跳过

    const std::string service = "DevDeskTestOrphan";
    // 三个密钥：keep 两个，孤儿一个
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:1", "sk-keep-1").isOk());
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:2", "sk-keep-2").isOk());
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:999", "sk-orphan").isOk());
    // P0 护栏②（2026-09-30）：白名单前缀（claude:）之外的条目——
    // 「同 service 下未来别的用途」的形态，扫描必须不删、不计数
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "notes:theme", "dark").isOk());

    auto r = platform::keychain::deleteSecretsNotIn(
        service, {"claude:1", "claude:2"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().size(), 1u);   // 只删 claude:999

    // 孤儿被删，保留的仍可读
    DOCTEST_CHECK(platform::keychain::loadSecret(service, "claude:999").isFail());
    auto k1 = platform::keychain::loadSecret(service, "claude:1");
    DOCTEST_REQUIRE(k1.isOk());
    DOCTEST_CHECK_EQ(k1.value(), std::string("sk-keep-1"));
    auto k2 = platform::keychain::loadSecret(service, "claude:2");
    DOCTEST_REQUIRE(k2.isOk());
    DOCTEST_CHECK_EQ(k2.value(), std::string("sk-keep-2"));
    // 前缀不匹配的条目原样保留（也没被算进 deleted）
    auto other = platform::keychain::loadSecret(service, "notes:theme");
    DOCTEST_REQUIRE(other.isOk());
    DOCTEST_CHECK_EQ(other.value(), std::string("dark"));

    platform::keychain::deleteSecret(service, "claude:1");
    platform::keychain::deleteSecret(service, "claude:2");
    platform::keychain::deleteSecret(service, "notes:theme");
    return true;
})

// P0 护栏②（2026-09-30）：各平台扫描删除分支共用的白名单决策函数。
// 平台实现里只是「扫描到 account → 交给它决策」，决策语义在此逐条钉死；
// macOS 的真实扫描用例不可跑（真实 login keychain 不可隔离，见文件头），
// 这里是该守卫在 macOS 上唯一可执行的断言面。
REGISTER_TEST(keychain_orphan_sweep_prefix_whitelist,
              "keychain: 孤儿清理白名单前缀（非 claude: 一律不删）", {
    using platform::keychain::deletableOrphanAccount;
    using platform::keychain::shouldDeleteOrphan;

    // 白名单前缀命中
    DOCTEST_CHECK(deletableOrphanAccount("claude:1"));
    DOCTEST_CHECK(deletableOrphanAccount("claude:"));
    // 非白名单形态一律不删
    DOCTEST_CHECK(!deletableOrphanAccount("notes:theme"));
    DOCTEST_CHECK(!deletableOrphanAccount("claude"));    // 少冒号
    DOCTEST_CHECK(!deletableOrphanAccount("Claude:1"));  // 大小写敏感
    DOCTEST_CHECK(!deletableOrphanAccount(""));

    // 决策函数：keep 集合内 → 不删（无论前缀）；keep 外只有白名单前缀才删
    const std::set<std::string> keep{"claude:1"};
    DOCTEST_CHECK(!shouldDeleteOrphan("claude:1", keep));     // 在 keep 里
    DOCTEST_CHECK(shouldDeleteOrphan("claude:999", keep));    // 白名单孤儿 → 删
    DOCTEST_CHECK(!shouldDeleteOrphan("notes:theme", keep));  // 前缀外 → 保留
    DOCTEST_CHECK(!shouldDeleteOrphan("notes:theme", {}));    // 空 keep 也不动前缀外
    return true;
})

// ==================== macOS 扫描路径接线断言（merge-review R2，2026-09-30）====================
// 上面的纯件用例只钉 shouldDeleteOrphan 这个**决策函数**的语义，钉不住「macOS 的
// 扫描分支真的调它」——keychain_macos.cpp 的扫描循环里那行
// `if (!shouldDeleteOrphan(account, keep)) continue;` 一旦被回退成裸
// `keep.count(account) == 0`，纯件用例照样全绿：白名单前缀护栏（P0 护栏②）在
// **事故发生的那个平台**上无人覆盖。而走真实存储的扫描用例在 macOS 全部
// SKIP_ON_MACOS_KEYCHAIN（P0 教训：真 login keychain 不可隔离，照跑会写用户钥匙串、
// 按 kSecMatchLimitAll 枚举整库）——SKIP 保持不解除。
//
// 解法：keychain_macos_scan.h 测试缝只把「枚举」与「删除」两个**边界动作**换成
// 可注入函数指针（默认 nullptr = 真实 Security.framework 调用），决策循环一行
// 未动。于是这里用**注入的假 keychain 命名空间**（与 test_claude_service.cpp 的
// ScopedFakeKeychain 同款 RAII 手法，但作用于 macOS 扫描钩子）端到端钉住接线：
//   1) macOS 扫描路径确实走了 shouldDeleteOrphan —— 裸 keep.count 写法必红；
//   2) 空 keep 短路生效 —— 空 keep ≠ 通配全删，前缀外条目仍必须保留（P0 护栏②
//      最容易被绕过的一条：keep 空时全清）。
// 全程不碰真钥匙串（枚举与删除都是假的）。
#if defined(__APPLE__)
namespace {

using platform::keychain::macos_scan::Hooks;
using platform::keychain::macos_scan::ScannedEntry;

struct FakeMacosScan {
    // 假 keychain 命名空间：service → 全部 account（含跨 service 条目，用来验
    // service 过滤没被回退）
    std::vector<ScannedEntry> entries;
    std::vector<std::string> erased;
    std::string lastEnumerateService;
};

FakeMacosScan* g_fakeScan = nullptr;

core::Result<std::vector<ScannedEntry>> fakeEnumerate(const std::string& service) {
    g_fakeScan->lastEnumerateService = service;
    return core::Result<std::vector<ScannedEntry>>::ok(g_fakeScan->entries);
}

core::Result<void> fakeErase(const std::string& service,
                             const std::string& account) {
    g_fakeScan->erased.push_back(service + "/" + account);
    return core::Result<void>::ok();
}

// RAII：安装假 macOS 扫描钩子，析构恢复真实 Security.framework 调用
struct ScopedMacosScan {
    FakeMacosScan fake;
    Hooks hooks{&fakeEnumerate, &fakeErase};
    const Hooks* prev;

    ScopedMacosScan() {
        g_fakeScan = &fake;
        prev = platform::keychain::macos_scan::setHooksForTest(&hooks);
    }
    ~ScopedMacosScan() {
        platform::keychain::macos_scan::setHooksForTest(prev);
        g_fakeScan = nullptr;
    }
    ScopedMacosScan(const ScopedMacosScan&) = delete;
    ScopedMacosScan& operator=(const ScopedMacosScan&) = delete;
};

constexpr const char* kScanService = "DevDeskTestMacosScan";

} // namespace

REGISTER_TEST(keychain_macos_scan_wiring,
              "keychain: macOS 扫描路径走 shouldDeleteOrphan（接线断言，不碰真钥匙串）", {
    ScopedMacosScan scan;
    // 假命名空间：两条白名单前缀（一条 keep 内、一条孤儿）、一条前缀外
    // （notes:theme）、一条**别的 service** 的同名 account（验 service 过滤）
    scan.fake.entries = {
        {kScanService, "claude:1"},
        {kScanService, "claude:999"},
        {kScanService, "notes:theme"},
        {"OtherService", "claude:999"},
    };

    // ① keep = {claude:1} → 只删 claude:999；notes:theme（前缀外）与
    //    别的 service 的条目都不得动
    auto r = platform::keychain::realDeleteSecretsNotIn(
        kScanService, {"claude:1"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE_EQ(r.value().size(), 1u);
    DOCTEST_CHECK_EQ(r.value()[0], std::string("claude:999"));
    DOCTEST_REQUIRE_EQ(scan.fake.erased.size(), 1u);
    DOCTEST_CHECK_EQ(scan.fake.erased[0], std::string("DevDeskTestMacosScan/claude:999"));
    DOCTEST_CHECK_EQ(scan.fake.lastEnumerateService, std::string(kScanService));

    // ② 空 keep 短路：空集合**不是**「全删」。白名单前缀外条目必须原地保留
    //    （这正是 P0 护栏②要防的：keep 空时无差别全清会连带删掉同 service 下
    //    未来别的用途的条目）
    scan.fake.erased.clear();
    auto r2 = platform::keychain::realDeleteSecretsNotIn(kScanService, {});
    DOCTEST_REQUIRE(r2.isOk());
    // claude:1 / claude:999 两条都成了孤儿；notes:theme 与别的 service 不动
    DOCTEST_REQUIRE_EQ(r2.value().size(), 2u);
    DOCTEST_REQUIRE_EQ(scan.fake.erased.size(), 2u);
    bool keptNotes = false;
    bool touchedOtherService = false;
    for (const auto& e : scan.fake.erased) {
        if (e == "DevDeskTestMacosScan/notes:theme") keptNotes = true;
        if (e.rfind("OtherService/", 0) == 0) touchedOtherService = true;
    }
    DOCTEST_CHECK(!keptNotes);           // 前缀外条目绝不被删
    DOCTEST_CHECK(!touchedOtherService);  // service 过滤未被回退成「扫到就删」
    return true;
})

// 反向口径：枚举失败必须原样上报，不得静默当成「无孤儿」返回空列表
// （静默吞掉枚举失败 = 孤儿清理看着成功、实际一条没清）
REGISTER_TEST(keychain_macos_scan_enumerate_failure_propagates,
              "keychain: macOS 扫描枚举失败上报而非静默空删", {
    ScopedMacosScan scan;
    scan.fake.entries = {{kScanService, "claude:999"}};

    // 换掉枚举钩子为恒失败版（其余同 ScopedMacosScan 结构）
    struct AlwaysFailEnumerate {
        static core::Result<std::vector<ScannedEntry>> enumerate(
            const std::string&) {
            return core::Result<std::vector<ScannedEntry>>::fail(
                core::ErrorCode::Internal, "fake enumerate failure");
        }
    };
    Hooks failing{&AlwaysFailEnumerate::enumerate, &fakeErase};
    const Hooks* prev = platform::keychain::macos_scan::setHooksForTest(&failing);

    auto r = platform::keychain::realDeleteSecretsNotIn(kScanService, {});
    platform::keychain::macos_scan::setHooksForTest(prev);   // 复位在断言前

    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(scan.fake.erased.empty());   // 失败时不得删任何条目
    return true;
})
#endif  // __APPLE__

// ==================== CP-11：account → 文件名映射 + 旧命名兼容 ====================
// 下面两个用例断言的是 keychain_win.cpp 的磁盘命名契约（<encode(account)>.key），
// 只看 Windows 实现（文件存储）；macOS 走 Security.framework、Linux 走 posix 文件
// 实现，命名规则不同 → 整段用 _WIN32 门控（与 test_claude_service.cpp 同款处理）。
#if defined(_WIN32)

// 旧规则（非白名单字符一律折成 '_'）下 "claude:1" 与 "claude/1" 折叠成同一个
// claude_1.key：后写的覆盖先写的。新编码（%XX 转义）下必须落两个不同文件。
REGISTER_TEST(keychain_account_names_do_not_fold,
              "keychain: 折叠 account 落不同文件且互不覆盖", {
    const std::string service = "DevDeskTestKeychainEncode";
    const std::string colon = "claude:1";
    const std::string slash = "claude/1";   // 旧规则下与 claude:1 同文件

    platform::keychain::deleteSecret(service, colon);
    platform::keychain::deleteSecret(service, slash);

    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, colon, "sk-colon").isOk());
    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, slash, "sk-slash").isOk());

    // 两个 account 的文件并存（旧规则下只会有 1 个 claude_1.key）
    const auto dir = core::paths::appDataDir() / "keychain" / service;
    std::error_code ec;
    DOCTEST_CHECK(std::filesystem::exists(dir / "claude%3A1.key", ec));
    DOCTEST_CHECK(std::filesystem::exists(dir / "claude%2F1.key", ec));
    DOCTEST_CHECK(!std::filesystem::exists(dir / "claude_1.key", ec));

    // 互不覆盖：重写其中一个，另一个的值不动
    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, colon, "sk-colon-2").isOk());
    auto c = platform::keychain::loadSecret(service, colon);
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), std::string("sk-colon-2"));
    auto s = platform::keychain::loadSecret(service, slash);
    DOCTEST_REQUIRE(s.isOk());
    DOCTEST_CHECK_EQ(s.value(), std::string("sk-slash"));

    platform::keychain::deleteSecret(service, colon);
    platform::keychain::deleteSecret(service, slash);
    return true;
})

// 升级前写入的文件用的是旧命名规则：新编码路径缺失时必须回退读旧文件（不迁移），
// 且孤儿清理不能把「账号仍在用、只是文件名是旧形态」的条目当孤儿删掉，删除时
// 两侧都清（否则 load 会经回退把已删的密钥读回来）。
REGISTER_TEST(keychain_legacy_naming_still_readable,
              "keychain: 旧命名文件仍可读且不被孤儿清理误删", {
    const std::string service = "DevDeskTestKeychainLegacy";
    const std::string account = "legacy:probe";   // 旧规则名 = legacy_probe

    platform::keychain::deleteSecret(service, account);

    // 用新编码写一份合法密文（DPAPI 只能同机同用户解，密文必须真造），
    // 再在磁盘上改名成旧规则的名字，模拟升级前留下的文件
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, account, "sk-legacy-value").isOk());

    const auto dir = core::paths::appDataDir() / "keychain" / service;
    const auto encoded = dir / "legacy%3Aprobe.key";
    const auto legacy = dir / "legacy_probe.key";
    std::error_code ec;
    DOCTEST_REQUIRE(std::filesystem::exists(encoded, ec));
    std::filesystem::remove(legacy, ec);
    ec.clear();
    std::filesystem::rename(encoded, legacy, ec);
    DOCTEST_REQUIRE_MESSAGE(!ec, "模拟旧命名文件失败: " + ec.message());

    // 新编码路径缺失 → 回退读旧命名文件
    auto r = platform::keychain::loadSecret(service, account);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("sk-legacy-value"));

    // 孤儿清理：account 仍在 keep 集合里 → 旧命名文件必须保留
    auto swept = platform::keychain::deleteSecretsNotIn(service, {account});
    DOCTEST_REQUIRE(swept.isOk());
    DOCTEST_CHECK(swept.value().empty());
    DOCTEST_CHECK(std::filesystem::exists(legacy, ec));

    // 删除：新旧两侧都清 → load 必须 NotFound（不能经回退把密钥读回来）
    DOCTEST_REQUIRE(platform::keychain::deleteSecret(service, account).isOk());
    DOCTEST_CHECK(platform::keychain::loadSecret(service, account).isFail());
    DOCTEST_CHECK(!std::filesystem::exists(legacy, ec));
    return true;
})

#endif  // _WIN32

// ==================== CPX-14：注入缝（函数指针表）契约 ====================
// 断言缝本身：默认指向真实实现 → 安装后四个入口全部路由到注入表 → 复位后回到
// 默认。全程只用假实现 + 指针比较，不碰真实存储（不写用户钥匙串），故 macOS
// 也能跑、无需 SKIP_ON_MACOS_KEYCHAIN——缝的使用规矩见 platform/keychain.h
// 的「注入缝」段（claude_service 的两段写失败分支用例在 test_claude_service.cpp）
namespace {

struct CountingKeychain {
    int storeCalls = 0;
    int loadCalls = 0;
    int deleteCalls = 0;
    int sweepCalls = 0;
};

CountingKeychain* g_counting = nullptr;

core::Result<void> countingStore(const std::string&, const std::string&,
                                 const std::string&) {
    ++g_counting->storeCalls;
    return core::Result<void>::ok();
}

core::Result<std::string> countingLoad(const std::string&,
                                       const std::string&) {
    ++g_counting->loadCalls;
    return core::Result<std::string>::ok("fake-value");
}

core::Result<void> countingDelete(const std::string&, const std::string&) {
    ++g_counting->deleteCalls;
    return core::Result<void>::ok();
}

core::Result<std::vector<std::string>> countingSweep(
    const std::string&, const std::vector<std::string>&) {
    ++g_counting->sweepCalls;
    return core::Result<std::vector<std::string>>::ok({"swept"});
}

} // namespace

REGISTER_TEST(keychain_backend_injection,
              "keychain: CPX-14 注入缝默认真实实现且可替换复位", {
    // 默认后端 = 真实实现（生产路径的落点）
    DOCTEST_CHECK(&platform::keychain::backend() ==
                  &platform::keychain::defaultBackend());

    CountingKeychain fake;
    CountingKeychain* prevFake = g_counting;
    g_counting = &fake;
    const platform::keychain::Backend table = {&countingStore, &countingLoad,
                                               &countingDelete, &countingSweep};
    const platform::keychain::Backend* prev =
        platform::keychain::setBackendForTest(&table);
    // 安装返回被替换的旧后端 = 默认（真实）后端
    DOCTEST_CHECK(prev == &platform::keychain::defaultBackend());

    // 四个入口全部路由到注入表（account 是编造的，真实存储一字未碰）
    DOCTEST_CHECK(platform::keychain::storeSecret("S", "a", "v").isOk());
    auto l = platform::keychain::loadSecret("S", "a");
    DOCTEST_REQUIRE(l.isOk());
    DOCTEST_CHECK_EQ(l.value(), std::string("fake-value"));
    DOCTEST_CHECK(platform::keychain::deleteSecret("S", "a").isOk());
    auto swept = platform::keychain::deleteSecretsNotIn("S", {"a"});
    DOCTEST_REQUIRE(swept.isOk());
    DOCTEST_CHECK_EQ(swept.value().size(), 1u);
    DOCTEST_CHECK_EQ(fake.storeCalls, 1);
    DOCTEST_CHECK_EQ(fake.loadCalls, 1);
    DOCTEST_CHECK_EQ(fake.deleteCalls, 1);
    DOCTEST_CHECK_EQ(fake.sweepCalls, 1);

    // 复位：返回当时在位的后端，当前后端回到默认
    DOCTEST_CHECK(platform::keychain::setBackendForTest(prev) == &table);
    DOCTEST_CHECK(&platform::keychain::backend() ==
                  &platform::keychain::defaultBackend());
    g_counting = prevFake;
    return true;
})