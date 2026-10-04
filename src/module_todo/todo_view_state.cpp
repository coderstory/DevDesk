// src/module_todo/todo_view_state.cpp
// todo 视图状态的 app_config 表存取（接口见 todo_view_state.h）。不读旧 JSON
// 文件、不迁移。
// 整份一行而非按顶层键拆分：本状态只有一个写者，且每次都是「load 整个结构 →
// 改 → 整个存回」，拆行的「多写者互不覆盖」收益不存在，成本却是多次 UPSERT。
#include "module_todo/todo_view_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "core/app_config.h"
#include "core/logging.h"
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

TodoViewStateStore::TodoViewStateStore(core::data::SqliteDb* db) : db_(db) {}

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

core::Result<TodoViewState> TodoViewStateStore::loadChecked() const {
    TodoViewState st;  // 全默认值兜底
    if (db_ == nullptr) {
        return core::Result<TodoViewState>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空）");
    }
    auto raw = core::appconfig::get(*db_, kTodoViewStateKey);
    if (raw.isFail()) {
        // 键不存在 = 首次启动（正常路径，静默用默认）；表不存在 / SQL 失败 =
        // 真故障，报出来而不是与「没配过」混同
        if (raw.errorCode() == core::ErrorCode::NotFound) {
            return core::Result<TodoViewState>::ok(st);
        }
        return core::Result<TodoViewState>::fail(
            raw.errorCode(),
            "读取 app_config[" + std::string(kTodoViewStateKey) + "] 失败: " +
                raw.errorMessage());
    }
    auto parsed = platform::json::parse(raw.value());
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<TodoViewState>::fail(
            core::ErrorCode::JsonParseError,
            "app_config[" + std::string(kTodoViewStateKey) +
                "] 不是合法 JSON 对象: " +
                (parsed.isFail() ? parsed.errorMessage() : std::string("顶层非对象")));
    }
    platform::json::JsonObject obj = parsed.value().asObject();

    if (obj.has(kKeyScrolls) && obj.get(kKeyScrolls).isObject()) {
        platform::json::JsonObject scrolls = obj.get(kKeyScrolls).asObject();
        for (const auto& key : scrolls.keys()) {
            int64_t id = 0;
            if (!parseColumnId(key, id)) continue;
            platform::json::JsonValue v = scrolls.get(key);
            if (v.isNumber()) {
                // 数字双分支读取：两种落盘形态都要认——整值可能写成 "512"
                // 走 Int 分支，也可能写成 "512.0" 走 Double 分支。只有
                // asDouble() 会丢值（走 Int 分支时返回 0），故回退 asInt()。
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
    return core::Result<TodoViewState>::ok(sanitized(st));
}

TodoViewState TodoViewStateStore::load() const {
    auto loaded = loadChecked();
    if (loaded.isFail()) {
        LOG_ERROR("Todo: 读取视图状态失败，回退默认值（本次改动将无法保存）: " +
                  loaded.errorMessage());
        return sanitized(TodoViewState{});
    }
    return loaded.value();
}

core::Result<void> TodoViewStateStore::save(const TodoViewState& stRaw) const {
    const TodoViewState st = sanitized(stRaw);
    if (db_ == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空），视图状态未保存");
    }

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
    // 一次 UPSERT 即一次提交——与旧的「原子写文件」等价（且顺带拿到事务的
    // 原子性：要么这行是新值，要么还是旧值，不会出现半截）
    return core::appconfig::set(*db_, kTodoViewStateKey, text.value());
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
