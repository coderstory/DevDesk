// tests/test_modules/test_module_gating.cpp
// 模块插拔单测：enabledModules 配置语义（缺省/往返/脏数据/未知 ID）+ 注册门控 + Home 降级渲染。
// 跨文件前提：
//   - home 与 settings 恒启用：moduleEnabled 谓词里硬编码，sanitized 也会强制补齐，
//     两者都不是配置里的普通项，从 enabledModules 里删掉也照样开着。
//   - agent 默认关（跑 LLM 有成本）、wallpaper 默认关；memory 默认开（引擎未装时只显
//     引导态）、release/schedule/terminal/git 默认开。默认清单是 10 项，改动会让本文件
//     的 size()==10 断言红（need:7 批次二 git 入清单迁移 v4）。
//   - 生产装配在 main.cpp getRouter()，本文件的注册段是它的复制品；改了那边这里不会自动跟。
#include "doctest.h"
#include "test_support.h"
#include "settings_config.h"

#include "core/app_config.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "app_registry.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_home_module();

namespace {

class StubModule : public ::core::Module {
public:
    StubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

// 每个用例独立的临时配置路径 —— 共享工厂，只造路径不落盘。
// 配置真落在 app_config 表：各用例里那对 tempConfigFile / filesystem::remove 都是空操作，
// 用例间的隔离靠 TestEnv 换库 + seedConfigJsonRows 的整份替换语义。
constexpr const char* kCfgPrefix = "devdesk_test_gating_";

// 把「手改过的配置」按顶层键分发进 app_config 表各行（顶层键 = 行键）。
// 返回 bool 而非直接断言：doctest 宏展开后含 `return false`，只能在返回 bool 的
// 函数里用，所以断言留给调用方。
bool writeRawConfig(const std::string& json) {
    return test_support::seedConfigJsonRows(
        ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
        json);
}

// 手改配置若模拟「当前版本」写出的内容，必须带迁移标记——否则 load 按存量升级
// 路径把 release 并入清单（见 gating_module_defaults_migration），下面的过滤/
// 补齐断言会被迁移行为污染。body 是 JSON 对象的其余字段（逗号分隔）。
std::string currentVersionSeed(const std::string& body) {
    return "{\"moduleDefaultsVersion\": " +
           std::to_string(module::settings::kModuleDefaultsVersion) + ", " + body +
           "}";
}

} // namespace

// enabledModules 配置

REGISTER_TEST(gating_config_defaults, "gating: enabledModules 缺省默认清单（agent 默认关）+ 恒启用项", {
    using namespace module::settings;
    const SettingsConfig cfg;  // 默认构造 = 字段缺失的回退语义
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<size_t>(10));
    for (const char* id : {module::settings::kModuleIdHome,
                           module::settings::kModuleIdMarkdown,
                           module::settings::kModuleIdGit,
                           module::settings::kModuleIdTodo,
                           module::settings::kModuleIdSchedule,
                           module::settings::kModuleIdClaudeProxy,
                           module::settings::kModuleIdMemory,
                           module::settings::kModuleIdTerminal,
                           module::settings::kModuleIdSettings,
                           module::settings::kModuleIdRelease}) {
        DOCTEST_CHECK(moduleEnabled(cfg, id));
    }
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdAgent));
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdWallpaper));
    // wallpaper 是已知 ID：sanitized 保留显式启用，但绝不自动补进默认清单
    SettingsConfig withWallpaper = cfg;
    withWallpaper.enabledModules.push_back(kModuleIdWallpaper);
    DOCTEST_CHECK(moduleEnabled(SettingsStore::sanitized(withWallpaper),
                                kModuleIdWallpaper));
    SettingsConfig stripped = cfg;
    stripped.enabledModules.assign(1, kModuleIdMarkdown);
    const SettingsConfig fixed = SettingsStore::sanitized(stripped);
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdSettings));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdWallpaper));
    // 未知 ID 恒 false（防手改 config 的兜底）
    DOCTEST_CHECK(!moduleEnabled(fixed, "bogus_module"));
    return true;
})

