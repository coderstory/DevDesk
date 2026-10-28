// 上线域数据访问层实现。表结构/列名以 core/schema.h 的 release_* 九张表为准；
// 枚举列一律存文本（schema.h 约定），读回未知文本按 ValidationError 失败。
//
// 并发纪律：单条 UPDATE/DELETE 用 "… RETURNING id" 一步判定「行是否存在」，
// 不用 UPDATE 后 sqlite3_changes() 的两步配对（两个语句之间会被并发写者穿插）；
// 多语句写入一律走 db_.transaction()，其全程持连接锁。
#include "release_store.h"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace module::release {

namespace {

using core::ErrorCode;

// ---------- 类型转换 ----------

std::int64_t boolInt(bool value) { return value ? 1 : 0; }

bool isUniqueViolation(const std::string& message) {
    return message.find("UNIQUE") != std::string::npos;
}

const char* serviceKindText(ServiceKind kind) {
    switch (kind) {
        case ServiceKind::Backend: return "backend";
        case ServiceKind::PcWeb: return "pc_web";
        case ServiceKind::MobileWeb: return "mobile_web";
    }
    return "backend";
}

core::Result<ServiceKind> serviceKindFromText(const std::string& text) {
    if (text == "backend") return core::Result<ServiceKind>::ok(ServiceKind::Backend);
    if (text == "pc_web") return core::Result<ServiceKind>::ok(ServiceKind::PcWeb);
    if (text == "mobile_web") return core::Result<ServiceKind>::ok(ServiceKind::MobileWeb);
    return core::Result<ServiceKind>::fail(ErrorCode::ValidationError,
                                           "release_service.kind 非法值: " + text);
}

const char* configKindText(ConfigKind kind) {
    switch (kind) {
        case ConfigKind::Nacos: return "nacos";
        case ConfigKind::Db: return "db";
        case ConfigKind::Cron: return "cron";
        case ConfigKind::Menu: return "menu";
        case ConfigKind::Gateway: return "gateway";
    }
    return "nacos";
}

core::Result<ConfigKind> configKindFromText(const std::string& text) {
    if (text == "nacos") return core::Result<ConfigKind>::ok(ConfigKind::Nacos);
    if (text == "db") return core::Result<ConfigKind>::ok(ConfigKind::Db);
    if (text == "cron") return core::Result<ConfigKind>::ok(ConfigKind::Cron);
    if (text == "menu") return core::Result<ConfigKind>::ok(ConfigKind::Menu);
    if (text == "gateway") return core::Result<ConfigKind>::ok(ConfigKind::Gateway);
    return core::Result<ConfigKind>::fail(ErrorCode::ValidationError,
                                          "release_task_config.kind 非法值: " + text);
}

const char* stepKindText(StepKind kind) {
    switch (kind) {
        case StepKind::MergeBranch: return "branch";
        case StepKind::Config: return "config";
        case StepKind::Deploy: return "deploy";
        case StepKind::Verify: return "verify";
    }
    return "branch";
}

core::Result<StepKind> stepKindFromText(const std::string& text) {
    if (text == "branch") return core::Result<StepKind>::ok(StepKind::MergeBranch);
    if (text == "config") return core::Result<StepKind>::ok(StepKind::Config);
    if (text == "deploy") return core::Result<StepKind>::ok(StepKind::Deploy);
    if (text == "verify") return core::Result<StepKind>::ok(StepKind::Verify);
    return core::Result<StepKind>::fail(ErrorCode::ValidationError,
                                        "release_task_step.kind 非法值: " + text);
}

const char* taskStatusText(TaskStatus status) {
    switch (status) {
        case TaskStatus::Draft: return "draft";
        case TaskStatus::Pending: return "pending";
        case TaskStatus::Running: return "running";
        case TaskStatus::Done: return "done";
        case TaskStatus::Voided: return "voided";
    }
    return "draft";
}

core::Result<TaskStatus> taskStatusFromText(const std::string& text) {
    if (text == "draft") return core::Result<TaskStatus>::ok(TaskStatus::Draft);
    if (text == "pending") return core::Result<TaskStatus>::ok(TaskStatus::Pending);
    if (text == "running") return core::Result<TaskStatus>::ok(TaskStatus::Running);
    if (text == "done") return core::Result<TaskStatus>::ok(TaskStatus::Done);
    if (text == "voided") return core::Result<TaskStatus>::ok(TaskStatus::Voided);
    return core::Result<TaskStatus>::fail(ErrorCode::ValidationError,
                                          "release_task.status 非法值: " + text);
}

// ---------- Result 转发（保持错误码与消息原样） ----------

template <typename U>
core::Result<void> failVoid(const core::Result<U>& err) {
    return core::Result<void>::fail(err.errorCode(), err.errorMessage());
}

template <typename T, typename U>
core::Result<T> failAs(const core::Result<U>& err) {
    return core::Result<T>::fail(err.errorCode(), err.errorMessage());
}

// 跑完 RETURNING 语句：返回「是否至少返回一行」（INSERT 恒真，UPDATE/DELETE 即行存在性）。
// fetchAll 会 step 到 DONE，不会把 in-progress 语句留给同连接事务的 COMMIT。
core::Result<bool> fetchAny(core::data::Statement& stmt) {
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<bool>(rows);
    return core::Result<bool>::ok(!rows.value().empty());
}

core::Result<std::int64_t> fetchId(core::data::Statement& stmt, const char* what) {
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::int64_t>(rows);
    if (rows.value().empty()) {
        return core::Result<std::int64_t>::fail(
            ErrorCode::Internal, std::string(what) + " 未返回 id");
    }
    auto id = rows.value()[0].getInt64("id");
    if (id.isFail()) return failAs<std::int64_t>(id);
    return core::Result<std::int64_t>::ok(id.value());
}

// 无 RETURNING 的写语句（INSERT / DELETE）：step 到 DONE 即成功
core::Result<void> execDone(core::data::Statement& stmt) {
    auto step = stmt.step();
    if (step.isFail()) return failVoid(step);
    return core::Result<void>::ok();
}

// ---------- 行映射 ----------

core::Result<ServiceDef> parseServiceRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<ServiceDef>(id);
    auto name = row.getString("name");
    if (name.isFail()) return failAs<ServiceDef>(name);
    auto kindText = row.getString("kind");
    if (kindText.isFail()) return failAs<ServiceDef>(kindText);
    auto kind = serviceKindFromText(kindText.value());
    if (kind.isFail()) return failAs<ServiceDef>(kind);
    auto branch = row.getString("default_branch");
    if (branch.isFail()) return failAs<ServiceDef>(branch);
    auto sort = row.getInt64("sort");
    if (sort.isFail()) return failAs<ServiceDef>(sort);

