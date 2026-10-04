// Claude UI 瞬时状态：按「可视区域」收成的纯值 struct（无方法、无虚接口）。
// ProfileListState = 左栏列表 + header + 数据缓存/后台刷新；DetailState =
// 详情页签；FormState = 表单 + 弹窗；LogsState = 调用记录弹窗；ImportState =
// 粘贴导入。字段名逐字保留原成员名（含尾下划线）
#pragma once

#include "claude_service.h"
#include "ccswitch_import.h"
#include "provider_config.h"
#include "quota/quota_dialog_config.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace module::claude {

// 数据刷新：工作线程产物（不触碰 this，结果经 future 值回渲染线程再落成员）。
// 脏标记刷三类，纯版本轮询只刷 recent（不重复查没变的 profile 列表）
struct DataRefresh {
    bool wantProfiles = false;
    bool wantRecent = false;
    std::vector<Profile> profiles;
    std::string profilesError;
    Profile active;
    bool hasActive = false;
    std::vector<ClaudeService::CallLogEntry> recent;
    bool recentOk = false;
    std::string callsError;
    uint64_t verBefore = 0;
    // 与 profiles 同组同刷——左栏区块 / 详情行 / 表单绑定下拉共用这份快照
    std::vector<SharedConfig> sharedConfigs;
    std::string sharedConfigsError;   // 空 = 成功（失败不置脏，保旧快照 + 记日志）
};

// 调用记录弹窗的后台查询结果包（值语义，经 future 回渲染线程）。
struct CallsPage {
    std::vector<ClaudeService::CallLogEntry> rows;  // 当页行
    int total = 0;                                  // 满足过滤的总条数
    std::string error;                              // 空串 = 成功
};

// 左栏：Profile 列表 + header/引导条 + 数据缓存
struct ProfileListState {
    // compose 数据缓存（刷新语义见 refreshDataCache）。初值脏 = 首帧必查；
    // seenCallLogVersion_ 初值 ~0 保证 recent 首帧必查（真实版本号从 0 起）
    std::vector<Profile> cachedProfiles_;
    std::string cachedProfilesError_;
    std::vector<ClaudeService::CallLogEntry> cachedRecent_;
    std::string cachedCallsError_;
    Profile cachedActive_;
    bool cachedHasActive_ = false;
    bool dataDirty_ = true;
    bool dataLoaded_ = false;   // 快照是否装载过（冷启动兜底判据，见 refreshDataCache）
    uint64_t seenCallLogVersion_ = ~0ull;
    // 后台刷新结果槽 + worker 生命周期（语义同 LogsState）：shared_future =
    // 结果共享状态，worker set_value 发布 → requestUiUpdate 唤醒；async 的
    // future 留作 join 句柄。在途判据 = refreshFuture_.valid()
    std::shared_future<DataRefresh> refreshFuture_;
    std::future<void> refreshWorker_;
    std::chrono::steady_clock::time_point lastRefreshPost_{};  // 节流窗口起点
    // 刷新节流窗口（原 static constexpr 成员；static 成员随 struct 走，
    // 只声明在状态旁边，不占实例字节）
    static constexpr int64_t kRefreshIntervalMs = 200;

    // 「最近调用」页签的数据侧上限（**不是**显示上限——显示行数由详情面板
    // 按可用高推，超出用「另有 N 条」表达）。取 30 覆盖常规窗口的页签预算
    static constexpr int kRecentCallsSnapshot = 30;

    // 选中态（0 = 跟随激活 profile）
    int64_t selectedId_ = 0;
    // Profile 列表滚动位置（审计 [26]：卡片超界可滚，不再压底部按钮）
    float profilesScrollOffset_ = 0.0f;
    // 共享片段卡片区的滚动位置：片段区块钉面板底，卡片装不下时在区块内滚
    float sharedScrollOffset_ = 0.0f;
    // 上一帧的片段集签名：一变就把 sharedScrollOffset_ 归零。初值取不可能
    // 撞上的值 → 首帧即归零一次（无行为变化）
    size_t sharedScrollKey_ = static_cast<size_t>(-1);
    // 引导条一次性标记（§6.4：会话内代理从未启动过才显示；内存态不持久化）
    bool proxyEverSeen_ = false;
    // 列表右键菜单（UI 现代化 M1-5）：目标 profile id（0 = 无）+ 锚点
    //（页面坐标，经 core::menuAnchorForPointer 统一换算）+ 开合标志
    int64_t ctxProfileId_ = 0;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    bool ctxMenuOpen_ = false;

