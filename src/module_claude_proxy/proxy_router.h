// src/module_claude_proxy/proxy_router.h
// 本地 Claude 代理路由（spec §10.4）
//
// 流程（每次请求）：
//   1. 客户端 POST http://127.0.0.1:<port>/<path>  + Authorization: Bearer <token>
//   2. http_server 已用 pre_routing_handler 校验 token（401 拦截）
//   3. ProxyRouter::route 接收请求：
//      - body 里 "stream": true 且路径可流式 → 走 SSE 流式转发（P10a）
//      - 否则 → 缓冲转发（P9）
//
// 两条路径的公共部分：
//      - 查 active profile（无 → 503 Service Unavailable）
//      - 读 API key（来自本行凭据列）
//      - 用 platform::http 转发到 base_url + path
//      - 写回上游响应（含 status / headers / body）
//      - 写入一条 claude_call_logs
//
// 流式路径（spec §10.4）：
//   - platform::http::sendStreaming 在独立线程跑 libcurl（生产者）
//   - cpp-httplib 的 chunked content provider 从队列拉（消费者）
//   - 队列 >= 100 暂停上游接收，< 50 恢复（backpressure）
//   - 客户端断开 → cancel 上游 libcurl
#pragma once

#include <atomic>
#include <functional>
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

    // 流式转发分支（spec §10.4）：SSE chunk 透传 + backpressure + 取消
    void handleStreamingRequest(const std::string& method,
                                const std::string& path,
                                const std::map<std::string, std::string>& headers,
                                const std::string& body,
                                platform::http::ResponseWriter& out);

    // 是否走流式路径：路径可流式 且 body 中 "stream": true
    static bool shouldUseStreaming(const std::string& path,
                                   const std::string& body);

    // 注入 profile 提供者（默认走 service_.getActiveProfile() + 凭据列）。
    // 测试可注入固定 Profile 以绕过 DB。
    using ProfileProvider = std::function<core::Result<Profile>()>;
    void setProfileProvider(ProfileProvider provider) {
        profileProvider_ = std::move(provider);
    }

private:
    ClaudeService& service_;
    platform::http::Server& server_;
    ProfileProvider profileProvider_;   // 为空时走 service_ 默认逻辑

    // 取当前生效的 profile（含解密后的 apiKeyPlain）
    core::Result<Profile> resolveProfile();
};

} // namespace module::claude

