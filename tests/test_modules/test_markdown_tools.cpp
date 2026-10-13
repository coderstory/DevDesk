// tests/test_modules/test_markdown_tools.cpp
// MarkdownTools 用例：只读（search / read / list_tags）、可逆写（create）、破坏性（delete），
// 参数校验、schema 抽查、注册注销、MarkdownModule 生命周期重入。
// 跨文件前提：
//   - 工具级失败（参数非法 / 目标不存在 / 越界 / 非法 UTF-8）一律 Result ok +
//     ToolResult.isError=true + content 含中文说明；Result::fail 只留给编程不变量破坏。
//   - ToolRegistry 是进程级单例，不在 test_main 的 resetCaseGlobals 清单里。清理只靠
//     ToolEnv 析构的 unregisterAll——断言失败 fail-fast 早退也会走析构，不会残留给后续用例。
//   - md.read / md.delete 的路径都必须先经 service resolve() 收口到笔记目录内。
//   - 非 UTF-8 内容经 platform::json::stringify 会放行（YYJSON_WRITE_ALLOW_INVALID_UNICODE），
//     而 parse 默认严格校验 UTF-8 会拒。工具回喂文本会被 AgentLoop 的 parseArrayOrDie
//     自检消费，非法 UTF-8 漏出去就是 LOG_FATAL + abort，整个应用连未保存数据一起死，
//     且 cron run 里无人值守。
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
#include <sstream>
#include <string>
#include <system_error>

namespace {

constexpr const char* kDirPrefix = "devdesk_test_mdtools_";

// 一次性环境：MdHeadlessEnv（临时笔记目录 + 已 migrate 库 + appDatabase + 环境隔离）+ 真实
// MarkdownService + MarkdownTools。
// 成员声明顺序即析构顺序，反过来即 use-after-free：~ToolEnv 体先 unregisterAll（注册表清干净）
// → tools 析构 → svc 析构 → env 析构（还原环境变量 + 删整棵目录）。
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

// 经 service.write 种 2 篇（磁盘 + 索引 + FTS 一步到位）：notes/a.md 含检索词 quokka，b.md 不含
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

REGISTER_TEST(md_tools_search_returns_hits,
              "md.search: 关键词命中 1 篇、无命中返回空 notes、空关键词报错", {
    ToolEnv env("search");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

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

    auto none = runTool("md.search", R"({"query":"no_such_token_xyz"})");
    DOCTEST_REQUIRE(none.isOk());
    DOCTEST_CHECK_FALSE(none.value().isError);
    auto noneParsed = platform::json::parse(none.value().contentJson);
    DOCTEST_REQUIRE(noneParsed.isOk());
    DOCTEST_CHECK_EQ(noneParsed.value().asObject().get("notes").size(), 0);

    auto empty = runTool("md.search", R"({"query":""})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().isError);
    DOCTEST_CHECK(empty.value().contentJson.find("关键词") !=
                  std::string::npos);
    return true;
})

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

// 非 UTF-8 笔记（GBK / Latin-1 / 二进制，中文用户极常见）必须由 md.read 在工具边界拒绝：
// 既不动盘上的原笔记，也不把乱码当正文回喂（乱码可能被 md.update 原样写回、彻底毁掉用户笔记）。
// 用户/模型收到的是可行动的显式失败，不是进程消失。
REGISTER_TEST(md_tools_read_non_utf8_note_is_error,
              "md.read: 非 UTF-8 笔记（GBK）→ isError 回喂，不触发 AgentLoop abort", {
    ToolEnv env("read_gbk");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    // 直接按字节落盘：GBK 的「中文」= D6 D0 CE C4（D6 是合法 UTF-8 前导，
    // 但 D0 不是合法续字节 → 严格校验必拒）。不经 svc.write（那是文本路径，
    // 且会把笔记写进索引——本用例只要磁盘上有这么一个文件）
    const std::filesystem::path gbk = env.env.dir / "notes_gbk.md";
    {
        std::ofstream out(gbk, std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(out.good());
        out << "GBK note: \xD6\xD0\xCE\xC4\n";
    }

    auto r = runTool("md.read", R"({"path":"notes_gbk.md"})");
    DOCTEST_REQUIRE(r.isOk());              // 工具级错误（不是 Result::fail）
    DOCTEST_CHECK(r.value().isError);
    DOCTEST_CHECK(r.value().contentJson.find("UTF-8") != std::string::npos);
    // 决定性断言：回喂文本必须能被严格 JSON 解析（非法 UTF-8 会被拒）——
    // 即「abort 路径从工具输出不可达」在工具边界成立
    DOCTEST_CHECK(platform::json::parse(r.value().contentJson).isOk());
    DOCTEST_CHECK(r.value().contentJson.find(std::string("\xD6\xD0", 2)) ==
                  std::string::npos);

    {
        std::ifstream in(gbk, std::ios::binary);
        std::ostringstream oss;
        oss << in.rdbuf();
        DOCTEST_CHECK_EQ(oss.str(), std::string("GBK note: \xD6\xD0\xCE\xC4\n"));
    }
    return true;
})

// md.read 是新开的 LLM 信任边界：读到的内容经 AgentLoop 回喂并送上游 LLM API，
// 路径逃逸 = 笔记目录外任意文件泄露。两种逃逸形态：
//   相对上跳 "../../x"；绝对路径 "/etc/passwd"（fs::path operator/ 对绝对
//   RHS 是整体替换，notesDir 前缀都不剩）。
// 相对上跳用「目录外真实存在」的文件做目标——"../../x" 目标不存在时靠
// 「读不到文件」碰巧不漏，本用例钉的是「拒绝原因 = 越界」而非运气。
REGISTER_TEST(md_tools_read_rejects_path_traversal,
              "md.read: 路径逃逸（相对上跳/绝对路径）→ isError，目录外内容不回喂", {
    ToolEnv env("traversal");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

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

    auto up = runTool("md.read", R"({"path":"../outside_secret.md"})");
    DOCTEST_REQUIRE(up.isOk());
    DOCTEST_CHECK(up.value().isError);
    DOCTEST_CHECK(up.value().contentJson.find("TOP_SECRET_OUTSIDE") ==
                  std::string::npos);

    // 绝对路径逃逸（macOS 上 /etc/passwd 存在）
    auto abs = runTool("md.read", R"({"path":"/etc/passwd"})");
    DOCTEST_REQUIRE(abs.isOk());
    DOCTEST_CHECK(abs.value().isError);
    DOCTEST_CHECK(abs.value().contentJson.find("root:") == std::string::npos);

    auto deep = runTool("md.read", R"({"path":"../../x"})");
    DOCTEST_REQUIRE(deep.isOk());
    DOCTEST_CHECK(deep.value().isError);
    return true;
})

REGISTER_TEST(md_tools_invalid_params_feed_error,
              "工具级错误：非法 JSON / 缺必填字段 → isError + 中文说明", {
    ToolEnv env("invalid");
    DOCTEST_REQUIRE(seedNotes(*env.svc));
    env.tools->registerAll();

    auto badJson = runTool("md.search", "not-json");
    DOCTEST_REQUIRE(badJson.isOk());
    DOCTEST_CHECK(badJson.value().isError);

    auto noQuery = runTool("md.search", "{}");
    DOCTEST_REQUIRE(noQuery.isOk());
    DOCTEST_CHECK(noQuery.value().isError);

    auto noPath = runTool("md.read", "{}");
    DOCTEST_REQUIRE(noPath.isOk());
    DOCTEST_CHECK(noPath.value().isError);
    DOCTEST_CHECK(noPath.value().contentJson.find("path") !=
                  std::string::npos);
    return true;
})

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
        // properties 键名抽查：宣传的入参键必须真实出现
        DOCTEST_CHECK(schema.find(std::string("\"") +
                                  (std::string(n) == "md.search" ? "query"
                                                                 : "path") +
                                  "\"") != std::string::npos);
        DOCTEST_CHECK(schema.find("\"required\"") != std::string::npos);
        DOCTEST_CHECK_FALSE(found.value()->isDestructive());  // 只读，不改磁盘
    }

    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.search").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("md.read").isFail());
    env.tools->unregisterAll();  // 重复注销 no-op
    return true;
})

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

