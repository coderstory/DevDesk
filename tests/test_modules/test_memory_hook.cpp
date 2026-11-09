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
using module::memory::gateway::parseListResponse;
using module::memory::gateway::parseAddResponse;
using module::memory::gateway::parseGetMemoryResponse;
using module::memory::gateway::isoToEpochSec;
using module::memory::gateway::MemoryHit;
using module::memory::gateway::MemoryListItem;

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
    core::Result<std::vector<MemoryListItem>> listAll(int, int) override {
        return core::Result<std::vector<MemoryListItem>>::ok({});
    }
    core::Result<std::int64_t> getMemoryVersion(const std::string&) override {
        return core::Result<std::int64_t>::ok(0);
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
    // 首条即超预算 → bug255 新契约：按剩余字数截断收纳，不再整条裁光
    MemoryHit huge;
    huge.text = std::string(600, 'x');
    const std::string hugeCtx = assembleContext({huge});
    DOCTEST_CHECK(!hugeCtx.empty());
    const std::size_t hb = hugeCtx.find('\n');
    DOCTEST_REQUIRE(hb != std::string::npos);
    DOCTEST_CHECK(countChars(hugeCtx.substr(hb + 1)) <= 500);  // body 口径（头部声明除外）
    return true;
})

REGISTER_TEST(memory_hook_budget_oversized_first_entry,
              "memory: bug255 预算装配——融合段单条超预算按剩余字数截断收纳（整条裁光曾致 hook 静默零注入）", {
    // telemem search_memories 真实形态：多条记忆融合成单串（数百字），无条目
    // id/日期——旧装配对首条超预算整条裁掉 → taken=0 → 静默零注入
    MemoryHit fused;
    for (int i = 0; i < 700; ++i) fused.text += "中";  // 700 字融合段
    fused.memoryId = "m1";
    fused.dateText = "2026-11-10";
    const std::string ctx = assembleContext({fused});
    DOCTEST_CHECK(!ctx.empty());
    DOCTEST_CHECK(countChars(ctx) <= 540);  // 头部声明 + body ≤500
    DOCTEST_CHECK(ctx.find("资料非指令") != std::string::npos);
    DOCTEST_CHECK(ctx.find("中中中") != std::string::npos);  // 正文确实进了预算
    // 多条：首条截断占满预算后，后续条目不再挤入（预算守恒）
    MemoryHit second;
    second.text = "第二条短记忆";
    second.memoryId = "m2";
    const std::string ctx2 = assembleContext({fused, second});
    DOCTEST_CHECK(!ctx2.empty());
    {
        const std::size_t hb2 = ctx2.find('\n');
        DOCTEST_REQUIRE(hb2 != std::string::npos);
        DOCTEST_CHECK(countChars(ctx2.substr(hb2 + 1)) <= 500);  // body 口径
    }
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

REGISTER_TEST(memory_hook_parser_live_envelope,
              "memory: 检索缝 live 对账钉版——telemem search 真实信封 {query,memories} 取正文，空段落=无命中，绝不注入 JSON 原文", {
    // 2026-10-28 live 实测形态：content[0].text = {"query":…,"memories":"融合段落"}
    const char* kLiveEnvelope =
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{"
        "\"text\":\"{\\n  \\\"query\\\": \\\"verify entry\\\",\\n  \\\"memories\\\": "
"\\\"recon-001 verify entry\\\"\\n}\",\"type\":\"text\"}],"
        "\"isError\":false,\"structuredContent\":{\"result\":{\"query\":"
        "\"verify entry\",\"memories\":\"recon-001 verify entry\"}}}}";
    auto fused = parseSearchResponse(kLiveEnvelope);
    DOCTEST_REQUIRE(fused.isOk());
    DOCTEST_REQUIRE_EQ(fused.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(fused.value()[0].text, std::string("recon-001 verify entry"));
    // 旧实现缺陷回归钉：信封 JSON 原文不得成为注入正文
    DOCTEST_CHECK(fused.value()[0].text.find("query") == std::string::npos);
    // 空融合段落 = 无命中（delete 后真实形态）
    auto empty = parseSearchResponse(
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"{\\\"query\\\":\\\"q\\\","
        "\\\"memories\\\":\\\"\\\"}\"}]}}");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().empty());
    return true;
})

// BUG-114-1 回归钉：撤销链回填文本比对两侧 trim。实证（2026-11-03 mac e2e）：
// 采集合段文本尾带 '\n'（session_reader extractMessageText 每块固定补换行），
// telemem 存储剥尾空白 → 库内 275 字 vs 入参 274 字，精确比对永失配 →
// target_ids 恒空 → createUndoCard 拒绝（撤销链断）。
REGISTER_TEST(memory_undo_text_match_trim,
              "memory: 撤销链回填比对——尾换行采集文本 vs 引擎剥尾存储（两侧 trim）", {
    using module::memory::gateway::memoryTextMatches;
    // 尾换行差异（缺陷原形）：库内无尾换行 vs 采集带尾换行
    DOCTEST_CHECK(memoryTextMatches("改完代码后用 memory_e2e_probe 验证", "改完代码后用 memory_e2e_probe 验证\n"));
    // 反向：库内带尾空白 vs 采集无
    DOCTEST_CHECK(memoryTextMatches("abc\n", "abc"));
    // 双侧都有空白 + 混合空白类型
    DOCTEST_CHECK(memoryTextMatches("  abc  ", "\tabc\r\n"));
    // trim 不是模糊匹配：实质差异仍失配
    DOCTEST_CHECK_FALSE(memoryTextMatches("abc", "abd"));
    DOCTEST_CHECK_FALSE(memoryTextMatches("abc", "abc def"));
    DOCTEST_CHECK_FALSE(memoryTextMatches("abc", "abc\n尾差"));
    DOCTEST_CHECK_FALSE(memoryTextMatches("abc", "xabc"));
    // 空串对空串成立；空对非空失配
    DOCTEST_CHECK(memoryTextMatches("", ""));
    DOCTEST_CHECK_FALSE(memoryTextMatches("", "x"));
    return true;
})

REGISTER_TEST(memory_gateway_list_parse_and_iso_epoch,
              "memory: 清单缝解析（池31）——get_memories results 逐条化（缺 id/正文跳过、updated_at→epoch）；ISO→epoch 纯函数", {
    // POC 定案形态：content[0].text = {"results":[{id,memory,…,updated_at}]}
    auto items = parseListResponse(
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{"
        "\"text\":\"{\\\"results\\\":[{\\\"id\\\":\\\"uuid-1\\\","
        "\\\"memory\\\":\\\"记忆一\\\",\\\"created_at\\\":\\\"2026-01-01T00:00:00Z\\\","
        "\\\"updated_at\\\":\\\"2026-10-27T12:34:56Z\\\"},"
        "{\\\"memory\\\":\\\"缺id跳过\\\"},"
        "{\\\"id\\\":\\\"uuid-2\\\",\\\"memory\\\":\\\"无时间字段\\\"}]}"
        "\"}]}}");
    DOCTEST_REQUIRE(items.isOk());
    DOCTEST_REQUIRE_EQ(items.value().size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(items.value()[0].id, std::string("uuid-1"));
    DOCTEST_CHECK_EQ(items.value()[0].text, std::string("记忆一"));
    DOCTEST_CHECK_EQ(items.value()[0].updatedAtEpoch,
                     static_cast<std::int64_t>(1793104496));
    // 无 updated_at → epoch 0（消费侧按「无使用记录」跳过，保守不删）
    DOCTEST_CHECK_EQ(items.value()[1].id, std::string("uuid-2"));
    DOCTEST_CHECK_EQ(items.value()[1].updatedAtEpoch,
                     static_cast<std::int64_t>(0));
    // 工具级错误 → fail；空清单 → ok 空
    DOCTEST_CHECK(parseListResponse(R"({"result":{"isError":true}})").isFail());
    auto empty = parseListResponse(
        R"({"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"results\":[]}"}]}})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().empty());
    // ISO → epoch：telemem 定案 UTC 形态 + 空格分隔/毫秒尾缀容忍 + 非法 → 0
    DOCTEST_CHECK_EQ(isoToEpochSec("2026-10-27T12:34:56Z"),
                     static_cast<std::int64_t>(1793104496));
    DOCTEST_CHECK_EQ(isoToEpochSec("2026-10-27 12:34:56"),
                     static_cast<std::int64_t>(1793104496));
    DOCTEST_CHECK_EQ(isoToEpochSec("2026-10-27T12:34:56.789Z"),
                     static_cast<std::int64_t>(1793104496));
    DOCTEST_CHECK_EQ(isoToEpochSec("2026-01-01T00:00:00Z"),
                     static_cast<std::int64_t>(1767225600));
    DOCTEST_CHECK_EQ(isoToEpochSec(""), static_cast<std::int64_t>(0));
    DOCTEST_CHECK_EQ(isoToEpochSec("garbage"), static_cast<std::int64_t>(0));
    DOCTEST_CHECK_EQ(isoToEpochSec("2026-13-40T99:99:99Z"),
                     static_cast<std::int64_t>(0));
    return true;
})

