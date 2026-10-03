// src/module_todo/todo_tools.cpp
// TodoTools 6 工具实现（agent M1 Task 8）。每个工具一个小类，同文件存放：
//   todo.list     列出任务（可选 completed 过滤）      只读
//   todo.create   新建任务（缺省建到第一列）
//   todo.update   更新内容字段（title/description/priority/dueAt）
//   todo.move     移动任务到指定列（缺省追加列尾）
//   todo.complete 标记完成
//   todo.delete   删除任务（不可恢复 → isDestructive=true）
//
// 出参形状约定：结构化数据组对象再 stringify（{"tasks":[...]} / 任务对象）；
// 数组用「逐元素 stringify + 文本拼接 + parse 校验」组装（platform::json 没有
// 数组追加 API，makeArray 是空壳桩——agent_loop.cpp joinBlockArray 同款先例）。
//
// updateTask 是整字段 UPDATE（title/description/priority/due_at/tags/
// completed_at 全列覆写，见 todo_service.cpp）→ todo.update 必须先取旧任务
// 叠加新值再回写，否则未提供的字段会被清空。同理 createTask/moveTask/
// completeTask/deleteTask 对不存在的 id 是静默 no-op 或悬空写入 → 工具先查
// 存在性，把「目标不存在」作为 isError 回喂，不给 LLM 假成功。
#include "todo_tools.h"

#include "core/logging.h"
#include "platform/json.h"

#include <optional>
#include <string>
#include <utility>

namespace module::todo {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// ---- 结果组装辅助（各字段仅本文件工具共用）----

// 工具级错误：Result ok + isError + {"error":"<中文>"}（错误回喂 LLM）
Result<ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
        // 自有字面量 + 一个字符串字段的序列化失败 = 编程不变量破坏
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "工具错误消息序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, text.value()});
}

Result<ToolResult> toolOk(const std::string& contentJson) {
    return Result<ToolResult>::ok(ToolResult{false, contentJson});
}

// 参数解析：空串视为 {}（LLM 的 input 可能为空对象省略）；其余必须解析成
// object。解析失败 = 参数非法（工具级错误），由调用方转 isError。
Result<pj::JsonObject> parseArgs(const ToolArgs& args) {
    if (args.json.empty()) {
        return Result<pj::JsonObject>::ok(pj::JsonObject());
    }
    auto v = pj::parse(args.json);
    if (v.isFail() || !v.value().isObject()) {
        return Result<pj::JsonObject>::fail(core::ErrorCode::ValidationError,
                                            "参数不是合法 JSON 对象");
    }
    return Result<pj::JsonObject>::ok(v.value().asObject());
}

// 字符串字段：缺失 / 非字符串 → 空串（必填校验由调用方做）
std::string strField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

// 整数字段：缺失 / 非数值 → nullopt
std::optional<int64_t> intField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    if (!v.isNumber()) return std::nullopt;
    return std::optional<int64_t>(v.asInt());
}

// Task → JSON 对象文本（list / create / update 的统一出参元素形状）
Result<std::string> taskToJsonText(const Task& t) {
    pj::JsonObject o;
    o.set("id", pj::makeInt(t.id));
    o.set("columnId", pj::makeInt(t.columnId));
    o.set("title", pj::makeString(t.title));
    o.set("description", t.description.has_value()
                             ? pj::JsonValue(t.description.value())
                             : pj::JsonValue(nullptr));
    o.set("priority", pj::makeInt(static_cast<int64_t>(t.priority)));
    o.set("dueAt", t.dueAt.has_value() ? pj::JsonValue(t.dueAt.value())
                                       : pj::JsonValue(nullptr));
    o.set("tags", pj::makeStringArray(t.tags));
    o.set("position", pj::makeInt(t.position));
    o.set("completedAt", t.completedAt.has_value()
                             ? pj::JsonValue(t.completedAt.value())
                             : pj::JsonValue(nullptr));
    auto text = pj::stringify(pj::JsonValue(std::move(o)));
    if (text.isFail()) {
        return Result<std::string>::fail(core::ErrorCode::Internal,
                                         "任务序列化失败: " + text.errorMessage());
    }
    return Result<std::string>::ok(text.value());
}