// md.delete 的 isDestructive()=true 是安全边界：AgentLoop 在未授权时见到破坏性工具会直接
// 终止 run。md.create 已存在时拒绝覆盖，不静默改写用户笔记。
REGISTER_TEST(md_create_list_tags_delete_roundtrip,
              "md_tools: md.create 建笔记（拒绝覆盖）、md.list_tags 统计、md.delete 破坏性删除", {
    ToolEnv env("m2create");
    env.tools->registerAll();
    auto made = runTool("md.create", R"({"path":"notes/m2.md","content":"# M2\n正文"})");
    DOCTEST_REQUIRE(made.isOk());
    DOCTEST_CHECK_FALSE(made.value().isError);
    auto dup = runTool("md.create", R"({"path":"notes/m2.md","content":"再写"})");
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK(dup.value().isError);                      // 已存在 → isError 回喂
    auto readBack = runTool("md.read", R"({"path":"notes/m2.md"})");
    DOCTEST_REQUIRE(readBack.isOk());
    DOCTEST_CHECK(readBack.value().contentJson.find("正文") != std::string::npos);
    DOCTEST_REQUIRE(env.svc->setTags(std::filesystem::path("notes/m2.md"),
                                     {"alpha"}).isOk());
    auto tags = runTool("md.list_tags", "{}");
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_CHECK_FALSE(tags.value().isError);
    DOCTEST_CHECK(tags.value().contentJson.find("\"name\":\"alpha\"") != std::string::npos);
    // noteCount 真的按引用数出（alpha 只挂在 m2.md 一篇上 → 1）
    DOCTEST_CHECK(tags.value().contentJson.find("\"noteCount\":1") != std::string::npos);
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
