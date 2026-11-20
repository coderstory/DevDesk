// tests/probes/wallpaper_import_ledger_probe.cpp — need356 B2 真 SQLite 墓碑/台账 probe
//
// 被测对象：module::wallpaper::importer 的墓碑三函数
// （recordDeletedSignature / removeTombstoneSignature / defaultTombstoneDatabasePath）
// 与 core/importer.h 的对账纯逻辑。did 单测只能验纯函数与 POSIX 兜底；「真写进
// SQLite、真按签名查到、坏库不崩」必须真库实测——F-06/F-11 的兜底语义只有这样才算验过。
//
// **为什么不跑完整 runHanamiImport**：它的源目录写死为
// ~/Library/Containers/whbalzac.Huajian/Data/Documents/Videos，probe 无法注入源目录；
// 跑完整流程等于把「花見 App 是否安装 + TCC 是否授权」变成 probe 的红绿前提——
// 那是环境断言不是代码断言。故拆成「真库行为 + 纯逻辑组合」两段，各自可独立判定。
//
// **副作用纪律**：默认库路径在真实数据根下（~/DevDesk/wallpaper/deleted-videos.sqlite），
// 本 probe 会写它。DbGuard 在构造时快照主库 + WAL 伴生三件套（存在则拷到临时目录），
// 析构时先删除 probe 期间产生的一切，再原样还原快照——任何退出路径（含断言失败）
// 都不留痕。库不可读写时整 probe 跳过（77），不报红。
//
// 事实记录（实测，2026-11-21）：乱码库的第一版断言「返回 false」在真实仓里撞出了
// 一个真 bug——openOrCreate 只在 SqliteDb::open 失败时删库，而乱码文件 open 成功、
// 只有建表失败 → 乱码库永久吞掉全部墓碑。该实现现已改为「open 或建表任一步失败都
// 删库重试一次」（同批 B2 修正）。本 probe 按**产品契约**断言（坏库不得拦删除，
// ≤2 次调用内落库），不锁死自愈发生在第几次调用。
#include "test_support.h"

#include "core/sqlite_db.h"
#include "module_wallpaper/core/importer.h"
#include "module_wallpaper/platform/importer_macos.h"

#include <cstdlib>
#include <exception>  // std::exception（main 兜底 catch）
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;

#if defined(__APPLE__)

namespace fs = std::filesystem;

bool gPassed = true;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::cout << "  [ok] " << what << "\n";
    } else {
        gPassed = false;
        std::cerr << "wallpaper_import_ledger_probe: FAIL——" << what << "\n";
    }
}

// 备份/恢复真实墓碑库三件套。SQLite 在 WAL 模式下写出一条数据就可能把页面留在
// <db>-wal 里，只备份主文件会漏数据；-shm 是索引缓存，缺了也能重建，但整组还原
// 语义最稳（还原后首次 open 自会 CHECKPOINT）。
struct DbGuard {
    std::vector<std::pair<fs::path, fs::path>> backups;  // {原件路径, 备份路径}
    std::vector<fs::path> companions;                    // {db, db-wal, db-shm}

    explicit DbGuard(const std::string& dbPath) {
        const fs::path base(dbPath);
        companions = {base, fs::path(base.string() + "-wal"),
                      fs::path(base.string() + "-shm")};
        std::error_code ec;
        const auto backupDir = test_support::makeTempDir("wallpaper_ledger_backup", "db");
        int index = 0;
        for (const fs::path& file : companions) {
            if (!fs::exists(file, ec) || ec) continue;
            const fs::path copy = backupDir / (std::to_string(index++) + "-" +
                                               file.filename().string());
            fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
            if (ec) continue;
            backups.emplace_back(file, copy);
        }
    }

    ~DbGuard() {
        std::error_code ec;
        for (const fs::path& file : companions) fs::remove(file, ec);
        for (auto& [original, backup] : backups) {
            fs::copy_file(backup, original, fs::copy_options::overwrite_existing, ec);
        }
    }

    DbGuard(const DbGuard&) = delete;
    DbGuard& operator=(const DbGuard&) = delete;
};

bool removeCompanions(const std::string& dbPath) {
    std::error_code ec;
    bool ok = true;
    for (const fs::path& file : {fs::path(dbPath), fs::path(dbPath + "-wal"),
                                 fs::path(dbPath + "-shm")}) {
        fs::remove(file, ec);
        if (ec) ok = false;
    }
    return ok;
}

// 用独立连接查墓碑表计数（-1 = 查询失败，不是 0）。
long long tombstoneCount(const std::string& dbPath, const std::string& signature) {
    auto opened = core::data::SqliteDb::open(dbPath);
    if (opened.isFail()) return -1;
    core::data::SqliteDb& db = opened.value();
    auto statement =
        db.prepare("SELECT COUNT(*) FROM deleted_video_signature WHERE signature = ?1");
    if (statement.isFail()) return -1;
    core::data::Statement& handle = statement.value();
    if (handle.bind(1, signature).isFail()) return -1;
    auto stepped = handle.step();
    if (stepped.isFail() || !stepped.value()) return -1;
    auto row = handle.currentRow();
    if (row.isFail()) return -1;
    auto count = row.value().getInt64("COUNT(*)");
    if (count.isFail()) return -1;
    return count.value();
}

bool writeGarbage(const std::string& dbPath) {
    removeCompanions(dbPath);
    std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "wallpaper-probe-garbage-not-a-sqlite-file-0123456789";
    out.close();
    return static_cast<bool>(out);
}

