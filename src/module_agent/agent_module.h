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
#include "module_agent/agent_time_text.h"
#include "module_agent/direct_llm_client.h"
#include "module_agent/scheduler.h"
#include "module_agent/task_draft.h"
#include "module_agent/task_draft_job.h"
#include "module_agent/tool_info.h"

#include <components/theme.h>
#include <ui_kit/toast.h>

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

// notifiedRunIds 的收敛：只留窗口（本轮 runId→status 快照）内的 id。
// 该集合只增不减是本模块唯一的无界容器（每次 run 结束 +1、会话内不释放）；窗口外 id
// 不会再被观测到（快照只取最近 N 条 run，更老的 run 回不来），剔除不改通知语义。
void pruneNotifiedRunIds(const std::map<int64_t, std::string>& window,
                         std::set<int64_t>& notifiedRunIds);

// 时长秒 → 紧凑中文 / 状态中文 / 各时间格式化已迁 module_agent/agent_time_text.h
//（详情弹窗 P2，spec §11；本头文件不再声明时间文案 helper）。
// 旧 formatRunResultTimeText / formatTaskRunSummaryText 已随「汇总行文本」退休。

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
    // 闹钟时间草稿（YYYY-MM-DD HH:MM，本地时区）：与 cron 同款测试缝
    void setEditOnceDraft(const std::string& text);
    void setDialogIterDraft(int iter);
    // 测试缝：兜底轻查询已执行次数（节流断言用——同窗口多帧只允许一次轻查询）
    int64_t runFallbackChecksForTest() const { return runFallbackChecks_; }
    // 测试缝：库导入脏标记是否已置位（compose 收割后复位）
    bool dbImportedDirtyForTest() const {
        return dbImportedDirty_.load(std::memory_order_acquire);
    }

    // ---- run 停止入口（C1）与运行态查询 ----
    // 协作式取消：只置 ActiveRun::cancel，当前迭代收尾后才生效（in-flight LLM 请求不中断，
    // 迭代边界检查在 agent_loop，用户取消优先于超限判定）。无在跑 run 时仅 LOG 落空，
    // 不崩、不新建 run——幂等
    void requestStopRun(int64_t taskId);
    // 任务是否有在跑 ActiveRun（未 done）：详情弹窗停止钮的渲染条件、列表删除禁用判据之一
    bool isTaskRunActive(int64_t taskId);
    // 在跑 run 是否已被请求停止：停止钮点击后的反馈态（「停止中」）断言面
    bool isStopRequested(int64_t taskId);

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
    // 闹钟分支预览行：editOnceAtText_ 每变 / 切到闹钟类型后重算
    void refreshOncePreview();
    // 详情弹窗
    void openTaskDetail(int64_t taskId);
    void closeTaskDetail();
    void refreshData();
    // 冷启动兜底：pollRunEnd 三守卫全假时的低频轻查询探测（节流见 kRunFallbackIntervalSec）
    void pollRunEndFallback();
    // 系统通知授权：Module 接口没有「窗口可见」回调，由 pollRunEnd（app::compose 在 splash
    // 结束后每帧调用）触发一次；重复调用由成员闸挡住，不重复弹授权框。
    void requestNotificationAuthorizationOnce();
    // 任务级 run 汇总缓存：失效判据 = writeVersion + 7d 窗口起点（版本没变零查询零锁，
    // uiPolling_ 每帧 refreshData 也不重查；版本取查询前快照）
    void refreshRunSummaries();
    // 「下一次」/「计划 vs 实际」缓存：键判等早退，禁在 compose 逐帧回扫 cron（§8.4/§8.5）
    void updateNextRunCache(const AgentTaskRow& task, const AgentRunRow* latest,
                            int64_t nowSec);
    void updateDetailPlanCache(const AgentTaskRow& task, const AgentRunRow& run);
    void reapFinishedRuns();
    // 单条消息展开/收起（seq 定位，切 run 即清）
    void toggleMessageExpanded(int64_t seq);
    // 默认展开最近一条带 text 的 assistant 消息；目标 seq 变化才动展开集，手动收起不被顶回
    void applyDefaultMessageExpansion();
    // 「可用工具」注册表快照：惰性装载，每次展开强制重装（反映模块启停变化）
    void ensureToolsCache(bool forceReload);
    // 单条消息行渲染：徽章头行（徽章+时间+展开/复制 chip）+ 收起预览 / 展开块级全文
    //（prefix = 宿主节点前缀；refSec = 选中 run 的 startedAt，消息时间同日/跨日口径的参照）
    void renderMessageRow(::eui::Ui& inner, float cw, const std::string& prefix,
                          size_t index, const AgentMessageRow& msg,
                          int64_t refSec, const ::core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& tokens);
    // 两张弹窗的绘制（关闭态不建内容节点）
    void composeTaskDialog(::eui::Ui& ui, const ::eui::Screen& screen,
                           const ::core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens);
    void composeTaskDetailDialog(
        ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
        const components::theme::ThemeColorTokens& themeTokens);
    // AI 草稿子弹窗（叠加在任务弹窗之上，zIndex 1100；只在任务弹窗打开时可达）
    void composeDraftDialog(::eui::Ui& ui, const ::eui::Screen& screen,
                            const ::core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens);
    void openDraftDialog();
    void closeDraftDialog();
    // 任务删除确认弹窗（E1，破坏性操作必须确认）：叠在任务/详情弹窗上（zIndex 1100）；
    // 取消/遮罩/Esc 任一路径都不动库，确认才走 performDeleteTask
    void composeDeleteConfirmDialog(
        ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
        const components::theme::ThemeColorTokens& themeTokens);
    // 确认后的真删除：store 单事务级联清 runs/messages → 刷列表、关可能开着的详情弹窗；
    // 失败置页级错误行（不吞）
    void performDeleteTask(int64_t taskId);
    // 点「生成」：主线程校验输入 → 起后台线程 requestTaskDraft + parseTaskDraft。
    // 生成中重复点击被挡（按钮禁用 + 这里再挡一道）；结果由 reapDraftJob 回收。
    void startDraftGeneration();
    // 主线程收割已完成的生成线程：join → 预填表单 / 写错误行 / 丢弃过期结果。
    // compose 每帧调用（worker 置 done 后 requestUiUpdate 主动要帧，不等用户输入）。
    void reapDraftJob();
    // 把校验过的草稿填进表单：只填模型给出的可信字段（goalFilled / triggerFilled /
    // 非空 contextNote）；触发不可信时一字不动。结果永不落库——保存仍是用户动作。
    void applyDraftPrefill(const TaskDraftResult& res);

    Deps deps_;
    std::unique_ptr<AgentStore> store_;
    std::unique_ptr<DirectLlmClient> llm_;
    std::unique_ptr<AgentLoop> loop_;
    // 声明序契约：scheduler_ 必须在 loop_/llm_/store_ 之后——逆序析构下 Scheduler
    // 先亡，其 run 线程 join 期间被引用的 loop_/store_ 仍存活
    std::unique_ptr<Scheduler> scheduler_;
    std::shared_ptr<::core::ITool> statsTool_;
    // app.notify：构造函数注入 fallback（工作线程 → pendingNotices_ 线程安全入队）。
    // onStop 在 run 线程 join 之后注销/reset，避免工具回调碰到已析构的 this。
    std::shared_ptr<::core::ITool> notifyTool_;
    // hot.list：默认构造（defaultHotFetcher/defaultNow，生产走 platform::http + 系统时钟），
    // 缓存与限频在工具实例内自持，模块零附加状态。非破坏性只读工具，onStop 与其余
    // 自带工具同批注销。
    std::shared_ptr<::core::ITool> hotTool_;
    // app.now：本地时间快照（纯读、无状态），onStart 注册 / onStop 注销同批口径。
    std::shared_ptr<::core::ITool> nowTool_;
    // net.fetch：联网 GET 抓取（外部请求，仅 GET，10s 超时；单次调用语义，无重放/
    // 并发限制）。非破坏性只读，onStop 与其余自带工具同批注销。
    std::shared_ptr<::core::ITool> netTool_;
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

    // 任务是否有在跑 ActiveRun（未 done）。锁内判定、不外泄指针：ActiveRun 生命周期归
    // runs_（erase 即亡），裸指针逃出 runsMu_ 后再解引用就是 use-after-free——
    // 停止/查询面一律「锁内找到、锁内用完」，不把 ActiveRun* 带出锁边界
    bool hasActiveRun(int64_t taskId);
    // 收割已完成的 OS 通知发送线程（M2）：只收 done 的（join 立即返回，不堵帧）；
    // 在飞的留下帧再收。仅主线程调用（pollRunEnd 每帧；onStop/析构走全量 drain join）
    void reapNotifySends();
    // run 线程表：主线程写；mutex 为纵深防御（现调用点全在主线程）
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;
    // 收尾写库失败的 runId（FinishFailureFn 回报，run 线程写）；判停必须排除，与 runs_ 同用 runsMu_
    std::set<int64_t> orphanedRunIds_;

    // 一次 OS 系统通知的后台发送（M2）：send 的授权检查在 macOS 未决期可阻塞 ≤1s，
    // 不得在 UI 主线程直调。worker 只摸值拷贝的 Notification 与自己的 done 原子，
    // 不碰任何模块成员；join 纪律同 T7b DraftJob——reap 只收 done 的，onStop/析构
    // 全量 join（先于成员析构，不用 detach，不留退出期悬垂线程）
    struct NotifySend {
        std::thread thread;
        std::atomic<bool> done{false};
    };
    std::mutex notifySendsMu_;
    std::vector<std::unique_ptr<NotifySend>> notifySends_;

    // compose 数据缓存（仅主线程读写）
    std::vector<AgentTaskRow> tasks_;
    std::map<int64_t, AgentRunRow> latestRuns_;     // taskId → 最近一次 run
    // 任务级「配置异常」缓存（taskId → 原因，仅非空入表）：纯函数判定（trigger_config.h，
    // 与 Scheduler 同源）。在 refreshData 里随任务列表算一次——不在每帧渲染里反复解析 JSON
    std::map<int64_t, std::string> taskTriggerProblems_;
    // 限次任务缓存（taskId → max_runs，仅 schedule / event 型入表；2026-10-17 加）：
    // 列表徽章与详情摘要的展示依据，同样随 refreshData 算一次。manual / resident 不入表
    // ——它们的 trigger_config 不被 Scheduler 消费，挂「一次性」徽章等于骗人
    std::map<int64_t, int> taskRunLimits_;
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
    // 详情弹窗窄屏 tab（0=执行记录 1=运行详情；宽屏恒 0 不消费）。点时间线行自动切 1
    int detailTab_ = 0;
    // 详情弹窗配置头开合（覆盖式浮层 .config.scrim/.popover 随本位渲染，P3 起）
    bool configOpen_ = false;
    // 消息区开合（D6 默认折叠）：false 只画标题行 + `.msgs.hint`，不建 .msgscroll/
    // .msgs.body；展开时 applyDefaultMessageExpansion 降级为「展开态内规则」
    bool messagesOpen_ = false;
    // 页级复制 toast（spec D13，`agent.detail.toast`）：compose() 尾部最后绘制
    ui_kit::ToastState toast_;
    // 「可用工具」区块：收起默认，头部点击展开
    bool toolsOpen_ = false;
    std::vector<ToolInfo> toolsCache_;      // 注册表快照（惰性装载）
    bool toolsCacheLoaded_ = false;
    // 任务弹窗：taskDialogOpen_ 控开关，editingTaskId_ 非空 = 编辑态，字段两态共用
    bool taskDialogOpen_ = false;
    // 任务删除确认弹窗（E1）：非空 = 确认弹窗开着，值 = 待删任务 id（名字由 tasks_ 现查）
    std::optional<int64_t> deleteConfirmTaskId_;
    std::optional<int64_t> editingTaskId_;
    std::string editGoal_, editCron_, editSlot_, editError_;
    // UAT5 重设计：任务名称独立输入（TaskDef.name 一一对应）；空 = 保存时 deriveTaskName(goal)
    std::string editName_;
    // 闹钟时间草稿（严格 YYYY-MM-DD HH:MM，本地时区）。只在 trigger_type=once 时消费；
    // 其它类型保留草稿但不落库（切换类型后原样切回不丢用户输入）
    std::string editOnceAtText_;
    int editTriggerIdx_ = 0;      // 0=手动 1=定时 2=常驻 3=事件 4=每天 5=闹钟（kTriggerKeys 下标）
    int editIter_ = 10;
    bool editDestructive_ = false;
    // 运行次数上限草稿（0 = 不限次数，缺省；>0 = 最多 N 次）。UI 目前只提供 1
    //（「只跑一次」开关），但草稿是整数：手改库/将来放开 N 次时编辑态能原样回写，
    // 不会被开关压回 1（往返不丢数据）
    int editMaxRuns_ = 0;
    std::string editContextNote_;
    bool editAutoNote_ = true;
    std::string editSystemPrompt_;
    // 「事件」分支的勾选态（与 agent_module.cpp 的 kEventNames 一一对应；仅主线程）
    std::array<bool, 3> editEvents_{};
    bool editTriggerOpen_ = false;   // 触发类型下拉开合（UI 态）
    bool editSlotOpen_ = false;      // 模型槽位下拉弹层开合
    bool editCronTemplateOpen_ = false;   // cron 模板下拉弹层开合
    // AI 草稿子弹窗态（全部仅主线程）：draftInput_ 跨开合保留（用户的话不因关窗丢失）；
    // 关闭子弹窗即 bump draftEpoch_（在途结果将作废），重开按 draftJob_ 存活如实显示生成中
    bool draftDialogOpen_ = false;
    std::string draftInput_;
    DraftPhase draftPhase_ = DraftPhase::Idle;
    std::string draftNotice_;    // Done：已预填字段摘要（+「请检查后点保存」）
    std::string draftWarnings_;  // Done：降级警告 + needsUserInput（空 = 不渲染）
    std::string draftError_;     // Failed：失败原因 / 输入校验错误（空 = 不渲染）
    std::mutex draftMu_;                        // 保护 draftJob_ 槽位本身的换入换出
    std::unique_ptr<DraftJob> draftJob_;        // 非空 = 有生成在途或待收割
    int draftEpoch_ = 0;                        // 会话代次（仅主线程写）
    // 预览行文案（空 = 不渲染）。合法 →「下次触发：<yyyy-MM-dd HH:mm>」；
    // 非法 → 校验文案（前置于保存期报错）；无匹配 →「一年内无触发时间」。
    std::string cronPreview_;
    bool cronPreviewIsError_ = false;   // 预览行配色：错误色 vs 次要文本色
    // 闹钟分支预览行：合法 →「将在 <yyyy-MM-dd HH:mm> 提醒（需应用运行；错过不补）」；
    // 非法 / 过去 → 错误文案（红色）。与 cronPreview_ 互斥渲染（同一行几何）
    std::string oncePreview_;
    bool oncePreviewIsError_ = false;
    // 页内错误行：弹窗之外动作失败唯一可见出口（弹窗内走 editError_）；下一次成功动作清
    std::string pageError_;
    // 已同步到页内错误行的 runId 集合。用来阻断**回弹**：用户清掉错误行后，只要那些
    // run 还是各自任务的最近一次 run，后续 refreshData 不得把同一份旧错误重新顶回来
    //（旧行为：见 pageError_ 为空即重新同步，粘到有新 run 为止）。
    // 为什么是集合而不是单个 id：多任务各有配置不可用时（A 的错展示 → 清除 → B 的错
    // 顶上），只记最后一个会让 A 的错在下一轮兜回来 —— 循环展示（用例
    // agent_ui_page_error_multi_task_order ③ 钉死）。
    // 有界性 = 任务数：每次 refreshData 按最新 run 表剪枝（任务再跑出新 run → 旧 id
    // 永不再匹配，留着只是无界增长 —— `notifiedRunIds_` 同款教训）。
    // 不在 onStop 清：换页回来也不该再弹用户已看过的旧错误。
    std::set<int64_t> pageErrorRunIds_;
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
    // 任务级 run 汇总（详情弹窗摘要三卡）：writeVersion + 7d 窗口双键缓存；查询失败显形
    std::map<int64_t, AgentTaskRunSummary> runSummaries_;   // taskId → 聚合
    std::string runSummariesError_;
    uint64_t runSummariesSeenVersion_ = 0;
    bool runSummariesSeenVersionValid_ = false;   // false = 从未查过（首查强制）
    int64_t runSummariesSeenWindowStart_ = -1;    // 缓存键另一半：跨天无写入也必须重查
    // 详情弹窗「计划 vs 实际」缓存（previousCronMatch 禁逐帧回扫，spec §8.4）：
    // 键 = taskId+runId+startedAt+triggerType+triggerConfig，键不变零重算
    std::optional<RunPlanInfo> detailPlan_;
    std::string detailPlanKey_;
    // 详情弹窗「下一次」缓存（nextCronMatch 同禁逐帧回扫，spec §8.5）：
    // 键 = now 分钟 + taskId + 最近 run id/status；manual/event 等无回扫路径也走键判等
    std::optional<NextRunInfo> nextRunText_;
    int64_t nextRunMinute_ = -1;
    int64_t nextRunTaskId_ = 0;
    int64_t nextRunLatestRunId_ = 0;
    std::string nextRunLatestStatus_;
    // 详情消息读取失败显形（spec §10：红字「消息读取失败：…」，标题保留、不展示旧消息）
    std::string detailMessagesError_;
    // 待发通知队列（title, message）：迁移检测置入，takeRunEndNotice 取走即清。
    // 同帧多个迁移按 runId 升序入队；app 通道 toast 单实例，后发顶替先发（接受）。
    // 生产者有两个线程：UI 线程（run-end 迁移）+ AgentLoop 工作线程（app.notify 失败
    // 降级），故读写都必须持锁；锁内只碰队列，不回调外部代码。
    std::mutex pendingNoticesMu_;
    std::vector<std::pair<std::string, std::string>> pendingNotices_;

    // 通知授权只请求一次（跨 onStop/onStart 也不重复弹框；系统对已决定状态本就幂等）
    bool notificationAuthorizationRequested_ = false;

    // 重入幂等守卫：double-onStart = no-op（防重复注册工具 / 重建服务）；未启动的 off = no-op
    bool started_ = false;
    // 「无配置来源」WARN 去重（惰性解析会从每帧 compose 触发）
    bool llmMissingWarned_ = false;
};

} // namespace module::agent
