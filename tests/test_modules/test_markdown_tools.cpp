// tests/test_modules/test_markdown_tools.cpp
// MarkdownTools M1 用例（agent M1 Task 9）：md.search / md.read 两个只读工具。
//
// 错误回喂约定（spec §10 M1 统一）：参数非法 / 目标不存在 → execute 返回
// Result ok + ToolResult.isError=true + content 含中文说明；Result::fail 只留给
// 编程不变量破坏。
//
// 注册表清理由 ToolEnv 析构做（RAII）：断言失败 fail-fast 早退也不会把工具
// 残留在进程级 ToolRegistry 里（Task 5 ToolReg 教训）。
#include "doctest.h"
#include "test_support.h"

#include "core/tool.h"
#include "module_markdown/markdown_service.h"
#include "module_markdown/markdown_tools.h"
#include "platform/json.h"

#include <memory>
#include <string>

namespace {

constexpr const char* kDirPrefix = "devdesk_test_mdtools_";

// 一次性环境：MdHeadlessEnv（临时笔记目录 + 已 migrate 库 + appDatabase +
// 环境隔离）+ 真实 MarkdownService + MarkdownTools。
// 析构顺序：~ToolEnv 体先 unregisterAll（注册表清干净）→ tools 析构 →
// svc 析构 → env 析构（还原环境变量 + 删整棵目录）。
struct ToolEnv {
    test_support::MdHeadlessEnv env;
    std::unique_ptr<module::markdown::MarkdownService> svc;
    std::unique_ptr<module::markdown::MarkdownTools> tools;

    explicit ToolEnv(const std::string& tag)
        : env(kDirPrefix, tag, "mdtools.db") {
        svc = std::make_unique<module::markdown::MarkdownService>(env.db->db(),
                                                                  env.dir);
        tools = std::make_unique<module::markdown::MarkdownTools>(*svc);
    }
    ~ToolEnv() { tools->unregisterAll(); }
};

// 从进程级注册表按名取工具并执行（走与 AgentLoop 相同的 find→execute 路径）
core::Result<core::ToolResult> runTool(const std::string& name,
                                       const std::string& argsJson) {
    auto found = core::ToolRegistry::instance().find(name);
    if (found.isFail()) {
        return core::Result<core::ToolResult>::fail(found.errorCode(),
                                                    found.errorMessage());
    }
    return found.value()->execute(core::ToolArgs{argsJson});
}

// 种 2 篇笔记（经 service write：磁盘 + 索引 + FTS 一步到位）；
// notes/a.md 含独特检索词 quokka，b.md 不含。
bool seedNotes(module::markdown::MarkdownService& svc) {
    if (svc.write("notes/a.md",
                  "# 笔记A\n\n这里包含独特关键词 quokka 的内容。")
            .isFail()) {
        return false;
    }
    if (svc.write("b.md", "# 另一篇\n\n完全无关的内容。").isFail()) {
        return false;
    }
    return true;
}

} // namespace

// ============ md.search ============

REGISTER_TEST(md_tools_search_returns_hits,
              "md.search: 关键词命中 1 篇、无命中返回空 notes、空关键词报错", {
    ToolEnv env("search");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    // 命中：只有 notes/a.md 含 quokka
    auto r = runTool("md.search", R"({"query":"quokka"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto notes = parsed.value().asObject().get("notes");
    DOCTEST_REQUIRE(notes.isArray());
    DOCTEST_CHECK_EQ(notes.size(), 1);
    DOCTEST_CHECK_EQ(notes[0].asObject().get("path").asString(),
                     std::string("notes/a.md"));
    DOCTEST_CHECK_EQ(notes[0].asObject().get("title").asString(),
                     std::string("笔记A"));

    // 无命中：不报错，空 notes
    auto none = runTool("md.search", R"({"query":"no_such_token_xyz"})");
    DOCTEST_REQUIRE(none.isOk());
    DOCTEST_CHECK_FALSE(none.value().isError);
    auto noneParsed = platform::json::parse(none.value().contentJson);
    DOCTEST_REQUIRE(noneParsed.isOk());
    DOCTEST_CHECK_EQ(noneParsed.value().asObject().get("notes").size(), 0);

    // 空关键词 → isError 回喂
    auto empty = runTool("md.search", R"({"query":""})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().isError);
    DOCTEST_CHECK(empty.value().contentJson.find("关键词") !=
                  std::string::npos);
    return true;
})

// ============ md.read ============

REGISTER_TEST(md_tools_read_returns_content,
              "md.read: 返回 {path,content} 且 content 是文件正文", {
    ToolEnv env("read");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    auto r = runTool("md.read", R"({"path":"notes/a.md"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("path").asString(), std::string("notes/a.md"));
    // content 含文件正文（标题行 + 正文关键词）
    const std::string content = obj.get("content").asString();
    DOCTEST_CHECK(content.find("# 笔记A") != std::string::npos);
    DOCTEST_CHECK(content.find("quokka") != std::string::npos);
    return true;
})

REGISTER_TEST(md_tools_read_missing_path_is_error,
              "md.read: 不存在的 path → Result ok + isError + 中文说明", {
    ToolEnv env("read_missing");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    auto r = runTool("md.read", R"({"path":"notes/ghost.md"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().isError);
    DOCTEST_CHECK(r.value().contentJson.find("读取") != std::string::npos);
    return true;
})

// ============ 工具级错误 ============

REGISTER_TEST(md_tools_invalid_params_feed_error,
              "工具级错误：非法 JSON / 缺必填字段 → isError + 中文说明", {
    ToolEnv env("invalid");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    // 非法 JSON
    auto badJson = runTool("md.search", "not-json");
    DOCTEST_REQUIRE(badJson.isOk());
    DOCTEST_CHECK(badJson.value().isError);

    // 缺必填字段 query
    auto noQuery = runTool("md.search", "{}");
    DOCTEST_REQUIRE(noQuery.isOk());
    DOCTEST_CHECK(noQuery.value().isError);

    // 缺必填字段 path
    auto noPath = runTool("md.read", "{}");
    DOCTEST_REQUIRE(noPath.isOk());
    DOCTEST_CHECK(noPath.value().isError);
    DOCTEST_CHECK(noPath.value().contentJson.find("path") !=
                  std::string::npos);
    return true;
})

// ============ schema 抽查 + 注册/注销 ============

REGISTER_TEST(md_tools_schema_spot_check_and_registry,
              "schema 抽查：两工具注册齐全、含 required、均非破坏性，注销后不可 find", {
    ToolEnv env("schema");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    for (const char* n : {"md.search", "md.read"}) {
        auto found = core::ToolRegistry::instance().find(n);
        DOCTEST_REQUIRE(found.isOk());
        const std::string schema = found.value()->schemaJson();
        DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);
        DOCTEST_CHECK(schema.find("\"required\"") != std::string::npos);
        DOCTEST_CHECK(schema.find(std::string("\"name\":\"") + n + "\"") !=
                      std::string::npos);
        DOCTEST_CHECK_FALSE(found.value()->isDestructive());  // M1 只读
    }

    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.read").isFail());
    env.tools->unregisterAll();  // 重复注销 no-op
    return true;
})
