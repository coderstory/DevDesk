// tests/test_modules/test_notes_mcp_tools.cpp
// 需-7 notes_search / notes_read / notes_write MCP 三工具单测（F-01..F-05）。
// 夹具：独立 temp 笔记目录 + :memory: 库（test_markdown_service.cpp 同款；
// 建表只走 core::schema::kSql）。写路径红线用例（F-04）验证「新文件路由」：
// 已存在文件必须被拒——编辑器 400ms 自动保存持内存全文，会整个覆盖 MCP 写入。
// git 链用例（F-03b）走真 git 子进程（test_md_note_git.cpp 同款探针 + 轮询）。
#include "doctest.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/mcp_tool.h"
#include "core/schema.h"
#include "core/sqlite_db.h"
#include "module_markdown/markdown_service.h"
#include "module_markdown/md_note_git.h"
#include "module_markdown/notes_mcp_tools.h"
#include "platform/exec_capture.h"
#include "platform/json.h"
#include "test_support.h"

namespace fs = std::filesystem;
using module::markdown::MarkdownService;
using module::markdown::NoteGitAutoCommit;
namespace nm = module::markdown::mcp;

namespace {

// in-memory 库（test_markdown_service.cpp 同款，勿另写一份 DDL）
core::data::SqliteDb makeDb() {
    auto r = core::data::SqliteDb::open(":memory:");
    if (r.isFail()) std::abort();
    return std::move(r.value());
}

void applySchema(core::data::SqliteDb& db) {
    if (db.applySchema(core::schema::kSql).isFail()) std::abort();
}

// execute 便捷口：结果折成 (isError, text)；Result 层失败折成 (true, errorMessage)
bool runTool(core::mcp::IMcpTool& tool, const std::string& args, bool* isErrorOut,
             std::string* textOut) {
    auto r = tool.execute(args);
    if (r.isFail()) {
        *isErrorOut = true;
        *textOut = r.errorMessage();
        return true;
    }
    *isErrorOut = r.value().isError;
    *textOut = r.value().text;
    return true;
}

// 从局部注册表取工具执行（顺带覆盖注册路径）
bool runRegistered(core::mcp::McpToolRegistry& registry, const std::string& name,
                   const std::string& args, bool* isErrorOut, std::string* textOut) {
    auto found = registry.find(name);
    if (found.isFail()) {
        *isErrorOut = true;
        *textOut = "工具未注册: " + name;
        return false;
    }
    runTool(*found.value(), args, isErrorOut, textOut);
    return true;
}

// ---- git 辅助（test_md_note_git.cpp 同款口径） ----

int countGitCommits(const fs::path& repoDir) {
    const auto headLog = repoDir / ".git" / "logs" / "HEAD";
    std::error_code ec;
    if (!fs::exists(headLog, ec) || ec) return 0;
    std::ifstream in(headLog, std::ios::binary);
    if (!in) return 0;
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    return lines;
}

int runGit(const fs::path& dir, const std::vector<std::string>& args) {
    return platform::execCapture(dir.string(), args);
}

// git 子进程可用性探针（Windows 受控文件夹访问可致 git 静默非零退出）
bool gitSubprocessWorks() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    test_support::TempDir tmp("notes_mcp_git_probe");
    int rc = runGit(tmp.path, {"git", "init"});
    if (rc != 0) {
        cached = 0;
        return false;
    }
    rc = runGit(tmp.path, {"git", "-c", "user.name=T", "-c",
                           "user.email=t@t", "commit", "--allow-empty", "-m", "p"});
    cached = (rc == 0) ? 1 : 0;
    return cached == 1;
}

}  // namespace

// ========== 契约：三工具 schema / 名字 / 注册往返 ==========