    ServiceDef out;
    out.id = id.value();
    out.name = name.value();
    out.kind = kind.value();
    out.defaultBranch = branch.value();
    out.sort = static_cast<int>(sort.value());
    return core::Result<ServiceDef>::ok(std::move(out));
}

core::Result<EnvDef> parseEnvRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<EnvDef>(id);
    auto name = row.getString("name");
    if (name.isFail()) return failAs<EnvDef>(name);
    auto start = row.getInt64("start_minute");
    if (start.isFail()) return failAs<EnvDef>(start);
    auto end = row.getInt64("end_minute");
    if (end.isFail()) return failAs<EnvDef>(end);
    auto sort = row.getInt64("sort");
    if (sort.isFail()) return failAs<EnvDef>(sort);

    EnvDef out;
    out.id = id.value();
    out.name = name.value();
    out.startMinute = static_cast<int>(start.value());
    out.endMinute = static_cast<int>(end.value());
    out.sort = static_cast<int>(sort.value());
    return core::Result<EnvDef>::ok(std::move(out));
}

core::Result<Task> parseTaskRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<Task>(id);
    auto planDate = row.getString("plan_date");
    if (planDate.isFail()) return failAs<Task>(planDate);
    auto statusText = row.getString("status");
    if (statusText.isFail()) return failAs<Task>(statusText);
    auto status = taskStatusFromText(statusText.value());
    if (status.isFail()) return failAs<Task>(status);
    auto announceText = row.getString("announce_text");
    if (announceText.isFail()) return failAs<Task>(announceText);
    auto announceSentAt = row.getInt64("announce_sent_at");
    if (announceSentAt.isFail()) return failAs<Task>(announceSentAt);
    auto pretestNo = row.getString("pretest_no");
    if (pretestNo.isFail()) return failAs<Task>(pretestNo);
    auto pretestOk = row.getInt64("pretest_ok");
    if (pretestOk.isFail()) return failAs<Task>(pretestOk);
    auto cabNo = row.getString("cab_no");
    if (cabNo.isFail()) return failAs<Task>(cabNo);
    auto cabOk = row.getInt64("cab_ok");
    if (cabOk.isFail()) return failAs<Task>(cabOk);
    auto nacosNo = row.getString("ticket_nacos_no");
    if (nacosNo.isFail()) return failAs<Task>(nacosNo);
    auto nacosOk = row.getInt64("ticket_nacos_ok");
    if (nacosOk.isFail()) return failAs<Task>(nacosOk);
    auto dbNo = row.getString("ticket_db_no");
    if (dbNo.isFail()) return failAs<Task>(dbNo);
    auto dbOk = row.getInt64("ticket_db_ok");
    if (dbOk.isFail()) return failAs<Task>(dbOk);

    Task out;
    out.id = id.value();
    out.planDate = planDate.value();
    out.status = status.value();
    out.announceText = announceText.value();
    out.announceSentAt = announceSentAt.value();
    out.pretestNo = pretestNo.value();
    out.pretestOk = pretestOk.value() != 0;
    out.cabNo = cabNo.value();
    out.cabOk = cabOk.value() != 0;
    out.ticketNacosNo = nacosNo.value();
    out.ticketNacosOk = nacosOk.value() != 0;
    out.ticketDbNo = dbNo.value();
    out.ticketDbOk = dbOk.value() != 0;
    return core::Result<Task>::ok(std::move(out));
}

core::Result<TaskEnv> parseTaskEnvRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<TaskEnv>(id);
    auto taskId = row.getInt64("task_id");
    if (taskId.isFail()) return failAs<TaskEnv>(taskId);
    auto envName = row.getString("env_name");
    if (envName.isFail()) return failAs<TaskEnv>(envName);
    auto start = row.getInt64("start_minute");
    if (start.isFail()) return failAs<TaskEnv>(start);
    auto end = row.getInt64("end_minute");
    if (end.isFail()) return failAs<TaskEnv>(end);
    auto removed = row.getInt64("removed");
    if (removed.isFail()) return failAs<TaskEnv>(removed);

    TaskEnv out;
    out.id = id.value();
    out.taskId = taskId.value();
    out.envName = envName.value();
    out.startMinute = static_cast<int>(start.value());
    out.endMinute = static_cast<int>(end.value());
    out.removed = removed.value() != 0;
    return core::Result<TaskEnv>::ok(std::move(out));
}

core::Result<TaskService> parseTaskServiceRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<TaskService>(id);
    auto taskEnvId = row.getInt64("task_env_id");
    if (taskEnvId.isFail()) return failAs<TaskService>(taskEnvId);
    auto serviceName = row.getString("service_name");
    if (serviceName.isFail()) return failAs<TaskService>(serviceName);
    auto kindText = row.getString("service_kind");
    if (kindText.isFail()) return failAs<TaskService>(kindText);
    auto kind = serviceKindFromText(kindText.value());
    if (kind.isFail()) return failAs<TaskService>(kind);
    auto defaultBranch = row.getString("default_branch");
    if (defaultBranch.isFail()) return failAs<TaskService>(defaultBranch);
    auto branch = row.getString("branch");
    if (branch.isFail()) return failAs<TaskService>(branch);
    auto removed = row.getInt64("removed");
    if (removed.isFail()) return failAs<TaskService>(removed);

    TaskService out;
    out.id = id.value();
    out.taskEnvId = taskEnvId.value();
    out.serviceName = serviceName.value();
    out.kind = kind.value();
    out.defaultBranch = defaultBranch.value();
    out.branch = branch.value();
    out.removed = removed.value() != 0;
    return core::Result<TaskService>::ok(std::move(out));
}

