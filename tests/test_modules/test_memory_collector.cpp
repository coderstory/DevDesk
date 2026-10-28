// tests/test_modules/test_memory_collector.cpp
// 采集器单测（S3-B）：四道过滤逐条钉 + 水位幂等（崩溃重放不重复入库）。
// 全部用临时目录夹具，不碰真实 ~/.claude。
#include "doctest.h"

#include "module_memory/collector/collector.h"
#include "module_memory/collector/session_reader.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace module::memory::collector;

namespace {

std::filesystem::path makeTempDir(const std::string& tag) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("memcpp-collector-" + tag + "-" +
                                 std::to_string(std::time(nullptr)) + "-" +
                                 std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& p, const std::string& content,
               bool append) {
    std::ofstream f(p, append ? std::ios::binary | std::ios::app
                              : std::ios::binary);
    f << content;
}

const std::string kUserLine =
    R"({"type":"user","sessionId":"sess-1","message":{"role":"user","content":"帮我看下构建报错"}})";
const std::string kAssistantLine =
    R"({"type":"assistant","sessionId":"sess-1","message":{"role":"assistant","content":[{"type":"text","text":"构建入口是 scripts/build-test-run.sh"}]}})";
const std::string kThinkingLine =
    R"({"type":"assistant","sessionId":"sess-1","message":{"role":"assistant","content":[{"type":"thinking","thinking":"内部推理不该入库"}]}})";
const std::string kToolOkLine =
    R"({"type":"user","sessionId":"sess-1","message":{"role":"user","content":[{"type":"tool_result","content":"long tool output"},{"tool_use_id":"x"}]}})";
const std::string kToolErrLine =
    R"({"type":"user","sessionId":"sess-1","message":{"role":"user","content":[{"type":"tool_result","content":"boom: exit 1","is_error":true}]}})";
const std::string kInjectedLine =
    R"({"type":"user","sessionId":"sess-1","message":{"role":"user","content":"<system-reminder>注入块不当记忆</system-reminder>"}})";

}  // namespace