// B1（T01 审计）：存量配置升级路径。旧版本写出的清单没有 moduleDefaultsVersion
// 标记，其缺席的 release 代表「从未存在」→ 迁移并入；用户显式关闭一定发生在
// 迁移之后（配置已带当前标记），绝不能被重新打开。
// need:122 P0 全链：sanitize→load→迁移并入 terminal→注册表行存在。堵「默认清单
// size 断言更新了但白名单/迁移表漏 terminal」的覆盖缺口（code-review P0）。
REGISTER_TEST(gating_terminal_full_chain,
              "gating: terminal 全链——v2 存量 load 并入（迁移 v3）+ sanitize 保留 + 注册表行存在", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "terminal-chain", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    // ① v2 存量形态（带 v2 标记、无 terminal）→ load 迁移并入 terminal
    // 池 bug246 起自校验按内容核对：本清单连 v2 引入的 schedule 也缺（漂移态），
    // 所以 schedule + terminal + memory 一起补；need:7 批次二起 git（迁移 v4）
    // 也并入 = 10 项。
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"settings\", \"memory\", \"release\"], "
        "\"moduleDefaultsVersion\": 2}"));
    const SettingsConfig upgraded = store.load();
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdTerminal));
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdSchedule));
    DOCTEST_CHECK_EQ(upgraded.enabledModules.size(), static_cast<size_t>(10));
    DOCTEST_CHECK_EQ(test_support::readRawConfigRow(::core::appDatabase(),
                                                    (std::string(SettingsStore::keyNamespace()) +
                                                     "moduleDefaultsVersion")
                                                        .c_str()),
                     std::to_string(kModuleDefaultsVersion));

    // ② sanitize：terminal 是合法 ID，显式关闭后 sanitized 不得自动补回
    SettingsConfig userOff;
    userOff.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
                              kModuleIdClaudeProxy, kModuleIdSettings};
    const SettingsConfig fixed = SettingsStore::sanitized(userOff);
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdTerminal));

    // ③ 注册表：terminal 行存在且门控键正确
    bool hasTerminalRow = false;
    for (const auto& entry : ::app::registry::kModuleRegistry) {
        if (entry.page == ::core::PageId::Terminal) {
            hasTerminalRow = entry.gatingKey != nullptr &&
                             std::string(entry.gatingKey) == kModuleIdTerminal;
        }
    }
    DOCTEST_CHECK(hasTerminalRow);
    return true;
})

REGISTER_TEST(gating_module_defaults_migration,
              "gating: 存量 5 项配置升级 → release/schedule 默认启用并写版本标记；关掉后不再重开", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "migrate", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());
    const std::string versionKey =
        std::string(SettingsStore::keyNamespace()) + "moduleDefaultsVersion";

    // ① 旧版本形态：5 项清单、无版本标记
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"settings\"]}"));
    const SettingsConfig upgraded = store.load();
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdRelease));  // 升级即开
    // v2 迁移：schedule（此前恒注册）并入存量清单
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdSchedule));
    // 池 bug249：memory 也登了迁移行 → 存量漂移机器升级即补开（本单要的效果）
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdMemory));
    // need:7 批次二：git 登迁移行 v4 → 存量升级即并入
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdGit));
    DOCTEST_CHECK_EQ(upgraded.enabledModules.size(), static_cast<size_t>(10));
    // 迁移标记已回写 → 下次 load 不再迁移
    DOCTEST_CHECK_EQ(test_support::readRawConfigRow(::core::appDatabase(),
                                                    versionKey.c_str()),
                     std::to_string(kModuleDefaultsVersion));

    // ② 用户显式关掉 release/schedule/terminal/git 后落盘 → 再 load 不得重开。
    // 池 bug246 起「显式关闭」是两笔写入（清单删 ID + 豁免集加 ID，由
    // applyModuleEnabled 一次完成），这里按同一契约模拟。
    SettingsConfig userOff;
    userOff.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
                              kModuleIdClaudeProxy, kModuleIdSettings};
    userOff.userDisabledModules = {kModuleIdRelease, kModuleIdSchedule,
                                   kModuleIdTerminal, kModuleIdMemory,
                                   kModuleIdGit};
    DOCTEST_REQUIRE(store.save(userOff).isOk());
    const SettingsConfig reloaded = store.load();
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdRelease));
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdSchedule));
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdTerminal));
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdMemory));
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdGit));
    DOCTEST_CHECK_EQ(reloaded.enabledModules.size(), static_cast<size_t>(5));

    // ③ 旧配置里用户关掉的既有模块也不得被迁移连带重开
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"settings\"]}"));
    const SettingsConfig sparse = store.load();
    DOCTEST_CHECK(moduleEnabled(sparse, kModuleIdRelease));
    DOCTEST_CHECK_FALSE(moduleEnabled(sparse, kModuleIdTodo));
    DOCTEST_CHECK_FALSE(moduleEnabled(sparse, kModuleIdMarkdown));
    return true;
})