// 按 id 线性找任务（listAllTasks 全量后过滤）；不存在 → NotFound。
// 服务层的按 id 写操作对幽灵 id 多为静默 no-op，存在性前置检查让错误可见。
Result<Task> findTask(TodoService& svc, int64_t id) {
    auto all = svc.listAllTasks();
    if (all.isFail()) {
        return Result<Task>::fail(all.errorCode(), all.errorMessage());
    }
    for (const auto& t : all.value()) {
        if (t.id == id) return Result<Task>::ok(t);
    }
    return Result<Task>::fail(core::ErrorCode::NotFound,
                              "任务不存在: id=" + std::to_string(id));
}

// 列存在性检查（防 create/move 把任务挂到幽灵列上）
bool columnExists(TodoService& svc, int64_t columnId, std::string* err) {
    auto cols = svc.listColumns();
    if (cols.isFail()) {
        if (err != nullptr) *err = "读取列失败: " + cols.errorMessage();
        return false;
    }
    for (const auto& c : cols.value()) {
        if (c.id == columnId) return true;
    }
    if (err != nullptr) *err = "列不存在: columnId=" + std::to_string(columnId);
    return false;
}

// ============ todo.list ============

class TodoListTool : public core::ITool {
public:
    explicit TodoListTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.list"; }

    std::string schemaJson() const override {
        return R"({"name":"todo.list","description":"列出全部待办任务（含 id/标题/列/优先级/完成态）","input_schema":{"type":"object","properties":{"completed":{"type":"boolean","description":"true=只看已完成，false=只看未完成，缺省=全部"}}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        // completed 过滤（可选）：nullopt = 全部
        std::optional<bool> completed;
        if (argsObj.value().has("completed")) {
            pj::JsonValue c = argsObj.value().get("completed");
            if (!c.isBool()) return toolError("参数 completed 必须是布尔值");
            completed = c.asBool();
        }

        auto tasks = svc_.listAllTasks();
        if (tasks.isFail()) {
            return toolError("读取任务失败: " + tasks.errorMessage());
        }

        // 数组组装：逐任务 stringify 后文本拼接（platform::json 无数组追加 API）
        std::string joined;
        for (const auto& t : tasks.value()) {
            if (completed.has_value() &&
                t.completedAt.has_value() != completed.value()) {
                continue;
            }
            auto one = taskToJsonText(t);
            if (one.isFail()) return toolError(one.errorMessage());
            if (!joined.empty()) joined += ",";
            joined += one.value();
        }
        const std::string content = "{\"tasks\":[" + joined + "]}";
        // 自有拼接必须可解析（joinBlockArray 同款校验）；失败 = 不变量破坏
        if (pj::parse(content).isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal, "工具结果 JSON 拼接失败（编程错误）");
        }
        return toolOk(content);
    }

private:
    TodoService& svc_;
};

// ============ todo.create ============

class TodoCreateTool : public core::ITool {
public:
    explicit TodoCreateTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.create"; }

    std::string schemaJson() const override {
        return R"({"name":"todo.create","description":"新建待办任务（缺省建到第一列）","input_schema":{"type":"object","properties":{"title":{"type":"string","description":"任务标题（必填）"},"columnId":{"type":"integer","description":"目标列 id，缺省=第一列"}},"required":["title"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const std::string title = strField(argsObj.value(), "title");
        if (title.empty()) {
            return toolError("任务标题不能为空（必填字段 title）");
        }

        // 目标列：显式 columnId > 第一列
        int64_t columnId = 0;
        if (auto explicitCol = intField(argsObj.value(), "columnId")) {
            std::string err;
            if (!columnExists(svc_, *explicitCol, &err)) {
                return toolError(err);
            }
            columnId = *explicitCol;
        } else {
            auto cols = svc_.listColumns();
            if (cols.isFail()) {
                return toolError("读取列失败: " + cols.errorMessage());
            }
            if (cols.value().empty()) {
                return toolError("没有任何列，无法新建任务（请先创建列）");
            }
            columnId = cols.value().front().id;
        }

        auto created = svc_.createTask(columnId, title);
        if (created.isFail()) {
            return toolError("新建任务失败: " + created.errorMessage());
        }
        auto one = taskToJsonText(created.value());
        if (one.isFail()) return toolError(one.errorMessage());
        return toolOk(one.value());  // 返回新任务对象（LLM 需要新 id）
    }

private:
    TodoService& svc_;
};