core::Result<TaskConfig> parseTaskConfigRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<TaskConfig>(id);
    auto taskServiceId = row.getInt64("task_service_id");
    if (taskServiceId.isFail()) return failAs<TaskConfig>(taskServiceId);
    auto kindText = row.getString("kind");
    if (kindText.isFail()) return failAs<TaskConfig>(kindText);
    auto kind = configKindFromText(kindText.value());
    if (kind.isFail()) return failAs<TaskConfig>(kind);
    auto content = row.getString("content");
    if (content.isFail()) return failAs<TaskConfig>(content);
    auto skipped = row.getInt64("skipped");
    if (skipped.isFail()) return failAs<TaskConfig>(skipped);
    auto order = row.getInt64("conf_order");
    if (order.isFail()) return failAs<TaskConfig>(order);
    auto doneAt = row.getInt64("done_at");
    if (doneAt.isFail()) return failAs<TaskConfig>(doneAt);

    TaskConfig out;
    out.id = id.value();
    out.taskServiceId = taskServiceId.value();
    out.kind = kind.value();
    out.content = content.value();
    out.skipped = skipped.value() != 0;
    out.order = static_cast<int>(order.value());
    out.doneAt = doneAt.value();
    return core::Result<TaskConfig>::ok(std::move(out));
}

core::Result<TaskStep> parseTaskStepRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<TaskStep>(id);
    auto taskServiceId = row.getInt64("task_service_id");
    if (taskServiceId.isFail()) return failAs<TaskStep>(taskServiceId);
    auto kindText = row.getString("kind");
    if (kindText.isFail()) return failAs<TaskStep>(kindText);
    auto kind = stepKindFromText(kindText.value());
    if (kind.isFail()) return failAs<TaskStep>(kind);
    auto doneAt = row.getInt64("done_at");
    if (doneAt.isFail()) return failAs<TaskStep>(doneAt);

    TaskStep out;
    out.id = id.value();
    out.taskServiceId = taskServiceId.value();
    out.kind = kind.value();
    out.doneAt = doneAt.value();
    return core::Result<TaskStep>::ok(std::move(out));
}

core::Result<TaskLog> parseTaskLogRow(const core::data::SqlRow& row) {
    auto id = row.getInt64("id");
    if (id.isFail()) return failAs<TaskLog>(id);
    auto taskId = row.getInt64("task_id");
    if (taskId.isFail()) return failAs<TaskLog>(taskId);
    auto scope = row.getString("scope");
    if (scope.isFail()) return failAs<TaskLog>(scope);
    auto step = row.getString("step");
    if (step.isFail()) return failAs<TaskLog>(step);
    auto action = row.getString("action");
    if (action.isFail()) return failAs<TaskLog>(action);
    auto at = row.getInt64("at");
    if (at.isFail()) return failAs<TaskLog>(at);
    auto note = row.getString("note");
    if (note.isFail()) return failAs<TaskLog>(note);

    TaskLog out;
    out.id = id.value();
    out.taskId = taskId.value();
    out.scope = scope.value();
    out.step = step.value();
    out.action = action.value();
    out.at = at.value();
    out.note = note.value();
    return core::Result<TaskLog>::ok(std::move(out));
}

} // namespace

ReleaseStore::ReleaseStore(core::data::SqliteDb& db) : db_(db) {}

// ==================== 基础配置：服务 ====================

core::Result<std::vector<ServiceDef>> ReleaseStore::listServices() {
    auto prep = db_.prepare(
        "SELECT id, name, kind, default_branch, sort FROM release_service "
        "ORDER BY sort ASC, id ASC");
    if (prep.isFail()) return failAs<std::vector<ServiceDef>>(prep);
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return failAs<std::vector<ServiceDef>>(rows);

    std::vector<ServiceDef> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseServiceRow(row);
        if (parsed.isFail()) return failAs<std::vector<ServiceDef>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<ServiceDef>>::ok(std::move(out));
}

core::Result<std::int64_t> ReleaseStore::upsertService(const ServiceDef& def) {
    const std::string kind = serviceKindText(def.kind);

    if (def.id == 0) {
        auto prep = db_.prepare(
            "INSERT INTO release_service (name, kind, default_branch, sort) "
            "VALUES (?, ?, ?, ?) RETURNING id");
        if (prep.isFail()) return failAs<std::int64_t>(prep);
        auto& stmt = prep.value();
        if (auto r = stmt.bind(1, def.name); r.isFail()) return failAs<std::int64_t>(r);
        if (auto r = stmt.bind(2, kind); r.isFail()) return failAs<std::int64_t>(r);
        if (auto r = stmt.bind(3, def.defaultBranch); r.isFail()) return failAs<std::int64_t>(r);
        if (auto r = stmt.bind(4, static_cast<std::int64_t>(def.sort)); r.isFail())
            return failAs<std::int64_t>(r);
        auto id = fetchId(stmt, "release_service INSERT");
        if (id.isFail() && isUniqueViolation(id.errorMessage())) {
            return core::Result<std::int64_t>::fail(
                ErrorCode::AlreadyExists, "服务名称已存在: " + def.name);
        }
        return id;
    }

    auto prep = db_.prepare(
        "UPDATE release_service SET name = ?, kind = ?, default_branch = ?, sort = ? "
        "WHERE id = ? RETURNING id");
    if (prep.isFail()) return failAs<std::int64_t>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, def.name); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(2, kind); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(3, def.defaultBranch); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(4, static_cast<std::int64_t>(def.sort)); r.isFail())
        return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(5, def.id); r.isFail()) return failAs<std::int64_t>(r);
    auto any = fetchAny(stmt);
    if (any.isFail()) {
        if (isUniqueViolation(any.errorMessage())) {
            return core::Result<std::int64_t>::fail(
                ErrorCode::AlreadyExists, "服务名称已存在: " + def.name);
        }
        return failAs<std::int64_t>(any);
    }
    if (!any.value()) {
        return core::Result<std::int64_t>::fail(ErrorCode::NotFound,
                                                "服务不存在: id=" + std::to_string(def.id));
    }
    return core::Result<std::int64_t>::ok(def.id);
}

