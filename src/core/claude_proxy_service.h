// src/core/claude_proxy_service.h
// Claude 代理「运行面」窄接口（任务 D / R0，审计 D6 跨模块耦合消解）
//
// 设置页要读 / 动 Claude 代理的运行时状态（代理是否在跑、实际端口、接管状态、
// 上层覆盖警告、改端口时的 stop→start），但 module_settings 不能 include
// module_claude_proxy 的任何头（module_* 互不依赖）。这里定义**消费方真正
// 用到的那几个方法**作为接口，提供方经 core::ServiceLocator 注册实现：
//   生产 = main.cpp 注册的转发适配器（转发到它持有的 ClaudeService 单例，
//          见 GlobalClaudeProxyService<T>）；
//   测试 = 各测试文件把自己 setGlobal 的 service 用同一个适配器注册。
// 接口不收「将来可能用到」的口子——加口子前先问消费方是否真的要用。
//
// 与单例的关系（R0 边界，不扩大）：本接口不改变 ClaudeService 的所有权，
// main.cpp 仍是唯一创建 / 持有方；claude_module.cpp 内部直呼 global() 属
// **同模块内**读取，不在 R0 范围（审计 V6：只治 settings 这 3 处跨模块引用）。
#pragma once

#include "core/result.h"

#include <optional>
#include <string>
#include <vector>

namespace core {

class IClaudeProxyService {
public:
    virtual ~IClaudeProxyService() = default;

    // ---- 代理 server 状态（设置页代理卡 / 端口应用）----
    virtual bool isProxyRunning() const = 0;
    // 实际监听端口；未运行 = 0（与 ClaudeService::proxyPort 同口径）
    virtual int proxyPort() const = 0;
    // 写运行期端口镜像（代理手动启动 / 自启用它，而不是硬编码默认端口）
    virtual void setConfiguredProxyPort(int port) = 0;
    virtual void stopProxy() = 0;
    // 启动本地代理（bindAddress 由调用方定；成功 = 实际绑定的端口）
    virtual core::Result<int> startProxy(const std::string& bindAddress,
                                         int port) = 0;

    // ---- Claude Code settings 接管（设置页接管状态行 / 覆盖警告）----
    virtual bool isClaudeCodeManaged() const = 0;
    virtual const std::vector<std::string>& lastSyncWarnings() const = 0;
    // 接管实际使用的 settings.json 路径（配置通道未注入 = 空串）。设置页只用
    // 它做展示文本，故直接给 string，不把 filesystem::path 摆进接口
    virtual std::string settingsFilePath() const = 0;

    // ---- Agent 模型槽位解析（agent 子系统 M1；LlmClient 消费）----
    // 返回激活 profile 的 models_config 里 slot 槽位的实际 model id。
    // "def" 与 "default" 等价；default 槽位未配置时回落 profile.model（旧列
    // 语义）。无激活 profile / 槽位未配置 / model 空 → nullopt。
    // non-const：与 getActiveProfile 链保持同一形态
    virtual std::optional<std::string> resolveModelSlot(const std::string& slot) = 0;
};

// 适配器：把「进程内单例（有 static T* global()）」适配成上面的接口。
//   - 生产实例化点 = main.cpp（T = module::claude::ClaudeService）
//   - 测试实例化点 = test_settings.cpp（T 同上，把自己 setGlobal 的 service
//     交给同一个适配器，生产 / 测试走同一条转发路径）
// core 不 include 任何 module_* 头——T 由实例化点提供（模板体只在实例化时
// 才需要 T 完整），模块边零新增。
//
// 空单例语义：global() 返回 nullptr（claude 模块关闭 / 服务未装配）时各方法
// 按「服务不可用」返回 false / 0 / 空串 / 空 vector / no-op——与消费方原先
// `auto* svc = ClaudeService::global(); if (svc) { ... }` 的降级结果逐字等价
//（R0「零行为变化」的落点：把判空口径收进这一处）。
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

    const std::vector<std::string>& lastSyncWarnings() const override {
        T* svc = T::global();
        if (svc != nullptr) {
            return svc->lastSyncWarnings();
        }
        static const std::vector<std::string> kNoWarnings;
        return kNoWarnings;
    }

    std::string settingsFilePath() const override {
        T* svc = T::global();
        if (svc == nullptr) {
            return std::string();
        }
        return svc->settingsSyncPaths().settingsFile.string();
    }

    std::optional<std::string> resolveModelSlot(const std::string& slot) override {
        T* svc = T::global();
        if (svc == nullptr) return std::nullopt;
        return svc->resolveModelSlot(slot);
    }
};

} // namespace core