// ============ todo.update ============

class TodoUpdateTool : public core::ITool {
public:
    explicit TodoUpdateTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.update"; }

    std::string schemaJson() const override {
        return R"({"name":"todo.update","description":"更新任务内容字段（标题/描述/优先级/截止时间），只传需要改的字段；改列改序请用 todo.move","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"任务 id（必填）"},"title":{"type":"string"},"description":{"type":"string","description":"传 null 清空描述"},"priority":{"type":"integer","description":"0=无 1=低 2=中 3=高"},"dueAt":{"type":"integer","description":"Unix 秒；传 null 清除"}},"required":["id"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const auto id = intField(argsObj.value(), "id");
        if (!id.has_value()) return toolError("缺少必填整数参数 id");
        auto found = findTask(svc_, *id);
        if (found.isFail()) return toolError(found.errorMessage());

        // 整字段更新：先取旧值，再叠加参数里提供的字段
        Task t = found.value();
        const pj::JsonObject& in = argsObj.value();
        if (in.has("title")) {
            pj::JsonValue v = in.get("title");
            if (!v.isString()) return toolError("参数 title 必须是字符串");
            if (v.asString().empty()) return toolError("任务标题不能为空");
            t.title = v.asString();
        }
        if (in.has("description")) {
            pj::JsonValue v = in.get("description");
            if (v.isNull()) {
                t.description = std::nullopt;   // 显式 null = 清空
            } else if (v.isString()) {
                t.description = v.asString();
            } else {
                return toolError("参数 description 必须是字符串或 null");
            }
        }
        if (in.has("priority")) {
            pj::JsonValue v = in.get("priority");
            if (!v.isNumber()) return toolError("参数 priority 必须是整数");
            const int64_t p = v.asInt();
            if (p < 0 || p > 3) {
                return toolError("参数 priority 取值 0-3 的优先级（0=无 1=低 2=中 3=高）");
            }
            t.priority = static_cast<Priority>(p);
        }
        if (in.has("dueAt")) {
            pj::JsonValue v = in.get("dueAt");
            if (v.isNull()) {
                t.dueAt = std::nullopt;         // 显式 null = 清除
            } else if (v.isNumber()) {
                t.dueAt = v.asInt();
            } else {
                return toolError("参数 dueAt 必须是整数或 null");
            }
        }

        if (auto r = svc_.updateTask(t); r.isFail()) {
            return toolError("更新任务失败: " + r.errorMessage());
        }
        auto one = taskToJsonText(t);  // 回喂更新后的完整任务
        if (one.isFail()) return toolError(one.errorMessage());
        return toolOk(one.value());
    }

private:
    TodoService& svc_;
};

// ============ todo.move ============

class TodoMoveTool : public core::ITool {
public:
    explicit TodoMoveTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.move"; }

