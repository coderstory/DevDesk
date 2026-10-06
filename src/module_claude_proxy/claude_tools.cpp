// src/module_claude_proxy/claude_tools.cpp
// ClaudeTools 实现。出参形状约定：结构化数据组对象再 stringify；数组用「逐元素
// stringify + 文本拼接 + parse 校验」组装（platform::json 没有数组追加 API）。
#include "claude_tools.h"

#include "core/logging.h"
#include "platform/json.h"

#include <optional>
#include <string>
#include <utility>

namespace module::claude {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// 结果组装辅助

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

// 参数解析：空串视为 {}；其余必须解析成 object。失败 = 参数非法（工具级错误）
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

// 结果对象 → contentJson 文本（三处共用；序列化失败 = 工具级错误）
Result<std::string> stringifyObj(pj::JsonObject obj) {
    auto text = pj::stringify(pj::JsonValue(std::move(obj)));
    if (text.isFail()) {
        return Result<std::string>::fail(core::ErrorCode::Internal,
                                         "结果序列化失败: " + text.errorMessage());
    }
    return Result<std::string>::ok(text.value());
}

// 可选整数参数：缺失 → nullopt。platform::json 带小数点/指数的 literal 存 Double（asInt 恒 0），
// 故 isNumber+asInt 会挡掉 1.5/1.0，不静默取整；字符串/布尔/null 同样拒绝。
Result<std::optional<int64_t>> optionalIntField(const pj::JsonObject& obj,
                                                const char* key) {
    if (!obj.has(key)) {
        return Result<std::optional<int64_t>>::ok(std::nullopt);
    }
    pj::JsonValue v = obj.get(key);
    const bool integral =
        v.isNumber() && !(v.asInt() == 0 && v.asDouble() != 0.0);
    if (!integral) {
        return Result<std::optional<int64_t>>::fail(
            core::ErrorCode::ValidationError,
            std::string("参数 ") + key + " 必须是整数");
    }
    return Result<std::optional<int64_t>>::ok(
        std::optional<int64_t>(v.asInt()));
}

// claude.list_profiles（只读）

class ClaudeListProfilesTool : public core::ITool {
public:
    explicit ClaudeListProfilesTool(ClaudeService& svc) : svc_(svc) {}

    std::string name() const override { return "claude.list_profiles"; }

    std::string schemaJson() const override {
        return R"({"name":"claude.list_profiles","description":"列出全部 Claude API Profile，并标出当前激活的那一个（只读，无入参）","input_schema":{"type":"object","properties":{},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        auto profiles = svc_.listProfiles();
        if (profiles.isFail()) {
            return toolError("读取 Profile 列表失败: " + profiles.errorMessage());
        }

        // 激活 id：无激活 profile 是**正常状态**（NoActiveProfile）→ 全部标 false；
        // 其它错误（DB 异常）不能静默成「全未激活」，照实回喂
        int64_t activeId = 0;
        auto active = svc_.getActiveProfile();
        if (active.isOk()) {
            activeId = active.value().id;
        } else if (active.errorCode() != core::ErrorCode::NoActiveProfile) {
            return toolError("读取激活 Profile 失败: " + active.errorMessage());
        }

        // 数组组装：逐元素 stringify 后文本拼接（markdown_tools.cpp 同款）
        std::string joined;
        for (const auto& p : profiles.value()) {
            pj::JsonObject o;
            o.set("id", pj::makeInt(p.id));
            o.set("name", pj::makeString(p.name));
            o.set("active", pj::makeBool(p.id == activeId));
            auto one = stringifyObj(std::move(o));
            if (one.isFail()) return toolError(one.errorMessage());
            if (!joined.empty()) joined += ",";
            joined += one.value();
        }
        const std::string content = "{\"profiles\":[" + joined + "]}";
        // 自有拼接必须可解析；失败 = 不变量破坏
        if (pj::parse(content).isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal, "工具结果 JSON 拼接失败（编程错误）");
        }
        return toolOk(content);
    }

private:
    ClaudeService& svc_;
};

// claude.switch_profile（破坏性）

class ClaudeSwitchProfileTool : public core::ITool {
public:
    explicit ClaudeSwitchProfileTool(ClaudeService& svc) : svc_(svc) {}

    std::string name() const override { return "claude.switch_profile"; }

    // 切激活 Profile = 不可逆的全局副作用（代理运行中立即改写 settings.json）；
    // allowDestructive=0 时 AgentLoop 直接终止 run。
    bool isDestructive() const override { return true; }

    std::string schemaJson() const override {
        return R"({"name":"claude.switch_profile","description":"切换当前激活的 Claude API Profile（破坏性：会改写本地代理的接管配置，代理运行中立即影响 Claude Code 的上游）","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"目标 Profile 的 id（必填，正整数，可用 claude.list_profiles 查）"}},"required":["id"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        auto idOpt = optionalIntField(argsObj.value(), "id");
        if (idOpt.isFail()) return toolError(idOpt.errorMessage());
        if (!idOpt.value().has_value()) {
            return toolError("缺少必填参数 id（目标 Profile 的整数 id）");
        }
        const int64_t id = *idOpt.value();

        // 服务层做全部校验（id=0 / 不存在 / settings.json 读失败中止切换）；
        // 失败原消息回喂（带 id / 原因，便于 LLM 纠正后重试）
        auto r = svc_.setActiveProfile(id);
        if (r.isFail()) {
            return toolError(r.errorMessage());
        }

        pj::JsonObject o;
        o.set("id", pj::makeInt(id));
        o.set("switched", pj::makeBool(true));
        auto text = stringifyObj(std::move(o));
        if (text.isFail()) return toolError(text.errorMessage());
        return toolOk(text.value());
    }

