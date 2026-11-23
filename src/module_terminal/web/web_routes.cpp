#include "web_routes.h"

#include "platform/http_server.h"

#include "web_assets.h"
#include "web_terminal_service.h"

#include <string>
#include <string_view>

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

// If-None-Match 与构建期 ETag 比对（容忍 W/ 弱标记与多值列表）
bool etagMatches(const platform::http::RequestCtx& ctx, const char* etag) {
    const auto it = ctx.headers.find("if-none-match");
    if (it == ctx.headers.end()) return false;
    std::string_view header = it->second;
    while (!header.empty()) {
        const auto comma = header.find(',');
        std::string_view item = header.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item.rfind("W/", 0) == 0) item.remove_prefix(2);
        if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
            item.remove_prefix(1);
            item.remove_suffix(1);
        }
        if (item == etag) return true;
        if (comma == std::string_view::npos) break;
        header.remove_prefix(comma + 1);
    }
    return false;
}

// 静态资源出口：命中嵌入表才 200；未命中 404 不伪装。HTML 每次回源校验（no-cache），
// JS/CSS 走 ETag + 1h 缓存（未做文件名指纹，不能 immutable）。
void serveAsset(const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw,
                std::string_view name, bool isDocument) {
    const EmbeddedAsset* asset = findEmbeddedAsset(name);
    if (asset == nullptr) {
        rw.setStatus(404);
        rw.setHeader("Content-Type", "text/plain; charset=utf-8");
        rw.write("asset not found");
        return;
    }
    const std::string quotedEtag = std::string("\"") + asset->etag + "\"";
    rw.setHeader("ETag", quotedEtag);
    rw.setHeader("Cache-Control", isDocument ? "no-cache" : "public, max-age=3600");
    rw.setHeader("Content-Type", asset->mime);
    if (etagMatches(ctx, asset->etag)) {
        rw.setStatus(304);
        return;
    }
    rw.setStatus(200);
    rw.write(std::string(asset->data, asset->size));
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
        [](const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw) {
            serveAsset(ctx, rw, "index.html", /*isDocument=*/true);
        };
    // 兼容两种引用形态：index.html 里写的是相对路径（/app.css），/assets/* 留给
    // 带前缀部署与后续子目录扩展；两者都查同一张嵌入表。
    const platform::http::RouteHandler assetsHandler =
        [](const platform::http::RequestCtx& ctx, platform::http::ResponseWriter& rw) {
            std::string_view name = ctx.path;
            if (name.rfind("/assets/", 0) == 0) name.remove_prefix(8);
            while (!name.empty() && name.front() == '/') name.remove_prefix(1);
            serveAsset(ctx, rw, name, /*isDocument=*/false);
        };

    server.route("GET", "/", page(indexHandler));
    server.route("GET", "/index.html", page(indexHandler));
    server.route("GET", "/bind", page(indexHandler));
    server.route("GET", "/login", page(indexHandler));
    server.route("GET", "/assets/.*", page(assetsHandler));
    for (const char* name : {"/app.js", "/app.css", "/protocol.js", "/render.js", "/crypto.js"}) {
        server.route("GET", name, page(assetsHandler));
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
