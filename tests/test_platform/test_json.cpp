#include "doctest.h"
#include "platform/json.h"

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