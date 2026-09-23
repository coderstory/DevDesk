// src/module_markdown/md_view_state.h
// 审计 [23]：markdown 视图状态跨会话持久化 —— <appDataDir>/markdown_view_state.json。
// 模块隔离：module_* 互不依赖，视图状态不进 module_settings 的 SettingsConfig
//（方案 B），照抄 pet_config 模式：路径构造注入、缺失/损坏回退默认、save 走 Result。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::markdown {

// 用户重启后最想恢复的三项（审计 [23] evidence：选中笔记 / 三态视图 / 收起的目录）
struct MarkdownViewState {
    std::string lastPath;                    // 打开的笔记（POSIX 相对路径，空 = 无）
    int mode = 0;                            // ViewMode：0=Edit 1=Split 2=Preview（越界 → 0）
    std::vector<std::string> collapsedDirs;  // 已收起的一级目录（POSIX 相对路径）
};

// 默认文件位置（core::paths::appDataDir() 下；测试经构造函数注入临时文件）
std::filesystem::path markdownViewStateFile();

// markdown_view_state.json 读写（不抛异常；文件不存在/损坏一律回退默认值）
class MarkdownViewStateStore {
public:
    explicit MarkdownViewStateStore(std::filesystem::path file);

    MarkdownViewState load() const;
    core::Result<void> save(const MarkdownViewState& st) const;

    // 净化：mode 钳进合法枚举域；含 ".." 的路径丢弃（防手改穿越笔记根目录）
    static MarkdownViewState sanitized(const MarkdownViewState& st);

private:
    std::filesystem::path file_;
};

} // namespace module::markdown