    // 共享配置片段快照（同组脏标记同刷，见 DataRefresh.sharedConfigs）
    std::vector<SharedConfig> cachedSharedConfigs_;
    std::string cachedSharedConfigsError_;   // 空 = 成功（失败保旧快照 + 记日志）
};

// 右栏：详情面板页签
struct DetailState {
    // 0 基础 / 1 模型映射 / 2 环境变量 / 3 高级配置 / 4 最近调用
    int detailTab_ = 0;
};

// ============ 新建/编辑表单 + 删除确认弹窗 ============
struct FormState {
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础 / 1 模型映射 / 2 高级 env / 3 高级配置
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    // formError_ 的来源标记：true = 红字来自高级设置 JSON 校验失败。「放弃
    // 修改」据此精准清幽灵错误：只清 JSON 源那条，其它仍成立的错误保留。
    // 维护约定：formError_ 所有写入必须走 claude_ui_form.cpp 的 setFormError()
    // / clearFormError()（直接赋值会留下过期标记）
    bool formErrorFromJson_ = false;
    std::string formImportWarn_;  // 导入当前配置的接管态警告（弹窗内展示，不阻断）
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
    ModelsConfig formModels_;  // 槽位表全行（默认模型 = def 槽位，旧 formModel_ 并入）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    // 删除确认弹窗
    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 共享配置片段绑定下拉（渲染在高级配置页）
    int64_t formSharedConfigId_ = 0;   // 表单态绑定（0 = 未绑定；保存时写入 profile）
    bool formSharedDdOpen_ = false;    // 绑定下拉展开态

    // 高级设置 JSON 多行编辑框原文；submitForm 校验通过后统一 pretty 落库
    std::string formExtraSettingsJson_;
    // 「上次落库值」基线（「放弃修改」的恢复目标）= 本次表单会话开始时的
    // 持久化值，不是「进入全屏那一刻的缓冲」
    std::string formExtraSettingsJsonBaseline_;
    // 高级设置 JSON 的行内红字（§7.2 的 claude.form.advanced.json.error 节点；
    // 格式化/校验失败共用，onChange 清空——与 formError_ 提交闸门分层）
    std::string formAdvancedJsonError_;
    // 高级设置 JSON 全屏编辑态：文本/红字一律复用表单既有字段——全屏只是
    // 同一状态的大画布，不出现第二份状态
    bool formJsonFullscreen_ = false;

    // 备用 Profile（failover）绑定下拉（复用绑定下拉模式：索引 0 = 无，
    // 编辑态排除自身）
    int64_t formFallbackProfileId_ = 0;   // 表单态备用（0 = 不启用 failover）
    bool formFallbackDdOpen_ = false;     // 备用下拉展开态

    // 方言覆盖：只存规范文本（空串 = 未覆盖）；选中态逐帧经 dialectKindFromText
    // 反查 idx（空/未知 → 0「自动」）
    std::string formDialectOverride_;
    bool formDialectDdOpen_ = false;      // 方言下拉展开态
};

// 共享配置片段编辑弹窗（独立状态）。**必须接入 anyDialogOpen 门控与
// closeTopmostDialog Esc 序链（入序链最顶层）**
struct SharedConfigDialogState {
    bool open_ = false;
    int64_t editId_ = 0;             // 0 = 新建，非 0 = 编辑该 id
    std::string name_;               // 片段名（name UNIQUE，服务层校验）
    // 片段内容 JSON 原文（settings.json 非 env 顶层属性预设；保存与落库
    // 规则同表单）
    std::string extraSettingsJson_;
    std::string error_;              // 校验/保存错误（红字）

    // 删除确认弹窗（删除 = 同事务解绑绑定它的 profile，设计 §2.6）
    bool deleteConfirmOpen_ = false;
    int64_t deleteConfirmId_ = 0;
    std::string deleteConfirmName_;
};

// cc-switch / settings.json 粘贴导入（高级 env 页）
struct ImportState {
    std::string importText_;       // 多行粘贴框原文
    bool importReady_ = false;     // 解析成功待应用
    std::string importError_;      // 解析失败信息（红字）
    ImportedConfig importPreview_; // 解析结果预览
};

