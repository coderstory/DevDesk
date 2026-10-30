// tests/test_modules/test_project_cards.cpp
// 需-11 项目卡存储层 + schema 迁移用例。防的事故：
// - 建卡→重启→卡丢（F-01）：持久化断言用**重新 open 同一路径**模拟重启，不是
//   同一连接上再查一次（后者证不了落盘）。
// - 老库缺列 → 首次写入/读取直接炸（agent context_note 同类报障）：先手工造
//   缺列表再跑补列迁移，断言列补齐且既有行不丢。
// - 改卡覆盖成新行（created_at 漂移 / 六字段没全更新）：upsert 后逐字段断言。
// - 二次保存覆盖 created_at（BUG-4）：两时间戳同为 epoch 秒，同秒内两写测不出
//   覆盖（QA 变异注入仍绿的假绿来源）——断言前必须睡过秒边界（>1s）。
// - 中文名/前后空格：trim 后精确匹配（F-02 的数据层前提）。
#include "doctest.h"

#include "core/schema.h"
#include "core/sqlite_db.h"
#include "core/time.h"
#include "module_settings/project_card_store.h"
#include "project_cards_test_util.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ms = module::settings;

ms::ProjectCard sampleCard(const std::string& name) {
    ms::ProjectCard c;
    c.name = name;
    c.repoPath = "/srv/git/order-center";
    c.startCommand = "make dev";
    c.testCommand = "make test";
    c.deployMethod = "k8s 滚动升级";
    c.testDbUrl = "mysql://order:pwd@test-db:3306/order_center";
    c.keyModules = "order-api / order-worker / pay-gateway";
    return c;
}

}  // namespace

REGISTER_TEST(project_card_store_crud_roundtrip,
              "project_cards: 建卡→查卡→改卡→删卡全链（中文/字段往返）", {
    devdesk::ProjectCardsDbEnv env;
    // 建卡：中文名 + 六字段全部入库
    ms::ProjectCard c = sampleCard("订单中心");
    auto saved = ms::upsertProjectCard(*env.db, c);
    DOCTEST_CHECK(saved.isOk());

    // 查卡：六字段逐项往返（防「只写了 name 其余丢」的半截落库）
    auto got = ms::getProjectCard(*env.db, " 订单中心 ");  // 前后空格 = trim 后命中
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("订单中心"));
    DOCTEST_CHECK_EQ(got.value().repoPath, c.repoPath);
    DOCTEST_CHECK_EQ(got.value().startCommand, c.startCommand);
    DOCTEST_CHECK_EQ(got.value().testCommand, c.testCommand);
    DOCTEST_CHECK_EQ(got.value().deployMethod, c.deployMethod);
    DOCTEST_CHECK_EQ(got.value().testDbUrl, c.testDbUrl);
    DOCTEST_CHECK_EQ(got.value().keyModules, c.keyModules);
    DOCTEST_CHECK(got.value().createdAt > 0);
    DOCTEST_CHECK(got.value().updatedAt >= got.value().createdAt);

    // 改卡：改两个字段，其余不动；created_at 不漂移
    ms::ProjectCard changed = got.value();
    changed.repoPath = "/srv/git/order-center-v2";
    changed.deployMethod = "helm upgrade";
    const int64_t createdBefore = got.value().createdAt;
    auto resaved = ms::upsertProjectCard(*env.db, changed);
    DOCTEST_CHECK(resaved.isOk());
    auto regot = ms::getProjectCard(*env.db, "订单中心");
    DOCTEST_REQUIRE(regot.isOk());
    DOCTEST_CHECK_EQ(regot.value().repoPath, std::string("/srv/git/order-center-v2"));
    DOCTEST_CHECK_EQ(regot.value().deployMethod, std::string("helm upgrade"));
    DOCTEST_CHECK_EQ(regot.value().startCommand, c.startCommand);  // 未改字段保持
    DOCTEST_CHECK_EQ(regot.value().createdAt, createdBefore);
    DOCTEST_CHECK(regot.value().updatedAt >= createdBefore);

    // 列表：两张卡按 name 字典序
    auto second = ms::upsertProjectCard(*env.db, sampleCard("会员系统"));
    DOCTEST_CHECK(second.isOk());
    auto list = ms::listProjectCards(*env.db);
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), std::size_t{2});

    // 删卡：删后查 NotFound、列表少一
    auto removed = ms::deleteProjectCard(*env.db, "订单中心");
    DOCTEST_CHECK(removed.isOk());
    auto missing = ms::getProjectCard(*env.db, "订单中心");
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK_EQ(missing.errorCode(), core::ErrorCode::NotFound);
    auto relist = ms::listProjectCards(*env.db);
    DOCTEST_REQUIRE(relist.isOk());
    DOCTEST_CHECK_EQ(relist.value().size(), std::size_t{1});
    DOCTEST_CHECK_EQ(relist.value()[0].name, std::string("会员系统"));

    // 删不存在的卡：明确失败（防静默吞误删）
    auto ghost = ms::deleteProjectCard(*env.db, "订单中心");
    DOCTEST_CHECK(ghost.isFail());
    DOCTEST_CHECK_EQ(ghost.errorCode(), core::ErrorCode::NotFound);
    return true;
})

