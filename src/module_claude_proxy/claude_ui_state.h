// Claude UI 瞬时状态：按「可视区域」收成的纯值 struct（无方法、无虚接口）。
// ProfileListState = 左栏列表；DetailState = 详情页签；FormState = 表单 + 弹窗；LogsState = 调用记录弹窗
#pragma once

#include "claude_service.h"
#include "ccswitch_import.h"
#include "provider_config.h"

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
    // compose 数据缓存（刷新语义见 refreshDataCache）：初值脏 = 首帧必查；seenCallLogVersion_ 初值 ~0 保证 recent 首帧必查
    std::vector<Profile> cachedProfiles_;
    std::string cachedProfilesError_;
    std::vector<ClaudeService::CallLogEntry> cachedRecent_;
    std::string cachedCallsError_;
    Profile cachedActive_;
    bool cachedHasActive_ = false;
    bool dataDirty_ = true;
    bool dataLoaded_ = false;   // 快照是否装载过（冷启动兜底判据，见 refreshDataCache）
    uint64_t seenCallLogVersion_ = ~0ull;
    // shared_future = 结果共享状态（worker 发布 → 唤醒 → 渲染线程收割）；async future 留作 join 句柄。在途判据 = refreshFuture_.valid()
    std::shared_future<DataRefresh> refreshFuture_;
    std::future<void> refreshWorker_;
    std::chrono::steady_clock::time_point lastRefreshPost_{};  // 节流窗口起点
    static constexpr int64_t kRefreshIntervalMs = 200;

    // 「最近调用」数据侧上限（非显示上限，显示行数按可用高推）；取 30 覆盖常规窗口
    static constexpr int kRecentCallsSnapshot = 30;

    // 选中态（0 = 跟随激活 profile）
    int64_t selectedId_ = 0;
    // Profile 列表滚动位置
    float profilesScrollOffset_ = 0.0f;
    // 共享片段卡片区的滚动位置：片段区块钉面板底，卡片装不下时在区块内滚
    float sharedScrollOffset_ = 0.0f;
    // 上一帧的片段集签名：一变就把 sharedScrollOffset_ 归零（初值必不撞 → 首帧归零一次）。
    size_t sharedScrollKey_ = static_cast<size_t>(-1);
    // 共享片段区块折叠态（UAT5 用户拍板：默认收起，点头行展开；会话内内存 bool，
    // claude 模块无 view-state 落盘机制 → 未持久化，跨会话回默认收起）
    bool sharedBlockCollapsed_ = true;
    // 引导条一次性标记（会话内代理从未启动过才显示；内存态）
    bool proxyEverSeen_ = false;
    // S3.4 引导条手动关闭（用户点 × 收起）
    bool guideDismissed_ = false;
    // 切换 Profile 二次确认弹窗（UAT6）：点非选中卡先弹窗，确定后才切 selectedId_
    bool switchConfirmOpen_ = false;
    int64_t switchTargetId_ = 0;
    std::string switchTargetName_;
    bool switchBypassConfirm_ = false;  // 测试缝：true 时直接切不弹窗（全量测试回归门控）
    // 列表右键菜单：目标 profile id（0 = 无）+ 锚点（页面坐标）+ 开合标志
    int64_t ctxProfileId_ = 0;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    bool ctxMenuOpen_ = false;

    // 共享配置片段快照（同组脏标记同刷，见 DataRefresh.sharedConfigs）
    std::vector<SharedConfig> cachedSharedConfigs_;
    std::string cachedSharedConfigsError_;   // 空 = 成功（失败保旧快照 + 记日志）
};

struct DetailState {
    // 0 基础 / 1 模型映射 / 2 环境变量 / 3 高级配置 / 4 最近调用
    int detailTab_ = 0;
};

// 新建/编辑表单 + 删除确认弹窗
struct FormState {
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础 / 1 模型映射 / 2 高级 env / 3 高级配置
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    // formError_ 的来源标记：true = 红字来自高级设置 JSON 校验失败（「放弃修改」据此只清 JSON 源那条）。所有写入必须走 setFormError()/clearFormError()。
    bool formErrorFromJson_ = false;
    std::string formImportWarn_;  // 导入当前配置的接管态警告（弹窗内展示，不阻断）
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
    ModelsConfig formModels_;  // 槽位表全行（默认模型 = def 槽位）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 共享配置片段绑定下拉（渲染在高级配置页）
    int64_t formSharedConfigId_ = 0;   // 表单态绑定（0 = 未绑定；保存时写入 profile）
    bool formSharedDdOpen_ = false;    // 绑定下拉展开态