core::Result<void> ReleaseStore::deleteService(std::int64_t id) {
    // 关联表显式先清：不依赖 PRAGMA foreign_keys 是否在本连接开启（值构造与事务同锁）
    return db_.transaction([&]() -> core::Result<void> {
        auto delLinks = db_.prepare(
            "DELETE FROM release_env_service WHERE service_id = ?");
        if (delLinks.isFail()) return failVoid(delLinks);
        if (auto r = delLinks.value().bind(1, id); r.isFail()) return r;
        if (auto r = execDone(delLinks.value()); r.isFail()) return r;

        auto del = db_.prepare("DELETE FROM release_service WHERE id = ? RETURNING id");
        if (del.isFail()) return failVoid(del);
        if (auto r = del.value().bind(1, id); r.isFail()) return r;
        auto any = fetchAny(del.value());
        if (any.isFail()) return failVoid(any);
        if (!any.value()) {
            return core::Result<void>::fail(ErrorCode::NotFound,
                                            "服务不存在: id=" + std::to_string(id));
        }
        return core::Result<void>::ok();
    });
}

// ==================== 基础配置：环境 ====================

core::Result<std::vector<EnvDef>> ReleaseStore::listEnvs() {
    auto prep = db_.prepare(
        "SELECT id, name, start_minute, end_minute, sort FROM release_env "
        "ORDER BY sort ASC, id ASC");
    if (prep.isFail()) return failAs<std::vector<EnvDef>>(prep);
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return failAs<std::vector<EnvDef>>(rows);

    std::vector<EnvDef> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseEnvRow(row);
        if (parsed.isFail()) return failAs<std::vector<EnvDef>>(parsed);
        EnvDef env = std::move(parsed.value());

        auto linkPrep = db_.prepare(
            "SELECT service_id FROM release_env_service WHERE env_id = ? "
            "ORDER BY service_id ASC");
        if (linkPrep.isFail()) return failAs<std::vector<EnvDef>>(linkPrep);
        if (auto r = linkPrep.value().bind(1, env.id); r.isFail())
            return failAs<std::vector<EnvDef>>(r);
        auto links = linkPrep.value().fetchAll();
        if (links.isFail()) return failAs<std::vector<EnvDef>>(links);
        env.serviceIds.reserve(links.value().size());
        for (const auto& linkRow : links.value()) {
            auto serviceId = linkRow.getInt64("service_id");
            if (serviceId.isFail()) return failAs<std::vector<EnvDef>>(serviceId);
            env.serviceIds.push_back(serviceId.value());
        }
        out.push_back(std::move(env));
    }
    return core::Result<std::vector<EnvDef>>::ok(std::move(out));
}