REGISTER_TEST(notes_mcp_schema_contract,
              "notes_mcp: 三工具 schema 合法 JSON、名字/描述/必填字段正确", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "schema");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{3});

    struct Spec {
        const char* name;
        const char* required;
    };
    const Spec specs[] = {
        {nm::kNotesSearchToolName, "query"},
        {nm::kNotesReadToolName, "path"},
        {nm::kNotesWriteToolName, "topic"},
    };
    for (const auto& s : specs) {
        auto found = registry.find(s.name);
        DOCTEST_REQUIRE(found.isOk());
        DOCTEST_CHECK_FALSE(found.value()->description().empty());
        auto schema = platform::json::parse(found.value()->inputSchemaJson());
        DOCTEST_REQUIRE(schema.isOk());
        DOCTEST_REQUIRE(schema.value().isObject());
        const auto obj = schema.value().asObject();
        DOCTEST_CHECK_EQ(obj.get("type").asString(), std::string("object"));
        const auto required = obj.get("required");
        DOCTEST_REQUIRE(required.isArray());
        bool hasField = false;
        for (std::size_t i = 0; i < required.size(); ++i) {
            if (required[i].asString() == s.required) hasField = true;
        }
        DOCTEST_CHECK_MESSAGE(hasField,
                              std::string(s.name) + " required 缺 " + s.required);
        if (std::string(s.name) == nm::kNotesWriteToolName) {
            DOCTEST_CHECK_EQ(required.size(), std::size_t{2});  // topic + content
        }
    }
    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(notes_mcp_registry_round_trip,
              "notes_mcp: 注册→重名 fail→注销→再注销 no-op→再注册", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "reg");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{3});

    DOCTEST_CHECK_TRUE(nm::registerNotesMcpTools(registry, svc, nullptr).isFail());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{3});

    nm::unregisterNotesMcpTools(registry);
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{0});
    DOCTEST_CHECK_TRUE(registry.find(nm::kNotesSearchToolName).isFail());
    nm::unregisterNotesMcpTools(registry);  // no-op 不崩
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{0});

    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{3});
    fs::remove_all(dir);
    return true;
})

// ========== F-01 中文搜索（LIKE 兜底路径） ==========

REGISTER_TEST(notes_mcp_search_cjk_substring_hit,
              "notes_mcp: 中文子串搜索命中中文标题（unicode61 无法分词，唯一出路 = LIKE 兜底）", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "f01");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    // 标题是一整段连续 CJK：unicode61 视为单 token，子串「结论数据」不可能被
    // FTS MATCH 命中——命中即证明走的是 LIKE 字面子串路径
    DOCTEST_REQUIRE(svc.write("工作/深挖结论数据库选型.md",
                              "# 深挖结论数据库选型\n\n正文第一段。\n")
                        .isOk());
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    bool isError = true;
    std::string text;
    DOCTEST_REQUIRE(runRegistered(registry, nm::kNotesSearchToolName,
                                  "{\"query\":\"结论数据\"}", &isError, &text));
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_MESSAGE(text.find("深挖结论数据库选型") != std::string::npos,
                          "未命中中文标题: " + text);
    DOCTEST_CHECK_MESSAGE(text.find("工作/深挖结论数据库选型.md") != std::string::npos,
                          "结果未带相对路径（notes_read 需要它）: " + text);
    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(notes_mcp_search_miss_and_bad_args,
              "notes_mcp: 未命中→「未命中任何笔记」；缺参/空 query → isError", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "miss");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    bool isError = false;
    std::string text;
    DOCTEST_REQUIRE(runRegistered(registry, nm::kNotesSearchToolName,
                                  "{\"query\":\"绝不存在的词组xyz\"}", &isError,
                                  &text));
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_EQ(text, std::string("未命中任何笔记"));

    const char* badCases[] = {"not json", "{}", "{\"query\":\"\"}",
                              "{\"query\":42}"};
    for (const char* args : badCases) {
        isError = false;
        runRegistered(registry, nm::kNotesSearchToolName, args, &isError, &text);
        DOCTEST_CHECK_MESSAGE(isError,
                              std::string("参数 ") + args + " 应判错: " + text);
    }
    fs::remove_all(dir);
    return true;
})

// ========== F-02 读全文 ==========

