// src/module_git/git_repo_store.h
// git 管理器记忆仓（need:7 批次二；技术方案 2026-11-13 §1 决策⑥）：
// 上次打开仓库持久化，落在 core::paths 数据根 git_manager.json（yyjson 薄封装读写）。
// 失效清除（F-14③/N1）：路径不存在或 open 失败 → 调用方 clear() 抹掉条目回到空态页。
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

namespace module::git {

// 记忆仓内容（v1 只有一项；后续批次加视图状态时扩字段，键缺省回退默认）
struct GitRepoMemory {
    std::string lastRepoPath;  // UTF-8 绝对路径；空 = 无记忆
};

class GitRepoStore {
public:
    // <数据根>/git_manager.json（数据根 = core::paths::appDataDir()，
    // 生产 ~/DevDesk，测试经 DEVDESK_DATA_ROOT 覆盖——core/paths 既有口径）
    static std::filesystem::path storeFile();

    // 读取。文件不存在 / 损坏 / 字段缺失 → 空 GitRepoMemory（不视为错误，N1 回空态页）
    static GitRepoMemory load();

    // 写入（整文件覆盖）。库不可写返回 fail，调用方记日志不阻断（记忆是最佳努力）
    static core::Result<void> save(const GitRepoMemory& memory);

    // 清除（失效路径 / 用户主动）：落一个 lastRepoPath 为空的文件
    static core::Result<void> clear();

    // 记忆是否有内容（load().lastRepoPath 非空）
    static bool hasMemory();
};

} // namespace module::git
