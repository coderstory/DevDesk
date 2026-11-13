// tests/test_platform/test_git_registry_module.cpp
// git 管理器批次二（need:7）：模块登记 + 门控 + 记忆仓持久化——
//   ① 注册表行：PageId::Git 存在、门控键 kModuleIdGit、位于笔记（Markdown）之后、
//     pageIdToString/短名/图标同值三处（app_registry 契约）；
//   ② 门控默认：git 进默认启用清单（迁移 v4）+ moduleEnabled 谓词；
//   ③ 真工厂：create_git_module 返回模块，pageId/名字/图标一致；
//   ④ GitRepoStore：save/load 往返 + clear 失效清除（DEVDESK_DATA_ROOT 隔离）。
#include "doctest.h"
#include "test_support.h"

#include "app_registry.h"
#include "module_git/git_module.h"
#include "module_git/git_repo_store.h"
#include "settings_config.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using module::git::GitRepoMemory;
using module::git::GitRepoStore;
using namespace module::settings;

namespace {

// 注册表内查行（恒存在，门控键非空）
const app::registry::ModuleEntry* findGitEntry() {
    for (const auto& e : app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Git) return &e;
    }
    return nullptr;
}

} // namespace

REGISTER_TEST(gitRegistryRowAndOrder, "注册表：Git 行存在 + 门控键 + 位于笔记之后 + 三处同值", {
    const auto* entry = findGitEntry();
    DOCTEST_REQUIRE(entry != nullptr);
    if (entry == nullptr) return false;
    DOCTEST_CHECK(std::string(entry->gatingKey) == kModuleIdGit);
    DOCTEST_CHECK(std::string(entry->shortName) == "Git");

    // 位置：紧跟笔记（Markdown）之后（决策④），枚举序 = 表序
    int markdownIdx = -1, gitIdx = -1, i = 0;
    for (const auto& e : app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Markdown) markdownIdx = i;
        if (e.page == ::core::PageId::Git) gitIdx = i;
        ++i;
    }
    DOCTEST_CHECK_EQ(gitIdx, markdownIdx + 1);

    // pageIdToString 与枚举同步（core/module_interface.h）
    DOCTEST_CHECK(std::string(::core::pageIdToString(::core::PageId::Git)) == "Git");
    return true;
})

REGISTER_TEST(gitGatingDefaultEnabled, "门控：git 默认启用（迁移 v4）+ moduleEnabled 谓词 + 可被显式关闭", {
    // 默认清单含 git 且尺寸同步（本批 9→10，与 test_module_gating 断言同源维护）
    const SettingsConfig cfg;
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<size_t>(10));
    DOCTEST_CHECK(moduleEnabled(cfg, kModuleIdGit));
    DOCTEST_CHECK_EQ(kModuleDefaultsVersion, 4);
    // 显式关闭语义
    SettingsConfig off;
    off.enabledModules = {kModuleIdHome, kModuleIdSettings};
    off.userDisabledModules = {kModuleIdGit};
    DOCTEST_CHECK_FALSE(moduleEnabled(SettingsStore::sanitized(off), kModuleIdGit));
    return true;
})

REGISTER_TEST(gitModuleFactoryContract, "真工厂：create_git_module 三同值 + compose 不炸（空态页路径）", {
    std::unique_ptr<::core::Module> mod(create_git_module());
    DOCTEST_REQUIRE(mod != nullptr);
    if (mod == nullptr) return false;
    DOCTEST_CHECK(mod->pageId() == ::core::PageId::Git);
    DOCTEST_CHECK(mod->displayName() == "Git");
    DOCTEST_CHECK(mod->shortName() == "Git");
    DOCTEST_CHECK_FALSE(mod->iconGlyph().empty());
    // 首启（无记忆）compose = 空态页，无仓库路径输入也能安全出帧
    eui::Screen screen;
    screen.width = 1280.0f;
    screen.height = 720.0f;
    eui::Ui ui;
    ui.begin("git_module_smoke");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("git.empty.title") != nullptr);
    DOCTEST_CHECK(ui.find("git.empty.pick") != nullptr);
    return true;
})

REGISTER_TEST(gitRepoStoreRoundtripAndClear, "记忆仓：save/load 往返 + clear 失效清除（数据根隔离）", {
    const auto root = test_support::makeTempDir("devdesk_git_store_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    // ① 无记忆态
    DOCTEST_CHECK_FALSE(GitRepoStore::hasMemory());
    DOCTEST_CHECK(GitRepoStore::load().lastRepoPath.empty());
    // ② 往返（中文路径全链 UTF-8）
    GitRepoMemory mem;
    mem.lastRepoPath = "D:\\repos\\说明文档仓";
    DOCTEST_CHECK(GitRepoStore::save(mem).isOk());
    DOCTEST_CHECK(GitRepoStore::hasMemory());
    const auto loaded = GitRepoStore::load();
    DOCTEST_CHECK_EQ(loaded.lastRepoPath, "D:\\repos\\说明文档仓");
    // ③ clear = 空记忆落盘（N1 失效清除语义）
    DOCTEST_CHECK(GitRepoStore::clear().isOk());
    DOCTEST_CHECK_FALSE(GitRepoStore::hasMemory());
    DOCTEST_CHECK(GitRepoStore::load().lastRepoPath.empty());
    return true;
})