REGISTER_TEST(memory_gateway_get_memory_version_parse,
              "memory: 版本缝解析（池32）——updated_at→epoch（直返/results 包裹两形态）；not_found→NotFound；缺字段 fail-closed", {
    // mem0 get_memory 直返条目形态（POC：与清单条目同形，多 score）
    auto direct = parseGetMemoryResponse(
        R"({"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"id\":\"uuid-9\",\"memory\":\"正文\",\"updated_at\":\"2026-10-27T12:34:56Z\"}"}]}})");
    DOCTEST_REQUIRE(direct.isOk());
    DOCTEST_CHECK_EQ(direct.value(), static_cast<std::int64_t>(1793104496));
    // results 包裹形态兼容
    auto wrapped = parseGetMemoryResponse(
        R"({"result":{"content":[{"text":"{\"results\":[{\"id\":\"uuid-9\",\"updated_at\":\"2026-01-01T00:00:00Z\"}]}"}]}})");
    DOCTEST_REQUIRE(wrapped.isOk());
    DOCTEST_CHECK_EQ(wrapped.value(), static_cast<std::int64_t>(1767225600));
    // not_found（JSON error 形态 / 非 JSON 文本形态）→ NotFound
    auto nf = parseGetMemoryResponse(
        R"({"result":{"content":[{"text":"{\"error\":\"not_found\"}"}]}})");
    DOCTEST_REQUIRE(nf.isFail());
    DOCTEST_CHECK(nf.errorCode() == core::ErrorCode::NotFound);
    auto nfText = parseGetMemoryResponse(
        R"({"result":{"content":[{"text":"memory not_found"}]}})");
    DOCTEST_REQUIRE(nfText.isFail());
    DOCTEST_CHECK(nfText.errorCode() == core::ErrorCode::NotFound);
    // 工具级错误 → fail；缺 updated_at → fail-closed（宁过期不误删）
    DOCTEST_CHECK(
        parseGetMemoryResponse(R"({"result":{"isError":true}})").isFail());
    auto missing = parseGetMemoryResponse(
        R"({"result":{"content":[{"text":"{\"id\":\"uuid-9\"}"}]}})");
    DOCTEST_CHECK(missing.isFail());
    return true;
})

