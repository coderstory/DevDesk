// src/platform/json.h
// yyjson 薄封装（详见 spec §7.4）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform {
namespace json {

class JsonObject;
class JsonValue;

// 拷贝语义（审计 2026-09-27 L6）：本类是 yyjson 子树的**深拷贝值类型**——
// 无引用计数 / 无 COW，拷贝构造与赋值都复制整棵子树（json.cpp:186-206）。
// 代价：大 JSON 上按值传参/返回 = 整树复制；在循环里对同一棵大树反复按值取值
//（get() / asObject()）每次都是一次子树深拷贝 → 容易累积成 O(n²) 拷贝链。
// 当前调用点规模小（SSE 事件、配置）；用户可粘贴导入的路径已加输入长度上限
//（module_claude_proxy/provider_config.h 的 kMaxImportTextBytes）。
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

    // 类型与取值一律从堆上的 JsonImpl 派生（唯一真值源，见 json.cpp 顶部）
    // CP-02（2026-09-27 全量审计，high）：此前 JsonValue 另存一份 public 镜像字段
    // （type_/boolVal_/intVal_/dblVal_/strVal_），但 4 个特殊成员只搬 data_ → 左值
    // 拷贝/移动后 asString()/asInt() 静默返回默认值。删掉镜像后不存在两份真值，
    // 也就不存在"漏同步"这一种漏法。
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

    // 数组访问（模块插拔 enabledModules 首个消费者；仅支持读，元素按值返回 =
    // 该元素子树深拷贝——大数组上避免在循环里反复取）
    int size() const;
    JsonValue operator[](int index) const;

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
// 字符串数组工厂（应用配置的 enabledModules；此前 makeArray 是空桩）
JsonValue makeStringArray(const std::vector<std::string>& items);

} // namespace json
} // namespace platform