// hot.list 工具：四源串行降级 + 进程内缓存/限频（无 UI、无注册）。
//
// 源链：微博 side/hotSearch → 微博 hot_band → 百度 PC → 今日头条；
// 首个成功即返回，不合并多源，失败只记录 attempts 后切下一源（5xx 不额外重试）。
// 缓存与限频都是**单实例**状态（std::mutex 保护）：fresh 300s、最小请求间隔 60s、
// 全部失败时可退回 1800s 内的 stale 缓存。
//
// 线程契约：execute() 在 AgentLoop 工作线程执行；fetcher/now 由构造注入，单测使用
// fake 实现，绝不联网。默认实现走 platform::http 与 core::time::nowUnix()。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/tool.h"
#include "module_agent/hot_parse.h"

namespace module::agent {

struct HotFetchRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    int timeoutMs = 8000;
};

struct HotFetchResponse {
    int statusCode = 0;
    std::string body;
    std::string networkError;  // 非空 = 网络层失败
};

using HotFetcher = std::function<HotFetchResponse(const HotFetchRequest&)>;
using NowFn = std::function<int64_t()>;

// 生产实现：platform::http 同步 GET（不 follow 重定向、不解压）与系统时钟。
HotFetchResponse defaultHotFetcher(const HotFetchRequest& request);
int64_t defaultNow();

class HotListTool final : public core::ITool {
public:
    HotListTool(HotFetcher fetcher = defaultHotFetcher, NowFn now = defaultNow);

    std::string name() const override;
    std::string schemaJson() const override;
    core::Result<core::ToolResult> execute(const core::ToolArgs& args) override;

private:
    struct CacheEntry {
        HotSource source = HotSource::Weibo;
        int64_t fetchedAtEpoch = 0;
        std::vector<HotItem> items;  // 已截取 top10
        bool degraded = false;
    };

    HotFetcher fetcher_;
    NowFn now_;
    // 单锁覆盖「缓存检查 → 限频检查 → 整条源链」；同实例并发调用被串行化，
    // 后到者在网络结束后会命中 fresh cache，而不是再打一轮源链。
    std::mutex mutex_;
    std::optional<CacheEntry> cache_;
    std::optional<int64_t> lastAttemptAt_;
};

} // namespace module::agent
