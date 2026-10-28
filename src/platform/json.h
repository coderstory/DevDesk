// src/platform/json.h
// yyjson 薄封装。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform {
namespace json {

class JsonObject;
class JsonValue;

// 深拷贝值类型：无引用计数/无 COW，拷贝构造与赋值复制整棵子树。大 JSON 按值传参/返回 = 整树复制，
// 循环里对大树反复取值易累积成 O(n²)；导入路径已加输入长度上限（provider_config.h 的 kMaxImportTextBytes）。
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
    JsonValue get(const std::string& key) const;  // 按值 = 该键子树深拷贝
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

    JsonValue(const JsonValue& other);  // 深拷贝（同 JsonObject，含整棵子对象）
    JsonValue& operator=(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(JsonValue&& other) noexcept;
    ~JsonValue();

    // 类型与取值一律从堆上的 JsonImpl 派生（唯一真值源）；特殊成员只搬 data_，不可另存镜像字段。
    Type type() const;
    bool isNull() const { return type() == T_Null; }
    bool isBool() const { return type() == T_Bool; }
    bool isNumber() const { return type() == T_Number; }
    bool isString() const { return type() == T_String; }
    bool isObject() const { return type() == T_Object; }
    bool isArray() const { return type() == T_Array; }

    bool asBool() const;
    int64_t asInt() const;
    double asDouble() const;
    std::string asString() const;
    JsonObject asObject() const;  // 按值 = 整对象子树深拷贝（同 JsonObject）

    // 数组访问（仅支持读，元素按值返回 = 该元素子树深拷贝；大数组避免循环里反复取）
    int size() const;
    JsonValue operator[](int index) const;

    // 数组尾部追加（元素深拷贝）。非数组调用 = no-op（不转型；先 makeArray 再 append）
    void append(const JsonValue& value);

    void* internal() const { return data_; }

private:
    void* data_ = nullptr;
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
// 字符串数组工厂（应用配置 enabledModules）
JsonValue makeStringArray(const std::vector<std::string>& items);

} // namespace json
} // namespace platform