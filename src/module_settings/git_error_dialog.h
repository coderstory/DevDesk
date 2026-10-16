// src/module_settings/git_error_dialog.h
// GitHub Sync 错误详情弹窗：中文摘要 + 可滚动请求日志列表 + 重试/复制详情/关闭。
//
// 解耦边界：日志经 std::function 注入（弹窗只认 platform::github::ApiLogEntry
// 这个**值类型**，不 include 任何 platform 实现、不自己发网络请求）。复制动作同样
// 经回调注入——弹窗产出文本，写剪贴板由调用方（settings_module）落 EUI-NEO
// clipboard，故本文件在无头测试里可完全脱离系统剪贴板。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <platform/github_api.h>

#include <functional>
#include <string>
#include <vector>

namespace module::settings {

// 弹窗变体：普通失败 / 409 冲突（决策 #7：动作 = 拉取远端后再推，**二次确认**）。
enum class GitErrorDialogVariant {
    Error,        // 标题「同步失败详情」，主按钮「重试」
    Conflict409,  // 标题「同步冲突（409）」，主按钮「拉取远端后再推」+ 二次确认
};

// 弹窗状态（模块持有；drawGitErrorDialog 只读入参 + 经回调写回）
struct GitErrorDialogState {
    bool open = false;
    GitErrorDialogVariant variant = GitErrorDialogVariant::Error;
    bool conflictConfirmArmed = false;  // 409 二次确认已按第一下（待确认）
    int selectedLogIndex = 0;      // 选中条目下标（越界 = 无选中）
    float logScrollOffset = 0.0f;  // 日志列表滚动位置
};

// 弹窗只读入参
struct GitErrorDialogIn {
    std::string summary;  // 中文错误摘要（上半部）
    // 日志源：每次绘制时调用取当前快照（模块侧持日志，弹窗不缓存）。
    std::function<std::vector<platform::github::ApiLogEntry>()> logs;
    // F6：409 变体下「拉取覆盖将覆盖掉的本地未推文件」清单（远端路径）。
    // 非空且变体为 Conflict409 → 日志列表上方渲染清单；空 = 不画。
    std::vector<std::string> overwriteFiles;
};

// 弹窗回调（compose 每帧装配；只捕获 this 的 lambda）
struct GitErrorDialogActions {
    std::function<void()> retry;       // Error 变体主按钮：重试
    std::function<void()> pullThenPush;  // Conflict409 变体主按钮（二次确认后触发）
    std::function<void()> close;                          // 关闭
    std::function<void(const std::string&)> copyDetail;   // 复制详情（文本由弹窗拼好）
    std::function<void(int)> selectLog;                   // 选中日志条目
    std::function<void(float)> logScrolled;               // 日志列表滚动
};

// 日志行文案：`方法 URL 状态码 耗时`（无状态码 = 网络错误）。
std::string formatGitLogRowLine(const platform::github::ApiLogEntry& e);

// 单条日志详情：优先 bodyTail，其次 errorMessage；都空 → 空串。
std::string formatGitLogDetail(const platform::github::ApiLogEntry& e);

// 「复制详情」的完整文本：摘要 + 每条日志行 + 每条的非空详情（bodyTail 优先，
// 其次 errorMessage）。全量进剪贴板——用户复制一次就带齐所有条目的诊断信息，
// 不依赖当前选中行。
std::string formatGitErrorDetail(
    const std::string& summary,
    const std::vector<platform::github::ApiLogEntry>& logs);

// 绘制错误详情弹窗（模态；open=false 时整棵树仍建，只把 panel 透明度置 0）。
void drawGitErrorDialog(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t, GitErrorDialogState& st,
                        const GitErrorDialogIn& in,
                        const GitErrorDialogActions& a);

} // namespace module::settings
