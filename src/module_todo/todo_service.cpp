// src/module_todo/todo_service.cpp
// Todo 看板业务逻辑层实现
#include "todo_service.h"

#include "core/logging.h"
#include "core/result.h"
#include "core/sqlite_db.h"

#include <yyjson.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

namespace module::todo {

namespace {

Priority intToPriority(int v) {
    switch (v) {
        case 3: return Priority::High;
        case 2: return Priority::Medium;
        case 1: return Priority::Low;
        default: return Priority::None;
    }
}

int priorityToInt(Priority p) {
    return static_cast<int>(p);
}

} // namespace

TodoService::TodoService(core::data::SqliteDb& db) : db_(db) {}

// ============ 内部辅助 ============

core::Result<int64_t> TodoService::nowEpochSeconds() {
    return core::Result<int64_t>::ok(static_cast<int64_t>(std::time(nullptr)));
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
    // kind 可能为 NULL（migration v4 之前的行 / 用户新建列）→ 留空 = 普通列
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
    return core::Result<Task>::ok(std::move(t));
}

std::string TodoService::serializeTags(const std::vector<std::string>& tags) {
    if (tags.empty()) {
        return "[]";
    }
    // 手写 JSON 数组序列化（因为 EUI-NEO 编译 yyjson.c 时定义了
    // YYJSON_DISABLE_WRITER=1，导致 yyjson_mut_write_opts 不可用）。
    // 仅支持 string 元素的数组，转义双引号、反斜杠、控制字符。
    std::string out;
    out.reserve(32 + tags.size() * 16);
    out.push_back('[');
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) out.push_back(',');
        out.push_back('"');
        for (char c : tags[i]) {
            switch (c) {
                case '"':  out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\b': out.append("\\b");  break;
                case '\f': out.append("\\f");  break;
                case '\n': out.append("\\n");  break;
                case '\r': out.append("\\r");  break;
                case '\t': out.append("\\t");  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned char>(c));
                        out.append(buf);
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }
    out.push_back(']');
    return out;
}

core::Result<std::vector<std::string>> TodoService::deserializeTags(const std::string& json) {
    if (json.empty()) {
        return core::Result<std::vector<std::string>>::ok({});
    }
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) {
        return core::Result<std::vector<std::string>>::fail(
            core::ErrorCode::JsonParseError, "failed to parse tags JSON");
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::vector<std::string> out;
    if (root && yyjson_is_arr(root)) {
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(root, &iter);
        yyjson_val* val = nullptr;
        while ((val = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (yyjson_is_str(val)) {
                const char* s = yyjson_get_str(val);
                if (s) out.emplace_back(s);
            }
        }
    }
    yyjson_doc_free(doc);
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

// ============ 列操作 ============

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
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<Column>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }

    int pos = nextPosResult.value();
    int64_t now = nowResult.value();

    auto txResult = db_.transaction([&]() -> core::Result<void> {
        auto prep = db_.prepare(
            "INSERT INTO todo_columns (title, position, color, created_at) VALUES (?, ?, NULL, ?)");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        if (auto r = stmt.bind(1, title); r.isFail()) return r;
        if (auto r = stmt.bind(2, static_cast<int64_t>(pos)); r.isFail()) return r;
        if (auto r = stmt.bind(3, now); r.isFail()) return r;
        auto stepResult = stmt.step();
        if (stepResult.isFail()) {
            return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
        }
        return core::Result<void>::ok();
    });
    if (txResult.isFail()) {
        return core::Result<Column>::fail(txResult.errorCode(), txResult.errorMessage());
    }

    auto idPrep = db_.prepare("SELECT last_insert_rowid() AS id");
    if (idPrep.isFail()) {
        return core::Result<Column>::fail(idPrep.errorCode(), idPrep.errorMessage());
    }
    auto idRows = idPrep.value().fetchAll();
    if (idRows.isFail()) {
        return core::Result<Column>::fail(idRows.errorCode(), idRows.errorMessage());
    }

    Column c;
    c.id = idRows.value().empty() ? 0 : idRows.value()[0].getInt64("id").valueOr(0);
    c.title = title;
    c.position = pos;
    c.color = std::nullopt;
    c.kind = "";  // 用户新建列默认普通列（迁移 v4 只回填种子列）
    return core::Result<Column>::ok(std::move(c));
}

core::Result<void> TodoService::renameColumn(int64_t id, const std::string& title) {
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<void>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }

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
    // SQLite 没有 rowsAffected 公开 API，但我们能用 changes() 验证
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
        // 先全部设为 -(position + 1) - 1000000（避免唯一约束冲突）
        auto prep1 = db_.prepare("UPDATE todo_columns SET position = -(position + 1) - 1000000");
        if (prep1.isFail()) {
            return core::Result<void>::fail(prep1.errorCode(), prep1.errorMessage());
        }
        auto step1 = prep1.value().step();
        if (step1.isFail()) {
            return core::Result<void>::fail(step1.errorCode(), step1.errorMessage());
        }

        // 按 ids 顺序重新设置 position = 0, 1, 2, ...
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

// ============ 任务操作 ============