// ── 池 bug246：默认清单内容自校验（版本号到位但清单漂移少模块也能自愈）──

// 钉子①：version 已到位（== 当前值）但清单缺 terminal → load 必须补回并回写。
// 这是线上真实形态（某次 save 把不含 terminal 的内存态整份写回，版本号已是 3）；
// 旧实现只在 version 落后时迁移，此态永不修复。
REGISTER_TEST(gating_reconcile_when_version_up_to_date,
              "gating: bug246——moduleDefaultsVersion 已到位但清单缺 terminal → load 自校验补回并回写", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"memory\", \"settings\", \"release\", "
        "\"schedule\"], \"moduleDefaultsVersion\": " +
        std::to_string(kModuleDefaultsVersion) + "}"));
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTerminal));   // 补回
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdGit));        // 同批迁移行一并补回
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(10));
    // 内存态之外还要真回写：重启后不再依赖再次自校验
    const std::string modsRow =
        test_support::readRawConfigRow(
            ::core::appDatabase(),
            (std::string(SettingsStore::keyNamespace()) + "enabledModules")
                .c_str());
    DOCTEST_CHECK(modsRow.find("terminal") != std::string::npos);
    return true;
})

// 钉子②：用户显式关闭的默认模块不被自校验重开。豁免记录（userDisabledModules）
// 由**用户关闭动作**写入（settings_module::applyModuleEnabled 双写：清单删 ID +
// 豁免集加 ID）；这里按同一契约模拟该动作，再走 save → load 全链。
REGISTER_TEST(gating_reconcile_respects_user_disabled,
              "gating: bug246——用户显式关闭 terminal（清单删+豁免加）→ 再 load 不重开", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile-useroff", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    // 先落一份完整默认清单（含 terminal），再模拟用户关掉 terminal 后 save
    DOCTEST_REQUIRE(store.save(SettingsConfig{}).isOk());
    SettingsConfig userOff = store.load();
    DOCTEST_REQUIRE(moduleEnabled(userOff, kModuleIdTerminal));
    userOff.enabledModules.erase(
        std::remove(userOff.enabledModules.begin(), userOff.enabledModules.end(),
                    std::string(kModuleIdTerminal)),
        userOff.enabledModules.end());
    userOff.userDisabledModules.push_back(kModuleIdTerminal);   // 用户动作的第二笔
    DOCTEST_REQUIRE(store.save(userOff).isOk());

    // 豁免集已落库
    const std::string exemptRow = test_support::readRawConfigRow(
        ::core::appDatabase(),
        (std::string(SettingsStore::keyNamespace()) + "userDisabledModules")
            .c_str());
    DOCTEST_CHECK(exemptRow.find("terminal") != std::string::npos);

    // 重新 load：terminal 仍是关的（自校验跳过豁免项）
    const SettingsConfig reloaded = store.load();
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdTerminal));
    // 同一次 load 的其它默认模块不受影响（没有顺手把整表重开）
    DOCTEST_CHECK(moduleEnabled(reloaded, kModuleIdRelease));
    DOCTEST_CHECK(moduleEnabled(reloaded, kModuleIdSchedule));

    // 用户又重新打开 terminal → 豁免自动失效（sanitized 归一），关它就还能正常关
    SettingsConfig back = store.load();
    back.enabledModules.push_back(kModuleIdTerminal);
    DOCTEST_REQUIRE(store.save(back).isOk());
    DOCTEST_CHECK(moduleEnabled(store.load(), kModuleIdTerminal));
    const std::string exemptAfter = test_support::readRawConfigRow(
        ::core::appDatabase(),
        (std::string(SettingsStore::keyNamespace()) + "userDisabledModules")
            .c_str());
    DOCTEST_CHECK(exemptAfter.find("terminal") == std::string::npos);
    return true;
})