#endif  // __APPLE__

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#if !defined(__APPLE__)
    std::cout << "wallpaper_import_ledger_probe: 非 macOS 平台，跳过"
                 "（花見导入/墓碑仅 macOS 有实现） exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    const std::string dbPath = module::wallpaper::importer::defaultTombstoneDatabasePath();
    if (dbPath.empty()) {
        std::cout << "wallpaper_import_ledger_probe: 墓碑库路径为空，跳过"
                  << " exit=" << kSkipExitCode << "\n";
        return kSkipExitCode;
    }

    // 前置：库目录可写。不可写时整 probe 跳过（不是代码缺陷，是环境限制）。
    std::error_code ec;
    const fs::path dbDir = fs::path(dbPath).parent_path();
    fs::create_directories(dbDir, ec);
    {
        const fs::path probeWrite = dbDir / "wallpaper_probe_write_check.tmp";
        std::ofstream out(probeWrite, std::ios::binary | std::ios::trunc);
        out << "x";
        out.close();
        const bool writable = static_cast<bool>(out);
        fs::remove(probeWrite, ec);
        if (!writable) {
            std::cout << "wallpaper_import_ledger_probe: 数据根不可写（TCC/权限），跳过"
                      << " exit=" << kSkipExitCode << "\n";
            return kSkipExitCode;
        }
    }

    try {
        DbGuard guard(dbPath);

        // ---- 断言 A：墓碑登记幂等（F-06）----
        const std::string signature = std::string("wallpaper_probe_a.mp4") + '\x1f' + "123";
        check(module::wallpaper::importer::recordDeletedSignature(signature),
              "recordDeletedSignature 首次调用返回 true");
        check(module::wallpaper::importer::recordDeletedSignature(signature),
              "recordDeletedSignature 重复调用仍返回 true（INSERT OR IGNORE 幂等）");
        check(tombstoneCount(dbPath, signature) == 1,
              "独立连接查询 deleted_video_signature 计数 == 1");

        // ---- 断言 B：解墓碑（F-09）----
        check(module::wallpaper::importer::removeTombstoneSignature(signature),
              "removeTombstoneSignature 返回 true");
        check(tombstoneCount(dbPath, signature) == 0,
              "解墓碑后计数 == 0");

        // ---- 断言 C：坏库兜底（F-11）----
        // 验「坏库不得拦删除」这条产品契约，而不是锁死自愈发生在第几次调用：
        // 实现正在收敛（open 成功但建表失败也算坏库 → 删库重试一次），断言写成
        // 「≤2 次调用内签名落库」对两种实现都成立，且一旦退化成「乱码库永久吞掉
        // 全部墓碑」这条就必红。
        check(writeGarbage(dbPath), "把库文件写成乱码字节");
        const std::string badSignature = std::string("wallpaper_probe_bad.mp4") + '\x1f' + "9";
        bool badRecorded = false;
        int badAttempts = 0;
        for (int attempt = 1; attempt <= 2 && !badRecorded; ++attempt) {
            badRecorded = module::wallpaper::importer::recordDeletedSignature(badSignature);
            badAttempts = attempt;
        }
        check(badRecorded, "乱码库不拦删除：≤2 次调用内 recordDeletedSignature 返回 true"
                           "（第 " + std::to_string(badAttempts) + " 次成功）");
        check(tombstoneCount(dbPath, badSignature) == 1,
              "自愈/重建后乱码场景的签名计数 == 1");

        // 对照：人工把库删干净后单次调用即成功（证明上面的成立不是靠巧合重试）。
        check(removeCompanions(dbPath), "人工删除坏库三件套");
        check(module::wallpaper::importer::recordDeletedSignature(badSignature),
              "删库后重建 → recordDeletedSignature 返回 true（坏库可弃、永不拦删除）");
        check(tombstoneCount(dbPath, badSignature) == 1,
              "重建库中新签名计数 == 1");

        // ---- 断言 D：对账纯逻辑组合（F-08）----
        const std::string sig = module::wallpaper::importer::makeSignature("a.mp4", 100);
        check(sig == std::string("a.mp4") + '\x1f' + "100",
              "makeSignature = 文件名 + \\x1f + 字节大小");
        check(module::wallpaper::importer::signatureFileName(sig) == "a.mp4",
              "signatureFileName 还原文件名");
        const test_support::TempDir reconcileDir("wallpaper_probe_reconcile");
        const fs::path target = reconcileDir.file("a.mp4");
        reconcileDir.write(target, "content");
        check(!module::wallpaper::importer::shouldTombstoneOnReconcile(
                  reconcileDir.exists(target)),
              "目标文件仍在 → shouldTombstoneOnReconcile == false（不补墓碑）");
        reconcileDir.remove(target);
        check(module::wallpaper::importer::shouldTombstoneOnReconcile(
                  reconcileDir.exists(target)),
              "目标文件已消失 → shouldTombstoneOnReconcile == true（补墓碑）");
    } catch (const std::exception& error) {
        std::cerr << "wallpaper_import_ledger_probe: 异常——" << error.what() << "\n";
        return 1;
    }

    if (!gPassed) {
        std::cerr << "wallpaper_import_ledger_probe: FAIL\n";
        return 1;
    }
    std::cout << "wallpaper_import_ledger_probe: PASS（真实库已恢复原状）\n";
    return 0;
#endif
}
