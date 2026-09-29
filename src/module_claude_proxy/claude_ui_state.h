// src/module_claude_proxy/claude_ui_state.h
// R4（P-B2）claude_module.cpp 的成员按「可视区域」收成 5 个纯值 struct
//（spec 2026-09-27-god-class-refactor-design §3.2）：ProfileListState /
// DetailState / FormState / LogsState / ImportState —— 判据 J2：纯 UI 瞬时
// 字段同文件顶部收成一个 struct，不成类（无方法、无虚接口、无 Pimpl）。
//
// 命名口径（P-B2「纯机械改名，编译期全量校验」）：字段名**逐字保留**原成员名
// （含尾下划线），只加 `state_.xxx.` 前缀 —— 好处是 diff 可逐行核对
// 「老名 → 新家」，不会在搬迁中引入第二处改名失误；也便于 grep 反查归属。
//
// 成员的分配依据「谁画谁持有」：
//   - ProfileListState：左栏列表 + header/引导条 + 数据缓存/后台刷新（compose
//     每帧读的快照 + 刷新意图都在这里；claude_ui_list 与 compose 骨架共用）
//   - DetailState：右栏三 pane 的页签
//   - FormState：表单三页 + 编辑弹窗 + 删除确认弹窗（同文件 —— claude_ui_form）
//   - LogsState：完整调用记录弹窗（过滤/分页/后台取数）—— claude_ui_logs
//   - ImportState：cc-switch / settings.json 粘贴导入（高级 env 页）—— claude_ui_form
#pragma once

#include "claude_service.h"
#include "provider_config.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace module::claude {

// ============ 数据刷新（审计 perf [13] + H2）============
// 工作线程产物：一次刷新的三类数据（不触碰 this —— 结果经 future 值回渲染
// 线程再落成员）。wantProfiles/wantRecent 保留旧语义：脏标记刷三类，
// 纯版本轮询只刷 recent（不重复查没变的 profile 列表）。
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
    // 批 2（共享配置片段）：与 profiles 同组（wantProfiles 顺带查），
    // 脏标记同刷——左栏区块 / 详情行 / 表单绑定下拉共用这一份快照
    std::vector<SharedConfig> sharedConfigs;
    std::string sharedConfigsError;   // 空 = 成功（失败不置脏，保旧快照 + 记日志）
};

// ============ 完整调用记录弹窗的后台查询结果包 ============
// 值语义：经 future 回渲染线程，工作线程不触碰 this。
struct CallsPage {
    std::vector<ClaudeService::CallLogEntry> rows;  // 当页行
    int total = 0;                                  // 满足过滤的总条数
    std::string error;                              // 空串 = 成功
};

// ============ 左栏：Profile 列表 + header/引导条 + 数据缓存 ============
struct ProfileListState {
    // 审计 perf [13]：compose 数据缓存（刷新语义见 refreshDataCache 注释）。
    // 初值脏 = 首帧 compose 必查；seenCallLogVersion_ 初值 ~0 保证 recent
    // 首帧必查（真实版本号从 0 起，不会撞上）。
    std::vector<Profile> cachedProfiles_;
    std::string cachedProfilesError_;
    std::vector<ClaudeService::CallLogEntry> cachedRecent_;
    std::string cachedCallsError_;
    Profile cachedActive_;
    bool cachedHasActive_ = false;
    bool dataDirty_ = true;
    bool dataLoaded_ = false;   // 快照是否装载过（冷启动兜底判据，见 refreshDataCache）
    uint64_t seenCallLogVersion_ = ~0ull;
    // 审计 H2：后台刷新在途 future（工作线程不触碰 this，结果经值回渲染线程；
    // 成员析构 = future 析构，自动等本轮查询结束 → 无悬垂）
    std::future<DataRefresh> refreshFuture_;
    std::chrono::steady_clock::time_point lastRefreshPost_{};  // 节流窗口起点
    // 刷新节流窗口（原 static constexpr 成员；static 成员随 struct 走，
    // 只声明在状态旁边，不占实例字节）
    static constexpr int64_t kRefreshIntervalMs = 200;

    // 「最近调用」页签的**数据侧**上限（2026-09-28 组 B P2）。
    // 注意语义：这**不是**显示上限——显示行数由详情面板按内容框可用高推
    //（`(内容框底 − 表头 − 查看全部带)/24`），超出部分用「另有 N 条」表达。
    // 旧值 3 是「面板底部固定预览块」（该块恒画 3 行）时代的遗留；「最近调用」
    // 抽成整页页签后（2175c24）它变成了第二个隐形上限：页签可容 13 行，数据
    // 却只给 3 行 —— 只修行数预算不修这里，「显示不全」依旧。取 30 覆盖
    // 常规窗口的页签预算（页高 1000 → 约 22 行）；再高的窗口靠「另有 N 条」
    //（相对快照条数）+「查看全部」弹窗拿全量。
    static constexpr int kRecentCallsSnapshot = 30;