core::Result<std::int64_t> ReleaseStore::upsertEnv(const EnvDef& def) {
    std::int64_t envId = def.id;
    auto tx = db_.transaction([&]() -> core::Result<void> {
        if (envId == 0) {
            auto prep = db_.prepare(
                "INSERT INTO release_env (name, start_minute, end_minute, sort) "
                "VALUES (?, ?, ?, ?) RETURNING id");
            if (prep.isFail()) return failVoid(prep);
            auto& stmt = prep.value();
            if (auto r = stmt.bind(1, def.name); r.isFail()) return r;
            if (auto r = stmt.bind(2, static_cast<std::int64_t>(def.startMinute)); r.isFail())
                return r;
            if (auto r = stmt.bind(3, static_cast<std::int64_t>(def.endMinute)); r.isFail())
                return r;
            if (auto r = stmt.bind(4, static_cast<std::int64_t>(def.sort)); r.isFail()) return r;
            auto id = fetchId(stmt, "release_env INSERT");
            if (id.isFail()) {
                if (isUniqueViolation(id.errorMessage())) {
                    return core::Result<void>::fail(ErrorCode::AlreadyExists,
                                                    "环境名称已存在: " + def.name);
                }
                return failVoid(id);
            }
            envId = id.value();
        } else {
            auto prep = db_.prepare(
                "UPDATE release_env SET name = ?, start_minute = ?, end_minute = ?, sort = ? "
                "WHERE id = ? RETURNING id");
            if (prep.isFail()) return failVoid(prep);
            auto& stmt = prep.value();
            if (auto r = stmt.bind(1, def.name); r.isFail()) return r;
            if (auto r = stmt.bind(2, static_cast<std::int64_t>(def.startMinute)); r.isFail())
                return r;
            if (auto r = stmt.bind(3, static_cast<std::int64_t>(def.endMinute)); r.isFail())
                return r;
            if (auto r = stmt.bind(4, static_cast<std::int64_t>(def.sort)); r.isFail()) return r;
            if (auto r = stmt.bind(5, envId); r.isFail()) return r;
            auto any = fetchAny(stmt);
            if (any.isFail()) {
                if (isUniqueViolation(any.errorMessage())) {
                    return core::Result<void>::fail(ErrorCode::AlreadyExists,
                                                    "环境名称已存在: " + def.name);
                }
                return failVoid(any);
            }
            if (!any.value()) {
                return core::Result<void>::fail(
                    ErrorCode::NotFound, "环境不存在: id=" + std::to_string(envId));
            }
        }

        // 关联整体替换：先清后建，避免旧关联残留（upsert 语义 = 以入参为准）
        auto delLinks = db_.prepare("DELETE FROM release_env_service WHERE env_id = ?");
        if (delLinks.isFail()) return failVoid(delLinks);
        if (auto r = delLinks.value().bind(1, envId); r.isFail()) return r;
        if (auto r = execDone(delLinks.value()); r.isFail()) return r;

        for (const auto serviceId : def.serviceIds) {
            auto ins = db_.prepare(
                "INSERT OR IGNORE INTO release_env_service (env_id, service_id) "
                "VALUES (?, ?)");
            if (ins.isFail()) return failVoid(ins);
            if (auto r = ins.value().bind(1, envId); r.isFail()) return r;
            if (auto r = ins.value().bind(2, serviceId); r.isFail()) return r;
            if (auto r = execDone(ins.value()); r.isFail()) return r;
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) return failAs<std::int64_t>(tx);
    return core::Result<std::int64_t>::ok(envId);
}

core::Result<void> ReleaseStore::deleteEnv(std::int64_t id) {
    return db_.transaction([&]() -> core::Result<void> {
        auto delLinks = db_.prepare("DELETE FROM release_env_service WHERE env_id = ?");
        if (delLinks.isFail()) return failVoid(delLinks);
        if (auto r = delLinks.value().bind(1, id); r.isFail()) return r;
        if (auto r = execDone(delLinks.value()); r.isFail()) return r;

        auto del = db_.prepare("DELETE FROM release_env WHERE id = ? RETURNING id");
        if (del.isFail()) return failVoid(del);
        if (auto r = del.value().bind(1, id); r.isFail()) return r;
        auto any = fetchAny(del.value());
        if (any.isFail()) return failVoid(any);
        if (!any.value()) {
            return core::Result<void>::fail(ErrorCode::NotFound,
                                            "环境不存在: id=" + std::to_string(id));
        }
        return core::Result<void>::ok();
    });
}

// ==================== 任务 ====================

core::Result<std::vector<Task>> ReleaseStore::listTasks() {
    auto prep = db_.prepare(
        "SELECT id, plan_date, status, announce_text, announce_sent_at, "
        "pretest_no, pretest_ok, cab_no, cab_ok, ticket_nacos_no, ticket_nacos_ok, "
        "ticket_db_no, ticket_db_ok FROM release_task "
        "ORDER BY plan_date DESC, id DESC");
    if (prep.isFail()) return failAs<std::vector<Task>>(prep);
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return failAs<std::vector<Task>>(rows);

    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskRow(row);
        if (parsed.isFail()) return failAs<std::vector<Task>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

core::Result<std::vector<Task>> ReleaseStore::listTasksPage(int offset, int limit) {
    // 负值直接拒：SQLite 的 LIMIT -1 语义是「不设上限」，会退化成一次读全表。
    if (offset < 0 || limit < 0) {
        return core::Result<std::vector<Task>>::fail(
            ErrorCode::ValidationError,
            "任务分页参数非法: offset=" + std::to_string(offset) +
                ", limit=" + std::to_string(limit));
    }
    // 排序口径必须与 listTasks 完全一致（plan_date DESC, id DESC），
    // 否则跨页会重复/漏条。分页在 SQL 侧完成，不在内存切片。
    auto prep = db_.prepare(
        "SELECT id, plan_date, status, announce_text, announce_sent_at, "
        "pretest_no, pretest_ok, cab_no, cab_ok, ticket_nacos_no, ticket_nacos_ok, "
        "ticket_db_no, ticket_db_ok FROM release_task "
        "ORDER BY plan_date DESC, id DESC LIMIT ? OFFSET ?");
    if (prep.isFail()) return failAs<std::vector<Task>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, static_cast<std::int64_t>(limit)); r.isFail())
        return failAs<std::vector<Task>>(r);
    if (auto r = stmt.bind(2, static_cast<std::int64_t>(offset)); r.isFail())
        return failAs<std::vector<Task>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<Task>>(rows);

    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskRow(row);
        if (parsed.isFail()) return failAs<std::vector<Task>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

core::Result<int> ReleaseStore::countTasks() {
    // 计入已作废任务：列表页会展示作废卡（「已作废」chip），总数必须与列表可见条数
    // 一致，否则尾页会少算。与 findTaskByDate 排除 voided 的口径不同——那是
    // 「作废不占同一天名额」的唯一性约束，不影响列表条数。
    auto prep = db_.prepare("SELECT COUNT(*) AS total FROM release_task");
    if (prep.isFail()) return failAs<int>(prep);
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return failAs<int>(rows);
    if (rows.value().empty()) {
        return core::Result<int>::fail(ErrorCode::Internal, "release_task COUNT 未返回行");
    }
    auto total = rows.value()[0].getInt64("total");
    if (total.isFail()) return failAs<int>(total);
    if (total.value() > std::numeric_limits<int>::max()) {
        return core::Result<int>::fail(ErrorCode::Internal,
                                       "release_task 行数超出 int 范围");
    }
    return core::Result<int>::ok(static_cast<int>(total.value()));
}

core::Result<std::optional<Task>> ReleaseStore::findTaskByDate(
    const std::string& planDate) {
    // 作废任务不占当天名额（schema.h 注释）：同一天可另建一个有效任务
    auto prep = db_.prepare(
        "SELECT id, plan_date, status, announce_text, announce_sent_at, "
        "pretest_no, pretest_ok, cab_no, cab_ok, ticket_nacos_no, ticket_nacos_ok, "
        "ticket_db_no, ticket_db_ok FROM release_task "
        "WHERE plan_date = ? AND status <> 'voided' ORDER BY id DESC LIMIT 1");
    if (prep.isFail()) return failAs<std::optional<Task>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, planDate); r.isFail()) return failAs<std::optional<Task>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::optional<Task>>(rows);
    if (rows.value().empty()) {
        return core::Result<std::optional<Task>>::ok(std::nullopt);
    }
    auto parsed = parseTaskRow(rows.value()[0]);
    if (parsed.isFail()) return failAs<std::optional<Task>>(parsed);
    return core::Result<std::optional<Task>>::ok(
        std::optional<Task>(std::move(parsed.value())));
}

