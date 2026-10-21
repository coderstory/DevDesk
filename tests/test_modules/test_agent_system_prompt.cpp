// tests/test_modules/test_agent_system_prompt.cpp
// 系统提示词默认生成用例：git 分支检测（三态）+ 默认值四项齐全 + OS/架构编译期宏。
//
// 场景与防事故映射：
//   readGitBranch_normal_仓库 — 防：HEAD 格式变化后误判为非 git 仓库
//   readGitBranch_worktree — 防：worktree 的 .git 是文件而非目录，未跟进 gitdir 导致误判
//   readGitBranch_非git目录 — 防：无 .git 时崩溃或返回垃圾（返回空串 = 正确）
//   buildDefaultSystemPrompt_四项齐全 — 防：预填默认值缺项（工作目录/git分支/OS/架构漏一不可）
//   os_arch_macros_非空 — 防：平台宏定义漂移导致运行环境信息全空（测试全绿但真机无信息）
//
// 不使用真实工作树——用临时目录 fixtures 构造三种 git 状态，
// 测试结束自动清理临时目录。
#include "doctest.h"
#include "module_agent/agent_system_prompt.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// 创建临时目录并在目录中设置 git HEAD
struct GitFixture {
    std::filesystem::path dir;

    explicit GitFixture(const std::string& name) {
        dir = std::filesystem::temp_directory_path() /
              ("devdesk_sysprompt_test_" + name);
        std::filesystem::remove_all(dir, ec_);
        ec_.clear();
        std::filesystem::create_directories(dir, ec_);
    }

    ~GitFixture() {
        ec_.clear();
        std::filesystem::remove_all(dir, ec_);
    }

    // 写入普通仓库 HEAD（ref: refs/heads/<branch>）
    void writeNormalHead(const std::string& branch) {
        auto gitDir = dir / ".git";
        std::filesystem::create_directory(gitDir, ec_);
        std::ofstream f(gitDir / "HEAD");
        f << "ref: refs/heads/" << branch << "\n";
    }

    // 写入 worktree HEAD（.git 是文件指向 gitdir）。
// 注意：必须先写 .git 文件再建 gitDir 子目录——create_directories 会把 .git 建成目录
    void writeWorktreeHead(const std::string& branch,
                           const std::string& gitDirPath) {
        // 先写 .git 文件（此时 .git 尚不存在，否则 ofstream 因 .git 已是目录而静默失败）
        {
            std::ofstream f(dir / ".git");
            f << "gitdir: " << gitDirPath << "\n";
        }
        auto gitDir = std::filesystem::path(gitDirPath);
        if (gitDir.is_relative()) {
            gitDir = dir / gitDir;
        }
        std::filesystem::create_directories(gitDir, ec_);
        std::ofstream f2(gitDir / "HEAD");
        f2 << "ref: refs/heads/" << branch << "\n";
    }

    std::string pathStr() const { return dir.string(); }

private:
    std::error_code ec_;
};

}  // namespace

// ====== readGitBranch 三态用例 ======

REGISTER_TEST(readGitBranch_normal_仓库, "system_prompt: readGitBranch 正常仓库返回分支名", {
    GitFixture fx("normal");
    fx.writeNormalHead("main");
    DOCTEST_CHECK_EQ(module::agent::readGitBranch(fx.pathStr()),
                     std::string("main"));
    return true;
})

REGISTER_TEST(readGitBranch_worktree, "system_prompt: readGitBranch worktree 跟进 gitdir 取分支名", {
    GitFixture fx("wt");
    // worktree 的 .git 文件指向主仓库的真实 git 目录（不在 worktree 自身体系内）
    // 绝对路径 —— 真实场景下 gitdir 在母仓库的 .git 里，与 worktree 目录树独立
    std::string gitDir = (fx.dir / "main-repo-dotgit" / "worktrees" / "wt").string();
    fx.writeWorktreeHead("feature/test", gitDir);
    DOCTEST_CHECK_EQ(module::agent::readGitBranch(fx.pathStr()),
                     std::string("feature/test"));
    return true;
})

