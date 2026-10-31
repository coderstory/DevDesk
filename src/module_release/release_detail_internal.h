// release_detail_internal.h
// release_detail_ui.cpp 三拆后的模块内部契约（2026-10-31）：度量常量、页面局部
// 类型、跨 TU 的状态查询 / 动作 / 绘制函数声明。只给 module_release 内部用，
// 不是对外 API（对外只有 release_detail_ui.h 的 composeReleaseDetailPage）。
//
// 反重构警告以 release_detail_ui.cpp 文件头为准：工单卡口径、五项配置槽位、
// 渲染帧禁写库、状态推进时机四条，拆分不改这四条语义。
#pragma once

#include "release_detail_ui.h"

#include "release_logic.h"      // 进度 / 完成判定 / 时间基准：唯一业务判据
#include "release_store.h"

#include <core/theme.h>         // Tokens：声明里到处用
#include <core/time.h>

#include <components/theme.h>   // ThemeColorTokens：声明里的绘制参数
#include <eui/types.h>          // eui::Ui / Screen：compose 与绘制签名

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 只放「声明闭包」需要的东西。绘制用的 components/* / ui_kit/* / core/render/text.h
// 由 release_detail_draw.cpp 自己 include —— 放这里会让 logic / ui 两个 TU 白解析
// 七百毫秒（2026-10-31 实测：全量头 1.72s vs 本闭包 1.01s）。
#include <utility>
#include <vector>

namespace module::release {
namespace detail {

using core::theme::Tokens;

inline constexpr float kSectionTitleH = 24.0f;
inline constexpr float kSectionGap = 12.0f;
inline constexpr float kPrereqGap = 16.0f;
inline constexpr float kEnvHeaderH = 56.0f;
inline constexpr float kEnvInset = 16.0f;
inline constexpr float kServiceGap = 12.0f;
inline constexpr float kServiceInset = 16.0f;
inline constexpr float kServiceRowStart = 56.0f;
inline constexpr float kStepRowH = 44.0f;   // 池 53 设计档：步骤/表单行 44（原 40）
inline constexpr float kStepRowGap = 4.0f;
inline constexpr float kStepDot = 18.0f;
inline constexpr float kCheckSize = 18.0f;
inline constexpr float kViewButtonW = 56.0f;
inline constexpr float kChipH = 22.0f;
inline constexpr float kNoticeH = 34.0f;
inline constexpr float kDragHandleW = 18.0f;
inline constexpr float kDragHandleH = 22.0f;
inline constexpr float kDragHandleGap = 6.0f;
inline constexpr float kDragLineH = 2.0f;
// 流程轨道（发布态）：行 44 + 行距 4；组头 36（含上 16 下 8 的间距预算）；
// 服务子标 22。轨道前缀和滚动定位与 drawPublishTrack 共用这四档，禁止另写。
inline constexpr float kTrackRowH = 44.0f;
inline constexpr float kTrackRowGap = 4.0f;
inline constexpr float kTrackGroupH = 36.0f;
inline constexpr float kTrackServiceH = 22.0f;
inline constexpr float kFormLabelW = 96.0f;

struct TicketNeeds {
    bool nacos = false;
    bool db = false;
};

struct ChipColors {
    eui::Color bg;
    eui::Color fg;
};

enum class PrereqKey { Pretest, Cab, Nacos, Db, Announce };

struct PrereqView {
    PrereqKey key = PrereqKey::Pretest;
    std::string title;
    std::string checkLabel;
    bool checked = false;
    std::string value;
    bool gateOpen = true;
    std::string dueText;
    bool overdue = false;
};

struct StepRowView {
    std::string id;
    std::string label;
    std::string statusText;  // 行尾状态字：未完成 / 已填 n 字 / 已跳过 / 内容为空 / 已完成
    bool done = false;
    bool skipped = false;
    bool timeReady = true;
    bool invalid = false;    // 提交校验未通过：整行浅红底 + 标签红字
    bool dragging = false;   // 该行正被拖拽：盖一层半透明 veil
    bool config = false;
    ConfigKind configKind = ConfigKind::Nacos;
    StepKind stepKind = StepKind::MergeBranch;
    std::size_t configIndex = 0;
    std::size_t configCount = 0;
};

// ==================== 发布态流程轨道（F-04 / F-14） ====================

// 节点四态（软引导的视觉语言）：当前步唯一绿染；未解锁锁定可见不可操作。
enum class TrackNodeState { Current, UnlockedTodo, Locked, Done, Skipped };
enum class TrackRowKind { Step, EnvGroupHeader, ServiceLabel };

struct TrackRowView {
    TrackRowKind kind = TrackRowKind::Step;
    std::string groupId;        // 折叠键（"prereq" / "env.<id>"）
    std::string title;          // 组头：环境名（含时间窗由 draw 拼）
    std::string progressText;   // 组头进度 "3/8"
    bool groupDone = false;     // 组全部完成 → 默认收起
    bool groupOverdue = false;  // 环境超时（组头 chip）
    // 步骤行
    TrackNodeState node = TrackNodeState::UnlockedTodo;
    std::string label;
    std::string statusText;     // 行尾状态字 / 解锁条件 / 门禁提示
    std::string valueText;      // 前置行只读值（单号 / 公告摘要）
    bool done = false;          // 前置已勾 / 步骤已完成（节点态的布尔镜像）
    bool config = false;        // 配置行（撤回走 openRevokeConfig）
    bool confirmable = false;   // 行尾「确认」钮 / 整行可点
    bool revocable = false;     // 已完成行点击 → 撤回确认
    bool hidden = false;        // 所在组已折叠：绘制与滚动前缀和都跳过
    // 动作回调定位依据
    PrereqKey prereqKey = PrereqKey::Pretest;
    bool isAnnounce = false;
    std::int64_t envId = 0;
    std::int64_t serviceId = 0;
    StepKind stepKind = StepKind::MergeBranch;
    ConfigKind configKind = ConfigKind::Nacos;
    float height = kTrackRowH;  // 前缀和滚动定位用；与 draw 侧逐档一致
};

bool isReadOnly(TaskStatus status);
bool isEditMode(TaskStatus status);
bool isPublishMode(TaskStatus status);
std::string trimmed(const std::string& value);
std::size_t utf8CharCount(const std::string& text);
bool parseTaskId(const std::string& text, std::int64_t* out);
int planMinuteNow(const std::string& planDate);
std::string formatPlanMinute(int minute);
int envEndMinuteAdjusted(const TaskEnv& env);
int configKindIndex(ConfigKind kind);
const std::string& configLabel(ConfigKind kind);
const char* stepLabel(StepKind kind);
const TaskService* findService(const ReleaseDetailUiState& state, std::int64_t id);
const TaskEnv* findEnv(const ReleaseDetailUiState& state, std::int64_t id);
const TaskConfig* findConfig(const ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind);
bool stepDone(const ReleaseDetailUiState& state, std::int64_t serviceId, StepKind kind);
std::vector<std::int64_t> parseServiceIdOrder(const std::string& note);
std::vector<std::int64_t> lastReorderOrder(const ReleaseDetailUiState& state, const std::string& envName);
std::vector<const TaskService*> orderedEnvServices(const ReleaseDetailUiState& state, std::int64_t envId, const std::string& envName, bool includeRemoved);
std::vector<const TaskService*> liveServices(const ReleaseDetailUiState& state, const TaskEnv& env);
std::size_t countLiveServices(const ReleaseDetailUiState& state, const TaskEnv& env);
TicketNeeds scanTicketNeeds(const ReleaseDetailUiState& state);
std::vector<TaskConfig> orderedConfigs(const ReleaseDetailUiState& state, const TaskService& svc);
std::string joinMissing(const std::vector<std::string>& parts);
std::vector<std::string> configGateMissing(const Task& task, ConfigKind kind);
std::vector<std::string> deployGateMissing(const Task& task, const TicketNeeds& needs);
bool prereqFlagged(const ReleaseDetailUiState& state, PrereqKey key);
void setNotice(ReleaseDetailUiState& state, std::string text, bool isError);
void saveTask(ReleaseStore& store, ReleaseDetailUiState& state);
bool appendActionLog(ReleaseStore& store, ReleaseDetailUiState& state, const std::string& scope, const std::string& step, const char* action, const std::string& note);
bool writePrereqFlag(ReleaseStore& store, ReleaseDetailUiState& state, const std::string& code, bool value);
bool markPrereqCompleted(ReleaseStore& store, ReleaseDetailUiState& state, const std::string& code, const std::string& label);
bool markAnnouncementSent(ReleaseStore& store, ReleaseDetailUiState& state);
bool ensureServiceConfigs(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId);
bool markStepCompleted(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId, StepKind kind, const TaskEnv& env, int nowMinute);
bool markConfigCompleted(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind, const TaskEnv& env, int nowMinute);
void moveConfig(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind, int delta);
std::vector<TaskConfig> configsForValidation(const ReleaseDetailUiState& state);
bool serviceBranchFlagged(const ReleaseDetailUiState& state, const std::string& name);
bool serviceConfigFlagged(const ReleaseDetailUiState& state, const std::string& name, ConfigKind kind);
void submitTask(ReleaseStore& store, ReleaseDetailUiState& state);
void applyWithdrawSubmission(ReleaseStore& store, ReleaseDetailUiState& state);
int validationItemCount(const ReleaseDetailUiState& state);
void runValidation(ReleaseDetailUiState& state);
void persistServiceOrder(ReleaseStore& store, ReleaseDetailUiState& state, const std::string& envName, const std::vector<std::int64_t>& ids);
void updateServiceDragTarget(ReleaseDetailUiState& state);
void updateConfigDragTarget(ReleaseDetailUiState& state);
void finishServiceDrag(ReleaseStore& store, ReleaseDetailUiState& state, bool released);
void finishConfigDrag(ReleaseStore& store, ReleaseDetailUiState& state, bool released);
TaskStatus computeDisplayStatus(const ReleaseDetailUiState& state, int nowMinute);
void advanceTaskStatus(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute);
TaskStatus restoredStatus(const ReleaseDetailUiState& state, int nowMinute);
void applyVoid(ReleaseStore& store, ReleaseDetailUiState& state);
void applyRestore(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute);
void applyRevoke(ReleaseStore& store, ReleaseDetailUiState& state);
void openRevokePrereq(ReleaseDetailUiState& state, const std::string& code, const std::string& label, bool announce);
void openRevokeStep(ReleaseDetailUiState& state, std::int64_t serviceId, StepKind kind, const std::string& scope);
void openRevokeConfig(ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind, const std::string& scope);
void toggleConfigSkip(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind);
void openConfigEditor(ReleaseStore& store, ReleaseDetailUiState& state, std::int64_t serviceId, ConfigKind kind);
void saveConfigEditor(ReleaseStore& store, ReleaseDetailUiState& state);
void beginBranchEdit(ReleaseDetailUiState& state, std::int64_t serviceId, const std::string& value);
void applySaveBranch(ReleaseStore& store, ReleaseDetailUiState& state);
std::string isIllegalBranchName(const std::string& trimmedValue);
void openRemoveService(ReleaseDetailUiState& state, std::int64_t serviceId, const std::string& name);
void openRemoveEnv(ReleaseDetailUiState& state, std::int64_t envId, const std::string& name);
void applyRemoveService(ReleaseStore& store, ReleaseDetailUiState& state);
void applyRemoveEnv(ReleaseStore& store, ReleaseDetailUiState& state);
void performNextStep(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute);
std::vector<TrackRowView> collectTrackRows(const ReleaseDetailUiState& state, int nowMinute);
float trackCurrentStepOffset(const std::vector<TrackRowView>& rows);
float measureText(const std::string& text, float fontSize, int weight);
float chipWidth(const std::string& text, float fontSize);
ChipColors statusChipColors(TaskStatus status, const Tokens& t);
float drawChip(eui::Ui& ui, const std::string& id, float x, float y, const std::string& text, const eui::Color& bg, const eui::Color& fg, const Tokens& t);
void drawCardBackground(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, bool subtle);
void drawSectionTitle(eui::Ui& ui, const std::string& id, float x, float y, const std::string& text, const Tokens& t);
void drawDragHandle(eui::Ui& ui, const std::string& id, float x, float y, float width, float height, const Tokens& t, const std::function<void(const eui::PointerEvent&, const eui::Rect&)>& onPress, const std::function<void(const core::dsl::DragEvent&)>& onDrag, const std::function<void(const eui::PointerEvent&, const eui::Rect&)>& onRelease);
void drawStepDot(eui::Ui& ui, const std::string& id, float x, float y, bool done, bool skipped, const Tokens& t);
void drawReadOnlyValue(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, const std::string& value, const std::string& placeholder, bool multiline, const Tokens& t);
const char* prereqTitle(PrereqKey key);
const char* prereqCode(PrereqKey key);
const char* prereqCheckLabel(PrereqKey key);
std::string prereqValue(const ReleaseDetailUiState& state, PrereqKey key);
void setPrereqValue(ReleaseDetailUiState& state, PrereqKey key, const std::string& value);
bool prereqChecked(const ReleaseDetailUiState& state, PrereqKey key);
bool prereqGateOpen(const ReleaseDetailUiState& state, PrereqKey key);
bool containsAsciiCaseInsensitive(const std::string& haystack, const std::string& needle);
void drawPrereqSection(eui::Ui& ui, float width, ReleaseDetailUiState& state, bool readOnly, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, ReleaseStore& store);
float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc);
float envCardHeight(const ReleaseDetailUiState& state, const TaskEnv& env);
float envSectionHeight(const ReleaseDetailUiState& state, float width);
void drawStepRow(eui::Ui& ui, const std::string& id, float x, float y, float width, const StepRowView& row, bool current, bool readOnly, bool configurable, bool readOnlyViewAllowed, bool menuOpen, const Tokens& t, const std::function<void()>& onRowClick, const std::function<void()>& onView, const std::function<void()>& onToggleMenu);
void drawServiceCard(eui::Ui& ui, const std::string& id, float x, float y, float width, const TaskEnv& env, const TaskService& svc, ReleaseDetailUiState& state, bool readOnly, bool configurable, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, ReleaseStore& store);
void drawEnvCard(eui::Ui& ui, const std::string& id, float x, float y, float width, const TaskEnv& env, ReleaseDetailUiState& state, bool readOnly, bool configurable, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, ReleaseStore& store);
void drawEnvSection(eui::Ui& ui, float width, ReleaseDetailUiState& state, bool readOnly, bool configurable, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, ReleaseStore& store, const ReleaseDetailActions& actions);
void drawPageHeader(eui::Ui& ui, float width, const Tokens& t, ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute, const ReleaseDetailActions& actions);
float drawModeBadge(eui::Ui& ui, const std::string& id, float x, float y, TaskStatus status, const Tokens& t);
void drawPublishTrack(eui::Ui& ui, float width, ReleaseDetailUiState& state, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens, ReleaseStore& store);
void drawNotice(eui::Ui& ui, float width, const ReleaseDetailUiState& state, const Tokens& t);
void drawValidationDialog(eui::Ui& ui, const eui::Screen& screen, ReleaseDetailUiState& state, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens);
void drawConfigEditor(eui::Ui& ui, const eui::Screen& screen, ReleaseDetailUiState& state, ReleaseStore& store, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens);
void drawConfirmDialogs(eui::Ui& ui, const eui::Screen& screen, ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute, const Tokens& t, const components::theme::ThemeColorTokens& themeTokens);
void applyContentRevocations(ReleaseStore& store, ReleaseDetailUiState& state, const std::vector<TaskService>& previousServices, const std::vector<TaskConfig>& previousConfigs);
void loadDetail(ReleaseStore& store, ReleaseDetailUiState& state);
void drawErrorPage(eui::Ui& ui, const eui::Screen& screen, const ReleaseDetailUiState& state, const Tokens& t);

}  // namespace detail
}  // namespace module::release
