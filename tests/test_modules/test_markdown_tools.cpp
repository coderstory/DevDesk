// tests/test_modules/test_markdown_tools.cpp
// MarkdownTools 用例（agent M1 Task 9 起，M2 Task 7 扩三工具）：
//   md.search / md.read            只读；
//   md.create                      写但可逆（已存在拒绝覆盖）；
//   md.list_tags                   只读（tags + noteCount）；
//   md.delete                      **破坏性**（isDestructive 已覆写，越界路径拒绝）。
//
// 错误回喂约定（spec §10 M1 统一）：参数非法 / 目标不存在 → execute 返回
// Result ok + ToolResult.isError=true + content 含中文说明；Result::fail 只留给
// 编程不变量破坏。
//
// 注册表清理由 ToolEnv 析构做（RAII）：断言失败 fail-fast 早退也不会把工具
// 残留在进程级 ToolRegistry 里（Task 5 ToolReg 教训）。
#include "doctest.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "core/tool.h"
#include "module_markdown/markdown_service.h"
#include "module_markdown/markdown_tools.h"
#include "platform/json.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

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

// 评审修复（2026-10-01）：md.read 是新开的 LLM 信任边界——读到的内容经
// AgentLoop 回喂并送上游 LLM API，路径逃逸 = 笔记目录外任意文件泄露。
// 两种逃逸形态（service resolve() 收口前都能真实读出）：
//   相对上跳 "../../x"；绝对路径 "/etc/passwd"（fs::path operator/ 对绝对
//   RHS 是整体替换，notesDir 前缀都不剩）。
// 用「目录外真实存在」的文件做相对上跳目标——修复前 isError 断言红
//（内容被读出回喂），修复后绿；"../../x" 目标不存在，修复前靠
// 「读不到文件」碰巧不漏，此用例钉的是「拒绝原因 = 越界」而非运气。
REGISTER_TEST(md_tools_read_rejects_path_traversal,
              "md.read: 路径逃逸（相对上跳/绝对路径）→ isError，目录外内容不回喂", {
    ToolEnv env("traversal");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    // 笔记目录外种一个真实文件（临时根下，与笔记目录平级）
    const std::filesystem::path secret =
        env.env.dir.parent_path() / "outside_secret.md";
    DOCTEST_REQUIRE(test_support::writeTextFile(secret, "TOP_SECRET_OUTSIDE"));
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } cleanup{secret};  // RAII：fail-fast 早退也不留垃圾

    // 相对上跳 + 目标真实存在（修复前：读出 TOP_SECRET_OUTSIDE，isError 断言红）
    auto up = runTool("md.read", R"({"path":"../outside_secret.md"})");
    DOCTEST_REQUIRE(up.isOk());
    DOCTEST_CHECK(up.value().isError);
    DOCTEST_CHECK(up.value().contentJson.find("TOP_SECRET_OUTSIDE") ==
                  std::string::npos);

    // 绝对路径逃逸（macOS 上 /etc/passwd 存在，修复前能整读回喂）
    auto abs = runTool("md.read", R"({"path":"/etc/passwd"})");
    DOCTEST_REQUIRE(abs.isOk());
    DOCTEST_CHECK(abs.value().isError);
    DOCTEST_CHECK(abs.value().contentJson.find("root:") == std::string::npos);

    // 多级上跳形态：目标不存在也必须拒（拒绝理由是越界，不是文件缺失）
    auto deep = runTool("md.read", R"({"path":"../../x"})");
    DOCTEST_REQUIRE(deep.isOk());
    DOCTEST_CHECK(deep.value().isError);
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
        DOCTEST_CHECK(schema.find(std::string("\"name\":\"") + n + "\"") !=
                      std::string::npos);
        // properties 键名抽查（batch 裁定）：宣传的入参键必须真实出现
        DOCTEST_CHECK(schema.find(std::string("\"") +
                                  (std::string(n) == "md.search" ? "query"
                                                                 : "path") +
                                  "\"") != std::string::npos);
        DOCTEST_CHECK(schema.find("\"required\"") != std::string::npos);
        DOCTEST_CHECK_FALSE(found.value()->isDestructive());  // M1 只读
    }

    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.read").isFail());
    env.tools->unregisterAll();  // 重复注销 no-op
    return true;
})

