// src/module_git/git_repo_store.h
// git 管理器记忆仓（need:7 批次二；技术方案 2026-11-13 §1 决策⑥）：
// 上次打开仓库 + 最近仓历史持久化，落在 core::paths 数据根 git_manager.json
//（yyjson 薄封装读写）。失效清除（F-14③/N1）：路径不存在或 open 失败 → 调用方
// clear() 抹掉条目回到空态页。最近仓历史（need299 §3.1）：≤8 条有序 LRU，按
// lexically_normal 折叠小写键去重——控制器在 openRepo 成功时前插维护，本类只存取。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::git {

// 记忆仓内容；后续批次加视图状态时扩字段，键缺省回退默认
struct GitRepoMemory {
    std::string lastRepoPath;  // UTF-8 绝对路径；空 = 无记忆
    std::vector<std::string> recentRepos;  // 最近仓历史（新→旧，≤kMaxRecentRepos）
};

class GitRepoStore {
public:
    // 最近仓历史上限（设计 §3.1/§12-5：下拉 320px 内滚阈 = 8 条，超出 LRU 淘汰）
    static constexpr size_t kMaxRecentRepos = 8;

    // <数据根>/git_manager.json（数据根 = core::paths::appDataDir()，
    // 生产 ~/DevDesk，测试经 DEVDESK_DATA_ROOT 覆盖——core/paths 既有口径）
    static std::filesystem::path storeFile();

    // 读取。文件不存在 / 损坏 / 字段缺失 → 缺省 GitRepoMemory（不视为错误）
    static GitRepoMemory load();

    // 写入（整文件覆盖）。库不可写返回 fail，调用方记日志不阻断（记忆是最佳努力）
    static core::Result<void> save(const GitRepoMemory& memory);

    // 清除（失效路径 / 用户主动）：落一个全空字段的文件
    static core::Result<void> clear();

    // 记忆是否有内容（load().lastRepoPath 非空）
    static bool hasMemory();
};

} // namespace module::git
