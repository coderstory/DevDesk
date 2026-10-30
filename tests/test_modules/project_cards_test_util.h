// tests/test_modules/project_cards_test_util.h
// 需-11 项目卡测试共享夹具：临时目录里的全新 SQLite 库 + 完整 schema 迁移。
// 三个测试 TU（存储 / MCP 工具 / UI 卡）共用，保证「同一套库形态」。
#pragma once

#include "core/schema.h"
#include "core/sqlite_db.h"
#include "test_support.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace devdesk {

struct ProjectCardsDbEnv {
    std::filesystem::path path;
    std::unique_ptr<core::data::SqliteDb> db;

    explicit ProjectCardsDbEnv(const std::string& tag = "shared") {
        path = test_support::tempDbPath("devdesk_test_pcards_", tag);
        auto opened = core::data::SqliteDb::open(path);
        if (opened.isFail()) {
            throw std::runtime_error("ProjectCardsDbEnv open failed: " +
                                     opened.errorMessage());
        }
        db = std::make_unique<core::data::SqliteDb>(std::move(opened.value()));
        auto migrated = core::schema::migrate(*db);
        if (migrated.isFail()) {
            throw std::runtime_error("ProjectCardsDbEnv migrate failed: " +
                                     migrated.errorMessage());
        }
    }

    ProjectCardsDbEnv(const ProjectCardsDbEnv&) = delete;
    ProjectCardsDbEnv& operator=(const ProjectCardsDbEnv&) = delete;
};

}  // namespace devdesk
