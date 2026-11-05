// schedule.runs 实现：过滤参数 → ScheduleStore::listRuns，出参 JSON 数组。
// 只读零副作用（isDestructive 默认 false）；错误语义 = ok + isError + 中文说明
//（错误回喂 LLM 自我纠正），Result::fail 只留给编程不变量破坏。
#include "schedule_runs_tool.h"

#include "core/time.h"
#include "platform/json.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace module::schedule {
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

// 参数解析：空串视为 {}（LLM 的 input 可能为空对象省略）；其余必须解析成 object
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

// JSON 数组组装：platform::json 无通用数组追加 API，逐元素 stringify 后文本
// 拼接，再 parse 校验（todo_tools 同款口径）
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

class ScheduleRunsTool : public core::ITool {
public:
    explicit ScheduleRunsTool(ScheduleStore& store) : store_(store) {}

    std::string name() const override { return "schedule.runs"; }

    std::string schemaJson() const override {
        return R"({"name":"schedule.runs","description":"查询定时任务执行历史（只读，schedule_runs 表）","input_schema":{"type":"object","properties":{"status":{"type":"string","description":"按状态过滤：pending/running/success/failed/skipped/missed/suppressed；缺省=全部"},"sinceHours":{"type":"number","description":"只查最近 N 小时，默认 24"},"ruleId":{"type":"integer","description":"只查指定规则 id"},"limit":{"type":"integer","description":"返回条数上限，默认 20，最大 200"}}}})";
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
        auto ruleId = intField(in, "ruleId");
        int64_t limit = kDefaultLimit;
        if (auto v = intField(in, "limit")) limit = *v;
        limit = std::clamp(limit, int64_t{1}, int64_t{kMaxLimit});

        auto rows = store_.listRuns(status, sinceSec, ruleId,
                                    static_cast<int>(limit));
        if (rows.isFail()) {
            return toolError("执行历史查询失败: " + rows.errorMessage());
        }

        std::vector<std::string> items;
        items.reserve(rows.value().size());
        for (const auto& r : rows.value()) {
            pj::JsonObject o;
            o.set("id", pj::makeInt(r.id));
            o.set("origin", pj::makeString(r.origin));
            o.set("ruleId", r.ruleId.has_value()
                                ? pj::JsonValue(r.ruleId.value())
                                : pj::JsonValue(nullptr));
            o.set("plannedAt", pj::makeInt(r.plannedAt));
            o.set("status", pj::makeString(r.status));
            o.set("attempt", pj::makeInt(static_cast<int64_t>(r.attempt)));
            o.set("durationMs", r.durationMs.has_value()
                                    ? pj::JsonValue(r.durationMs.value())
                                    : pj::JsonValue(nullptr));
            o.set("error", pj::makeString(r.error));
            o.set("resultSummary", pj::makeString(r.resultSummary));
            auto text = pj::stringify(pj::JsonValue(std::move(o)));
            if (text.isFail()) {
                return toolError("执行记录序列化失败: " + text.errorMessage());
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
    ScheduleStore& store_;
};

} // namespace

std::shared_ptr<core::ITool> makeScheduleRunsTool(ScheduleStore& store) {
    return std::make_shared<ScheduleRunsTool>(store);
}

} // namespace module::schedule
