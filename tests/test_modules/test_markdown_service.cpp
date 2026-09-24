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

// P3 watcher 接入后的确定性辅助：等待原生文件监听把此前磁盘操作的突发
// 通知送达（Win 去抖 200ms + 线程调度余量），再 rescan 兑现/消费 credit。
// stub 平台事件永不到达 → credit 恒 false → pollChanged 回到 mtime 比对，
// 断言不受影响。用于「无变化 → false」类断言前置。
void settleWatcher(module::markdown::MarkdownService& svc) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    (void)svc.rescan();  // 消费 credit + 以磁盘为准对齐（幂等）
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

// ============ rename（审计 [4][20] 配套服务层） ============

REGISTER_TEST(md_rename_moves_file_updates_index_and_keeps_tags,
              "MarkdownService: rename 移动文件+更新索引/FTS，标签（FK 挂 id）保留", {
    const auto dir = makeTempDir("rename_basic");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write("a.md", "# Alpha\n独特内容XYZ\n").isOk());
    DOCTEST_REQUIRE(svc.setTags("a.md", {"工作", "重要"}).isOk());

    auto r = svc.rename("a.md", "b.md");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("b.md"));

    // 磁盘：旧文件消失，新文件内容原样
    DOCTEST_CHECK_FALSE(fs::exists(dir / "a.md"));
    DOCTEST_CHECK(fs::exists(dir / "b.md"));
    auto content = svc.read("b.md");
    DOCTEST_REQUIRE(content.isOk());
    DOCTEST_CHECK_EQ(content.value(), std::string("# Alpha\n独特内容XYZ\n"));

    // 索引：只剩 b.md，title 重算仍为 Alpha
    auto notes = svc.listAll();
    DOCTEST_REQUIRE(notes.isOk());
    DOCTEST_CHECK_EQ(notes.value().size(), 1u);
    DOCTEST_CHECK_EQ(notes.value()[0].relativePath.generic_string(), std::string("b.md"));
    DOCTEST_CHECK_EQ(notes.value()[0].title, std::string("Alpha"));

    // 标签随 id 保留（rename 不删索引行，markdown_note_tags FK 不受影响）
    auto tags = svc.tagsForNote("b.md");
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_CHECK_EQ(tags.value().size(), 2u);

    // FTS 迁移：旧 path 不再命中，新 path 命中内容
    auto hits = svc.search("独特内容XYZ");
    DOCTEST_REQUIRE(hits.isOk());
    DOCTEST_REQUIRE_EQ(hits.value().size(), 1u);
    DOCTEST_CHECK_EQ(hits.value()[0].relativePath.generic_string(), std::string("b.md"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_rename_normalizes_and_rejects,
              "MarkdownService: rename 补 .md/子目录移动/目标冲突/源缺失/不安全名拒绝", {
    const auto dir = makeTempDir("rename_reject");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write("a.md", "# A\n").isOk());
    DOCTEST_REQUIRE(svc.write("taken.md", "# T\n").isOk());

    // 无扩展名自动补 .md
    auto ext = svc.rename("a.md", "c");
    DOCTEST_REQUIRE(ext.isOk());
    DOCTEST_CHECK_EQ(ext.value(), std::string("c.md"));

    // 子目录 = 移动语义（中间目录自动创建）
    auto move = svc.rename("c.md", "work/d");
    DOCTEST_REQUIRE(move.isOk());
    DOCTEST_CHECK_EQ(move.value(), std::string("work/d.md"));
    DOCTEST_CHECK(fs::exists(dir / "work" / "d.md"));

    // 目标已存在 → 拒绝，原文件不动
    auto clash = svc.rename("work/d.md", "taken.md");
    DOCTEST_CHECK(clash.isFail());
    DOCTEST_CHECK(fs::exists(dir / "work" / "d.md"));
    DOCTEST_CHECK_EQ(svc.listAll().value().size(), 2u);

    // 源不存在 → 拒绝
    DOCTEST_CHECK(svc.rename("ghost.md", "x.md").isFail());
    // 不安全新名（绝对路径/盘符/../）→ 拒绝
    DOCTEST_CHECK(svc.rename("taken.md", "../escape").isFail());
    DOCTEST_CHECK(svc.rename("taken.md", "C:/evil").isFail());
    // 原地改名 → 幂等成功
    auto noop = svc.rename("taken.md", "taken.md");
    DOCTEST_REQUIRE(noop.isOk());
    DOCTEST_CHECK_EQ(noop.value(), std::string("taken.md"));
    // taken → taken.md 显式同路径也算无操作
    DOCTEST_CHECK(svc.rename("taken.md", "taken").isOk());

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

// 含被丢弃符号的词元（c++ / std::vector / #tag 等）：FTS 清洗会把
// "c++" 变 "c"* 前缀误命中所有 c 开头词（clean/crisp…）。修复后：
// 全失真查询跳过 FTS 只走 LIKE 字面子串；混合失真查询 LIKE 兜底补命中
REGISTER_TEST(md_search_symbol_tokens,
              "MarkdownService: 符号词元（c++）不前缀误命中且能搜到", {
    const auto dir = makeTempDir("search_symbol");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.write("cpp.md",
                              "# Cpp\n\nc++ template specialization notes").isOk());
    DOCTEST_REQUIRE(svc.write("clean.md",
                              "# Clean\n\nclean code and crisp content").isOk());

    // 搜 "c++"：只命中含字面 c++ 的笔记（clean/crisp 不再被 c* 前缀误中）
    auto r1 = svc.search("c++");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE_EQ(r1.value().size(), 1u);
    DOCTEST_CHECK_EQ(r1.value()[0].relativePath.generic_string(),
                     std::string("cpp.md"));

    // 混合查询（失真词元 + 干净词元）：FTS 保 AND 语义，LIKE 兜底补字面命中
    auto r2 = svc.search("c++ template");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE_EQ(r2.value().size(), 1u);
    DOCTEST_CHECK_EQ(r2.value()[0].relativePath.generic_string(),
                     std::string("cpp.md"));

    // 纯干净词元查询行为不变（FTS 前缀命中）
    auto r3 = svc.search("crisp");
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_REQUIRE_EQ(r3.value().size(), 1u);
    DOCTEST_CHECK_EQ(r3.value()[0].relativePath.generic_string(),
                     std::string("clean.md"));

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

REGISTER_TEST(md_create_subpath_makes_dirs,
              "MarkdownService: create 带子路径时自动建中间目录（W9）", {
    const auto dir = makeTempDir("subpath_create");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    // 「工作/周会.md」形式的输入（无 .md 后缀时 create 内部补全）：
    // 中间目录不存在 → create_directories 建出
    DOCTEST_REQUIRE(svc.create("工作/周会").isOk());
    DOCTEST_CHECK(fs::is_regular_file(dir / "工作" / "周会.md"));
    DOCTEST_CHECK(fs::is_directory(dir / "工作"));

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_create_folder_and_list_top,
              "MarkdownService: createFolder + listTopFolders（W9 新建文件夹入口）", {
    const auto dir = makeTempDir("folder");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    // 空目录 → 无一级子目录
    auto empty = svc.listTopFolders();
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK_EQ(empty.value().size(), static_cast<size_t>(0));

    // 新建文件夹（多级也支持）→ listTopFolders 枚举出一级目录名
    DOCTEST_REQUIRE(svc.createFolder("工作").isOk());
    DOCTEST_REQUIRE(svc.createFolder("归档/2026").isOk());
    auto folders = svc.listTopFolders();
    DOCTEST_REQUIRE(folders.isOk());
    DOCTEST_CHECK_EQ(folders.value().size(), static_cast<size_t>(2));
    // std::sort 按 UTF-8 字节序：E5B7(工) < E5BD(归)
    DOCTEST_CHECK_EQ(folders.value()[0], std::string("工作"));
    DOCTEST_CHECK_EQ(folders.value()[1], std::string("归档"));
    DOCTEST_CHECK(fs::is_directory(dir / "工作"));
    DOCTEST_CHECK(fs::is_directory(dir / "归档" / "2026"));

    // 幂等：已存在不算错；不安全路径拒绝
    DOCTEST_CHECK(svc.createFolder("工作").isOk());
    DOCTEST_CHECK(svc.createFolder("../escape").isFail());
    DOCTEST_CHECK(svc.createFolder("C:/evil").isFail());
    DOCTEST_CHECK(svc.createFolder("/abs").isFail());
    DOCTEST_CHECK(!fs::exists(dir / ".." / "escape"));

    // 有 .md 文件的目录也在枚举里（与笔记路径推导的目录一致）
    DOCTEST_REQUIRE(svc.create("inbox/idea.md").isOk());
    auto mixed = svc.listTopFolders();
    DOCTEST_REQUIRE(mixed.isOk());
    DOCTEST_CHECK_EQ(mixed.value().size(), static_cast<size_t>(3));

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
    settleWatcher(svc);  // P3：等 watcher 事件送达并消费 credit，恢复「无变化=false」稳态

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
    settleWatcher(svc);  // P3：同 c1，消费 b.md 的 watcher 事件 credit
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

// ==================== P3：FileWatcher 接线（#5 根治）====================

REGISTER_TEST(md_service_watcher_external_new_file,
              "MarkdownService: 外部新建文件 → watcher 即时入索引（不显式 rescan）+ pollChanged credit", {
    const auto dir = makeTempDir("fwatch_new");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);
    if (!svc.watcherActive()) {  // POSIX/macOS stub：事件链路不存在，跳过
        std::printf("[SKIP] FileWatcher 非活跃平台（stub），事件链路用例跳过\n");
        fs::remove_all(dir);
        return true;
    }

    writeTextFile(dir / "ext_new.md", "# 外部新建\n\nuniquetoken73f9 body");
    // 不显式 rescan：watcher 线程 force-sync 应让搜索在几秒内命中
    bool hit = false;
    for (int i = 0; i < 50 && !hit; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto sr = svc.search("uniquetoken73f9"); sr.isOk() && !sr.value().empty()) {
            hit = true;
        }
    }
    DOCTEST_CHECK(hit);
    // credit：事件到达后置位且无人消费 → pollChanged 必须 true（哪怕
    // watcher 线程已把索引同步好、mtime 比对已无差异——UI 仍需刷新一轮）
    auto c = svc.pollChanged();
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK(c.value());

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_service_watcher_same_len_external_edit,
              "MarkdownService: 同秒等长外部改写 → watcher 事件路径看见（mtime/size 盲区，#5 根治点）", {
    const auto dir = makeTempDir("fwatch_eq");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);
    if (!svc.watcherActive()) {
        std::printf("[SKIP] FileWatcher 非活跃平台（stub），等长改写用例跳过\n");
        fs::remove_all(dir);
        return true;
    }

    // 等长两条内容（仅 token 不同）→ (mtime,size) 元组理论上不变
    const std::string bodyA = "# eq\n\naaabbbccc731 body padding xx";
    const std::string bodyB = "# eq\n\nzzzxywwvv732 body padding xx";
    DOCTEST_REQUIRE_EQ(bodyA.size(), bodyB.size());

    writeTextFile(dir / "eq.md", bodyA);
    DOCTEST_REQUIRE(svc.rescan().isOk());

    // 外部等长改写；全程不再 rescan——搜索命中只可能来自 watcher 事件 force-sync
    writeTextFile(dir / "eq.md", bodyB);
    bool hit = false;
    for (int i = 0; i < 50 && !hit; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto sr = svc.search("zzzxywwvv732"); sr.isOk() && !sr.value().empty()) {
            hit = true;
        }
    }
    DOCTEST_CHECK(hit);

    fs::remove_all(dir);
    return true;
})

// ==================== 标签（spec §8.1：setTags / listAllTags / notesWithTag） ====================

REGISTER_TEST(md_tags_set_roundtrip,
              "MarkdownService: setTags 全量替换往返 + 清洗去重", {
    const auto dir = makeTempDir("tags_rw");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("a.md").isOk());
    DOCTEST_CHECK(svc.setTags("a.md", {"工作", "idea"}).isOk());
    auto t1 = svc.tagsForNote("a.md");
    DOCTEST_REQUIRE(t1.isOk());
    DOCTEST_REQUIRE_EQ(t1.value().size(), 2u);
    DOCTEST_CHECK_EQ(t1.value()[0], std::string("idea"));   // 按名字母序
    DOCTEST_CHECK_EQ(t1.value()[1], std::string("工作"));

    // 全量替换：旧标签消失
    DOCTEST_CHECK(svc.setTags("a.md", {"工作"}).isOk());
    auto t2 = svc.tagsForNote("a.md");
    DOCTEST_REQUIRE(t2.isOk());
    DOCTEST_REQUIRE_EQ(t2.value().size(), 1u);
    DOCTEST_CHECK_EQ(t2.value()[0], std::string("工作"));

    // 清洗：空白标签跳过、首尾空白去除、重复去重
    DOCTEST_CHECK(svc.setTags("a.md", {" x ", "", "x", "y"}).isOk());
    auto t3 = svc.tagsForNote("a.md");
    DOCTEST_REQUIRE(t3.isOk());
    DOCTEST_REQUIRE_EQ(t3.value().size(), 2u);
    DOCTEST_CHECK_EQ(t3.value()[0], std::string("x"));
    DOCTEST_CHECK_EQ(t3.value()[1], std::string("y"));

    // 清空 = 移除全部标签
    DOCTEST_CHECK(svc.setTags("a.md", {}).isOk());
    auto t4 = svc.tagsForNote("a.md");
    DOCTEST_REQUIRE(t4.isOk());
    DOCTEST_CHECK_EQ(t4.value().size(), 0u);

    // 未索引的笔记 → fail（标签挂在 markdown_index.id 上）
    DOCTEST_CHECK(svc.setTags("ghost.md", {"x"}).isFail());

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_tags_list_all_counts,
              "MarkdownService: listAllTags 计数 + 孤儿标签清理", {
    const auto dir = makeTempDir("tags_list");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("a.md").isOk());
    DOCTEST_REQUIRE(svc.create("b.md").isOk());
    DOCTEST_REQUIRE(svc.create("c.md").isOk());
    DOCTEST_REQUIRE(svc.setTags("a.md", {"工作", "急"}).isOk());
    DOCTEST_REQUIRE(svc.setTags("b.md", {"工作"}).isOk());
    DOCTEST_REQUIRE(svc.setTags("c.md", {"工作"}).isOk());

    auto tags = svc.listAllTags();
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_REQUIRE_EQ(tags.value().size(), 2u);
    // UTF-8 字节序：工(E5) < 急(E6) → 「工作」在前
    DOCTEST_CHECK_EQ(tags.value()[0].name, std::string("工作"));
    DOCTEST_CHECK_EQ(tags.value()[0].noteCount, 3);
    DOCTEST_CHECK_EQ(tags.value()[1].name, std::string("急"));
    DOCTEST_CHECK_EQ(tags.value()[1].noteCount, 1);

    // b 不再用「工作」→ 计数降为 2；「急」升到 2
    DOCTEST_REQUIRE(svc.setTags("b.md", {"急"}).isOk());
    auto tags2 = svc.listAllTags();
    DOCTEST_REQUIRE(tags2.isOk());
    DOCTEST_REQUIRE_EQ(tags2.value().size(), 2u);
    DOCTEST_CHECK_EQ(tags2.value()[0].name, std::string("工作"));
    DOCTEST_CHECK_EQ(tags2.value()[0].noteCount, 2);
    DOCTEST_CHECK_EQ(tags2.value()[1].name, std::string("急"));
    DOCTEST_CHECK_EQ(tags2.value()[1].noteCount, 2);

    // 所有笔记移除某标签 → 该标签（孤儿）从 listAllTags 消失
    DOCTEST_REQUIRE(svc.setTags("a.md", {}).isOk());
    DOCTEST_REQUIRE(svc.setTags("b.md", {}).isOk());
    auto tags3 = svc.listAllTags();
    DOCTEST_REQUIRE(tags3.isOk());
    DOCTEST_REQUIRE_EQ(tags3.value().size(), 1u);
    DOCTEST_CHECK_EQ(tags3.value()[0].name, std::string("工作"));
    DOCTEST_CHECK_EQ(tags3.value()[0].noteCount, 1);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_tags_filter_notes,
              "MarkdownService: notesWithTag 按标签过滤笔记", {
    const auto dir = makeTempDir("tags_filter");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("a.md").isOk());
    DOCTEST_REQUIRE(svc.create("b.md").isOk());
    DOCTEST_REQUIRE(svc.create("c.md").isOk());
    DOCTEST_REQUIRE(svc.setTags("a.md", {"工作"}).isOk());
    DOCTEST_REQUIRE(svc.setTags("b.md", {"工作", "急"}).isOk());

    auto hit = svc.notesWithTag("工作");
    DOCTEST_REQUIRE(hit.isOk());
    DOCTEST_REQUIRE_EQ(hit.value().size(), 2u);
    std::set<std::string> paths;
    for (const auto& n : hit.value()) {
        paths.insert(n.relativePath.generic_string());
    }
    DOCTEST_CHECK(paths.count("a.md") == 1);
    DOCTEST_CHECK(paths.count("b.md") == 1);

    // 无命中标签 → 空结果（不报错）
    auto none = svc.notesWithTag("不存在");
    DOCTEST_REQUIRE(none.isOk());
    DOCTEST_CHECK_EQ(none.value().size(), 0u);

    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(md_tags_removed_note_cleans_associations,
              "MarkdownService: 删除笔记清理标签关联（cascade + 孤儿清理）", {
    const auto dir = makeTempDir("tags_del");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    DOCTEST_REQUIRE(svc.create("a.md").isOk());
    DOCTEST_REQUIRE(svc.create("b.md").isOk());
    DOCTEST_REQUIRE(svc.setTags("a.md", {"独占", "共享"}).isOk());
    DOCTEST_REQUIRE(svc.setTags("b.md", {"共享"}).isOk());

    // 删除 a：独占标签消失，共享标签计数降 1
    DOCTEST_REQUIRE(svc.remove("a.md").isOk());
    auto tags = svc.listAllTags();
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_REQUIRE_EQ(tags.value().size(), 1u);
    DOCTEST_CHECK_EQ(tags.value()[0].name, std::string("共享"));
    DOCTEST_CHECK_EQ(tags.value()[0].noteCount, 1);

    // note_tags 表里 a 的关联行已清（FK cascade）
    {
        auto prep = db.prepare(
            "SELECT COUNT(*) AS n FROM markdown_note_tags nt "
            "JOIN markdown_index i ON i.id = nt.note_id "
            "WHERE i.path = 'a.md'");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_CHECK_EQ(rows.value()[0].getInt64("n").value(), 0);
    }

    // 删除最后一个使用者 → 标签表彻底干净
    DOCTEST_REQUIRE(svc.remove("b.md").isOk());
    auto tags2 = svc.listAllTags();
    DOCTEST_REQUIRE(tags2.isOk());
    DOCTEST_CHECK_EQ(tags2.value().size(), 0u);
    {
        auto prep = db.prepare("SELECT COUNT(*) AS n FROM markdown_tags");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_CHECK_EQ(rows.value()[0].getInt64("n").value(), 0);
    }

    fs::remove_all(dir);
    return true;
})

// ============ 审计 2026-09-23 [2]：笔记保存原子写 ============

// write/create 改 tmp+rename 后验证三件事：
//   1. 成功路径不留 *.devdesk-tmp 残骸（rename 完成即消失）
//   2. 覆盖已存在的非空文件正常工作（remove+rename 路径，MinGW rename
//      不覆盖语义正是先删再替换的原因）
//   3. create()（旧实现写失败连检查都没有）同样原子化且内容完整
REGISTER_TEST(md_write_atomic_no_tmp_leftover,
              "MarkdownService: write/create 原子写不留 tmp 残骸", {
    const auto dir = makeTempDir("atomic_write");
    auto db = makeDb();
    runMigrations(db);
    module::markdown::MarkdownService svc(db, dir);

    // 深一层目录 + 较大内容（走 create_directories + tmp+rename 全路径）
    const std::string big = "# Deep\n\n" + std::string(200000, 'x');
    DOCTEST_REQUIRE(svc.write(fs::path("notes") / "deep.md", big).isOk());

    // 覆盖写（目标已存在 → 先 remove 再 rename）
    DOCTEST_REQUIRE(svc.write(fs::path("notes") / "deep.md", std::string("# Short")).isOk());
    auto rd = svc.read(fs::path("notes") / "deep.md");
    DOCTEST_REQUIRE(rd.isOk());
    DOCTEST_CHECK_EQ(rd.value(), std::string("# Short"));

    // create 同款原子写（补 .md 后缀 + 默认模板）
    DOCTEST_REQUIRE(svc.create(fs::path("brand-new")).isOk());
    auto rd2 = svc.read(fs::path("brand-new.md"));
    DOCTEST_REQUIRE(rd2.isOk());
    DOCTEST_CHECK(rd2.value().rfind("# brand-new", 0) == 0);

    // 全目录扫描：不允许存在任何 .devdesk-tmp 残留
    int tmpCount = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->path().extension() == ".devdesk-tmp") ++tmpCount;
    }
    DOCTEST_CHECK_EQ(tmpCount, 0);
    fs::remove_all(dir, ec);
    return true;
})
