// AppNotifyTool 实现：参数校验 → platform::notify::send → Receipt 状态映射（spec §7.4）。
// 限速/去重/截断全在平台 facade；本文件只做参数校验、状态映射与降级入队。
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

// 环境类失败（NotBundled/PermissionDenied/NotDetermined/BackendUnavailable）：
// 降级为应用内提示。fallback 在工作线程执行，只允许线程安全入队；它抛异常也按
//「降级失败」处理，不把异常泄给 AgentLoop。
Result<ToolResult> fallbackOrError(const platform::notify::Receipt& receipt,
                                   const std::string& fallbackText,
                                   const NotifyTool::FallbackFn& fallback) {
    const char* reason = platform::notify::statusToString(receipt.status);
    // facade 对每个 status 都填了中文 detail；这里只做空串防御。
    const std::string detail = receipt.detail.empty()
                                   ? platform::notify::statusToString(receipt.status)
                                   : receipt.detail;

    if (fallback) {
        bool fallbackDone = false;
        std::string fallbackError;
        try {
            fallback(fallbackText);
            fallbackDone = true;
        } catch (const std::exception& error) {
            fallbackError = error.what();
        } catch (...) {
            fallbackError = "未知异常";
        }
        if (fallbackDone) {
            pj::JsonObject out;
            out.set("status", pj::makeString("fallback"));
            out.set("deliveredVia", pj::makeString("app_toast"));
            out.set("reason", pj::makeString(reason));
            out.set("detail", pj::makeString(detail));
            out.set("id", pj::makeString(receipt.effectiveId));
            return toolJson(std::move(out));
        }
        std::string message = "系统通知不可用: " + detail;
        message += "；应用内提示也失败";
        if (!fallbackError.empty()) {
            message += ": " + fallbackError;
        }
        return toolError(message);
    }

    return toolError("系统通知不可用: " + detail + "；未配置应用内提示降级");
}

} // namespace

NotifyTool::NotifyTool(FallbackFn fallback) : fallback_(std::move(fallback)) {}

std::string NotifyTool::name() const { return "app.notify"; }

std::string NotifyTool::schemaJson() const {
    return R"({"name":"app.notify","description":"向用户发送系统通知，用于逾期待办提醒、闹钟到点提醒；不要滥用。","input_schema":{"type":"object","properties":{"title":{"type":"string","description":"通知标题，必填，单行简短"},"body":{"type":"string","description":"通知正文，必填"},"sound":{"type":"boolean","description":"是否播放提示音，默认 true"},"level":{"type":"string","enum":["info","success","warning","error"],"description":"通知级别，默认 info"},"id":{"type":"string","description":"可选稳定标识；相同 id + 内容在 2 秒内去重；相同 id 会替换 macOS 中未展示/已展示的同 id 通知"}},"required":["title","body"],"additionalProperties":false}})";
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

    // level：缺省 Info；未知值报工具错误，不静默降级成 Info（spec §7.3）。
    platform::notify::Level level = platform::notify::Level::Info;
    if (input.has("level")) {
        pj::JsonValue value = input.get("level");
        if (!value.isString()) {
            return toolError("参数 level 必须是字符串");
        }
        const std::string text = value.asString();
        if (text == "info") {
            level = platform::notify::Level::Info;
        } else if (text == "success") {
            level = platform::notify::Level::Success;
        } else if (text == "warning") {
            level = platform::notify::Level::Warning;
        } else if (text == "error") {
            level = platform::notify::Level::Error;
        } else {
            return toolError("参数 level 非法: " + text +
                             "（只支持 info / success / warning / error）");
        }
    }

    platform::notify::Notification notification;
    notification.id = std::move(id);
    notification.title = title.value();
    notification.body = body.value();
    notification.sound = sound;
    notification.level = level;

    auto sent = platform::notify::send(notification);
    // Result::fail 只发生在参数非法（清洗后为空）或 facade 内部错误：不降级，
    // 直接回喂错误（失败矩阵 #1/#2 的「降级=无」）。
    if (sent.isFail()) {
        return toolError(sent.errorMessage());
    }
    const platform::notify::Receipt& receipt = sent.value();

    switch (receipt.status) {
        case platform::notify::Status::Accepted: {
            pj::JsonObject out;
            out.set("status", pj::makeString("accepted"));
            out.set("deliveredVia", pj::makeString("system"));
            out.set("id", pj::makeString(receipt.effectiveId));
            out.set("truncated", pj::makeBool(receipt.titleTruncated ||
                                              receipt.bodyTruncated));
            return toolJson(std::move(out));
        }
        case platform::notify::Status::Deduped: {
            pj::JsonObject out;
            out.set("status", pj::makeString("deduped"));
            out.set("deliveredVia", pj::makeString("none"));
            out.set("id", pj::makeString(receipt.effectiveId));
            return toolJson(std::move(out));
        }
        case platform::notify::Status::RateLimited: {
            pj::JsonObject out;
            out.set("status", pj::makeString("rate_limited"));
            out.set("deliveredVia", pj::makeString("none"));
            out.set("retryAfterMs", pj::makeInt(receipt.retryAfterMs));
            return toolJson(std::move(out));
        }
        default:
            // NotBundled / PermissionDenied / NotDetermined / BackendUnavailable
            // → 应用内提示降级。
            return fallbackOrError(
                receipt, notification.title + "：" + notification.body, fallback_);
    }
}

} // namespace module::agent
