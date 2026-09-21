// src/platform/json.h
// yyjson 薄封装（详见 spec §7.4）
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform {
namespace json {

class JsonObject;
class JsonValue;

class JsonObject {
public:
    JsonObject();
    JsonObject(const JsonObject& other);
    JsonObject& operator=(const JsonObject& other);
    JsonObject(JsonObject&& other) noexcept;
    JsonObject& operator=(JsonObject&& other) noexcept;
    ~JsonObject();

    void set(const std::string& key, JsonValue value);
    bool has(const std::string& key) const;
    JsonValue get(const std::string& key) const;
    std::vector<std::string> keys() const;

    void* internal() const { return data_; }

private:
    void* data_ = nullptr;
};

class JsonValue {
public:
    enum Type { T_Null, T_Bool, T_Number, T_String, T_Array, T_Object };

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool b);
    JsonValue(int i);
    JsonValue(int64_t i);
    JsonValue(double d);
    JsonValue(const std::string& s);
    JsonValue(const char* s);
    JsonValue(JsonObject obj);

    JsonValue(const JsonValue& other);
    JsonValue& operator=(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(JsonValue&& other) noexcept;
    ~JsonValue();

    Type type() const { return type_; }
    bool isNull() const { return type_ == T_Null; }
    bool isBool() const { return type_ == T_Bool; }
    bool isNumber() const { return type_ == T_Number; }
    bool isString() const { return type_ == T_String; }
    bool isObject() const { return type_ == T_Object; }
    bool isArray() const { return type_ == T_Array; }

    bool asBool() const { return boolVal_; }
    int64_t asInt() const { return intVal_; }
    double asDouble() const { return dblVal_; }
    std::string asString() const { return strVal_; }
    JsonObject asObject() const;

    int size() const { return 0; }  // 数组简化
    JsonValue operator[](int) const { return JsonValue(); }

    void* internal() const { return data_; }

    // 类型和值（公开，便于 makeFromImpl 等内部函数访问）
    Type type_ = T_Null;
    void* data_ = nullptr;
    bool boolVal_ = false;
    int64_t intVal_ = 0;
    double dblVal_ = 0;
    std::string strVal_;
};

// 解析
core::Result<JsonValue> parse(const std::string& text);

// 序列化
core::Result<std::string> stringify(const JsonValue& value, bool pretty = false);

// 工厂函数
JsonValue makeNull();
JsonValue makeBool(bool b);
JsonValue makeInt(int64_t i);
JsonValue makeDouble(double d);
JsonValue makeString(const std::string& s);
JsonValue makeObject();
JsonValue makeArray();

} // namespace json
} // namespace platform