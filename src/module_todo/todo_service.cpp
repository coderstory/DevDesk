// Todo 看板业务逻辑层实现
#include "todo_service.h"

#include "core/logging.h"
#include "core/overdue.h"
#include "core/result.h"
#include "core/sqlite_db.h"
#include "core/str_util.h"
#include "platform/json.h"

#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

#include <sqlite3.h>

namespace module::todo {

namespace {

Priority intToPriority(int v) {
    // 值域检查 + 转换（枚举连续 None=0..High=3）。越界 → None。
    if (v < 0 || v > static_cast<int>(Priority::High)) return Priority::None;
    return static_cast<Priority>(v);
}

// LIKE 模式转义 % _ \（配合 SQL 的 ESCAPE '\'），防查询串里的通配符被当作元字符误命中
using core::str::escapeLikePattern;

} // namespace

TodoService::TodoService(core::data::SqliteDb& db) : db_(db) {}

int64_t TodoService::nowEpochSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

core::Result<int> TodoService::nextColumnPosition() {
    auto prep = db_.prepare("SELECT COALESCE(MAX(position), -1) + 1 AS next FROM todo_columns");
    if (prep.isFail()) {
        return core::Result<int>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<int>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) {
        return core::Result<int>::ok(0);
    }
    return core::Result<int>::ok(static_cast<int>(rows.value()[0].getInt64("next").valueOr(0)));
}

core::Result<int> TodoService::nextTaskPosition(int64_t columnId) {
    auto prep = db_.prepare(
        "SELECT COALESCE(MAX(position), -1) + 1 AS next FROM todo_tasks WHERE column_id = ?");
    if (prep.isFail()) {
        return core::Result<int>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, columnId); r.isFail()) {
        return core::Result<int>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<int>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) {
        return core::Result<int>::ok(0);
    }
    return core::Result<int>::ok(static_cast<int>(rows.value()[0].getInt64("next").valueOr(0)));
}

core::Result<Column> TodoService::parseColumnRow(const core::data::SqlRow& row) {
    auto idResult = row.getInt64("id");
    if (idResult.isFail()) {
        return core::Result<Column>::fail(idResult.errorCode(), idResult.errorMessage());
    }
    auto titleResult = row.getString("title");
    if (titleResult.isFail()) {
        return core::Result<Column>::fail(titleResult.errorCode(), titleResult.errorMessage());
    }
    auto posResult = row.getInt64("position");
    if (posResult.isFail()) {
        return core::Result<Column>::fail(posResult.errorCode(), posResult.errorMessage());
    }
    Column c;
    c.id = idResult.value();
    c.title = titleResult.value();
    c.position = static_cast<int>(posResult.value());
    if (!row.isNull("color")) {
        auto colorResult = row.getString("color");
        if (colorResult.isOk()) {
            c.color = colorResult.value();
        }
    }
    // kind 可能为 NULL（migration v4 之前的行）→ 留空 = 普通列
    if (!row.isNull("kind")) {
        auto kindResult = row.getString("kind");
        if (kindResult.isOk()) {
            c.kind = kindResult.value();
        }
    }
    return core::Result<Column>::ok(std::move(c));
}