REGISTER_TEST(readGitBranch_非git目录, "system_prompt: readGitBranch 无 .git 返回空串", {
    GitFixture fx("nogit");
    // 不创建 .git
    DOCTEST_CHECK_EQ(module::agent::readGitBranch(fx.pathStr()),
                     std::string(""));
    return true;
})

REGISTER_TEST(readGitBranch_空目录, "system_prompt: readGitBranch 空目录返回空串", {
    GitFixture fx("empty");
    // 空目录 — 无 .git
    DOCTEST_CHECK_EQ(module::agent::readGitBranch(fx.pathStr()),
                     std::string(""));
    return true;
})

// ====== buildDefaultSystemPrompt 用例 ======

REGISTER_TEST(buildDefaultSystemPrompt_四项齐全, "system_prompt: buildDefaultSystemPrompt 包含四项环境信息", {
    std::string prompt = module::agent::buildDefaultSystemPrompt("/tmp/test-project");
    // 工作目录
    DOCTEST_CHECK_NE(prompt.find("工作目录："), std::string::npos);
    DOCTEST_CHECK_NE(prompt.find("/tmp/test-project"), std::string::npos);
    // git 分支
    DOCTEST_CHECK_NE(prompt.find("git 分支："), std::string::npos);
    // 操作系统
    DOCTEST_CHECK_NE(prompt.find("操作系统："), std::string::npos);
    // 系统架构
    DOCTEST_CHECK_NE(prompt.find("系统架构："), std::string::npos);
    return true;
})

REGISTER_TEST(buildDefaultSystemPrompt_非git目录_显示_非git仓库, "system_prompt: buildDefaultSystemPrompt 无 git 时显示「非 git 仓库」", {
    GitFixture fx("nogit2");
    std::string prompt = module::agent::buildDefaultSystemPrompt(fx.pathStr());
    DOCTEST_CHECK_NE(prompt.find("非 git 仓库"), std::string::npos);
    return true;
})

// ====== 编译期宏验证 ======

REGISTER_TEST(os_arch_macros_非空, "system_prompt: OS/架构编译期宏产出非空值", {
    std::string prompt = module::agent::buildDefaultSystemPrompt("/tmp/x");
    // 操作系统非空
    auto osPos = prompt.find("操作系统：");
    DOCTEST_CHECK_NE(osPos, std::string::npos);
    std::string afterOs = prompt.substr(osPos + 15);  // skip "操作系统："
    afterOs = afterOs.substr(0, afterOs.find('\n'));
    DOCTEST_CHECK_FALSE(afterOs.empty());
    // 架构非空
    auto archPos = prompt.find("系统架构：");
    DOCTEST_CHECK_NE(archPos, std::string::npos);
    std::string afterArch = prompt.substr(archPos + 15);  // skip "系统架构："
    afterArch = afterArch.substr(0, afterArch.find('\n'));
    DOCTEST_CHECK_FALSE(afterArch.empty());
    return true;
})

// ====== 默认提示词内容验证 ======

REGISTER_TEST(defaultPrompt_可编辑原则, "system_prompt: buildDefaultSystemPrompt 产物是纯文本、不含标记语言", {
    std::string prompt = module::agent::buildDefaultSystemPrompt("/tmp/y");
    // 不含 JSON/XML/Markdown 标记
    DOCTEST_CHECK_EQ(prompt.find('{'), std::string::npos);
    DOCTEST_CHECK_EQ(prompt.find('<'), std::string::npos);
    DOCTEST_CHECK_EQ(prompt.find('#'), std::string::npos);  // 无 markdown 标题
    // 首行含 "DevDesk 智能体"
    DOCTEST_CHECK_NE(prompt.find("DevDesk"), std::string::npos);
    return true;
})