REGISTER_TEST(memory_session_reader_four_filters,
              "memory: 会话行四道过滤——角色/注入块丢弃、错误工具留摘要、截断", {
    // ① 只取 user/assistant 正文；thinking 丢弃
    auto ev = parseSessionLine(kUserLine, 0);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_REQUIRE_EQ(ev.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(ev.value()[0].narrative);
    ev = parseSessionLine(kThinkingLine, 1);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_CHECK(ev.value().empty());
    // ① 工具结果：成功丢弃、错误留 200 字节摘要
    ev = parseSessionLine(kToolOkLine, 2);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_CHECK(ev.value().empty());
    ev = parseSessionLine(kToolErrLine, 3);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_REQUIRE_EQ(ev.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(ev.value()[0].text.find("[tool_error] boom") !=
                  std::string::npos);
    DOCTEST_CHECK(!ev.value()[0].narrative);  // 工具摘要不算正文
    // ② 注入块整条丢弃
    ev = parseSessionLine(kInjectedLine, 4);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_CHECK(ev.value().empty());
    // ③ 单事件截断到 4096 字节且不劈开 UTF-8 码点
    const std::string cjk = "记";  // 3 字节
    std::string big = cjk;
    while (big.size() < kMaxEventBytes + 100) big += cjk;
    const std::string bigLine =
        "{\"type\":\"user\",\"sessionId\":\"s\",\"message\":{\"role\":\"user\","
        "\"content\":\"" + big + "\"}}";
    ev = parseSessionLine(bigLine, 5);
    DOCTEST_REQUIRE(ev.isOk());
    DOCTEST_REQUIRE_EQ(ev.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(ev.value()[0].text.size() <= kMaxEventBytes);
    DOCTEST_CHECK_EQ(ev.value()[0].text.size() % 3,
                     static_cast<std::size_t>(0));  // 码点边界完整
    // 坏行：Error 但带行号（不中断整文件摄取）
    ev = parseSessionLine("{not json", 6);
    DOCTEST_CHECK(ev.isFail());
    return true;
})

REGISTER_TEST(memory_collector_watermark_idempotent,
              "memory: 采集水位幂等——同文件二次 poll 不出段；commit 后事件不重复", {
    const auto dir = makeTempDir("wm");
    const auto root = dir / "projects";
    std::filesystem::create_directories(root);
    const auto db = dir / "collector.db";
    writeFile(root / "sess-1.jsonl",
              kUserLine + "\n" + kAssistantLine + "\n", false);

    Collector c(db, root);
    auto segs = c.pollOnce();
    DOCTEST_REQUIRE(segs.isOk());
    DOCTEST_REQUIRE_EQ(segs.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(segs.value()[0].hasUserText);
    DOCTEST_CHECK_EQ(segs.value()[0].events.size(), static_cast<std::size_t>(2));

    // 未 commit 前重放：水位已推进但事件未记账——pollOnce 不再出段（水位续读）
    auto again = c.pollOnce();
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK(again.value().empty());

    // commit 记账后：同事件不重复（唯一键）
    DOCTEST_CHECK(c.commit(segs.value()).isOk());

    // 文件继续增长：只出新追加部分
    writeFile(root / "sess-1.jsonl", kAssistantLine + "\n", true);
    auto grown = c.pollOnce();
    DOCTEST_REQUIRE(grown.isOk());
    DOCTEST_REQUIRE_EQ(grown.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(grown.value()[0].events.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(c.commit(grown.value()).isOk());
    return true;
})

REGISTER_TEST(memory_collector_segment_skip_no_user_text,
              "memory: 段级跳过——整段无 user 正文（仅错误工具摘要）不出段", {
    const auto dir = makeTempDir("seg");
    const auto root = dir / "projects";
    std::filesystem::create_directories(root);
    writeFile(root / "sess-2.jsonl", kToolErrLine + "\n", false);

    Collector c(dir / "collector.db", root);
    auto segs = c.pollOnce();
    DOCTEST_REQUIRE(segs.isOk());
    DOCTEST_CHECK(segs.value().empty());  // ④ 无 user 正文 → 跳过
    return true;
})

REGISTER_TEST(memory_collector_root_resolution_and_truncation,
              "memory: 扫描根 env 覆盖优先；文件截断时水位归零重读", {
    // 纯函数解析：覆盖 → <home>/.claude/projects → 空
    DOCTEST_CHECK_EQ(
        Collector::resolveProjectsRoot("/custom/root", "/home/u").string(),
        std::string("/custom/root"));
    DOCTEST_CHECK_EQ(Collector::resolveProjectsRoot("", "/home/u").generic_string(),
                     std::string("/home/u/.claude/projects"));
    DOCTEST_CHECK(Collector::resolveProjectsRoot("", "").empty());

    // 截断检测：文件被缩短 → 水位归零，全量重出（事件唯一键兜底不重复入库）
    const auto dir = makeTempDir("trunc");
    const auto root = dir / "projects";
    std::filesystem::create_directories(root);
    const auto file = root / "sess-3.jsonl";
    writeFile(file, kUserLine + "\n", false);
    Collector c(dir / "collector.db", root);
    auto first = c.pollOnce();
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK_EQ(first.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(c.commit(first.value()).isOk());
    DOCTEST_CHECK(c.commit(first.value()).isOk());  // 幂等：重复 commit no-op

    // 截断重写为更长内容
    std::filesystem::resize_file(file, 0);
    writeFile(file, kUserLine + "\n" + kAssistantLine + "\n", false);
    auto reread = c.pollOnce();
    DOCTEST_REQUIRE(reread.isOk());
    DOCTEST_REQUIRE_EQ(reread.value().size(), static_cast<std::size_t>(1));
    // 已记账的 seq 0 事件被唯一键去重，只剩 seq 1
    DOCTEST_CHECK_EQ(reread.value()[0].events.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(reread.value()[0].events[0].eventSeq,
                     static_cast<std::uint64_t>(1));
    return true;
})
