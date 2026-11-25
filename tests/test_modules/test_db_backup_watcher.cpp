// 数据库每小时整点备份（need455）测试：整点跨桶触发 / 不足一小时不触发 /
// 保留份数裁剪 / 设置开关关闭不触发 / 库文件缺失失败不崩。
// 假时钟驱动：watcher.tick(now) 直接注入 unix 秒，不经真实 ticker。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "module_schedule/db_backup_watcher.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* kDbPrefix = "devdesk_test_hourlydbbackup_";
// 1800000000 是 3600 的整数倍：作「恰好整点」的相位起点
constexpr int64_t kHourTop = 1800000000;
constexpr int64_t kHourSec = 3600;

std::string backupPrefix(const fs::path& dbFile) {
    return dbFile.filename().string() + ".backup-";
}

std::vector<fs::path> backupsOf(const fs::path& dbFile) {
    std::vector<fs::path> out;
    const std::string prefix = backupPrefix(dbFile);
    std::error_code ec;
    for (fs::directory_iterator it(dbFile.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0) == 0) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void removeBackups(const fs::path& dbFile) {
    for (const auto& p : backupsOf(dbFile)) {
        std::error_code ec;
        fs::remove(p, ec);
    }
}

// 真库 + 迁移（备份是另开连接的在线拷贝，主库需真实存在）
struct DbFixture {
    fs::path dbFile;
    std::optional<core::Database> db;

    explicit DbFixture(const std::string& tag) {
        dbFile = test_support::tempDbPath(kDbPrefix, tag);
        std::error_code ec;
        removeBackups(dbFile);
        fs::remove(dbFile, ec);
        fs::remove(fs::path(dbFile.string() + "-wal"), ec);
        fs::remove(fs::path(dbFile.string() + "-shm"), ec);
        auto opened = core::Database::open(dbFile);
        if (opened.isFail()) {
            std::fprintf(stderr, "DbFixture(%s): %s\n", tag.c_str(),
                         opened.errorMessage().c_str());
            std::exit(1);
        }
        db.emplace(std::move(opened.value()));
        auto migrated = db->migrate();
        if (migrated.isFail()) {
            std::fprintf(stderr, "DbFixture(%s) migrate: %s\n", tag.c_str(),
                         migrated.errorMessage().c_str());
            std::exit(1);
        }
    }

    ~DbFixture() {
        db.reset();
        removeBackups(dbFile);
        std::error_code ec;
        fs::remove(dbFile, ec);
        fs::remove(fs::path(dbFile.string() + "-wal"), ec);
        fs::remove(fs::path(dbFile.string() + "-shm"), ec);
    }
};

// 全局开关是用例间共享的进程级状态：用例结束必须还原，防串味
struct HourlyEnabledGuard {
    bool prev;
    explicit HourlyEnabledGuard(bool on)
        : prev(module::schedule::hourlyDbBackupEnabled()) {
        module::schedule::setHourlyDbBackupEnabled(on);
    }
    ~HourlyEnabledGuard() { module::schedule::setHourlyDbBackupEnabled(prev); }
};

}  // namespace

// 防：相位错（首个 tick 就备 = 启动触发）/ 整点不触发
REGISTER_TEST(db_backup_hourly_fires_at_hour_top,
              "db_backup: 跨整点才生成——首 tick 只对相位（不做启动触发），+3599s 不备、+3600s 产出一份", {
    DbFixture fx("fire");
    HourlyEnabledGuard g(true);
    module::schedule::DbBackupWatcher w(fx.dbFile);
    DOCTEST_CHECK_FALSE(w.tick(kHourTop));   // 启动时刻 = 恰好整点：只对相位
    DOCTEST_CHECK(backupsOf(fx.dbFile).empty());
    DOCTEST_CHECK_FALSE(w.tick(kHourTop + 1));
    DOCTEST_CHECK_FALSE(w.tick(kHourTop + kHourSec - 1));
    DOCTEST_CHECK(backupsOf(fx.dbFile).empty());
    DOCTEST_CHECK(w.tick(kHourTop + kHourSec));   // 跨整点 → 触发
    DOCTEST_CHECK_EQ(backupsOf(fx.dbFile).size(), static_cast<std::size_t>(1));
    return true;
})

