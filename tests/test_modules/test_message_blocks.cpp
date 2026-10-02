// tests/test_modules/test_message_blocks.cpp
// agent_messages.contentJson 的块级编解码 + 预览/摘要/高度估算用例
//（run 日志块级渲染改造，2026-10-02 用户反馈「日志展示太简陋 / 长输出看不全」）。
//
// 钉的契约（module_agent/message_blocks.h 头注释逐字）：
//   - decodeMessageBlocks：blocks 数组 → Text/ToolUse/ToolResult 块；
//     旧版纯文本行 / 坏 JSON / 非数组 JSON / JSON 字符串字面量 → 单 Plain 块
//     回落（旧数据不丢，容错降级不是报错）；
//   - utf8Truncate：UTF-8 边界安全（不切半个多字节字符）；
//   - toolArgsSummary：单行化 + 截断；
//   - estimateTextBlockHeight / estimateMessageHeight /
//     estimateMessagesAreaHeight：长度单调不减、面积高有上下钳制。
//
// tool_info（可用工具区块）的 schema 解码用例在本文件尾部：
//   decodeToolInfo 取 name/description/input_schema.properties 键集，
//   坏 schema 容错回落 name-only。
#include "doctest.h"
#include "test_support.h"

#include "module_agent/message_blocks.h"
#include "module_agent/tool_info.h"
#include "module_agent/agent_store.h"

#include <set>
#include <string>
#include <vector>

namespace {

using module::agent::MessageBlock;
using MessageType = module::agent::MessageBlock::Type;

const MessageBlock* findBlock(const std::vector<MessageBlock>& blocks,
                              MessageType type) {
    for (const auto& b : blocks) {
        if (b.type == type) return &b;
    }
    return nullptr;
}

} // namespace

// ============ decodeMessageBlocks：blocks 数组 ============

REGISTER_TEST(msg_blocks_text_block,
              "msg_blocks: [{type:text}] → Text 块取 text 字段", {
    auto blocks = module::agent::decodeMessageBlocks(
        R"([{"type":"text","text":"你好，世界"}])");
    DOCTEST_REQUIRE(blocks.size() == 1);
    DOCTEST_CHECK(blocks[0].type == MessageType::Text);
    DOCTEST_CHECK_EQ(blocks[0].text, std::string("你好，世界"));
    return true;
})

REGISTER_TEST(msg_blocks_mixed_tool_use_and_result,
              "msg_blocks: assistant 混合块 text+tool_use 提取工具名与 input", {
    auto blocks = module::agent::decodeMessageBlocks(
        R"([{"type":"text","text":"我来查一下"},)"
        R"({"type":"tool_use","id":"tu_1","name":"todo.list","input":{"limit":5}}])");
    DOCTEST_REQUIRE(blocks.size() == 2);
    DOCTEST_CHECK(blocks[0].type == MessageType::Text);
    DOCTEST_CHECK_EQ(blocks[0].text, std::string("我来查一下"));
    const auto* tu = findBlock(blocks, MessageType::ToolUse);
    DOCTEST_REQUIRE(tu != nullptr);
    DOCTEST_CHECK_EQ(tu->toolName, std::string("todo.list"));
    // input 保留 JSON 原文（渲染层自己做截断摘要）
    DOCTEST_CHECK(tu->text.find("\"limit\":5") != std::string::npos ||
                  tu->text.find("\"limit\": 5") != std::string::npos);
    return true;
})

REGISTER_TEST(msg_blocks_tool_result_content_variants,
              "msg_blocks: tool_result content 字符串/blocks 数组两种形态都取文本", {
    // 形态 1：content 是字符串（Anthropic 协议允许）
    auto blocks1 = module::agent::decodeMessageBlocks(
        R"([{"type":"tool_result","tool_use_id":"tu_1","content":"共 3 条逾期","is_error":false}])");
    const auto* tr1 = findBlock(blocks1, MessageType::ToolResult);
    DOCTEST_REQUIRE(tr1 != nullptr);
    DOCTEST_CHECK_EQ(tr1->text, std::string("共 3 条逾期"));
    DOCTEST_CHECK(!tr1->isError);

    // 形态 2：content 是 text blocks 数组（agent_loop 落库的实际形态）
    auto blocks2 = module::agent::decodeMessageBlocks(
        R"([{"type":"tool_result","tool_use_id":"tu_2","content":[{"type":"text","text":"工具崩溃了"}],"is_error":true}])");
    const auto* tr2 = findBlock(blocks2, MessageType::ToolResult);
    DOCTEST_REQUIRE(tr2 != nullptr);
    DOCTEST_CHECK_EQ(tr2->text, std::string("工具崩溃了"));
    DOCTEST_CHECK(tr2->isError);
    return true;
})

// ============ decodeMessageBlocks：容错回落（旧数据/坏数据） ============

