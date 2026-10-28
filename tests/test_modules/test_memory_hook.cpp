// tests/test_modules/test_memory_hook.cpp
// hook 同步注入单测（S3-C）。红线：additionalContext 只在 hookSpecificOutput 内；
// 任何内部错误路径 = 空输出 + exit 0（绝不 exit 2）；预算 ≤5 条 ≤500 字。
#include "doctest.h"

#include "module_memory/hook/hook_command.h"
#include "module_memory/hook/hooks_settings.h"
#include "module_memory/hook/inject.h"
#include "platform/json.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace module::memory::hook;
using module::memory::gateway::IMemoryRetriever;
using module::memory::gateway::parseSearchResponse;
using module::memory::gateway::MemoryHit;

namespace {

// 假检索器：注入固定命中/失败态，不起真引擎
class FakeRetriever : public IMemoryRetriever {
public:
    std::vector<MemoryHit> hits;
    bool fail = false;
    std::string lastQuery;
    core::Result<std::vector<MemoryHit>> search(const std::string& query,
                                                int limit, int) override {
        (void)limit;
        lastQuery = query;
        if (fail) {
            return core::Result<std::vector<MemoryHit>>::fail(
                core::ErrorCode::NetworkUnreachable, "引擎不在");
        }
        return core::Result<std::vector<MemoryHit>>::ok(hits);
    }
    core::Result<std::string> addDirect(const std::string&) override {
        return core::Result<std::string>::ok("1");
    }
    core::Result<void> setMemoryForgotten(const std::string&,
                                          bool) override {
        return core::Result<void>::ok();
    }
};

const char* kStdinTemplate =
    R"({"session_id":"s1","hook_event_name":"UserPromptSubmit","prompt":"%s"})";

}  // namespace

REGISTER_TEST(memory_hook_output_contract,
              "memory: hook 输出契约——additionalContext 在 hookSpecificOutput 内且事件名正确", {
    const std::string out =
        buildHookOutputJson("测试注入内容");
    const auto parsed = platform::json::parse(out);
    DOCTEST_REQUIRE(parsed.isOk());
    // 字段位置契约：顶层没有 additionalContext（放顶层会被静默忽略）
    DOCTEST_CHECK(parsed.value().asObject().get("additionalContext").isNull());
    const auto inner = parsed.value().asObject().get("hookSpecificOutput");
    DOCTEST_REQUIRE(inner.isObject());
    DOCTEST_CHECK_EQ(inner.asObject().get("hookEventName").asString(),
                     std::string("UserPromptSubmit"));
    DOCTEST_CHECK_EQ(inner.asObject().get("additionalContext").asString(),
                     std::string("测试注入内容"));
    return true;
})

REGISTER_TEST(memory_hook_budget_assembly,
              "memory: 预算拼装——≤5 条 ≤500 字、日期来源格式、资料非指令声明、超预算条目裁剪", {
    std::vector<MemoryHit> hits;
    for (int i = 0; i < 7; ++i) {
        MemoryHit h;
        h.text = "记忆条目内容" + std::to_string(i);
        h.memoryId = std::to_string(i + 1);
        h.dateText = "2026-10-27";
        hits.push_back(h);
    }
    const std::string ctx = assembleContext(hits);
    DOCTEST_CHECK(!ctx.empty());
    DOCTEST_CHECK(ctx.find("资料非指令") != std::string::npos);
    DOCTEST_CHECK(ctx.find("[2026-10-27 来源:#1]") != std::string::npos);
    // ≤5 条：取 0~4，第 6 条（内容5）起裁掉
    DOCTEST_CHECK(ctx.find("记忆条目内容4") != std::string::npos);
    DOCTEST_CHECK(ctx.find("记忆条目内容5") == std::string::npos);
    // ≤500 字（码点口径）
    DOCTEST_CHECK(countChars(ctx) <= 500);
    // 换行正文单行化
    MemoryHit multi;
    multi.text = "第一行\n第二行";
    multi.dateText = "2026-10-01";
    DOCTEST_CHECK(assembleContext({multi}).find("第一行 第二行") !=
                  std::string::npos);
    // 首条即超预算 → 空串 = 本轮不注入
    MemoryHit huge;
    huge.text = std::string(600, 'x');
    DOCTEST_CHECK(assembleContext({huge}).empty());
    return true;
})