// 防：1Hz 泵被误当触发源（每 tick 备份刷盘）
REGISTER_TEST(db_backup_sub_hour_no_fire,
              "db_backup: 不足一小时不生成——同小时桶内逐十分钟 tick 59 分钟零备份", {
    DbFixture fx("subhour");
    HourlyEnabledGuard g(true);
    module::schedule::DbBackupWatcher w(fx.dbFile);
    const int64_t arm = kHourTop + 60;   // 启动于整点后 1 分钟
    DOCTEST_CHECK_FALSE(w.tick(arm));
    for (int64_t t = arm + 600; t < kHourTop + kHourSec; t += 600) {
        DOCTEST_CHECK_FALSE(w.tick(t));
    }
    DOCTEST_CHECK(backupsOf(fx.dbFile).empty());
    return true;
})

// 防：备份无限增长 / 误删新备份（裁剪按文件名时间序）
REGISTER_TEST(db_backup_prune_keeps_last_n,
              "db_backup: 触发后裁剪到保留份数——预置 3 份同名旧备份 + keep=3 → 总数 3、新备份在、最旧被删", {
    DbFixture fx("prune");
    HourlyEnabledGuard g(true);
    const std::string prefix = backupPrefix(fx.dbFile);
    for (const char* ts : {"20200101-000000", "20200102-000000", "20200103-000000"}) {
        std::ofstream(fx.dbFile.parent_path() / (prefix + ts)) << "old";
    }
    module::schedule::DbBackupWatcher w(fx.dbFile, 3);
    DOCTEST_CHECK_FALSE(w.tick(kHourTop));
    DOCTEST_CHECK(w.tick(kHourTop + kHourSec));
    const auto list = backupsOf(fx.dbFile);
    DOCTEST_CHECK_EQ(list.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK(list.front().filename().string() != prefix + "20200101-000000");
    DOCTEST_CHECK(list.back().filename().string() != prefix + "20200103-000000");
    return true;
})

// 防：设置开关失效（关掉仍在备份）/ 重新打开时补跑旧小时
REGISTER_TEST(db_backup_disabled_toggle_skips,
              "db_backup: 开关关闭跨整点不生成；重新打开不补旧小时（相位照常推进）", {
    DbFixture fx("disabled");
    module::schedule::DbBackupWatcher w(fx.dbFile);
    {
        HourlyEnabledGuard g(false);
        DOCTEST_CHECK_FALSE(w.tick(kHourTop));
        DOCTEST_CHECK_FALSE(w.tick(kHourTop + kHourSec));
        DOCTEST_CHECK(backupsOf(fx.dbFile).empty());
    }
    {
        HourlyEnabledGuard g(true);
        // 已在下一小时桶内：开回不补已过去的整点
        DOCTEST_CHECK_FALSE(w.tick(kHourTop + kHourSec + 1));
    }
    DOCTEST_CHECK(backupsOf(fx.dbFile).empty());
    return true;
})

// 防：失败路径崩溃 / 弹窗打扰 / 留半成品备份
REGISTER_TEST(db_backup_missing_db_logs_not_crash,
              "db_backup: 库文件缺失失败路径——tick 返回 false、不崩、无残留备份文件", {
    HourlyEnabledGuard g(true);
    const fs::path missing = test_support::tempDbPath(kDbPrefix, "missing");
    std::error_code ec;
    fs::remove(missing, ec);
    removeBackups(missing);
    module::schedule::DbBackupWatcher w(missing);
    DOCTEST_CHECK_FALSE(w.tick(kHourTop));
    DOCTEST_CHECK_FALSE(w.tick(kHourTop + kHourSec));
    DOCTEST_CHECK(backupsOf(missing).empty());
    return true;
})