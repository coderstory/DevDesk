// 新建智能体任务的默认系统提示词生成：环境上下文注入（工作目录、git 分支、OS、架构）。
// 参照 Claude Code 的内置环境注入设计——用户可编辑，只在新建弹窗打开时预填。
//
// 纯函数：无 IO 之外的副作用、不摸 UI/DB/网络。全部平台分叉用编译期宏：
//   _WIN32 / __APPLE__    → 操作系统名
//   __x86_64__ / __aarch64__ → 系统架构
// git 分支检测走文件 IO 读 .git/HEAD，禁 spawn git 子进程——跨平台零依赖。
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace module::agent {

// 读 <workdir>/.git/HEAD 解析当前分支名。
//
// 三态处理（与需求三态对齐）：
//   - 正常仓库：HEAD 是文件，内容 "ref: refs/heads/<name>" → 返回分支名
//   - worktree / 子模块：.git 是文件，内容 "gitdir: <path>" → 跟过去再读 <path>/HEAD
//   - 无 .git → 返回空串（调用方转"非 git 仓库"文案）
//
// gitdir 路径含相对性时相对 workdir 解析（git worktree 的标准行为）。
inline std::string readGitBranch(const std::string& workdir) {
    namespace fs = std::filesystem;
    fs::path dotGit = fs::path(workdir) / ".git";
    std::error_code ec;
    size_t pos = std::string::npos;

    // 情况 1：.git 是普通文件 → worktree 或子模块
    if (fs::is_regular_file(dotGit, ec) && !ec) {
        std::ifstream f(dotGit);
        if (!f.is_open()) return "";
        std::string line;
        if (!std::getline(f, line)) return "";
        // 格式："gitdir: /path/to/.git" ——去掉前缀白空格，取冒号后面
        const char* kPrefix = "gitdir:";
        pos = line.find(kPrefix);
        if (pos == std::string::npos) return "";
        std::string gitDirPath = line.substr(pos + 7);  // strlen("gitdir:")
        // 去首尾空白
        while (!gitDirPath.empty() && (gitDirPath.front() == ' ' || gitDirPath.front() == '\t'))
            gitDirPath.erase(0, 1);
        while (!gitDirPath.empty() && (gitDirPath.back() == ' ' || gitDirPath.back() == '\t' ||
                                       gitDirPath.back() == '\r' || gitDirPath.back() == '\n'))
            gitDirPath.pop_back();
        // 相对路径 → 相对 workdir 解析
        fs::path resolved = gitDirPath;
        if (resolved.is_relative()) {
            resolved = fs::path(workdir) / resolved;
        }
        dotGit = resolved;
    }

    // 情况 2：.git 是目录 → 正常仓库；情况 3：不存在 → 已在上面跳过
    if (!fs::exists(dotGit, ec) || ec) return "";
    fs::path headFile = dotGit / "HEAD";
    if (!fs::is_regular_file(headFile, ec) || ec) return "";

    std::ifstream f(headFile);
    if (!f.is_open()) return "";
    std::string line;
    if (!std::getline(f, line)) return "";

    // HEAD 格式："ref: refs/heads/<branch>" 或 detached HEAD（hash）
    const char* kRefPrefix = "ref: refs/heads/";
    pos = line.find(kRefPrefix);
    if (pos == std::string::npos) return "";  // detached HEAD → 空
    std::string branch = line.substr(pos + 16);  // strlen(kRefPrefix)
    // 去尾随空白/换行
    while (!branch.empty() && (branch.back() == '\r' || branch.back() == '\n'))
        branch.pop_back();
    return branch;
}

// 生成新建任务的默认系统提示词（四项环境信息）。
// workdir：任务工作目录（应用启动时的 CWD）。
inline std::string buildDefaultSystemPrompt(const std::string& workdir) {
    std::string osName;
#if defined(_WIN32)
    osName = "Windows";
#elif defined(__APPLE__)
    osName = "macOS";
#else
    osName = "Linux";
#endif

    std::string archName;
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    archName = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    archName = "arm64";
#else
    archName = "未知架构";
#endif

    std::string branch = readGitBranch(workdir);
    std::string branchText = branch.empty() ? "非 git 仓库" : branch;

    std::string prompt;
    prompt += "你是 DevDesk 智能体，运行环境如下：\n";
    prompt += "- 工作目录：" + workdir + "\n";
    prompt += "- git 分支：" + branchText + "\n";
    prompt += "- 操作系统：" + osName + "\n";
    prompt += "- 系统架构：" + archName + "\n";
    return prompt;
}

}  // namespace module::agent