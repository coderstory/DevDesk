// tests/test_modules/test_markdown_service.cpp
// MarkdownService 单元测试
//
// 测试策略：
//   - 用 temp dir 作为笔记根
//   - 每个测试独立目录，独立 in-memory DB
//   - 覆盖：rescan / write+read / delete / search / listInDirectory / 索引同步
#include "doctest.h"
#include "core/database.h"
#include "core/paths.h"
#include "core/sqlite_db.h"
#include "module_markdown/markdown_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

// 创建一个临时目录，返回路径（已确保干净）
fs::path makeTempDir(const std::string& tag) {
    static int counter = 0;
    ++counter;
    fs::path base = fs::temp_directory_path();
    fs::path dir = base / ("devdesk_md_test_" + tag + "_" +
                          std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);           // 清掉上次失败留下的残骸
    fs::create_directories(dir, ec);
    return dir;
}

// 把字符串写入文件（覆盖）
bool writeTextFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// 在 in-memory DB 上跑 migrations，返回数据库实例（失败则 abort）
core::data::SqliteDb makeDb() {
    auto r = core::data::SqliteDb::open(":memory:");
    if (r.isFail()) {
        std::abort();
    }
    return std::move(r.unsafeValue());
}

void runMigrations(core::data::SqliteDb& db) {
    // 复用 migrations.h 中的 kCreateInitialTables（version 1）
    auto r = db.exec(R"(
        CREATE TABLE IF NOT EXISTS markdown_index (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            title TEXT,
            size_bytes INTEGER NOT NULL,
            modified_at INTEGER NOT NULL,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS markdown_tags (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL
        );
        CREATE TABLE IF NOT EXISTS markdown_note_tags (
            note_id INTEGER NOT NULL,
            tag_id INTEGER NOT NULL,
            PRIMARY KEY (note_id, tag_id),
            FOREIGN KEY (note_id) REFERENCES markdown_index(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id) REFERENCES markdown_tags(id) ON DELETE CASCADE
        );
        CREATE VIRTUAL TABLE IF NOT EXISTS markdown_fts USING fts5(
            path UNINDEXED,
            title,
            content,
            tokenize = 'unicode61 remove_diacritics 2'
        );
    )");
    if (r.isFail()) std::abort();
}

} // namespace

// ============ 测试 ============

