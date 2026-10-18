// AppNotifyTool 实现：参数校验 → platform::notify::send → 失败降级 app toast。
// 返回契约（回喂 LLM）：
//   * 成功：{"ok":true,"accepted":true,"note":"已提交系统通知，是否展示取决于系统设置"}
//   * 降级成功：{"ok":true,"fallback":"app","warning":"系统通知不可用，已改为应用内提示"}
//   * 参数错误 / 降级也失败：Result ok + isError=true + {"error":"<中文>"}
// Result::fail 只留给编程不变量破坏（自有字面量序列化失败）。
#include "app_notify_tool.h"

#include "platform/json.h"
#include "platform/notification.h"

#include <exception>
#include <string>
#include <utility>

namespace module::agent {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// 只裁 ASCII 空白（与 agent_module.cpp 的 trimCopy 同口径）；全角空格不当作空白。
std::string trimAscii(const std::string& text) {
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

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

Result<ToolResult> toolJson(pj::JsonObject object) {
    auto text = pj::stringify(pj::JsonValue(std::move(object)));
    if (text.isFail()) {
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "通知结果序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{false, text.value()});
}

// 参数：空串视为 {}（与 todo/agent 其它工具同口径）；其余必须解析成 object。
Result<pj::JsonObject> parseArgs(const ToolArgs& args) {
    if (args.json.empty()) {
        return Result<pj::JsonObject>::ok(pj::JsonObject());
    }
    auto parsed = pj::parse(args.json);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return Result<pj::JsonObject>::fail(core::ErrorCode::ValidationError,
                                            "参数不是合法 JSON 对象");
    }
    return Result<pj::JsonObject>::ok(parsed.value().asObject());
}

// 必填字符串：缺失 / 非字符串 / trim 后为空都回 isError，不进入平台后端。
Result<std::string> requiredString(const pj::JsonObject& args, const char* key,
                                   const char* label) {
    if (!args.has(key)) {
        return Result<std::string>::fail(
            core::ErrorCode::ValidationError,
            std::string("缺少必填参数 ") + key + "（" + label + "）");
    }
    pj::JsonValue value = args.get(key);
    if (!value.isString()) {
        return Result<std::string>::fail(
            core::ErrorCode::ValidationError,
            std::string("参数 ") + key + " 必须是字符串");
    }
    std::string text = trimAscii(value.asString());
    if (text.empty()) {
        return Result<std::string>::fail(
            core::ErrorCode::ValidationError,
            std::string("参数 ") + key + " 不能为空（trim 后为空）");
    }
    return Result<std::string>::ok(std::move(text));
}

} // namespace

NotifyTool::NotifyTool(FallbackFn fallback) : fallback_(std::move(fallback)) {}

std::string NotifyTool::name() const { return "app.notify"; }

std::string NotifyTool::schemaJson() const {
    return R"({"name":"app.notify","description":"向用户发送系统通知，用于逾期待办提醒、闹钟到点提醒；不要滥用。","input_schema":{"type":"object","properties":{"title":{"type":"string","description":"通知标题，必填，单行简短"},"body":{"type":"string","description":"通知正文，必填"},"sound":{"type":"boolean","description":"是否播放提示音，默认 true"},"id":{"type":"string","description":"可选稳定标识；macOS 用于替换同一 id 的通知，Windows 忽略"}},"required":["title","body"],"additionalProperties":false}})";
}

Result<ToolResult> NotifyTool::execute(const ToolArgs& args) {
    auto parsed = parseArgs(args);
    if (parsed.isFail()) {
        return toolError(parsed.errorMessage());
    }
    const pj::JsonObject& input = parsed.value();

    auto title = requiredString(input, "title", "通知标题");
    if (title.isFail()) {
        return toolError(title.errorMessage());
    }
    auto body = requiredString(input, "body", "通知正文");
    if (body.isFail()) {
        return toolError(body.errorMessage());
    }

    bool sound = true;
    if (input.has("sound")) {
        pj::JsonValue value = input.get("sound");
        if (!value.isBool()) {
            return toolError("参数 sound 必须是布尔值");
        }
        sound = value.asBool();
    }

    std::string id;
    if (input.has("id")) {
        pj::JsonValue value = input.get("id");
        if (!value.isString()) {
            return toolError("参数 id 必须是字符串");
        }
        id = value.asString();
    }

    platform::notify::Notification notification;
    notification.id = std::move(id);
    notification.title = title.value();
    notification.body = body.value();
    notification.sound = sound;

    auto sent = platform::notify::send(notification);
    if (sent.isOk()) {
        pj::JsonObject out;
        out.set("ok", pj::makeBool(true));
        out.set("accepted", pj::makeBool(true));
        out.set("note", pj::makeString("已提交系统通知，是否展示取决于系统设置"));
        return toolJson(std::move(out));
    }

    // 系统通知失败 → 降级应用内提示。fallback 在工作线程执行，只允许线程安全入队；
    // 它抛异常也按「降级失败」处理，不把异常泄给 AgentLoop。
    bool fallbackDone = false;
    std::string fallbackError;
    if (fallback_) {
        try {
            fallback_(notification.title + "：" + notification.body);
            fallbackDone = true;
        } catch (const std::exception& error) {
            fallbackError = error.what();
        } catch (...) {
            fallbackError = "未知异常";
        }
    }

    if (fallbackDone) {
        pj::JsonObject out;
        out.set("ok", pj::makeBool(true));
        out.set("fallback", pj::makeString("app"));
        out.set("warning",
                pj::makeString("系统通知不可用，已改为应用内提示"));
        return toolJson(std::move(out));
    }

    std::string message = "系统通知发送失败: " + sent.errorMessage();
    if (!fallback_) {
        message += "；未配置应用内提示降级";
    } else if (fallbackError.empty()) {
        message += "；应用内提示也失败";
    } else {
        message += "；应用内提示也失败: " + fallbackError;
    }
    return toolError(message);
}

} // namespace module::agent
