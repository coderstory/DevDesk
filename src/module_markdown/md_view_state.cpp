// src/module_markdown/md_view_state.cpp
// markdown_view_state.json 读写（容错模式照抄 pet_config：缺失/损坏回退默认）
#include "module_markdown/md_view_state.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
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

std::filesystem::path markdownViewStateFile() {
    // 环境变量覆盖（无头测试隔离真实 %APPDATA% 状态文件；与 markdownDir 同款机制）
    if (const char* env = std::getenv("DEVDESK_MARKDOWN_VIEW_STATE_PATH")) {
        if (*env != '\0') return std::filesystem::path(env);
    }
    return core::paths::appDataDir() / "markdown_view_state.json";
}

MarkdownViewStateStore::MarkdownViewStateStore(std::filesystem::path file)
    : file_(std::move(file)) {}

MarkdownViewState MarkdownViewStateStore::sanitized(const MarkdownViewState& st) {
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

MarkdownViewState MarkdownViewStateStore::load() const {
    MarkdownViewState st;  // 全默认值兜底

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return st;  // 首次启动：无视图状态文件，正常路径
    }

    std::ifstream f(file_);
    if (!f) {
        LOG_WARN("Markdown: 无法读取视图状态文件，使用默认值: " + file_.string());
        return st;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();

    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.value().isObject()) {
        LOG_WARN("Markdown: 视图状态文件解析失败，使用默认值: " + file_.string());
        return st;
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
    return sanitized(st);
}

core::Result<void> MarkdownViewStateStore::save(const MarkdownViewState& stRaw) const {
    const MarkdownViewState st = sanitized(stRaw);

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

    // 父目录确保存在（appDataDir 正常已由 ensureDirectoriesExist 建好，兜底）
    std::error_code ec;
    const auto parent = file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec) && !ec) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "视图状态目录创建失败: " + ec.message());
        }
    }

    // 原子写（tmp → remove → rename，core/atomic_write.h 唯一实现，审计 THS-03
    // 第二批收口）：旧的 std::ofstream(file_, trunc) 原地写在写窗口内崩溃/断电
    // 会把文件截成半截，load 走解析失败分支静默回落默认值（上次笔记路径/视图
    // 模式/目录折叠全丢）。逐字节尾处理照旧实现：serialize 结果 + "\n"
    return core::atomicWriteFile(file_, text.value() + "\n", "视图状态");
}

} // namespace module::markdown