REGISTER_TEST(notes_mcp_read_returns_full_text,
              "notes_mcp: notes_read 返回指定笔记全文（逐字节相等）", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "f02");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    const std::string content =
        "# 会议纪要\n\n第一段：路线图对齐。\n\n```cpp\nint x = 1;\n```\n\n尾段。";
    DOCTEST_REQUIRE(svc.write("会议/纪要.md", content).isOk());
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    bool isError = true;
    std::string text;
    DOCTEST_REQUIRE(runRegistered(registry, nm::kNotesReadToolName,
                                  "{\"path\":\"会议/纪要.md\"}", &isError, &text));
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_EQ(text, content);
    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(notes_mcp_read_bad_path_rejected,
              "notes_mcp: notes_read 缺参/不存在/越界路径 → isError", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "readbad");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    const char* badCases[] = {
        "not json",
        "{}",
        "{\"path\":\"不存在.md\"}",
        "{\"path\":\"../越界.md\"}",
    };
    bool isError = false;
    std::string text;
    for (const char* args : badCases) {
        isError = false;
        runRegistered(registry, nm::kNotesReadToolName, args, &isError, &text);
        DOCTEST_CHECK_MESSAGE(isError,
                              std::string("参数 ") + args + " 应判错: " + text);
    }
    fs::remove_all(dir);
    return true;
})

// ========== F-03a 写新文件 → FTS 立即可搜 ==========

REGISTER_TEST(notes_mcp_write_new_file_fts_synced,
              "notes_mcp: notes_write 落「来自Claude/日期-主题.md」新文件且 FTS 立即可搜", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "f03a");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    bool isError = true;
    std::string text;
    DOCTEST_REQUIRE(runRegistered(
        registry, nm::kNotesWriteToolName,
        "{\"topic\":\"会议纪要\",\"content\":\"# 会议纪要\\n\\n唯一令牌蓝鲸量子港。\"}",
        &isError, &text));
    DOCTEST_CHECK_FALSE(isError);

    // 路由红线：只进 来自Claude/ 独立目录，文件名 = 日期-主题
    const std::string expected =
        nm::notesMcpRelPath(nm::notesMcpTodayYmd(), "会议纪要");
    DOCTEST_CHECK_MESSAGE(text.find(expected) != std::string::npos,
                          "回执未含期望路径 " + expected + ": " + text);
    DOCTEST_CHECK_TRUE(fs::exists(dir / fs::path(expected)));

    auto disk = svc.read(expected);
    DOCTEST_REQUIRE(disk.isOk());
    DOCTEST_CHECK_EQ(disk.value(),
                     std::string("# 会议纪要\n\n唯一令牌蓝鲸量子港。"));

    // 写入即同步索引：立刻经工具可搜（新文件唯一令牌）
    isError = true;
    DOCTEST_REQUIRE(runRegistered(registry, nm::kNotesSearchToolName,
                                  "{\"query\":\"蓝鲸量子港\"}", &isError, &text));
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_MESSAGE(text.find(expected) != std::string::npos,
                          "FTS 未同步新文件: " + text);
    fs::remove_all(dir);
    return true;
})

// ========== F-04 已存在文件写请求被拒（红线用例） ==========

REGISTER_TEST(notes_mcp_write_rejects_existing_file,
              "notes_mcp: notes_write 对已存在文件拒绝并给明确错误，原文不动（防编辑器覆盖冲突）", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "f04");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    bool isError = true;
    std::string text;
    // 第一发：新文件成功
    DOCTEST_REQUIRE(runRegistered(
        registry, nm::kNotesWriteToolName,
        "{\"topic\":\"评审纪要\",\"content\":\"# 第一版\"}", &isError, &text));
    DOCTEST_CHECK_FALSE(isError);

    // 第二发同主题同日 = 同一路径 → 必须拒绝，且文案明确（已存在 + 拒绝覆盖）
    isError = false;
    DOCTEST_REQUIRE(runRegistered(
        registry, nm::kNotesWriteToolName,
        "{\"topic\":\"评审纪要\",\"content\":\"# 想覆盖的第二版\"}", &isError,
        &text));
    DOCTEST_CHECK_TRUE(isError);
    DOCTEST_CHECK_MESSAGE(text.find("已存在") != std::string::npos,
                          "文案缺「已存在」: " + text);
    DOCTEST_CHECK_MESSAGE(text.find("拒绝覆盖") != std::string::npos,
                          "文案缺「拒绝覆盖」: " + text);

    const std::string path =
        nm::notesMcpRelPath(nm::notesMcpTodayYmd(), "评审纪要");
    auto disk = svc.read(path);
    DOCTEST_REQUIRE(disk.isOk());
    DOCTEST_CHECK_EQ(disk.value(), std::string("# 第一版"));  // 原文未被改动

    // 外部预建的同路径文件同样被拒（非 MCP 自己写的也算「已有文件」）
    const std::string occupied =
        nm::notesMcpRelPath(nm::notesMcpTodayYmd(), "被占用主题");
    DOCTEST_REQUIRE(svc.write(occupied, "# 应用内已有\n").isOk());
    isError = false;
    DOCTEST_REQUIRE(runRegistered(
        registry, nm::kNotesWriteToolName,
        "{\"topic\":\"被占用主题\",\"content\":\"# 硬闯\"}", &isError, &text));
    DOCTEST_CHECK_TRUE(isError);
    DOCTEST_CHECK_MESSAGE(text.find("已存在") != std::string::npos,
                          "外部预建用例文案缺「已存在」: " + text);
    fs::remove_all(dir);
    return true;
})