core::Result<std::vector<Task>> TodoService::listTasks(int64_t columnId) {
    auto prep = db_.prepare(
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at "
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
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at "
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
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<Task>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }

    int pos = nextPosResult.value();
    int64_t now = nowResult.value();

    auto txResult = db_.transaction([&]() -> core::Result<void> {
        auto prep = db_.prepare(
            "INSERT INTO todo_tasks "
            "(column_id, title, description, priority, due_at, tags, position, completed_at, created_at, updated_at) "
            "VALUES (?, ?, NULL, 0, NULL, '[]', ?, NULL, ?, ?)");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        if (auto r = stmt.bind(1, columnId); r.isFail()) return r;
        if (auto r = stmt.bind(2, title); r.isFail()) return r;
        if (auto r = stmt.bind(3, static_cast<int64_t>(pos)); r.isFail()) return r;
        if (auto r = stmt.bind(4, now); r.isFail()) return r;
        if (auto r = stmt.bind(5, now); r.isFail()) return r;
        auto stepResult = stmt.step();
        if (stepResult.isFail()) {
            return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
        }
        return core::Result<void>::ok();
    });
    if (txResult.isFail()) {
        return core::Result<Task>::fail(txResult.errorCode(), txResult.errorMessage());
    }

    auto idPrep = db_.prepare("SELECT last_insert_rowid() AS id");
    if (idPrep.isFail()) {
        return core::Result<Task>::fail(idPrep.errorCode(), idPrep.errorMessage());
    }
    auto idRows = idPrep.value().fetchAll();
    if (idRows.isFail()) {
        return core::Result<Task>::fail(idRows.errorCode(), idRows.errorMessage());
    }

    Task t;
    t.id = idRows.value().empty() ? 0 : idRows.value()[0].getInt64("id").valueOr(0);
    t.columnId = columnId;
    t.title = title;
    t.position = pos;
    t.priority = Priority::None;
    t.tags = {};
    return core::Result<Task>::ok(std::move(t));
}

core::Result<void> TodoService::updateTask(const Task& task) {
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<void>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }
    int64_t now = nowResult.value();

    std::string tagsJson = serializeTags(task.tags);

    auto prep = db_.prepare(
        "UPDATE todo_tasks SET "
        "column_id = ?, title = ?, description = ?, priority = ?, "
        "due_at = ?, tags = ?, position = ?, completed_at = ?, updated_at = ? "
        "WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, task.columnId); r.isFail()) return r;
    if (auto r = stmt.bind(2, task.title); r.isFail()) return r;
    if (task.description.has_value()) {
        if (auto r = stmt.bind(3, task.description.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(3, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(4, static_cast<int64_t>(priorityToInt(task.priority))); r.isFail()) return r;
    if (task.dueAt.has_value()) {
        if (auto r = stmt.bind(5, task.dueAt.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(5, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(6, tagsJson); r.isFail()) return r;
    if (auto r = stmt.bind(7, static_cast<int64_t>(task.position)); r.isFail()) return r;
    if (task.completedAt.has_value()) {
        if (auto r = stmt.bind(8, task.completedAt.value()); r.isFail()) return r;
    } else {
        if (auto r = stmt.bind(8, nullptr); r.isFail()) return r;
    }
    if (auto r = stmt.bind(9, now); r.isFail()) return r;
    if (auto r = stmt.bind(10, task.id); r.isFail()) return r;
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
        // 1. 取 task 当前的 column_id 和 position
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

        // 2. 把 task 暂时挪到目标列 + position = -1（避免后续 update 冲突）
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

        // 3. 老列里 position > oldPos 的任务下移 1 位（T 已在新列，不影响）
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

        // 4. 新列里 position >= newPos 的任务上移 1 位（排除 T at -1）
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

        // 5. 把 T 设为 newPosition
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

        // 6. updated_at 刷新
        auto nowR = nowEpochSeconds();
        if (nowR.isFail()) {
            return core::Result<void>::fail(nowR.errorCode(), nowR.errorMessage());
        }
        auto prep6 = db_.prepare("UPDATE todo_tasks SET updated_at = ? WHERE id = ?");
        if (prep6.isFail()) {
            return core::Result<void>::fail(prep6.errorCode(), prep6.errorMessage());
        }
        auto& stmt6 = prep6.value();
        if (auto r = stmt6.bind(1, nowR.value()); r.isFail()) return r;
        if (auto r = stmt6.bind(2, taskId); r.isFail()) return r;
        auto step6 = stmt6.step();
        if (step6.isFail()) {
            return core::Result<void>::fail(step6.errorCode(), step6.errorMessage());
        }

        return core::Result<void>::ok();
    });
    return txResult;
}

core::Result<void> TodoService::completeTask(int64_t id) {
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<void>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }
    int64_t now = nowResult.value();

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
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<void>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }
    int64_t now = nowResult.value();

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
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at "
        "FROM todo_tasks "
        "WHERE title LIKE ? OR (description IS NOT NULL AND description LIKE ?) "
        "ORDER BY column_id ASC, position ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Task>>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    std::string like = "%" + query + "%";
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
    auto nowResult = nowEpochSeconds();
    if (nowResult.isFail()) {
        return core::Result<std::vector<Task>>::fail(nowResult.errorCode(), nowResult.errorMessage());
    }
    int64_t now = nowResult.value();

    auto prep = db_.prepare(
        "SELECT id, column_id, title, description, priority, due_at, tags, position, completed_at "
        "FROM todo_tasks "
        "WHERE due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL "
        "ORDER BY due_at ASC");
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