core::Result<Task> TodoService::parseTaskRow(const core::data::SqlRow& row) {
    auto idResult = row.getInt64("id");
    if (idResult.isFail()) {
        return core::Result<Task>::fail(idResult.errorCode(), idResult.errorMessage());
    }
    auto columnIdResult = row.getInt64("column_id");
    if (columnIdResult.isFail()) {
        return core::Result<Task>::fail(columnIdResult.errorCode(), columnIdResult.errorMessage());
    }
    auto titleResult = row.getString("title");
    if (titleResult.isFail()) {
        return core::Result<Task>::fail(titleResult.errorCode(), titleResult.errorMessage());
    }
    auto posResult = row.getInt64("position");
    if (posResult.isFail()) {
        return core::Result<Task>::fail(posResult.errorCode(), posResult.errorMessage());
    }
    auto priorityResult = row.getInt64("priority");
    if (priorityResult.isFail()) {
        return core::Result<Task>::fail(priorityResult.errorCode(), priorityResult.errorMessage());
    }

    Task t;
    t.id = idResult.value();
    t.columnId = columnIdResult.value();
    t.title = titleResult.value();
    t.position = static_cast<int>(posResult.value());
    t.priority = intToPriority(static_cast<int>(priorityResult.value()));

    if (!row.isNull("description")) {
        auto descResult = row.getString("description");
        if (descResult.isOk()) {
            t.description = descResult.value();
        }
    }
    if (!row.isNull("due_at")) {
        auto dueResult = row.getInt64("due_at");
        if (dueResult.isOk()) {
            t.dueAt = dueResult.value();
        }
    }
    if (!row.isNull("tags")) {
        auto tagsResult = row.getString("tags");
        if (tagsResult.isOk()) {
            auto parsedResult = deserializeTags(tagsResult.value());
            if (parsedResult.isOk()) {
                t.tags = parsedResult.value();
            }
        }
    }
    if (!row.isNull("completed_at")) {
        auto completedResult = row.getInt64("completed_at");
        if (completedResult.isOk()) {
            t.completedAt = completedResult.value();
        }
    }
    if (!row.isNull("archived_at")) {
        auto archivedResult = row.getInt64("archived_at");
        if (archivedResult.isOk()) {
            t.archivedAt = archivedResult.value();
        }
    }
    return core::Result<Task>::ok(std::move(t));
}

std::string TodoService::serializeTags(const std::vector<std::string>& tags) {
    // 走 platform::json 封装（makeStringArray + stringify），不留第二份序列化实现
    auto arr = platform::json::makeStringArray(tags);
    auto out = platform::json::stringify(arr);
    if (out.isFail()) {
        // stringify 当前实现不会失败；防御式退化为空数组
        return "[]";
    }
    return out.value();
}

