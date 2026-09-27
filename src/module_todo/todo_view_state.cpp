// src/module_todo/todo_view_state.cpp
// todo_view_state.json 读写（容错模式照抄 pet_config：缺失/损坏回退默认）
#include "module_todo/todo_view_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"

namespace module::todo {

namespace {
constexpr const char* kKeyScrolls = "columnScrolls";
constexpr const char* kKeySelectedTask = "selectedTaskId";

// json 对象键是字符串；手写十进制解析（项目铁律不碰异常，不用 std::stoll）。
// 列 id 是数据库自增正整数，只接受纯数字键；越界/乱键返回 false
bool parseColumnId(const std::string& key, int64_t& out) {
    if (key.empty() || key.size() > 18) return false;
    int64_t v = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v <= 0) return false;
    out = v;
    return true;
}
} // namespace

std::filesystem::path todoViewStateFile() {
    // 环境变量覆盖（无头测试隔离真实 %APPDATA% 状态文件；与 markdownDir 同款机制）
    if (const char* env = std::getenv("DEVDESK_TODO_VIEW_STATE_PATH")) {
        if (*env != '\0') return std::filesystem::path(env);
    }
    return core::paths::appDataDir() / "todo_view_state.json";
}

TodoViewStateStore::TodoViewStateStore(std::filesystem::path file)
    : file_(std::move(file)) {}

TodoViewState TodoViewStateStore::sanitized(const TodoViewState& st) {
    TodoViewState out;
    for (const auto& [id, offset] : st.columnScrolls) {
        if (id <= 0) continue;  // 列 id 数据库自增，非正即乱值
        out.columnScrolls[id] =
            (std::isfinite(offset) && offset > 0.0f) ? offset : 0.0f;
    }
    out.selectedTaskId = st.selectedTaskId > 0 ? st.selectedTaskId : 0;
    return out;
}

TodoViewState TodoViewStateStore::load() const {
    TodoViewState st;  // 全默认值兜底

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return st;  // 首次启动：无视图状态文件，正常路径
    }

    std::ifstream f(file_);
    if (!f) {
        LOG_WARN("Todo: 无法读取视图状态文件，使用默认值: " + file_.string());
        return st;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();

    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.unsafeValue().isObject()) {
        LOG_WARN("Todo: 视图状态文件解析失败，使用默认值: " + file_.string());
        return st;
    }
    platform::json::JsonObject obj = parsed.unsafeValue().asObject();

    if (obj.has(kKeyScrolls) && obj.get(kKeyScrolls).isObject()) {
        platform::json::JsonObject scrolls = obj.get(kKeyScrolls).asObject();
        for (const auto& key : scrolls.keys()) {
            int64_t id = 0;
            if (!parseColumnId(key, id)) continue;
            platform::json::JsonValue v = scrolls.get(key);
            if (v.isNumber()) {
                // 数字双分支读取：本平台 stringify 用 %.17g，整值浮点（如
                // 512.0）输出 "512" 回读走 Int 分支，只有 asDouble() 会丢值
                const double d = v.asDouble() != 0.0
                                     ? v.asDouble()
                                     : static_cast<double>(v.asInt());
                st.columnScrolls[id] = static_cast<float>(d);
            }
        }
    }
    if (obj.has(kKeySelectedTask) && obj.get(kKeySelectedTask).isNumber()) {
        st.selectedTaskId = obj.get(kKeySelectedTask).asInt();
    }
    return sanitized(st);
}

core::Result<void> TodoViewStateStore::save(const TodoViewState& stRaw) const {
    const TodoViewState st = sanitized(stRaw);

    platform::json::JsonObject scrolls;
    for (const auto& [id, offset] : st.columnScrolls) {
        scrolls.set(std::to_string(id), platform::json::makeDouble(
                                            static_cast<double>(offset)));
    }
    platform::json::JsonObject obj;
    obj.set(kKeyScrolls, platform::json::JsonValue(scrolls));
    obj.set(kKeySelectedTask, platform::json::makeInt(st.selectedTaskId));

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

    // 原子写（tmp → remove → rename，core/atomic_write.h 唯一实现，审计 THS-03）：
    // 旧的 std::ofstream(file_, trunc) 原地写在写窗口内崩溃/断电会把文件截成
    // 半截，load 走解析失败分支静默回落默认值（列滚动位置/选中任务丢失；
    // 本函数由卡片点击/右键高频触发，暴露窗口最多）
    return core::atomicWriteFile(file_, text.unsafeValue() + "\n", "视图状态");
}

size_t TodoViewStateStore::pruneScrolls(std::map<int64_t, float>& scrolls,
                                        const std::vector<int64_t>& liveColumnIds) {
    size_t removed = 0;
    for (auto it = scrolls.begin(); it != scrolls.end();) {
        if (std::find(liveColumnIds.begin(), liveColumnIds.end(), it->first) ==
            liveColumnIds.end()) {
            it = scrolls.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

} // namespace module::todo
