// src/platform/http_server_sockopt_posix.cpp
// 服务端 socket 绑定选项（POSIX 实现）：SO_REUSEADDR。
// 语义取舍见 http_server.cpp 的 applyExclusiveServerSocketOptions 注释：REUSEADDR
// 只豁免已死连接的端口残留（TIME_WAIT / 关闭尾部），两个活动 LISTEN 仍互相排他。
#include <httplib.h>

namespace platform::http {

void applyExclusiveServerSocketOptions(socket_t sock) {
    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

} // namespace platform::http