core::Result<std::int64_t> ReleaseStore::createTask(const Task& task) {
    auto prep = db_.prepare(
        "INSERT INTO release_task "
        "(plan_date, status, announce_text, announce_sent_at, "
        " pretest_no, pretest_ok, cab_no, cab_ok, "
        " ticket_nacos_no, ticket_nacos_ok, ticket_db_no, ticket_db_ok) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id");
    if (prep.isFail()) return failAs<std::int64_t>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, task.planDate); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(2, taskStatusText(task.status)); r.isFail())
        return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(3, task.announceText); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(4, task.announceSentAt); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(5, task.pretestNo); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(6, boolInt(task.pretestOk)); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(7, task.cabNo); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(8, boolInt(task.cabOk)); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(9, task.ticketNacosNo); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(10, boolInt(task.ticketNacosOk)); r.isFail())
        return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(11, task.ticketDbNo); r.isFail()) return failAs<std::int64_t>(r);
    if (auto r = stmt.bind(12, boolInt(task.ticketDbOk)); r.isFail())
        return failAs<std::int64_t>(r);
    return fetchId(stmt, "release_task INSERT");
}

core::Result<std::int64_t> ReleaseStore::createTaskWithEnvs(
    const Task& task, const std::vector<TaskEnv>& envs,
    const std::vector<std::vector<TaskService>>& servicesPerEnv) {
    if (envs.size() != servicesPerEnv.size()) {
        return core::Result<std::int64_t>::fail(
            ErrorCode::ValidationError,
            "环境数与服务清单数不一致: envs=" + std::to_string(envs.size()) +
                ", servicesPerEnv=" + std::to_string(servicesPerEnv.size()));
    }
    std::int64_t taskId = 0;
    // 一个外层事务包住 createTask + 逐环境 addTaskEnv：建行 SQL 零复制。
    // addTaskEnv 自身的事务是嵌套事务（内层只跑 fn、不做 BEGIN/COMMIT），
    // 任一环境失败内层失败外传 → 外层整体 ROLLBACK，任务/环境/服务无一行残留。
    auto tx = db_.transaction([&]() -> core::Result<void> {
        auto created = createTask(task);
        if (created.isFail()) return failVoid(created);
        taskId = created.value();
        for (std::size_t i = 0; i < envs.size(); ++i) {
            TaskEnv bound = envs[i];
            bound.taskId = taskId;  // 快照一律挂本次新建任务，忽略入参 taskId
            auto added = addTaskEnv(bound, servicesPerEnv[i]);
            if (added.isFail()) return failVoid(added);
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) return failAs<std::int64_t>(tx);
    return core::Result<std::int64_t>::ok(taskId);
}

core::Result<void> ReleaseStore::updateTask(const Task& task) {
    if (task.id == 0) {
        return core::Result<void>::fail(ErrorCode::ValidationError, "task.id 不能为 0");
    }
    auto prep = db_.prepare(
        "UPDATE release_task SET plan_date = ?, status = ?, announce_text = ?, "
        "announce_sent_at = ?, pretest_no = ?, pretest_ok = ?, cab_no = ?, cab_ok = ?, "
        "ticket_nacos_no = ?, ticket_nacos_ok = ?, ticket_db_no = ?, ticket_db_ok = ? "
        "WHERE id = ? RETURNING id");
    if (prep.isFail()) return failVoid(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, task.planDate); r.isFail()) return r;
    if (auto r = stmt.bind(2, taskStatusText(task.status)); r.isFail()) return r;
    if (auto r = stmt.bind(3, task.announceText); r.isFail()) return r;
    if (auto r = stmt.bind(4, task.announceSentAt); r.isFail()) return r;
    if (auto r = stmt.bind(5, task.pretestNo); r.isFail()) return r;
    if (auto r = stmt.bind(6, boolInt(task.pretestOk)); r.isFail()) return r;
    if (auto r = stmt.bind(7, task.cabNo); r.isFail()) return r;
    if (auto r = stmt.bind(8, boolInt(task.cabOk)); r.isFail()) return r;
    if (auto r = stmt.bind(9, task.ticketNacosNo); r.isFail()) return r;
    if (auto r = stmt.bind(10, boolInt(task.ticketNacosOk)); r.isFail()) return r;
    if (auto r = stmt.bind(11, task.ticketDbNo); r.isFail()) return r;
    if (auto r = stmt.bind(12, boolInt(task.ticketDbOk)); r.isFail()) return r;
    if (auto r = stmt.bind(13, task.id); r.isFail()) return r;
    auto any = fetchAny(stmt);
    if (any.isFail()) return failVoid(any);
    if (!any.value()) {
        return core::Result<void>::fail(ErrorCode::NotFound,
                                        "任务不存在: id=" + std::to_string(task.id));
    }
    return core::Result<void>::ok();
}

// ==================== 任务环境 / 服务快照 ====================

core::Result<std::vector<TaskEnv>> ReleaseStore::listTaskEnvs(std::int64_t taskId) {
    auto prep = db_.prepare(
        "SELECT id, task_id, env_name, start_minute, end_minute, removed "
        "FROM release_task_env WHERE task_id = ? ORDER BY id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskEnv>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskId); r.isFail()) return failAs<std::vector<TaskEnv>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskEnv>>(rows);

    std::vector<TaskEnv> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskEnvRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskEnv>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskEnv>>::ok(std::move(out));
}

core::Result<std::int64_t> ReleaseStore::addTaskEnv(
    const TaskEnv& env, const std::vector<TaskService>& services) {
    std::int64_t envId = 0;
    auto tx = db_.transaction([&]() -> core::Result<void> {
        auto envPrep = db_.prepare(
            "INSERT INTO release_task_env "
            "(task_id, env_name, start_minute, end_minute, removed) "
            "VALUES (?, ?, ?, ?, ?) RETURNING id");
        if (envPrep.isFail()) return failVoid(envPrep);
        auto& envStmt = envPrep.value();
        if (auto r = envStmt.bind(1, env.taskId); r.isFail()) return r;
        if (auto r = envStmt.bind(2, env.envName); r.isFail()) return r;
        if (auto r = envStmt.bind(3, static_cast<std::int64_t>(env.startMinute)); r.isFail())
            return r;
        if (auto r = envStmt.bind(4, static_cast<std::int64_t>(env.endMinute)); r.isFail())
            return r;
        if (auto r = envStmt.bind(5, boolInt(env.removed)); r.isFail()) return r;
        auto id = fetchId(envStmt, "release_task_env INSERT");
        if (id.isFail()) return failVoid(id);
        envId = id.value();

        // services 入参的 id / taskEnvId 一律忽略：id 由库生成，taskEnvId 绑定本次新环境
        for (const auto& svc : services) {
            auto svcPrep = db_.prepare(
                "INSERT INTO release_task_service "
                "(task_env_id, service_name, service_kind, default_branch, branch, removed) "
                "VALUES (?, ?, ?, ?, ?, ?) RETURNING id");
            if (svcPrep.isFail()) return failVoid(svcPrep);
            auto& svcStmt = svcPrep.value();
            if (auto r = svcStmt.bind(1, envId); r.isFail()) return r;
            if (auto r = svcStmt.bind(2, svc.serviceName); r.isFail()) return r;
            if (auto r = svcStmt.bind(3, serviceKindText(svc.kind)); r.isFail()) return r;
            if (auto r = svcStmt.bind(4, svc.defaultBranch); r.isFail()) return r;
            if (auto r = svcStmt.bind(5, svc.branch); r.isFail()) return r;
            if (auto r = svcStmt.bind(6, boolInt(svc.removed)); r.isFail()) return r;
            auto svcId = fetchId(svcStmt, "release_task_service INSERT");
            if (svcId.isFail()) return failVoid(svcId);
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) return failAs<std::int64_t>(tx);
    return core::Result<std::int64_t>::ok(envId);
}

core::Result<void> ReleaseStore::removeTaskEnv(std::int64_t taskEnvId) {
    auto prep = db_.prepare(
        "UPDATE release_task_env SET removed = 1 WHERE id = ? RETURNING id");
    if (prep.isFail()) return failVoid(prep);
    if (auto r = prep.value().bind(1, taskEnvId); r.isFail()) return r;
    auto any = fetchAny(prep.value());
    if (any.isFail()) return failVoid(any);
    if (!any.value()) {
        return core::Result<void>::fail(
            ErrorCode::NotFound, "任务环境不存在: id=" + std::to_string(taskEnvId));
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<TaskService>> ReleaseStore::listTaskServices(
    std::int64_t taskEnvId) {
    auto prep = db_.prepare(
        "SELECT id, task_env_id, service_name, service_kind, default_branch, branch, removed "
        "FROM release_task_service WHERE task_env_id = ? ORDER BY id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskService>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskEnvId); r.isFail())
        return failAs<std::vector<TaskService>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskService>>(rows);

    std::vector<TaskService> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskServiceRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskService>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskService>>::ok(std::move(out));
}

core::Result<void> ReleaseStore::updateTaskService(const TaskService& svc) {
    if (svc.id == 0) {
        return core::Result<void>::fail(ErrorCode::ValidationError,
                                        "taskService.id 不能为 0");
    }
    auto prep = db_.prepare(
        "UPDATE release_task_service SET service_name = ?, service_kind = ?, "
        "default_branch = ?, branch = ?, removed = ? WHERE id = ? RETURNING id");
    if (prep.isFail()) return failVoid(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, svc.serviceName); r.isFail()) return r;
    if (auto r = stmt.bind(2, serviceKindText(svc.kind)); r.isFail()) return r;
    if (auto r = stmt.bind(3, svc.defaultBranch); r.isFail()) return r;
    if (auto r = stmt.bind(4, svc.branch); r.isFail()) return r;
    if (auto r = stmt.bind(5, boolInt(svc.removed)); r.isFail()) return r;
    if (auto r = stmt.bind(6, svc.id); r.isFail()) return r;
    auto any = fetchAny(stmt);
    if (any.isFail()) return failVoid(any);
    if (!any.value()) {
        return core::Result<void>::fail(
            ErrorCode::NotFound, "任务服务不存在: id=" + std::to_string(svc.id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ReleaseStore::removeTaskService(std::int64_t taskServiceId) {
    auto prep = db_.prepare(
        "UPDATE release_task_service SET removed = 1 WHERE id = ? RETURNING id");
    if (prep.isFail()) return failVoid(prep);
    if (auto r = prep.value().bind(1, taskServiceId); r.isFail()) return r;
    auto any = fetchAny(prep.value());
    if (any.isFail()) return failVoid(any);
    if (!any.value()) {
        return core::Result<void>::fail(
            ErrorCode::NotFound, "任务服务不存在: id=" + std::to_string(taskServiceId));
    }
    return core::Result<void>::ok();
}

// ==================== 配置项 ====================

core::Result<std::vector<TaskConfig>> ReleaseStore::listTaskConfigs(
    std::int64_t taskServiceId) {
    auto prep = db_.prepare(
        "SELECT id, task_service_id, kind, content, skipped, conf_order, done_at "
        "FROM release_task_config WHERE task_service_id = ? "
        "ORDER BY conf_order ASC, id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskConfig>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskServiceId); r.isFail())
        return failAs<std::vector<TaskConfig>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskConfig>>(rows);

    std::vector<TaskConfig> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskConfigRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskConfig>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskConfig>>::ok(std::move(out));
}

core::Result<void> ReleaseStore::upsertTaskConfig(const TaskConfig& cfg) {
    const std::string kind = configKindText(cfg.kind);

    if (cfg.id == 0) {
        auto prep = db_.prepare(
            "INSERT INTO release_task_config "
            "(task_service_id, kind, content, skipped, conf_order, done_at) "
            "VALUES (?, ?, ?, ?, ?, ?) RETURNING id");
        if (prep.isFail()) return failVoid(prep);
        auto& stmt = prep.value();
        if (auto r = stmt.bind(1, cfg.taskServiceId); r.isFail()) return r;
        if (auto r = stmt.bind(2, kind); r.isFail()) return r;
        if (auto r = stmt.bind(3, cfg.content); r.isFail()) return r;
        if (auto r = stmt.bind(4, boolInt(cfg.skipped)); r.isFail()) return r;
        if (auto r = stmt.bind(5, static_cast<std::int64_t>(cfg.order)); r.isFail()) return r;
        if (auto r = stmt.bind(6, cfg.doneAt); r.isFail()) return r;
        auto id = fetchId(stmt, "release_task_config INSERT");
        if (id.isFail()) return failVoid(id);
        return core::Result<void>::ok();
    }

    auto prep = db_.prepare(
        "UPDATE release_task_config SET task_service_id = ?, kind = ?, content = ?, "
        "skipped = ?, conf_order = ?, done_at = ? WHERE id = ? RETURNING id");
    if (prep.isFail()) return failVoid(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, cfg.taskServiceId); r.isFail()) return r;
    if (auto r = stmt.bind(2, kind); r.isFail()) return r;
    if (auto r = stmt.bind(3, cfg.content); r.isFail()) return r;
    if (auto r = stmt.bind(4, boolInt(cfg.skipped)); r.isFail()) return r;
    if (auto r = stmt.bind(5, static_cast<std::int64_t>(cfg.order)); r.isFail()) return r;
    if (auto r = stmt.bind(6, cfg.doneAt); r.isFail()) return r;
    if (auto r = stmt.bind(7, cfg.id); r.isFail()) return r;
    auto any = fetchAny(stmt);
    if (any.isFail()) return failVoid(any);
    if (!any.value()) {
        return core::Result<void>::fail(
            ErrorCode::NotFound, "配置项不存在: id=" + std::to_string(cfg.id));
    }
    return core::Result<void>::ok();
}

// ==================== 步骤 ====================

core::Result<std::vector<TaskStep>> ReleaseStore::listTaskSteps(
    std::int64_t taskServiceId) {
    auto prep = db_.prepare(
        "SELECT id, task_service_id, kind, done_at FROM release_task_step "
        "WHERE task_service_id = ? ORDER BY id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskStep>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskServiceId); r.isFail())
        return failAs<std::vector<TaskStep>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskStep>>(rows);

    std::vector<TaskStep> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskStepRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskStep>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskStep>>::ok(std::move(out));
}

core::Result<std::vector<TaskStep>> ReleaseStore::listTaskStepsByTask(
    std::int64_t taskId) {
    auto prep = db_.prepare(
        "SELECT step.id, step.task_service_id, step.kind, step.done_at "
        "FROM release_task_step AS step "
        "JOIN release_task_service AS svc ON svc.id = step.task_service_id "
        "JOIN release_task_env AS env ON env.id = svc.task_env_id "
        "WHERE env.task_id = ? ORDER BY step.id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskStep>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskId); r.isFail())
        return failAs<std::vector<TaskStep>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskStep>>(rows);

    std::vector<TaskStep> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskStepRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskStep>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskStep>>::ok(std::move(out));
}

core::Result<void> ReleaseStore::setStepDone(std::int64_t taskServiceId,
                                             StepKind kind, std::int64_t doneAt) {
    const std::string kindText = stepKindText(kind);
    return db_.transaction([&]() -> core::Result<void> {
        // 先按 (taskServiceId, kind) 定位；语句行不存在才补插一行。
        // 事务全程持连接锁，UPDATE 与 INSERT 之间不会被并发写者插进同键行。
        auto upd = db_.prepare(
            "UPDATE release_task_step SET done_at = ? "
            "WHERE task_service_id = ? AND kind = ? RETURNING id");
        if (upd.isFail()) return failVoid(upd);
        auto& stmt = upd.value();
        if (auto r = stmt.bind(1, doneAt); r.isFail()) return r;
        if (auto r = stmt.bind(2, taskServiceId); r.isFail()) return r;
        if (auto r = stmt.bind(3, kindText); r.isFail()) return r;
        auto any = fetchAny(stmt);
        if (any.isFail()) return failVoid(any);
        if (any.value()) return core::Result<void>::ok();

        // 撤回（doneAt == 0）从未置位过的步骤：不补插幻影行（done_at=0 的行
        // 与「无行」语义等价，插了反而污染「缺行按未完成计」的口径与唯一索引）。
        if (doneAt == 0) return core::Result<void>::ok();

        auto ins = db_.prepare(
            "INSERT INTO release_task_step (task_service_id, kind, done_at) "
            "VALUES (?, ?, ?)");
        if (ins.isFail()) return failVoid(ins);
        if (auto r = ins.value().bind(1, taskServiceId); r.isFail()) return r;
        if (auto r = ins.value().bind(2, kindText); r.isFail()) return r;
        if (auto r = ins.value().bind(3, doneAt); r.isFail()) return r;
        return execDone(ins.value());
    });
}

// ==================== 日志 ====================

core::Result<void> ReleaseStore::appendLog(const TaskLog& log) {
    auto prep = db_.prepare(
        "INSERT INTO release_task_log (task_id, scope, step, action, at, note) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    if (prep.isFail()) return failVoid(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, log.taskId); r.isFail()) return r;
    if (auto r = stmt.bind(2, log.scope); r.isFail()) return r;
    if (auto r = stmt.bind(3, log.step); r.isFail()) return r;
    if (auto r = stmt.bind(4, log.action); r.isFail()) return r;
    if (auto r = stmt.bind(5, log.at); r.isFail()) return r;
    if (auto r = stmt.bind(6, log.note); r.isFail()) return r;
    return execDone(stmt);
}

core::Result<std::vector<TaskLog>> ReleaseStore::listLogs(std::int64_t taskId) {
    auto prep = db_.prepare(
        "SELECT id, task_id, scope, step, action, at, note FROM release_task_log "
        "WHERE task_id = ? ORDER BY at ASC, id ASC");
    if (prep.isFail()) return failAs<std::vector<TaskLog>>(prep);
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, taskId); r.isFail()) return failAs<std::vector<TaskLog>>(r);
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return failAs<std::vector<TaskLog>>(rows);

    std::vector<TaskLog> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto parsed = parseTaskLogRow(row);
        if (parsed.isFail()) return failAs<std::vector<TaskLog>>(parsed);
        out.push_back(std::move(parsed.value()));
    }
    return core::Result<std::vector<TaskLog>>::ok(std::move(out));
}

} // namespace module::release