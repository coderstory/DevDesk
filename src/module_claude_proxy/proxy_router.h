// src/module_claude_proxy/proxy_router.h
// 本地 Claude 代理路由（spec §10.4 非流式版本）
//
// 流程（每次请求）：
//   1. 客户端 POST http://127.0.0.1:<port>/<path>  + Authorization: Bearer <token>
//   2. http_server 已用 pre_routing_handler 校验 token（401 拦截）
//   3. ProxyRouter::route 接收请求：
//      - 查 active profile（无 → 503 Service Unavailable）
//      - 读 API key from Keychain
//      - 用 platform::http::send 转发到 base_url + path
//      - 写回上游响应（含 status / headers / body）
//      - 写入一条 claude_call_logs
//
// 流式实现留到 P10a（P9 之后）。
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "claude_service.h"
#include "platform/http_server.h"

namespace module::claude {

class ProxyRouter {
public:
    ProxyRouter(ClaudeService& service, platform::http::Server& server);
    ~ProxyRouter() = default;

    // 注册代理路由：把所有 method + 所有 path 转发到 Claude 上游
    void registerRoutes();

    // 内部：对单条请求做一次完整转发（也可由测试直接调用）
    //   - method/path/headers/body：来自客户端
    //   - 返回的 ResponseWriter 由调用方写回客户端
    void handleRequest(const std::string& method,
                       const std::string& path,
                       const std::map<std::string, std::string>& headers,
                       const std::string& body,
                       platform::http::ResponseWriter& out);

private:
    ClaudeService& service_;
    platform::http::Server& server_;
};

} // namespace module::claude
