#include "http_server.h"
#include <core/logging.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace platform::http {

void ResponseWriter::setStatus(int code) { status_ = code; }
void ResponseWriter::setHeader(const std::string& key, const std::string& value) { headers_[key] = value; }
void ResponseWriter::write(const std::string& data) { body_ += data; }
void ResponseWriter::end() { ended_ = true; }

struct Server::Impl {
    std::map<std::string, RouteHandler> handlers;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
};

Server::Server(std::string bindAddress, int port, std::string authToken)
    : bindAddress_(std::move(bindAddress)),
      port_(port),
      authToken_(std::move(authToken)),
      impl_(new Impl()) {}

Server::~Server() {
    stop();
    delete impl_;
}

void Server::route(const std::string& method, const std::string& path, RouteHandler handler) {
    impl_->handlers[method + " " + path] = std::move(handler);
}

bool Server::start() {
    if (running_) return true;
    LOG_INFO("HTTP server started: " + bindAddress_ + ":" + std::to_string(port_) +
            (authToken_.empty() ? "" : " (with auth)"));
    running_ = true;
    return true;
}

void Server::stop() {
    if (!running_) return;
    impl_->stopRequested = true;
    if (impl_->worker.joinable()) impl_->worker.join();
    running_ = false;
    LOG_INFO("HTTP server stopped");
}

} // namespace platform::http