    // 选中态（0 = 跟随激活 profile）
    int64_t selectedId_ = 0;
    // Profile 列表滚动位置（审计 [26]：卡片超界可滚，不再压底部按钮）
    float profilesScrollOffset_ = 0.0f;
    // 共享配置片段卡片区的滚动位置（2026-09-28 组 B P4）：片段区块整体**钉面板
    // 底**，卡片装不下时在区块内部滚（而不是把「+ 新建片段」推出去/盖掉）——
    // 卡片全部可达可编辑，按钮恒可见可点
    float sharedScrollOffset_ = 0.0f;
    // 上一帧的片段集签名（审计 F-09 补的复位路径）：签名（片段集 / 空态 / 错误态）
    // 一变就把 sharedScrollOffset_ 归零。初值故意取一个不可能撞上的值 → 首帧即
    // 归零一次（首帧偏移本来就是 0，无行为变化）。
    size_t sharedScrollKey_ = static_cast<size_t>(-1);
    // 引导条一次性标记（§6.4：会话内代理从未启动过才显示；内存态不持久化）
    bool proxyEverSeen_ = false;
    // 列表右键菜单（UI 现代化 M1-5）：目标 profile id（0 = 无）+ 锚点
    //（页面坐标，经 core::menuAnchorForPointer 统一换算）+ 开合标志
    int64_t ctxProfileId_ = 0;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    bool ctxMenuOpen_ = false;

    // 批 2（设计 §2.10）：共享配置片段快照（与 profiles 同组脏标记同刷，
    // 见 DataRefresh.sharedConfigs）——左栏「共享配置」区块 / 详情行 /
    // 表单绑定下拉共用这一份，渲染路径零查询
    std::vector<SharedConfig> cachedSharedConfigs_;
    std::string cachedSharedConfigsError_;   // 空 = 成功（失败保旧快照 + 记日志）
};

// ============ 右栏：详情面板页签（三 pane + 最近调用）============
struct DetailState {
    // 详情页签：0 基础 / 1 模型映射 / 2 环境变量 / 3 最近调用
    //（2026-09-24 起 0-2；3 = 2026-09-28 由底部固定预览块抽出的独立页签）
    int detailTab_ = 0;
};

// ============ 新建/编辑表单 + 删除确认弹窗 ============
struct FormState {
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础 / 1 模型映射 / 2 高级 env
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    std::string formImportWarn_;  // 导入当前配置的接管态警告（弹窗内展示，不阻断）
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
    ModelsConfig formModels_;  // 槽位表全行（默认模型 = def 槽位，旧 formModel_ 并入）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    // 删除确认弹窗
    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 批 2（设计 §2.10）：共享配置片段绑定（高级 env 页顶部下拉 +
    // 片段赢预览/新增同键拦截判定）
    int64_t formSharedConfigId_ = 0;   // 表单态绑定（0 = 未绑定；保存时写入 profile）
    bool formSharedDdOpen_ = false;    // 绑定下拉展开态
    // 当前绑定片段 K 键集（sharedFrozenKeys，别名归一后，与写进 live 的键形
    // 一致；悬空绑定 → 归一为空）——K 键行置灰只读 + 新增同键拦截的判定源
    std::vector<std::string> formFrozenKeys_;
    // 打开表单时该 profile 自有 env 的键（归一后）：「新增同键」拦截只拦
    // **打开后新加的**行（存量键非破坏保留、冻结显示，解绑后恢复生效，
    // 设计 §2.3）
    std::vector<std::string> formOriginalOwnKeys_;

    // 批 3（设计 §3.4/§3.8）：备用 Profile（failover）绑定（基础页下拉，
    // 复用批 2 绑定下拉模式——索引 0 = 无，其余 = 候选 profile 表序 +1，
    // 编辑态排除自身；保存时写入 profile.fallbackProfileId）
    int64_t formFallbackProfileId_ = 0;   // 表单态备用（0 = 不启用 failover）
    bool formFallbackDdOpen_ = false;     // 备用下拉展开态
};

// ============ 共享配置片段编辑弹窗（批 2，设计 §2.10 待拍板②：独立状态）============
// 复用处：键值行写入模式照 formEnvRows_（kMaxVisibleRows 同款）；弹窗形态照表单
// 弹窗（DialogFrame）。**必须接入 claude_module 的 anyDialogOpen 门控与
// closeTopmostDialog Esc 序链（入序链最顶层）**——「formOpen_ 单弹窗天然互斥」
// 只是惯例、非结构性约束。
struct SharedConfigDialogState {
    bool open_ = false;
    int64_t editId_ = 0;             // 0 = 新建，非 0 = 编辑该 id
    std::string name_;               // 片段名（name UNIQUE，服务层校验）
    std::vector<std::pair<std::string, std::string>> envRows_;  // 键值行
    std::string error_;              // 校验/保存错误（红字）