REGISTER_TEST(memory_gateway_add_error_envelope_fail,
              "memory: add 缝解析（bug239）——telemem _call 异常兜底 {error,detail} 回 200 → parseAddResponse 判负不静默；stored 正常形态不受影响", {
    // bug239 真机实证形态（2026-11-05）：云端 LLM 401 → telemem mcp/server.py
    // _call 兜底 {"error","detail"} 普通字典（非 isError）→ MCP HTTP 200；
    // 旧实现忽略 error 键 → 静默 ok("") → 记账出队但引擎零产物。
    auto failed = parseAddResponse(
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"content\":[{"
        "\"text\":\"{\\\"error\\\":\\\"AuthenticationError\\\","
        "\\\"detail\\\":\\\"Error code: 401 - 令牌已过期或验证不正确\\\"}\"}],"
        "\"isError\":false}}");
    DOCTEST_REQUIRE(failed.isFail());
    DOCTEST_CHECK(failed.errorCode() == core::ErrorCode::HttpError);
    DOCTEST_CHECK(failed.errorMessage().find("AuthenticationError") !=
                  std::string::npos);
    DOCTEST_CHECK(failed.errorMessage().find("401") != std::string::npos);
    // 正常 stored 形态（无 error 键）仍 ok 空 id（既有契约）
    auto good = parseAddResponse(
        R"({"jsonrpc":"2.0","id":7,"result":{"content":[{"type":"text","text":"{\"status\":\"stored\"}"}]}})");
    DOCTEST_REQUIRE(good.isOk());
    DOCTEST_CHECK(good.value().empty());
    // 未来带 id 形态仍 ok 取 id
    auto withId = parseAddResponse(
        R"({"jsonrpc":"2.0","id":7,"result":{"content":[{"type":"text","text":"{\"id\":\"uuid-3\"}"}]}})");
    DOCTEST_REQUIRE(withId.isOk());
    DOCTEST_CHECK_EQ(withId.value(), std::string("uuid-3"));
    return true;
})

