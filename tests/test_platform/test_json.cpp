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