// 钉子②补：漂移态不得被当成「用户选择」固化——清单缺 terminal 且**无**豁免记录时，
// save（被侧栏折叠/退出存窗体等后台路径调用）不能替用户生成豁免，否则自校验会
// 永远放弃补回该模块（正是 bug246 的成因）。
REGISTER_TEST(gating_reconcile_drift_not_laundered_into_exemption,
              "gating: bug246——清单漂移缺 terminal 且无豁免 → save 不自动生成豁免，随后 load 补回", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile-drift", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    // 漂移态：版本号到位、清单缺 terminal、无豁免记录
    const std::string drifted =
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"memory\", \"settings\", \"release\", "
        "\"schedule\"], \"moduleDefaultsVersion\": " +
        std::to_string(kModuleDefaultsVersion) + "}";
    DOCTEST_REQUIRE(writeRawConfig(drifted));

    // 后台路径的典型写法：load → 改无关字段 → save（此处的 load 会先自校验补回，
    // 因此再按漂移态重铺一次，模拟「读到的就是缺 terminal 的快照」）
    DOCTEST_REQUIRE(writeRawConfig(drifted));
    SettingsConfig snap;
    snap.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
                           kModuleIdClaudeProxy, kModuleIdMemory,
                           kModuleIdSettings, kModuleIdRelease,
                           kModuleIdSchedule};
    snap.sidebarCollapsed = true;   // 无关字段，模拟侧栏折叠的 save
    DOCTEST_REQUIRE(store.save(snap).isOk());
    const std::string exemptRow = test_support::readRawConfigRow(
        ::core::appDatabase(),
        (std::string(SettingsStore::keyNamespace()) + "userDisabledModules")
            .c_str());
    DOCTEST_CHECK(exemptRow.find("terminal") == std::string::npos);  // 未替用户记账

    // 随后 load 照常自校验补回
    DOCTEST_CHECK(moduleEnabled(store.load(), kModuleIdTerminal));
    return true;
})

// 池 bug249：memory 是「后转默认」模块（f343f436 改默认清单时漏登迁移行），漂移
// 机器一直补不回。登行后与 terminal 同语义：自校验补回 + 豁免集尊重（两钉分列）。
REGISTER_TEST(gating_reconcile_memory_missing_readded,
              "gating: bug249——版本到位清单缺 memory 且无豁免 → load 自校验补回", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile-mem", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    // 漂移态：版本号到位、清单缺 memory、无豁免记录（种子形态同 bug246 钉子）
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"settings\", \"release\", \"schedule\"], "
        "\"moduleDefaultsVersion\": " +
        std::to_string(kModuleDefaultsVersion) + "}"));
    DOCTEST_CHECK(moduleEnabled(store.load(), kModuleIdMemory));
    // 既有迁移行同一次 load 一并核对，不受新行影响
    DOCTEST_CHECK(moduleEnabled(store.load(), kModuleIdTerminal));
    return true;
})

