// 任务执行页实现（票据 06 / 07 / 08 / 10）：任务级前置卡、环境与服务执行清单、
// 撤回、任务状态（作废 / 恢复）。
//
// 反重构警告：
//   - 工单卡适用性与工单号门槛必须与 release_logic 的 ticketNeeds 同口径
//     （未移除环境下的未移除服务、未跳过且内容非空）。改一边不改另一边会出现
//     「提醒有工单、执行页没卡」的反向错位。
//   - 五项配置以固定 kind 为准；库里缺行时只在用户交互（查看 / 勾选 / 排序）时补建
//     数据库行。渲染 / 校验的占位行由 orderedConfigs 按期望配置槽位在返回值里补齐
//     （不写库、不改 state.configs；进度与完成判定按期望槽位口径，缺行天然按未完成计）。
//     渲染帧禁止常规写库：每帧写库会让输入卡顿，并和滚动回调节流互相踩踏；唯一例外是
//     advanceTaskStatus 的一次性状态推进（仅当推导状态与库中状态不同才写）。
//   - 状态推进只能在数据加载完成（dirty=false）后执行，且 Draft 不得自动写 Pending
//     ——只有本页「提交」动作（validateTask 通过后）显式推进，提前勾前置不算执行中。
#include "release_detail_ui.h"

#include "release_checkbox.h"
#include "release_config_ui.h"  // configItemLabels / formatMinuteOfDay / formatTimeWindow
#include "release_list_ui.h"    // releaseTaskStatusText / releaseWeekdayLabel
#include "release_logic.h"      // 进度 / 完成判定 / 时间基准：唯一业务判据
#include "release_store.h"

#include <core/component_theme.h>
#include <core/render/text.h>
#include <core/theme.h>
#include <core/time.h>

#include <components/dialog.h>
#include <components/input.h>
#include <components/progress.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <eui/types.h>

#include <ui_kit/button.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace module::release {

namespace {

using core::theme::Tokens;

constexpr float kSectionTitleH = 24.0f;
constexpr float kSectionGap = 12.0f;
constexpr float kPrereqCardH = 160.0f;
constexpr float kPrereqCardMinW = 220.0f;
constexpr float kPrereqGap = 16.0f;
constexpr float kEnvHeaderH = 56.0f;
constexpr float kEnvInset = 16.0f;
constexpr float kServiceGap = 12.0f;
constexpr float kServiceInset = 16.0f;
constexpr float kServiceRowStart = 56.0f;
constexpr float kStepRowH = 40.0f;
constexpr float kStepRowGap = 4.0f;
constexpr float kStepDot = 18.0f;
constexpr float kCheckSize = 18.0f;
constexpr float kViewButtonW = 56.0f;
constexpr float kArrowW = 22.0f;
constexpr float kArrowGap = 4.0f;
constexpr float kChipH = 22.0f;
constexpr float kNoticeH = 34.0f;
constexpr float kDragHandleW = 18.0f;
constexpr float kDragHandleH = 22.0f;
constexpr float kDragHandleGap = 6.0f;
constexpr float kDragLineH = 2.0f;

bool isReadOnly(TaskStatus status) {
    return status == TaskStatus::Done || status == TaskStatus::Voided;
}

bool isConfigPhase(TaskStatus status) {
    return status == TaskStatus::Draft || status == TaskStatus::Pending;
}

std::string trimmed(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                           value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// UTF-8 字符数（编辑层字数统计）：非续字节（0b10xxxxxx）计一个字符。
std::size_t utf8CharCount(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0u) != 0x80u) ++count;
    }
    return count;
}

bool parseTaskId(const std::string& text, std::int64_t* out) {
    if (text.empty() || out == nullptr) return false;
    std::int64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const std::int64_t digit = c - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == 0) return false;
    *out = value;
    return true;
}

// 计划日零点起的分钟数；跨零点继续累计。换算委托 release_logic::planDateElapsedMinutes
// （全仓库单一口径：本地时区零点起算，DST 日不偏移；禁「日历日差 × 1440」）。
// 日期非法 / 本地时间不可表示返回 -1，所有基于时钟的判定必须先排除 -1。
int planMinuteNow(const std::string& planDate) {
    std::int64_t minute = 0;
    if (!planDateElapsedMinutes(planDate, core::time::nowUnix(), minute)) return -1;
    if (minute < std::numeric_limits<int>::min() ||
        minute > std::numeric_limits<int>::max()) {
        return -1;
    }
    return static_cast<int>(minute);
}

std::string formatPlanMinute(int minute) {
    int normalized = minute % 1440;
    if (normalized < 0) normalized += 1440;
    std::string prefix;
    if (minute < 0) {
        prefix = "前一日 ";
    } else if (minute >= 1440) {
        prefix = "次日 ";
    }
    return prefix + formatMinuteOfDay(normalized);
}

int envEndMinuteAdjusted(const TaskEnv& env) {
    return env.endMinute <= env.startMinute ? env.endMinute + 1440 : env.endMinute;
}

int configKindIndex(ConfigKind kind) {
    switch (kind) {
        case ConfigKind::Nacos: return 0;
        case ConfigKind::Db: return 1;
        case ConfigKind::Cron: return 2;
        case ConfigKind::Menu: return 3;
        case ConfigKind::Gateway: return 4;
    }
    return 0;
}

const std::string& configLabel(ConfigKind kind) {
    static const std::string kFallback = "配置";
    const auto& labels = configItemLabels(ServiceKind::Backend);
    const int index = configKindIndex(kind);
    if (index < 0 || static_cast<std::size_t>(index) >= labels.size()) return kFallback;
    return labels[static_cast<std::size_t>(index)];
}

const char* stepLabel(StepKind kind) {
    switch (kind) {
        case StepKind::MergeBranch: return "合并分支";
        case StepKind::Config: return "配置";
        case StepKind::Deploy: return "部署";
        case StepKind::Verify: return "验证完成";
    }
    return "步骤";
}

const TaskService* findService(const ReleaseDetailUiState& state, std::int64_t id) {
    for (const auto& svc : state.services) {
        if (svc.id == id) return &svc;
    }
    return nullptr;
}

const TaskEnv* findEnv(const ReleaseDetailUiState& state, std::int64_t id) {
    for (const auto& env : state.envs) {
        if (env.id == id) return &env;
    }
    return nullptr;
}

const TaskConfig* findConfig(const ReleaseDetailUiState& state, std::int64_t serviceId,
                             ConfigKind kind) {
    for (const auto& cfg : state.configs) {
        if (cfg.taskServiceId == serviceId && cfg.kind == kind) return &cfg;
    }
    return nullptr;
}

bool stepDone(const ReleaseDetailUiState& state, std::int64_t serviceId, StepKind kind) {
    for (const auto& step : state.steps) {
        if (step.taskServiceId == serviceId && step.kind == kind && step.doneAt != 0) {
            return true;
        }
    }
    return false;
}

// 服务顺序还原：服务没有独立 sort 列，唯一持久化载体是 TaskLog
//（scope=环境名 / step=reorder / note=逗号分隔的服务 id 序列）。
// 反重构警告：不要为了「更干净」给 TaskService 加 sort 字段——store / types 是
// 共享契约，改动会波及库表与其它页面；这里只消费既有日志。
std::vector<std::int64_t> parseServiceIdOrder(const std::string& note) {
    std::vector<std::int64_t> out;
    std::size_t i = 0;
    while (i < note.size()) {
        while (i < note.size() && (note[i] == ',' || note[i] == ' ' || note[i] == '\t')) {
            ++i;
        }
        std::int64_t value = 0;
        bool sawDigit = false;
        bool overflow = false;
        while (i < note.size() && note[i] >= '0' && note[i] <= '9') {
            sawDigit = true;
            const std::int64_t digit = note[i] - '0';
            if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
                overflow = true;
                break;
            }
            value = value * 10 + digit;
            ++i;
        }
        while (i < note.size() && note[i] != ',') ++i;
        if (i < note.size()) ++i;
        if (sawDigit && !overflow && value > 0) out.push_back(value);
    }
    return out;
}

std::vector<std::int64_t> lastReorderOrder(const ReleaseDetailUiState& state,
                                           const std::string& envName) {
    for (auto it = state.logs.rbegin(); it != state.logs.rend(); ++it) {
        if (it->step != "reorder" || it->scope != envName) continue;
        return parseServiceIdOrder(it->note);
    }
    return {};
}

// 环境内服务（按最后一条 reorder 日志还原，未命中的服务保持读取顺序追加在后）。
// includeRemoved = false 供渲染 / 进度；true 供持久化（保留已移除服务的位置）。
std::vector<const TaskService*> orderedEnvServices(const ReleaseDetailUiState& state,
                                                   std::int64_t envId,
                                                   const std::string& envName,
                                                   bool includeRemoved) {
    std::vector<const TaskService*> all;
    for (const auto& svc : state.services) {
        if (svc.taskEnvId != envId) continue;
        if (!includeRemoved && svc.removed) continue;
        all.push_back(&svc);
    }
    const std::vector<std::int64_t> order = lastReorderOrder(state, envName);
    if (order.empty()) return all;

    std::vector<const TaskService*> out;
    out.reserve(all.size());
    std::vector<bool> used(all.size(), false);
    for (const std::int64_t id : order) {
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (used[i] || all[i]->id != id) continue;
            out.push_back(all[i]);
            used[i] = true;
            break;
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!used[i]) out.push_back(all[i]);
    }
    return out;
}

std::vector<const TaskService*> liveServices(const ReleaseDetailUiState& state,
                                             const TaskEnv& env) {
    return orderedEnvServices(state, env.id, env.envName, false);
}

std::size_t countLiveServices(const ReleaseDetailUiState& state, const TaskEnv& env) {
    return liveServices(state, env).size();
}

// 工单适用性：只有「未移除环境下的未移除服务」填了对应类型的非跳过内容才欠工单。
// 与 release_logic 内部 ticketNeeds 同口径（见文件头反重构警告）。
struct TicketNeeds {
    bool nacos = false;
    bool db = false;
};

TicketNeeds scanTicketNeeds(const ReleaseDetailUiState& state) {
    TicketNeeds needs;
    for (const auto& svc : state.services) {
        if (svc.removed) continue;
        const TaskEnv* env = findEnv(state, svc.taskEnvId);
        if (env == nullptr || env->removed) continue;
        for (const auto& cfg : state.configs) {
            if (cfg.taskServiceId != svc.id) continue;
            if (cfg.skipped || cfg.content.empty()) continue;
            if (cfg.kind == ConfigKind::Nacos) needs.nacos = true;
            if (cfg.kind == ConfigKind::Db) needs.db = true;
        }
    }
    return needs;
}

// 显示用配置序列：以服务类型的期望配置槽位为口径（Backend 固定 5 项）。库里
// 缺行时在此按期望槽位补 id=0 的占位行——只返回副本，不写库、不改 state.configs；
// 渲染 / 提交校验 / 进度全部消费同一份清单，占位行在首次交互时由
// ensureServiceConfigs 落库。
std::vector<TaskConfig> orderedConfigs(const ReleaseDetailUiState& state,
                                       const TaskService& svc) {
    std::vector<TaskConfig> out;
    if (svc.kind != ServiceKind::Backend) return out;
    for (const auto& cfg : state.configs) {
        if (cfg.taskServiceId == svc.id) out.push_back(cfg);
    }
    std::sort(out.begin(), out.end(), [](const TaskConfig& a, const TaskConfig& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.id < b.id;
    });
    bool present[5] = {false, false, false, false, false};
    int maxOrder = -1;
    for (const auto& cfg : out) {
        present[configKindIndex(cfg.kind)] = true;
        maxOrder = std::max(maxOrder, cfg.order);
    }
    static const ConfigKind kKinds[5] = {ConfigKind::Nacos, ConfigKind::Db, ConfigKind::Cron,
                                         ConfigKind::Menu, ConfigKind::Gateway};
    for (int i = 0; i < 5; ++i) {
        if (present[i]) continue;
        TaskConfig cfg;
        cfg.taskServiceId = svc.id;
        cfg.kind = kKinds[i];
        cfg.order = ++maxOrder;
        out.push_back(std::move(cfg));
    }
    return out;
}

std::string joinMissing(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += "、";
        out += parts[i];
    }
    return out;
}

std::vector<std::string> configGateMissing(const Task& task, ConfigKind kind) {
    std::vector<std::string> missing;
    if (kind == ConfigKind::Nacos && trimmed(task.ticketNacosNo).empty()) {
        missing.push_back("nacos 工单号");
    }
    if (kind == ConfigKind::Db && trimmed(task.ticketDbNo).empty()) {
        missing.push_back("数据库工单号");
    }
    return missing;
}

std::vector<std::string> deployGateMissing(const Task& task, const TicketNeeds& needs) {
    std::vector<std::string> missing;
    if (trimmed(task.pretestNo).empty()) missing.push_back("提测单号");
    if (trimmed(task.cabNo).empty()) missing.push_back("CAB 单号");
    if (needs.nacos && trimmed(task.ticketNacosNo).empty()) missing.push_back("nacos 工单号");
    if (needs.db && trimmed(task.ticketDbNo).empty()) missing.push_back("数据库工单号");
    return missing;
}

void setNotice(ReleaseDetailUiState& state, std::string text, bool isError) {
    state.notice = std::move(text);
    state.noticeIsError = isError;
}

void saveTask(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (isReadOnly(state.displayStatus) || state.task.id == 0) return;
    auto result = store.updateTask(state.task);
    if (result.isFail()) {
        setNotice(state, "保存任务失败：" + result.errorMessage(), true);
    }
}

// 返回是否落库成功；成功后同步追加到内存日志（服务顺序还原只读内存，不逐帧查库）。
bool appendActionLog(ReleaseStore& store, ReleaseDetailUiState& state,
                     const std::string& scope, const std::string& step,
                     const char* action, const std::string& note) {
    TaskLog log;
    log.taskId = state.task.id;
    log.scope = scope;
    log.step = step;
    log.action = action;
    log.at = static_cast<std::int64_t>(core::time::nowUnix());
    log.note = note;
    auto result = store.appendLog(log);
    if (result.isFail()) {
        setNotice(state, "操作已写入，但日志记录失败：" + result.errorMessage(), true);
        return false;
    }
    state.logs.push_back(log);
    return true;
}

// 写前置标志；失败不动内存态。返回是否落库成功。
bool writePrereqFlag(ReleaseStore& store, ReleaseDetailUiState& state,
                     const std::string& code, bool value) {
    Task next = state.task;
    if (code == "pretest") {
        next.pretestOk = value;
    } else if (code == "cab") {
        next.cabOk = value;
    } else if (code == "nacos") {
        next.ticketNacosOk = value;
    } else if (code == "db") {
        next.ticketDbOk = value;
    } else {
        setNotice(state, "未知的前置项：" + code, true);
        return false;
    }
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "保存前置项失败：" + result.errorMessage(), true);
        return false;
    }
    state.task = next;
    state.dirty = true;
    return true;
}