private:
    ClaudeService& svc_;
};

// claude.test_connection（只读，发测试请求）

class ClaudeTestConnectionTool : public core::ITool {
public:
    explicit ClaudeTestConnectionTool(ClaudeService& svc) : svc_(svc) {}

    std::string name() const override { return "claude.test_connection"; }

    // 只读探测（GET /v1/models），不改任何本地状态
    std::string schemaJson() const override {
        return R"({"name":"claude.test_connection","description":"对某个 Profile 的上游发一次连通性测试（GET /v1/models，只读）。不传 id 则测当前激活的 Profile","input_schema":{"type":"object","properties":{"id":{"type":"integer","description":"目标 Profile 的 id（可选；缺省 = 当前激活的 Profile）"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        auto idOpt = optionalIntField(argsObj.value(), "id");
        if (idOpt.isFail()) return toolError(idOpt.errorMessage());

        int64_t id = 0;
        if (idOpt.value().has_value()) {
            id = *idOpt.value();
        } else {
            auto active = svc_.getActiveProfile();
            if (active.isFail()) {
                return toolError(
                    "未指定 id 且取不到激活 Profile: " + active.errorMessage());
            }
            id = active.value().id;
        }

        auto prof = svc_.getProfile(id);
        if (prof.isFail()) {
            return toolError(prof.errorMessage());   // ProfileNotFound 原消息（带 id）
        }
        auto key = svc_.loadApiKey(id);
        if (key.isFail()) {
            return toolError("读取该 Profile 的 API Key 失败: " + key.errorMessage());
        }

        Profile p = std::move(prof).value();
        p.apiKeyPlain = key.value();
        auto resp = svc_.testConnection(p);
        if (resp.isFail()) {
            return toolError("连通测试失败: " + resp.errorMessage());
        }

        const int64_t code = resp.value().statusCode;
        pj::JsonObject o;
        o.set("statusCode", pj::makeInt(code));
        // ok 判据与 UI 的「测试连通」同源（claude_module.cpp：200..299 = 成功）
        o.set("ok", pj::makeBool(code >= 200 && code < 300));
        auto text = stringifyObj(std::move(o));
        if (text.isFail()) return toolError(text.errorMessage());
        return toolOk(text.value());
    }

private:
    ClaudeService& svc_;
};

// claude.recent_logs（只读）

class ClaudeRecentLogsTool : public core::ITool {
public:
    explicit ClaudeRecentLogsTool(ClaudeService& svc) : svc_(svc) {}

    std::string name() const override { return "claude.recent_logs"; }

    std::string schemaJson() const override {
        return R"({"name":"claude.recent_logs","description":"最近经过本地代理的 Claude 调用日志（只读，按时间倒序）","input_schema":{"type":"object","properties":{"limit":{"type":"integer","description":"返回条数（可选，默认 20，取值被钳制到 1..100）"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        auto limitOpt = optionalIntField(argsObj.value(), "limit");
        if (limitOpt.isFail()) return toolError(limitOpt.errorMessage());

        // 默认 20；clamp [1,100]（上限防止一次工具调用回灌巨量日志给 LLM）
        int64_t limit = limitOpt.value().value_or(20);
        if (limit < 1) limit = 1;
        if (limit > 100) limit = 100;

        auto logs = svc_.recentCalls(static_cast<int>(limit));
        if (logs.isFail()) {
            return toolError("读取调用日志失败: " + logs.errorMessage());
        }

        std::string joined;
        for (const auto& e : logs.value()) {
            pj::JsonObject o;
            o.set("id", pj::makeInt(e.id));
            o.set("statusCode", pj::makeInt(e.statusCode));
            o.set("latencyMs", pj::makeInt(e.latencyMs));
            o.set("error", pj::makeString(e.error));
            o.set("startedAt", pj::makeInt(static_cast<int64_t>(e.startedAt)));
            auto one = stringifyObj(std::move(o));
            if (one.isFail()) return toolError(one.errorMessage());
            if (!joined.empty()) joined += ",";
            joined += one.value();
        }
        const std::string content = "{\"logs\":[" + joined + "]}";
        if (pj::parse(content).isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal, "工具结果 JSON 拼接失败（编程错误）");
        }
        return toolOk(content);
    }

private:
    ClaudeService& svc_;
};

} // namespace

// ClaudeTools

ClaudeTools::ClaudeTools(ClaudeService& svc) {
    tools_.push_back(std::make_shared<ClaudeListProfilesTool>(svc));
    tools_.push_back(std::make_shared<ClaudeSwitchProfileTool>(svc));
    tools_.push_back(std::make_shared<ClaudeTestConnectionTool>(svc));
    tools_.push_back(std::make_shared<ClaudeRecentLogsTool>(svc));
}

void ClaudeTools::registerAll() {
    for (const auto& tool : tools_) {
        auto r = core::ToolRegistry::instance().registerTool(tool);
        if (r.isFail()) {
            LOG_ERROR("ClaudeTools: 注册工具失败: " + r.errorMessage());
        }
    }
}

void ClaudeTools::unregisterAll() {
    for (const auto& tool : tools_) {
        core::ToolRegistry::instance().unregisterTool(tool->name());
    }
}

} // namespace module::claude
