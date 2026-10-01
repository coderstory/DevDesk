// src/module_agent/agent_module.h
// 智能体页模块（agent M1 Task 12）：生命周期实体 + 手动触发 + 最小 UI。
// Task 11 的装配面（PageId::Agent / displayName / iconGlyph / 工厂签名）保持
// 不变；本文件填实 onStart/onStop 的服务装配与拆解、run 线程管理、compose。
//
// 线程模型（spec §10）：
//   - onStart/onStop/startTask/compose 全部在主线程（装配段 / UI 帧）；
//   - 每次「启动」起一个 run 工作线程，跑同步的 AgentLoop::run（落库在
//     run 内完成）；线程只触碰自己的 ActiveRun::cancel/done 两个 atomic 与
//     AgentStore（SqliteDb 内部 recursive_mutex 串行化），不摸模块成员；
//   - onStop 顺序是结构契约（spec §3，勿改）：全取消 → 全 join → 清容器 →
//     注销工具 → 放服务。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/module_interface.h"
#include "core/tool.h"
#include "module_agent/agent_store.h"
#include "module_agent/llm_client.h"

namespace core {
class IClaudeProxyService;
}

namespace module::agent {

class AgentLoop;

class AgentModule : public ::core::Module {
public:
    // 依赖注入：测试直接构造传 fake；生产工厂传空 Deps，onStart 内回退
    // core::appDatabase() / core::ServiceLocator 解析（照 todo/settings 既有
    // 渠道，main.cpp 装配段零改动）
    struct Deps {
        core::data::SqliteDb* db = nullptr;
        core::IClaudeProxyService* proxy = nullptr;
        // LlmClient 的 HTTP 缝（nullptr = 生产路径 platform::http::send；
        // 测试注入假发送，零真实网络）
        LlmClient::HttpFn http;
    };
    explicit AgentModule(Deps d);
    // 兜底拆解：漏调 onStop（异常路径 / 用例 fail-fast）时先收 run 线程再
    // 析构，不给进程留可 join 的悬垂线程；对已正常 onStop 的实例是 no-op
    ~AgentModule() override;

    ::core::PageId pageId() const override { return ::core::PageId::Agent; }
    std::string displayName() const override { return "智能体"; }
    std::string shortName() const override { return "智能体"; }
    std::string iconGlyph() const override;   // eui::utf8(0xF544)，cpp 内实现
    void compose(::eui::Ui& ui, const ::eui::Screen& screen) override;
    void onPageEnter() override;
    void onStart() override;
    void onStop() override;

private:
    // 一次手动触发的 run：cancel 由 onStop/后续机制置位，done 由 run 线程
    // 收尾置位（startTask 收割已结束 run 的依据，保证「同 task 不并发」
    // 判定不误伤已完成的 run）
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
    };

    void startTask(int64_t taskId);
    void createTaskFromForm();
    void toggleExpand(int64_t taskId);
    void refreshData();
    void reapFinishedRuns();

    Deps deps_;
    std::unique_ptr<AgentStore> store_;
    std::unique_ptr<LlmClient> llm_;
    std::unique_ptr<AgentLoop> loop_;
    std::shared_ptr<::core::ITool> statsTool_;

    // run 线程表：主线程写（startTask/onStop），mutex 照 brief 骨架保留
    //（现全部调用点同在主线程，属纵深防御，不为省锁去改契约）
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;

    // compose 数据缓存（仅主线程读写；run 线程只经 SQLite 触库，不碰这里）
    std::vector<AgentTaskRow> tasks_;
    std::map<int64_t, AgentRunRow> latestRuns_;     // taskId → 最近一次 run
    std::optional<int64_t> expandedTaskId_;         // 展开详情的任务（单开）
    std::vector<AgentMessageRow> detailMessages_;   // 展开 task 最近 run 的消息
    std::string goalInput_;
    std::string formError_;
    bool tasksDirty_ = false;   // onStart/onPageEnter/写操作置位 → 下帧重查
    bool uiPolling_ = false;    // 有 run 期间每帧跟进状态/消息，全终止后停

    // 重入幂等守卫（活门控 Task 4）：double-onStart = no-op——防 store_/
    // statsTool_ 重建后 registerTool 撞同名旧指针 fail（被日志吞）、注册表留
    // 旧工具而成员已换新；double-onStop / 未启动的 off = no-op
    bool started_ = false;
};

} // namespace module::agent