bool markPrereqCompleted(ReleaseStore& store, ReleaseDetailUiState& state,
                         const std::string& code, const std::string& label) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能确认步骤", true);
        return false;
    }
    // 幂等：连点 / 重复推进不产生重复确认日志。
    if ((code == "pretest" && state.task.pretestOk) ||
        (code == "cab" && state.task.cabOk) ||
        (code == "nacos" && state.task.ticketNacosOk) ||
        (code == "db" && state.task.ticketDbOk)) {
        return true;
    }
    std::vector<std::string> missing;
    if (code == "pretest" && trimmed(state.task.pretestNo).empty()) {
        missing.push_back("提测单号");
    }
    if (code == "cab" && trimmed(state.task.cabNo).empty()) missing.push_back("CAB 单号");
    if (code == "nacos" && trimmed(state.task.ticketNacosNo).empty()) {
        missing.push_back("nacos 工单号");
    }
    if (code == "db" && trimmed(state.task.ticketDbNo).empty()) {
        missing.push_back("数据库工单号");
    }
    if (!missing.empty()) {
        setNotice(state, "请先填写：" + joinMissing(missing), true);
        return false;
    }
    if (!writePrereqFlag(store, state, code, true)) return false;
    appendActionLog(store, state, "任务", label, "confirm", "");
    setNotice(state, "已确认「" + label + "」", false);
    return true;
}

bool markAnnouncementSent(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能确认步骤", true);
        return false;
    }
    if (state.task.announceSentAt != 0) return true;
    if (trimmed(state.task.announceText).empty()) {
        setNotice(state, "请先填写公告内容", true);
        return false;
    }
    Task next = state.task;
    next.announceSentAt = static_cast<std::int64_t>(core::time::nowUnix());
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "保存公告状态失败：" + result.errorMessage(), true);
        return false;
    }
    state.task = next;
    state.dirty = true;
    appendActionLog(store, state, "任务", "上线公告", "confirm", "");
    setNotice(state, "已标记公告为已发", false);
    return true;
}

// 补建后端服务的 5 个配置行（仅交互时调用）。成功后 state.configs 已同步。
bool ensureServiceConfigs(ReleaseStore& store, ReleaseDetailUiState& state,
                          std::int64_t serviceId) {
    const TaskService* svc = findService(state, serviceId);
    if (svc == nullptr) {
        setNotice(state, "服务不存在，无法读取配置", true);
        return false;
    }
    if (svc->kind != ServiceKind::Backend) return true;

    bool present[5] = {false, false, false, false, false};
    int maxOrder = -1;
    std::vector<TaskConfig> toCreate;
    for (const auto& cfg : state.configs) {
        if (cfg.taskServiceId != serviceId) continue;
        present[configKindIndex(cfg.kind)] = true;
        maxOrder = std::max(maxOrder, cfg.order);
        // id=0 兜底：当前装配不再注入内存占位行（占位只在 orderedConfigs 返回值里），
        // 保留此分支防未来调用方把草稿行塞进 state.configs。
        if (cfg.id == 0) toCreate.push_back(cfg);
    }

    static const ConfigKind kKinds[5] = {ConfigKind::Nacos, ConfigKind::Db, ConfigKind::Cron,
                                         ConfigKind::Menu, ConfigKind::Gateway};
    for (int i = 0; i < 5; ++i) {
        if (present[i]) continue;
        TaskConfig cfg;
        cfg.taskServiceId = serviceId;
        cfg.kind = kKinds[i];
        cfg.order = ++maxOrder;
        toCreate.push_back(cfg);
    }
    bool created = false;
    for (const auto& cfg : toCreate) {
        auto result = store.upsertTaskConfig(cfg);  // id=0 → INSERT，由库分配 id
        if (result.isFail()) {
            setNotice(state, "创建配置行失败：" + result.errorMessage(), true);
            state.dirty = true;
            return false;
        }
        created = true;
    }
    if (!created) return true;

    auto reloaded = store.listTaskConfigs(serviceId);
    if (reloaded.isFail()) {
        setNotice(state, "读取配置失败：" + reloaded.errorMessage(), true);
        state.dirty = true;
        return false;
    }
    state.configs.erase(
        std::remove_if(state.configs.begin(), state.configs.end(),
                       [serviceId](const TaskConfig& cfg) {
                           return cfg.taskServiceId == serviceId;
                       }),
        state.configs.end());
    for (const auto& cfg : reloaded.value()) state.configs.push_back(cfg);
    // 不置 dirty：本函数已把该服务的配置同步进 state.configs，而调用方可能持有
    // 刚读取的配置引用（编辑层）；整页重读会让这些引用失效。需要全量刷新时由更上层
    // 的写操作（勾选完成 / 排序）置 dirty。
    return true;
}

bool markStepCompleted(ReleaseStore& store, ReleaseDetailUiState& state,
                       std::int64_t serviceId, StepKind kind, const TaskEnv& env,
                       int nowMinute) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能确认步骤", true);
        return false;
    }
    if (kind != StepKind::MergeBranch && (nowMinute < 0 || nowMinute < env.startMinute)) {
        setNotice(state, "环境「" + env.envName + "」时间窗未开始，暂时不能执行该步骤",
                  true);
        return false;
    }
    if (kind == StepKind::Deploy) {
        const std::vector<std::string> missing =
            deployGateMissing(state.task, scanTicketNeeds(state));
        if (!missing.empty()) {
            setNotice(state, "请先填写：" + joinMissing(missing), true);
            return false;
        }
    }
    if (stepDone(state, serviceId, kind)) return true;  // 幂等
    const std::int64_t at = static_cast<std::int64_t>(core::time::nowUnix());
    auto result = store.setStepDone(serviceId, kind, at);
    if (result.isFail()) {
        setNotice(state, "保存步骤失败：" + result.errorMessage(), true);
        return false;
    }
    bool updated = false;
    for (auto& step : state.steps) {
        if (step.taskServiceId == serviceId && step.kind == kind) {
            step.doneAt = at;
            updated = true;
            break;
        }
    }
    if (!updated) {
        TaskStep step;
        step.taskServiceId = serviceId;
        step.kind = kind;
        step.doneAt = at;
        state.steps.push_back(std::move(step));
    }
    const TaskService* svc = findService(state, serviceId);
    const std::string scope = svc != nullptr ? svc->serviceName : "服务";
    appendActionLog(store, state, scope, stepLabel(kind), "confirm", env.envName);
    state.dirty = true;
    setNotice(state, "已确认「" + std::string(stepLabel(kind)) + "」（" + env.envName +
                         " / " + scope + "）",
              false);
    return true;
}

bool markConfigCompleted(ReleaseStore& store, ReleaseDetailUiState& state,
                         std::int64_t serviceId, ConfigKind kind, const TaskEnv& env,
                         int nowMinute) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能确认步骤", true);
        return false;
    }
    if (nowMinute < 0 || nowMinute < env.startMinute) {
        setNotice(state, "环境「" + env.envName + "」时间窗未开始，暂时不能执行该步骤",
                  true);
        return false;
    }
    const std::vector<std::string> missing = configGateMissing(state.task, kind);
    if (!missing.empty()) {
        setNotice(state, "请先填写：" + joinMissing(missing), true);
        return false;
    }
    if (!ensureServiceConfigs(store, state, serviceId)) return false;
    const TaskConfig* current = findConfig(state, serviceId, kind);
    if (current == nullptr) {
        setNotice(state, "配置项不存在，无法确认", true);
        return false;
    }
    if (current->doneAt != 0) return true;  // 幂等
    TaskConfig next = *current;
    next.doneAt = static_cast<std::int64_t>(core::time::nowUnix());
    auto result = store.upsertTaskConfig(next);
    if (result.isFail()) {
        setNotice(state, "保存配置完成状态失败：" + result.errorMessage(), true);
        return false;
    }
    for (auto& cfg : state.configs) {
        if (cfg.taskServiceId == serviceId && cfg.kind == kind) {
            cfg.doneAt = next.doneAt;
            break;
        }
    }
    const TaskService* svc = findService(state, serviceId);
    const std::string scope = svc != nullptr ? svc->serviceName : "服务";
    appendActionLog(store, state, scope, configLabel(kind), "confirm", env.envName);
    state.dirty = true;
    setNotice(state, "已确认「" + configLabel(kind) + "」（" + env.envName + " / " +
                         scope + "）",
              false);
    return true;
}

// 配置顺序：先补行并把 order 归一化成 0..n-1，再交换目标项与邻居。
void moveConfig(ReleaseStore& store, ReleaseDetailUiState& state,
                std::int64_t serviceId, ConfigKind kind, int delta) {
    if (!isConfigPhase(state.displayStatus) || isReadOnly(state.displayStatus)) {
        setNotice(state, "进入执行后配置顺序已锁定", true);
        return;
    }
    if (!ensureServiceConfigs(store, state, serviceId)) return;

    std::vector<TaskConfig> list;
    for (const auto& cfg : state.configs) {
        if (cfg.taskServiceId == serviceId) list.push_back(cfg);
    }
    std::sort(list.begin(), list.end(), [](const TaskConfig& a, const TaskConfig& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.id < b.id;
    });
    if (list.empty()) return;
    std::size_t index = list.size();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind == kind) {
            index = i;
            break;
        }
    }
    if (index >= list.size()) return;
    const std::ptrdiff_t target =
        static_cast<std::ptrdiff_t>(index) + static_cast<std::ptrdiff_t>(delta);
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(list.size())) {
        setNotice(state, "已到配置顺序边界", false);
        return;
    }

    std::vector<int> orders(list.size());
    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i] = static_cast<int>(i);
    }
    std::swap(orders[index], orders[static_cast<std::size_t>(target)]);
    for (std::size_t i = 0; i < list.size(); ++i) {
        TaskConfig cfg = list[i];
        cfg.order = orders[i];
        auto result = store.upsertTaskConfig(cfg);
        if (result.isFail()) {
            setNotice(state, "保存配置顺序失败：" + result.errorMessage(), true);
            state.dirty = true;
            return;
        }
    }
    state.dirty = true;
    setNotice(state, "已调整配置顺序", false);
}

// 服务卡高度在绘制段定义；拖拽命中换算需要它（与绘制同源，禁止另写一套步进值）。
float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc);

// 校验用配置快照：与清单同源（orderedConfigs 含 id=0 占位行），保证「显示出来的 5 项」
// 与 validateTask 的输入是同一份清单；不在这里判定规则。
std::vector<TaskConfig> configsForValidation(const ReleaseDetailUiState& state) {
    std::vector<TaskConfig> out;
    for (const auto& svc : state.services) {
        if (svc.removed) continue;
        const std::vector<TaskConfig> list = orderedConfigs(state, svc);
        out.insert(out.end(), list.begin(), list.end());
    }
    return out;
}

// 标红映射：消息文案必须与 release_logic::validateTask 的拼装逐字一致，
// 只做「哪一行」定位，不重新判定缺失规则。
bool serviceBranchFlagged(const ReleaseDetailUiState& state, const std::string& name) {
    if (!state.validationActive) return false;
    const std::string message = "服务「" + name + "」的分支未填写";
    return std::find(state.validationMissing.begin(), state.validationMissing.end(),
                     message) != state.validationMissing.end();
}

bool serviceConfigFlagged(const ReleaseDetailUiState& state, const std::string& name,
                          ConfigKind kind) {
    if (!state.validationActive) return false;
    const std::string message = "服务「" + name + "」的「" + configLabel(kind) +
                                "」配置内容为空";
    return std::find(state.validationMissing.begin(), state.validationMissing.end(),
                     message) != state.validationMissing.end();
}

// 提交：Draft → Pending。校验只认 release_logic::validateTask 的结果，
// 缺失清单原样留作标红依据；通过才写库并记一条 action=submit 的日志。
void submitTask(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (state.task.id == 0) return;
    if (state.displayStatus != TaskStatus::Draft) {
        setNotice(state, "只有「配置中」的任务可以提交", true);
        return;
    }
    const std::vector<std::string> missing =
        validateTask(state.services, configsForValidation(state));
    if (!missing.empty()) {
        state.validationActive = true;
        state.validationMissing = missing;
        state.confirm = ReleaseDetailConfirm::ValidationReport;
        setNotice(state, "提交校验未通过：还有 " + std::to_string(missing.size()) +
                             " 项不完整",
                  true);
        return;
    }
    Task next = state.task;
    next.status = TaskStatus::Pending;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "提交失败：" + result.errorMessage(), true);
        return;
    }
    state.task = next;
    state.displayStatus = TaskStatus::Pending;
    state.validationActive = false;
    state.validationMissing.clear();
    if (!appendActionLog(store, state, "任务", "提交", "submit", "")) return;
    setNotice(state, "已提交，任务进入待执行", false);
}

// 校验项数 = 未移除服务的分支 + 各自未跳过的配置（含内存占位行），
// 与 validateTask 的检查面一致，仅用于「N 项齐全」反馈。
int validationItemCount(const ReleaseDetailUiState& state) {
    int count = 0;
    for (const auto& svc : state.services) {
        if (svc.removed) continue;
        ++count;
        for (const auto& cfg : orderedConfigs(state, svc)) {
            if (!cfg.skipped) ++count;
        }
    }
    return count;
}

// 独立「校验」：只读动作，不改任务状态。缺失项标红并弹清单（与提交共用同一渲染），
// 通过时只回显结果；判定规则只来自 release_logic::validateTask。
void runValidation(ReleaseDetailUiState& state) {
    const std::vector<std::string> missing =
        validateTask(state.services, configsForValidation(state));
    if (!missing.empty()) {
        state.validationActive = true;
        state.validationMissing = missing;
        state.confirm = ReleaseDetailConfirm::ValidationReport;
        setNotice(state, "校验未通过：还有 " + std::to_string(missing.size()) + " 项不完整",
                  true);
        return;
    }
    state.validationActive = false;
    state.validationMissing.clear();
    if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
        state.confirm = ReleaseDetailConfirm::None;
    }
    setNotice(state, "校验通过，共 " + std::to_string(validationItemCount(state)) + " 项齐全",
              false);
}

void persistServiceOrder(ReleaseStore& store, ReleaseDetailUiState& state,
                         const std::string& envName,
                         const std::vector<std::int64_t>& ids) {
    if (ids.empty()) return;
    std::string note;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) note += ",";
        note += std::to_string(ids[i]);
    }
    if (!appendActionLog(store, state, envName, "reorder", "reorder", note)) return;
    setNotice(state, "已调整服务顺序", false);
}

// 拖拽目标插位：以「被拖项中心」越过其它项中心的数量作为去源后的插入下标。
// 高度一律复用绘制函数，避免拖拽命中与视觉行高漂移。
void updateServiceDragTarget(ReleaseDetailUiState& state) {
    if (state.drag.kind != ReleaseDetailDragKind::ServiceOrder) return;
    const std::vector<const TaskService*> live =
        orderedEnvServices(state, state.drag.envId, state.drag.envName, false);
    int source = -1;
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (live[i]->id == state.drag.serviceId) {
            source = static_cast<int>(i);
            break;
        }
    }
    if (source < 0) {
        state.drag.sourceIndex = -1;
        state.drag.targetIndex = -1;
        return;
    }
    state.drag.sourceIndex = source;
    std::vector<float> centers(live.size(), 0.0f);
    float top = 0.0f;
    for (std::size_t i = 0; i < live.size(); ++i) {
        const float height = serviceCardHeight(state, *live[i]);
        centers[i] = top + height * 0.5f;
        top += height + kServiceGap;
    }
    const float draggedCenter = centers[static_cast<std::size_t>(source)] + state.drag.deltaY;
    int target = 0;
    for (std::size_t i = 0; i < centers.size(); ++i) {
        if (static_cast<int>(i) != source && centers[i] < draggedCenter) ++target;
    }
    state.drag.targetIndex = target;
}

