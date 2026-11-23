// 网页终端路由表（need#490 W3，技术卡 §4）。
#pragma once

namespace platform::http {
class Server;
} // namespace platform::http

namespace module::terminal::web {

class WebTerminalService;

void registerWebRoutes(platform::http::Server& server, WebTerminalService& service);

} // namespace module::terminal::web
