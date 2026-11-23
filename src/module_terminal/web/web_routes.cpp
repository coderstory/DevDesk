#include "web_routes.h"

#include "platform/http_server.h"

#include "web_terminal_service.h"

#include <string>

namespace module::terminal::web {

namespace {

void applySecurityHeaders(platform::http::ResponseWriter& rw, bool api) {
    rw.setHeader("X-Content-Type-Options", "nosniff");
    rw.setHeader("Referrer-Policy", "no-referrer");
    if (api) {
        rw.setHeader("Cache-Control", "no-store");
    }
    rw.setHeader("Content-Security-Policy",
                 "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:");
}

std::string placeholderPage() {
    return "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
           "content=\"width=device-width,initial-scale=1\"><title>DevDesk 网页终端</title></head>"
           "<body><p>DevDesk 网页终端资源待嵌入（W5）。</p></body></html>";
}

// 返回 true = 已写 403，调用方不再进 handler
bool rejectOriginHost(const platform::http::RequestCtx& ctx,
                      const WebTerminalService& service,
                      platform::http::ResponseWriter& rw) {
    const auto hostIt = ctx.headers.find("host");
    if (hostIt != ctx.headers.end() && !service.isAllowedHost(hostIt->second)) {
        rw.setStatus(403);
        rw.setHeader("Content-Type", "text/plain; charset=utf-8");
        rw.write("Forbidden: Host not allowed");
        return true;
    }
    const auto originIt = ctx.headers.find("origin");
    if (originIt != ctx.headers.end() && !service.isAllowedOrigin(originIt->second)) {
        rw.setStatus(403);
        rw.setHeader("Content-Type", "text/plain; charset=utf-8");
        rw.write("Forbidden: Origin not allowed");
        return true;
    }
    return false;
}

} // namespace

void registerWebRoutes(platform::http::Server& server, WebTerminalService& service) {
    auto api = [&service](platform::http::RouteHandler handler) {
        return [&service, handler = std::move(handler)](
                   const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw) {
            if (rejectOriginHost(ctx, service, rw)) return;
            handler(ctx, rw);
            applySecurityHeaders(rw, /*api=*/true);
        };
    };
    auto page = [&service](platform::http::RouteHandler handler) {
        return [&service, handler = std::move(handler)](
                   const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw) {
            if (rejectOriginHost(ctx, service, rw)) return;
            handler(ctx, rw);
            applySecurityHeaders(rw, /*api=*/false);
        };
    };

    const platform::http::RouteHandler indexHandler =
        [](const platform::http::RequestCtx&, platform::http::ResponseWriter& rw) {
            rw.setStatus(200);
            rw.setHeader("Content-Type", "text/html; charset=utf-8");
            rw.write(placeholderPage());
        };
    const platform::http::RouteHandler assetPlaceholder =
        [](const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw) {
            // 静态资源占位：真实嵌入由 W5 的 embed_assets 提供；未嵌入前一律 404，
            // 避免把不存在的前端文件伪装成 200 让浏览器报语法错误。
            rw.setStatus(404);
            rw.setHeader("Content-Type", "text/plain; charset=utf-8");
            rw.write(ctx.path == "/assets/" ? "no assets" : "asset not found");
        };

    server.route("GET", "/", page(indexHandler));
    server.route("GET", "/index.html", page(indexHandler));
    server.route("GET", "/bind", page(indexHandler));
    server.route("GET", "/login", page(indexHandler));
    server.route("GET", "/assets/.*", page(assetPlaceholder));
    for (const char* name : {"/app.js", "/app.css", "/protocol.js", "/render.js", "/crypto.js"}) {
        server.route("GET", name, page(assetPlaceholder));
    }

    server.route("GET", "/api/hs", api([&service](const platform::http::RequestCtx& ctx,
                                                  platform::http::ResponseWriter& rw) {
                     service.handleHandshakeGet(ctx, rw);
                 }));
    server.route("POST", "/api/hs", api([&service](const platform::http::RequestCtx& ctx,
                                                   platform::http::ResponseWriter& rw) {
                     service.handleHandshakePost(ctx, rw);
                 }));

    server.route("POST", "/api/e", api([&service](const platform::http::RequestCtx& ctx,
                                                  platform::http::ResponseWriter& rw) {
                     service.handleEnvelope(ctx, rw);
                 }));
    for (const char* alias : {"/api/input", "/api/settings", "/api/login", "/api/logout",
                              "/api/bind/claim", "/api/bind/confirm", "/api/bind/revoke"}) {
        server.route("POST", alias,
                     api([&service, alias](const platform::http::RequestCtx& ctx,
                                           platform::http::ResponseWriter& rw) {
                         (void)alias;
                         if (std::string(alias) == "/api/input") {
                             service.handleInput(ctx, rw);
                         } else if (std::string(alias) == "/api/settings") {
                             service.handleSettings(ctx, rw);
                         } else {
                             service.handleEnvelope(ctx, rw);
                         }
                     }));
    }

    server.route("GET", "/api/stream", api([&service](const platform::http::RequestCtx& ctx,
                                                      platform::http::ResponseWriter& rw) {
                     service.handleStream(ctx, rw);
                 }));
    server.route("GET", "/api/status", api([&service](const platform::http::RequestCtx& ctx,
                                                      platform::http::ResponseWriter& rw) {
                     service.handleStatus(ctx, rw);
                 }));
}

} // namespace module::terminal::web