    std::string schemaJson() const override {
        return R"({"name":"todo.move","description":"移动任务到指定列（可指定插入位置，缺省追加列尾）","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"任务 id（必填）"},"columnId":{"type":"integer","description":"目标列 id（必填）"},"position":{"type":"integer","description":"目标位置，缺省=列尾追加"}},"required":["id","columnId"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const auto id = intField(argsObj.value(), "id");
        if (!id.has_value()) return toolError("缺少必填整数参数 id");
        const auto columnId = intField(argsObj.value(), "columnId");
        if (!columnId.has_value()) return toolError("缺少必填整数参数 columnId");

        auto found = findTask(svc_, *id);
        if (found.isFail()) return toolError(found.errorMessage());
        std::string err;
        if (!columnExists(svc_, *columnId, &err)) return toolError(err);

        // 缺省位置 = 目标列列尾追加
        int newPosition = 0;
        if (auto pos = intField(argsObj.value(), "position")) {
            newPosition = static_cast<int>(*pos);
        } else {
            auto colTasks = svc_.listTasks(*columnId);
            if (colTasks.isFail()) {
                return toolError("读取目标列任务失败: " + colTasks.errorMessage());
            }
            newPosition = static_cast<int>(colTasks.value().size());
        }

        if (auto r = svc_.moveTask(*id, *columnId, newPosition); r.isFail()) {
            return toolError("移动任务失败: " + r.errorMessage());
        }
        return toolOk(R"({"ok":true})");
    }

private:
    TodoService& svc_;
};

// ============ todo.complete ============

class TodoCompleteTool : public core::ITool {
public:
    explicit TodoCompleteTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.complete"; }

    std::string schemaJson() const override {
        return R"({"name":"todo.complete","description":"把任务标记为已完成","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"任务 id（必填）"}},"required":["id"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const auto id = intField(argsObj.value(), "id");
        if (!id.has_value()) return toolError("缺少必填整数参数 id");
        auto found = findTask(svc_, *id);
        if (found.isFail()) return toolError(found.errorMessage());

        if (auto r = svc_.completeTask(*id); r.isFail()) {
            return toolError("标记完成失败: " + r.errorMessage());
        }
        return toolOk(R"({"ok":true})");
    }

private:
    TodoService& svc_;
};

// ============ todo.delete（破坏性）============

// 类名避开 delete 关键字（C++ 关键字不能作标识符），工具名仍是 "todo.delete"
class TodoDeleteTool : public core::ITool {
public:
    explicit TodoDeleteTool(TodoService& svc) : svc_(svc) {}

    std::string name() const override { return "todo.delete"; }

    // 删除不可恢复 → AgentLoop 在 task.allowDestructive=0 时直接终止 run
    bool isDestructive() const override { return true; }

    std::string schemaJson() const override {
        return R"({"name":"todo.delete","description":"删除任务（不可恢复）","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"任务 id（必填）"}},"required":["id"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const auto id = intField(argsObj.value(), "id");
        if (!id.has_value()) return toolError("缺少必填整数参数 id");
        auto found = findTask(svc_, *id);
        if (found.isFail()) return toolError(found.errorMessage());

        if (auto r = svc_.deleteTask(*id); r.isFail()) {
            return toolError("删除任务失败: " + r.errorMessage());
        }
        return toolOk(R"({"ok":true})");
    }

private:
    TodoService& svc_;
};

} // namespace

// ============ TodoTools ============

TodoTools::TodoTools(TodoService& svc) {
    tools_.push_back(std::make_shared<TodoListTool>(svc));
    tools_.push_back(std::make_shared<TodoCreateTool>(svc));
    tools_.push_back(std::make_shared<TodoUpdateTool>(svc));
    tools_.push_back(std::make_shared<TodoMoveTool>(svc));
    tools_.push_back(std::make_shared<TodoCompleteTool>(svc));
    tools_.push_back(std::make_shared<TodoDeleteTool>(svc));
}

void TodoTools::registerAll() {
    for (const auto& tool : tools_) {
        auto r = core::ToolRegistry::instance().registerTool(tool);
        if (r.isFail()) {
            LOG_ERROR("TodoTools: 注册工具失败: " + r.errorMessage());
        }
    }
}

void TodoTools::unregisterAll() {
    for (const auto& tool : tools_) {
        core::ToolRegistry::instance().unregisterTool(tool->name());
    }
}

} // namespace module::todo