REGISTER_TEST(gating_reconcile_memory_exempt_stays_off,
              "gating: bug249——用户显式关闭 memory（豁免集含 memory）→ load 不重开", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile-mem-off", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());

    // 模拟用户关闭动作双写（清单删 ID + 豁免集加 ID，同 settings_module 契约）
    SettingsConfig cfg;
    cfg.enabledModules.erase(
        std::remove(cfg.enabledModules.begin(), cfg.enabledModules.end(),
                    std::string(kModuleIdMemory)),
        cfg.enabledModules.end());
    cfg.userDisabledModules.push_back(kModuleIdMemory);
    DOCTEST_REQUIRE(store.save(cfg).isOk());

    const SettingsConfig reloaded = store.load();
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdMemory));
    // 其它默认模块不被连带重开/误伤
    DOCTEST_CHECK(moduleEnabled(reloaded, kModuleIdTerminal));
    DOCTEST_CHECK(moduleEnabled(reloaded, kModuleIdRelease));
    DOCTEST_CHECK(moduleEnabled(reloaded, kModuleIdSchedule));
    return true;
})

// 钉子③：正常路径（清单已完整）零写放大——load 不得因自校验触发任何库写入。
// 判据 = SqliteDb 写版本号（每次真写 +1，读/只读语句不动）。
REGISTER_TEST(gating_reconcile_no_write_amplification,
              "gating: bug246——清单完整时 load 自校验零写盘（写版本号不变）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "reconcile-nowrite", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());
    DOCTEST_REQUIRE(store.save(SettingsConfig{}).isOk());  // 完整默认清单

    const uint64_t before = env.handle().writeVersion();
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTerminal));
    DOCTEST_CHECK_EQ(env.handle().writeVersion(), before);   // 零写

    // 反证：缺模块的库 load 必须真写（否则上一断言可能是「根本没写路径」）
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"memory\", \"settings\", \"release\", "
        "\"schedule\"], \"moduleDefaultsVersion\": " +
        std::to_string(kModuleDefaultsVersion) + "}"));
    const uint64_t beforeFix = env.handle().writeVersion();
    (void)store.load();
    DOCTEST_CHECK(env.handle().writeVersion() > beforeFix);
    return true;
})

REGISTER_TEST(gating_config_roundtrip, "gating: enabledModules 保存-加载往返", {
    using namespace module::settings;
    // registerAppDb 不可省：SettingsStore 无参构造经 ::core::appDatabase() 取连接，
    // 未注册时 load() 直接失败，下面的断言会全部走回退分支而绿。
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    SettingsConfig cfg;
    cfg.enabledModules = {kModuleIdMarkdown, kModuleIdHome, kModuleIdSettings};
    // 默认模块全关 → 按用户关闭契约记豁免（否则 load 自校验会补回，池 bug246/249）
    cfg.userDisabledModules = {kModuleIdRelease, kModuleIdSchedule,
                               kModuleIdTerminal, kModuleIdMemory, kModuleIdGit};
    const SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK(store.save(cfg).isOk());
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdClaudeProxy));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_unknown_ids_filtered, "gating: 未知模块 ID 忽略 + 恒启用补齐", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "unknown");
    // 手改 config 写入未知 ID + 漏掉恒启用项 + 重复项。
    // 默认模块（release/schedule/terminal/memory/git）缺席要显式记进豁免集，否则 load 的
    // 内容自校验（池 bug246/249）会把它们补回来，下面的条数断言就测不到过滤本意了。
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed(
        "\"enabledModules\": [\"home\", \"todo\", \"bogus\", \"todo\"], "
        "\"userDisabledModules\": [\"release\", \"schedule\", \"terminal\", "
        "\"memory\", \"git\"]")));
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));  // 补齐
    DOCTEST_CHECK(!moduleEnabled(loaded, "bogus"));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_empty_array, "gating: 空数组 = 四工具模块全关（home/settings 补齐）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "empty");
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed("\"enabledModules\": []")));
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdClaudeProxy));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdAgent));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_corrupt_falls_back, "gating: 字段损坏回退默认全启用", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "corrupt");
    // 数组里的非字符串项被跳过，但数组本身有效 → 列表按字面生效（只含 todo）
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed(
        "\"enabledModules\": [\"todo\", 42, true]")));
    SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    // 整份内容非 JSON → 全默认（默认清单；agent 默认关）
    // 走原始行通道而非 writeRawConfig：seedConfigJsonRows 要求顶层是对象，
    // 遇到非对象返回 false，「值本身是坏的」这条负例过不去。
    const std::string kEnabledModulesRow =
        std::string(module::settings::SettingsStore::keyNamespace()) +
        "enabledModules";
    DOCTEST_REQUIRE(core::appconfig::eraseWithPrefixExcept(
                        *::core::appDatabase(),
                        module::settings::SettingsStore::keyNamespace(), {})
                        .isOk());
    DOCTEST_REQUIRE(test_support::writeRawConfigRow(
        ::core::appDatabase(), kEnabledModulesRow.c_str(), "{not json at all"));
    loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(10));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMemory));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSchedule));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdRelease));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTerminal));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdGit));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