void updateConfigDragTarget(ReleaseDetailUiState& state) {
    if (state.drag.kind != ReleaseDetailDragKind::ConfigOrder) return;
    const TaskService* svc = findService(state, state.drag.serviceId);
    if (svc == nullptr) {
        state.drag.targetIndex = -1;
        return;
    }
    const std::vector<TaskConfig> configs = orderedConfigs(state, *svc);
    int source = -1;
    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (configs[i].kind == state.drag.configKind) {
            source = static_cast<int>(i);
            break;
        }
    }
    if (source < 0) {
        state.drag.sourceIndex = -1;
        state.drag.targetIndex = -1;
        return;
    }
    state.drag.sourceIndex = source;
    const float step = kStepRowH + kStepRowGap;
    const float draggedCenter =
        static_cast<float>(source) * step + kStepRowH * 0.5f + state.drag.deltaY;
    int target = 0;
    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (static_cast<int>(i) == source) continue;
        const float center = static_cast<float>(i) * step + kStepRowH * 0.5f;
        if (center < draggedCenter) ++target;
    }
    state.drag.targetIndex = target;
}

void finishServiceDrag(ReleaseStore& store, ReleaseDetailUiState& state, bool released) {
    if (state.drag.kind != ReleaseDetailDragKind::ServiceOrder) return;
    // 中途状态跳到执行中 / 完成时把手柄锁死：落在锁定后松手不得写库。
    if (!isConfigPhase(state.displayStatus) || isReadOnly(state.displayStatus)) {
        state.drag = ReleaseDetailDragState{};
        return;
    }
    const bool active = state.drag.active;
    const std::int64_t envId = state.drag.envId;
    const std::string envName = state.drag.envName;
    const std::int64_t serviceId = state.drag.serviceId;
    const int target = state.drag.targetIndex;
    state.drag = ReleaseDetailDragState{};
    if (!released || !active || target < 0) return;

    const std::vector<const TaskService*> live =
        orderedEnvServices(state, envId, envName, false);
    int source = -1;
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (live[i]->id == serviceId) {
            source = static_cast<int>(i);
            break;
        }
    }
    if (source < 0 || target == source) return;

    std::vector<const TaskService*> newLive;
    for (int i = 0; i < static_cast<int>(live.size()); ++i) {
        if (i != source) newLive.push_back(live[static_cast<std::size_t>(i)]);
    }
    const int insert = std::clamp(target, 0, static_cast<int>(newLive.size()));
    newLive.insert(newLive.begin() + insert, live[static_cast<std::size_t>(source)]);

    // 已移除服务不参与拖拽，但其相对位置保留：只把可视服务依次填回原来的非移除槽位。
    const std::vector<const TaskService*> orderedFull =
        orderedEnvServices(state, envId, envName, true);
    std::vector<std::int64_t> ids;
    ids.reserve(orderedFull.size());
    std::size_t nextLive = 0;
    for (const TaskService* svc : orderedFull) {
        if (!svc->removed && nextLive < newLive.size()) {
            ids.push_back(newLive[nextLive++]->id);
        } else {
            ids.push_back(svc->id);
        }
    }
    persistServiceOrder(store, state, envName, ids);
}

void finishConfigDrag(ReleaseStore& store, ReleaseDetailUiState& state, bool released) {
    if (state.drag.kind != ReleaseDetailDragKind::ConfigOrder) return;
    if (!isConfigPhase(state.displayStatus) || isReadOnly(state.displayStatus)) {
        state.drag = ReleaseDetailDragState{};
        return;
    }
    const bool active = state.drag.active;
    const std::int64_t serviceId = state.drag.serviceId;
    const ConfigKind kind = state.drag.configKind;
    const int target = state.drag.targetIndex;
    state.drag = ReleaseDetailDragState{};
    if (!released || !active || target < 0) return;
    const TaskService* service = findService(state, serviceId);
    if (service == nullptr) return;
    // 无位移的拖动不落任何库（包括不补建配置行）。
    {
        const std::vector<TaskConfig> pre = orderedConfigs(state, *service);
        int preSource = -1;
        for (std::size_t i = 0; i < pre.size(); ++i) {
            if (pre[i].kind == kind) {
                preSource = static_cast<int>(i);
                break;
            }
        }
        if (preSource < 0 || target == preSource) return;
    }
    if (!ensureServiceConfigs(store, state, serviceId)) return;

    std::vector<TaskConfig> list;
    for (const auto& cfg : state.configs) {
        if (cfg.taskServiceId == serviceId) list.push_back(cfg);
    }
    std::sort(list.begin(), list.end(), [](const TaskConfig& a, const TaskConfig& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.id < b.id;
    });
    int source = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind == kind) {
            source = static_cast<int>(i);
            break;
        }
    }
    if (source < 0 || target == source) return;

    std::vector<TaskConfig> next;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (i != source) next.push_back(list[static_cast<std::size_t>(i)]);
    }
    const int insert = std::clamp(target, 0, static_cast<int>(next.size()));
    next.insert(next.begin() + insert, list[static_cast<std::size_t>(source)]);

    for (std::size_t i = 0; i < next.size(); ++i) {
        TaskConfig cfg = next[i];
        cfg.order = static_cast<int>(i);
        auto result = store.upsertTaskConfig(cfg);
        if (result.isFail()) {
            setNotice(state, "保存配置顺序失败：" + result.errorMessage(), true);
            state.dirty = true;
            return;
        }
    }
    state.dirty = true;
    setNotice(state, "已调整配置顺序", false);
}

TaskStatus computeDisplayStatus(const ReleaseDetailUiState& state, int nowMinute) {
    const TaskStatus stored = state.task.status;
    if (stored == TaskStatus::Done || stored == TaskStatus::Voided ||
        stored == TaskStatus::Draft) {
        return stored;
    }
    const bool completed =
        taskCompleted(state.task, state.envs, state.services, state.configs, state.steps);
    if (completed) return TaskStatus::Done;
    const std::int64_t baseline = taskBaseline(state.envs);
    if (stored == TaskStatus::Pending && nowMinute >= 0 && baseline >= 0 &&
        nowMinute >= baseline) {
        return TaskStatus::Running;
    }
    return stored;
}

void advanceTaskStatus(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute) {
    state.displayStatus = computeDisplayStatus(state, nowMinute);
    if (state.displayStatus == state.task.status) return;
    if (state.task.status == TaskStatus::Voided) return;
    // 写失败后不在每帧重试（避免持续写盘），下一次 dirty 重读会重置该标志。
    if (state.statusWriteFailed) return;
    Task next = state.task;
    next.status = state.displayStatus;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        if (!state.statusWriteFailed) {
            setNotice(state, "状态推进失败：" + result.errorMessage(), true);
            state.statusWriteFailed = true;
        }
        return;
    }
    state.task = next;
    state.statusWriteFailed = false;
}

TaskStatus restoredStatus(const ReleaseDetailUiState& state, int nowMinute) {
    if (taskCompleted(state.task, state.envs, state.services, state.configs, state.steps)) {
        return TaskStatus::Done;
    }
    const std::int64_t baseline = taskBaseline(state.envs);
    if (nowMinute >= 0 && baseline >= 0 && nowMinute >= baseline) {
        return TaskStatus::Running;
    }
    return TaskStatus::Pending;
}

void applyVoid(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (state.displayStatus == TaskStatus::Voided) return;
    Task next = state.task;
    next.status = TaskStatus::Voided;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "作废失败：" + result.errorMessage(), true);
        return;
    }
    state.task = next;
    state.displayStatus = TaskStatus::Voided;
    state.validationActive = false;
    state.validationMissing.clear();
    setNotice(state, "任务已作废，整单只读", false);
}

void applyRestore(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute) {
    if (state.displayStatus != TaskStatus::Voided) {
        setNotice(state, "任务未作废，无需恢复", true);
        return;
    }
    // 同日已有非作废任务时拒绝恢复（作废不占名额，恢复会覆盖当天另一个任务）。
    auto existing = store.findTaskByDate(state.task.planDate);
    if (existing.isFail()) {
        setNotice(state, "检查同日任务失败：" + existing.errorMessage(), true);
        return;
    }
    if (existing.value().has_value() && existing.value()->id != state.task.id) {
        setNotice(state, "同一天已有非作废任务，不能恢复", true);
        return;
    }
    // 提交是状态推进的唯一入口：从未提交过（日志无 action=submit）的任务，
    // 作废→恢复后必须回到 Draft，不得凭空得到 Pending/Running 绕过提交校验。
    bool submitted = false;
    for (const auto& log : state.logs) {
        if (log.action == "submit") {
            submitted = true;
            break;
        }
    }
    Task next = state.task;
    next.status = submitted ? restoredStatus(state, nowMinute) : TaskStatus::Draft;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "恢复失败：" + result.errorMessage(), true);
        return;
    }
    state.task = next;
    state.displayStatus = next.status;
    state.validationActive = false;
    state.validationMissing.clear();
    setNotice(state, "任务已恢复为「" + releaseTaskStatusText(next.status) + "」", false);
}

void applyRevoke(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能撤回", true);
        state.confirm = ReleaseDetailConfirm::None;
        return;
    }
    switch (state.revokeKind) {
        case ReleaseDetailRevokeKind::Prereq: {
            if (!writePrereqFlag(store, state, state.revokePrereq, false)) {
                state.confirm = ReleaseDetailConfirm::None;
                return;
            }
            break;
        }
        case ReleaseDetailRevokeKind::Announce: {
            Task next = state.task;
            next.announceSentAt = 0;
            auto result = store.updateTask(next);
            if (result.isFail()) {
                setNotice(state, "撤回公告失败：" + result.errorMessage(), true);
                state.confirm = ReleaseDetailConfirm::None;
                return;
            }
            state.task = next;
            state.dirty = true;
            break;
        }
        case ReleaseDetailRevokeKind::Step: {
            auto result = store.setStepDone(state.revokeServiceId, state.revokeStepKind, 0);
            if (result.isFail()) {
                setNotice(state, "撤回步骤失败：" + result.errorMessage(), true);
                state.confirm = ReleaseDetailConfirm::None;
                return;
            }
            for (auto& step : state.steps) {
                if (step.taskServiceId == state.revokeServiceId &&
                    step.kind == state.revokeStepKind) {
                    step.doneAt = 0;
                    break;
                }
            }
            state.dirty = true;
            break;
        }
        case ReleaseDetailRevokeKind::Config: {
            const TaskConfig* current =
                findConfig(state, state.revokeServiceId, state.revokeConfigKind);
            if (current == nullptr) {
                setNotice(state, "配置项不存在，无法撤回", true);
                state.confirm = ReleaseDetailConfirm::None;
                return;
            }
            TaskConfig next = *current;
            next.doneAt = 0;
            auto result = store.upsertTaskConfig(next);
            if (result.isFail()) {
                setNotice(state, "撤回配置失败：" + result.errorMessage(), true);
                state.confirm = ReleaseDetailConfirm::None;
                return;
            }
            for (auto& cfg : state.configs) {
                if (cfg.taskServiceId == state.revokeServiceId &&
                    cfg.kind == state.revokeConfigKind) {
                    cfg.doneAt = 0;
                    break;
                }
            }
            state.dirty = true;
            break;
        }
    }
    appendActionLog(store, state, state.revokeScope, state.revokeStepLabel, "revoke", "");
    setNotice(state, "已撤回「" + state.revokeStepLabel + "」", false);
    state.confirm = ReleaseDetailConfirm::None;
}

// 打开撤回确认：不写库。
void openRevokePrereq(ReleaseDetailUiState& state, const std::string& code,
                      const std::string& label, bool announce) {
    state.confirm = ReleaseDetailConfirm::Revoke;
    state.revokeKind = announce ? ReleaseDetailRevokeKind::Announce
                                : ReleaseDetailRevokeKind::Prereq;
    state.revokePrereq = code;
    state.revokeScope = "任务";
    state.revokeStepLabel = label;
}

void openRevokeStep(ReleaseDetailUiState& state, std::int64_t serviceId,
                    StepKind kind, const std::string& scope) {
    state.confirm = ReleaseDetailConfirm::Revoke;
    state.revokeKind = ReleaseDetailRevokeKind::Step;
    state.revokeServiceId = serviceId;
    state.revokeStepKind = kind;
    state.revokeScope = scope;
    state.revokeStepLabel = stepLabel(kind);
}

void openRevokeConfig(ReleaseDetailUiState& state, std::int64_t serviceId,
                      ConfigKind kind, const std::string& scope) {
    state.confirm = ReleaseDetailConfirm::Revoke;
    state.revokeKind = ReleaseDetailRevokeKind::Config;
    state.revokeServiceId = serviceId;
    state.revokeConfigKind = kind;
    state.revokeScope = scope;
    state.revokeStepLabel = configLabel(kind);
}

// ==================== 跳过 / 移除 / 配置编辑 ====================

// 配置项「跳过」toggle：upsertTaskConfig 写 skipped 后同步内存并置 dirty。
// 已完成（doneAt != 0）的配置项不能跳过——进度与视觉都以「跳过优先」解释状态，
// 允许已完成项再跳过会让 doneAt 残留成「恢复非跳过即已完成」。
void toggleConfigSkip(ReleaseStore& store, ReleaseDetailUiState& state,
                      std::int64_t serviceId, ConfigKind kind) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能跳过", true);
        return;
    }
    const TaskConfig* current = findConfig(state, serviceId, kind);
    if (current == nullptr || current->id == 0) {
        // 库里缺行（尚未落库）先补建，再重取行指针（ensure 会整段替换 state.configs）。
        if (!ensureServiceConfigs(store, state, serviceId)) return;
        current = findConfig(state, serviceId, kind);
    }
    if (current == nullptr) {
        setNotice(state, "配置项不存在，无法跳过", true);
        return;
    }
    if (current->doneAt != 0) {
        setNotice(state, "已完成的配置项不能跳过", true);
        return;
    }
    TaskConfig next = *current;
    next.skipped = !next.skipped;
    auto result = store.upsertTaskConfig(next);
    if (result.isFail()) {
        setNotice(state, "保存跳过状态失败：" + result.errorMessage(), true);
        return;
    }
    for (auto& cfg : state.configs) {
        if (cfg.taskServiceId == serviceId && cfg.kind == kind) {
            cfg.skipped = next.skipped;
            break;
        }
    }
    state.dirty = true;
    setNotice(state, next.skipped
                          ? "已跳过「" + configLabel(kind) + "」，本次不做该配置"
                          : "已取消跳过「" + configLabel(kind) + "」",
              false);
}

// 打开配置编辑层：先补建落库行，再把现有内容快照进编辑缓冲（编辑层关闭前不写库）。
void openConfigEditor(ReleaseStore& store, ReleaseDetailUiState& state,
                      std::int64_t serviceId, ConfigKind kind) {
    if (!ensureServiceConfigs(store, state, serviceId)) return;
    const TaskConfig* cfg = findConfig(state, serviceId, kind);
    if (cfg == nullptr) {
        setNotice(state, "配置项不存在，无法打开编辑", true);
        return;
    }
    state.editorServiceId = serviceId;
    state.editorKind = kind;
    state.editorText = cfg->content;
    state.editorOpen = true;
}

