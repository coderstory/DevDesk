// tests/test_modules/test_md_folder_tree.cpp
// H27 目录树文件夹服务层单测：folderNoteCount / renameFolder / deleteFolder。
// 每条用例防的具体事故见用例头注释；断言口径与模块层一致（错误走 Result）。
#include "doctest.h"
#include "test_support.h"
#include "core/schema.h"
#include "core/sqlite_db.h"
#include "module_markdown/markdown_service.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

using test_support::writeTextFile;

// in-memory 库（打开失败即 abort）
core::data::SqliteDb makeDb() {
    auto r = core::data::SqliteDb::open(":memory:");
    if (r.isFail()) {
        std::abort();
    }
    return std::move(r.value());
}

void applySchema(core::data::SqliteDb& db) {
    // 落 core::schema::kSql（单份当前 schema，幂等）
    auto r = db.applySchema(core::schema::kSql);
    if (r.isFail()) std::abort();
}

} // namespace

// 场景1（防：只查一级漏深层——深层 .md 所在文件夹被误判为空而放行删除）：
//   多层子文件夹下的 .md 必须被递归计数；纯空子树（含空子夹）计数为 0。
REGISTER_TEST(folder_note_count_recursive,
              "markdown_folder_tree: folderNoteCount 递归计任意层级 .md；空子树计 0", {
    const auto dir = test_support::makeTempDir("devdesk_md_test_", "foldercount");
    auto db = makeDb();
    applySchema(db);
    module::markdown::MarkdownService svc(db, dir);

    // 深层：甲/乙/丙.md（≥2 层）+ 甲/丁.md；空子树：空夹/空子夹/（无 .md）
    writeTextFile(dir / "甲" / "乙" / "丙.md", "# 丙\n");
    writeTextFile(dir / "甲" / "丁.md", "# 丁\n");
    fs::create_directories(dir / "空夹" / "空子夹");

    // 任意层级命中都计数
    auto c = svc.folderNoteCount("甲");
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), 2);
    auto c2 = svc.folderNoteCount("甲/乙");
    DOCTEST_REQUIRE(c2.isOk());
    DOCTEST_CHECK_EQ(c2.value(), 1);
    // 纯空子树 = 0
    auto c3 = svc.folderNoteCount("空夹");
    DOCTEST_REQUIRE(c3.isOk());
    DOCTEST_CHECK_EQ(c3.value(), 0);
    // 不存在的目录 → fail（不静默当 0）
    auto c4 = svc.folderNoteCount("不存在");
    DOCTEST_CHECK(c4.isFail());

    fs::remove_all(dir);
    return true;
})

// 场景2（防：删除误放行 / 只删顶层不删子树）：
//   含 .md（任意层级）→ deleteFolder fail 且磁盘原样；纯空子树 → 整棵删除成功
//   （含空子目录）。
REGISTER_TEST(folder_delete_blocked_and_allowed,
              "markdown_folder_tree: 含 .md 禁删且不动磁盘；空子树整棵删除", {
    const auto dir = test_support::makeTempDir("devdesk_md_test_", "folderdel");
    auto db = makeDb();
    applySchema(db);
    module::markdown::MarkdownService svc(db, dir);

    writeTextFile(dir / "资料" / "深层" / "笔记.md", "# 笔记\n");
    fs::create_directories(dir / "杂物" / "空子夹");

    // 非空（深层 .md）→ fail 且一个文件都不丢
    auto r1 = svc.deleteFolder("资料");
    DOCTEST_CHECK(r1.isFail());
    DOCTEST_CHECK(fs::is_regular_file(dir / "资料" / "深层" / "笔记.md"));

    // 空子树（含空子夹）→ 放行且整棵删除
    auto r2 = svc.deleteFolder("杂物");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(!fs::exists(dir / "杂物"));            // 根没了
    DOCTEST_CHECK(!fs::exists(dir / "杂物" / "空子夹")); // 空子夹一并删

    // 越界路径拒绝删（目录外删除缺口防线）
    auto r3 = svc.deleteFolder("../escape");
    DOCTEST_CHECK(r3.isFail());

    fs::remove_all(dir);
    return true;
})

