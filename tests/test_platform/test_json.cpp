#include "doctest.h"
#include "platform/json.h"

#include <cstdio>
#include <utility>
#include <vector>

using namespace platform::json;

// ---- CP-02 回归（2026-09-27 全量审计 core-platform，high）----
// 旧实现里 JsonValue 有"public 镜像字段（type_/boolVal_/intVal_/dblVal_/strVal_）
// + 堆上 JsonImpl"两份真值，4 个特殊成员只搬 data_（和 type_），标量镜像不同步：
// 左值拷贝/移动后 isString() 仍是 true，但 asString() 静默返回 ""、asInt() 返回 0。
// 下面 5 条用例覆盖"左值拷贝构造 / 拷贝赋值 / 左值移动构造 / 移动赋值 / 容器存取"
// 五条路径，任何一条漏同步即失败。

REGISTER_TEST(json_copy_ctor_keeps_value, "json: copy ctor keeps scalar value", {
    JsonValue src = makeString("x");   // 具名左值：初始化必须是真拷贝构造
    JsonValue copied = src;
    DOCTEST_CHECK_EQ(copied.asString(), std::string("x"));
    DOCTEST_CHECK(copied.isString());

    JsonValue srcNum = makeInt(42);
    JsonValue copiedNum = srcNum;
    DOCTEST_CHECK_EQ(copiedNum.asInt(), static_cast<int64_t>(42));
    DOCTEST_CHECK(copiedNum.isNumber());

    JsonValue srcBool = makeBool(true);
    JsonValue copiedBool = srcBool;
    DOCTEST_CHECK(copiedBool.asBool());

    JsonValue srcDbl = makeDouble(2.5);
    JsonValue copiedDbl = srcDbl;
    DOCTEST_CHECK(copiedDbl.asDouble() == 2.5);
    return true;
})

REGISTER_TEST(json_copy_assign_keeps_value, "json: copy assignment keeps scalar value", {
    JsonValue src = makeString("x");
    JsonValue dst;                     // 先造一个 Null，再拷贝赋值覆盖
    dst = src;
    DOCTEST_CHECK_EQ(dst.asString(), std::string("x"));
    DOCTEST_CHECK(dst.isString());

    JsonValue srcNum = makeInt(42);
    JsonValue dstNum;
    dstNum = srcNum;
    DOCTEST_CHECK_EQ(dstNum.asInt(), static_cast<int64_t>(42));
    DOCTEST_CHECK(dstNum.isNumber());

    JsonValue srcBool = makeBool(true);
    JsonValue dstBool;
    dstBool = srcBool;
    DOCTEST_CHECK(dstBool.asBool());

    JsonValue srcDbl = makeDouble(2.5);
    JsonValue dstDbl;
    dstDbl = srcDbl;
    DOCTEST_CHECK(dstDbl.asDouble() == 2.5);
    return true;
})

REGISTER_TEST(json_move_ctor_keeps_value, "json: move ctor keeps scalar value", {
    JsonValue src = makeString("x");
    JsonValue moved = std::move(src);   // 具名左值 move：必须是真移动构造
    DOCTEST_CHECK_EQ(moved.asString(), std::string("x"));
    DOCTEST_CHECK(moved.isString());

    JsonValue srcNum = makeInt(42);
    JsonValue movedNum = std::move(srcNum);
    DOCTEST_CHECK_EQ(movedNum.asInt(), static_cast<int64_t>(42));
    DOCTEST_CHECK(movedNum.isNumber());

    JsonValue srcBool = makeBool(true);
    JsonValue movedBool = std::move(srcBool);
    DOCTEST_CHECK(movedBool.asBool());

    JsonValue srcDbl = makeDouble(2.5);
    JsonValue movedDbl = std::move(srcDbl);
    DOCTEST_CHECK(movedDbl.asDouble() == 2.5);

    // 被移动之后的源对象回到合法 null（不得残留旧值/旧类型）
    JsonValue srcStr2 = makeString("y");
    JsonValue movedStr2 = std::move(srcStr2);
    DOCTEST_CHECK_EQ(movedStr2.asString(), std::string("y"));
    DOCTEST_CHECK(srcStr2.isNull());
    DOCTEST_CHECK_EQ(srcStr2.asString(), std::string(""));
    return true;
})