// 编辑层保存：只写 content。刻意不改内存 content——置 dirty 后 loadDetail 重读库，
// 与重读前的旧快照对账（applyContentRevocations）发现内容变化时自动撤回该配置的
// 旧确认并记 revoke 日志（与「其它层保存后置 dirty」的既有契约同一条路径）。
void saveConfigEditor(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能编辑配置", true);
        state.editorOpen = false;
        return;
    }
    const TaskConfig* current =
        findConfig(state, state.editorServiceId, state.editorKind);
    if (current == nullptr) {
        setNotice(state, "配置项不存在，无法保存", true);
        state.editorOpen = false;
        return;
    }
    if (current->skipped) {
        setNotice(state, "该项已跳过，取消跳过后可编辑", true);
        state.editorOpen = false;
        return;
    }
    TaskConfig next = *current;
    next.content = state.editorText;
    auto result = store.upsertTaskConfig(next);
    if (result.isFail()) {
        setNotice(state, "保存配置失败：" + result.errorMessage(), true);
        state.editorOpen = false;
        return;
    }
    state.editorOpen = false;
    state.dirty = true;
    setNotice(state, "已保存「" + configLabel(state.editorKind) + "」配置内容", false);
}

void openRemoveService(ReleaseDetailUiState& state, std::int64_t serviceId,
                       const std::string& name) {
    state.confirm = ReleaseDetailConfirm::RemoveService;
    state.removeServiceId = serviceId;
    state.removeEnvId = 0;
    state.removeName = name;
}

void openRemoveEnv(ReleaseDetailUiState& state, std::int64_t envId,
                   const std::string& name) {
    state.confirm = ReleaseDetailConfirm::RemoveEnv;
    state.removeEnvId = envId;
    state.removeServiceId = 0;
    state.removeName = name;
}

void applyRemoveService(ReleaseStore& store, ReleaseDetailUiState& state) {
    const TaskService* svc = findService(state, state.removeServiceId);
    const std::string name = svc != nullptr ? svc->serviceName : state.removeName;
    auto result = store.removeTaskService(state.removeServiceId);
    if (result.isFail()) {
        setNotice(state, "移除服务失败：" + result.errorMessage(), true);
        return;
    }
    // dirty 重读后该服务 removed=true：进度 / 提醒 / 渲染口径全部自动排除。
    state.dirty = true;
    setNotice(state, "已移除服务「" + name + "」，进度与提醒不再包含它", false);
}

void applyRemoveEnv(ReleaseStore& store, ReleaseDetailUiState& state) {
    const TaskEnv* env = findEnv(state, state.removeEnvId);
    const std::string name = env != nullptr ? env->envName : state.removeName;
    auto result = store.removeTaskEnv(state.removeEnvId);
    if (result.isFail()) {
        setNotice(state, "移除环境失败：" + result.errorMessage(), true);
        return;
    }
    // dirty 重读后该环境不再入 state.envs（loadDetail 跳过 removed 环境）。
    state.dirty = true;
    setNotice(state, "已移除环境「" + name + "」，环境内服务不再参与进度与提醒", false);
}

void performNextStep(ReleaseStore& store, ReleaseDetailUiState& state, int nowMinute) {
    if (isReadOnly(state.displayStatus)) {
        setNotice(state, "任务已只读，不能推进", true);
        return;
    }
    if (!state.task.pretestOk) {
        markPrereqCompleted(store, state, "pretest", "提测单");
        return;
    }
    if (!state.task.cabOk) {
        markPrereqCompleted(store, state, "cab", "CAB 单");
        return;
    }
    const TicketNeeds needs = scanTicketNeeds(state);
    if (needs.nacos && !state.task.ticketNacosOk) {
        markPrereqCompleted(store, state, "nacos", "nacos 工单");
        return;
    }
    if (needs.db && !state.task.ticketDbOk) {
        markPrereqCompleted(store, state, "db", "数据库工单");
        return;
    }
    if (state.task.announceSentAt == 0) {
        markAnnouncementSent(store, state);
        return;
    }

    for (const auto& env : state.envs) {
        if (env.removed) continue;
        // 「下一步」跟随用户拖拽后的服务顺序，与清单展示同源。
        const std::vector<const TaskService*> services =
            orderedEnvServices(state, env.id, env.envName, false);
        for (const TaskService* service : services) {
            const TaskService& svc = *service;
            if (!stepDone(state, svc.id, StepKind::MergeBranch)) {
                markStepCompleted(store, state, svc.id, StepKind::MergeBranch, env,
                                  nowMinute);
                return;
            }
            const std::vector<TaskConfig> configs = orderedConfigs(state, svc);
            for (const auto& cfg : configs) {
                if (cfg.skipped || cfg.doneAt != 0) continue;
                markConfigCompleted(store, state, svc.id, cfg.kind, env, nowMinute);
                return;
            }
            if (!stepDone(state, svc.id, StepKind::Deploy)) {
                markStepCompleted(store, state, svc.id, StepKind::Deploy, env, nowMinute);
                return;
            }
            if (!stepDone(state, svc.id, StepKind::Verify)) {
                markStepCompleted(store, state, svc.id, StepKind::Verify, env, nowMinute);
                return;
            }
        }
    }
    setNotice(state, "全部步骤已完成", false);
}

// ==================== 绘制辅助 ====================

float measureText(const std::string& text, float fontSize, int weight) {
    return core::TextPrimitive::measureTextWidth(text, {}, fontSize, weight);
}

float chipWidth(const std::string& text, float fontSize) {
    return measureText(text, fontSize, 600) + 20.0f;
}

struct ChipColors {
    eui::Color bg;
    eui::Color fg;
};

ChipColors statusChipColors(TaskStatus status, const Tokens& t) {
    switch (status) {
        case TaskStatus::Draft:
            return {t.colors.hover, t.colors.textSecondary};
        case TaskStatus::Pending:
            return {eui::mixColor(t.colors.surface, t.colors.info, 0.12f), t.colors.info};
        case TaskStatus::Running:
            return {eui::mixColor(t.colors.surface, t.colors.warning, 0.14f),
                    t.colors.warning};
        case TaskStatus::Done:
            return {eui::mixColor(t.colors.surface, t.colors.success, 0.12f),
                    t.colors.success};
        case TaskStatus::Voided:
            return {t.colors.hover, t.colors.textDisabled};
    }
    return {t.colors.hover, t.colors.textSecondary};
}

float drawChip(eui::Ui& ui, const std::string& id, float x, float y,
               const std::string& text, const eui::Color& bg, const eui::Color& fg,
               const Tokens& t) {
    const float fontSize = t.sizes.fontSizeSmall - 1.0f;
    const float width = chipWidth(text, fontSize);
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, kChipH)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(width, kChipH)
        .text(text)
        .fontSize(fontSize)
        .lineHeight(kChipH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .build();
    return width;
}

void drawCardBackground(eui::Ui& ui, const std::string& id, float x, float y,
                        float w, float h, const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens,
                        bool subtle) {
    // 卡片统一 12 圆角 + 1px #E5E7EB 描边（视觉契约）；嵌套服务卡只用浅底区分层级，
    // 不改圆角档，避免同页出现 8/12 两套卡圆角。
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(subtle ? t.colors.background : t.colors.surface)
        .border(1.0f, t.colors.borderDefault)
        .radius(t.sizes.radiusLarge)
        .shadow(subtle ? core::Shadow{}
                       : components::theme::shadow(themeTokens, 3.0f, 1.0f, 0.04f, 0.02f))
        .build();
}

void drawSectionTitle(eui::Ui& ui, const std::string& id, float x, float y,
                      const std::string& text, const Tokens& t) {
    ui.text(id)
        .position(x, y)
        .size(600.0f, kSectionTitleH)
        .text(text)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(kSectionTitleH)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
}

void drawMiniArrow(eui::Ui& ui, const std::string& id, float x, float y,
                   bool up, bool enabled, const Tokens& t,
                   const std::function<void()>& onClick) {
    auto rect = ui.rect(id)
                    .position(x, y)
                    .size(kArrowW, kArrowW)
                    .color(t.colors.hover)
                    .radius(t.sizes.radiusSmall)
                    .opacity(enabled ? 1.0f : 0.4f);
    if (enabled && onClick) rect.onClick(onClick);
    rect.build();
    ui.text(id + ".icon")
        .position(x, y)
        .size(kArrowW, kArrowW)
        .icon(up ? 0xF077u : 0xF078u)
        .fontSize(core::theme::scaled(10.0f))
        .lineHeight(kArrowW)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .opacity(enabled ? 1.0f : 0.4f)
        .build();
}

// 拖拽手柄（⋮⋮）：透明底 + hover 浅染；命中区与图标分离，图标在下层之后绘制。
// 手柄自身承担 press/drag/release，行点击命中区在其下方，点手柄不会误触行。
void drawDragHandle(eui::Ui& ui, const std::string& id, float x, float y,
                    float width, float height, const Tokens& t,
                    const std::function<void(const eui::PointerEvent&, const eui::Rect&)>&
                        onPress,
                    const std::function<void(const core::dsl::DragEvent&)>& onDrag,
                    const std::function<void(const eui::PointerEvent&,
                                             const eui::Rect&)>& onRelease) {
    eui::Color hover = t.colors.textPrimary;
    hover.a = 0.06f;
    eui::Color pressed = t.colors.textPrimary;
    pressed.a = 0.10f;
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, hover, pressed)
        .radius(4.0f)
        .onPress(onPress)
        .onDrag(onDrag)
        .onRelease(onRelease)
        .build();
    ui.text(id + ".icon")
        .position(x, y)
        .size(width, height)
        .text("⋮⋮")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(height)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
}

void drawStepDot(eui::Ui& ui, const std::string& id, float x, float y,
                 bool done, bool skipped, const Tokens& t) {
    // 步骤状态与勾选框同一套自绘形状：未做空心、已完成 success 实底 + 勾、
    // 跳过灰底 + 横线（行首标签另有划线，在 drawStepRow 内画）。
    drawCheckboxGlyph(ui, id + ".dot", x, y, kStepDot, done, skipped, true, t, true);
}

void drawReadOnlyValue(eui::Ui& ui, const std::string& id, float x, float y,
                       float w, float h, const std::string& value,
                       const std::string& placeholder, bool multiline,
                       const Tokens& t) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.hover)
        .border(1.0f, t.colors.borderDefault)
        .radius(10.0f)
        .build();
    ui.text(id + ".text")
        .position(x + 12.0f, y + (multiline ? 8.0f : 0.0f))
        .size(std::max(0.0f, w - 24.0f), multiline ? std::max(0.0f, h - 16.0f) : h)
        .text(value.empty() ? placeholder : value)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(multiline ? 20.0f : h)
        .maxWidth(std::max(0.0f, w - 24.0f))
        .wrap(multiline)
        .overflow(core::TextOverflow::Clip)
        .verticalAlign(multiline ? eui::VerticalAlign::Top : eui::VerticalAlign::Center)
        .color(value.empty() ? t.colors.textTertiary : t.colors.textSecondary)
        .build();
}

// ==================== 任务级前置卡 ====================

enum class PrereqKey { Pretest, Cab, Nacos, Db, Announce };

const char* prereqTitle(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return "提测单";
        case PrereqKey::Cab: return "CAB 单";
        case PrereqKey::Nacos: return "nacos 工单";
        case PrereqKey::Db: return "数据库工单";
        case PrereqKey::Announce: return "上线公告";
    }
    return "前置项";
}

const char* prereqCode(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return "pretest";
        case PrereqKey::Cab: return "cab";
        case PrereqKey::Nacos: return "nacos";
        case PrereqKey::Db: return "db";
        case PrereqKey::Announce: return "announce";
    }
    return "";
}

const char* prereqCheckLabel(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest:
        case PrereqKey::Cab: return "已完成";
        case PrereqKey::Nacos:
        case PrereqKey::Db: return "已审批";
        case PrereqKey::Announce: return "已发";
    }
    return "已完成";
}

std::string prereqValue(const ReleaseDetailUiState& state, PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return state.task.pretestNo;
        case PrereqKey::Cab: return state.task.cabNo;
        case PrereqKey::Nacos: return state.task.ticketNacosNo;
        case PrereqKey::Db: return state.task.ticketDbNo;
        case PrereqKey::Announce: return state.task.announceText;
    }
    return {};
}

void setPrereqValue(ReleaseDetailUiState& state, PrereqKey key, const std::string& value) {
    switch (key) {
        case PrereqKey::Pretest:
            state.task.pretestNo = value;
            break;
        case PrereqKey::Cab:
            state.task.cabNo = value;
            break;
        case PrereqKey::Nacos:
            state.task.ticketNacosNo = value;
            break;
        case PrereqKey::Db:
            state.task.ticketDbNo = value;
            break;
        case PrereqKey::Announce:
            state.task.announceText = value;
            break;
    }
}

bool prereqChecked(const ReleaseDetailUiState& state, PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return state.task.pretestOk;
        case PrereqKey::Cab: return state.task.cabOk;
        case PrereqKey::Nacos: return state.task.ticketNacosOk;
        case PrereqKey::Db: return state.task.ticketDbOk;
        case PrereqKey::Announce: return state.task.announceSentAt != 0;
    }
    return false;
}

bool prereqGateOpen(const ReleaseDetailUiState& state, PrereqKey key) {
    if (key == PrereqKey::Announce) return !trimmed(state.task.announceText).empty();
    return !trimmed(prereqValue(state, key)).empty();
}

// ASCII 大小写不敏感子串（中文按字节原样比较）。用于把 dueItems 的条目对回卡片。
bool containsAsciiCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            unsigned char a = static_cast<unsigned char>(haystack[i + j]);
            unsigned char b = static_cast<unsigned char>(needle[j]);
            if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

int prereqColumns(float availableWidth, int count) {
    if (count <= 0) return 1;
    const float columns = std::floor((availableWidth + kPrereqGap) /
                                     (kPrereqCardMinW + kPrereqGap));
    return std::clamp(static_cast<int>(columns), 1, count);
}

float prereqSectionHeight(float width, int count) {
    if (count <= 0) return kSectionTitleH;
    const int cols = prereqColumns(width, count);
    const int rows = (count + cols - 1) / cols;
    return kSectionTitleH + kSectionGap +
           static_cast<float>(rows) * kPrereqCardH +
           static_cast<float>(rows - 1) * kPrereqGap;
}

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