    // 高级设置 JSON 多行编辑框原文；submitForm 校验通过后统一 pretty 落库
    std::string formExtraSettingsJson_;
    // 「上次落库值」基线（「放弃修改」的恢复目标）= 表单会话开始时的持久化值。
    std::string formExtraSettingsJsonBaseline_;
    // 高级设置 JSON 的行内红字（格式化/校验失败共用，onChange 清空——与 formError_ 分层）。
    std::string formAdvancedJsonError_;
    // 高级设置 JSON 全屏编辑态：文本/红字一律复用表单既有字段，不出现第二份状态
    bool formJsonFullscreen_ = false;

    // 备用 Profile（failover）绑定下拉（复用绑定下拉模式：索引 0 = 无，编辑态排除自身）
    int64_t formFallbackProfileId_ = 0;   // 表单态备用（0 = 不启用 failover）
    bool formFallbackDdOpen_ = false;     // 备用下拉展开态

    // 方言覆盖：只存规范文本（空串 = 未覆盖）；选中态逐帧经 dialectKindFromText 反查 idx（空/未知 → 0「自动」）
    std::string formDialectOverride_;
    bool formDialectDdOpen_ = false;      // 方言下拉展开态
};

// 共享配置片段编辑弹窗（独立状态）。必须接入 anyDialogOpen 门控与 closeTopmostDialog Esc 序链（最顶层）
struct SharedConfigDialogState {
    bool open_ = false;
    int64_t editId_ = 0;             // 0 = 新建，非 0 = 编辑该 id
    std::string name_;               // 片段名（name UNIQUE，服务层校验）
    // 片段内容 JSON 原文（settings.json 非 env 顶层属性预设；保存与落库规则同表单）
    std::string extraSettingsJson_;
    std::string error_;              // 校验/保存错误（红字）

    // 删除确认弹窗（删除 = 同事务解绑绑定它的 profile）
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

// cc-switch 导入弹窗（独立状态，不塞进 FormState：复用会污染表单状态）。必须接入 anyDialogOpen 门控与 closeTopmostDialog Esc 序链（最顶层）
struct CcSwitchImportDialogState {
    bool open_ = false;
    bool loaded_ = false;             // rows 已装载（false = 首次打开需触发读库）
    // 候选行 + 每行映射结果 + 是否勾选；三个向量同长度、按下标对齐
    std::vector<CcSwitchImportRow> rows_;
    std::vector<CcSwitchImportMapping> mappings_;
    std::vector<bool> selected_;
    // 读库失败的中文原因（空 = 成功 / 未尝试）。非空时列表区不画，改画错误态。
    std::string loadError_;
    // 导入结果报告（每条「name: 成功 / 跳过(原因) / 失败(原因)」一行；非空 = 已跑过导入）
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
    // callsFuture_ = 结果共享状态（收割后显式清空）；callsWorker_ = async join 句柄，不留它会同步派发。在途判据 = callsFuture_.valid()；成员析构自动等本轮结束
    std::shared_future<CallsPage> callsFuture_;
    std::future<void> callsWorker_;
};

struct ClaudeUiState {
    ProfileListState list;
    DetailState detail;
    FormState form;
    SharedConfigDialogState shared;   // 共享配置片段编辑弹窗（独立状态）
    CcSwitchImportDialogState ccswitchImport;  // 从 cc-switch 导入弹窗
    ImportState import;
    LogsState logs;
};

// 数据缓存的后台刷新：compose 每帧只做「搬已就绪结果进快照 + 发下一次刷新意图」两件无 IO 的事；SQLite 查询全在后台（同步查会与代理 worker 争锁）。失败保持脏、按节流窗口重试。

// 唯一跑在后台线程的 DB 入口（static 语义：不碰模块成员）
DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles, bool wantRecent);

// 落快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
void applyDataRefresh(ProfileListState& st, DataRefresh r);

// 收割：后台结果就绪才搬（wait_for(0) 不阻塞；未就绪快照沿用上次值）
void harvestDataRefresh(ProfileListState& st);

// 节流：代理高频落库时版本每帧都新，不节流会退化成每帧一个后台查询；失败重试同窗口
bool refreshDue(const ProfileListState& st);

// compose 每帧调：只发意图 + 搬快照，零 IO（脏 = 进页/写操作后重查；recent 另在 callLogVersion 变化时重查）
void refreshDataCache(ProfileListState& st, ClaudeService* svc);

} // namespace module::claude
