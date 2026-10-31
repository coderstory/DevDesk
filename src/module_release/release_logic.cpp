// 上线域纯逻辑实现：只读入参、只返回结果（无 IO / 无异常 / 无静态可变状态）。
#include "release_logic.h"

#include <cstddef>
#include <ctime>

namespace module::release {

namespace {

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// 严格解析 YYYY-MM-DD：四位年 + 两位月 + 两位日，且日期真实存在。
bool parseIsoDate(const std::string& isoDate, int& year, int& month, int& day) {
    if (isoDate.size() != 10 || isoDate[4] != '-' || isoDate[7] != '-') return false;
    for (std::size_t i = 0; i < isoDate.size(); ++i) {
        if (i == 4 || i == 7) continue;
        const char c = isoDate[i];
        if (c < '0' || c > '9') return false;
    }
    year = (isoDate[0] - '0') * 1000 + (isoDate[1] - '0') * 100 +
           (isoDate[2] - '0') * 10 + (isoDate[3] - '0');
    month = (isoDate[5] - '0') * 10 + (isoDate[6] - '0');
    day = (isoDate[8] - '0') * 10 + (isoDate[9] - '0');
    if (month < 1 || month > 12 || day < 1) return false;
    static const int kDaysPerMonth[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    int maxDay = kDaysPerMonth[month - 1];
    if (month == 2 && isLeapYear(year)) maxDay = 29;
    return day <= maxDay;
}

// 向负无穷取整的除法：负数分钟也要「欠整分钟」，与 mktime 前的余数语义对齐。
std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) --quotient;
    return quotient;
}

// 公历日期 → 1970-01-01 起的天数（Howard Hinnant days_from_civil，纯整数、无时区）。
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - static_cast<int>(era) * 400;
    const int mp = (m > 2) ? m - 3 : m + 9;
    const int doy = (153 * mp + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// 工单卡适用性：只有「未移除环境下的未移除服务」填了对应类型的非跳过内容才欠工单。
struct TicketNeeds {
    bool nacos = false;
    bool db = false;
};

TicketNeeds ticketNeeds(const std::vector<TaskEnv>& envs,
                        const std::vector<TaskService>& services,
                        const std::vector<TaskConfig>& configs) {
    TicketNeeds needs;
    for (const auto& svc : services) {
        if (svc.removed) continue;
        bool envLive = false;
        for (const auto& env : envs) {
            if (env.id == svc.taskEnvId && !env.removed) {
                envLive = true;
                break;
            }
        }
        if (!envLive) continue;
        for (const auto& cfg : configs) {
            if (cfg.taskServiceId != svc.id) continue;
            if (cfg.skipped || cfg.content.empty()) continue;
            if (cfg.kind == ConfigKind::Nacos) needs.nacos = true;
            if (cfg.kind == ConfigKind::Db) needs.db = true;
        }
    }
    return needs;
}

const char* configKindLabel(ConfigKind kind) {
    switch (kind) {
        case ConfigKind::Nacos: return "nacos";
        case ConfigKind::Db: return "数据库";
        case ConfigKind::Cron: return "定时任务";
        case ConfigKind::Menu: return "菜单";
        case ConfigKind::Gateway: return "网关";
    }
    return "配置";
}

// 某服务某类配置槽位是否已关闭：存在完成行（doneAt != 0）或存在跳过行。
// 行缺失 → 未关闭（按未完成计）；同 kind 多行按槽位去重，不重复计。
bool configSlotClosed(const std::vector<TaskConfig>& configs,
                      std::int64_t taskServiceId, ConfigKind kind) {
    for (const auto& cfg : configs) {
        if (cfg.taskServiceId != taskServiceId || cfg.kind != kind) continue;
        if (cfg.doneAt != 0 || cfg.skipped) return true;
    }
    return false;
}

} // namespace

bool isRegularReleaseDay(const std::string& isoDate) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parseIsoDate(isoDate, year, month, day)) return false;
    // 0 = 周日；1970-01-01（0 天）是周四 → 偏移 +4
    const std::int64_t days = daysFromCivil(year, month, day);
    const int weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);
    return weekday == 2 /*周二*/ || weekday == 4 /*周四*/;
}