REGISTER_TEST(project_card_store_persist_across_reopen,
              "project_cards: 建卡→重开库→卡还在（F-01 持久化）", {
    devdesk::ProjectCardsDbEnv env;
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard("订单中心")).isOk());

    // 重启模拟：销毁原连接，按同一路径重新 open + migrate，再查
    {
        auto reopened = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(reopened.isOk());
        auto migrated = core::schema::migrate(reopened.value());
        DOCTEST_CHECK(migrated.isOk());
        auto got = ms::getProjectCard(reopened.value(), "订单中心");
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().repoPath, std::string("/srv/git/order-center"));
        DOCTEST_CHECK_EQ(got.value().testDbUrl,
                         std::string("mysql://order:pwd@test-db:3306/order_center"));
    }
    return true;
})

REGISTER_TEST(project_card_store_rejects_blank_name,
              "project_cards: 空/纯空白名被拒（ValidationError）", {
    devdesk::ProjectCardsDbEnv env;
    ms::ProjectCard blank = sampleCard("   ");
    auto saved = ms::upsertProjectCard(*env.db, blank);
    DOCTEST_CHECK(saved.isFail());
    DOCTEST_CHECK_EQ(saved.errorCode(), core::ErrorCode::ValidationError);
    auto got = ms::getProjectCard(*env.db, "");
    DOCTEST_CHECK(got.isFail());
    DOCTEST_CHECK_EQ(got.errorCode(), core::ErrorCode::ValidationError);
    auto list = ms::listProjectCards(*env.db);
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK(list.value().empty());
    return true;
})

REGISTER_TEST(project_card_migration_adds_missing_columns,
              "project_cards: 老库缺列表补列迁移（幂等 + 既有行不丢）", {
    devdesk::ProjectCardsDbEnv env;
    // 夹具已跑过完整 migrate、project_cards 已存在——先拆掉再造「老库残表」
    DOCTEST_REQUIRE((*env.db).exec("DROP TABLE IF EXISTS project_cards").isOk());
    // 造「老库」：只有 name + repo_path 两列的残表，带一行数据
    DOCTEST_REQUIRE((*env.db).exec(
                        "CREATE TABLE project_cards_old ("
                        "name TEXT PRIMARY KEY, repo_path TEXT NOT NULL DEFAULT '')")
                        .isOk());
    DOCTEST_REQUIRE((*env.db).exec(
                        "ALTER TABLE project_cards_old RENAME TO project_cards")
                        .isOk());
    DOCTEST_REQUIRE((*env.db).exec(
                        "INSERT INTO project_cards (name, repo_path) "
                        "VALUES ('老系统', '/old/path')")
                        .isOk());

    auto migrated = core::schema::migrateProjectCardColumns(*env.db);
    DOCTEST_CHECK(migrated.isOk());
    // 幂等：再跑一遍零操作不红
    DOCTEST_CHECK(core::schema::migrateProjectCardColumns(*env.db).isOk());

    // 补列后既有行可整卡读出：缺的字段取列默认（空串），行不丢
    auto got = ms::getProjectCard(*env.db, "老系统");
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().repoPath, std::string("/old/path"));
    DOCTEST_CHECK_EQ(got.value().startCommand, std::string(""));
    DOCTEST_CHECK_EQ(got.value().testDbUrl, std::string(""));
    // 补列库上 CRUD 恢复完整语义
    ms::ProjectCard updated = got.value();
    updated.startCommand = "./run.sh";
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, updated).isOk());
    auto regot = ms::getProjectCard(*env.db, "老系统");
    DOCTEST_REQUIRE(regot.isOk());
    DOCTEST_CHECK_EQ(regot.value().startCommand, std::string("./run.sh"));
    return true;
})