// ── 需-3 前缀文法与分组渲染（细则 §1/§4，解析契约单测落点）──

REGISTER_TEST(memory_hook_prefix_parse_contract,
              "memory: 需-3 前缀文法——四类合法归组号；畸形三态（无|/未知类型/非[起始）+空主题/无闭合/带空格全未分类，永不报错", {
    const auto ok = parseMemoryPrefix("[踩坑|分库分表] @Transactional 自调用不生效");
    DOCTEST_CHECK(ok.valid);
    DOCTEST_CHECK_EQ(ok.group, 0);
    DOCTEST_CHECK_EQ(ok.tag, std::string("[踩坑|分库分表]"));
    DOCTEST_CHECK(parseMemoryPrefix("[决策|存储选型] x").group == 1);
    DOCTEST_CHECK(parseMemoryPrefix("[契约|接口约定] x").group == 2);
    DOCTEST_CHECK(parseMemoryPrefix("[环境|双机差异] x").group == 3);
    // 畸形三态（细则 §1.3）：一律未分类，不抛错不拒存
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[踩坑] 无管道符").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[感想|X] 未知类型").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("正文在前 [踩坑|A] 非 [ 起始").valid);
    // 其余非法形态
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[踩坑|] 空主题").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[踩坑|无闭合").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[踩坑 |X] 类型带空格").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("").valid);
    DOCTEST_CHECK_FALSE(parseMemoryPrefix("[").valid);
    // 主题以首个 ] 为准（内含 ] 非法用法按截断处理）
    const auto cut = parseMemoryPrefix("[决策|A]B] 尾部");
    DOCTEST_CHECK(cut.valid);
    DOCTEST_CHECK_EQ(cut.tag, std::string("[决策|A]"));
    return true;
})