void drawPrereqSection(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                       bool readOnly, int nowMinute, const Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       ReleaseStore& store) {
    const TicketNeeds needs = scanTicketNeeds(state);
    std::vector<PrereqView> cards;
    const PrereqKey baseKeys[2] = {PrereqKey::Pretest, PrereqKey::Cab};
    for (PrereqKey key : baseKeys) {
        PrereqView view;
        view.key = key;
        view.title = prereqTitle(key);
        view.checkLabel = prereqCheckLabel(key);
        view.checked = prereqChecked(state, key);
        view.value = prereqValue(state, key);
        view.gateOpen = prereqGateOpen(state, key);
        cards.push_back(std::move(view));
    }
    if (needs.nacos) {
        PrereqView view;
        view.key = PrereqKey::Nacos;
        view.title = prereqTitle(PrereqKey::Nacos);
        view.checkLabel = prereqCheckLabel(PrereqKey::Nacos);
        view.checked = prereqChecked(state, PrereqKey::Nacos);
        view.value = prereqValue(state, PrereqKey::Nacos);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Nacos);
        cards.push_back(std::move(view));
    }
    if (needs.db) {
        PrereqView view;
        view.key = PrereqKey::Db;
        view.title = prereqTitle(PrereqKey::Db);
        view.checkLabel = prereqCheckLabel(PrereqKey::Db);
        view.checked = prereqChecked(state, PrereqKey::Db);
        view.value = prereqValue(state, PrereqKey::Db);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Db);
        cards.push_back(std::move(view));
    }
    {
        PrereqView view;
        view.key = PrereqKey::Announce;
        view.title = prereqTitle(PrereqKey::Announce);
        view.checkLabel = prereqCheckLabel(PrereqKey::Announce);
        view.checked = prereqChecked(state, PrereqKey::Announce);
        view.value = prereqValue(state, PrereqKey::Announce);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Announce);
        cards.push_back(std::move(view));
    }

    const std::int64_t baseline = taskBaseline(state.envs);
    // 到期状态只读 release_logic 的聚合结果，不在 UI 重算「基准 − 60」的触发时刻。
    // 卡片标题是 dueItems 标题的子串（提测单 / CAB 单 / nacos 工单 / 数据库工单 /
    // 上线公告），据此把条目对回卡片；已完成的项不会出现在 dueItems 里。
    // 走 canonical 6 参入口：state.steps 是本任务全部步骤的汇总（loadDetail 按服务
    // 收集），与提醒引擎同源——步骤全做完的环境不再因空 steps 在详情页假红。
    const std::vector<DueItem> due = dueItems(state.task, state.envs, state.services,
                                              state.configs, state.steps, nowMinute);
    const int cols = prereqColumns(width, static_cast<int>(cards.size()));
    const float cardW = cols > 0
        ? (width - kPrereqGap * static_cast<float>(cols - 1)) / static_cast<float>(cols)
        : width;
    const float sectionH = prereqSectionHeight(width, static_cast<int>(cards.size()));

    ui.stack("release.detail.prereq.section")
        .size(width, sectionH)
        .content([&] {
            drawSectionTitle(ui, "release.detail.prereq.title", 0.0f, 0.0f, "任务前置", t);
            for (std::size_t i = 0; i < cards.size(); ++i) {
                PrereqView& card = cards[i];
                const int col = static_cast<int>(i) % cols;
                const int row = static_cast<int>(i) / cols;
                const float x = static_cast<float>(col) * (cardW + kPrereqGap);
                const float y = kSectionTitleH + kSectionGap +
                                static_cast<float>(row) * (kPrereqCardH + kPrereqGap);
                const bool announce = card.key == PrereqKey::Announce;
                const bool doc = !announce;

                card.overdue = false;
                if (!card.checked) {
                    for (const auto& item : due) {
                        if (item.scope == "task" &&
                            containsAsciiCaseInsensitive(item.title, card.title)) {
                            card.overdue = true;
                            break;
                        }
                    }
                }
                if (baseline < 0) {
                    card.dueText = "到期时间未定义";
                } else if (announce) {
                    // 公告到期时间 = 当天最早环境的开始时间（taskBaseline）。
                    card.dueText = (card.checked ? "已确认 · " : "到期 ") +
                                   formatPlanMinute(static_cast<int>(baseline)) +
                                   "（最早环境）";
                } else {
                    card.dueText = "到期：时间基准前 60 分钟";
                }

                const std::string id = "release.detail.prereq." + std::to_string(i);
                drawCardBackground(ui, id + ".bg", x, y, cardW, kPrereqCardH, t,
                                   themeTokens, false);
                ui.text(id + ".title")
                    .position(x + 16.0f, y + 14.0f)
                    .size(std::max(0.0f, cardW - 32.0f), 20.0f)
                    .text(card.title)
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(20.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".due")
                    .position(x + 16.0f, y + 36.0f)
                    .size(std::max(0.0f, cardW - 32.0f), 16.0f)
                    .text(card.dueText)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(16.0f)
                    .maxWidth(std::max(0.0f, cardW - 32.0f))
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(card.overdue ? t.colors.errorText : t.colors.textTertiary)
                    .build();

                const float inputX = x + 16.0f;
                const float inputY = y + 58.0f;
                const float inputW = std::max(0.0f, cardW - 32.0f);
                const float inputH = announce ? 60.0f : 40.0f;
                if (readOnly) {
                    drawReadOnlyValue(ui, id + ".input", inputX, inputY, inputW, inputH,
                                      card.value,
                                      doc ? "未填写单号" : "未填写公告内容", announce, t);
                } else {
                    components::input(ui, id + ".input")
                        .position(inputX, inputY)
                        .size(inputW, inputH)
                        .value(card.value)
                        .placeholder(doc ? "填写单号" : "填写上线公告")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(announce)
                        .scrollbar(announce)
                        .inset(announce ? 8.0f : 12.0f)
                        .theme(themeTokens)
                        .onChange([&state, key = card.key](const std::string& value) {
                            setPrereqValue(state, key, value);
                        })
                        .onFocus([&store, &state](bool focused) {
                            if (!focused) saveTask(store, state);
                        })
                        .onEnter([&store, &state]() { saveTask(store, state); })
                        .onCancel([&store, &state]() { saveTask(store, state); })
                        .build();
                }

                const float checkY = y + (announce ? 126.0f : 112.0f);
                // 单据号未填时禁用：灰态、不可点，标签位置改为提示文案。
                const bool showHint = !readOnly && !card.checked && !card.gateOpen;
                ReleaseCheckboxOptions check;
                check.checked = card.checked;
                check.enabled = !readOnly && (card.gateOpen || card.checked);
                check.label = card.checkLabel;
                check.hint = showHint
                                 ? (doc ? "请先填写单据号" : "请先填写公告内容")
                                 : std::string();
                check.boxSize = kCheckSize;
                check.fontSize = t.sizes.fontSizeSmall;
                check.width = std::max(0.0f, cardW - 32.0f);
                check.height = kCheckSize;
                check.inset = 0.0f;
                check.gap = 8.0f;
                std::function<void()> onCheck;
                if (!readOnly) {
                    const std::string code = prereqCode(card.key);
                    const std::string label = card.checkLabel;
                    onCheck = [&store, &state, key = card.key, code, label]() {
                        if (isReadOnly(state.displayStatus)) return;
                        if (prereqChecked(state, key)) {
                            openRevokePrereq(state, code, label,
                                             key == PrereqKey::Announce);
                            return;
                        }
                        if (key == PrereqKey::Announce) {
                            markAnnouncementSent(store, state);
                        } else {
                            markPrereqCompleted(store, state, code, label);
                        }
                    };
                }
                drawReleaseCheckboxAt(ui, id + ".check", inputX, checkY, check,
                                      onCheck, t);
            }
        })
        .build();
}

// ==================== 环境 / 服务 ====================

float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc) {
    const float rows = static_cast<float>(3 + orderedConfigs(state, svc).size());
    return 68.0f + rows * kStepRowH + (rows - 1.0f) * kStepRowGap;
}

float envCardHeight(const ReleaseDetailUiState& state, const TaskEnv& env) {
    const std::vector<const TaskService*> services = liveServices(state, env);
    float height = kEnvHeaderH + kEnvInset;
    for (std::size_t i = 0; i < services.size(); ++i) {
        height += serviceCardHeight(state, *services[i]);
        if (i + 1 < services.size()) height += kServiceGap;
    }
    return height;
}

float envSectionHeight(const ReleaseDetailUiState& state, float width) {
    (void)width;
    if (state.envs.empty()) return kSectionTitleH + kSectionGap + 48.0f;
    float height = kSectionTitleH + kSectionGap;
    for (std::size_t i = 0; i < state.envs.size(); ++i) {
        height += envCardHeight(state, state.envs[i]);
        if (i + 1 < state.envs.size()) height += kPrereqGap;
    }
    return height;
}

