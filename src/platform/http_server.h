// src/platform/http_server.h
// 基于 cpp-httplib 的本地 HTTP 服务器（详见 spec §7.2）
#pragma once

#include <functional>
#include <map>
#include <string>

namespace platform::http {

struct RequestCtx {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string clientAddress;
    bool authenticated = false;
};

class ResponseWriter {
public:
    void setStatus(int code);
    void setHeader(const std::string& key, const std::string& value);
    void write(const std::string& data);
    void end();

private:
    int status_ = 200;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool ended_ = false;
    friend class Server;
};

using RouteHandler = std::function<void(const RequestCtx&, ResponseWriter&)>;

class Server {
public:
    Server(std::string bindAddress, int port, std::string authToken = "");
    ~Server();

    void route(const std::string& method, const std::string& path, RouteHandler handler);

    bool start();
    void stop();

    bool isRunning() const { return running_; }
    int actualPort() const { return port_; }
    bool requiresAuth() const { return !authToken_.empty(); }
    const std::string& authToken() const { return authToken_; }

private:
    struct Impl;
    std::string bindAddress_;
    int port_;
    std::string authToken_;
    bool running_ = false;
    Impl* impl_ = nullptr;
};

} // namespace platform::http