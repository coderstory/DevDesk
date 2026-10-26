// src/module_settings/settings_ui_cards.h
// settings 模块 7 张卡的绘制单元（+ GitHub Sync 卡在 github_sync_card.h，共 8 卡），
// 全部是自由函数：吃只读入参（SettingsCardsIn）与回调（SettingsCardActions），
// 不捕获 this、不写模块成员。
// 回调约定：卡片在元素回调里按值捕获用到的 std::function（`[f = a.xxx]`），不捕获
// 本文件内的临时对象——元素树比一帧活得久，捕引用会悬垂；写回模块状态一律经
// SettingsCardActions。
#pragma once

#include "settings_config.h"
#include "settings_ui_logic.h"

#include <core/claude_proxy_service.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>
#include <vector>

namespace module::settings {

// 卡片的只读入参（compose 每帧装配；引用指向模块的存活成员，只在绘制期读）
struct SettingsCardsIn {
    const SettingsConfig& cfg;                 // 配置（模块成员 cfg_）
    const std::string& proxyPortDraft;         // 端口草稿（空 = 未编辑）
    const std::vector<std::string>& logTail;   // 日志 tail 快照
    unsigned logTailVersion = 0;               // tail 版本号（进查看卡 contentKey）
    float logViewScrollOffset = 0.0f;          // 日志查看卡内滚动位置
    bool takeoverManaged = false;              // 接管状态快照
    int openDropdown = -1;                     // 下拉唯一持有者
    ::core::IClaudeProxyService* proxy = nullptr;  // 代理运行面（null = 未装配）

    // 开机自启动（UAT6）：显示的是平台注册真值（HKCU Run / LaunchAgents 有项 = 开，
    // 页面打开时回读防漂移），不是 cfg 意图；POSIX 兜底 false → 行整行不建（卡高不变）
    bool autostartSupported = true;
    bool autostartRegistered = false;

    // GitHub Sync 快照
    bool githubSyncEnabled = false;
    std::string githubSyncRepoUrl;
    std::string githubSyncBranch;
    std::string githubSyncTokenDraft;   // Token 输入草稿（不存明文）
    bool githubSyncAutoPush = true;
    std::string githubSyncStatus;
    bool githubSyncStatusError = false;
    bool githubSyncRunning = false;      // 同步/拉取/测试进行中 → 三按钮禁用
    std::string githubSyncProgress;      // 阶段文案（如「① 校验 token…」）
    std::string githubSyncError;         // 非空 = 错误行（红字摘要）+「详情」小按钮
    bool githubSyncRepoPublic = false;   // 仓库是 public → 黄字提醒（不硬拦）
    std::string githubSyncBranchHint;    // 分支被自动创建时的提示
    bool githubSyncLocalGitUnavailable = false;  // 本地 git 不可用 → 灰字
    bool githubSyncUnsyncedNotice = false;       // 退出推送未完成 → 黄字
    std::string githubSyncDeleteSkipHint;        // 删远端跳过提示 → 黄字
    bool githubSyncTokenVisible = false;          // S3.7#6 Token 显隐切换
    bool githubSyncDetailExpanded = false;        // S3.7#4 同步说明折叠展开
};

// 卡片回写模块的回调（compose 每帧装配；lambda 只捕获 this → SBO 无堆分配）
struct SettingsCardActions {
    std::function<void(int)> applyThemeMode;                 // 主题模式三档
    std::function<void(int)> applyFontScalePercent;          // 字号缩放（%，无级 80-130）
    std::function<void(int)> applyWindowEffect;              // 窗口效果三档
    std::function<void(const std::string&, bool)> applyModuleEnabled;  // 模块插拔
    std::function<void(Dropdown, bool)> setDropdownOpen;      // 下拉唯一持有者
    std::function<void(bool)> applyProxyAutoStart;            // 随应用启动代理
    std::function<void()> applyProxyPort;                     // 应用代理端口
    std::function<void(const std::string&)> setProxyPortDraft;  // 端口输入（含清错误态）
    std::function<void()> discardProxyPortDraft;              // 端口草稿 Esc 丢弃
    std::function<void(int)> applyRetentionPreset;            // 日志保留档
    std::function<void(int)> applyLogLevelPreset;             // 日志级别档
    std::function<void(int)> applyEditorFontPreset;           // 编辑器字号档
    std::function<void(bool)> applyAutostartEnabled;          // 开机自启动开关（UAT6）
    std::function<void(bool)> applyFluidBackgroundEnabled;   // 动态背景开关（UAT6 活体流质）
    std::function<void()> requestLogTailReload;               // 日志查看卡刷新
    std::function<void(float)> logViewScrolled;               // 日志查看卡内滚动

    // GitHub Sync 回调
    std::function<void(bool)> applyGithubSyncEnabled;           // 启用开关
    std::function<void(const std::string&)> applyGithubSyncRepoUrl;  // 仓库 URL
    std::function<void(const std::string&)> applyGithubSyncBranch;   // 分支
    std::function<void(const std::string&)> setGithubSyncTokenDraft;  // Token 输入
    std::function<void(bool)> applyGithubSyncAutoPush;          // 自动推送
    std::function<void()> githubSyncPush;                       // 立即同步
    std::function<void()> githubSyncPull;                       // 拉取
    std::function<void()> githubSyncTestConnection;             // 测试连接
    std::function<void()> githubSyncShowErrorDetail;            // 错误行「详情」
    std::function<void()> toggleGithubSyncTokenVisibility;   // S3.7#6 Token 显隐切换
    std::function<void()> toggleGithubSyncDetail;            // S3.7#4 同步说明折叠展开
};

// 单卡绘制入参（注册表按序调用；签名统一，未用到的字段忽略）
struct CardDrawArgs {
    eui::Ui& ui;
    std::string id;               // 卡根 id（注册表给的字面量）
    float w = 0.0f;               // 卡宽（= scrollView 内容宽）
    float h = 0.0f;               // 卡高（注册表按卡高种类解析）
    const core::theme::Tokens& t;
    float pageY = 0.0f;           // 卡顶页面绝对 y（仅 openUp 几何卡读）
    float screenH = 0.0f;         // 页面逻辑高（同上）
    const SettingsCardsIn& in;
    const SettingsCardActions& a;
};

// 7 张卡（签名统一，注册表按 tab 分派；绘制顺序 = 注册表顺序）
void drawAppearanceCard(const CardDrawArgs& args);   // 通用：外观（主题/字号/窗口效果）
void drawEditorFontCard(const CardDrawArgs& args);   // 通用：编辑器字号
void drawModulesCard(const CardDrawArgs& args);      // 模块：插拔开关
void drawWallpaperCard(const CardDrawArgs& args);    // 模块：壁纸开关（平台能力门控在注册表）
void drawProxyCard(const CardDrawArgs& args);        // 通用：Claude 代理（开关门控；UAT5 自模块 tab 迁入）
void drawLogCard(const CardDrawArgs& args);          // 日志：保留天数 + 级别
void drawLogViewCard(const CardDrawArgs& args);      // 日志：tail 查看

} // namespace module::settings