REGISTER_TEST(md_rescan_finds_new_files,
              "MarkdownService: rescan discovers new .md files", {
    const auto dir = makeTempDir("rescan_new");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    // 初始：写两个文件
    writeTextFile(dir / "alpha.md", "# Alpha\n\nFirst note.");
    writeTextFile(dir / "inbox" / "beta.md", "# Beta\n\nIn inbox.");

    auto r = svc.rescan();
    DOCTEST_REQUIRE(r.isOk());

    auto list = svc.listAll();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), 2u);

    // 验证 path 是 POSIX 风格
    std::set<std::string> paths;
    for (const auto& n : list.value()) {
        paths.insert(n.relativePath.generic_string());
    }
    DOCTEST_CHECK(paths.count("alpha.md") == 1);
    DOCTEST_CHECK(paths.count(std::string("inbox/beta.md")) == 1);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_rescan_removes_deleted_files,
              "MarkdownService: rescan removes deleted files from index", {
    const auto dir = makeTempDir("rescan_del");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    writeTextFile(dir / "keep.md", "# Keep\n\nStay.");
    writeTextFile(dir / "remove.md", "# Remove\n\nBye.");
    DOCTEST_REQUIRE(svc.rescan().isOk());
    DOCTEST_CHECK_EQ(svc.listAll().value().size(), 2u);

    // 物理删除 remove.md
    fs::remove(dir / "remove.md");
    DOCTEST_REQUIRE(svc.rescan().isOk());

    auto list = svc.listAll();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), 1u);
    DOCTEST_CHECK_EQ(list.value()[0].relativePath.generic_string(),
                     std::string("keep.md"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_rescan_updates_modified_files,
              "MarkdownService: rescan detects file modification", {
    const auto dir = makeTempDir("rescan_mod");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    writeTextFile(dir / "doc.md", "# Old\n\nOriginal content with magic_keyword.");
    DOCTEST_REQUIRE(svc.rescan().isOk());

    auto before = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(before.size(), 1u);
    const int64_t oldSize = before[0].sizeBytes;

    // 修改内容（确保 mtime 改变）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    writeTextFile(dir / "doc.md", "# New\n\nUpdated content with new_keyword xyz.");
    DOCTEST_REQUIRE(svc.rescan().isOk());

    auto after = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(after.size(), 1u);
    DOCTEST_CHECK(after[0].sizeBytes != oldSize);
    DOCTEST_CHECK_EQ(after[0].title, std::string("New"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_write_read_roundtrip,
              "MarkdownService: write + read roundtrip preserves content", {
    const auto dir = makeTempDir("rw");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    const std::string content =
        "# 中文标题\n\n这是包含 UTF-8 的内容。\n\n```cpp\nint x = 1;\n```\n";
    DOCTEST_REQUIRE(svc.write("notes/test.md", content).isOk());

    auto read = svc.read("notes/test.md");
    DOCTEST_REQUIRE(read.isOk());
    DOCTEST_CHECK_EQ(read.value(), content);

    // 索引里也应可见
    auto list = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(list.size(), 1u);
    DOCTEST_CHECK_EQ(list[0].title, std::string("中文标题"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_create_new_note,
              "MarkdownService: create new note with template", {
    const auto dir = makeTempDir("create");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("inbox/ideas.md").isOk());

    auto read = svc.read("inbox/ideas.md");
    DOCTEST_REQUIRE(read.isOk());
    DOCTEST_CHECK(read.value().find("# 标题") != std::string::npos ||
                  read.value().find("# ideas") != std::string::npos);

    auto list = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(list.size(), 1u);
    DOCTEST_CHECK_EQ(list[0].title, std::string("ideas"));

    // 重复创建应报错
    auto dup = svc.create("inbox/ideas.md");
    DOCTEST_CHECK(dup.isFail());

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_remove_deletes_file_and_index,
              "MarkdownService: remove deletes file and clears index", {
    const auto dir = makeTempDir("remove");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("to_del.md").isOk());
    DOCTEST_CHECK(fs::exists(dir / "to_del.md"));
    DOCTEST_CHECK_EQ(svc.listAll().value().size(), 1u);

    DOCTEST_REQUIRE(svc.remove("to_del.md").isOk());
    DOCTEST_CHECK_FALSE(fs::exists(dir / "to_del.md"));
    DOCTEST_CHECK_EQ(svc.listAll().value().size(), 0u);

    // 删除不存在的文件不报错
    auto r = svc.remove("nope.md");
    DOCTEST_CHECK(r.isOk());

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_search_finds_matching_notes,
              "MarkdownService: FTS5 search returns matching notes", {
    const auto dir = makeTempDir("search");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write(
        "a.md",
        "# Markdown Notes\n\nThis note discusses DevDesk architecture.").isOk());
    DOCTEST_REQUIRE(svc.write(
        "b.md",
        "# Random\n\nJust some content with apple and banana.").isOk());
    DOCTEST_REQUIRE(svc.write(
        "c.md",
        "# Architecture\n\nAnother doc about architecture patterns.").isOk());

    // 搜索 "architecture" 应该命中 a.md 和 c.md
    auto r1 = svc.search("architecture");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().size(), 2u);

    // 搜索 "apple" 应该只命中 b.md
    auto r2 = svc.search("apple");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().size(), 1u);
    DOCTEST_CHECK_EQ(r2.value()[0].relativePath.generic_string(),
                     std::string("b.md"));

    // 搜索没有匹配 → 空结果（不报错）
    auto r3 = svc.search("nonexistent_keyword_xyz");
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value().size(), 0u);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_search_unicode,
              "MarkdownService: FTS5 search handles Chinese (CJK)", {
    const auto dir = makeTempDir("search_cjk");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write(
        "1.md", "# 设计稿\n\n讨论 DevDesk 的用户界面。").isOk());
    DOCTEST_REQUIRE(svc.write(
        "2.md", "# 算法\n\n介绍排序算法。").isOk());

    auto r = svc.search("界面");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().size(), 1u);
    DOCTEST_CHECK_EQ(r.value()[0].relativePath.generic_string(),
                     std::string("1.md"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_list_by_directory,
              "MarkdownService: listInDirectory filters by parent directory", {
    const auto dir = makeTempDir("listdir");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("root1.md").isOk());
    DOCTEST_REQUIRE(svc.create("root2.md").isOk());
    DOCTEST_REQUIRE(svc.create("inbox/idea1.md").isOk());
    DOCTEST_REQUIRE(svc.create("inbox/idea2.md").isOk());
    DOCTEST_REQUIRE(svc.create("inbox/sub/nested.md").isOk());
    DOCTEST_REQUIRE(svc.create("archive/old.md").isOk());

    // 根目录：root1, root2（直接子，不含 inbox/*）
    auto root = svc.listInDirectory(fs::path(""));
    DOCTEST_REQUIRE(root.isOk());
    DOCTEST_CHECK_EQ(root.value().size(), 2u);

    // inbox：idea1, idea2（不含 inbox/sub/*）
    auto inbox = svc.listInDirectory(fs::path("inbox"));
    DOCTEST_REQUIRE(inbox.isOk());
    DOCTEST_CHECK_EQ(inbox.value().size(), 2u);

    // inbox/sub
    auto sub = svc.listInDirectory(fs::path("inbox/sub"));
    DOCTEST_REQUIRE(sub.isOk());
    DOCTEST_CHECK_EQ(sub.value().size(), 1u);

    // archive
    auto arc = svc.listInDirectory(fs::path("archive"));
    DOCTEST_REQUIRE(arc.isOk());
    DOCTEST_CHECK_EQ(arc.value().size(), 1u);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_extract_title,
              "MarkdownService: extractTitle handles heading + filename fallback", {
    using module::markdown::MarkdownService;

    // 从第一行 # 提取
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("# My Title\n\nbody", "file.md"),
                     std::string("My Title"));
    // 跳过空行/前导空白
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("\n\n#   Spaced  \nbody", "x.md"),
                     std::string("Spaced"));
    // 多级标题仍然工作（extract 二级）
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("## Sub\nbody", "x.md"),
                     std::string("Sub"));
    // 没有 # → 用文件名
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("Just text\n", "fallback.md"),
                     std::string("fallback"));
    // 中文标题
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("# 中文标题\n", "x.md"),
                     std::string("中文标题"));

    // 静态方法直接测试
    DOCTEST_CHECK_EQ(MarkdownService::extractTitle("# T", "x.md"),
                     std::string("T"));
    return true;
})

