// agent.runs 实现：过滤参数 → AgentStore::listRunsFiltered，出参 JSON 数组。
// 只读零副作用；错误语义 = ok + isError + 中文说明（schedule_runs_tool 同款口径）。
#include "agent_runs_tool.h"

#include "core/time.h"
#include "platform/json.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace module::agent {
namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

constexpr int kDefaultLimit = 20;
constexpr int kMaxLimit = 200;
constexpr double kDefaultSinceHours = 24.0;

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

std::string strField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

std::optional<int64_t> intField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    if (!v.isNumber()) return std::nullopt;
    return std::optional<int64_t>(v.asInt());
}

Result<std::string> arrayOfJsonTexts(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += items[i];
    }
    out += "]";
    if (pj::parse(out).isFail()) {
        return Result<std::string>::fail(core::ErrorCode::Internal,
                                         "结果数组组装校验失败");
    }
    return Result<std::string>::ok(out);
}

class AgentRunsTool : public core::ITool {
public:
    explicit AgentRunsTool(AgentStore& store) : store_(store) {}

    std::string name() const override { return "agent.runs"; }

    std::string schemaJson() const override {
        return R"({"name":"agent.runs","description":"查询智能体任务运行历史（只读，agent_runs 表）；status 缺省排除运行中的记录（含当前自查）","input_schema":{"type":"object","properties":{"status":{"type":"string","description":"按状态过滤：completed/failed/limited/cancelled/running；缺省=排除 running"},"sinceHours":{"type":"number","description":"只查最近 N 小时，默认 24"},"taskId":{"type":"integer","description":"只查指定任务 id"},"triggerType":{"type":"string","description":"按任务触发类型过滤：manual/schedule"},"limit":{"type":"integer","description":"返回条数上限，默认 20，最大 200"}}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) {
            return toolError(argsObj.errorMessage());
        }
        const pj::JsonObject& in = argsObj.value();
        const std::string status = strField(in, "status");
        double sinceHours = kDefaultSinceHours;
        if (pj::JsonValue v = in.get("sinceHours"); v.isNumber()) {
            sinceHours = v.asDouble() < 0 ? 0 : v.asDouble();
        }
        int64_t sinceSec = 0;
        if (sinceHours > 0) {
            sinceSec = static_cast<int64_t>(core::time::nowUnix()) -
                       static_cast<int64_t>(sinceHours * 3600.0);
        }
        auto taskId = intField(in, "taskId");
        const std::string triggerType = strField(in, "triggerType");
        int64_t limit = kDefaultLimit;
        if (auto v = intField(in, "limit")) limit = *v;
        limit = std::clamp(limit, int64_t{1}, int64_t{kMaxLimit});

        auto rows = store_.listRunsFiltered(status, sinceSec,
                                            taskId.value_or(0), triggerType,
                                            static_cast<int>(limit));
        if (rows.isFail()) {
            return toolError("运行历史查询失败: " + rows.errorMessage());
        }

        std::vector<std::string> items;
        items.reserve(rows.value().size());
        for (const auto& r : rows.value()) {
            pj::JsonObject o;
            o.set("id", pj::makeInt(r.id));
            o.set("taskId", pj::makeInt(r.taskId));
            o.set("taskName", pj::makeString(r.taskName));
            o.set("status", pj::makeString(r.status));
            o.set("startedAt", pj::makeInt(r.startedAt));
            o.set("endedAt", r.endedAt.has_value()
                                 ? pj::JsonValue(r.endedAt.value())
                                 : pj::JsonValue(nullptr));
            int64_t durationSec = 0;
            if (r.endedAt.has_value() && r.endedAt.value() > r.startedAt) {
                durationSec = r.endedAt.value() - r.startedAt;
            }
            o.set("durationSeconds", pj::makeInt(durationSec));
            o.set("iterations",
                  pj::makeInt(static_cast<int64_t>(r.iterations)));
            o.set("error", r.error.has_value() ? pj::JsonValue(r.error.value())
                                               : pj::JsonValue(nullptr));
            auto text = pj::stringify(pj::JsonValue(std::move(o)));
            if (text.isFail()) {
                return toolError("运行记录序列化失败: " + text.errorMessage());
            }
            items.push_back(text.value());
        }
        auto arr = arrayOfJsonTexts(items);
        if (arr.isFail()) {
            return toolError(arr.errorMessage());
        }
        return Result<ToolResult>::ok(ToolResult{false, arr.value()});
    }

private:
    AgentStore& store_;
};

} // namespace

std::shared_ptr<core::ITool> makeAgentRunsTool(AgentStore& store) {
    return std::make_shared<AgentRunsTool>(store);
}

} // namespace module::agent