bool isValidIsoDate(const std::string& isoDate) {
    int year = 0;
    int month = 0;
    int day = 0;
    return parseIsoDate(isoDate, year, month, day);
}

std::int64_t taskBaseline(const std::vector<TaskEnv>& envs) {
    bool found = false;
    std::int64_t earliest = 0;
    for (const auto& env : envs) {
        if (env.removed) continue;
        const std::int64_t start = env.startMinute;
        if (!found || start < earliest) {
            earliest = start;
            found = true;
        }
    }
    return found ? earliest : -1;
}

bool planDateElapsedMinutes(const std::string& planDate, std::int64_t nowUnix,
                            std::int64_t& outMinute) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parseIsoDate(planDate, year, month, day)) return false;

    std::tm planMidnight{};
    planMidnight.tm_year = year - 1900;
    planMidnight.tm_mon = month - 1;
    planMidnight.tm_mday = day;
    planMidnight.tm_isdst = -1;  // 让 mktime 按当日规则定 DST：零点恒为该日真实本地零点
    const std::time_t midnight = std::mktime(&planMidnight);
    if (midnight == static_cast<std::time_t>(-1)) return false;

    outMinute = floorDiv(nowUnix - static_cast<std::int64_t>(midnight), 60);
    return true;
}

TaskStatus deriveStatus(const Task& task,
                        const std::vector<TaskEnv>& envs,
                        std::int64_t nowUnix) {
    // 终态原样返回：完成 / 作废不随时间回流。
    if (task.status == TaskStatus::Done || task.status == TaskStatus::Voided) {
        return task.status;
    }
    // Draft 恒为 Draft：提交这个动作才改变状态；已过基准的未提交任务由提醒条目
    // 「任务未提交」负责警示，状态栏不显示「执行中」。
    if (task.status == TaskStatus::Draft) return TaskStatus::Draft;
    // Running 恒为 Running（不回退）。
    if (task.status == TaskStatus::Running) return TaskStatus::Running;

    // 下面只剩 Pending：没有时间依据（无有效环境 / 计划日期非法）不猜，保持库值。
    const std::int64_t baseline = taskBaseline(envs);
    if (baseline < 0) return task.status;

    // 计划日零点起的本地流逝分钟：单一换算口径（planDateElapsedMinutes，本地时区
    // 零点起算）；跨零点继续累计可 > 1440，计划日之前为负。日期非法 → 保持库值。
    std::int64_t nowMinute = 0;
    if (!planDateElapsedMinutes(task.planDate, nowUnix, nowMinute)) return task.status;

    // 未到时间基准 → 待执行；时间基准已到（含恰好相等）→ 执行中。
    if (nowMinute < baseline) return TaskStatus::Pending;
    return TaskStatus::Running;
}