// 注册门控

REGISTER_TEST(gating_registration_respects_config, "gating: 关闭模块不注册进 Router（modules() 不含它）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "registration");
    SettingsConfig cfg;
    cfg.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdSettings};
    // 默认模块全关 → 按用户关闭契约记豁免（否则 load 自校验会补回，池 bug246/249）
    cfg.userDisabledModules = {kModuleIdRelease, kModuleIdSchedule,
                               kModuleIdTerminal, kModuleIdMemory};
    DOCTEST_CHECK(SettingsStore(::core::appDatabase()).save(cfg).isOk());

    // 复制 main.cpp getRouter() 的装配门控；那边改了这里不会自动跟
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule todo(::core::PageId::Todo, "todo");
    StubModule schedule(::core::PageId::Schedule, "schedule");
    StubModule claude(::core::PageId::ClaudeProxy, "claude");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);  // 恒注册
    if (moduleEnabled(loaded, kModuleIdMarkdown)) router.registerModule(&markdown);
    if (moduleEnabled(loaded, kModuleIdTodo)) router.registerModule(&todo);
    if (moduleEnabled(loaded, kModuleIdSchedule)) router.registerModule(&schedule);
    if (moduleEnabled(loaded, kModuleIdClaudeProxy)) router.registerModule(&claude);
    router.registerModule(&settings);  // 恒注册

    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(3));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Home));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Markdown));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Settings));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Todo));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Schedule));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::ClaudeProxy));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

// Home 降级渲染

REGISTER_TEST(gating_home_all_modules_off, "gating: Home 三模块全关 → 卡片/按钮全隐藏 + 空态文案", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_all_off");
    mod->compose(ui, screen);
    ui.end();

    // 5 张统计卡全隐
    DOCTEST_CHECK(ui.find("home.stat.tokens") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.notes") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.todo") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.overdue") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.api") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    // 一个工具模块都没注册时的兜底文案
    const auto* empty = ui.find("home.stats.alloff");
    DOCTEST_REQUIRE(empty != nullptr);
    DOCTEST_CHECK(empty->text.find("所有工具模块已关闭") != std::string::npos);
    return true;
})

REGISTER_TEST(gating_home_partial_modules, "gating: Home 部分模块关闭 → 对应卡隐藏、其余保留", {
    // 只启用 markdown（todo/claude 关闭）
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_md_only");
    mod->compose(ui, screen);
    ui.end();

    // 卡片可见性按目标模块是否注册，跨模块不互相牵连
    DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") != nullptr);
    DOCTEST_CHECK(ui.find("home.stat.tokens") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.api") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.todo") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.overdue") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    // 部分启用不出空态文案
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})