struct StepRowView {
    std::string id;
    std::string label;
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

void drawStepRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const StepRowView& row, bool current, bool readOnly,
                 bool configurable, bool readOnlyViewAllowed, const Tokens& t,
                 const std::function<void()>& onRowClick,
                 const std::function<void()>& onView,
                 const std::function<void()>& onMoveUp,
                 const std::function<void()>& onMoveDown,
                 const std::function<void()>& onSkipToggle) {
    const float rowX = x + 8.0f;
    const float rowW = std::max(0.0f, width - 16.0f);
    if (current) {
        ui.rect(id + ".current")
            .position(rowX, y - 1.0f)
            .size(rowW, kStepRowH + 2.0f)
            .color(eui::mixColor(t.colors.surface, t.colors.primary, 0.08f))
            .border(1.5f, t.colors.primary)
            .radius(t.sizes.radius)
            .build();
    }
    if (row.invalid) {
        ui.rect(id + ".invalid")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
            .radius(t.sizes.radius)
            .build();
    }
    if (row.dragging) {
        eui::Color veil = t.colors.surface;
        veil.a = 0.55f;
        ui.rect(id + ".drag.veil")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(veil)
            .radius(t.sizes.radius)
            .build();
    }
    if (!readOnly && !row.skipped && onRowClick) {
        ui.rect(id + ".hit")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(onRowClick)
            .build();
    }

    const float dotX = rowX + 10.0f;
    const float dotY = y + (kStepRowH - kStepDot) * 0.5f;
    drawStepDot(ui, id + ".status", dotX, dotY, row.done, row.skipped, t);

    const float viewX = rowX + rowW - 8.0f - kViewButtonW;
    const float arrowsX = viewX - kArrowGap - (kArrowW * 2.0f + kArrowGap);
    // 可排序的配置行在最左侧预留拖拽手柄位（⋮⋮）；手柄由 drawServiceCard 叠加绘制。
    const bool showDragHandle = row.config && configurable;
    const float nameX = dotX + kStepDot +
                        (showDragHandle ? kDragHandleW + kDragHandleGap : 10.0f);
    // 「跳过」勾选框（未完成配置行，readOnly 时 onSkipToggle 为空不出现）：
    // 可排序时靠排序箭头左侧，执行中（不可排序）靠「查看」按钮左侧。
    ReleaseCheckboxOptions skipOptions;
    float skipToggleW = 0.0f;
    float skipToggleX = 0.0f;
    if (row.config && !row.done && onSkipToggle) {
        skipOptions.checked = row.skipped;
        skipOptions.enabled = true;
        skipOptions.label = "跳过";
        skipOptions.boxSize = kCheckSize;
        skipOptions.fontSize = t.sizes.fontSizeSmall;
        skipOptions.height = kStepRowH;
        skipOptions.inset = 0.0f;
        skipOptions.gap = 6.0f;
        skipToggleW = checkbox_detail::stackWidth(skipOptions);
        const float anchor = configurable ? arrowsX : viewX;
        skipToggleX = anchor - kArrowGap - skipToggleW;
    }
    float nameRight = rowX + rowW - 8.0f;
    if (row.config) {
        nameRight = readOnly ? viewX - 8.0f
                             : (skipToggleW > 0.0f ? skipToggleX - 8.0f
                                                   : arrowsX - 8.0f);
    }
    const float nameW = std::max(0.0f, nameRight - nameX);

    const std::string label = row.invalid ? row.label + " · 内容为空" : row.label;
    eui::Color labelColor = row.done ? t.colors.textPrimary
                             : (row.skipped ? t.colors.textDisabled
                                            : (current ? t.colors.textPrimary
                                                       : t.colors.textSecondary));
    if (row.invalid) labelColor = t.colors.errorText;
    ui.text(id + ".label")
        .position(nameX, y)
        .size(nameW, kStepRowH)
        .text(label)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(kStepRowH)
        .maxWidth(nameW)
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(labelColor)
        .build();
    if (row.skipped) {
        const float labelW =
            std::min(nameW, measureText(row.label, t.sizes.fontSizeBody, 400));
        ui.rect(id + ".strike")
            .position(nameX, y + kStepRowH * 0.5f)
            .size(labelW, 1.0f)
            .color(t.colors.textDisabled)
            .build();
        const float skipX = nameX + labelW + 8.0f;
        const float skipW = std::max(0.0f, nameRight - skipX);
        if (skipW > 28.0f) {
            ui.text(id + ".skip")
                .position(skipX, y)
                .size(skipW, kStepRowH)
                .text("已跳过")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(kStepRowH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        }
    }

    if (row.config && readOnlyViewAllowed && onView) {
        ui_kit::ButtonOptions options;
        options.onClick = onView;
        ui_kit::button(ui, id + ".view", "查看", ui_kit::ButtonLevel::Text, viewX, y,
                       kViewButtonW, kStepRowH, t, options);
    }
    if (row.config && configurable) {
        const bool canUp = row.configIndex > 0;
        const bool canDown = row.configIndex + 1 < row.configCount;
        drawMiniArrow(ui, id + ".up", arrowsX, y + (kStepRowH - kArrowW) * 0.5f, true,
                      canUp, t, onMoveUp);
        drawMiniArrow(ui, id + ".down", arrowsX + kArrowW + kArrowGap,
                      y + (kStepRowH - kArrowW) * 0.5f, false, canDown, t, onMoveDown);
    }
    // 最后画勾选框，保证命中区盖在行点击命中区之上。
    if (skipToggleW > 0.0f) {
        drawReleaseCheckboxAt(ui, id + ".skip.toggle", skipToggleX, y, skipOptions,
                              onSkipToggle, t);
    }
}

void drawServiceCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                     const TaskEnv& env, const TaskService& svc,
                     ReleaseDetailUiState& state, bool readOnly, bool configurable,
                     int nowMinute, const Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     ReleaseStore& store) {
    const float height = serviceCardHeight(state, svc);
    drawCardBackground(ui, id + ".bg", x, y, width, height, t, themeTokens, true);
    if (state.drag.kind == ReleaseDetailDragKind::ServiceOrder && state.drag.active &&
        state.drag.serviceId == svc.id) {
        eui::Color veil = t.colors.surface;
        veil.a = 0.55f;
        ui.rect(id + ".drag.veil")
            .position(x, y)
            .size(width, height)
            .color(veil)
            .radius(t.sizes.radiusLarge)
            .build();
    }

    const float inset = kServiceInset;
    const float branchY = y + 32.0f;
    // 「移除」入口（服务卡右上角；Done / Voided 只读时隐藏）。Text 级 + 错误红，
    // 点击只开二次确认，不直接写库。
    const float removeW = readOnly ? 0.0f : 44.0f;
    const float chipW = chipWidth(serviceKindLabel(svc.kind), t.sizes.fontSizeSmall - 1.0f);
    const float chipX =
        x + width - inset - chipW - (removeW > 0.0f ? removeW + 8.0f : 0.0f);
    drawChip(ui, id + ".kind", chipX, y + 12.0f,
             serviceKindLabel(svc.kind),
             svc.kind == ServiceKind::Backend ? t.colors.primarySoftBg : t.colors.hover,
             svc.kind == ServiceKind::Backend ? t.colors.primaryText
                                              : t.colors.textSecondary,
             t);
    if (removeW > 0.0f) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state, serviceId = svc.id, name = svc.serviceName] {
            openRemoveService(state, serviceId, name);
        };
        options.hasTextColorOverride = true;
        options.textColorOverride = t.colors.errorText;
        ui_kit::button(ui, id + ".remove", "移除", ui_kit::ButtonLevel::Text,
                       x + width - inset - removeW, y + 12.0f, removeW, 22.0f, t,
                       options);
    }
    // 服务顺序拖拽只在配置中 / 待执行可用；单服务没有排序意义，不画手柄。
    const bool showOrderHandle = configurable && countLiveServices(state, env) > 1;
    const float nameX = x + inset + (showOrderHandle ? kDragHandleW + kDragHandleGap : 0.0f);
    const float nameW = std::max(
        0.0f, width - inset * 2.0f - chipW - 8.0f -
                  (removeW > 0.0f ? removeW + 8.0f : 0.0f) -
                  (showOrderHandle ? kDragHandleW + kDragHandleGap : 0.0f));
    if (showOrderHandle) {
        drawDragHandle(
            ui, id + ".order", x + inset, y + 8.0f, kDragHandleW, kDragHandleH, t,
            [&state, serviceId = svc.id, envId = env.id,
             envName = env.envName](const eui::PointerEvent&, const eui::Rect&) {
                state.drag = ReleaseDetailDragState{};
                state.drag.kind = ReleaseDetailDragKind::ServiceOrder;
                state.drag.envId = envId;
                state.drag.envName = envName;
                state.drag.serviceId = serviceId;
            },
            [&state](const core::dsl::DragEvent& event) {
                if (state.drag.kind != ReleaseDetailDragKind::ServiceOrder) return;
                state.drag.active = true;
                const double scale = event.scale > 0.0f ? event.scale : 1.0f;
                state.drag.deltaY = static_cast<float>(event.totalY / scale);
                updateServiceDragTarget(state);
            },
            [&store, &state](const eui::PointerEvent& event, const eui::Rect&) {
                finishServiceDrag(store, state,
                                  event.action == eui::PointerAction::Release);
            });
    }
    ui.text(id + ".name")
        .position(nameX, y + 10.0f)
        .size(nameW, 20.0f)
        .text(svc.serviceName)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    const bool hasBranch = !svc.branch.empty();
    const bool branchFlagged = serviceBranchFlagged(state, svc.serviceName);
    if (branchFlagged) {
        ui.rect(id + ".branch.flag")
            .position(nameX - 4.0f, branchY - 2.0f)
            .size(nameW + 8.0f, 20.0f)
            .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
            .radius(4.0f)
            .build();
    }
    ui.text(id + ".branch")
        .position(nameX, branchY)
        .size(nameW, 16.0f)
        .text(hasBranch ? "分支 " + svc.branch
                        : (branchFlagged ? "分支未填写 · 提交校验未通过"
                                         : "分支未填写"))
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(branchFlagged || !hasBranch ? t.colors.errorText : t.colors.textTertiary)
        .build();

    std::vector<StepRowView> rows;
    {
        StepRowView merge;
        merge.id = "merge";
        merge.label = stepLabel(StepKind::MergeBranch);
        merge.done = stepDone(state, svc.id, StepKind::MergeBranch);
        merge.stepKind = StepKind::MergeBranch;
        rows.push_back(std::move(merge));

        const std::vector<TaskConfig> configs = orderedConfigs(state, svc);
        for (std::size_t i = 0; i < configs.size(); ++i) {
            const TaskConfig& cfg = configs[i];
            StepRowView row;
            row.id = "config." + std::to_string(configKindIndex(cfg.kind));
            row.label = configLabel(cfg.kind);
            row.skipped = cfg.skipped;
            // 跳过态优先于完成态：逻辑层对 skipped 配置不计进度，视觉也不能显示
            // 成「已完成」，否则会出现划线与绿勾并存的矛盾状态。
            row.done = !cfg.skipped && cfg.doneAt != 0;
            row.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
            row.invalid = serviceConfigFlagged(state, svc.serviceName, cfg.kind);
            row.dragging = state.drag.kind == ReleaseDetailDragKind::ConfigOrder &&
                           state.drag.active && state.drag.serviceId == svc.id &&
                           state.drag.configKind == cfg.kind;
            row.config = true;
            row.configKind = cfg.kind;
            row.configIndex = i;
            row.configCount = configs.size();
            rows.push_back(std::move(row));
        }

        StepRowView deploy;
        deploy.id = "deploy";
        deploy.label = stepLabel(StepKind::Deploy);
        deploy.done = stepDone(state, svc.id, StepKind::Deploy);
        deploy.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
        deploy.stepKind = StepKind::Deploy;
        rows.push_back(std::move(deploy));

        StepRowView verify;
        verify.id = "verify";
        verify.label = stepLabel(StepKind::Verify);
        verify.done = stepDone(state, svc.id, StepKind::Verify);
        verify.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
        verify.stepKind = StepKind::Verify;
        rows.push_back(std::move(verify));
    }

    std::size_t currentIndex = rows.size();
    if (!readOnly) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].done && !rows[i].skipped && rows[i].timeReady) {
                currentIndex = i;
                break;
            }
        }
    }

    const std::string scope = svc.serviceName;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const StepRowView& row = rows[i];
        const float rowY = y + kServiceRowStart +
                           static_cast<float>(i) * (kStepRowH + kStepRowGap);
        const std::string rowId = id + ".step." + row.id;

        std::function<void()> onRowClick;
        if (!row.done) {
            if (row.config) {
                onRowClick = [&store, &state, serviceId = svc.id, kind = row.configKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, nowMinute] {
                    if (isReadOnly(state.displayStatus)) return;
                    if (!timeReady) {
                        setNotice(state,
                                  "环境「" + envName + "」时间窗未开始，暂时不能执行该步骤",
                                  true);
                        return;
                    }
                    // 门槛按「此刻」的表单值重查：输入框刚填完、同一帧点击时捕获值
                    // 可能还是旧的，不能拿快照挡下合法确认。
                    const std::vector<std::string> missing =
                        configGateMissing(state.task, kind);
                    if (!missing.empty()) {
                        setNotice(state, "请先填写：" + joinMissing(missing), true);
                        return;
                    }
                    markConfigCompleted(store, state, serviceId, kind, envCopy, nowMinute);
                };
            } else {
                onRowClick = [&store, &state, serviceId = svc.id, kind = row.stepKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, nowMinute] {
                    if (isReadOnly(state.displayStatus)) return;
                    if (!timeReady) {
                        setNotice(state,
                                  "环境「" + envName + "」时间窗未开始，暂时不能执行该步骤",
                                  true);
                        return;
                    }
                    if (kind == StepKind::Deploy) {
                        const std::vector<std::string> missing =
                            deployGateMissing(state.task, scanTicketNeeds(state));
                        if (!missing.empty()) {
                            setNotice(state, "请先填写：" + joinMissing(missing), true);
                            return;
                        }
                    }
                    markStepCompleted(store, state, serviceId, kind, envCopy, nowMinute);
                };
            }
        }

        std::function<void()> onRowClickWithRevoke = onRowClick;
        if (row.done) {
            if (row.config) {
                onRowClickWithRevoke = [&state, serviceId = svc.id,
                                        kind = row.configKind, scope] {
                    if (isReadOnly(state.displayStatus)) return;
                    openRevokeConfig(state, serviceId, kind, scope);
                };
            } else {
                onRowClickWithRevoke = [&state, serviceId = svc.id, kind = row.stepKind,
                                        scope] {
                    if (isReadOnly(state.displayStatus)) return;
                    openRevokeStep(state, serviceId, kind, scope);
                };
            }
        }

        std::function<void()> onView;
        if (row.config) {
            // 配置查看 / 编辑 = 页内自绘全屏编辑层（不再经 actions.viewConfig）。
            onView = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                openConfigEditor(store, state, serviceId, kind);
            };
        }

        std::function<void()> onMoveUp;
        std::function<void()> onMoveDown;
        if (row.config) {
            onMoveUp = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, -1);
            };
            onMoveDown = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, 1);
            };
        }

        std::function<void()> onSkipToggle;
        if (row.config && !readOnly) {
            onSkipToggle = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                toggleConfigSkip(store, state, serviceId, kind);
            };
        }
        drawStepRow(ui, rowId, x, rowY, width, row, i == currentIndex, readOnly,
                    configurable, true, t, onRowClickWithRevoke, onView, onMoveUp,
                    onMoveDown, onSkipToggle);

        if (row.config && configurable) {
            const float handleX = x + 8.0f + 10.0f + kStepDot + 4.0f;
            const float handleY = rowY + (kStepRowH - kDragHandleH) * 0.5f;
            drawDragHandle(
                ui, rowId + ".drag", handleX, handleY, kDragHandleW, kDragHandleH, t,
                [&state, serviceId = svc.id,
                 kind = row.configKind](const eui::PointerEvent&, const eui::Rect&) {
                    state.drag = ReleaseDetailDragState{};
                    state.drag.kind = ReleaseDetailDragKind::ConfigOrder;
                    state.drag.serviceId = serviceId;
                    state.drag.configKind = kind;
                },
                [&state](const core::dsl::DragEvent& event) {
                    if (state.drag.kind != ReleaseDetailDragKind::ConfigOrder) return;
                    state.drag.active = true;
                    const double scale = event.scale > 0.0f ? event.scale : 1.0f;
                    state.drag.deltaY = static_cast<float>(event.totalY / scale);
                    updateConfigDragTarget(state);
                },
                [&store, &state](const eui::PointerEvent& event, const eui::Rect&) {
                    finishConfigDrag(store, state,
                                     event.action == eui::PointerAction::Release);
                });
        }
    }

    // 配置拖拽的插入位指示线：画在目标边界（行间）。
    if (configurable && state.drag.kind == ReleaseDetailDragKind::ConfigOrder &&
        state.drag.active && state.drag.serviceId == svc.id) {
        const std::vector<TaskConfig> allConfigs = orderedConfigs(state, svc);
        const int source = state.drag.sourceIndex;
        const int target = state.drag.targetIndex;
        if (!allConfigs.empty() && source >= 0 && target >= 0 &&
            source < static_cast<int>(allConfigs.size())) {
            const int boundary = target < source ? target : target + 1;
            const float step = kStepRowH + kStepRowGap;
            const float firstConfigY = y + kServiceRowStart + step;
            float lineY = 0.0f;
            if (boundary < static_cast<int>(allConfigs.size())) {
                lineY = firstConfigY + static_cast<float>(boundary) * step -
                        kStepRowGap * 0.5f - kDragLineH * 0.5f;
            } else {
                lineY = firstConfigY +
                        static_cast<float>(allConfigs.size() - 1) * step + kStepRowH +
                        kStepRowGap * 0.5f - kDragLineH * 0.5f;
            }
            ui.rect(id + ".drag.insert")
                .position(x + 8.0f, lineY)
                .size(std::max(0.0f, width - 16.0f), kDragLineH)
                .color(t.colors.primary)
                .radius(1.0f)
                .build();
        }
    }
}

void drawEnvCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const TaskEnv& env, ReleaseDetailUiState& state, bool readOnly,
                 bool configurable, int nowMinute, const Tokens& t,
                 const components::theme::ThemeColorTokens& themeTokens,
                 ReleaseStore& store) {
    const float height = envCardHeight(state, env);
    drawCardBackground(ui, id + ".bg", x, y, width, height, t, themeTokens, false);

    const Progress progress = envProgress(env, state.services, state.configs, state.steps);
    const bool completed = envCompleted(env, state.services, state.configs, state.steps);
    const bool overdue = nowMinute >= 0 && nowMinute > envEndMinuteAdjusted(env) &&
                         !completed;
    const float barW = 120.0f;
    const float right = x + width - kEnvInset;
    const float barX = right - barW;
    const float barY = y + 34.0f;
    const float countW = 64.0f;
    // 「移除环境」入口（环境分组头右上角；Done / Voided 只读时隐藏）。
    // Text 级 + 错误红，点击只开二次确认，不直接写库。
    const float removeW = readOnly ? 0.0f : 64.0f;
    const float removeX = right - removeW;
    const float topRight = right - (removeW > 0.0f ? removeW + 8.0f : 0.0f);
    const float chipTextW = chipWidth("已超时", t.sizes.fontSizeSmall - 1.0f);
    const float chipX = barX - 8.0f - chipTextW;
    if (overdue) {
        drawChip(ui, id + ".overdue", chipX, y + 17.0f, "已超时",
                 eui::mixColor(t.colors.surface, t.colors.error, 0.12f),
                 t.colors.errorText, t);
    }
    if (removeW > 0.0f) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state, envId = env.id, name = env.envName] {
            openRemoveEnv(state, envId, name);
        };
        options.hasTextColorOverride = true;
        options.textColorOverride = t.colors.errorText;
        ui_kit::button(ui, id + ".remove", "移除环境", ui_kit::ButtonLevel::Text,
                       removeX, y + 8.0f, removeW, 24.0f, t, options);
    }
    ui.text(id + ".progress")
        .position(topRight - countW, y + 10.0f)
        .size(countW, 16.0f)
        .text(std::to_string(progress.done) + "/" + std::to_string(progress.total))
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(16.0f)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(completed ? t.colors.success : t.colors.textSecondary)
        .build();
    ui.stack(id + ".progress.bar")
        .position(barX, barY)
        .size(barW, 4.0f)
        .content([&] {
            components::progress(ui, id + ".progress.fill")
                .size(barW, 4.0f)
                .value(progress.total > 0
                           ? static_cast<float>(progress.done) /
                                 static_cast<float>(progress.total)
                           : 1.0f)
                .theme(themeTokens)
                .build();
        })
        .build();

    const float nameW = std::max(0.0f, (overdue ? chipX : barX) - x - kEnvInset - 8.0f);
    ui.text(id + ".name")
        .position(x + kEnvInset, y + 8.0f)
        .size(nameW, 24.0f)
        .text(env.envName)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(24.0f)
        .fontWeight(600)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    ui.text(id + ".window")
        .position(x + kEnvInset, y + 32.0f)
        .size(nameW, 16.0f)
        .text(formatTimeWindow(env.startMinute, env.endMinute))
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(overdue ? t.colors.errorText : t.colors.textTertiary)
        .build();

    const std::vector<const TaskService*> services = liveServices(state, env);
    std::vector<float> serviceTops;
    std::vector<float> serviceHeights;
    serviceTops.reserve(services.size());
    serviceHeights.reserve(services.size());
    float serviceY = y + kEnvHeaderH;
    for (std::size_t i = 0; i < services.size(); ++i) {
        const TaskService& svc = *services[i];
        serviceTops.push_back(serviceY);
        const float cardHeight = serviceCardHeight(state, svc);
        serviceHeights.push_back(cardHeight);
        drawServiceCard(ui, id + ".svc." + std::to_string(svc.id),
                        x + kEnvInset, serviceY, width - kEnvInset * 2.0f, env, svc,
                        state, readOnly, configurable, nowMinute, t, themeTokens, store);
        serviceY += cardHeight + kServiceGap;
    }

    // 服务拖拽的插入位指示线（卡片间），与被拖卡片的半透明 veil 共同表达落点。
    if (configurable && !services.empty() &&
        state.drag.kind == ReleaseDetailDragKind::ServiceOrder && state.drag.active &&
        state.drag.envId == env.id) {
        const int source = state.drag.sourceIndex;
        const int target = state.drag.targetIndex;
        if (source >= 0 && target >= 0 && source < static_cast<int>(services.size())) {
            const int boundary = target < source ? target : target + 1;
            float lineY = 0.0f;
            if (boundary < static_cast<int>(services.size())) {
                lineY = serviceTops[static_cast<std::size_t>(boundary)] -
                        kServiceGap * 0.5f - kDragLineH * 0.5f;
            } else {
                const std::size_t last = services.size() - 1;
                lineY = serviceTops[last] + serviceHeights[last] + kServiceGap * 0.5f -
                        kDragLineH * 0.5f;
            }
            ui.rect(id + ".drag.insert")
                .position(x + kEnvInset, lineY)
                .size(std::max(0.0f, width - kEnvInset * 2.0f), kDragLineH)
                .color(t.colors.primary)
                .radius(1.0f)
                .build();
        }
    }
}

