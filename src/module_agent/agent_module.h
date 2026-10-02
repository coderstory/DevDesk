// src/module_agent/agent_module.h
// 智能体页模块（agent M1 Task 12）：生命周期实体 + 手动触发 + 页面骨架。
// Task 11 的装配面（PageId::Agent / displayName / iconGlyph / 工厂签名）保持
// 不变；本文件填实 onStart/onStop 的服务装配与拆解、run 线程管理、compose。
//
// 2026-10-02 批次 A「交互骨架重做」：页面只留「标题 + 可用工具 + 任务列表
// （scrollView）」，新建 / 编辑共用一张任务弹窗，run 详情改独立弹窗——页内
// 表单与页内详情展开区已删除（节点 id 契约见 agent_module.cpp 头注释）。
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
        // Scheduler 的 unix 秒时钟缝（nullptr = 生产真实系统时间；测试注入假
        // 时钟驱动 cron 判定，零墙钟等待）
        Scheduler::NowFn schedulerNow;
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
    // 弹窗打开时 Esc = 关最上层弹窗（任务弹窗 / 详情弹窗）。键走得到本通道 =
    // 焦点元素没消费（弹窗输入框聚焦时 Esc 已被 input 的 onCancel 收走，
    // 与 todo 模块同一约定）
    bool onKeyEvent(const ::eui::KeyEvent& event) override;

    // 测试缝（agent_ui 用例直调，等价于用户在弹窗表单里完成该动作）：
    // 不引 EUI-NEO 输入的真事件机制——输入框的 onTextInput 只能追加、无法覆盖
    // 已有草稿（程序赋值把光标置 0，追加也落在开头），故草稿写入口落在这里
    //（trigger / slot 下拉不需要缝：用例直调 `agent.dialog.task.<f>.item.<i>`
    // 上真实挂的 onChange；destructive 开关同理直调 `.hit` 的 onClick）
    void setEditCronDraft(const std::string& cron);
    void setDialogIterDraft(int iter);

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

    // 惰性装配单入口（Task 6 Step 1b）：首次使用时解析 IClaudeProxyService 并
    // 建 llm_/loop_/scheduler_（onStart 的装配退化为一次预热）。幂等——loop_ 非空
    // 即返回，重复调用不会起第二个 Scheduler/tick 线程。
    void ensureLlm();
    void startTask(int64_t taskId);
    // ---- 任务弹窗（2026-10-02 批次 A2：新建 / 编辑共用同一张表单）----
    // 新建态：editingTaskId_ 空 + 字段取默认值；编辑态：字段从 getTask 回填。
    // 保存：编辑态 updateTask / 新建态 createTask；cron 非法 → 弹窗内报错、
    // 不落库、弹窗不关。取消 / ✕ / 遮罩 / Esc → 关弹窗不落库。
    void openNewTaskDialog();
    void openTaskEditor(int64_t taskId);
    void closeTaskDialog();
    void saveTaskDialog();
    void toggleTaskEnabled(const AgentTaskRow& task);
    // ---- 详情弹窗（2026-10-02 批次 A3）----
    void openTaskDetail(int64_t taskId);
    void closeTaskDetail();
    void refreshData();
    void reapFinishedRuns();
    // run 日志（2026-10-02 改造）：单条消息展开/收起（seq 定位，切 run 即清）
    void toggleMessageExpanded(int64_t seq);
    // 默认展开态：最近一条带 text 块的 assistant 消息（「跑完啥都没输出」的
    // 正面修复——详情弹窗一开就能看到最终回复）；目标 seq 变化才动展开集，
    // 用户手动收起后不再被每帧刷新顶回来
    void applyDefaultMessageExpansion();
    // 「可用工具」区块：注册表快照的惰性装载（首次 compose 装一次，
    // 每次展开重装一次——反映模块启停后的注册表变化）
    void ensureToolsCache(bool forceReload);
    // 单条消息行渲染（scroll 内容回调内逐条调）：角色徽章头行 + 收起预览 /
    // 展开块级全文（text 块 wrap、tool_use 🔧 chip、tool_result 输出全文）。
    // prefix = 宿主节点前缀（详情弹窗的 agent.dialog.detail）
    void renderMessageRow(::eui::Ui& inner, float cw, const std::string& prefix,
                          size_t index, const AgentMessageRow& msg,
                          const ::core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& tokens);
    // 两张弹窗的绘制（compose 尾段调用；关闭态不建内容节点）
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
    // 声明序契约（scheduler.h 头注释 ①）：scheduler_ 必须在它引用的 loop_ /
    // llm_ / store_ **之后**——逆序析构下 Scheduler 先亡，其 run 线程 join 期间
    // 被引用的 loop_/store_ 仍存活
    std::unique_ptr<Scheduler> scheduler_;
    std::shared_ptr<::core::ITool> statsTool_;

    // run 线程表：主线程写（startTask/onStop），mutex 照 brief 骨架保留
    //（现全部调用点同在主线程，属纵深防御，不为省锁去改契约）
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;

    // compose 数据缓存（仅主线程读写；run 线程只经 SQLite 触库，不碰这里）
    std::vector<AgentTaskRow> tasks_;
    std::map<int64_t, AgentRunRow> latestRuns_;     // taskId → 最近一次 run
    // 详情弹窗的 run 列表（每任务最近 5 次，refreshData 装载）
    std::map<int64_t, std::vector<AgentRunRow>> taskRuns_;
    // 详情弹窗（2026-10-02 批次 A3：取代旧的页内展开区状态）
    std::optional<int64_t> detailTaskId_;           // 非空 = 弹窗打开
    std::optional<AgentTaskRow> detailTaskRow_;     // 配置摘要快照（getTask，与 A2 同源）
    std::optional<int64_t> selectedRunId_;          // 详情弹窗选中的 run（默认最近一次）
    std::vector<AgentMessageRow> detailMessages_;   // 选中 run 的消息
    // 展开全文的消息（按 seq 定位；切任务/切 run 清空——seq 只在 run 内唯一）
    std::set<int64_t> expandedMsgSeqs_;
    // 自动展开的那条（最近一条带 text 块的 assistant 消息）：目标 seq 变化才
    // 重设展开集，用户手动收起后不被每帧刷新顶回来
    std::optional<int64_t> autoExpandedSeq_;
    // scrollView contentKey 版本号：展开/收起改变内容高，bump 触发重测
    //（MeasureCache 按 key 缓存，key 不变会吃到旧内容高）
    uint64_t msgScrollVersion_ = 0;
    // 「可用工具」区块（2026-10-02 用户反馈④）：收起默认，头部点击展开
    bool toolsOpen_ = false;
    std::vector<ToolInfo> toolsCache_;      // 注册表快照（惰性装载）
    bool toolsCacheLoaded_ = false;
    // 任务弹窗（2026-10-02 批次 A2）：新建 / 编辑共用一张表单——taskDialogOpen_
    // 控开关，editingTaskId_ 非空 = 编辑态（空 = 新建态），字段两态共用
    bool taskDialogOpen_ = false;
    std::optional<int64_t> editingTaskId_;
    std::string editGoal_, editCron_, editSlot_, editError_;
    int editTriggerIdx_ = 0;      // 0=手动 1=定时 2=常驻（kTriggerTypes 下标）
    int editIter_ = 10;
    bool editDestructive_ = false;
    bool editTriggerOpen_ = false;   // 触发类型下拉弹层开合（UI 态，不落库）
    bool editSlotOpen_ = false;      // 模型槽位下拉弹层开合
    // 页内错误行（agent.page.error）：动作在「弹窗之外」就失败时的唯一可见
    // 出口——弹窗内的失败走 editError_（弹窗内红字），页级的失败（编辑读库
    // 失败 / 启停失败，都是陈旧缓存的行已被外部删除）走这里；下一次成功动作清
    std::string pageError_;
    bool tasksDirty_ = false;   // onStart/onPageEnter/写操作置位 → 下帧重查
    bool uiPolling_ = false;    // 有 run 期间每帧跟进状态/消息，全终止后停

    // 重入幂等守卫（活门控 Task 4）：double-onStart = no-op——防 store_/
    // statsTool_ 重建后 registerTool 撞同名旧指针 fail（被日志吞）、注册表留
    // 旧工具而成员已换新；double-onStop / 未启动的 off = no-op
    bool started_ = false;
    // 「无代理服务」WARN 去重（惰性解析可能从每帧 compose 触发：不节流会刷屏）
    bool llmMissingWarned_ = false;
};

} // namespace module::agent
