// markdown 视图状态跨会话持久化 —— SQLite app_config 表（键 markdown_view_state）。
// 原 markdown_view_state.json 已退役（不读、不写、不迁移）。
// 模块隔离：落在中立层 app_config 表，不进 module_settings，谁都不依赖谁。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::markdown {

// 默认 app_config 键名
inline constexpr const char* kMarkdownViewStateKey = "markdown_view_state";

// 用户重启后恢复的四项：选中笔记 / 四态视图 / 收起的目录 / 目录树选中目录。
struct MarkdownViewState {
    std::string lastPath;                    // 打开的笔记（POSIX 相对路径，空 = 无）
    // ViewMode：0=Edit 1=Split 2=Preview 3=Live（越界 → 0；Live 为追加，免迁移）。
    int mode = 0;
    std::vector<std::string> collapsedDirs;  // 已收起的目录（POSIX 相对路径，各层同表）
    // 目录树的当前选中目录（POSIX 相对路径；"" = 根目录行，合法值）。新建条目父
    // 目录由它派生，故须跨会话记住，否则重启后新条目又散回根目录。
    std::string selectedDir;
};

// 视图状态读写（不抛异常；键缺失 / 值损坏回退默认值）。db 允许为 nullptr，
// 但那不是「没配置」而是「库未就绪」：save() 返回 fail，load() 回退并打 LOG_ERROR。
class MarkdownViewStateStore {
public:
    explicit MarkdownViewStateStore(core::data::SqliteDb* db);

    MarkdownViewState load() const;

    // 与 load() 同源，把「库未就绪 / 表不存在 / 值损坏」如实报出来
    core::Result<MarkdownViewState> loadChecked() const;

    core::Result<void> save(const MarkdownViewState& st) const;

    // 净化：mode 钳进合法枚举域；含 ".." 的路径丢弃（防穿越）
    static MarkdownViewState sanitized(const MarkdownViewState& st);

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::markdown
