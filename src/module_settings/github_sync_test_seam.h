// src/module_settings/github_sync_test_seam.h
// GitHub Sync 真机验收测试缝（唯一消费方 tests/live/github_sync_live.cpp）。
//
// 为什么需要它：同步引擎是 SettingsModule 的私有成员，而 SettingsModule 是
// UI 模块（compose 驱动、动作由按钮触发）——无头驱动不到。这里按
// github_sync_exit_hook.h 的同款形态（模块实例指针桥 + 自由函数）把
// 「跑一次动作 + 读回结果」暴露出去。
//
// 门禁：整组缝只在 DEVDESK_GITHUB_TEST_MODE=1 时生效（防生产误用）；
// 未开时 githubSyncTestSeamAvailable() 返 false、githubSyncRunForTest 恒返
// ran=false，驱动据此拒绝跑。
#pragma once

#include <string>
#include <vector>

namespace module::settings {

// 动作集 = 引擎 SyncAction 的测试镜像（避免把内部枚举泄进头文件）。
enum class GithubSyncTestAction {
    Push,
    Pull,
    PullThenPush,
    FirstSync,          // 首次方向判定（远端有库→先拉后推；无→首次上传）
    ValidateFirstSync,  // 启用开关流程：先测试连接三步，过了才做方向判定
    Test,               // 只跑测试连接
};

struct GithubSyncTestResult {
    bool ran = false;      // 动作真的跑完（false = 超时 / 模块未启动 / 门禁未开）
    bool ok = false;       // 引擎判定的成败
    bool conflict = false; // 走了 409 冲突路径
    bool dbImported = false;  // 本轮导入了远端 database.sql
    std::string summary;      // 卡片摘要（成功文案）
    std::string error;        // 卡片红字（成功为空）
    std::vector<std::string> overwriteFiles;  // 409 覆盖清单（无冲突为空）
    std::string branchHint;   // 分支被自动创建的提示（未创建为空）
};

// 测试门是否打开（DEVDESK_GITHUB_TEST_MODE=1）。驱动必须先查它。
bool githubSyncTestSeamAvailable();

// 驱动一次同步动作并阻塞至完成；timeoutMs 到点即返回 ran=false。
GithubSyncTestResult githubSyncRunForTest(GithubSyncTestAction action,
                                          bool manual, int timeoutMs);

// 触发一次本地 git 探测并等它收束，返回快照（-1 未探 / 0 不可用 / 1 可用）。
// 探测在 UI 每帧被调起，无头驱动没有 compose 帧，必须显式触发。
int githubSyncLocalGitProbeForTest();

}  // namespace module::settings
