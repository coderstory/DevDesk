#include "json.h"

#include <map>
#include <sstream>
#include <vector>

namespace platform {
namespace json {

namespace {
    struct JsonImpl {
        enum class Kind { Null, Bool, Int, Double, String, Array, Object };
        Kind kind = Kind::Null;
        bool boolVal = false;
        int64_t intVal = 0;
        double dblVal = 0;
        std::string strVal;
        std::vector<JsonImpl> arrVal;
        std::map<std::string, JsonImpl> objVal;
    };

    JsonImpl* getImpl(void* p) { return static_cast<JsonImpl*>(p); }

    JsonValue makeFromImpl(JsonImpl impl) {
        JsonValue v;
        auto* dst = static_cast<JsonImpl*>(v.internal());
        *dst = std::move(impl);
        switch (dst->kind) {
            case JsonImpl::Kind::Null: v.type_ = JsonValue::T_Null; break;
            case JsonImpl::Kind::Bool: v.type_ = JsonValue::T_Bool; break;
            case JsonImpl::Kind::Int:
            case JsonImpl::Kind::Double: v.type_ = JsonValue::T_Number; break;
            case JsonImpl::Kind::String: v.type_ = JsonValue::T_String; break;
            case JsonImpl::Kind::Array: v.type_ = JsonValue::T_Array; break;
            case JsonImpl::Kind::Object: v.type_ = JsonValue::T_Object; break;
        }
        return v;
    }
}

// JsonObject
JsonObject::JsonObject() : data_(new JsonImpl()) {
    static_cast<JsonImpl*>(data_)->kind = JsonImpl::Kind::Object;
}
JsonObject::JsonObject(const JsonObject& o) : data_(new JsonImpl(*getImpl(o.data_))) {}
JsonObject::JsonObject(JsonObject&& o) noexcept : data_(o.data_) { o.data_ = new JsonImpl(); }
JsonObject& JsonObject::operator=(const JsonObject& o) {
    if (this != &o) {
        delete getImpl(data_);
        data_ = new JsonImpl(*getImpl(o.data_));
    }
    return *this;
}
JsonObject& JsonObject::operator=(JsonObject&& o) noexcept {
    if (this != &o) {
        delete getImpl(data_);
        data_ = o.data_;
        o.data_ = new JsonImpl();
    }
    return *this;
}
JsonObject::~JsonObject() { delete getImpl(data_); }

void JsonObject::set(const std::string& key, JsonValue v) {
    auto* impl = getImpl(data_);
    impl->kind = JsonImpl::Kind::Object;
    auto* vi = getImpl(v.internal());
    impl->objVal[key] = *vi;
}

bool JsonObject::has(const std::string& key) const {
    return getImpl(data_)->objVal.count(key) > 0;
}

JsonValue JsonObject::get(const std::string& key) const {
    JsonValue result;
    auto* impl = getImpl(data_);
    auto it = impl->objVal.find(key);
    if (it != impl->objVal.end()) {
        return makeFromImpl(it->second);
    }
    return result;
}

std::vector<std::string> JsonObject::keys() const {
    std::vector<std::string> result;
    for (const auto& [k, v] : getImpl(data_)->objVal) result.push_back(k);
    return result;
}

// JsonValue
JsonValue::JsonValue() : data_(new JsonImpl()) {
    static_cast<JsonImpl*>(data_)->kind = JsonImpl::Kind::Null;
}
JsonValue::JsonValue(std::nullptr_t) : JsonValue() {}
JsonValue::JsonValue(bool b) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Bool; i->boolVal = b;
    type_ = T_Bool;
}
JsonValue::JsonValue(int v) : JsonValue(static_cast<int64_t>(v)) {}
JsonValue::JsonValue(int64_t v) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Int; i->intVal = v;
    type_ = T_Number;
}
JsonValue::JsonValue(double d) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Double; i->dblVal = d;
    type_ = T_Number;
}
JsonValue::JsonValue(const std::string& s) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::String; i->strVal = s;
    type_ = T_String;
}
JsonValue::JsonValue(const char* s) : JsonValue(std::string(s)) {}
JsonValue::JsonValue(JsonObject obj) : data_(new JsonImpl(*getImpl(obj.internal()))) {
    static_cast<JsonImpl*>(data_)->kind = JsonImpl::Kind::Object;
    type_ = T_Object;
}

JsonValue::JsonValue(const JsonValue& o) : data_(new JsonImpl(*getImpl(o.data_))), type_(o.type_) {}
JsonValue& JsonValue::operator=(const JsonValue& o) {
    if (this != &o) {
        delete getImpl(data_);
        data_ = new JsonImpl(*getImpl(o.data_));
        type_ = o.type_;
    }
    return *this;
}
JsonValue::JsonValue(JsonValue&& o) noexcept : data_(o.data_), type_(o.type_) { o.data_ = new JsonImpl(); o.type_ = T_Null; }
JsonValue& JsonValue::operator=(JsonValue&& o) noexcept {
    if (this != &o) {
        delete getImpl(data_);
        data_ = o.data_;
        type_ = o.type_;
        o.data_ = new JsonImpl();
        o.type_ = T_Null;
    }
    return *this;
}
JsonValue::~JsonValue() { delete getImpl(data_); }

JsonObject JsonValue::asObject() const {
    JsonObject obj;
    auto* dst = getImpl(obj.internal());
    *dst = *getImpl(data_);
    dst->kind = JsonImpl::Kind::Object;
    return obj;
}

// 工厂
JsonValue makeNull() { return JsonValue(nullptr); }
JsonValue makeBool(bool b) { return JsonValue(b); }
JsonValue makeInt(int64_t i) { return JsonValue(i); }
JsonValue makeDouble(double d) { return JsonValue(d); }
JsonValue makeString(const std::string& s) { return JsonValue(s); }
JsonValue makeObject() { return JsonValue(JsonObject()); }
JsonValue makeArray() { JsonValue v; return v; }

// 解析（简化版 - 实际应使用 yyjson）
core::Result<JsonValue> parse(const std::string& text) {
    (void)text;
    return core::Result<JsonValue>::ok(JsonValue());
}

core::Result<std::string> stringify(const JsonValue& value, bool pretty) {
    (void)pretty;
    auto* i = getImpl(value.internal());
    std::string out;
    switch (i->kind) {
        case JsonImpl::Kind::Null: out = "null"; break;
        case JsonImpl::Kind::Bool: out = i->boolVal ? "true" : "false"; break;
        case JsonImpl::Kind::Int: out = std::to_string(i->intVal); break;
        case JsonImpl::Kind::Double: out = std::to_string(i->dblVal); break;
        case JsonImpl::Kind::String: out = "\"" + i->strVal + "\""; break;
        default: out = "{}"; break;
    }
    return core::Result<std::string>::ok(std::move(out));
}

} // namespace json
} // namespace platform