REGISTER_TEST(msg_blocks_legacy_plain_text_fallback,
              "msg_blocks: 纯文本/坏 JSON/非数组/JSON 字符串 → 单 Plain 块原文", {
    // 旧版纯文本行（M1 早期可能落过）
    auto plain = module::agent::decodeMessageBlocks("just plain text");
    DOCTEST_REQUIRE(plain.size() == 1);
    DOCTEST_CHECK(plain[0].type == MessageType::Plain);
    DOCTEST_CHECK_EQ(plain[0].text, std::string("just plain text"));

    // 坏 JSON（截断的 blocks 数组）
    auto bad = module::agent::decodeMessageBlocks(R"([{"type":"text","text":"截断)");
    DOCTEST_REQUIRE(bad.size() == 1);
    DOCTEST_CHECK(bad[0].type == MessageType::Plain);
    DOCTEST_CHECK_EQ(bad[0].text, std::string(R"([{"type":"text","text":"截断)"));

    // 合法 JSON 但不是数组（对象/数字）
    auto obj = module::agent::decodeMessageBlocks(R"({"error":"boom"})");
    DOCTEST_REQUIRE(obj.size() == 1);
    DOCTEST_CHECK(obj[0].type == MessageType::Plain);
    DOCTEST_CHECK(obj[0].text.find("boom") != std::string::npos);

    // JSON 字符串字面量（LLM 网关兼容形态）→ Text 取字符串值
    auto str = module::agent::decodeMessageBlocks(R"("hello world")");
    DOCTEST_REQUIRE(str.size() == 1);
    DOCTEST_CHECK(str[0].type == MessageType::Text);
    DOCTEST_CHECK_EQ(str[0].text, std::string("hello world"));

    // 空串 → 空向量（渲染层跳过）
    auto empty = module::agent::decodeMessageBlocks("");
    DOCTEST_CHECK(empty.empty());
    return true;
})

REGISTER_TEST(msg_blocks_unknown_block_type_preserved_as_plain,
              "msg_blocks: 未知块类型不丢——回落 Plain 保留原文", {
    auto blocks = module::agent::decodeMessageBlocks(
        R"([{"type":"thinking","thinking":"内部推理…"}])");
    DOCTEST_REQUIRE(blocks.size() == 1);
    DOCTEST_CHECK(blocks[0].type == MessageType::Plain);
    DOCTEST_CHECK(blocks[0].text.find("内部推理") != std::string::npos);
    return true;
})

// ============ utf8Truncate：边界安全 ============

REGISTER_TEST(msg_blocks_utf8_truncate_boundary,
              "msg_blocks: utf8Truncate 不切半个多字节字符", {
    // 9 字节的「你好啊」（每字 3 字节）：上限 7 → 回退到 6（截掉最后一个字）
    DOCTEST_CHECK_EQ(module::agent::utf8Truncate("你好啊", 7),
                     std::string("你好"));
    // 上限大于长度 → 原样
    DOCTEST_CHECK_EQ(module::agent::utf8Truncate("你好", 100),
                     std::string("你好"));
    // ASCII 精确截断
    DOCTEST_CHECK_EQ(module::agent::utf8Truncate("abcdef", 3),
                     std::string("abc"));
    // 空串安全
    DOCTEST_CHECK_EQ(module::agent::utf8Truncate("", 5), std::string(""));
    return true;
})

// ============ 预览与摘要 ============

REGISTER_TEST(msg_blocks_preview_120_bytes,
              "msg_blocks: 预览取解码文本前 120 字节（超长加省略号）", {
    const std::string longAscii(200, 'x');
    auto blocksJson =
        R"([{"type":"text","text":")" + longAscii + R"("}])";
    std::string preview =
        module::agent::messagePreviewFromJson(blocksJson, 120);
    // 120 字节 + 3 字节省略号 = 123
    DOCTEST_CHECK_EQ(preview.size(), static_cast<size_t>(123));
    DOCTEST_CHECK(preview[120] == '\xe2');  // … 的首字节
    DOCTEST_CHECK(preview.substr(0, 120) == longAscii.substr(0, 120));

    // 短文本原样
    std::string shortPreview =
        module::agent::messagePreviewFromJson(R"([{"type":"text","text":"短"}])", 120);
    DOCTEST_CHECK_EQ(shortPreview, std::string("短"));

    // 旧数据纯文本也走同一入口
    DOCTEST_CHECK_EQ(module::agent::messagePreviewFromJson("legacy row", 120),
                     std::string("legacy row"));
    return true;
})

REGISTER_TEST(msg_blocks_preview_single_line,
              "msg_blocks: 预览单行化——多行文本摊平成一行（wrap(false) 元素画 \\n 会溢出压行）", {
    // tool_result 的摊平输出天然带 \n（20 行清单）：预览必须不含任何换行
    std::string multiLineJson =
        R"([{"type":"tool_result","tool_use_id":"t1","content":[{"type":"text","text":"第一行\n第二行\n第三行"}],"is_error":false}])";
    std::string preview =
        module::agent::messagePreviewFromJson(multiLineJson, 120);
    DOCTEST_CHECK_MESSAGE(preview.find('\n') == std::string::npos,
                          "预览含换行，实测: " + preview);
    DOCTEST_CHECK_MESSAGE(preview.find("第一行") != std::string::npos &&
                              preview.find("第二行") != std::string::npos,
                          "摊平后内容丢失，实测: " + preview);
    return true;
})

REGISTER_TEST(msg_blocks_tool_args_summary_single_line,
              "msg_blocks: 工具参数摘要压成单行并截断", {
    std::string summary = module::agent::toolArgsSummary(
        R"({"path":"a.md","content":"第一行
第二行"})" , 80);
    DOCTEST_CHECK(summary.find('\n') == std::string::npos);
    DOCTEST_CHECK(summary.find("a.md") != std::string::npos);
    DOCTEST_CHECK(summary.size() <= 80 + 3);  // 截断 + 省略号
    return true;
})

// ============ 高度估算：单调 + 钳制 ============

REGISTER_TEST(msg_blocks_height_estimator_monotonic_and_clamped,
              "msg_blocks: 展开高度随内容单调不减；区域高钳在 [min,max]", {
    using namespace module::agent;
    const float w = 400.0f;
    // 单调：同宽下更长的文本 → 高度不减
    float h1 = estimateTextBlockHeight("短文本", w, 12.0f, 18.0f);
    std::string longText(2000, 'a');
    float h2 = estimateTextBlockHeight(longText, w, 12.0f, 18.0f);
    DOCTEST_CHECK(h2 > h1);
    DOCTEST_CHECK(h1 >= 18.0f);  // 至少一行

    // 消息高：展开 > 收起（同一条消息）。row 按 AgentStore 落库形态构造
    module::agent::AgentMessageRow row;
    row.seq = 0;
    row.role = "user";
    row.contentJson = R"([{"type":"text","text":"hello"}])";
    float hCollapsed = estimateMessageHeight(row, /*expanded=*/false, w, 18.0f);
    float hExpanded = estimateMessageHeight(row, /*expanded=*/true, w, 18.0f);
    DOCTEST_CHECK(hExpanded > hCollapsed);

    // 区域高：钳制生效——再多消息也不超过 maxH
    std::vector<module::agent::AgentMessageRow> many;
    for (int i = 0; i < 500; ++i) {
        module::agent::AgentMessageRow m;
        m.seq = i;
        m.role = "assistant";
        m.contentJson = R"([{"type":"text","text":")" + longText + R"("}])";
        many.push_back(m);
    }
    std::set<int64_t> allExpanded;
    for (const auto& m : many) allExpanded.insert(m.seq);
    float area = estimateMessagesAreaHeight(many, allExpanded, w, 18.0f,
                                            /*minH=*/96.0f, /*maxH=*/520.0f);
    DOCTEST_CHECK(area <= 520.0f + 0.5f);
    float areaMin = estimateMessagesAreaHeight(many, {}, w, 18.0f,
                                               96.0f, 520.0f);
    DOCTEST_CHECK(areaMin >= 96.0f - 0.5f);
    // 空消息列表 → minH
    float areaEmpty = estimateMessagesAreaHeight({}, {}, w, 18.0f, 96.0f, 520.0f);
    DOCTEST_CHECK(areaEmpty <= 96.0f + 0.5f);
    return true;
})

// ============ tool_info：schema 解码 ============

REGISTER_TEST(msg_blocks_tool_info_decode_schema,
              "msg_blocks: decodeToolInfo 取 name/description/参数名集", {
    auto info = module::agent::decodeToolInfo(
        R"({"name":"md.search","description":"全文搜索笔记",)"
        R"("input_schema":{"type":"object","properties":{)"
        R"("query":{"type":"string"},"limit":{"type":"integer"}},)"
        R"("required":["query"]}})");
    DOCTEST_CHECK_EQ(info.name, std::string("md.search"));
    DOCTEST_CHECK(info.description.find("全文搜索") != std::string::npos);
    DOCTEST_REQUIRE(info.params.size() == 2);
    DOCTEST_CHECK(info.params[0] == "limit");   // map 序：字母序
    DOCTEST_CHECK(info.params[1] == "query");
    return true;
})

REGISTER_TEST(msg_blocks_tool_info_malformed_schema,
              "msg_blocks: 坏 schema 容错——name 尽力取，不抛不崩", {
    auto bad = module::agent::decodeToolInfo("not json at all");
    DOCTEST_CHECK(bad.name.empty());
    DOCTEST_CHECK(bad.params.empty());

    // 有 name 但没有 input_schema → 参数空
    auto noSchema = module::agent::decodeToolInfo(
        R"({"name":"app.stats","description":"统计"})");
    DOCTEST_CHECK_EQ(noSchema.name, std::string("app.stats"));
    DOCTEST_CHECK(noSchema.params.empty());
    return true;
})