    // 删除确认弹窗（删除 = 同事务解绑绑定它的 profile，设计 §2.6）
    bool deleteConfirmOpen_ = false;
    int64_t deleteConfirmId_ = 0;
    std::string deleteConfirmName_;
};

// ============ cc-switch / settings.json 粘贴导入（高级 env 页）============
struct ImportState {
    std::string importText_;       // 多行粘贴框原文
    bool importReady_ = false;     // 解析成功待应用
    std::string importError_;      // 解析失败信息（红字）
    ImportedConfig importPreview_; // 解析结果预览
};

// ============ 完整调用记录弹窗（审计 [11]：搜索/状态码过滤/翻页共用）============
struct LogsState {
    bool callsDialogOpen_ = false;
    std::vector<ClaudeService::CallLogEntry> allCalls_;  // 快照：当页行（渲染只读）
    std::string callsSearch_;        // 搜索框文本（path/method 模糊）
    int callsStatusIdx_ = 0;         // 状态下拉索引（→ kCallsStatusClasses）
    bool callsStatusDdOpen_ = false; // 状态下拉展开态
    int callsPage_ = 0;              // 当前页（0-based）
    int callsTotalMatches_ = 0;      // 快照：满足过滤的总条数（countCalls）
    float callsScrollOffset_ = 0.0f; // 列表区滚动位置（翻页时归零）
    // 审计 CPX-06：调用记录取数的 future 刷新态（语义见 refreshCallsCache）。
    // 初值脏 = 首次打开弹窗下一帧必查
    bool callsPageDirty_ = true;     // 置脏 → 下一帧（弹窗打开时）发一次读意图
    bool callsPageLoaded_ = false;   // 快照是否装载过（未装载显示「加载中…」）
    std::chrono::steady_clock::time_point callsEarliestQueryAt_{};  // 最早派发时刻
    // 后台查询在途 future（工作线程不触碰 this，结果经值回渲染线程；成员析构
    // = future 析构，自动等本轮查询结束 → 无悬垂）
    std::future<CallsPage> callsFuture_;
};

// ============ 容器：模块唯一的 UI 状态成员（compose 内一律 state_.xxx.yyy_）============
struct ClaudeUiState {
    ProfileListState list;
    DetailState detail;
    FormState form;
    SharedConfigDialogState shared;   // 批 2：共享配置片段编辑弹窗（独立状态）
    ImportState import;
    LogsState logs;
};

// ============ 数据缓存的后台刷新（审计 perf [13] + H2）============
// 语义：compose 每帧只做两件无 IO 的事——搬已就绪结果进快照 + 发下一次刷新
// 意图；SQLite 查询全部在后台线程（原实现在渲染线程的 compose 里同步查，与
// 代理 worker 的 logCall INSERT 争同一把 recursive_mutex，代理忙时整帧被卡）。
// 脏标记（onStart/onPageEnter/写操作）触发三类重查；recent 另按
// ClaudeService::callLogVersion 感知代理 worker 异步落库（刷新最多晚一个
// 200ms 节流 tick + 1 帧，不丢数据）；查询失败保持脏、按节流窗口重试。
//
// 这三个函数不触碰模块 this（只吃状态引用 + 服务指针），故与状态同住一个单元。

// 唯一跑在后台线程的 DB 入口（static 语义：不碰模块成员，可安全脱离渲染线程）
DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles, bool wantRecent);

// 落快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
void applyDataRefresh(ProfileListState& st, DataRefresh r);

// 收割：后台结果就绪才搬（wait_for(0) 不阻塞；未就绪直接跳过，快照沿用
// 上次值——不丢不改，无空帧）
void harvestDataRefresh(ProfileListState& st);

// 审计 H2：节流窗口。版本轮询在代理高频落库时每帧都能看到新版本，不节流
// 会退化成每帧一个后台查询（仍与 logCall INSERT 抢同一把 recursive_mutex）；
// 失败重试同窗口（脏标记不清 → 按窗口重发，不按帧起线程）
bool refreshDue(const ProfileListState& st);

// compose 每帧调：只发意图 + 搬快照，零 IO（语义同旧 refreshDataCache：
// 脏 = 进页/写操作后的重查；recent 另在 callLogVersion 变化时重查）
void refreshDataCache(ProfileListState& st, ClaudeService* svc);

} // namespace module::claude
