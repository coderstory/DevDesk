// 智能体页模块：生命周期实体 + 手动触发 + 页面骨架。
// 线程模型：onStart/onStop/startTask/compose 全在主线程；run 工作线程只触碰自己的
// cancel/done atomic 与 AgentStore。onStop 顺序是结构契约（勿改）：全取消 → 全 join → 清容器 → 注销工具 → 放服务。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/tool.h"
#include "module_agent/agent_store.h"
#include "module_agent/direct_llm_client.h"
#include "module_agent/scheduler.h"
#include "module_agent/tool_info.h"

#include <components/theme.h>

namespace core {
class LlmProfileSource;
}

namespace module::agent {

class AgentLoop;

// UI 轮询判停唯一判定：finishRun 写库失败 → DB 行永停 'running'，若当「还有 run 在跑」
// 则 uiPolling_ 永不复位。孤儿 runId 须按 AgentRunRow::id（runId）排除，非 map 键 taskId
bool hasLiveRun(const std::map<int64_t, AgentRunRow>& latestRuns,
                const std::set<int64_t>& orphanedRunIds);

// 错误行「去 Claude 页」引导的判定：仅「配置不可用」类错误为 true——
// 前缀 = direct_llm_client.cpp 的 kConfigErrorPrefix（无激活 profile / 未配 Base URL /
// 未配 API Key / 槽位未配置，四者都由该客户端产出），按前缀匹配。
// 其余错误（网络 / 上游 4xx / 落库 / 读库）一律 false。前缀刻意钉死：文案一变，
// 测试（test_agent_ui.cpp predicate 用例）即红，防文案漂移后引导静默失效。
bool needsClaudePageHint(const std::string& errorText);

// run 完成通知的迁移检测（纯逻辑，refreshData 每次轮询消费）：
// prev/now = 上次/本次轮询的 runId→status 快照；notified = 已通知过的 runId。
// 返回需通知的 runId（升序）：仅「prev=running 且 now=failed/limited 且未通知过」；
// completed/cancelled 静默；prev 缺席（未观测过 running，如 run 结束后才进页）不算迁移不通知。
std::vector<int64_t> detectRunEndNotifications(
    const std::map<int64_t, std::string>& prevStatus,
    const std::map<int64_t, std::string>& nowStatus,
    const std::set<int64_t>& notifiedRunIds);

// 时长秒 → 紧凑中文（0秒 / 45秒 / 2分13秒 / 1时23分 / 2天3时；负值按 0，与 runRowText 同口径）
std::string formatRunDurationText(int64_t seconds);

// 本地日历时刻 → 「yyyy-MM-dd HH:mm」。入参取 core::time::localTime 产物，测试传固定
// tm 保时区无关
std::string formatRunResultTimeText(const std::tm& local);

// 详情弹窗汇总行文案：「共 N 次 · 成功率 P%（ok/N）· 总耗时 <时长> · 最近结果 <时间>」。
// lastTimeText 由调用方生成（时间依赖本地时区）：有 lastEndedAt 传 formatRunResultTimeText
// 产物，无则传「—」；totalRuns=0 时百分比按 0（正常不走到——无 run 的任务不渲染汇总行）
std::string formatTaskRunSummaryText(const AgentTaskRunSummary& summary,
                                     const std::string& lastTimeText);

// 任务弹窗定时分支的常用 cron 模板表：文案与表达式成对，选中即回填 editCron_。
// 表内容是产品文案不是代码结构，加条目只改这里；表达式必须能过 parseCron。
struct CronTemplate {
    const char* label;
    const char* expr;
};
inline constexpr CronTemplate kCronTemplates[] = {
    {"工作日 9 点", "0 9 * * 1-5"},
    {"每天 9 点", "0 9 * * *"},
    {"每小时", "0 * * * *"},
    {"每周一 9 点", "0 9 * * 1"},
};
inline constexpr int kCronTemplateCount =
    static_cast<int>(sizeof(kCronTemplates) / sizeof(kCronTemplates[0]));

// 冷启动兜底轮询间隔（秒）：pollRunEnd 三守卫全假时轻查询的节流窗口——外部直写库
// 插入 running run 后，观测恢复的最大延迟就等于这个值
inline constexpr int64_t kRunFallbackIntervalSec = 15;

class AgentModule : public ::core::Module {
public:
    // 依赖注入：测试传 fake；生产工厂传空 Deps，onStart 内回退 appDatabase / ServiceLocator
    struct Deps {
        core::data::SqliteDb* db = nullptr;
        // 直连配置来源（nullptr = onStart/首次使用时经 ServiceLocator 解析）
        core::LlmProfileSource* profileSource = nullptr;
        // HTTP 缝（nullptr = 生产 platform::http::send；测试注入假发送）
        DirectLlmClient::HttpFn http;
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