std::vector<DueItem> dueItems(const Task& task,
                              const std::vector<TaskEnv>& envs,
                              const std::vector<TaskService>& services,
                              const std::vector<TaskConfig>& configs,
                              const std::vector<TaskStep>& steps,
                              int nowMinute) {
    std::vector<DueItem> items;
    if (task.status == TaskStatus::Done || task.status == TaskStatus::Voided) {
        return items;
    }
    const std::int64_t baseline = taskBaseline(envs);
    if (baseline < 0) return items;

    const std::int64_t now = nowMinute;
    const bool draft = task.status == TaskStatus::Draft;
    // 配置中且未到基准：整单静默（Draft 在 [基准−60, 基准) 内也不提醒）
    if (draft && now < baseline) return items;

    const std::int64_t prereqDueMinute = baseline - 60;
    if (now >= prereqDueMinute) {
        if (!task.pretestOk) {
            items.push_back({RemindLevel::Orange, task.id, "task", "提测单未通过",
                             "时间基准前 60 分钟需完成提测单"});
        }
        if (!task.cabOk) {
            items.push_back({RemindLevel::Orange, task.id, "task", "CAB 单未通过",
                             "时间基准前 60 分钟需完成 CAB 单"});
        }
        const TicketNeeds needs = ticketNeeds(envs, services, configs);
        if (needs.nacos && !task.ticketNacosOk) {
            items.push_back({RemindLevel::Orange, task.id, "task", "Nacos 工单未审批",
                             "任务内有 nacos 配置内容，需在上线前 60 分钟完成工单"});
        }
        if (needs.db && !task.ticketDbOk) {
            items.push_back({RemindLevel::Orange, task.id, "task", "数据库工单未审批",
                             "任务内有数据库配置内容，需在上线前 60 分钟完成工单"});
        }
    }

    if (now >= baseline && task.announceSentAt == 0) {
        items.push_back({RemindLevel::Yellow, task.id, "task", "上线公告未发布",
                         "时间基准已到，公告尚未发布"});
    }

    for (const auto& env : envs) {
        if (env.removed) continue;
        if (now < env.startMinute) continue;
        // 环境已完成（步骤与配置全部关闭；服务全移除 → 0/0 亦完成）→ 不产出条目。
        // 完成判定与 envCompleted 同源，避免「步骤全勾完仍持续假红」。
        if (envCompleted(env, services, configs, steps)) continue;
        items.push_back({RemindLevel::Red, task.id, env.envName, "部署未完成",
                         "环境「" + env.envName + "」开始时间已到，仍有步骤或配置未完成"});
    }

    if (draft && now >= baseline) {
        items.push_back({RemindLevel::Red, task.id, "task", "任务未提交",
                         "计划上线日已到，任务仍处于配置中"});
    }

    return items;
}

std::vector<DueItem> dueItems(const Task& task,
                              const std::vector<TaskEnv>& envs,
                              const std::vector<TaskService>& services,
                              const std::vector<TaskConfig>& configs,
                              int nowMinute) {
    // 兼容入口：空 steps = 全部步骤未完成（只要环境仍有未移除服务就报红），
    // 与旧契约行为一致。详情页已迁移到带 steps 的入口；此重载仅供旧测试。
    return dueItems(task, envs, services, configs, {}, nowMinute);
}

RemindLevel highestLevel(const std::vector<DueItem>& items) {
    RemindLevel level = RemindLevel::Yellow;
    for (const auto& item : items) {
        if (item.level == RemindLevel::Red) return RemindLevel::Red;
        if (item.level == RemindLevel::Orange) level = RemindLevel::Orange;
    }
    return level;
}

std::vector<std::string> validateTask(const std::vector<TaskService>& services,
                                      const std::vector<TaskConfig>& configs) {
    std::vector<std::string> missing;
    for (const auto& svc : services) {
        if (svc.removed) continue;
        if (svc.branch.empty()) {
            missing.push_back("服务「" + svc.serviceName + "」的分支未填写");
        }
    }
    for (const auto& cfg : configs) {
        if (cfg.skipped || !cfg.content.empty()) continue;
        const TaskService* owner = nullptr;
        for (const auto& svc : services) {
            if (svc.id == cfg.taskServiceId) {
                owner = &svc;
                break;
            }
        }
        if (owner == nullptr || owner->removed) continue;  // 已移除服务的配置不参与校验
        missing.push_back("服务「" + owner->serviceName + "」的「" +
                          configKindLabel(cfg.kind) + "」配置内容为空");
    }
    return missing;
}