core::Result<std::vector<std::string>> TodoService::deserializeTags(const std::string& json) {
    if (json.empty()) {
        return core::Result<std::vector<std::string>>::ok({});
    }
    auto parsed = platform::json::parse(json);
    if (parsed.isFail()) {
        return core::Result<std::vector<std::string>>::fail(
            parsed.errorCode(), parsed.errorMessage());
    }
    const auto& root = parsed.value();
    std::vector<std::string> out;
    if (root.isArray()) {
        for (int i = 0; i < root.size(); ++i) {
            auto item = root[i];
            if (item.isString()) {
                out.push_back(item.asString());
            }
        }
    }
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

core::Result<std::vector<Column>> TodoService::listColumns() {
    auto prep = db_.prepare(
        "SELECT id, title, position, color, kind FROM todo_columns ORDER BY position ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Column>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Column>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<Column> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto colResult = parseColumnRow(row);
        if (colResult.isFail()) {
            return core::Result<std::vector<Column>>::fail(
                colResult.errorCode(), colResult.errorMessage());
        }
        out.push_back(std::move(colResult).value());
    }
    return core::Result<std::vector<Column>>::ok(std::move(out));
}

core::Result<Column> TodoService::createColumn(const std::string& title) {
    auto nextPosResult = nextColumnPosition();
    if (nextPosResult.isFail()) {
        return core::Result<Column>::fail(nextPosResult.errorCode(), nextPosResult.errorMessage());
    }
    int pos = nextPosResult.value();
    const int64_t now = nowEpochSeconds();

    // INSERT ... RETURNING id 单语句原子取回新 rowid：两步「INSERT 后 SELECT last_insert_rowid()」
    // 会被并发写者改写连接级 last_insert_rowid，取回错误 id
    auto prep = db_.prepare(
        "INSERT INTO todo_columns (title, position, color, created_at) "
        "VALUES (?, ?, NULL, ?) RETURNING id");
    if (prep.isFail()) {
        return core::Result<Column>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, title); r.isFail()) {
        return core::Result<Column>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(2, static_cast<int64_t>(pos)); r.isFail()) {
        return core::Result<Column>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(3, now); r.isFail()) {
        return core::Result<Column>::fail(r.errorCode(), r.errorMessage());
    }
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<Column>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    if (!stepResult.value()) {
        // RETURNING 理论上必返回一行；防御式处理（DONE 意味着异常路径）
        return core::Result<Column>::fail(
            core::ErrorCode::Internal, "INSERT ... RETURNING id 未返回行");
    }
    auto idRow = stmt.currentRow();
    if (idRow.isFail()) {
        return core::Result<Column>::fail(idRow.errorCode(), idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("id");
    if (idVal.isFail()) {
        return core::Result<Column>::fail(idVal.errorCode(), idVal.errorMessage());
    }

    // drain 到 DONE 释放 in-progress 语句：不 drain 则同连接事务 COMMIT 会撞
    // "cannot commit transaction - SQL statements in progress"（recursive_mutex 只逐调用持锁）。
    // 失败极罕见且行已写入，只 WARN
    auto drain = stmt.step();
    if (drain.isFail()) {
        LOG_WARN("Todo: INSERT ... RETURNING 收尾 step 失败（行已写入，继续）: " +
                 drain.errorMessage());
    }

    Column c;
    c.id = idVal.value();
    c.title = title;
    c.position = pos;
    c.color = std::nullopt;
    c.kind = "";  // 用户新建列默认普通列
    return core::Result<Column>::ok(std::move(c));
}

core::Result<void> TodoService::renameColumn(int64_t id, const std::string& title) {
    // 持锁覆盖 UPDATE + changes() 取数，避免并发语句插队导致计数失真（recursive_mutex 可重入）
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());

    auto prep = db_.prepare("UPDATE todo_columns SET title = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, title); r.isFail()) return r;
    if (auto r = stmt.bind(2, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    // 头文件承诺「id 不存在 → NotFound」：UPDATE 影响 0 行即目标行不存在
    if (sqlite3_changes(db_.handle()) == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound, "column not found");
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::setColumnColor(int64_t id,
                                                const std::optional<std::string>& color) {
    auto prep = db_.prepare("UPDATE todo_columns SET color = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (color.has_value()) {
        if (auto r = stmt.bind(1, color.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(1, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(2, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::deleteColumn(int64_t id) {
    auto prep = db_.prepare("DELETE FROM todo_columns WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::reorderColumns(const std::vector<int64_t>& ids) {
    auto txResult = db_.transaction([&]() -> core::Result<void> {
        // 按 ids 顺序重设 position = 0,1,2...；ids 必须是全部列的一次排列（唯一调用点 doShiftColumn 传全集）。
        // 每次重新 prepare（sqlite3 不允许同一 stmt 多次 step）
        for (size_t i = 0; i < ids.size(); ++i) {
            auto onePrep = db_.prepare("UPDATE todo_columns SET position = ? WHERE id = ?");
            if (onePrep.isFail()) {
                return core::Result<void>::fail(onePrep.errorCode(), onePrep.errorMessage());
            }
            auto& oneStmt = onePrep.value();
            if (auto r = oneStmt.bind(1, static_cast<int64_t>(i)); r.isFail()) return r;
            if (auto r = oneStmt.bind(2, ids[i]); r.isFail()) return r;
            auto s = oneStmt.step();
            if (s.isFail()) {
                return core::Result<void>::fail(s.errorCode(), s.errorMessage());
            }
        }
        return core::Result<void>::ok();
    });
    return txResult;
}

core::Result<std::vector<Task>> TodoService::listTasks(int64_t columnId) {
    auto prep = db_.prepare(
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at, archived_at "
        "FROM todo_tasks WHERE column_id = ? ORDER BY position ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Task>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, columnId); r.isFail()) {
        return core::Result<std::vector<Task>>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Task>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto taskResult = parseTaskRow(row);
        if (taskResult.isFail()) {
            return core::Result<std::vector<Task>>::fail(
                taskResult.errorCode(), taskResult.errorMessage());
        }
        out.push_back(std::move(taskResult).value());
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

core::Result<std::vector<Task>> TodoService::listAllTasks() {
    auto prep = db_.prepare(
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at, archived_at "
        "FROM todo_tasks ORDER BY column_id ASC, position ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Task>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Task>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto taskResult = parseTaskRow(row);
        if (taskResult.isFail()) {
            return core::Result<std::vector<Task>>::fail(
                taskResult.errorCode(), taskResult.errorMessage());
        }
        out.push_back(std::move(taskResult).value());
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

core::Result<Task> TodoService::createTask(int64_t columnId, const std::string& title) {
    auto nextPosResult = nextTaskPosition(columnId);
    if (nextPosResult.isFail()) {
        return core::Result<Task>::fail(nextPosResult.errorCode(), nextPosResult.errorMessage());
    }
    int pos = nextPosResult.value();
    const int64_t now = nowEpochSeconds();

    // 同 createColumn：INSERT ... RETURNING id 消除并发写者改写连接级 rowid 的竞态
    auto prep = db_.prepare(
        "INSERT INTO todo_tasks "
        "(column_id, title, description, priority, due_at, tags, position, completed_at, created_at, updated_at) "
        "VALUES (?, ?, NULL, 0, NULL, '[]', ?, NULL, ?, ?) RETURNING id");
    if (prep.isFail()) {
        return core::Result<Task>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, columnId); r.isFail()) {
        return core::Result<Task>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(2, title); r.isFail()) {
        return core::Result<Task>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(3, static_cast<int64_t>(pos)); r.isFail()) {
        return core::Result<Task>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(4, now); r.isFail()) {
        return core::Result<Task>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(5, now); r.isFail()) {
        return core::Result<Task>::fail(r.errorCode(), r.errorMessage());
    }
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<Task>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    if (!stepResult.value()) {
        // RETURNING 理论上必返回一行；防御式处理（DONE 意味着异常路径）
        return core::Result<Task>::fail(
            core::ErrorCode::Internal, "INSERT ... RETURNING id 未返回行");
    }
    auto idRow = stmt.currentRow();
    if (idRow.isFail()) {
        return core::Result<Task>::fail(idRow.errorCode(), idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("id");
    if (idVal.isFail()) {
        return core::Result<Task>::fail(idVal.errorCode(), idVal.errorMessage());
    }

    // 同 createColumn：drain 到 DONE 关闭 in-progress 窗口
    auto drain = stmt.step();
    if (drain.isFail()) {
        LOG_WARN("Todo: INSERT ... RETURNING 收尾 step 失败（行已写入，继续）: " +
                 drain.errorMessage());
    }

    Task t;
    t.id = idVal.value();
    t.columnId = columnId;
    t.title = title;
    t.position = pos;
    t.priority = Priority::None;
    t.tags = {};
    return core::Result<Task>::ok(std::move(t));
}

core::Result<void> TodoService::updateTask(const Task& task) {
    const int64_t now = nowEpochSeconds();

    std::string tagsJson = serializeTags(task.tags);

    // 只更新内容字段：column_id / position 改列/改序一律走 moveTask（避免 updateTask 误用时静默挪列或顶到列首）
    auto prep = db_.prepare(
        "UPDATE todo_tasks SET "
        "title = ?, description = ?, priority = ?, "
        "due_at = ?, tags = ?, completed_at = ?, updated_at = ? "
        "WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, task.title); r.isFail()) return r;
    if (task.description.has_value()) {
        if (auto r = stmt.bind(2, task.description.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(2, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(3, static_cast<int64_t>(task.priority)); r.isFail()) return r;
    if (task.dueAt.has_value()) {
        if (auto r = stmt.bind(4, task.dueAt.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(4, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(5, tagsJson); r.isFail()) return r;
    if (task.completedAt.has_value()) {
        if (auto r = stmt.bind(6, task.completedAt.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(6, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(7, now); r.isFail()) return r;
    if (auto r = stmt.bind(8, task.id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::deleteTask(int64_t id) {
    auto prep = db_.prepare("DELETE FROM todo_tasks WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::moveTask(int64_t taskId, int64_t toColumnId, int newPosition) {
    auto txResult = db_.transaction([&]() -> core::Result<void> {
        auto prep1 = db_.prepare("SELECT column_id, position FROM todo_tasks WHERE id = ?");
        if (prep1.isFail()) {
            return core::Result<void>::fail(prep1.errorCode(), prep1.errorMessage());
        }
        auto& stmt1 = prep1.value();
        if (auto r = stmt1.bind(1, taskId); r.isFail()) return r;
        auto rows1 = stmt1.fetchAll();
        if (rows1.isFail()) {
            return core::Result<void>::fail(rows1.errorCode(), rows1.errorMessage());
        }
        if (rows1.value().empty()) {
            return core::Result<void>::fail(core::ErrorCode::NotFound, "task not found");
        }
        int64_t fromColumnId = rows1.value()[0].getInt64("column_id").valueOr(0);
        int oldPos = static_cast<int>(rows1.value()[0].getInt64("position").valueOr(0));

        // 暂挪到目标列 + position = -1（避免后续 update 冲突）
        auto prep2 = db_.prepare(
            "UPDATE todo_tasks SET column_id = ?, position = -1 WHERE id = ?");
        if (prep2.isFail()) {
            return core::Result<void>::fail(prep2.errorCode(), prep2.errorMessage());
        }
        auto& stmt2 = prep2.value();
        if (auto r = stmt2.bind(1, toColumnId); r.isFail()) return r;
        if (auto r = stmt2.bind(2, taskId); r.isFail()) return r;
        auto step2 = stmt2.step();
        if (step2.isFail()) {
            return core::Result<void>::fail(step2.errorCode(), step2.errorMessage());
        }

        // 老列 position > oldPos 的下移 1 位（T 已在新列，不影响）
        if (fromColumnId != toColumnId) {
            auto prep3 = db_.prepare(
                "UPDATE todo_tasks SET position = position - 1 "
                "WHERE column_id = ? AND position > ?");
            if (prep3.isFail()) {
                return core::Result<void>::fail(prep3.errorCode(), prep3.errorMessage());
            }
            auto& stmt3 = prep3.value();
            if (auto r = stmt3.bind(1, fromColumnId); r.isFail()) return r;
            if (auto r = stmt3.bind(2, static_cast<int64_t>(oldPos)); r.isFail()) return r;
            auto step3 = stmt3.step();
            if (step3.isFail()) {
                return core::Result<void>::fail(step3.errorCode(), step3.errorMessage());
            }
        }

        // 新列 position >= newPos 的上移 1 位（排除 T at -1）
        auto prep4 = db_.prepare(
            "UPDATE todo_tasks SET position = position + 1 "
            "WHERE column_id = ? AND position >= ? AND id != ?");
        if (prep4.isFail()) {
            return core::Result<void>::fail(prep4.errorCode(), prep4.errorMessage());
        }
        auto& stmt4 = prep4.value();
        if (auto r = stmt4.bind(1, toColumnId); r.isFail()) return r;
        if (auto r = stmt4.bind(2, static_cast<int64_t>(newPosition)); r.isFail()) return r;
        if (auto r = stmt4.bind(3, taskId); r.isFail()) return r;
        auto step4 = stmt4.step();
        if (step4.isFail()) {
            return core::Result<void>::fail(step4.errorCode(), step4.errorMessage());
        }

        auto prep5 = db_.prepare("UPDATE todo_tasks SET position = ? WHERE id = ?");
        if (prep5.isFail()) {
            return core::Result<void>::fail(prep5.errorCode(), prep5.errorMessage());
        }
        auto& stmt5 = prep5.value();
        if (auto r = stmt5.bind(1, static_cast<int64_t>(newPosition)); r.isFail()) return r;
        if (auto r = stmt5.bind(2, taskId); r.isFail()) return r;
        auto step5 = stmt5.step();
        if (step5.isFail()) {
            return core::Result<void>::fail(step5.errorCode(), step5.errorMessage());
        }

        const int64_t now = nowEpochSeconds();
        auto prep6 = db_.prepare("UPDATE todo_tasks SET updated_at = ? WHERE id = ?");
        if (prep6.isFail()) {
            return core::Result<void>::fail(prep6.errorCode(), prep6.errorMessage());
        }
        auto& stmt6 = prep6.value();
        if (auto r = stmt6.bind(1, now); r.isFail()) return r;
        if (auto r = stmt6.bind(2, taskId); r.isFail()) return r;
        auto step6 = stmt6.step();
        if (step6.isFail()) {
            return core::Result<void>::fail(step6.errorCode(), step6.errorMessage());
        }

        // 归档时间戳：进归档列置 now，出归档列置 NULL，其它列间移动留 NULL。
        // 只读判据走列 kind（运行期真相），不靠 archived_at 值——拖出归档列后 archived_at 被清，
        // 但保留在其它列上的卡不应该因 archived_at 残留而被误判只读。
        auto prepKind = db_.prepare("SELECT kind FROM todo_columns WHERE id = ?");
        if (prepKind.isFail()) {
            return core::Result<void>::fail(prepKind.errorCode(), prepKind.errorMessage());
        }
        auto& stmtKind = prepKind.value();
        if (auto r = stmtKind.bind(1, toColumnId); r.isFail()) return r;
        auto kindRows = stmtKind.fetchAll();
        std::string targetKind;
        if (kindRows.isOk() && !kindRows.value().empty()) {
            auto k = kindRows.value()[0].getString("kind");
            if (k.isOk()) targetKind = k.value();
        }
        if (targetKind == "archive") {
            auto prepA = db_.prepare("UPDATE todo_tasks SET archived_at = ? WHERE id = ?");
            if (prepA.isFail()) {
                return core::Result<void>::fail(prepA.errorCode(), prepA.errorMessage());
            }
            auto& stmtA = prepA.value();
            if (auto r = stmtA.bind(1, now); r.isFail()) return r;
            if (auto r = stmtA.bind(2, taskId); r.isFail()) return r;
            auto stepA = stmtA.step();
            if (stepA.isFail()) {
                return core::Result<void>::fail(stepA.errorCode(), stepA.errorMessage());
            }
        } else {
            auto prepA = db_.prepare("UPDATE todo_tasks SET archived_at = NULL WHERE id = ?");
            if (prepA.isFail()) {
                return core::Result<void>::fail(prepA.errorCode(), prepA.errorMessage());
            }
            auto& stmtA = prepA.value();
            if (auto r = stmtA.bind(1, taskId); r.isFail()) return r;
            auto stepA = stmtA.step();
            if (stepA.isFail()) {
                return core::Result<void>::fail(stepA.errorCode(), stepA.errorMessage());
            }
        }

        return core::Result<void>::ok();
    });
    return txResult;
}

core::Result<void> TodoService::completeTask(int64_t id) {
    const int64_t now = nowEpochSeconds();

    auto prep = db_.prepare(
        "UPDATE todo_tasks SET completed_at = ?, updated_at = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, now); r.isFail()) return r;
    if (auto r = stmt.bind(2, now); r.isFail()) return r;
    if (auto r = stmt.bind(3, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> TodoService::uncompleteTask(int64_t id) {
    const int64_t now = nowEpochSeconds();

    auto prep = db_.prepare(
        "UPDATE todo_tasks SET completed_at = NULL, updated_at = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, now); r.isFail()) return r;
    if (auto r = stmt.bind(2, id); r.isFail()) return r;
    auto stepResult = stmt.step();
    if (stepResult.isFail()) {
        return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<Task>> TodoService::search(const std::string& query) {
    if (query.empty()) {
        return listAllTasks();
    }
    auto prep = db_.prepare(
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at, archived_at "
        "FROM todo_tasks "
        "WHERE title LIKE ? ESCAPE '\\' "
        "   OR (description IS NOT NULL AND description LIKE ? ESCAPE '\\') "
        "ORDER BY column_id ASC, position ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Task>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    const std::string like = "%" + escapeLikePattern(query) + "%";
    if (auto r = stmt.bind(1, like); r.isFail()) {
        return core::Result<std::vector<Task>>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = stmt.bind(2, like); r.isFail()) {
        return core::Result<std::vector<Task>>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Task>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto taskResult = parseTaskRow(row);
        if (taskResult.isFail()) {
            return core::Result<std::vector<Task>>::fail(
                taskResult.errorCode(), taskResult.errorMessage());
        }
        out.push_back(std::move(taskResult).value());
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

core::Result<std::vector<Task>> TodoService::listOverdue() {
    const int64_t now = nowEpochSeconds();

    // 逾期谓词单源：core/overdue.h 的 kWhereSql（与 HomeService::getStats 共用，改一处两边同步）
    const std::string sql =
        std::string(
            "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at, archived_at "
            "FROM todo_tasks WHERE ") +
        core::overdue::kWhereSql + " ORDER BY due_at ASC";
    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<std::vector<Task>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, now); r.isFail()) {
        return core::Result<std::vector<Task>>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Task>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<Task> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto taskResult = parseTaskRow(row);
        if (taskResult.isFail()) {
            return core::Result<std::vector<Task>>::fail(
                taskResult.errorCode(), taskResult.errorMessage());
        }
        out.push_back(std::move(taskResult).value());
    }
    return core::Result<std::vector<Task>>::ok(std::move(out));
}

} // namespace module::todo