// cc-switch 导入弹窗（独立状态，不塞进 FormState：带只读读库与导入结果
// 报告，复用会让表单状态被一次导入批量污染）。**必须接入 anyDialogOpen
// 门控与 closeTopmostDialog Esc 序链（绘制序最靠后 = 序链最顶层）**
struct CcSwitchImportDialogState {
    bool open_ = false;
    bool loaded_ = false;             // rows 已装载（false = 首次打开需触发读库）
    // 候选行 + 每行映射结果 + 是否勾选；三个向量同长度、按下标对齐
    std::vector<CcSwitchImportRow> rows_;
    std::vector<CcSwitchImportMapping> mappings_;
    std::vector<bool> selected_;
    // 读库失败的中文原因（空 = 成功 / 未尝试）。非空时列表区不画，改画错误态。
    std::string loadError_;
    // 导入结果报告（每条「name: 成功 / 跳过(原因) / 失败(原因)」一行；
    // 非空 = 已跑过一次导入）
    std::vector<std::string> resultLines_;
    int resultSuccess_ = 0;
    int resultSkipped_ = 0;
    int resultFailed_ = 0;
};

// 完整调用记录弹窗（搜索 / 状态码过滤 / 翻页共用）
struct LogsState {
    bool callsDialogOpen_ = false;
    std::vector<ClaudeService::CallLogEntry> allCalls_;  // 快照：当页行（渲染只读）
    std::string callsSearch_;        // 搜索框文本（path/method 模糊）
    int callsStatusIdx_ = 0;         // 状态下拉索引（→ kCallsStatusClasses）
    bool callsStatusDdOpen_ = false; // 状态下拉展开态
    int callsPage_ = 0;              // 当前页（0-based）
    int callsTotalMatches_ = 0;      // 快照：满足过滤的总条数（countCalls）
    float callsScrollOffset_ = 0.0f; // 列表区滚动位置（翻页时归零）
    // 调用记录取数的 future 刷新态（语义见 refreshCallsCache）
    bool callsPageDirty_ = true;     // 置脏 → 下一帧（弹窗打开时）发一次读意图
    bool callsPageLoaded_ = false;   // 快照是否装载过（未装载显示「加载中…」）
    std::chrono::steady_clock::time_point callsEarliestQueryAt_{};  // 最早派发时刻
    // 后台查询结果槽 + worker 生命周期（工作线程不触碰 this；成员析构自动等
    // 本轮查询结束 → 无悬垂）。callsFuture_ = 结果共享状态（worker set_value
    // 发布，渲染线程收割后显式清空）；callsWorker_ = async 的 join 句柄，不留
    // 它等于同步派发（临时 future 析构会阻塞到任务跑完），不表示「在途」。
    // 在途判据 = callsFuture_.valid()
    std::shared_future<CallsPage> callsFuture_;
    std::future<void> callsWorker_;
};

// 余量查询配置弹窗（独立状态：带后台查询「测试连接」，复用会污染表单状态；
// 必须接入 anyDialogOpen 门控与 closeTopmostDialog Esc 序链最顶层）
using quota::QuotaDialogState;

struct ClaudeUiState {
    ProfileListState list;
    DetailState detail;
    FormState form;
    SharedConfigDialogState shared;   // 批 2：共享配置片段编辑弹窗（独立状态）
    CcSwitchImportDialogState ccswitchImport;  // 从 cc-switch 导入弹窗
    ImportState import;
    LogsState logs;
    QuotaDialogState quota;           // G6：余量查询配置弹窗
};

// 数据缓存的后台刷新：compose 每帧只做两件无 IO 的事——搬已就绪结果进快照 +
// 发下一次刷新意图；SQLite 查询全部在后台线程（在 compose 里同步查会与代理
// worker 的 logCall INSERT 争同一把 recursive_mutex，代理忙时整帧被卡）。
// 脏标记触发三类重查；recent 另按 callLogVersion 感知异步落库（最多晚一个
// 200ms 节流 tick + 1 帧，不丢数据）；查询失败保持脏、按节流窗口重试。

// 唯一跑在后台线程的 DB 入口（static 语义：不碰模块成员）
DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles, bool wantRecent);

// 落快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
void applyDataRefresh(ProfileListState& st, DataRefresh r);

// 收割：后台结果就绪才搬（wait_for(0) 不阻塞；未就绪快照沿用上次值）
void harvestDataRefresh(ProfileListState& st);

// 节流窗口：代理高频落库时版本每帧都新，不节流会退化成每帧一个后台查询；
// 失败重试同窗口（按窗口重发，不按帧起线程）
bool refreshDue(const ProfileListState& st);

// compose 每帧调：只发意图 + 搬快照，零 IO（脏 = 进页/写操作后重查；
// recent 另在 callLogVersion 变化时重查）
void refreshDataCache(ProfileListState& st, ClaudeService* svc);

} // namespace module::claude
