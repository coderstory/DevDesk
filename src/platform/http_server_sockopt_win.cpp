// src/platform/http_server_sockopt_win.cpp
// 服务端 socket 绑定选项（Windows 实现）：SO_EXCLUSIVEADDRUSE。
// 双平台语义取舍与 TIME_WAIT 已知取舍见 http_server.cpp 的
// applyExclusiveServerSocketOptions 注释（文档：docs/superpowers/research/
// 2026-10-27-mcp-round2-review.md §二）。
#include <httplib.h>

namespace platform::http {

void applyExclusiveServerSocketOptions(socket_t sock) {
    BOOL exclusive = TRUE;
    ::setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
}

} // namespace platform::http
