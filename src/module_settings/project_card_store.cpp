// src/module_settings/project_card_store.cpp
#include "project_card_store.h"

#include <initializer_list>
#include <utility>

#include "core/str_util.h"
#include "core/time.h"

namespace module::settings {

namespace {

core::Result<std::string> normalizeName(const std::string& name) {
    std::string trimmed = core::str::trimAscii(name);
    if (trimmed.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::ValidationError, "项目卡名称不能为空");
    }
    return core::Result<std::string>::ok(std::move(trimmed));
}

core::Result<ProjectCard> cardFromRow(const core::data::SqlRow& row) {
    ProjectCard card;
    auto name = row.getString("name");
    if (name.isFail()) {
        return core::Result<ProjectCard>::fail(name.errorCode(),
                                               name.errorMessage());
    }
    card.name = name.value();
    // NOT NULL DEFAULT '' 列：getString 恒成功；防御式逐列传递错误码。
    for (const auto& [field, col] :
         std::initializer_list<std::pair<std::string*, const char*>>{
             {&card.repoPath, "repo_path"},
             {&card.startCommand, "start_command"},
             {&card.testCommand, "test_command"},
             {&card.deployMethod, "deploy_method"},
             {&card.testDbUrl, "test_db_url"},
             {&card.keyModules, "key_modules"}}) {
        auto v = row.getString(col);
        if (v.isFail()) {
            return core::Result<ProjectCard>::fail(v.errorCode(),
                                                   v.errorMessage());
        }
        *field = v.value();
    }
    auto created = row.getInt64("created_at");
    if (created.isFail()) {
        return core::Result<ProjectCard>::fail(created.errorCode(),
                                               created.errorMessage());
    }
    auto updated = row.getInt64("updated_at");
    if (updated.isFail()) {
        return core::Result<ProjectCard>::fail(updated.errorCode(),
                                               updated.errorMessage());
    }
    card.createdAt = created.value();
    card.updatedAt = updated.value();
    return core::Result<ProjectCard>::ok(std::move(card));
}

constexpr const char* kSelectColumns =
    "name, repo_path, start_command, test_command, deploy_method, "
    "test_db_url, key_modules, created_at, updated_at";

}  // namespace

core::Result<std::vector<ProjectCard>> listProjectCards(
    core::data::SqliteDb& db) {
    auto prep = db.prepare(std::string("SELECT ") + kSelectColumns +
                           " FROM project_cards ORDER BY name");
    if (prep.isFail()) {
        return core::Result<std::vector<ProjectCard>>::fail(prep.errorCode(),
                                                            prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<ProjectCard>>::fail(rows.errorCode(),
                                                            rows.errorMessage());
    }
    std::vector<ProjectCard> cards;
    cards.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto card = cardFromRow(row);
        if (card.isFail()) {
            return core::Result<std::vector<ProjectCard>>::fail(
                card.errorCode(), card.errorMessage());
        }
        cards.push_back(std::move(card.value()));
    }
    return core::Result<std::vector<ProjectCard>>::ok(std::move(cards));
}

core::Result<ProjectCard> getProjectCard(core::data::SqliteDb& db,
                                         const std::string& name) {
    auto normalized = normalizeName(name);
    if (normalized.isFail()) {
        return core::Result<ProjectCard>::fail(normalized.errorCode(),
                                               normalized.errorMessage());
    }
    auto prep = db.prepare(std::string("SELECT ") + kSelectColumns +
                           " FROM project_cards WHERE name = ?");
    if (prep.isFail()) {
        return core::Result<ProjectCard>::fail(prep.errorCode(),
                                               prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, normalized.value()); b.isFail()) {
        return core::Result<ProjectCard>::fail(b.errorCode(), b.errorMessage());
    }
    auto first = stmt.step();
    if (first.isFail()) {
        return core::Result<ProjectCard>::fail(first.errorCode(),
                                               first.errorMessage());
    }
    if (!first.value()) {
        return core::Result<ProjectCard>::fail(
            core::ErrorCode::NotFound,
            "未找到名为「" + normalized.value() + "」的项目卡");
    }
    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::Result<ProjectCard>::fail(row.errorCode(),
                                               row.errorMessage());
    }
    return cardFromRow(row.value());
}

core::Result<void> upsertProjectCard(core::data::SqliteDb& db,
                                     const ProjectCard& card) {
    auto normalized = normalizeName(card.name);
    if (normalized.isFail()) {
        return core::Result<void>::fail(normalized.errorCode(),
                                        normalized.errorMessage());
    }
    const int64_t now = static_cast<int64_t>(core::time::nowUnix());
    // ON CONFLICT 不回写 created_at：建卡时刻一次写定；updated_at 恒随保存刷新。
    auto prep = db.prepare(
        "INSERT INTO project_cards (name, repo_path, start_command, "
        "test_command, deploy_method, test_db_url, key_modules, created_at, "
        "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET repo_path = excluded.repo_path, "
        "start_command = excluded.start_command, "
        "test_command = excluded.test_command, "
        "deploy_method = excluded.deploy_method, "
        "test_db_url = excluded.test_db_url, "
        "key_modules = excluded.key_modules, updated_at = excluded.updated_at");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, normalized.value()); b.isFail()) return b;
    if (auto b = stmt.bind(2, card.repoPath); b.isFail()) return b;
    if (auto b = stmt.bind(3, card.startCommand); b.isFail()) return b;
    if (auto b = stmt.bind(4, card.testCommand); b.isFail()) return b;
    if (auto b = stmt.bind(5, card.deployMethod); b.isFail()) return b;
    if (auto b = stmt.bind(6, card.testDbUrl); b.isFail()) return b;
    if (auto b = stmt.bind(7, card.keyModules); b.isFail()) return b;
    if (auto b = stmt.bind(8, now); b.isFail()) return b;
    if (auto b = stmt.bind(9, now); b.isFail()) return b;
    auto step = stmt.step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> deleteProjectCard(core::data::SqliteDb& db,
                                     const std::string& name) {
    // 先查后删：未知名当失败返回（而不是静默 DELETE 0 行），调用方（UI/MCP）
    // 能给出明确反馈。
    auto exists = getProjectCard(db, name);
    if (exists.isFail()) {
        return core::Result<void>::fail(exists.errorCode(),
                                        exists.errorMessage());
    }
    auto prep = db.prepare("DELETE FROM project_cards WHERE name = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, exists.value().name); b.isFail()) return b;
    auto step = stmt.step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

}  // namespace module::settings