REGISTER_TEST(notes_mcp_write_validates_topic,
              "notes_mcp: topic 空/含斜杠/含.. /超长 → isError 且不落盘", {
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "topic");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, nullptr).isOk());

    std::string longTopic(200, '长');
    const char* badCases[] = {
        "not json",
        "{}",
        "{\"topic\":\"\",\"content\":\"x\"}",
        "{\"topic\":\"a/b\",\"content\":\"x\"}",
        "{\"topic\":\"a\\\\b\",\"content\":\"x\"}",
        "{\"topic\":\"..\",\"content\":\"x\"}",
        "{\"content\":\"缺 topic\"}",
    };
    bool isError = false;
    std::string text;
    for (const char* args : badCases) {
        isError = false;
        runRegistered(registry, nm::kNotesWriteToolName, args, &isError, &text);
        DOCTEST_CHECK_MESSAGE(isError,
                              std::string("参数 ") + args + " 应判错: " + text);
    }
    isError = false;
    runRegistered(registry, nm::kNotesWriteToolName,
                  "{\"topic\":\"" + longTopic + "\",\"content\":\"x\"}",
                  &isError, &text);
    DOCTEST_CHECK_TRUE(isError);

    // 全部被拒：目录里除了被 svc 夹具自己建的库外无笔记落盘
    auto list = svc.listAll();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), std::size_t{0});
    fs::remove_all(dir);
    return true;
})

// ========== F-03b git 自动提交链 ==========

REGISTER_TEST(notes_mcp_write_triggers_git_autocommit,
              "notes_mcp: notes_write 后笔记 git 自动提交链产生一条 commit（git log 有记录）", {
    if (!gitSubprocessWorks()) {
        std::cout << "[SKIP] git 子进程不可用，F-03b 降级跳过" << std::endl;
        return true;
    }
    module::markdown::NoteGitAutoCommit::setEnabledForTest(true);
    const auto dir = test_support::makeTempDir("devdesk_notes_mcp_", "f03b");
    auto db = makeDb();
    applySchema(db);
    MarkdownService svc(db, dir);
    // shared 持有（生产同款：模块 reset 后在途工具不悬垂）
    auto gitSvc = std::make_shared<NoteGitAutoCommit>(svc.notesDir());
    gitSvc->start();

    // 有界等 git init 完成（.git 出现；init 失败 → disabled，本用例降级通过——
    // git 链本身是白拿的既有能力，不属本单交付面）
    bool inited = false;
    for (int i = 0; i < 100 && !inited; ++i) {
        std::error_code ec;
        inited = fs::exists(dir / ".git", ec) && !ec;
        if (!inited) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!inited || gitSvc->disabled()) {
        std::cout << "[SKIP] git init 未完成/被禁用" << std::endl;
        fs::remove_all(dir);
        return true;
    }

    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(nm::registerNotesMcpTools(registry, svc, gitSvc).isOk());
    bool isError = true;
    std::string text;
    DOCTEST_REQUIRE(runRegistered(
        registry, nm::kNotesWriteToolName,
        "{\"topic\":\"发版复盘\",\"content\":\"# 发版复盘\\n\\n结论：白拿 git 链\"}",
        &isError, &text));
    DOCTEST_CHECK_FALSE(isError);

    // 有界等后台 commit 落账（节流判定 first-commit 恒过；后台线程 + 子进程有延迟）
    bool committed = false;
    for (int i = 0; i < 200 && !committed; ++i) {
        committed = countGitCommits(dir) >= 1;
        if (!committed) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    DOCTEST_CHECK_MESSAGE(committed, "git log 未出现自动提交（10s 内）");
    fs::remove_all(dir);
    return true;
})
