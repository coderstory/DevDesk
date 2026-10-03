// src/module_agent/app_stats_tool.cpp
// AppStatsTool 实现（agent M1 Task 10）：三表 COUNT(*) 直查。
// 表名与 migrations.h kAgentTables（v12）/ kCreateInitialTables（v1）对齐：
//   todos      ← todo_tasks
//   notes      ← markdown_index
//   agentTasks ← agent_tasks
#include "app_stats_tool.h"

#include "platform/json.h"

#include <string>
#include <utility>
#include <variant>

namespace module::agent {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// 工具级错误：Result ok + isError + {"error":"<中文>"}（错误回喂 LLM）
Result<ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "工具错误消息序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, text.value()});
}

class AppStatsTool : public core::ITool {
public:
    explicit AppStatsTool(core::data::SqliteDb& db) : db_(db) {}

    std::string name() const override { return "app.stats"; }

    std::string schemaJson() const override {
        return R"({"name":"app.stats","description":"查询应用统计（待办任务数 / 笔记数 / 智能体任务数），只读","input_schema":{"type":"object","properties":{}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;  // 无参数

        auto todos = countRows("todo_tasks");
        if (todos.isFail()) {
            return toolError("统计查询失败: " + todos.errorMessage());
        }
        auto notes = countRows("markdown_index");
        if (notes.isFail()) {
            return toolError("统计查询失败: " + notes.errorMessage());
        }
        auto agentTasks = countRows("agent_tasks");
        if (agentTasks.isFail()) {
            return toolError("统计查询失败: " + agentTasks.errorMessage());
        }

        pj::JsonObject o;
        o.set("todos", pj::makeInt(todos.value()));
        o.set("notes", pj::makeInt(notes.value()));
        o.set("agentTasks", pj::makeInt(agentTasks.value()));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                            "统计结果序列化失败: " +
                                                text.errorMessage());
        }
        return Result<ToolResult>::ok(ToolResult{false, text.value()});
    }

private:
    // SELECT COUNT(*) AS n 单行单列；按 batch 裁定取 values[0]（SqlRow 的
    // 按名取值 getInt64 同效，但 COUNT 恒整数，直接取首列更直白）
    Result<int64_t> countRows(const char* table) {
        auto prep = db_.prepare(std::string("SELECT COUNT(*) AS n FROM ") +
                                table);
        if (prep.isFail()) {
            return Result<int64_t>::fail(prep.errorCode(),
                                         prep.errorMessage());
        }
        auto& stmt = prep.value();
        auto step = stmt.step();
        if (step.isFail()) {
            return Result<int64_t>::fail(step.errorCode(),
                                         step.errorMessage());
        }
        auto row = stmt.currentRow();
        if (row.isFail()) {
            return Result<int64_t>::fail(row.errorCode(),
                                         row.errorMessage());
        }
        const auto& r = row.value();
        if (r.values.empty() ||
            !std::holds_alternative<int64_t>(r.values[0])) {
            return Result<int64_t>::fail(
                core::ErrorCode::Internal,
                std::string("COUNT 查询返回异常形状: ") + table);
        }
        return Result<int64_t>::ok(std::get<int64_t>(r.values[0]));
    }

    core::data::SqliteDb& db_;
};

} // namespace

std::shared_ptr<core::ITool> makeAppStatsTool(core::data::SqliteDb& db) {
    return std::make_shared<AppStatsTool>(db);
}

} // namespace module::agent
