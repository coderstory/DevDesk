// src/module_settings/project_card_store.h
// 需-11 项目卡存储：project_cards 表的 CRUD 薄封装（纯 SQL、不碰 UI）。
// 表结构见 core/schema.h 的 kSql；连接由调用方注入（生产 = core::appDatabase()，
// 测试 = 临时库）。SqliteDb 自带 recursive_mutex，UI 线程与 MCP worker 线程
// 共用同一连接安全（并发纪律见 sqlite_db.h）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::settings {

// 草稿字段索引（设置页表单 ↔ 模块状态的回写通道用；顺序无关，仅作命名枚举）。
enum class ProjectCardField {
    Name,
    RepoPath,
    StartCommand,
    TestCommand,
    DeployMethod,
    TestDbUrl,
    KeyModules,
};

// 一张项目卡 = 一个业务系统的结构化档案。文本字段空串 = 未填写（与 NULL 无关，
// 表层列即 NOT NULL DEFAULT ''）；name 是主键，trim 后非空才可入库。
struct ProjectCard {
    std::string name;
    std::string repoPath;       // 仓库路径
    std::string startCommand;   // 启动命令
    std::string testCommand;    // 测试命令
    std::string deployMethod;   // 部署方式
    std::string testDbUrl;      // 测试库地址（可能含凭据，不入 SQL dump）
    std::string keyModules;     // 关键模块说明
    int64_t createdAt = 0;      // epoch 秒，首次建卡时刻（更新不刷新）
    int64_t updatedAt = 0;      // epoch 秒，最近一次保存时刻
};

// 全部卡片，按 name 字典序（确定性，列表 UI 与测试断言都依赖稳定序）。
core::Result<std::vector<ProjectCard>> listProjectCards(core::data::SqliteDb& db);

// 按名取卡：不存在 → fail(NotFound)。中文（含前后空格）按 trim 后精确匹配。
core::Result<ProjectCard> getProjectCard(core::data::SqliteDb& db,
                                         const std::string& name);

// 建卡/改卡（name 相同即覆盖六字段并刷新 updated_at；created_at 建卡时写定、
// 更新不动的语义由 INSERT ... ON CONFLICT(name) DO UPDATE 保证）。
// name trim 后为空 → fail(ValidationError)。
core::Result<void> upsertProjectCard(core::data::SqliteDb& db,
                                     const ProjectCard& card);

// 删卡：不存在 → fail(NotFound)（先查后删，双层确认防静默吞误删）。
core::Result<void> deleteProjectCard(core::data::SqliteDb& db,
                                     const std::string& name);

}  // namespace module::settings
