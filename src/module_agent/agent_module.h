// src/module_agent/agent_module.h
// 智能体页模块：生命周期实体 + 手动触发 + 页面骨架。
// 线程模型：onStart/onStop/startTask/compose 全在主线程；每次「启动」起一个 run
// 工作线程跑同步 AgentLoop::run，只触碰自己的 cancel/done atomic 与 AgentStore，
// 不摸模块成员。onStop 顺序是结构契约（勿改）：全取消 → 全 join → 清容器 →
// 注销工具 → 放服务。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/module_interface.h"
#include "core/theme.h"
#include "core/tool.h"
#include "module_agent/agent_store.h"
#include "module_agent/llm_client.h"
#include "module_agent/scheduler.h"
#include "module_agent/tool_info.h"

#include <components/theme.h>

namespace core {
class IClaudeProxyService;
}

namespace module::agent {

class AgentLoop;

// UI 轮询判停的唯一判定。收尾 finishRun 写库失败时 DB 行永停 'running'（线程早已
// 结束、行再也等不到改写）；若当作「还有 run 在跑」，uiPolling_ 永不复位 →
// 此后每帧一次 refreshData() 直到重启。孤儿 runId 必须排除——比对的是
// AgentRunRow::id（runId），不是 map 键（taskId）。抽成自由函数便于单测钉死
bool hasLiveRun(const std::map<int64_t, AgentRunRow>& latestRuns,
                const std::set<int64_t>& orphanedRunIds);

class AgentModule : public ::core::Module {
public:
    // 依赖注入：测试传 fake；生产工厂传空 Deps，onStart 内回退 appDatabase / ServiceLocator
    struct Deps {
        core::data::SqliteDb* db = nullptr;
        core::IClaudeProxyService* proxy = nullptr;
        // HTTP 缝（nullptr = 生产 platform::http::send；测试注入假发送）
        LlmClient::HttpFn http;
        // unix 秒时钟缝（nullptr = 生产真实系统时间；测试注入假时钟驱动 cron 判定）
        Scheduler::NowFn schedulerNow;
    };
    explicit AgentModule(Deps d);
    // 兜底拆解：漏调 onStop 时先收 run 线程再析构，不留悬垂线程；正常路径为 no-op
    ~AgentModule() override;

    ::core::PageId pageId() const override { return ::core::PageId::Agent; }
    std::string displayName() const override { return "智能体"; }
    std::string shortName() const override { return "智能体"; }
    std::string iconGlyph() const override;   // eui::utf8(0xF544)，cpp 内实现
    void compose(::eui::Ui& ui, const ::eui::Screen& screen) override;
    void onPageEnter() override;
    void onStart() override;
    void onStop() override;
    // Esc 关最上层弹窗；键走得到本通道 = 焦点元素没消费（输入框聚焦时已被 onCancel 收走）
    bool onKeyEvent(const ::eui::KeyEvent& event) override;

    // 测试缝（用例直调，等价于用户在表单里完成动作）：输入框 onTextInput 只能
    // 追加、无法覆盖草稿（赋值把光标置 0），故草稿写入口落在这里；下拉/开关无需缝
    void setEditCronDraft(const std::string& cron);
    void setDialogIterDraft(int iter);

private:
    // 一次 run：cancel 由 onStop 置位，done 由 run 线程收尾置位（reap 收割依据）
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
    };

    // 惰性装配单入口：首次使用时解析 IClaudeProxyService 并建 llm_/loop_/scheduler_；幂等
    void ensureLlm();
    void startTask(int64_t taskId);
    // 任务弹窗（新建/编辑共用表单）：新建态 editingTaskId_ 空、字段取默认值，
    // 编辑态从 getTask 回填。cron 非法 → 弹窗内报错、不落库、不关窗
    void openNewTaskDialog();
    void openTaskEditor(int64_t taskId);
    void closeTaskDialog();
    void saveTaskDialog();
    void toggleTaskEnabled(const AgentTaskRow& task);
    // 详情弹窗
    void openTaskDetail(int64_t taskId);
    void closeTaskDetail();
    void refreshData();
    void reapFinishedRuns();
    // 单条消息展开/收起（seq 定位，切 run 即清）
    void toggleMessageExpanded(int64_t seq);
    // 默认展开态：最近一条带 text 的 assistant 消息；目标 seq 变化才动展开集，
    // 用户手动收起后不被每帧刷新顶回来
    void applyDefaultMessageExpansion();
    // 「可用工具」注册表快照：惰性装载，每次展开强制重装（反映模块启停变化）
    void ensureToolsCache(bool forceReload);
    // 单条消息行渲染：徽章头行 + 收起预览 / 展开块级全文（prefix = 宿主节点前缀）
    void renderMessageRow(::eui::Ui& inner, float cw, const std::string& prefix,
                          size_t index, const AgentMessageRow& msg,
                          const ::core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& tokens);
    // 两张弹窗的绘制（关闭态不建内容节点）
    void composeTaskDialog(::eui::Ui& ui, const ::eui::Screen& screen,
                           const ::core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens);
    void composeTaskDetailDialog(
        ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
        const components::theme::ThemeColorTokens& themeTokens);

