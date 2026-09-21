#include "doctest.h"
#include "platform/json.h"

using namespace platform::json;

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