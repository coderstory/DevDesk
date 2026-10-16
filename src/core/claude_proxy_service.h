// Claude 代理「运行面」窄接口：module_settings 不能 include module_claude_proxy 的头，故定义
// 消费方真正用到的几个方法，提供方经 ServiceLocator 注册（生产 = main.cpp 的转发适配器）。
// 本接口不改变 ClaudeService 所有权，main.cpp 仍是唯一创建/持有方
#pragma once

#include "core/result.h"

#include <optional>
#include <string>
#include <vector>

namespace core {

class IClaudeProxyService {
public:
    virtual ~IClaudeProxyService() = default;

    virtual bool isProxyRunning() const = 0;
    // 实际监听端口；未运行 = 0（与 ClaudeService::proxyPort 同口径）
    virtual int proxyPort() const = 0;
    // 写运行期端口镜像（代理手动启动 / 自启用它，而不是硬编码默认端口）
    virtual void setConfiguredProxyPort(int port) = 0;
    virtual void stopProxy() = 0;
    // 启动本地代理（bindAddress 由调用方定；成功 = 实际绑定的端口）
    virtual core::Result<int> startProxy(const std::string& bindAddress,
                                         int port) = 0;

    virtual bool isClaudeCodeManaged() const = 0;
    // 按值快照——实现侧可能被 agent 线程改写
    virtual std::vector<std::string> lastSyncWarnings() const = 0;
    // 接管实际使用的 settings.json 路径（未注入 = 空串）；给 string 不把 filesystem::path 摆进接口
    virtual std::string settingsFilePath() const = 0;

};

// 适配器：进程内单例（static T* global()）→ 上面的接口；core 不 include module_* 头，T 由实例化点提供。
// 空单例语义：global() == nullptr 时各方法按服务不可用返回 false/0/空串/空 vector/no-op
template <typename T>
class GlobalClaudeProxyService final : public IClaudeProxyService {
public:
    bool isProxyRunning() const override {
        T* svc = T::global();
        return svc != nullptr && svc->isProxyRunning();
    }

    int proxyPort() const override {
        T* svc = T::global();
        return svc != nullptr ? svc->proxyPort() : 0;
    }

    void setConfiguredProxyPort(int port) override {
        if (T* svc = T::global()) {
            svc->setConfiguredProxyPort(port);
        }
    }

    void stopProxy() override {
        if (T* svc = T::global()) {
            svc->stopProxy();
        }
    }

    core::Result<int> startProxy(const std::string& bindAddress,
                                 int port) override {
        T* svc = T::global();
        if (svc == nullptr) {
            // 不可达（消费方先判 isProxyRunning 才会走到这里），仅作防御
            return core::Result<int>::fail(core::ErrorCode::NotImplemented,
                                           "ClaudeService 未装配（模块关闭？）");
        }
        return svc->startProxy(bindAddress, port);
    }

    bool isClaudeCodeManaged() const override {
        T* svc = T::global();
        return svc != nullptr && svc->isClaudeCodeManaged();
    }

    std::vector<std::string> lastSyncWarnings() const override {
        T* svc = T::global();
        if (svc != nullptr) {
            return svc->lastSyncWarnings();
        }
        return {};
    }

    std::string settingsFilePath() const override {
        T* svc = T::global();
        if (svc == nullptr) {
            return std::string();
        }
        return svc->settingsSyncPaths().settingsFile.string();
    }

};

} // namespace core