REGISTER_TEST(json_move_assign_keeps_value, "json: move assignment keeps scalar value", {
    JsonValue src = makeString("x");
    JsonValue dst;
    dst = std::move(src);
    DOCTEST_CHECK_EQ(dst.asString(), std::string("x"));
    DOCTEST_CHECK(dst.isString());

    JsonValue srcNum = makeInt(42);
    JsonValue dstNum;
    dstNum = std::move(srcNum);
    DOCTEST_CHECK_EQ(dstNum.asInt(), static_cast<int64_t>(42));
    DOCTEST_CHECK(dstNum.isNumber());

    JsonValue srcBool = makeBool(true);
    JsonValue dstBool;
    dstBool = std::move(srcBool);
    DOCTEST_CHECK(dstBool.asBool());

    JsonValue srcDbl = makeDouble(2.5);
    JsonValue dstDbl;
    dstDbl = std::move(srcDbl);
    DOCTEST_CHECK(dstDbl.asDouble() == 2.5);
    return true;
})

// JsonObject 同款路径复查（CP-02 顺带结论）：JsonObject 只有 data_ 一份状态、
// 没有镜像字段，4 个特殊成员不涉及"漏同步"——用同一套左值拷贝/移动断言固化结论。
REGISTER_TEST(json_object_copy_move_keeps_keys, "json: JsonObject copy/move keeps keys", {
    JsonObject src;
    src.set("name", makeString("DevDesk"));
    JsonObject copied = src;                 // 拷贝构造
    DOCTEST_CHECK_EQ(copied.get("name").asString(), std::string("DevDesk"));
    JsonObject copyAssigned;                 // 拷贝赋值
    copyAssigned = src;
    DOCTEST_CHECK_EQ(copyAssigned.get("name").asString(), std::string("DevDesk"));
    JsonObject moved = std::move(src);       // 移动构造
    DOCTEST_CHECK_EQ(moved.get("name").asString(), std::string("DevDesk"));
    JsonObject moveAssigned;                 // 移动赋值
    moveAssigned = std::move(moved);
    DOCTEST_CHECK_EQ(moveAssigned.get("name").asString(), std::string("DevDesk"));
    DOCTEST_CHECK_FALSE(moved.has("name"));  // 被移动的源回到空对象，不残留
    return true;
})

// 现实触发路径：容器存取（push_back 左值 = 拷贝构造、prvalue = 移动构造、
// 扩容搬元素 = 移动构造）——审计点名的"看似无害的重构"场景。
REGISTER_TEST(json_container_storage_keeps_value, "json: container storage keeps scalar value", {
    std::vector<JsonValue> items;
    items.push_back(makeString("prvalue"));   // prvalue → 移动构造
    JsonValue lvalue = makeString("lvalue");
    items.push_back(lvalue);                  // 左值 → 拷贝构造 + 扩容搬移
    DOCTEST_CHECK_EQ(items[0].asString(), std::string("prvalue"));
    DOCTEST_CHECK_EQ(items[1].asString(), std::string("lvalue"));

    // 拷贝出去的字符串值也拿到（覆盖 String 之外的 Number 走容器）
    JsonValue num = makeInt(7);
    std::vector<JsonValue> nums;
    nums.push_back(num);
    nums.push_back(std::move(num));
    DOCTEST_CHECK_EQ(nums[0].asInt(), static_cast<int64_t>(7));
    DOCTEST_CHECK_EQ(nums[1].asInt(), static_cast<int64_t>(7));
    return true;
})

REGISTER_TEST(json_null, "json: create null", {
    auto v = makeNull();
    DOCTEST_CHECK(v.isNull());
    return true;
})

REGISTER_TEST(json_primitives, "json: create bool/int/string", {
    DOCTEST_CHECK(makeBool(true).asBool() == true);
    DOCTEST_CHECK(makeBool(false).asBool() == false);
    DOCTEST_CHECK(makeInt(42).asInt() == 42);
    DOCTEST_CHECK(makeDouble(3.14).asDouble() == 3.14);
    DOCTEST_CHECK(makeString("hello").asString() == "hello");
    return true;
})

REGISTER_TEST(json_object, "json: object set/get", {
    JsonObject obj;
    obj.set("name", makeString("DevDesk"));
    obj.set("version", makeString("0.1.0"));
    DOCTEST_CHECK(obj.has("name"));
    DOCTEST_CHECK(obj.has("version"));
    DOCTEST_CHECK_FALSE(obj.has("nonexistent"));
    DOCTEST_CHECK_EQ(obj.get("name").asString(), std::string("DevDesk"));
    return true;
})

REGISTER_TEST(json_stringify_primitives, "json: stringify primitives", {
    auto r = stringify(makeInt(42));
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("42"));

    auto r2 = stringify(makeString("hi"));
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value(), std::string("\"hi\""));

    auto r3 = stringify(makeNull());
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value(), std::string("null"));
    return true;
})

// ---- parse / stringify 对象往返（T11 实装后补的用例，config.json 依赖） ----