REGISTER_TEST(memory_hook_prefix_group_render_order,
              "memory: 需-3 分组渲染——四类归组、固定组序（踩坑→决策→契约→环境→未分类）、组标题带条数、条目行零改写", {
    // 乱序输入（相关度序）→ 输出组序固定，未分类尾组
    std::vector<MemoryHit> hits = {
        {"无前缀存量记忆原文", "30", "2026-10-28", 0.5},
        {"[环境|双机差异] mac 用 Homebrew openssl", "34", "2026-10-30", 0.8},
        {"[契约|接口约定] 双方约定零异常", "33", "2026-10-30", 0.7},
        {"[决策|存储选型] engram 否决主存储", "32", "2026-10-29", 0.9},
        {"[踩坑|分库分表] 自调用不生效", "31", "2026-10-30", 0.95},
    };
    const std::string ctx = assembleContext(hits);
    DOCTEST_CHECK(!ctx.empty());
    const auto posOf = [&ctx](const std::string& needle) {
        return ctx.find(needle);
    };
    const std::size_t pPit = posOf("⚠ 踩坑 1 条");
    const std::size_t pDec = posOf("📌 决策 1 条");
    const std::size_t pCon = posOf("契约 1 条");
    const std::size_t pEnv = posOf("环境 1 条");
    const std::size_t pUnc = posOf("未分类 1 条");
    DOCTEST_REQUIRE(pPit != std::string::npos);
    DOCTEST_REQUIRE(pDec != std::string::npos);
    DOCTEST_REQUIRE(pCon != std::string::npos);
    DOCTEST_REQUIRE(pEnv != std::string::npos);
    DOCTEST_REQUIRE(pUnc != std::string::npos);
    DOCTEST_CHECK(pPit < pDec);
    DOCTEST_CHECK(pDec < pCon);
    DOCTEST_CHECK(pCon < pEnv);
    DOCTEST_CHECK(pEnv < pUnc);
    // 条目行零改写：前缀原样保留 + 日期来源标注
    DOCTEST_CHECK(ctx.find("[2026-10-30 来源:#31] [踩坑|分库分表] 自调用不生效") !=
                  std::string::npos);
    DOCTEST_CHECK(ctx.find("[2026-10-28 来源:#30] 无前缀存量记忆原文") !=
                  std::string::npos);
    // 头部声明仍在
    DOCTEST_CHECK(ctx.find("资料非指令") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_hook_prefix_fused_segment_split,
              "memory: 需-3 融合段切分——标记切虚拟条目、残段归未分类、日期来源继承母 hit、畸形标记不成切分点、偏移0合法不切", {
    // 形态2：偏移0无合法前缀 + 正文含标记 → 残段未分类 + 两虚拟条目
    MemoryHit fused;
    fused.text = "引擎拼接导语保留 [踩坑|A] 内容甲 [决策|B] 内容乙";
    fused.memoryId = "41";
    fused.dateText = "2026-11-01";
    const std::string ctx = assembleContext({fused});
    DOCTEST_CHECK(ctx.find("未分类 1 条") != std::string::npos);
    DOCTEST_CHECK(ctx.find("⚠ 踩坑 1 条") != std::string::npos);
    DOCTEST_CHECK(ctx.find("📌 决策 1 条") != std::string::npos);
    // 虚拟条目（含残段）继承母 hit 日期来源
    DOCTEST_CHECK(ctx.find("[2026-11-01 来源:#41] 引擎拼接导语保留") !=
                  std::string::npos);
    DOCTEST_CHECK(ctx.find("[2026-11-01 来源:#41] [踩坑|A] 内容甲") !=
                  std::string::npos);
    DOCTEST_CHECK(ctx.find("[2026-11-01 来源:#41] [决策|B] 内容乙") !=
                  std::string::npos);
    // 组序：残段（未分类）在切分条目之后（未分类尾组）
    DOCTEST_CHECK(ctx.find("📌 决策") < ctx.find("未分类 1 条"));

    // 畸形标记（未知类型）不是切分点：残段含畸形标记整体入未分类
    MemoryHit bad;
    bad.text = "[感想|X] 假标记导语 [踩坑|Y] 真标记尾部";
    bad.memoryId = "42";
    bad.dateText = "2026-11-01";
    const std::string ctxBad = assembleContext({bad});
    DOCTEST_CHECK(ctxBad.find("未分类 1 条") != std::string::npos);
    DOCTEST_CHECK(ctxBad.find("[感想|X] 假标记导语") != std::string::npos);
    DOCTEST_CHECK(ctxBad.find("⚠ 踩坑 1 条") != std::string::npos);
    DOCTEST_CHECK(ctxBad.find("[踩坑|Y] 真标记尾部") != std::string::npos);

    // 形态1：偏移 0 合法前缀 → 单条一组员，正文内标记不触发切分
    MemoryHit head0;
    head0.text = "[契约|C] 正文里出现 [踩坑|D] 不切分";
    head0.memoryId = "43";
    head0.dateText = "2026-11-01";
    const std::string ctx0 = assembleContext({head0});
    DOCTEST_CHECK(ctx0.find("契约 1 条") != std::string::npos);
    DOCTEST_CHECK(ctx0.find("⚠ 踩坑") == std::string::npos);
    DOCTEST_CHECK(ctx0.find("[契约|C] 正文里出现 [踩坑|D] 不切分") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(memory_hook_prefix_budget_group_survival,
              "memory: 需-3 预算截断——先分组后截断踩坑优先存活、标题计字不计条、切分条各占 1 条、超预算条截断收纳（bug255）", {
    // 条数预算：2 踩坑 + 4 决策 = 6 条（切分后）→ 渲染 5，踩坑组全活，决策截 3
    std::vector<MemoryHit> hits;
    hits.push_back({"[踩坑|甲] 踩坑一", "1", "2026-11-01", 0.9});
    hits.push_back({"[踩坑|乙] 踩坑二", "2", "2026-11-01", 0.8});
    for (int i = 0; i < 4; ++i) {
        hits.push_back({"[决策|批次] 决策条目" + std::to_string(i), "3",
                        "2026-11-01", 0.5});
    }
    const std::string ctx = assembleContext(hits);
    DOCTEST_CHECK(ctx.find("⚠ 踩坑 2 条") != std::string::npos);
    DOCTEST_CHECK(ctx.find("📌 决策 3 条") != std::string::npos);
    DOCTEST_CHECK(ctx.find("决策条目2") != std::string::npos);
    DOCTEST_CHECK(ctx.find("决策条目3") == std::string::npos);  // 第 6 条裁掉
    // 组标题计字不计条：字数预算内（头部声明除外，与 ≤500 同口径不含头部）
    const std::size_t headerEnd = ctx.find('\n');
    DOCTEST_REQUIRE(headerEnd != std::string::npos);
    DOCTEST_CHECK(countChars(ctx.substr(headerEnd + 1)) <= 500);

    // 字数预算：踩坑短行存活；后继组长行超预算 → 截断收纳（bug255：整组
    // 缺席曾致融合段场景预算永远填不满 → hook 静默零注入）；组序不变
    std::vector<MemoryHit> mix;
    mix.push_back({"[踩坑|短] 短踩坑", "1", "2026-11-01", 0.9});
    MemoryHit longDecision;
    longDecision.text = "[决策|长] " + std::string(600, 'y');
    longDecision.memoryId = "2";
    mix.push_back(longDecision);
    const std::string ctx2 = assembleContext(mix);
    DOCTEST_CHECK(ctx2.find("⚠ 踩坑 1 条") != std::string::npos);
    DOCTEST_CHECK(ctx2.find("短踩坑") != std::string::npos);
    DOCTEST_CHECK(ctx2.find("📌 决策 1 条") != std::string::npos);  // 截断收纳
    {
        const std::size_t hb = ctx2.find('\n');
        DOCTEST_REQUIRE(hb != std::string::npos);
        DOCTEST_CHECK(countChars(ctx2.substr(hb + 1)) <= 500);  // body 口径守恒
    }

    // 融合段虚拟条目计入条数：1 hit 切 3 条 + 3 直存条 = 6 → 只渲染 5
    MemoryHit fused;
    fused.text = "残段 [踩坑|A] 甲 [决策|B] 乙";
    fused.memoryId = "9";
    fused.dateText = "2026-11-01";
    std::vector<MemoryHit> six;
    six.push_back(fused);
    for (int i = 0; i < 3; ++i) {
        six.push_back({"[契约|批] 契约条" + std::to_string(i),
                       std::to_string(10 + i), "2026-11-01", 0.4});
    }
    const std::string ctx3 = assembleContext(six);
    // 渲染序：踩坑(甲) → 决策(乙) → 契约×3 → 未分类(残段) = 6 条 → 末位裁掉
    DOCTEST_CHECK(ctx3.find("⚠ 踩坑 1 条") != std::string::npos);
    DOCTEST_CHECK(ctx3.find("契约 3 条") != std::string::npos);
    DOCTEST_CHECK(ctx3.find("未分类") == std::string::npos);  // 残段被裁
    return true;
})

REGISTER_TEST(memory_hook_long_prompt_codepoint_truncation,
              "memory: bug255 长提问截断——>4000 字节查询按码点边界截断（字节 substr 劈开多字节 → telemem 400 → 静默零注入）", {
    FakeRetriever ok;
    ok.hits = {MemoryHit{"记忆条目", "1", "2026-11-10", 0.9}};
    std::string cjk = "这是一个用于截断回归测试的中文句子";  // 3 字节/字
    std::string prompt = cjk;
    constexpr std::size_t kQueryCapBytes = 4000;  // 同 hook_command.cpp kMaxQueryChars（匿名空间不可见）
    while (prompt.size() <= kQueryCapBytes) prompt += cjk;
    std::vector<char> buf(prompt.size() + 256, '\0');
    std::snprintf(buf.data(), buf.size(), kStdinTemplate, prompt.c_str());
    auto r = runMemoryHook(std::string(buf.data()), &ok);  // C-string 截断嵌入 NUL
    DOCTEST_CHECK_EQ(r.exitCode, 0);
    DOCTEST_CHECK(!r.stdoutText.empty());
    DOCTEST_CHECK(ok.lastQuery.size() <= kQueryCapBytes);
    // 尾字节不是裸前导字节——码点完整，telemem 侧 JSON 解析必成功
    DOCTEST_CHECK(!ok.lastQuery.empty());
    const auto last = static_cast<unsigned char>(ok.lastQuery.back());
    DOCTEST_CHECK((last & 0xC0) != 0xC0);
    return true;
})