REGISTER_TEST(project_card_migration_idempotent_on_current_schema,
              "project_cards: migrate() 全链对新库幂等（连跑两遍不红）", {
    devdesk::ProjectCardsDbEnv env;
    DOCTEST_CHECK(core::schema::migrate(*env.db).isOk());
    DOCTEST_CHECK(core::schema::migrate(*env.db).isOk());
    return true;
})

// BUG-4（2026-10-30）：钉死 upsert 的 created_at 语义「建卡写定、更新不动」。
// 防的回归：改 ON CONFLICT(name) DO UPDATE SET 字段清单时顺手加上
// created_at = excluded.created_at（或等价地改绑新值），二次保存会把建卡时刻
// 覆盖成保存时刻。crud_roundtrip 也断言过 createdAt 不变，但它两次 upsert 同在
// 一个 epoch 秒内完成——覆盖后的新值与首值同为「本秒」，断言照样绿（QA 变异
// 注入后仍绿的假绿来源）。所以这里必须睡过秒边界再断言，变异才现形。
REGISTER_TEST(project_card_created_at_fixed_at_creation,
              "project_cards: 建卡写定 created_at，二次保存不动、只刷新 updated_at", {
    devdesk::ProjectCardsDbEnv env;
    // 建卡：created_at 非空且落在插入动作的墙钟窗口内（与 SUT 同用 nowUnix 口径）
    const int64_t t0 = static_cast<int64_t>(core::time::nowUnix());
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard("订单中心")).isOk());
    const int64_t t1 = static_cast<int64_t>(core::time::nowUnix());
    auto got = ms::getProjectCard(*env.db, "订单中心");
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK(got.value().createdAt > 0);
    DOCTEST_CHECK(got.value().createdAt >= t0);
    DOCTEST_CHECK(got.value().createdAt <= t1);
    const int64_t created = got.value().createdAt;
    const int64_t updated = got.value().updatedAt;

    // 睡 >1s 跨过 epoch 秒边界：同秒内二次保存时，created_at 即便被覆盖成
    // excluded.created_at（本次防的变异）也与首值同秒相等，断言不会红。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    ms::ProjectCard changed = sampleCard("订单中心");
    changed.deployMethod = "helm upgrade";
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, changed).isOk());
    auto regot = ms::getProjectCard(*env.db, "订单中心");
    DOCTEST_REQUIRE(regot.isOk());
    DOCTEST_CHECK_EQ(regot.value().createdAt, created);  // 变异态必红
    DOCTEST_CHECK(regot.value().updatedAt > updated);    // updated_at 恒随保存刷新
    DOCTEST_CHECK_EQ(regot.value().deployMethod, std::string("helm upgrade"));
    return true;
})

// BUG-4 第二闸门：连续多次保存下 created_at 恒定、updated_at 单调递增——
// 防住「第三次起才漏 created_at」或 DO UPDATE SET 清单被反复扩坏的写法。
REGISTER_TEST(project_card_created_at_constant_across_repeated_saves,
              "project_cards: 连续多次保存 created_at 恒定、updated_at 递增", {
    devdesk::ProjectCardsDbEnv env;
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard("会员系统")).isOk());
    auto got = ms::getProjectCard(*env.db, "会员系统");
    DOCTEST_REQUIRE(got.isOk());
    const int64_t created = got.value().createdAt;
    int64_t updated = got.value().updatedAt;
    for (int round = 1; round <= 2; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        ms::ProjectCard c = sampleCard("会员系统");
        c.keyModules = "round " + std::to_string(round);
        DOCTEST_CHECK(ms::upsertProjectCard(*env.db, c).isOk());
        auto again = ms::getProjectCard(*env.db, "会员系统");
        DOCTEST_REQUIRE(again.isOk());
        DOCTEST_CHECK_EQ(again.value().createdAt, created);  // 变异态必红
        DOCTEST_CHECK(again.value().updatedAt > updated);
        updated = again.value().updatedAt;
    }
    return true;
})