REGISTER_TEST(json_parse_object, "json: parse object with mixed types", {
    auto r = parse("{\"theme\": \"light\", \"fontScalePercent\": 115, \"ok\": true}");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(r.value().isObject());
    JsonObject obj = r.value().asObject();
    DOCTEST_CHECK(obj.has("theme"));
    DOCTEST_CHECK(obj.has("fontScalePercent"));
    DOCTEST_CHECK(obj.has("ok"));
    DOCTEST_CHECK_EQ(obj.get("theme").asString(), std::string("light"));
    DOCTEST_CHECK(obj.get("theme").isString());
    DOCTEST_CHECK(obj.get("fontScalePercent").isNumber());
    DOCTEST_CHECK_EQ(obj.get("fontScalePercent").asInt(), static_cast<int64_t>(115));
    DOCTEST_CHECK(obj.get("ok").isBool());
    DOCTEST_CHECK(obj.get("ok").asBool());
    DOCTEST_CHECK_FALSE(obj.has("nonexistent"));
    return true;
})

REGISTER_TEST(json_parse_invalid, "json: parse invalid text fails", {
    auto r = parse("{ this is not valid json !!!");
    DOCTEST_CHECK(r.isFail());
    return true;
})

REGISTER_TEST(json_stringify_object_roundtrip, "json: object stringify -> parse roundtrip", {
    JsonObject obj;
    obj.set("theme", makeString("dark"));
    obj.set("fontScalePercent", makeInt(100));
    obj.set("logRetentionDays", makeInt(30));

    auto s = stringify(JsonValue(obj));
    DOCTEST_REQUIRE(s.isOk());

    auto p = parse(s.value());
    DOCTEST_REQUIRE(p.isOk());
    DOCTEST_REQUIRE(p.value().isObject());
    JsonObject back = p.value().asObject();
    DOCTEST_CHECK_EQ(back.get("theme").asString(), std::string("dark"));
    DOCTEST_CHECK_EQ(back.get("fontScalePercent").asInt(), static_cast<int64_t>(100));
    DOCTEST_CHECK_EQ(back.get("logRetentionDays").asInt(), static_cast<int64_t>(30));
    return true;
})