// ============ MarkdownModule 生命周期重入（活门控 Task 4）============

extern "C" ::core::Module* create_markdown_module();

REGISTER_TEST(md_module_onstart_reentry_idempotent,
              "markdown_module: onStart→onStop→onStart 重入 + 双 onStart/双 onStop no-op", {
    test_support::MdHeadlessEnv env(kDirPrefix, "modreentry", "mdreentry.db");
    core::ToolRegistry::instance().unregisterTool("md.search");   // 起点干净
    core::ToolRegistry::instance().unregisterTool("md.read");
    std::unique_ptr<::core::Module> mod(create_markdown_module());

    mod->onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isOk());
    mod->onStart();   // 双 onStart：no-op（不重建 service_、不重复注册）
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isOk());
    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isFail());
    mod->onStop();    // 双 onStop：no-op
    mod->onStart();   // off→on 循环：工具恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isOk());
    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isFail());
    return true;
})

// ============ M2 Task 7：md.create / md.list_tags / md.delete ============

// 三工具一轮闭环（spec §5.3 / §7）：
//   md.create    写但可逆——已存在拒绝覆盖（不静默改写用户笔记）；
//   md.list_tags 只读——{tags:[{name,noteCount}]}；
//   md.delete    破坏性——isDestructive()=true（AgentLoop 未授权时直接终止
//                run）+ 越界路径经 service resolve containment 拒绝。
REGISTER_TEST(md_create_list_tags_delete_roundtrip,
              "md_tools: md.create 建笔记（拒绝覆盖）、md.list_tags 统计、md.delete 破坏性删除", {
    ToolEnv env("m2create");
    env.tools->registerAll();
    // create：成功 + 重复拒绝
    auto made = runTool("md.create", R"({"path":"notes/m2.md","content":"# M2\n正文"})");
    DOCTEST_REQUIRE(made.isOk());
    DOCTEST_CHECK_FALSE(made.value().isError);
    auto dup = runTool("md.create", R"({"path":"notes/m2.md","content":"再写"})");
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK(dup.value().isError);                      // 已存在 → isError 回喂
    auto readBack = runTool("md.read", R"({"path":"notes/m2.md"})");
    DOCTEST_REQUIRE(readBack.isOk());
    DOCTEST_CHECK(readBack.value().contentJson.find("正文") != std::string::npos);
    // list_tags：给 m2.md 打 tag 后统计（经 service setTags）
    DOCTEST_REQUIRE(env.svc->setTags(std::filesystem::path("notes/m2.md"),
                                     {"alpha"}).isOk());
    auto tags = runTool("md.list_tags", "{}");
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_CHECK_FALSE(tags.value().isError);
    DOCTEST_CHECK(tags.value().contentJson.find("\"name\":\"alpha\"") != std::string::npos);
    // noteCount 真的按引用数出（alpha 只挂在 m2.md 一篇上 → 1）
    DOCTEST_CHECK(tags.value().contentJson.find("\"noteCount\":1") != std::string::npos);
    // delete：破坏性标志 + 删除生效 + 越界路径拒绝
    auto delTool = core::ToolRegistry::instance().find("md.delete");
    DOCTEST_REQUIRE(delTool.isOk());
    DOCTEST_CHECK(delTool.value()->isDestructive());
    auto gone = runTool("md.delete", R"({"path":"notes/m2.md"})");
    DOCTEST_REQUIRE(gone.isOk());
    DOCTEST_CHECK_FALSE(gone.value().isError);
    auto after = env.svc->read(std::filesystem::path("notes/m2.md"));
    DOCTEST_CHECK(after.isFail());                           // 磁盘已删
    auto escape = runTool("md.delete", R"({"path":"../../etc/passwd"})");
    DOCTEST_REQUIRE(escape.isOk());
    DOCTEST_CHECK(escape.value().isError);                   // resolve 拒绝越界
    return true;
})