    // 测试缝：输入框 onTextInput 只能追加、无法覆盖草稿（赋值把光标置 0），故草稿写入口在此
    void setEditCronDraft(const std::string& cron);
    void setDialogIterDraft(int iter);
    // 测试缝：兜底轻查询已执行次数（节流断言用——同窗口多帧只允许一次轻查询）
    int64_t runFallbackChecksForTest() const { return runFallbackChecks_; }
    // 测试缝：库导入脏标记是否已置位（compose 收割后复位）
    bool dbImportedDirtyForTest() const {
        return dbImportedDirty_.load(std::memory_order_acquire);
    }

    // ---- run 完成通知的 app 级通道（fix round：页面无关） ----
    // 装配层每帧调（app::compose 内）：等价 refreshData 的检测路径，不依赖智能体页可见。
    // 离开智能体页后 AgentModule::compose 不跑，迁移检测的唯一驱动就是这里。
    void pollRunEnd();
    // 取走语义访问器：返回待发通知（title, message），取走即清；空 = 无待发。
    // 装配层拉到后经 app::showAppNotice 发应用级 toast（跨页可见）。
    std::optional<std::pair<std::string, std::string>> takeRunEndNotice();

private:
    // 一次 run：cancel 由 onStop 置位，done 由 run 线程收尾置位（reap 收割依据）
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
    };

    // 惰性装配单入口：首次使用时解析 LlmProfileSource 并建 llm_/loop_/scheduler_；幂等
    void ensureLlm();
    void startTask(int64_t taskId);
    // 任务弹窗（新建/编辑共用表单）：cron 非法 → 弹窗内报错、不落库、不关窗
    void openNewTaskDialog();
    void openTaskEditor(int64_t taskId);
    void closeTaskDialog();
    void saveTaskDialog();
    void toggleTaskEnabled(const AgentTaskRow& task);
    // 定时分支预览行：editCron_ 每变（模板回填 / 手改 / 编辑回填）后重算
    void refreshCronPreview();
    // 详情弹窗
    void openTaskDetail(int64_t taskId);
    void closeTaskDetail();
    void refreshData();
    // 冷启动兜底：pollRunEnd 三守卫全假时的低频轻查询探测（节流见 kRunFallbackIntervalSec）
    void pollRunEndFallback();
    // 任务级 run 汇总缓存：失效判据 = store_->writeVersion()（同 home 余量卡口径）——
    // 版本没变零查询零锁，uiPolling_ 每帧 refreshData 也不重查；版本取查询前快照（查询后
    // 取会漏：读到旧数据却记成新版本，之后永不再查）
    void refreshRunSummaries();
    void reapFinishedRuns();
    // 单条消息展开/收起（seq 定位，切 run 即清）
    void toggleMessageExpanded(int64_t seq);
    // 默认展开最近一条带 text 的 assistant 消息；目标 seq 变化才动展开集，手动收起不被顶回
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
    std::unique_ptr<DirectLlmClient> llm_;
    std::unique_ptr<AgentLoop> loop_;
    // 声明序契约：scheduler_ 必须在 loop_/llm_/store_ 之后——逆序析构下 Scheduler
    // 先亡，其 run 线程 join 期间被引用的 loop_/store_ 仍存活
    std::unique_ptr<Scheduler> scheduler_;
    std::shared_ptr<::core::ITool> statsTool_;
    // 事件触发的 EventBus 订阅 token（ensureLlm 建 scheduler_ 时订阅三事件，emit 在
    // UI 主线程同步回调 → scheduler_->onEvent 轻入队）。RAII：onStop 清空即退订，
    // 不得漏——bus() 单例跨用例/跨生命周期共享（Task 1 concerns ④）
    std::vector<core::event::EventBus::Subscription> eventSubs_;