// ---- U8 消费侧：序列化输出逐字节快照（2026-09-27，yyjson writer 形态锁定） ----
// 上游 U8 放开 yyjson writer 后，json.cpp 的手写序列化（implToString + escapeTo）已换成
// yyjson writer，本用例把切换后的输出字节锁住（防回退/防再漂移）。
// 与旧手写实现的差异只有两类，均为文本形态、语义等价（解析结果不变）：
//   ① double：旧 %.17g（512.0→"512"、1e17→"1e+17"、0.1→"0.10000000000000001"），
//      新 yyjson 最短形（512.0→"512.0"、1e17→"100000000000000000.0"、0.1→"0.1"）；
//   ② 控制字符转义十六进制大小写：旧 "\u000b" 小写，新 "\u000B" 大写。
// 其余形态逐字节一致（含 settings.json 形状 fixture）：两空格缩进、键按 std::map 序、
// ": " 分隔、空容器写 [] / {}、无尾换行、"/" 不转义、UTF-8 原样透传、整数/转义串。
// fixture 一律走 parse → stringify（settings.json 等持久化路径的真实形态）。
namespace {

// 任意字节串 → 可直接粘回 C++ 源码的字面量体（非可打印字节用三位八进制）
std::string jsonByteLiteral(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%03o", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

struct JsonByteSnapshot {
    const char* name;
    const char* input;   // 源 JSON 文本（parse 后序列化）
    const char* pretty;  // 期望的 pretty 输出字节（yyjson writer 形态）
    const char* compact; // 期望的 compact 输出字节（yyjson writer 形态）
};

} // namespace

REGISTER_TEST(json_stringify_bytes_snapshot, "json: stringify 输出逐字节快照（U8：yyjson writer 形态锁定）", {
    std::vector<JsonByteSnapshot> cases;
    cases.push_back({"null", "null", "null", "null"});
    cases.push_back({"bool_true", "true", "true", "true"});
    cases.push_back({"int_neg", "-42", "-42", "-42"});
    cases.push_back({"int_max", "9223372036854775807", "9223372036854775807", "9223372036854775807"});
    cases.push_back({"str_plain", "\"hi\"", "\"hi\"", "\"hi\""});
    cases.push_back({"str_empty", "\"\"", "\"\"", "\"\""});
    cases.push_back({"str_escapes", "\"a\\\"b\\\\c/d\\ne\\tf\"", "\"a\\\"b\\\\c/d\\ne\\tf\"",
                     "\"a\\\"b\\\\c/d\\ne\\tf\""});
    cases.push_back({"str_ctl", "\"\\u0001\\u000b\\u000f\\u001f x\"", "\"\\u0001\\u000B\\u000F\\u001F x\"",
                     "\"\\u0001\\u000B\\u000F\\u001F x\""});
    cases.push_back({"str_utf8", "\"\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80\"",
                     "\"\344\270\255\346\226\207 \360\237\230\200\"",
                     "\"\344\270\255\346\226\207 \360\237\230\200\""});
    cases.push_back({"dbl_frac", "0.1", "0.1", "0.1"});
    cases.push_back({"dbl_intlike", "512.0", "512.0", "512.0"});
    cases.push_back({"dbl_big", "1e17", "100000000000000000.0", "100000000000000000.0"});
    cases.push_back({"arr_empty", "[]", "[]", "[]"});
    cases.push_back({"obj_empty", "{}", "{}", "{}"});
    cases.push_back({"arr_nested", "[1,\"s\",null,true,[2,3],{\"k\":\"v\"}]",
                     "[\n  1,\n  \"s\",\n  null,\n  true,\n  [\n    2,\n    3\n  ],\n  {\n    \"k\": \"v\"\n  }\n]",
                     "[1,\"s\",null,true,[2,3],{\"k\":\"v\"}]"});
    cases.push_back({"obj_key_order", "{\"z\": 1, \"a\": 2, \"M\": 3}",
                     "{\n  \"M\": 3,\n  \"a\": 2,\n  \"z\": 1\n}",
                     "{\"M\":3,\"a\":2,\"z\":1}"});
    cases.push_back(
        {"settings_like",
         "{\"env\": {\"ANTHROPIC_BASE_URL\": \"http://127.0.0.1:8787\", \"ANTHROPIC_MODEL\": "
         "\"claude-sonnet-4-5-1m\"}, \"permissions\": {\"allow\": [\"Bash(git:*)\", \"Read\"], "
         "\"deny\": []}, \"theme\": \"light\", \"fontScalePercent\": 115, \"ok\": true}",
         "{\n  \"env\": {\n    \"ANTHROPIC_BASE_URL\": \"http://127.0.0.1:8787\",\n    \"ANTHROPIC_MODEL\": "
         "\"claude-sonnet-4-5-1m\"\n  },\n  \"fontScalePercent\": 115,\n  \"ok\": true,\n  \"permissions\": "
         "{\n    \"allow\": [\n      \"Bash(git:*)\",\n      \"Read\"\n    ],\n    \"deny\": []\n  },\n  "
         "\"theme\": \"light\"\n}",
         "{\"env\":{\"ANTHROPIC_BASE_URL\":\"http://127.0.0.1:8787\",\"ANTHROPIC_MODEL\":"
         "\"claude-sonnet-4-5-1m\"},\"fontScalePercent\":115,\"ok\":true,\"permissions\":{\"allow\":"
         "[\"Bash(git:*)\",\"Read\"],\"deny\":[]},\"theme\":\"light\"}"});
    cases.push_back(
        {"view_state_like",
         "{\"columnScrolls\": {\"7\": 512.0, \"10\": 150.25, \"11\": 0.0}, \"selectedTaskId\": 3}",
         "{\n  \"columnScrolls\": {\n    \"10\": 150.25,\n    \"11\": 0.0,\n    \"7\": 512.0\n  },\n  "
         "\"selectedTaskId\": 3\n}",
         "{\"columnScrolls\":{\"10\":150.25,\"11\":0.0,\"7\":512.0},\"selectedTaskId\":3}"});

    // 不在循环里逐条 FAIL（框架 fail-fast，只会暴露第一处）：先把全部差异打出来
    // （含可直接粘回本表的字面量），最后一次断言收口
    size_t bad = 0;
    for (const auto& c : cases) {
        auto parsed = parse(c.input);
        DOCTEST_REQUIRE(parsed.isOk());
        auto p = stringify(parsed.value(), /*pretty=*/true);
        auto k = stringify(parsed.value(), /*pretty=*/false);
        DOCTEST_REQUIRE(p.isOk());
        DOCTEST_REQUIRE(k.isOk());
        if (p.value() != c.pretty || k.value() != c.compact) {
            ++bad;
            std::printf("SNAPSHOT-MISMATCH %s\n    {\"%s\",\n     \"%s\",\n     \"%s\",\n     \"%s\"},\n",
                        c.name, c.name, c.input, jsonByteLiteral(p.value()).c_str(),
                        jsonByteLiteral(k.value()).c_str());
        }
    }
    DOCTEST_CHECK_EQ(bad, static_cast<size_t>(0));
    return true;
})