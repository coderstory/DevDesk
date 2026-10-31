// release_detail_logic.cpp
// 从 release_detail_ui.cpp 拆出（2026-10-31）：状态查询与动作层——前置/步骤/配置
// 读写、拖拽落位、提交与状态推进、撤回与作废恢复。纯数据与状态变更，不含绘制。

#include "release_detail_internal.h"

#include "release_config_ui.h"  // formatMinuteOfDay / configItemLabels
#include "release_list_ui.h"    // releaseTaskStatusText

#include <algorithm>
#include <ctime>
#include <limits>

namespace module::release {
namespace detail {

bool isReadOnly(TaskStatus status) {
    return status == TaskStatus::Done || status == TaskStatus::Voided;
}

bool isEditMode(TaskStatus status) {
    // 池 53 双模式：编辑域只认 Draft。Pending 并入发布态（F-03 废除 issue 06
    // 「提交后仍可改配置」）；发布态 = Pending + Running，归档态 = Done + Voided。
    return status == TaskStatus::Draft;
}

bool isPublishMode(TaskStatus status) {
    return status == TaskStatus::Pending || status == TaskStatus::Running;
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
    state.noticeAt = static_cast<std::int64_t>(core::time::nowUnix());
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
    if (!isEditMode(state.displayStatus) || isReadOnly(state.displayStatus)) {
        setNotice(state, "发布中任务不能调整配置顺序，请撤回提交或作废后操作", true);
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

// 前置项标红：文案与 release_logic::validateTaskSubmission 的拼装逐字一致
//（同一契约的另一侧，改措辞必须同步）。

bool prereqFlagged(const ReleaseDetailUiState& state, PrereqKey key) {
    if (!state.validationActive) return false;
    const char* message = nullptr;
    switch (key) {
        case PrereqKey::Pretest: message = "提测单号未填写"; break;
        case PrereqKey::Cab: message = "CAB 单号未填写"; break;
        case PrereqKey::Nacos: message = "nacos 工单号未填写"; break;
        case PrereqKey::Db: message = "数据库工单号未填写"; break;
        case PrereqKey::Announce: message = "上线公告内容未填写"; break;
    }
    return std::find(state.validationMissing.begin(), state.validationMissing.end(),
                     std::string(message)) != state.validationMissing.end();
}

// 提交：Draft → Pending。校验只认 release_logic::validateTaskSubmission 的结果
//（F-20：内容齐备 + 前置单号/公告齐备），缺失清单原样留作标红依据；
// 通过才写库并记一条 action=submit 的日志。

void submitTask(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (state.task.id == 0) return;
    if (state.displayStatus != TaskStatus::Draft) {
        setNotice(state, "只有「配置中」的任务可以提交", true);
        return;
    }
    const std::vector<std::string> missing = validateTaskSubmission(
        state.task, state.envs, state.services, configsForValidation(state));
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

// 撤回提交（F-02）：Pending → Draft 的状态回退，**不是数据清理**——已勾确认
//（前置 ok 标志 / 公告时间 / 步骤 doneAt / 配置 doneAt）与既有日志原样保留，
// 仅 release_task.status 落回 draft；applyRestore 以「日志存在 submit」判已提交，
// 撤回不删该日志，作废→恢复链不受影响。Running 起拒绝：执行留痕不可回退。

void applyWithdrawSubmission(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (state.task.id == 0) return;
    if (state.displayStatus != TaskStatus::Pending) {
        setNotice(state, isPublishMode(state.displayStatus)
                             ? "任务已开始执行，不能撤回提交"
                             : "只有「待执行」的任务可以撤回提交",
                  true);
        return;
    }
    Task next = state.task;
    next.status = TaskStatus::Draft;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "撤回提交失败：" + result.errorMessage(), true);
        return;
    }
    state.task = next;
    state.displayStatus = TaskStatus::Draft;
    state.validationActive = false;
    state.validationMissing.clear();
    appendActionLog(store, state, "任务", "提交", "revoke", "Pending→Draft 撤回提交");
    state.dirty = true;
    setNotice(state, "已撤回提交，可继续编辑", false);
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
    // 与提交同一门禁（F-20）：Draft 的「校验」是提交前预演；发布态的只读校验
    // 走内容检查（validateTask），前置在提交时已齐备，不再重复报。
    const std::vector<std::string> missing = isEditMode(state.displayStatus)
        ? validateTaskSubmission(state.task, state.envs, state.services,
                                 configsForValidation(state))
        : validateTask(state.services, configsForValidation(state));
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
    if (!isEditMode(state.displayStatus) || isReadOnly(state.displayStatus)) {
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
    if (!isEditMode(state.displayStatus) || isReadOnly(state.displayStatus)) {
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
    const std::string fromStatus = releaseTaskStatusText(state.task.status);
    Task next = state.task;
    next.status = TaskStatus::Voided;
    auto result = store.updateTask(next);
    if (result.isFail()) {
        setNotice(state, "作废失败：" + result.errorMessage(), true);
        return;
    }
    state.task = next;
    state.displayStatus = TaskStatus::Voided;
    // 写库已成功：清掉历史推进失败标志，避免残留挡下后续所有状态推进。
    state.statusWriteFailed = false;
    state.validationActive = false;
    state.validationMissing.clear();
    appendActionLog(store, state, "任务", "作废", "void",
                    "由「" + fromStatus + "」改为「" +
                        releaseTaskStatusText(TaskStatus::Voided) + "」");
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
    // 写库已成功：清掉历史推进失败标志，避免残留挡下后续所有状态推进。
    state.statusWriteFailed = false;
    state.validationActive = false;
    state.validationMissing.clear();
    appendActionLog(store, state, "任务", "恢复", "restore",
                    "由「" + releaseTaskStatusText(TaskStatus::Voided) + "」恢复为「" +
                        releaseTaskStatusText(next.status) + "」");
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
    if (!isEditMode(state.displayStatus)) {
        setNotice(state, isReadOnly(state.displayStatus)
                             ? "任务已只读，不能跳过"
                             : "发布中任务不能跳过配置，请撤回提交或作废后操作",
                  true);
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
// 只读态（Done/Voided）跳过补建——对已终态任务写库属范围外副作用，查看以清单占位行展示为准。

void openConfigEditor(ReleaseStore& store, ReleaseDetailUiState& state,
                      std::int64_t serviceId, ConfigKind kind) {
    // 发布态禁止进入编辑层（F-03 只读）；归档态只读查看放行。
    if (isPublishMode(state.displayStatus)) {
        setNotice(state, "发布中任务不能编辑配置，请撤回提交或作废后操作", true);
        return;
    }
    if (!isReadOnly(state.displayStatus)) {
        if (!ensureServiceConfigs(store, state, serviceId)) return;
    }
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
        // 保留编辑层与缓冲内容：用户可重试或主动取消，整段输入不丢。
        setNotice(state, "保存配置失败：" + result.errorMessage(), true);
        return;
    }
    state.editorOpen = false;
    state.dirty = true;
    setNotice(state, "已保存「" + configLabel(state.editorKind) + "」配置内容", false);
}

// ==================== 分支编辑（池 54 / F-54-2） ====================

// 分支名合法性（记录台口径）：只拦会破版显示的空格/控制字符与超长，
// 不做完整 git-check-ref-format 校验（本模块定位记录台，不执行 git）。
// 调用前先 trim；空/纯空白合法——「分支未填写」是提交校验的可标记态，
// 由 validateTask 拦提交，不在此拦保存。

std::string isIllegalBranchName(const std::string& trimmedValue) {
    if (trimmedValue.size() > 200) return "分支名过长（上限 200 字符）";
    for (const unsigned char c : trimmedValue) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) {
            return "分支名不能包含空格或控制字符";
        }
    }
    return "";
}

// 登记分支编辑缓冲（服务 id + 输入全文）。刻意不写 state.services：
// 缓冲与内存隔离，落库成功后置 dirty 重读统一进内存并触发对账——若 onChange
// 直改内存，重读时 prev==DB 对账空转，「合并分支」旧确认不会撤（saveConfigEditor
// 同一条契约，见 release_detail_ui.h 头注释）。

void beginBranchEdit(ReleaseDetailUiState& state, std::int64_t serviceId,
                     const std::string& value) {
    state.branchEditServiceId = serviceId;
    state.branchEditText = value;
}

// 提交分支缓冲（blur / Enter / Esc 三路同语义，由 draw 侧输入框回调统一调）。
// 无缓冲 = 仅聚焦未输入，静默返回；trim 后与现值等值不打库不置 dirty（避免
// 每次失焦空转重读）。成功清缓冲置 dirty：重读对账发现分支变更时自动撤回该
// 服务「合并分支」旧确认并记 revoke 日志「服务分支已修改」（既有，零新增）。

void applySaveBranch(ReleaseStore& store, ReleaseDetailUiState& state) {
    if (state.branchEditServiceId == 0) return;
    const std::int64_t serviceId = state.branchEditServiceId;
    if (!isEditMode(state.displayStatus)) {
        // 发布态只读铁律（F-03）的纵深防御：UI 层发布态不渲染输入框，
        // 越权调用（测试 / 回归路径）在此拒绝；缓冲一并作废。
        setNotice(state, isReadOnly(state.displayStatus)
                             ? "任务已只读，不能修改分支"
                             : "发布中任务不能修改分支，请撤回提交或作废后操作",
                  true);
        state.branchEditServiceId = 0;
        state.branchEditText.clear();
        return;
    }
    const TaskService* current = findService(state, serviceId);
    if (current == nullptr) {
        // 缓冲挂的服务行已不在（并发移除后重读）：无可重试对象，缓冲作废。
        setNotice(state, "服务不存在，无法保存分支", true);
        state.branchEditServiceId = 0;
        state.branchEditText.clear();
        return;
    }
    const std::string value = trimmed(state.branchEditText);
    if (value == current->branch) {
        state.branchEditServiceId = 0;
        state.branchEditText.clear();
        return;
    }
    const std::string illegal = isIllegalBranchName(value);
    if (!illegal.empty()) {
        setNotice(state, illegal, true);
        return;  // 缓冲保留：可直接改后重试
    }
    TaskService next = *current;  // 整行写接口：其余字段原样透传
    next.branch = value;
    auto result = store.updateTaskService(next);
    if (result.isFail()) {
        // 保留缓冲与输入：与 saveConfigEditor 同款，写库失败不丢输入。
        setNotice(state, "保存分支失败：" + result.errorMessage(), true);
        return;
    }
    state.branchEditServiceId = 0;
    state.branchEditText.clear();
    state.dirty = true;
    setNotice(state, "已保存「" + current->serviceName + "」分支", false);
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
    if (!isEditMode(state.displayStatus)) {
        setNotice(state, isReadOnly(state.displayStatus)
                             ? "任务已只读，不能移除服务"
                             : "发布中任务不能移除服务，请撤回提交或作废后操作",
                  true);
        return;
    }
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
    if (!isEditMode(state.displayStatus)) {
        setNotice(state, isReadOnly(state.displayStatus)
                             ? "任务已只读，不能移除环境"
                             : "发布中任务不能移除环境，请撤回提交或作废后操作",
                  true);
        return;
    }
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
    // 轨道 = 发布态专属（2026-10-31 审计裁决）：编辑态主动作是提交，不放轨道引导。
    if (!isPublishMode(state.displayStatus)) {
        setNotice(state, isReadOnly(state.displayStatus)
                             ? "任务已只读，不能推进"
                             : "任务尚未提交，先完成内容并提交后再执行",
                  true);
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

// ==================== 发布态流程轨道（F-04 / F-14 / F-23） ====================

// 轨道序 = performNextStep 既有固定序（不新造）：任务前置 → 各环境（服务按任务内
// 顺序：合并分支 → 配置 n 项 → 部署 → 验证）。drawPublishTrack、「下一步」按钮文案
// 与 F-23 锚点偏移都消费同一份行清单——顺序与高度档单一来源，禁止另排一套。
// 折叠：完成组默认收起、当前步所在组强制展开；trackGroupState 存手动覆盖，
// 重读（状态变化）时由 loadDetail 清空重算默认态。

std::vector<TrackRowView> collectTrackRows(const ReleaseDetailUiState& state,
                                           int nowMinute) {
    std::vector<TrackRowView> rows;
    const TicketNeeds needs = scanTicketNeeds(state);

    auto pushPrereq = [&](PrereqKey key, const std::string& value, bool checked,
                          bool gateOpen) {
        TrackRowView row;
        row.kind = TrackRowKind::Step;
        row.groupId = "prereq";
        row.label = prereqTitle(key);
        row.valueText = value;
        row.node = checked ? TrackNodeState::Done
                           : (gateOpen ? TrackNodeState::UnlockedTodo
                                       : TrackNodeState::Locked);
        row.statusText = checked ? std::string("已完成")
                                 : (gateOpen ? std::string()
                                             : std::string(key == PrereqKey::Announce
                                                               ? "请先填写公告内容"
                                                               : "请先填写单据号"));
        row.confirmable = !checked && gateOpen;
        row.revocable = checked;
        row.prereqKey = key;
        row.isAnnounce = key == PrereqKey::Announce;
        row.height = kTrackRowH;
        rows.push_back(std::move(row));
    };
    pushPrereq(PrereqKey::Pretest, state.task.pretestNo, state.task.pretestOk,
               prereqGateOpen(state, PrereqKey::Pretest));
    pushPrereq(PrereqKey::Cab, state.task.cabNo, state.task.cabOk,
               prereqGateOpen(state, PrereqKey::Cab));
    if (needs.nacos) {
        pushPrereq(PrereqKey::Nacos, state.task.ticketNacosNo, state.task.ticketNacosOk,
                   prereqGateOpen(state, PrereqKey::Nacos));
    }
    if (needs.db) {
        pushPrereq(PrereqKey::Db, state.task.ticketDbNo, state.task.ticketDbOk,
                   prereqGateOpen(state, PrereqKey::Db));
    }
    pushPrereq(PrereqKey::Announce, state.task.announceText,
               state.task.announceSentAt != 0, prereqGateOpen(state, PrereqKey::Announce));

    // 前置组头插在最前。
    {
        rows.insert(rows.begin(), TrackRowView{});
        TrackRowView& head = rows.front();
        head.kind = TrackRowKind::EnvGroupHeader;
        head.groupId = "prereq";
        head.title = "任务前置";
        int done = 0;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            if (rows[i].groupId == "prereq" && rows[i].node == TrackNodeState::Done) {
                ++done;
            }
        }
        head.progressText = std::to_string(done) + "/" + std::to_string(rows.size() - 1);
        head.groupDone = done == static_cast<int>(rows.size()) - 1;
        head.groupOverdue = false;
        head.height = kTrackGroupH;
    }

    for (const auto& env : state.envs) {
        if (env.removed) continue;
        const std::vector<const TaskService*> services = liveServices(state, env);
        const Progress progress = envProgress(env, state.services, state.configs, state.steps);
        TrackRowView header;
        header.kind = TrackRowKind::EnvGroupHeader;
        header.groupId = "env." + std::to_string(env.id);
        header.title = env.envName;
        header.progressText =
            std::to_string(progress.done) + "/" + std::to_string(progress.total);
        header.groupDone = progress.total == 0 || progress.done == progress.total;
        header.groupOverdue = nowMinute >= 0 && nowMinute > envEndMinuteAdjusted(env) &&
                              !header.groupDone;
        header.envId = env.id;
        header.height = kTrackGroupH;
        rows.push_back(std::move(header));

        for (const TaskService* service : services) {
            const TaskService& svc = *service;
            TrackRowView svcLabel;
            svcLabel.kind = TrackRowKind::ServiceLabel;
            svcLabel.groupId = header.groupId;
            svcLabel.title = svc.serviceName;
            svcLabel.height = kTrackServiceH;
            rows.push_back(std::move(svcLabel));

            auto pushStep = [&](TrackRowView row) {
                row.groupId = header.groupId;
                row.envId = env.id;
                row.serviceId = svc.id;
                row.height = kTrackRowH;
                rows.push_back(std::move(row));
            };
            {
                TrackRowView row;
                row.label = stepLabel(StepKind::MergeBranch);
                row.stepKind = StepKind::MergeBranch;
                row.done = stepDone(state, svc.id, StepKind::MergeBranch);
                row.node = row.done ? TrackNodeState::Done : TrackNodeState::UnlockedTodo;
                row.statusText = row.done ? "已完成" : "";
                row.confirmable = !row.done;
                row.revocable = row.done;
                pushStep(std::move(row));
            }
            for (const auto& cfg : orderedConfigs(state, svc)) {
                TrackRowView row;
                row.config = true;
                row.configKind = cfg.kind;
                row.label = configLabel(cfg.kind);
                row.done = !cfg.skipped && cfg.doneAt != 0;
                if (cfg.skipped) {
                    row.node = TrackNodeState::Skipped;
                    row.statusText = "已跳过";
                } else if (row.done) {
                    row.node = TrackNodeState::Done;
                    row.statusText = "已完成";
                    row.revocable = true;
                } else if (nowMinute < 0 || nowMinute < env.startMinute) {
                    row.node = TrackNodeState::Locked;
                    row.statusText = formatPlanMinute(env.startMinute) + " 解锁";
                } else if (!configGateMissing(state.task, cfg.kind).empty()) {
                    row.node = TrackNodeState::Locked;
                    row.statusText = "请先填写工单单号";
                } else {
                    row.node = TrackNodeState::UnlockedTodo;
                    row.confirmable = true;
                }
                pushStep(std::move(row));
            }
            {
                TrackRowView row;
                row.label = stepLabel(StepKind::Deploy);
                row.stepKind = StepKind::Deploy;
                row.done = stepDone(state, svc.id, StepKind::Deploy);
                if (row.done) {
                    row.node = TrackNodeState::Done;
                    row.statusText = "已完成";
                    row.revocable = true;
                } else if (nowMinute < 0 || nowMinute < env.startMinute) {
                    row.node = TrackNodeState::Locked;
                    row.statusText = formatPlanMinute(env.startMinute) + " 解锁";
                } else if (!deployGateMissing(state.task, needs).empty()) {
                    row.node = TrackNodeState::Locked;
                    row.statusText = "请先填写工单单号";
                } else {
                    row.node = TrackNodeState::UnlockedTodo;
                    row.confirmable = true;
                }
                pushStep(std::move(row));
            }
            {
                TrackRowView row;
                row.label = stepLabel(StepKind::Verify);
                row.stepKind = StepKind::Verify;
                row.done = stepDone(state, svc.id, StepKind::Verify);
                if (row.done) {
                    row.node = TrackNodeState::Done;
                    row.statusText = "已完成";
                    row.revocable = true;
                } else if (nowMinute < 0 || nowMinute < env.startMinute) {
                    row.node = TrackNodeState::Locked;
                    row.statusText = formatPlanMinute(env.startMinute) + " 解锁";
                } else {
                    row.node = TrackNodeState::UnlockedTodo;
                    row.confirmable = true;
                }
                pushStep(std::move(row));
            }
        }
    }

    // 当前步 = 轨道里第一个未完成且已解锁的行（唯一绿染）。
    std::string currentGroup;
    for (auto& row : rows) {
        if (row.kind == TrackRowKind::Step && row.node == TrackNodeState::UnlockedTodo) {
            row.node = TrackNodeState::Current;
            currentGroup = row.groupId;
            break;
        }
    }
    // 折叠：缺省 = 完成组收起；手动覆盖以 trackGroupState 为准；当前步组强制展开。
    for (auto& row : rows) {
        if (row.kind != TrackRowKind::EnvGroupHeader) continue;
        const bool collapsed =
            row.groupId != currentGroup &&
            (state.trackGroupState.count(row.groupId)
                 ? state.trackGroupState.at(row.groupId)
                 : row.groupDone);
        if (!collapsed) continue;
        for (auto& other : rows) {
            if (other.kind != TrackRowKind::EnvGroupHeader &&
                other.groupId == row.groupId) {
                other.hidden = true;
            }
        }
    }
    return rows;
}

// F-23 锚点：当前步行顶部的内容偏移（= 可见行前缀和 − 24px 余量，clamp ≥ 0）。
// 与 drawPublishTrack 用同一份行清单与同一组高度档，折叠隐藏行不计入。

float trackCurrentStepOffset(const std::vector<TrackRowView>& rows) {
    float offset = 0.0f;
    for (const auto& row : rows) {
        if (row.kind == TrackRowKind::Step && row.node == TrackNodeState::Current) {
            return std::max(0.0f, offset - 24.0f);
        }
        if (!row.hidden) offset += row.height + kTrackRowGap;
    }
    return 0.0f;
}

// ==================== 绘制辅助 ====================

}  // namespace detail
}  // namespace module::release