REGISTER_TEST(md_index_sync_correct,
              "MarkdownService: write triggers index + FTS sync", {
    const auto dir = makeTempDir("sync");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    // 不通过 rescan，直接 write → 索引应该立即更新
    DOCTEST_REQUIRE(svc.write("foo.md",
        "# Foo\n\nThis contains special_token_123.").isOk());

    // listAll 应该看到
    auto list = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(list.size(), 1u);
    DOCTEST_CHECK_EQ(list[0].title, std::string("Foo"));

    // search 应该命中
    auto sr = svc.search("special_token_123");
    DOCTEST_REQUIRE(sr.isOk());
    DOCTEST_CHECK_EQ(sr.value().size(), 1u);

    // 改写覆盖
    DOCTEST_REQUIRE(svc.write("foo.md", "# Foo2\n\nUpdated.").isOk());
    auto list2 = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(list2.size(), 1u);
    DOCTEST_CHECK_EQ(list2[0].title, std::string("Foo2"));

    // 搜索旧词找不到
    auto sr2 = svc.search("special_token_123");
    DOCTEST_REQUIRE(sr2.isOk());
    DOCTEST_CHECK_EQ(sr2.value().size(), 0u);

    // FTS 行不应随覆盖写而累积（path 必须唯一）
    auto ftsPrep = db.prepare(
        "SELECT COUNT(*) AS n FROM markdown_fts WHERE path = 'foo.md'");
    DOCTEST_REQUIRE(ftsPrep.isOk());
    auto ftsRows = ftsPrep.value().fetchAll();
    DOCTEST_REQUIRE(ftsRows.isOk());
    DOCTEST_REQUIRE_EQ(ftsRows.value().size(), 1u);
    DOCTEST_CHECK_EQ(ftsRows.value()[0].getInt64("n").value(), 1);

    // 连续多写 10 次，索引仍然只有一行
    for (int i = 0; i < 10; ++i) {
        DOCTEST_REQUIRE(svc.write("foo.md",
            "# Foo\n\nrevision " + std::to_string(i)).isOk());
    }
    auto ftsPrep2 = db.prepare(
        "SELECT COUNT(*) AS n FROM markdown_fts WHERE path = 'foo.md'");
    DOCTEST_REQUIRE(ftsPrep2.isOk());
    auto ftsRows2 = ftsPrep2.value().fetchAll();
    DOCTEST_REQUIRE(ftsRows2.isOk());
    DOCTEST_CHECK_EQ(ftsRows2.value()[0].getInt64("n").value(), 1);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_mtime_epoch_sane,
              "MarkdownService: indexed mtime is real unix epoch (anti-false-negative)", {
    // W7 P0-5 回归防护：旧代码直接 duration_cast(file_clock)，MinGW 的
    // file_clock epoch 非 1970，写入的时间戳荒谬（Home「今日笔记」恒 0）。
    // 旧测试用 SQL 直插合成时间戳绕过了这条链路，本用例走真实 write/rescan。
    const auto dir = makeTempDir("mtime");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write("fresh.md", "# Fresh\n\nJust written.").isOk());
    // rescan 走磁盘 mtime 读取链路（write 内部也走 syncOneToIndex，双保险）
    DOCTEST_REQUIRE(svc.rescan().isOk());

    auto list = svc.listAll();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t mtime = list.value()[0].modifiedAt;
    DOCTEST_CHECK(mtime > now - 86400);
    DOCTEST_CHECK(mtime < now + 86400);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_search_in_query_escape,
              "MarkdownService: search safely handles FTS5 operators", {
    const auto dir = makeTempDir("escape");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write("x.md", "# Title\n\nHas colon : and quotes \" hi").isOk());

    // 含 FTS5 操作符的查询不应崩溃
    auto r1 = svc.search("\"");   // 裸引号
    DOCTEST_CHECK(r1.isOk());
    auto r2 = svc.search("a:b:c"); // 冒号
    DOCTEST_CHECK(r2.isOk());
    auto r3 = svc.search("*");    // 星号
    DOCTEST_CHECK(r3.isOk());

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_path_posix_separator,
              "MarkdownService: relativePath uses POSIX separators", {
    const auto dir = makeTempDir("posix");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    auto createResult = svc.create("a/b/c/deep.md");
    DOCTEST_CHECK(createResult.isOk());
    if (createResult.isFail()) {
        std::cerr << "  create 失败: " << createResult.errorMessage() << std::endl;
    }
    DOCTEST_REQUIRE(createResult.isOk());

    auto list = svc.listAll().value();
    DOCTEST_REQUIRE_EQ(list.size(), 1u);

    const std::string p = list[0].relativePath.generic_string();
    // 必须用 '/'
    DOCTEST_CHECK(p.find('\\') == std::string::npos);
    DOCTEST_CHECK(p.find('/') != std::string::npos);
    DOCTEST_CHECK_EQ(p, std::string("a/b/c/deep.md"));

    // listInDirectory 只返回**直接子项**（非递归）：
    //   a/b       → 空（deep.md 的直接父目录是 a/b/c）
    //   a/b/c     → deep.md
    auto mid = svc.listInDirectory(fs::path("a/b"));
    DOCTEST_REQUIRE(mid.isOk());
    DOCTEST_CHECK_EQ(mid.value().size(), 0u);

    auto sub = svc.listInDirectory(fs::path("a/b/c"));
    DOCTEST_REQUIRE(sub.isOk());
    DOCTEST_CHECK_EQ(sub.value().size(), 1u);
    DOCTEST_CHECK_EQ(sub.value()[0].relativePath.generic_string(),
                     std::string("a/b/c/deep.md"));

    fs::remove_all(dir);
    return true;
})
// ==================== W8：文件变化感知 + 路径安全 ====================

REGISTER_TEST(md_is_safe_new_note_path,
              "MarkdownService: isSafeNewNotePath 拒绝逃逸/盘符/绝对路径", {
    using module::markdown::MarkdownService;
    DOCTEST_CHECK(MarkdownService::isSafeNewNotePath("笔记.md"));
    DOCTEST_CHECK(MarkdownService::isSafeNewNotePath("inbox/ideas.md"));
    DOCTEST_CHECK(MarkdownService::isSafeNewNotePath("a/b/c/deep.md"));
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath(""));            // 空
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("../etc/passwd.md")); // 父目录逃逸
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("a/../../x.md"));
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("C:/evil.md"));  // 盘符
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("c:x.md"));
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("/abs.md"));     // 绝对路径
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("\\\\srv\\share.md")); // UNC
    DOCTEST_CHECK(!MarkdownService::isSafeNewNotePath("a\\b.md"));     // 反斜杠
    return true;
})