    Deps deps_;
    std::unique_ptr<AgentStore> store_;
    std::unique_ptr<LlmClient> llm_;
    std::unique_ptr<AgentLoop> loop_;
    // 声明序契约：scheduler_ 必须在 loop_/llm_/store_ 之后——逆序析构下 Scheduler
    // 先亡，其 run 线程 join 期间被引用的 loop_/store_ 仍存活
    std::unique_ptr<Scheduler> scheduler_;
    std::shared_ptr<::core::ITool> statsTool_;

    // run 线程表：主线程写；mutex 为纵深防御（现调用点全在主线程）
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;
    // 收尾写库失败的 runId（FinishFailureFn 回报，run 线程写）；判停时必须排除，
    // 否则 uiPolling_ 永不复位。与 runs_ 同用 runsMu_
    std::set<int64_t> orphanedRunIds_;

    // compose 数据缓存（仅主线程读写）
    std::vector<AgentTaskRow> tasks_;
    std::map<int64_t, AgentRunRow> latestRuns_;     // taskId → 最近一次 run
    // 详情弹窗 run 列表（每任务最近 5 次，refreshData 装载）
    std::map<int64_t, std::vector<AgentRunRow>> taskRuns_;
    std::optional<int64_t> detailTaskId_;           // 非空 = 弹窗打开
    std::optional<AgentTaskRow> detailTaskRow_;     // 配置摘要快照（getTask）
    std::optional<int64_t> selectedRunId_;          // 详情弹窗选中的 run（默认最近一次）
    std::vector<AgentMessageRow> detailMessages_;   // 选中 run 的消息
    // 展开全文的消息（按 seq 定位，切任务/切 run 清空）
    std::set<int64_t> expandedMsgSeqs_;
    // 自动展开那条的 seq：目标变化才重设展开集，用户手动收起后不被刷新顶回来
    std::optional<int64_t> autoExpandedSeq_;
    // scrollView contentKey 版本号：展开/收起改变内容高，bump 触发重测（MeasureCache 按 key 缓存）
    uint64_t msgScrollVersion_ = 0;
    // 「可用工具」区块：收起默认，头部点击展开
    bool toolsOpen_ = false;
    std::vector<ToolInfo> toolsCache_;      // 注册表快照（惰性装载）
    bool toolsCacheLoaded_ = false;
    // 任务弹窗：taskDialogOpen_ 控开关，editingTaskId_ 非空 = 编辑态，字段两态共用
    bool taskDialogOpen_ = false;
    std::optional<int64_t> editingTaskId_;
    std::string editGoal_, editCron_, editSlot_, editError_;
    int editTriggerIdx_ = 0;      // 0=手动 1=定时 2=常驻（kTriggerTypes 下标）
    int editIter_ = 10;
    bool editDestructive_ = false;
    bool editTriggerOpen_ = false;   // 触发类型下拉开合（UI 态）
    bool editSlotOpen_ = false;      // 模型槽位下拉弹层开合
    // 页内错误行：弹窗之外动作失败时的唯一可见出口（弹窗内失败走 editError_）；
    // 下一次成功动作清
    std::string pageError_;
    bool tasksDirty_ = false;   // onStart/onPageEnter/写操作置位 → 下帧重查
    bool uiPolling_ = false;    // 有 run 期间每帧跟进状态/消息，全终止后停

    // 重入幂等守卫：double-onStart = no-op（防重复注册工具 / 重建服务）；未启动的 off = no-op
    bool started_ = false;
    // 「无代理服务」WARN 去重（惰性解析会从每帧 compose 触发）
    bool llmMissingWarned_ = false;
};

} // namespace module::agent