void drawEnvSection(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                    bool readOnly, bool configurable, int nowMinute, const Tokens& t,
                    const components::theme::ThemeColorTokens& themeTokens,
                    ReleaseStore& store) {
    const float sectionH = envSectionHeight(state, width);
    ui.stack("release.detail.env.section")
        .size(width, sectionH)
        .content([&] {
            drawSectionTitle(ui, "release.detail.env.title", 0.0f, 0.0f, "环境与服务", t);
            if (state.envs.empty()) {
                ui.text("release.detail.env.empty")
                    .position(0.0f, kSectionTitleH + kSectionGap)
                    .size(width, 48.0f)
                    .text("任务还没有环境")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(48.0f)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }
            float envY = kSectionTitleH + kSectionGap;
            for (const auto& env : state.envs) {
                drawEnvCard(ui, "release.detail.env." + std::to_string(env.id), 0.0f,
                            envY, width, env, state, readOnly, configurable, nowMinute, t,
                            themeTokens, store);
                envY += envCardHeight(state, env) + kPrereqGap;
            }
        })
        .build();
}

// ==================== 页头 / 弹层 ====================

void drawPageHeader(eui::Ui& ui, float width, const Tokens& t,
                    ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                    const ReleaseDetailActions& actions) {
    const float headerH = ui_kit::kHeaderH;
    const TaskStatus status = state.displayStatus;
    const std::string weekday = releaseWeekdayLabel(state.task.planDate);
    const Progress progress =
        taskProgress(state.task, state.envs, state.services, state.configs, state.steps);
    const bool readOnly = isReadOnly(status);

    // 「← 返回」由模块注入 onBack 才出现（默认空 = 不占位）；Esc 返回是模块层既有路径。
    const auto backFn = actions.onBack;
    float titleX = 0.0f;
    if (backFn) {
        ui_kit::ButtonOptions backOptions;
        backOptions.onClick = backFn;
        ui_kit::button(ui, "release.detail.back", "← 返回", ui_kit::ButtonLevel::Text, 0.0f,
                       0.0f, 72.0f, headerH, t, backOptions);
        titleX = 72.0f + 8.0f;
    }

    float right = width;
    float voidX = 0.0f;
    float restoreX = 0.0f;
    float nextX = 0.0f;
    float submitX = 0.0f;
    bool showVoid = false;
    bool showRestore = false;
    bool showNext = false;
    // 「提交」只在配置中可用（Draft→Pending 的唯一入口）；其余状态不出按钮。
    const bool showSubmit = status == TaskStatus::Draft;
    if (status == TaskStatus::Voided) {
        restoreX = right - 80.0f;
        right = restoreX - 8.0f;
        showRestore = true;
    } else {
        voidX = right - 80.0f;
        right = voidX - 8.0f;
        showVoid = true;
        if (!readOnly) {
            nextX = right - 96.0f;
            right = nextX - 8.0f;
            showNext = true;
            if (showSubmit) {
                submitX = right - 88.0f;
                right = submitX - 8.0f;
            }
        }
    }
    // 「校验」任何状态可见（独立只读校验）；Done / Voided 整单只读时禁用。
    const float validateX = right - 72.0f;
    right = validateX - 8.0f;

    const std::string progressText =
        std::to_string(progress.done) + "/" + std::to_string(progress.total);
    ui.text("release.detail.progress")
        .position(right - 56.0f, 0.0f)
        .size(56.0f, headerH)
        .text(progressText)
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(headerH)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(readOnly ? t.colors.textTertiary : t.colors.textSecondary)
        .build();
    right -= 56.0f + 8.0f;

    const ChipColors colors = statusChipColors(status, t);
    const std::string statusText = releaseTaskStatusText(status);
    const float chipTextW = chipWidth(statusText, t.sizes.fontSizeSmall - 1.0f);
    drawChip(ui, "release.detail.status", right - chipTextW, (headerH - kChipH) * 0.5f,
             statusText, colors.bg, colors.fg, t);
    right -= chipTextW + 12.0f;

    // 标题宽按字号档实测（130% 字号下固定 96 会裁掉最后一个字）。
    const float titleW = measureText("上线任务", t.sizes.fontSizeTitle, 700) + 8.0f;
    const float metaX = titleX + titleW + 8.0f;
    ui.text("release.detail.title")
        .position(titleX, 0.0f)
        .size(titleW, headerH)
        .text("上线任务")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(headerH)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    const std::string meta = state.task.planDate + (weekday.empty() ? "" : "  " + weekday);
    ui.text("release.detail.meta")
        .position(metaX, 0.0f)
        .size(std::max(0.0f, right - metaX), headerH)
        .text(meta)
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(headerH)
        .maxWidth(std::max(0.0f, right - metaX))
        .overflow(core::TextOverflow::Ellipsis)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();

    if (showNext) {
        ui_kit::ButtonOptions options;
        options.onClick = [&store, &state, nowMinute] {
            performNextStep(store, state, nowMinute);
        };
        // 同一视口主按钮唯一：配置中「提交」是主动作，「下一步」降为次要按钮。
        ui_kit::button(ui, "release.detail.next", "下一步",
                       showSubmit ? ui_kit::ButtonLevel::Secondary
                                  : ui_kit::ButtonLevel::Primary,
                       nextX, 0.0f, 96.0f, headerH, t, options);
    }
    if (showSubmit) {
        ui_kit::ButtonOptions options;
        options.onClick = [&store, &state] { submitTask(store, state); };
        ui_kit::button(ui, "release.detail.submit", "提交", ui_kit::ButtonLevel::Primary,
                       submitX, 0.0f, 88.0f, headerH, t, options);
    }
    if (showVoid) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state] { state.confirm = ReleaseDetailConfirm::VoidTask; };
        ui_kit::button(ui, "release.detail.void", "作废", ui_kit::ButtonLevel::Danger,
                       voidX, 0.0f, 80.0f, headerH, t, options);
    }
    if (showRestore) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state] {
            state.confirm = ReleaseDetailConfirm::RestoreTask;
        };
        ui_kit::button(ui, "release.detail.restore", "恢复",
                       ui_kit::ButtonLevel::Secondary, restoreX, 0.0f, 80.0f, headerH, t,
                       options);
    }
    // 独立校验：只读动作，不改任务状态；只读整单时置灰且无点击动作。
    ui_kit::ButtonOptions validateOptions;
    if (!readOnly) validateOptions.onClick = [&state] { runValidation(state); };
    if (readOnly) {
        validateOptions.hasTextColorOverride = true;
        validateOptions.textColorOverride = t.colors.textDisabled;
    }
    ui_kit::button(ui, "release.detail.validate", "校验",
                   readOnly ? ui_kit::ButtonLevel::Text : ui_kit::ButtonLevel::Secondary,
                   validateX, 0.0f, 72.0f, headerH, t, validateOptions);
}

void drawNotice(eui::Ui& ui, float width, const ReleaseDetailUiState& state,
                const Tokens& t) {
    const eui::Color bg = state.noticeIsError
        ? eui::mixColor(t.colors.surface, t.colors.error, 0.10f)
        : t.colors.primarySoftBg;
    ui.stack("release.detail.notice")
        .size(width, kNoticeH)
        .content([&] {
            ui.rect("release.detail.notice.bg")
                .size(width, kNoticeH)
                .color(bg)
                .radius(t.sizes.radius)
                .build();
            ui.text("release.detail.notice.text")
                .position(12.0f, 0.0f)
                .size(std::max(0.0f, width - 24.0f), kNoticeH)
                .text(state.notice)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kNoticeH)
                .maxWidth(std::max(0.0f, width - 24.0f))
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(state.noticeIsError ? t.colors.errorText : t.colors.primaryText)
                .build();
        })
        .build();
}

