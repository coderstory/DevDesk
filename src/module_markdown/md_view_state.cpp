// src/module_markdown/md_view_state.cpp
// markdown 视图状态的 app_config 表存取（接口见 md_view_state.h）。
// 整份状态存为 app_config 一行（键 markdown_view_state），原 JSON 文件已退役、不迁移；
// 单写者（markdown 模块页）且整存整取，故不按顶层键拆行。
#include "module_markdown/md_view_state.h"

#include <utility>

#include "core/app_config.h"
#include "core/logging.h"
#include "platform/json.h"

namespace module::markdown {

namespace {
constexpr const char* kKeyLastPath = "lastPath";
constexpr const char* kKeyMode = "mode";
constexpr const char* kKeyCollapsedDirs = "collapsedDirs";
constexpr const char* kKeySelectedDir = "selectedDir";
constexpr int kMaxMode = 3;  // ViewMode::Live（0=Edit 1=Split 2=Preview 3=Live）

// 路径穿越守卫：笔记路径一律 POSIX 相对路径，含 ".." 段即拒
bool hasTraversal(const std::string& p) {
    return p.find("..") != std::string::npos;
}
} // namespace

MarkdownViewStateStore::MarkdownViewStateStore(core::data::SqliteDb* db)
    : db_(db) {}

MarkdownViewState MarkdownViewStateStore::sanitized(
    const MarkdownViewState& st) {
    MarkdownViewState out;
    if (!st.lastPath.empty() && !hasTraversal(st.lastPath)) {
        out.lastPath = st.lastPath;
    }
    out.mode = (st.mode >= 0 && st.mode <= kMaxMode) ? st.mode : 0;
    for (const auto& d : st.collapsedDirs) {
        if (!d.empty() && !hasTraversal(d)) out.collapsedDirs.push_back(d);
    }
    // "" = 根目录行（合法值）；非空则与其它路径同规则（拒 ".." 穿越）
    if (st.selectedDir.empty() || !hasTraversal(st.selectedDir)) {
        out.selectedDir = st.selectedDir;
    }
    return out;
}

core::Result<MarkdownViewState> MarkdownViewStateStore::loadChecked() const {
    MarkdownViewState st;  // 全默认值兜底
    if (db_ == nullptr) {
        return core::Result<MarkdownViewState>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空）");
    }
    auto raw = core::appconfig::get(*db_, kMarkdownViewStateKey);
    if (raw.isFail()) {
        // 键不存在 = 首次启动（正常路径，静默用默认）；表不存在 / SQL 失败 = 真故障，报出来。
        if (raw.errorCode() == core::ErrorCode::NotFound) {
            return core::Result<MarkdownViewState>::ok(st);
        }
        return core::Result<MarkdownViewState>::fail(
            raw.errorCode(),
            "读取 app_config[" + std::string(kMarkdownViewStateKey) +
                "] 失败: " + raw.errorMessage());
    }
    auto parsed = platform::json::parse(raw.value());
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<MarkdownViewState>::fail(
            core::ErrorCode::JsonParseError,
            "app_config[" + std::string(kMarkdownViewStateKey) +
                "] 不是合法 JSON 对象: " +
                (parsed.isFail() ? parsed.errorMessage()
                                 : std::string("顶层非对象")));
    }
    platform::json::JsonObject obj = parsed.value().asObject();

    if (obj.has(kKeyLastPath) && obj.get(kKeyLastPath).isString()) {
        st.lastPath = obj.get(kKeyLastPath).asString();
    }
    if (obj.has(kKeyMode) && obj.get(kKeyMode).isNumber()) {
        st.mode = static_cast<int>(obj.get(kKeyMode).asInt());
    }
    if (obj.has(kKeyCollapsedDirs) && obj.get(kKeyCollapsedDirs).isArray()) {
        platform::json::JsonValue arr = obj.get(kKeyCollapsedDirs);
        for (int i = 0; i < arr.size(); ++i) {
            platform::json::JsonValue item = arr[i];
            if (item.isString()) st.collapsedDirs.push_back(item.asString());
        }
    }
    if (obj.has(kKeySelectedDir) && obj.get(kKeySelectedDir).isString()) {
        st.selectedDir = obj.get(kKeySelectedDir).asString();
    }
    return core::Result<MarkdownViewState>::ok(sanitized(st));
}

MarkdownViewState MarkdownViewStateStore::load() const {
    auto loaded = loadChecked();
    if (loaded.isFail()) {
        LOG_ERROR("Markdown: 读取视图状态失败，回退默认值（本次改动将无法保存）: " +
                  loaded.errorMessage());
        return sanitized(MarkdownViewState{});
    }
    return loaded.value();
}

core::Result<void> MarkdownViewStateStore::save(
    const MarkdownViewState& stRaw) const {
    const MarkdownViewState st = sanitized(stRaw);
    if (db_ == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空），视图状态未保存");
    }

    platform::json::JsonObject obj;
    obj.set(kKeyLastPath, platform::json::makeString(st.lastPath));
    obj.set(kKeyMode, platform::json::makeInt(st.mode));
    obj.set(kKeyCollapsedDirs, platform::json::makeStringArray(st.collapsedDirs));
    obj.set(kKeySelectedDir, platform::json::makeString(st.selectedDir));

    auto text = platform::json::stringify(
        platform::json::JsonValue(obj), /*pretty=*/true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "视图状态序列化失败: " + text.errorMessage());
    }
    // 一次 UPSERT 即一次提交——与旧的「原子写文件」等价，且自带事务原子性（不会半截）。
    return core::appconfig::set(*db_, kMarkdownViewStateKey, text.value());
}

} // namespace module::markdown
