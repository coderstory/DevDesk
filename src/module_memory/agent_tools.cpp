// memory.stats / memory.health 实现：全部只读取数，零新存储。
// proposals/ledger 经各自 store 的 open 自建 schema（面板未开过页也成立）；
// 探活复用引擎管线同款 probe（telemem_engine 导出的 probeEngineHealthy）。
#include "agent_tools.h"

#include "core/time.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/engine/telemem_engine.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"
#include "platform/json.h"

#include <string>
#include <utility>

namespace module::memory {
namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

Result<ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "工具错误消息序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, text.value()});
}

const char* engineStateName(engine::TelememEngine::State s) {
    switch (s) {
        case engine::TelememEngine::State::Idle: return "Idle";
        case engine::TelememEngine::State::Preparing: return "Preparing";
        case engine::TelememEngine::State::Running: return "Running";
        case engine::TelememEngine::State::Stopped: return "Stopped";
        case engine::TelememEngine::State::Failed: return "Failed";
    }
    return "Unknown";
}

const char* embedStateName(engine::TelememEngine::EmbedState s) {
    switch (s) {
        case engine::TelememEngine::EmbedState::Unknown: return "Unknown";
        case engine::TelememEngine::EmbedState::NeedDownload: return "NeedDownload";
        case engine::TelememEngine::EmbedState::Downloading: return "Downloading";
        case engine::TelememEngine::EmbedState::Ready: return "Ready";
        case engine::TelememEngine::EmbedState::Failed: return "Failed";
    }
    return "Unknown";
}

// F-03：提案分组计数 + 用量账面 + 引擎状态一行
class MemoryStatsTool : public core::ITool {
public:
    MemoryStatsTool(std::filesystem::path proposalsDb,
                    std::filesystem::path ledgerDb)
        : proposalsDb_(std::move(proposalsDb)),
          ledgerDb_(std::move(ledgerDb)) {}

    std::string name() const override { return "memory.stats"; }

    std::string schemaJson() const override {
        return R"({"name":"memory.stats","description":"记忆模块统计（提案分组计数 / token 用量账面 / 引擎状态一行），只读，不依赖引擎进程存活","input_schema":{"type":"object","properties":{}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;  // 无参数
        const auto now = static_cast<int64_t>(core::time::nowUnix());

        auto prop = proposals::ProposalStore::open(proposalsDb_);
        if (prop.isFail()) {
            return toolError("提案库打开失败: " + prop.errorMessage());
        }
        auto byStatus = prop.value().countByStatus();
        if (byStatus.isFail()) {
            return toolError("提案统计查询失败: " + byStatus.errorMessage());
        }
        pj::JsonObject propObj;
        int64_t total = 0;
        for (const auto& [status, count] : byStatus.value()) {
            propObj.set(status.c_str(), pj::makeInt(count));
            total += count;
        }
        propObj.set("total", pj::makeInt(total));

        auto led = proposals::UsageLedger::open(ledgerDb_);
        if (led.isFail()) {
            return toolError("账本打开失败: " + led.errorMessage());
        }
        auto todayTokens = led.value().todayTokens(now);
        if (todayTokens.isFail()) {
            return toolError("账本查询失败: " + todayTokens.errorMessage());
        }
        auto monthTokens = led.value().monthTokens(now);
        if (monthTokens.isFail()) {
            return toolError("账本查询失败: " + monthTokens.errorMessage());
        }
        auto todayCost = led.value().todayCostMilliYuan(now);
        if (todayCost.isFail()) {
            return toolError("账本查询失败: " + todayCost.errorMessage());
        }
        auto monthCost = led.value().monthCostMilliYuan(now);
        if (monthCost.isFail()) {
            return toolError("账本查询失败: " + monthCost.errorMessage());
        }
        pj::JsonObject usage;
        usage.set("todayTokens", pj::makeInt(todayTokens.value()));
        usage.set("monthTokens", pj::makeInt(monthTokens.value()));
        usage.set("todayCostMilliYuan", pj::makeInt(todayCost.value()));
        usage.set("monthCostMilliYuan", pj::makeInt(monthCost.value()));

        auto snap = engine::TelememEngine::global().snapshot();
        pj::JsonObject eng;
        eng.set("state", pj::makeString(engineStateName(snap.state)));
        eng.set("embedState", pj::makeString(embedStateName(snap.embedState)));
        eng.set("pythonFound", pj::makeBool(snap.pythonFound));

        pj::JsonObject o;
        o.set("proposals", pj::JsonValue(std::move(propObj)));
        o.set("usage", pj::JsonValue(std::move(usage)));
        o.set("engine", pj::JsonValue(std::move(eng)));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                            "统计结果序列化失败: " +
                                                text.errorMessage());
        }
        return Result<ToolResult>::ok(ToolResult{false, text.value()});
    }

private:
    std::filesystem::path proposalsDb_;
    std::filesystem::path ledgerDb_;
};

// F-04：引擎快照 + 回环探活（探活结果是数据，isError 恒 false）
class MemoryHealthTool : public core::ITool {
public:
    std::string name() const override { return "memory.health"; }

    std::string schemaJson() const override {
        return R"({"name":"memory.health","description":"记忆引擎健康检测（运行状态 / venv 与 Python 定位 / 8421 回环探活），只读","input_schema":{"type":"object","properties":{}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;  // 无参数
        auto snap = engine::TelememEngine::global().snapshot();
        std::string reason;
        const bool probeOk = engine::probeEngineHealthy(&reason);
        pj::JsonObject probe;
        probe.set("ok", pj::makeBool(probeOk));
        probe.set("reason", pj::makeString(probeOk ? "" : reason));

        pj::JsonObject o;
        o.set("state", pj::makeString(engineStateName(snap.state)));
        o.set("detail", pj::makeString(snap.detail));
        o.set("pythonFound", pj::makeBool(snap.pythonFound));
        o.set("pythonVersion", pj::makeString(snap.pythonVersion));
        o.set("embedState", pj::makeString(embedStateName(snap.embedState)));
        o.set("embedReady", pj::makeBool(snap.embedReady));
        o.set("probe", pj::JsonValue(std::move(probe)));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                            "健康结果序列化失败: " +
                                                text.errorMessage());
        }
        return Result<ToolResult>::ok(ToolResult{false, text.value()});
    }
};

} // namespace

std::shared_ptr<core::ITool> makeMemoryStatsTool(
    const std::filesystem::path& proposalsDb,
    const std::filesystem::path& ledgerDb) {
    return std::make_shared<MemoryStatsTool>(proposalsDb, ledgerDb);
}

std::shared_ptr<core::ITool> makeMemoryHealthTool() {
    return std::make_shared<MemoryHealthTool>();
}

} // namespace module::memory