// 校验清单（「提交」与页头「校验」共用）：内容 = release_logic::validateTask
// 的原样输出；只读不改库。
void drawValidationDialog(eui::Ui& ui, const eui::Screen& screen,
                          ReleaseDetailUiState& state, const Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens) {
    const float margin = themeTokens.metrics.spacing.overlay;
    const float panelW = std::min(560.0f, std::max(0.0f, screen.width - margin));
    const float panelH = std::min(420.0f, std::max(0.0f, screen.height - margin));
    auto dialog = components::dialog(ui, "release.detail.validation")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(panelW, panelH)
                      .theme(themeTokens)
                      .zIndex(1300)
                      .onOpenChange([&state](bool open) {
                          if (!open) state.confirm = ReleaseDetailConfirm::None;
                      });
    dialog.content([&, panelW, panelH] {
        ui.text("release.detail.validation.title")
            .position(24.0f, 16.0f)
            .size(std::max(0.0f, panelW - 48.0f), 26.0f)
            .text("校验未通过")
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(26.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        ui.text("release.detail.validation.hint")
            .position(24.0f, 46.0f)
            .size(std::max(0.0f, panelW - 48.0f), 20.0f)
            .text("以下 " + std::to_string(state.validationMissing.size()) +
                  " 项不完整，补齐后再继续：")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();

        const float listY = 74.0f;
        const float listH =
            std::max(0.0f, panelH - listY - ui_kit::kButtonHeight - 40.0f);
        const float itemFontSize = t.sizes.fontSizeSmall;
        const eui::Color itemColor = t.colors.errorText;
        components::scrollView(ui, "release.detail.validation.scroll")
            .position(24.0f, listY)
            .size(std::max(0.0f, panelW - 48.0f), listH)
            .gap(2.0f)
            .theme(themeTokens)
            .scrollbarOverlay()
            .content([&state, itemFontSize, itemColor](eui::Ui& inner, float innerWidth,
                                                       float) {
                for (std::size_t i = 0; i < state.validationMissing.size(); ++i) {
                    inner.text("release.detail.validation.item." + std::to_string(i))
                        .size(innerWidth, 22.0f)
                        .text("• " + state.validationMissing[i])
                        .fontSize(itemFontSize)
                        .lineHeight(22.0f)
                        .maxWidth(innerWidth)
                        .wrap(false)
                        .overflow(core::TextOverflow::Ellipsis)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(itemColor)
                        .build();
                }
            })
            .build();

        ui_kit::ButtonOptions okOptions;
        okOptions.onClick = [&state] { state.confirm = ReleaseDetailConfirm::None; };
        ui_kit::button(ui, "release.detail.validation.ok", "知道了",
                       ui_kit::ButtonLevel::Primary,
                       std::max(24.0f, panelW - 24.0f - 96.0f),
                       std::max(16.0f, panelH - 24.0f - ui_kit::kButtonHeight), 96.0f,
                       ui_kit::kButtonHeight, t, okOptions);
    });
    dialog.build();
}

// ==================== 配置编辑层（页内全屏） ====================

// 页内自绘全屏配置编辑层（不经模块回调）：多行大文本框 + 字数统计 + 保存 / 取消。
// 已跳过或整单只读（Done / Voided）时只读展示并提示原因。保存只写 content，
// 内容变化的自动撤回走 loadDetail → applyContentRevocations 的既有对账路径。
void drawConfigEditor(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseDetailUiState& state, ReleaseStore& store,
                      const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens) {
    const TaskService* svc = findService(state, state.editorServiceId);
    const TaskConfig* cfg = findConfig(state, state.editorServiceId, state.editorKind);
    if (svc == nullptr || cfg == nullptr) {
        // 目标已随重读消失（如任务被切走）：直接关层。
        state.editorOpen = false;
        return;
    }
    const bool readOnly = isReadOnly(state.displayStatus);
    const bool editable = !readOnly && !cfg->skipped;

    const float panelW = screen.width;
    const float panelH = screen.height;
    const float inset = 24.0f;
    const float titleH = 28.0f;
    const float hintH = editable ? 0.0f : 30.0f;
    const float footerH = ui_kit::kButtonHeight;
    const float footerY = panelH - inset - footerH;
    const float countH = 20.0f;
    const float inputY = inset + titleH + 12.0f + (editable ? 0.0f : hintH + 8.0f);
    const float inputH = std::max(0.0f, footerY - 8.0f - countH - 8.0f - inputY);
    const std::string title = svc->serviceName + " · " + configLabel(state.editorKind);

    auto dialog = components::dialog(ui, "release.detail.config.editor")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(panelW, panelH)
                      .theme(themeTokens)
                      .zIndex(1300)
                      .onOpenChange([&state](bool open) {
                          if (!open) state.editorOpen = false;
                      });
    dialog.content([&, panelW, inset, titleH, hintH, inputY, inputH, footerY,
                    countH, title] {
        ui.text("release.detail.config.editor.title")
            .position(inset, inset)
            .size(std::max(0.0f, panelW - inset * 2.0f), titleH)
            .text(title)
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(titleH)
            .fontWeight(600)
            .maxWidth(std::max(0.0f, panelW - inset * 2.0f))
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .color(t.colors.textPrimary)
            .build();

        if (!editable) {
            const std::string hint = cfg->skipped
                ? "该项本次已跳过，取消跳过后可编辑"
                : "任务已只读，配置内容仅可查看";
            const eui::Color bg =
                cfg->skipped ? eui::mixColor(t.colors.surface, t.colors.warning, 0.14f)
                             : t.colors.hover;
            const eui::Color fg =
                cfg->skipped ? t.colors.warning : t.colors.textTertiary;
            const float hintY = inset + titleH + 12.0f;
            ui.rect("release.detail.config.editor.hint.bg")
                .position(inset, hintY)
                .size(panelW - inset * 2.0f, hintH)
                .color(bg)
                .radius(t.sizes.radius)
                .build();
            ui.text("release.detail.config.editor.hint.text")
                .position(inset + 12.0f, hintY)
                .size(std::max(0.0f, panelW - inset * 2.0f - 24.0f), hintH)
                .text(hint)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(hintH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(fg)
                .build();
        }

        if (editable) {
            components::input(ui, "release.detail.config.editor.input")
                .position(inset, inputY)
                .size(panelW - inset * 2.0f, inputH)
                .value(state.editorText)
                .placeholder("粘贴或填写本次配置内容")
                .multiline(true)
                .scrollbar(true)
                .fontSize(t.sizes.fontSizeBody)
                .inset(10.0f)
                .theme(themeTokens)
                .onChange(
                    [&state](const std::string& value) { state.editorText = value; })
                // 输入框聚焦时 Esc 被输入框消费：与「取消」同语义（丢弃草稿关层）。
                .onCancel([&state] { state.editorOpen = false; })
                .build();
        } else {
            drawReadOnlyValue(ui, "release.detail.config.editor.view", inset, inputY,
                              panelW - inset * 2.0f, inputH, cfg->content,
                              "未填写配置内容", true, t);
        }

        ui.text("release.detail.config.editor.count")
            .position(inset, footerY - 8.0f - countH)
            .size(240.0f, countH)
            .text("共 " + std::to_string(utf8CharCount(state.editorText)) + " 字")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(countH)
            .color(t.colors.textTertiary)
            .build();

        ui_kit::ButtonOptions cancelOptions;
        cancelOptions.onClick = [&state] { state.editorOpen = false; };
        ui_kit::button(ui, "release.detail.config.editor.cancel",
                       editable ? "取消" : "关闭", ui_kit::ButtonLevel::Secondary,
                       panelW - inset - 88.0f - 8.0f - 96.0f, footerY, 88.0f, footerH,
                       t, cancelOptions);
        if (editable) {
            ui_kit::ButtonOptions saveOptions;
            saveOptions.onClick = [&store, &state] { saveConfigEditor(store, state); };
            ui_kit::button(ui, "release.detail.config.editor.save", "保存",
                           ui_kit::ButtonLevel::Primary, panelW - inset - 96.0f,
                           footerY, 96.0f, footerH, t, saveOptions);
        }
    });
    dialog.build();
}

void drawConfirmDialogs(eui::Ui& ui, const eui::Screen& screen,
                        ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                        const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
    if (state.confirm == ReleaseDetailConfirm::None) return;
    if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
        drawValidationDialog(ui, screen, state, t, themeTokens);
        return;
    }

    std::string title;
    std::string message;
    std::string primaryText;
    bool danger = false;
    if (state.confirm == ReleaseDetailConfirm::Revoke) {
        title = "撤回确认";
        message = "撤回后「" + state.revokeStepLabel + "」回到未完成，进度会重新计算。确定撤回？";
        primaryText = "确定撤回";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::VoidTask) {
        title = "作废任务";
        message = "作废后停止提醒、整单只读；之后可随时恢复。确定作废？";
        primaryText = "确定作废";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::RemoveService) {
        title = "移除服务";
        message =
            "确定把「" + state.removeName + "」从本次任务移除？进度与提醒不再包含它";
        primaryText = "确定移除";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::RemoveEnv) {
        title = "移除环境";
        message = "确定把「" + state.removeName +
                  "」从本次任务移除？环境内服务、进度与提醒不再包含它";
        primaryText = "确定移除";
        danger = true;
    } else {
        title = "恢复任务";
        message = "恢复后按当前进度与时间回到对应状态。确定恢复？";
        primaryText = "确定恢复";
    }

    components::DialogStyle style(themeTokens);
    if (danger) {
        style.primary = t.colors.error;
        style.primaryHover = eui::mixColor(t.colors.error, t.colors.textPrimary, 0.12f);
        style.primaryPressed = eui::mixColor(t.colors.error, t.colors.textPrimary, 0.24f);
    }
    auto dialog = components::dialog(ui, "release.detail.confirm")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(460.0f, 220.0f)
                      .theme(themeTokens)
                      .style(style)
                      .zIndex(1300)
                      .title(title)
                      .message(message)
                      .primaryText(primaryText)
                      .secondaryText("取消")
                      .onOpenChange([&state](bool open) {
                          if (!open) state.confirm = ReleaseDetailConfirm::None;
                      });
    if (state.confirm == ReleaseDetailConfirm::RestoreTask) {
        dialog.onPrimary([&store, &state, nowMinute] {
            applyRestore(store, state, nowMinute);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::VoidTask) {
        dialog.onPrimary([&store, &state] {
            applyVoid(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::RemoveService) {
        dialog.onPrimary([&store, &state] {
            applyRemoveService(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::RemoveEnv) {
        dialog.onPrimary([&store, &state] {
            applyRemoveEnv(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else {
        dialog.onPrimary([&store, &state] {
            applyRevoke(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    }
    dialog.onSecondary([&state] { state.confirm = ReleaseDetailConfirm::None; });
    dialog.build();
}

// ==================== 数据加载 ====================

// 配置内容 / 服务分支保存后（宿主置 state.dirty → 本页重读）对比旧快照：内容变了
// 旧确认作废——对应步骤已完成则自动撤回并记 action=revoke 日志（不弹二次确认，
// 因为内容已经改了）；未完成不动。只对同一任务的重读生效，切换任务不是修改。
// 配置步骤以 TaskConfig.doneAt 记账（release_logic 忽略 TaskStep 的 Config 行）。
void applyContentRevocations(ReleaseStore& store, ReleaseDetailUiState& state,
                             const std::vector<TaskService>& previousServices,
                             const std::vector<TaskConfig>& previousConfigs) {
    if (isReadOnly(state.displayStatus)) return;
    int revoked = 0;
    bool failed = false;

    for (const auto& prev : previousServices) {
        if (prev.removed) continue;
        const TaskService* current = findService(state, prev.id);
        if (current == nullptr || current->removed) continue;
        if (current->branch == prev.branch) continue;
        if (!stepDone(state, current->id, StepKind::MergeBranch)) continue;
        auto result = store.setStepDone(current->id, StepKind::MergeBranch, 0);
        if (result.isFail()) {
            setNotice(state, "自动撤回合并分支失败：" + result.errorMessage(), true);
            failed = true;
            continue;
        }
        for (auto& step : state.steps) {
            if (step.taskServiceId == current->id && step.kind == StepKind::MergeBranch) {
                step.doneAt = 0;
            }
        }
        if (!appendActionLog(store, state, current->serviceName,
                             stepLabel(StepKind::MergeBranch), "revoke",
                             "服务分支已修改")) {
            failed = true;
        }
        ++revoked;
    }

    for (const auto& prev : previousConfigs) {
        if (prev.skipped || prev.id == 0) continue;  // 占位行没有已确认状态可撤
        const TaskService* svc = findService(state, prev.taskServiceId);
        if (svc == nullptr || svc->removed) continue;
        for (auto& cfg : state.configs) {
            if (cfg.taskServiceId != prev.taskServiceId || cfg.kind != prev.kind) continue;
            if (cfg.skipped || cfg.content == prev.content || cfg.doneAt == 0) break;
            TaskConfig next = cfg;
            next.doneAt = 0;
            auto result = store.upsertTaskConfig(next);
            if (result.isFail()) {
                setNotice(state, "自动撤回配置步骤失败：" + result.errorMessage(), true);
                failed = true;
                break;
            }
            cfg.doneAt = 0;
            if (!appendActionLog(store, state, svc->serviceName, configLabel(cfg.kind),
                                 "revoke", "配置内容已修改")) {
                failed = true;
            }
            ++revoked;
            break;
        }
    }

    if (revoked > 0 && !failed) {
        setNotice(state, "配置已修改，已自动撤回 " + std::to_string(revoked) + " 项旧确认",
                  false);
    }
}

void loadDetail(ReleaseStore& store, ReleaseDetailUiState& state) {
    // 同一任务重读 = 其它层保存后的刷新：先留旧快照，加载完成后对比并自动撤回
    // （见 applyContentRevocations）。切换任务没有可比的旧任务，不做对比。
    const bool compareContentChanges =
        !state.loadedTaskId.empty() && state.loadedTaskId == state.taskId;
    std::vector<TaskService> previousServices;
    std::vector<TaskConfig> previousConfigs;
    if (compareContentChanges) {
        previousServices = state.services;
        previousConfigs = state.configs;
    }
    // 切任务时清掉上一个任务的反馈与未决确认，避免把旧任务的撤回目标套到新任务上；
    // 编辑层目标（editorServiceId）同理作废，防止对旧任务的服务写库。
    if (state.loadedTaskId != state.taskId) {
        state.notice.clear();
        state.noticeIsError = false;
        state.confirm = ReleaseDetailConfirm::None;
        state.validationActive = false;
        state.validationMissing.clear();
        state.editorOpen = false;
        state.editorText.clear();
    }
    state.loadedTaskId = state.taskId;
    state.dirty = false;
    state.loadError.clear();
    state.task = Task{};
    state.displayStatus = TaskStatus::Draft;
    state.statusWriteFailed = false;
    state.envs.clear();
    state.services.clear();
    state.configs.clear();
    state.steps.clear();
    state.logs.clear();
    state.drag = ReleaseDetailDragState{};

    std::int64_t taskId = 0;
    if (!parseTaskId(state.taskId, &taskId)) {
        state.loadError = "任务 id 无效";
        return;
    }

    auto tasks = store.listTasks();
    if (tasks.isFail()) {
        state.loadError = "读取任务失败：" + tasks.errorMessage();
        return;
    }
    const Task* found = nullptr;
    for (const auto& task : tasks.value()) {
        if (task.id == taskId) {
            found = &task;
            break;
        }
    }
    if (found == nullptr) {
        state.loadError = "任务不存在";
        return;
    }
    state.task = *found;
    state.displayStatus = state.task.status;

    // 服务顺序还原只消费日志；读库顺序（at ASC, id ASC）即「最后一条 reorder」的判据。
    auto logs = store.listLogs(taskId);
    if (logs.isFail()) {
        state.loadError = "读取任务日志失败：" + logs.errorMessage();
        return;
    }
    state.logs = logs.value();

    auto envs = store.listTaskEnvs(taskId);
    if (envs.isFail()) {
        state.loadError = "读取任务环境失败：" + envs.errorMessage();
        return;
    }
    for (const auto& env : envs.value()) {
        if (env.removed) continue;
        state.envs.push_back(env);

        auto services = store.listTaskServices(env.id);
        if (services.isFail()) {
            state.loadError = "读取环境服务失败：" + services.errorMessage();
            return;
        }
        for (const auto& svc : services.value()) {
            state.services.push_back(svc);
            if (svc.removed) continue;
            auto configs = store.listTaskConfigs(svc.id);
            if (configs.isFail()) {
                state.loadError = "读取服务配置失败：" + configs.errorMessage();
                return;
            }
            for (const auto& cfg : configs.value()) state.configs.push_back(cfg);
            auto steps = store.listTaskSteps(svc.id);
            if (steps.isFail()) {
                state.loadError = "读取服务步骤失败：" + steps.errorMessage();
                return;
            }
            for (const auto& step : steps.value()) state.steps.push_back(step);
        }
    }

    // 内容已变的步骤先撤回再重算校验：以撤回后的最新数据为准。
    if (compareContentChanges) {
        applyContentRevocations(store, state, previousServices, previousConfigs);
    }

    // 校验结果随数据变化重算：规则只来自 release_logic，UI 不自行推断哪些项算缺。
    if (state.validationActive) {
        state.validationMissing =
            validateTask(state.services, configsForValidation(state));
        if (state.validationMissing.empty()) {
            state.validationActive = false;
            if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
                state.confirm = ReleaseDetailConfirm::None;
            }
        }
    }
}

void drawErrorPage(eui::Ui& ui, const eui::Screen& screen,
                   const ReleaseDetailUiState& state, const Tokens& t) {
    const float pad = t.sizes.spacingLarge * 2.0f;
    ui.column("release.detail.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            ui.text("release.detail.title")
                .size(std::max(0.0f, screen.width - pad * 2.0f), ui_kit::kHeaderH)
                .text("上线任务")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(ui_kit::kHeaderH)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .build();
            ui.text("release.detail.error")
                .size(std::max(0.0f, screen.width - pad * 2.0f), 24.0f)
                .text(state.loadError)
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .color(t.colors.errorText)
                .build();
        })
        .build();
}

} // namespace

void composeReleaseDetailPage(eui::Ui& ui, const eui::Screen& screen,
                              ReleaseStore& store, ReleaseDetailUiState& state,
                              const ReleaseDetailActions& actions) {
    const bool taskSwitched = state.loadedTaskId != state.taskId;
    if (state.dirty || taskSwitched) loadDetail(store, state);
    if (taskSwitched) {
        // 切任务时回到顶部：沿用上一个任务的滚动位置会落在新任务中段。
        ui.setScrollOffset("release.detail.scroll", 0.0f);
    }

    const auto& t = core::theme::current();
    const float pad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pad * 2.0f);

    if (!state.loadError.empty()) {
        drawErrorPage(ui, screen, state, t);
        return;
    }

    const int nowMinute = planMinuteNow(state.task.planDate);
    advanceTaskStatus(store, state, nowMinute);

    const bool readOnly = isReadOnly(state.displayStatus);
    const bool configurable = !readOnly && isConfigPhase(state.displayStatus);
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const bool hasNotice = !state.notice.empty();
    // 反馈条固定在页头下方（不随内容滚动），用户在清单底部操作失败时也能看到原因。
    const float noticeBlockH = hasNotice ? kNoticeH + t.sizes.spacingLarge : 0.0f;
    const float bodyH = std::max(
        0.0f, screen.height - pad - ui_kit::kHeaderH - t.sizes.spacingLarge - noticeBlockH);

    const TicketNeeds needs = scanTicketNeeds(state);
    // 高度只由可见行数决定；把「未移除服务 + 每个服务的步骤行数」纳入 key，
    // 否则移除一个无配置/无步骤的服务时计数不变，滚动区会继续用旧内容高。
    std::size_t visibleRows = 0;
    std::size_t liveServiceCount = 0;
    for (const auto& svc : state.services) {
        if (svc.removed) continue;
        ++liveServiceCount;
        visibleRows += 3 + orderedConfigs(state, svc).size();
    }
    const std::string contentKey =
        "release.detail." + state.taskId + "." +
        std::to_string(state.envs.size()) + "." + std::to_string(liveServiceCount) +
        "." + std::to_string(visibleRows) + "." +
        std::to_string(state.configs.size()) + "." +
        std::to_string(state.steps.size()) + "." +
        (needs.nacos ? "1" : "0") + (needs.db ? "1" : "0") + "." +
        releaseTaskStatusText(state.displayStatus);

    ui.column("release.detail.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            ui.stack("release.detail.header")
                .size(contentW, ui_kit::kHeaderH)
                .content([&] {
                    drawPageHeader(ui, contentW, t, state, store, nowMinute, actions);
                })
                .build();

            if (hasNotice) {
                drawNotice(ui, contentW, state, t);
            }

            components::scrollView(ui, "release.detail.scroll")
                .size(contentW, bodyH)
                .gap(t.sizes.spacingLarge)
                .step(60.0f)
                .theme(themeTokens)
                .scrollbarOverlay()
                .contentKey(contentKey)
                .content([&](eui::Ui& inner, float innerWidth, float viewportHeight) {
                    (void)viewportHeight;
                    drawPrereqSection(inner, innerWidth, state, readOnly, nowMinute, t,
                                      themeTokens, store);
                    drawEnvSection(inner, innerWidth, state, readOnly, configurable,
                                   nowMinute, t, themeTokens, store);
                })
                .build();
        })
        .build();

    drawConfirmDialogs(ui, screen, state, store, nowMinute, t, themeTokens);
    if (state.editorOpen) {
        drawConfigEditor(ui, screen, state, store, t, themeTokens);
    }
}

} // namespace module::release