// 场景3（防：枚举错误被静默当 0——不可读子树笔记所在文件夹被删除闸门放行而
//   remove_all 误删（A-1））：枚举遇错（悬挂软链，POSIX 恒可建）→ folderNoteCount
//   显式 fail（非 0 放行）→ deleteFolder 拒绝删除且磁盘原样。Windows 建软链需
//   特权，建不了则跳过（该路径 POSIX 平台验证）。
REGISTER_TEST(folder_note_count_enum_error_fails,
              "markdown_folder_tree: 枚举遇错显式 fail + 删除闸门拒绝放行", {
    const auto dir = test_support::makeTempDir("devdesk_md_test_", "enumerr");
    auto db = makeDb();
    applySchema(db);
    module::markdown::MarkdownService svc(db, dir);

    fs::create_directories(dir / "异常");
    std::error_code ec;
    fs::create_symlink(dir / "不存在的目标.md", dir / "异常" / "悬挂.md", ec);
    if (ec) {
        // 平台不支持建软链（Windows 无特权）：无断言跳过，POSIX 平台验证
        fs::remove_all(dir);
        return true;
    }

    auto cnt = svc.folderNoteCount("异常");
    DOCTEST_CHECK(cnt.isFail());   // 枚举遇错不静默当 0
    auto r = svc.deleteFolder("异常");
    DOCTEST_CHECK(r.isFail());     // 闸门拒绝放行
    DOCTEST_CHECK(fs::exists(dir / "异常"));  // 磁盘原样

    fs::remove_all(dir);
    return true;
})

// 场景3（防：改名后树显示旧名——索引不随目录名刷新）：
//   同级改名落盘 → rescan 后索引全换新路径、旧路径清零；
//   新名含 / → fail；目标已存在 → AlreadyExists。
REGISTER_TEST(folder_rename_same_level_and_index_refresh,
              "markdown_folder_tree: 同级改名+索引刷新；含 / 与重名报错", {
    const auto dir = test_support::makeTempDir("devdesk_md_test_", "folderrename");
    auto db = makeDb();
    applySchema(db);
    module::markdown::MarkdownService svc(db, dir);

    writeTextFile(dir / "项目" / "周会" / "纪要.md", "# 纪要\n");
    DOCTEST_REQUIRE(svc.rescan().isOk());

    // 新名含 / → 拒绝（同级改名的判定边界）
    auto bad = svc.renameFolder("项目/周会", "项目/新名");
    DOCTEST_CHECK(bad.isFail());
    // 目标已存在 → AlreadyExists
    fs::create_directories(dir / "项目" / "新名");
    auto dup = svc.renameFolder("项目/周会", "新名");
    DOCTEST_CHECK(dup.isFail());
    fs::remove_all(dir / "项目" / "新名");

    // 同级改名：项目/周会 → 项目/例会（新名不含 /，父目录不变）
    auto r = svc.renameFolder("项目/周会", "例会");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("项目/例会"));
    DOCTEST_CHECK(fs::is_directory(dir / "项目" / "例会"));
    DOCTEST_CHECK(!fs::exists(dir / "项目" / "周会"));

    // 索引刷新（改名后树显示旧名正是缺这一步）：rescan 后只有新路径
    auto rs = svc.rescan();
    DOCTEST_REQUIRE(rs.isOk());
    auto list = svc.listAll();
    DOCTEST_REQUIRE(list.isOk());
    std::set<std::string> paths;
    for (const auto& n : list.value()) {
        paths.insert(n.relativePath.generic_string());
    }
    DOCTEST_CHECK_EQ(paths.count("项目/例会/纪要.md"), 1u);
    DOCTEST_CHECK_EQ(paths.count("项目/周会/纪要.md"), 0u);

    fs::remove_all(dir);
    return true;
})