REGISTER_TEST(md_create_rejects_unsafe_path,
              "MarkdownService: create 拒绝不合法路径（trust boundary 兜底）", {
    const auto dir = makeTempDir("unsafe_create");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_CHECK(svc.create("../escape.md").isFail());
    DOCTEST_CHECK(svc.create("C:/evil.md").isFail());
    DOCTEST_CHECK(svc.create("/abs.md").isFail());
    // 逃逸路径不应在 notesDir 之外创建任何文件
    DOCTEST_CHECK(!fs::exists(dir / ".." / "escape.md"));
    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_poll_changed_and_file_mtime,
              "MarkdownService: pollChanged/fileMtime 轮询底座", {
    const auto dir = makeTempDir("poll");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    writeTextFile(dir / "a.md", "# a\n");
    DOCTEST_REQUIRE(svc.rescan().isOk());

    // 无变化 → false
    auto c1 = svc.pollChanged();
    DOCTEST_REQUIRE(c1.isOk());
    DOCTEST_CHECK(!c1.value());

    // fileMtime：存在的文件返回非负；不存在返回 fail
    auto m = svc.fileMtime("a.md");
    DOCTEST_REQUIRE(m.isOk());
    DOCTEST_CHECK(m.value() >= 0);
    DOCTEST_CHECK(svc.fileMtime("nope.md").isFail());

    // 新文件 → true；rescan 后恢复 false
    writeTextFile(dir / "inbox" / "b.md", "# b\n");
    auto c2 = svc.pollChanged();
    DOCTEST_REQUIRE(c2.isOk());
    DOCTEST_CHECK(c2.value());
    DOCTEST_REQUIRE(svc.rescan().isOk());
    auto c3 = svc.pollChanged();
    DOCTEST_REQUIRE(c3.isOk());
    DOCTEST_CHECK(!c3.value());

    // 删除 → true
    fs::remove(dir / "a.md");
    auto c4 = svc.pollChanged();
    DOCTEST_REQUIRE(c4.isOk());
    DOCTEST_CHECK(c4.value());

    // 修改（内容变化改变 size）→ true
    DOCTEST_REQUIRE(svc.rescan().isOk());
    writeTextFile(dir / "inbox" / "b.md", "# b changed\n");
    auto c5 = svc.pollChanged();
    DOCTEST_REQUIRE(c5.isOk());
    DOCTEST_CHECK(c5.value());

    fs::remove_all(dir);
    return true;
})