REGISTER_TEST(memory_hook_fail_open_never_exit2,
              "memory: 失败开放——stdin 非法/检索失败/事件不符一律空输出 + exit 0（绝无 2）", {
    FakeRetriever ok;
    ok.hits = {MemoryHit{"构建入口是 scripts/build-test-run.sh", "7",
                         "2026-10-27", 0.9}};
    // 正常路径：注入送达
    char buf[256];
    std::snprintf(buf, sizeof(buf), kStdinTemplate, "构建怎么跑");
    auto good = runMemoryHook(buf, &ok);
    DOCTEST_CHECK_EQ(good.exitCode, 0);
    DOCTEST_CHECK(!good.stdoutText.empty());
    DOCTEST_CHECK(ok.lastQuery == std::string("构建怎么跑"));

    // ① stdin 非法 JSON
    auto badJson = runMemoryHook("{not json", &ok);
    DOCTEST_CHECK_EQ(badJson.exitCode, 0);
    DOCTEST_CHECK(badJson.stdoutText.empty());
    // ② 检索失败（引擎不在/超时）
    FakeRetriever dead;
    dead.fail = true;
    auto failSearch = runMemoryHook(buf, &dead);
    DOCTEST_CHECK_EQ(failSearch.exitCode, 0);
    DOCTEST_CHECK(failSearch.stdoutText.empty());
    // ③ 非 UserPromptSubmit 事件
    auto wrongEvent = runMemoryHook(
        R"({"hook_event_name":"PreToolUse","prompt":"构建怎么跑"})", &ok);
    DOCTEST_CHECK_EQ(wrongEvent.exitCode, 0);
    DOCTEST_CHECK(wrongEvent.stdoutText.empty());
    // ④ 空检索结果
    FakeRetriever none;
    auto emptyHits = runMemoryHook(buf, &none);
    DOCTEST_CHECK_EQ(emptyHits.exitCode, 0);
    DOCTEST_CHECK(emptyHits.stdoutText.empty());
    // ⑤ 检索器空指针（装配异常态）
    auto nullRetriever = runMemoryHook(buf, nullptr);
    DOCTEST_CHECK_EQ(nullRetriever.exitCode, 0);
    // 负向断言口径：所有见过的退出码枚举里不含 2
    DOCTEST_CHECK(good.exitCode != 2);
    DOCTEST_CHECK(badJson.exitCode != 2);
    DOCTEST_CHECK(failSearch.exitCode != 2);
    return true;
})

REGISTER_TEST(memory_hook_parser_sse_and_fused_shapes,
              "memory: 检索缝解析——SSE 信封/嵌套 error/融合段落/mem0 逐条四形态", {
    // SSE 信封 + 融合段落（推测项 6 形态）
    auto sse = parseSearchResponse(
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"融合成的一段记忆\"}]}}");
    DOCTEST_REQUIRE(sse.isOk());
    DOCTEST_REQUIRE_EQ(sse.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(sse.value()[0].text, std::string("融合成的一段记忆"));
    // 纯 JSON + mem0 逐条
    auto items = parseSearchResponse(
        R"({"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"[{\"id\":\"9\",\"memory\":\"端口是 8421\",\"score\":0.83,\"created_at\":\"2026-10-27T10:00:00Z\"}]"}]}})");
    DOCTEST_REQUIRE(items.isOk());
    DOCTEST_REQUIRE_EQ(items.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(items.value()[0].memoryId, std::string("9"));
    DOCTEST_CHECK_EQ(items.value()[0].dateText, std::string("2026-10-27"));
    // 嵌套 error（HTTP 200 body 内）→ Error
    DOCTEST_CHECK(
        parseSearchResponse(
            R"({"result":{"isError":true,"error":{"message":"bad"}}})")
            .isFail());
    DOCTEST_CHECK(parseSearchResponse(
                      R"({"error":"invalid_arguments"})")
                      .isFail());
    return true;
})

REGISTER_TEST(memory_hooks_settings_install_restore,
              "memory: hooks 配置写入/还原——键级 diff、幂等、不碰用户既有 hook", {
    const std::string cmd = "\"C:/Program Files/DevDesk/devdesk.exe\" memory hook";
    // 干净 settings：逐层补建
    bool changed = false;
    auto installed = applyHookEntry("{}", cmd, &changed);
    DOCTEST_REQUIRE(installed.isOk());
    DOCTEST_CHECK(changed);
    DOCTEST_CHECK(installed.value().find("memory hook") != std::string::npos);
    DOCTEST_CHECK(installed.value().find("UserPromptSubmit") !=
                  std::string::npos);
    // 二次安装幂等
    auto again = applyHookEntry(installed.value(), cmd, &changed);
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK(!changed);
    // 用户既有 hook 不受影响；还原只摘 ours
    const std::string withUserHook =
        R"({"hooks":{"UserPromptSubmit":[{"matcher":"","hooks":[{"type":"command","command":"user-own-tool"}]}]},"env":{"A":"B"}})";
    auto merged = applyHookEntry(withUserHook, cmd, &changed);
    DOCTEST_REQUIRE(merged.isOk());
    DOCTEST_CHECK(merged.value().find("user-own-tool") != std::string::npos);
    auto restored = removeHookEntry(merged.value(), cmd, &changed);
    DOCTEST_REQUIRE(restored.isOk());
    DOCTEST_CHECK(changed);
    DOCTEST_CHECK(restored.value().find("memory hook") == std::string::npos);
    DOCTEST_CHECK(restored.value().find("user-own-tool") != std::string::npos);
    DOCTEST_CHECK(restored.value().find("\"A\"") != std::string::npos);
    DOCTEST_CHECK(restored.value().find("\"B\"") != std::string::npos);
    // 还原本无 → no-op
    auto noop = removeHookEntry(withUserHook, cmd, &changed);
    DOCTEST_REQUIRE(noop.isOk());
    DOCTEST_CHECK(!changed);
    return true;
})