REGISTER_TEST(gating_home_all_on_unchanged, "gating: 全注册时 Home 与现状等价（5 卡 + 3 按钮）", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule todo(::core::PageId::Todo, "todo");
    StubModule claude(::core::PageId::ClaudeProxy, "claude");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&todo);
    router.registerModule(&claude);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_all_on");
    mod->compose(ui, screen);
    ui.end();

    for (const char* id : {"home.stat.tokens", "home.stat.notes", "home.stat.todo",
                           "home.stat.overdue", "home.stat.api",
                           "home.act.newnote", "home.act.newtodo", "home.act.claude"}) {
        DOCTEST_CHECK(ui.find(id) != nullptr);
    }
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})

// G4 补缺（docs/superpowers/plans/2026-10-19-ui-test-gap-map.md）：home.actions 与
// 模块注册态的**联动**——既有用例只断按钮在场/缺席，这里钉三档收缩几何：
//   ① 全注册：三按钮等宽（UAT2 覆盖 W9 不等宽：权重全 1.0），行宽 = contentW；
//   ② 只剩 markdown：幸存按钮独占整行（宽 = ①行宽 = contentW，权重归一）；
//   ③ 全关：home.actions 整行元素不存在（不是空行占位）。
// contentW 两次 compose 间不变（同 screen 尺寸同代码路径），①②可互推。
REGISTER_TEST(gating_home_actions_row_collapse,
              "gating: Home 快捷按钮联动收缩（等宽）/ 全关整行隐藏", {
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    float rowW = 0.0f;  // ① 行宽（= contentW，跨档复算用）

    // ① 全注册：三按钮等宽（UAT2 覆盖 W9 不等宽：权重全 1.0）
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule markdown(::core::PageId::Markdown, "markdown");
        StubModule todo(::core::PageId::Todo, "todo");
        StubModule claude(::core::PageId::ClaudeProxy, "claude");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&markdown);
        router.registerModule(&todo);
        router.registerModule(&claude);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_all3");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);

        const auto* row = ui.find("home.actions");
        DOCTEST_REQUIRE(row != nullptr);
        const auto* note = ui.find("home.act.newnote");
        const auto* td = ui.find("home.act.newtodo");
        const auto* cl = ui.find("home.act.claude");
        DOCTEST_REQUIRE(note != nullptr);
        DOCTEST_REQUIRE(td != nullptr);
        DOCTEST_REQUIRE(cl != nullptr);
        rowW = row->frame.width;  // = contentW
        DOCTEST_CHECK(rowW > 0.0f);
        // UAT2 覆盖 W9：三按钮等宽（权重全 1.0），newnote == newtodo == claude
        DOCTEST_CHECK(std::fabs(td->frame.width - note->frame.width) < 0.5f);
        DOCTEST_CHECK(std::fabs(cl->frame.width - note->frame.width) < 0.5f);
        // 行宽 = 三按钮宽 + 2×16 gap
        DOCTEST_CHECK(std::fabs(row->frame.width -
                                (note->frame.width + td->frame.width +
                                 cl->frame.width + 32.0f)) < 1.0f);
    }

    // ② 只剩 markdown：newnote 独占整行，兄弟按钮消失
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule markdown(::core::PageId::Markdown, "markdown");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&markdown);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_md_only");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);

        const auto* row = ui.find("home.actions");
        DOCTEST_REQUIRE(row != nullptr);
        const auto* note = ui.find("home.act.newnote");
        DOCTEST_REQUIRE(note != nullptr);
        DOCTEST_REQUIRE_MESSAGE(
            std::fabs(note->frame.width - rowW) < 1.0f,
            "关掉兄弟模块后幸存按钮未独占整行: " +
                std::to_string(note->frame.width) + " vs 期望 " +
                std::to_string(rowW));
        DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    }

    // ③ 全关：整行隐藏
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_none");
        mod->compose(ui, screen);
        ui.end();

        DOCTEST_CHECK(ui.find("home.actions") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.newnote") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    }
    return true;
})