namespace {

bool isBlank(const std::string& value) {
    for (char c : value) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

} // namespace

std::vector<std::string> validateTaskSubmission(const Task& task,
                                                const std::vector<TaskEnv>& envs,
                                                const std::vector<TaskService>& services,
                                                const std::vector<TaskConfig>& configs) {
    // 前置项在前（表单流顺序）、内容项在后：详情页标红映射按文案逐字定位，
    // 改措辞必须同步 validateTaskSubmission 的契约注释与标红映射两侧。
    std::vector<std::string> missing;
    if (isBlank(task.pretestNo)) missing.push_back("提测单号未填写");
    if (isBlank(task.cabNo)) missing.push_back("CAB 单号未填写");
    const TicketNeeds needs = ticketNeeds(envs, services, configs);
    if (needs.nacos && isBlank(task.ticketNacosNo)) {
        missing.push_back("nacos 工单号未填写");
    }
    if (needs.db && isBlank(task.ticketDbNo)) {
        missing.push_back("数据库工单号未填写");
    }
    if (isBlank(task.announceText)) missing.push_back("上线公告内容未填写");
    for (std::string& item : validateTask(services, configs)) {
        missing.push_back(std::move(item));
    }
    return missing;
}

int expectedConfigs(ServiceKind kind) {
    switch (kind) {
        case ServiceKind::Backend: return 5;  // nacos / db / cron / menu / gateway
        case ServiceKind::PcWeb:
        case ServiceKind::MobileWeb: return 0;  // 前端服务无配置项
    }
    return 0;
}

Progress envProgress(const TaskEnv& env,
                     const std::vector<TaskService>& services,
                     const std::vector<TaskConfig>& configs,
                     const std::vector<TaskStep>& steps) {
    Progress progress;
    if (env.removed) return progress;
    for (const auto& svc : services) {
        if (svc.removed || svc.taskEnvId != env.id) continue;
        // 3 个固定步骤：合并分支 / 部署 / 验证（step 行缺失按未完成计）
        progress.total += 3;
        bool mergeDone = false;
        bool deployDone = false;
        bool verifyDone = false;
        for (const auto& step : steps) {
            if (step.taskServiceId != svc.id || step.doneAt == 0) continue;
            if (step.kind == StepKind::MergeBranch) {
                mergeDone = true;
            } else if (step.kind == StepKind::Deploy) {
                deployDone = true;
            } else if (step.kind == StepKind::Verify) {
                verifyDone = true;
            }
            // kind == Config 的聚合行不参与：配置进度以 TaskConfig.doneAt 为准
        }
        if (mergeDone) ++progress.done;
        if (deployDone) ++progress.done;
        if (verifyDone) ++progress.done;

        // 配置项分母 = 服务类型的期望配置数（Backend 固定 5，前端 0），与库中
        // 行数无关：缺行按未完成计，跳过视为该槽位关闭；同 kind 多行去重。
        const int expected = expectedConfigs(svc.kind);
        progress.total += expected;
        const ConfigKind kConfigKinds[5] = {ConfigKind::Nacos, ConfigKind::Db,
                                            ConfigKind::Cron, ConfigKind::Menu,
                                            ConfigKind::Gateway};
        for (int i = 0; i < expected && i < 5; ++i) {
            if (configSlotClosed(configs, svc.id, kConfigKinds[i])) ++progress.done;
        }
    }
    return progress;
}

Progress taskProgress(const Task& task,
                      const std::vector<TaskEnv>& envs,
                      const std::vector<TaskService>& services,
                      const std::vector<TaskConfig>& configs,
                      const std::vector<TaskStep>& steps) {
    Progress progress;
    for (const auto& env : envs) {
        if (env.removed) continue;
        const Progress envPart = envProgress(env, services, configs, steps);
        progress.done += envPart.done;
        progress.total += envPart.total;
    }

    // 任务级前置：提测 / CAB / 适用工单 / 公告（与 taskCompleted 同一适用性判据）
    ++progress.total;
    if (task.pretestOk) ++progress.done;
    ++progress.total;
    if (task.cabOk) ++progress.done;
    const TicketNeeds needs = ticketNeeds(envs, services, configs);
    if (needs.nacos) {
        ++progress.total;
        if (task.ticketNacosOk) ++progress.done;
    }
    if (needs.db) {
        ++progress.total;
        if (task.ticketDbOk) ++progress.done;
    }
    ++progress.total;
    if (task.announceSentAt != 0) ++progress.done;
    return progress;
}

bool envCompleted(const TaskEnv& env,
                  const std::vector<TaskService>& services,
                  const std::vector<TaskConfig>& configs,
                  const std::vector<TaskStep>& steps) {
    const Progress progress = envProgress(env, services, configs, steps);
    return progress.done == progress.total;
}

bool taskCompleted(const Task& task,
                   const std::vector<TaskEnv>& envs,
                   const std::vector<TaskService>& services,
                   const std::vector<TaskConfig>& configs,
                   const std::vector<TaskStep>& steps) {
    const Progress progress = taskProgress(task, envs, services, configs, steps);
    return progress.done == progress.total;
}

} // namespace module::release