    // GitHub Sync 导入成功 → 置脏，compose 收割（置 tasksDirty_ 走既有重查路径）。
    // 导入线程同步回调里只碰这两个原子，不碰 UI 线程成员。闸 = 存活守卫：
    // onStop 先关闸再退订（退订拦不住 emit 已拷出的在途 handler）。
    std::atomic<bool> dbImportedDirty_{false};
    std::atomic<bool> dbImportHandlersEnabled_{false};
    core::event::EventBus::Subscription dbImportedSub_;

    // run 线程表：主线程写；mutex 为纵深防御（现调用点全在主线程）
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;
    // 收尾写库失败的 runId（FinishFailureFn 回报，run 线程写）；判停必须排除，与 runs_ 同用 runsMu_
    std::set<int64_t> orphanedRunIds_;

    // compose 数据缓存（仅主线程读写）
    std::vector<AgentTaskRow> tasks_;
    std::map<int64_t, AgentRunRow> latestRuns_;     // taskId → 最近一次 run
    // 任务级「配置异常」缓存（taskId → 原因，仅非空入表）：纯函数判定（trigger_config.h，
    // 与 Scheduler 同源）。在 refreshData 里随任务列表算一次——不在每帧渲染里反复解析 JSON
    std::map<int64_t, std::string> taskTriggerProblems_;
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
    int editTriggerIdx_ = 0;      // 0=手动 1=定时 2=常驻 3=事件（kTriggerKeys 下标）
    int editIter_ = 10;
    bool editDestructive_ = false;
    std::string editContextNote_;
    bool editAutoNote_ = true;
    // 「事件」分支的勾选态（与 agent_module.cpp 的 kEventNames 一一对应；仅主线程）
    std::array<bool, 3> editEvents_{};
    bool editTriggerOpen_ = false;   // 触发类型下拉开合（UI 态）
    bool editSlotOpen_ = false;      // 模型槽位下拉弹层开合
    bool editCronTemplateOpen_ = false;   // cron 模板下拉弹层开合
    // 预览行文案（空 = 不渲染）。合法 →「下次触发：<yyyy-MM-dd HH:mm>」；
    // 非法 → 校验文案（前置于保存期报错）；无匹配 →「一年内无触发时间」。
    std::string cronPreview_;
    bool cronPreviewIsError_ = false;   // 预览行配色：错误色 vs 次要文本色
    // 页内错误行：弹窗之外动作失败唯一可见出口（弹窗内走 editError_）；下一次成功动作清
    std::string pageError_;
    bool tasksDirty_ = false;   // onStart/onPageEnter/写操作置位 → 下帧重查
    bool uiPolling_ = false;    // 有 run 期间每帧跟进状态/消息，全终止后停

    // run 完成通知（refreshData 迁移检测）：上次轮询快照 + 已通知集合（同一 run 只通知一次）。
    // 通知经 pendingNotices_ 队列由装配层取走后发 app 级 toast（app::showAppNotice）——
    // 页内直发 toast 已删（页面无关，fix round 裁决）。
    std::map<int64_t, std::string> prevRunStatuses_;   // runId → status
    std::set<int64_t> notifiedRunIds_;
    // 冷启动兜底轻查询节流：lastRunFallbackSec_ = 上次执行时刻（unix 秒，0 = 从未查过——
    // 真实时刻不可能为 0）；计数供 runFallbackChecksForTest() 节流断言
    int64_t lastRunFallbackSec_ = 0;
    int64_t runFallbackChecks_ = 0;
    // 任务级 run 汇总（详情弹窗汇总行）：writeVersion 缓存；查询失败显形在弹窗内
    std::map<int64_t, AgentTaskRunSummary> runSummaries_;   // taskId → 聚合
    std::string runSummariesError_;
    uint64_t runSummariesSeenVersion_ = 0;
    bool runSummariesSeenVersionValid_ = false;   // false = 从未查过（首查强制）
    // 待发通知队列（title, message）：迁移检测置入，takeRunEndNotice 取走即清。
    // 同帧多个迁移按 runId 升序入队；app 通道 toast 单实例，后发顶替先发（接受）。
    std::vector<std::pair<std::string, std::string>> pendingNotices_;

    // 重入幂等守卫：double-onStart = no-op（防重复注册工具 / 重建服务）；未启动的 off = no-op
    bool started_ = false;
    // 「无配置来源」WARN 去重（惰性解析会从每帧 compose 触发）
    bool llmMissingWarned_ = false;
};

} // namespace module::agent
