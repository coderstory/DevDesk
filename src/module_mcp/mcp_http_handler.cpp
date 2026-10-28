#include "mcp_http_handler.h"

#include <cctype>
#include <cstddef>
#include <string>

#include "platform/http_server.h"

namespace module::mcp {

namespace {

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string lowercased(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// 入站头键已由 platform::http 统一小写；缺失返回空串。
std::string headerValue(const platform::http::RequestCtx& ctx,
                        const std::string& key) {
    auto it = ctx.headers.find(key);
    return it == ctx.headers.end() ? std::string() : it->second;
}

// POST 只接受 application/json（允许 ; charset=... 后缀，大小写不敏感）
bool isJsonContentType(const std::string& raw) {
    std::string main = lowercased(trimmed(raw));
    const auto semicolon = main.find(';');
    if (semicolon != std::string::npos) {
        main = trimmed(main.substr(0, semicolon));
    }
    return main == "application/json";
}

// Origin 只放行回环三形态；忽略 scheme 与端口。null / 无法解析一律拒绝
//（无 Origin = 非浏览器客户端，放行）。
bool isOriginAllowed(const std::string& raw) {
    std::string value = trimmed(raw);
    const auto schemeEnd = value.find("://");
    if (schemeEnd != std::string::npos) {
        value = value.substr(schemeEnd + 3);
    }
    const auto pathEnd = value.find_first_of("/?#");
    if (pathEnd != std::string::npos) value = value.substr(0, pathEnd);
    // Origin 按规范不含 userinfo；出现即视为不可解析
    if (value.find('@') != std::string::npos) return false;
    if (value.empty()) return false;

    std::string host;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string::npos) return false;
        host = value.substr(1, close - 1);
    } else {
        const auto colon = value.find(':');
        host = colon == std::string::npos ? value : value.substr(0, colon);
    }
    host = lowercased(host);
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

void writeEmpty(platform::http::ResponseWriter& writer, int status) {
    writer.setStatus(status);
    writer.end();
}

} // namespace

McpHttpHandler::McpHttpHandler(McpProtocol& protocol) : protocol_(protocol) {}

void McpHttpHandler::registerRoutes(platform::http::Server& server) {
    server.route("POST", "/mcp",
                 [this](const platform::http::RequestCtx& ctx,
                        platform::http::ResponseWriter& writer) {
                     handlePost(ctx, writer);
                 });
    server.route("GET", "/mcp",
                 [this](const platform::http::RequestCtx&,
                        platform::http::ResponseWriter& writer) {
                     handleMethodNotAllowed(writer);
                 });
    server.route("DELETE", "/mcp",
                 [this](const platform::http::RequestCtx& ctx,
                        platform::http::ResponseWriter& writer) {
                     handleDelete(ctx, writer);
                 });
    server.route("OPTIONS", "/mcp",
                 [this](const platform::http::RequestCtx&,
                        platform::http::ResponseWriter& writer) {
                     handleMethodNotAllowed(writer);
                 });
}

void McpHttpHandler::handlePost(const platform::http::RequestCtx& ctx,
                                platform::http::ResponseWriter& writer) {
    if (!isJsonContentType(headerValue(ctx, "content-type"))) {
        writeEmpty(writer, 415);
        return;
    }
    if (ctx.headers.count("origin") != 0 &&
        !isOriginAllowed(headerValue(ctx, "origin"))) {
        writeEmpty(writer, 403);
        return;
    }

    std::string newSessionId;
    const HttpOutcome outcome = protocol_.handlePost(
        ctx.body, headerValue(ctx, "mcp-session-id"),
        headerValue(ctx, "mcp-protocol-version"), newSessionId);

    writer.setStatus(outcome.status);
    if (!newSessionId.empty()) {
        writer.setHeader("Mcp-Session-Id", newSessionId);
    }
    if (outcome.notificationOnly) {
        writer.end();
        return;
    }
    if (!outcome.body.empty()) {
        writer.setHeader("Content-Type", "application/json; charset=utf-8");
        writer.write(outcome.body);
    }
    writer.end();
}

void McpHttpHandler::handleDelete(const platform::http::RequestCtx& ctx,
                                  platform::http::ResponseWriter& writer) {
    const HttpOutcome outcome =
        protocol_.handleDelete(headerValue(ctx, "mcp-session-id"));
    writeEmpty(writer, outcome.status);
}

void McpHttpHandler::handleMethodNotAllowed(
    platform::http::ResponseWriter& writer) {
    writer.setStatus(405);
    writer.setHeader("Allow", "POST, DELETE");
    writer.end();
}

} // namespace module::mcp