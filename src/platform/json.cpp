#include "json.h"

#include <yyjson.h>

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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
        v.type_ = JsonValue::T_Null;
        auto* dst = static_cast<JsonImpl*>(v.internal());
        *dst = std::move(impl);
        // 同步拷贝值字段（strVal_/intVal_/dblVal_/boolVal_）到 JsonValue 自身
        v.boolVal_ = dst->boolVal;
        v.intVal_ = dst->intVal;
        v.dblVal_ = dst->dblVal;
        v.strVal_ = dst->strVal;
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

    // yyjson_val → JsonImpl（递归；数组/对象完整还原）
    // 注意：yyjson 读 API 不接收 const 指针（上游 const 语义不完整），此处不 const 化
    JsonImpl valToImpl(yyjson_val* val) {
        JsonImpl out;
        if (yyjson_is_null(val)) {
            out.kind = JsonImpl::Kind::Null;
        } else if (yyjson_is_bool(val)) {
            out.kind = JsonImpl::Kind::Bool;
            out.boolVal = yyjson_get_bool(val);
        } else if (yyjson_is_int(val)) {
            out.kind = JsonImpl::Kind::Int;
            out.intVal = yyjson_get_sint(val);
        } else if (yyjson_is_num(val)) {
            out.kind = JsonImpl::Kind::Double;
            out.dblVal = yyjson_get_real(val);
        } else if (yyjson_is_str(val)) {
            out.kind = JsonImpl::Kind::String;
            out.strVal.assign(yyjson_get_str(val), yyjson_get_len(val));
        } else if (yyjson_is_arr(val)) {
            out.kind = JsonImpl::Kind::Array;
            size_t idx = 0;
            size_t max = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(val, idx, max, item) {
                out.arrVal.push_back(valToImpl(item));
            }
        } else if (yyjson_is_obj(val)) {
            out.kind = JsonImpl::Kind::Object;
            size_t idx = 0;
            size_t max = 0;
            yyjson_val* key = nullptr;
            yyjson_val* v = nullptr;
            yyjson_obj_foreach(val, idx, max, key, v) {
                out.objVal[std::string(yyjson_get_str(key),
                                       yyjson_get_len(key))] = valToImpl(v);
            }
        }
        return out;
    }

    // JsonImpl 字符串转义（RFC 8259：引号/反斜杠/控制字符；UTF-8 原样透传）
    void escapeTo(const std::string& s, std::string& out) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned>(c) & 0xFFu);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }

    void indentTo(std::string& out, int depth) {
        out.append(static_cast<size_t>(depth) * 2u, ' ');
    }

    // JsonImpl → JSON 文本（递归）
    // 注意：yyjson 库被 EUI-NEO 以 YYJSON_DISABLE_WRITER=1 编译（共享库无 writer），
    // 序列化自实现；DevDesk 场景（扁平对象 + 标量）足够，双精度用 %.17g 保证往返
    void implToString(const JsonImpl& impl, std::string& out, bool pretty,
                      int depth) {
        switch (impl.kind) {
            case JsonImpl::Kind::Null:
                out += "null";
                break;
            case JsonImpl::Kind::Bool:
                out += impl.boolVal ? "true" : "false";
                break;
            case JsonImpl::Kind::Int:
                out += std::to_string(impl.intVal);
                break;
            case JsonImpl::Kind::Double: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", impl.dblVal);
                out += buf;
                break;
            }
            case JsonImpl::Kind::String:
                escapeTo(impl.strVal, out);
                break;
            case JsonImpl::Kind::Array: {
                if (impl.arrVal.empty()) {
                    out += "[]";
                    break;
                }
                out += pretty ? "[\n" : "[";
                for (size_t i = 0; i < impl.arrVal.size(); ++i) {
                    if (i > 0) out += pretty ? ",\n" : ",";
                    if (pretty) indentTo(out, depth + 1);
                    implToString(impl.arrVal[i], out, pretty, depth + 1);
                }
                out += pretty ? "\n" : "";
                if (pretty) indentTo(out, depth);
                out += "]";
                break;
            }
            case JsonImpl::Kind::Object: {
                if (impl.objVal.empty()) {
                    out += "{}";
                    break;
                }
                out += pretty ? "{\n" : "{";
                bool first = true;
                for (const auto& [k, v] : impl.objVal) {
                    if (!first) out += pretty ? ",\n" : ",";
                    first = false;
                    if (pretty) indentTo(out, depth + 1);
                    escapeTo(k, out);
                    out += pretty ? ": " : ":";
                    implToString(v, out, pretty, depth + 1);
                }
                out += pretty ? "\n" : "";
                if (pretty) indentTo(out, depth);
                out += "}";
                break;
            }
        }
    }
} // anonymous

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
JsonValue::JsonValue(bool b) : data_(new JsonImpl()), boolVal_(b) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Bool; i->boolVal = b;
    type_ = T_Bool;
}
JsonValue::JsonValue(int v) : JsonValue(static_cast<int64_t>(v)) {}
JsonValue::JsonValue(int64_t v) : data_(new JsonImpl()), intVal_(v) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Int; i->intVal = v;
    type_ = T_Number;
}
JsonValue::JsonValue(double d) : data_(new JsonImpl()), dblVal_(d) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Double; i->dblVal = d;
    type_ = T_Number;
}
JsonValue::JsonValue(const std::string& s) : data_(new JsonImpl()), strVal_(s) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::String; i->strVal = s;
    type_ = T_String;
}
JsonValue::JsonValue(const char* s) : JsonValue(std::string(s)) {}
JsonValue::JsonValue(JsonObject obj) : data_(new JsonImpl(*getImpl(obj.internal()))) {
    static_cast<JsonImpl*>(data_)->kind = JsonImpl::Kind::Object;
    type_ = T_Object;
}

// 成员初始化顺序必须与声明顺序一致（type_ 在前，data_ 在后），否则 -Werror=reorder
JsonValue::JsonValue(const JsonValue& o) : type_(o.type_), data_(new JsonImpl(*getImpl(o.data_))) {}
JsonValue& JsonValue::operator=(const JsonValue& o) {
    if (this != &o) {
        delete getImpl(data_);
        data_ = new JsonImpl(*getImpl(o.data_));
        type_ = o.type_;
    }
    return *this;
}
JsonValue::JsonValue(JsonValue&& o) noexcept : type_(o.type_), data_(o.data_) { o.data_ = new JsonImpl(); o.type_ = T_Null; }
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

int JsonValue::size() const {
    const auto* impl = getImpl(data_);
    return impl->kind == JsonImpl::Kind::Array
               ? static_cast<int>(impl->arrVal.size())
               : 0;
}

JsonValue JsonValue::operator[](int index) const {
    const auto* impl = getImpl(data_);
    if (impl->kind == JsonImpl::Kind::Array && index >= 0 &&
        static_cast<size_t>(index) < impl->arrVal.size()) {
        return makeFromImpl(impl->arrVal[static_cast<size_t>(index)]);
    }
    return JsonValue();
}

// 工厂
JsonValue makeNull() { return JsonValue(nullptr); }
JsonValue makeBool(bool b) { return JsonValue(b); }
JsonValue makeInt(int64_t i) { return JsonValue(i); }
JsonValue makeDouble(double d) { return JsonValue(d); }
JsonValue makeString(const std::string& s) { return JsonValue(s); }
JsonValue makeObject() { return JsonValue(JsonObject()); }
JsonValue makeArray() { JsonValue v; return v; }

JsonValue makeStringArray(const std::vector<std::string>& items) {
    JsonValue v;
    auto* impl = getImpl(v.internal());
    impl->kind = JsonImpl::Kind::Array;
    impl->arrVal.reserve(items.size());
    for (const auto& s : items) {
        JsonImpl item;
        item.kind = JsonImpl::Kind::String;
        item.strVal = s;
        impl->arrVal.push_back(std::move(item));
    }
    v.type_ = JsonValue::T_Array;
    return v;
}

// 解析（yyjson；T11 实装——此前是返回 null 的桩，config.json 持久化首个消费者）
core::Result<JsonValue> parse(const std::string& text) {
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) {
        return core::Result<JsonValue>::fail(
            core::ErrorCode::JsonParseError, "JSON 解析失败（yyjson_read 失败）");
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    JsonValue out;
    if (root != nullptr) {
        out = makeFromImpl(valToImpl(root));
    }  // 空文档 → 保持默认 null
    yyjson_doc_free(doc);
    return core::Result<JsonValue>::ok(std::move(out));
}

// 序列化（自实现，见 implToString 注释——yyjson 共享库无 writer）
core::Result<std::string> stringify(const JsonValue& value, bool pretty) {
    std::string out;
    out.reserve(64);
    implToString(*getImpl(value.internal()), out, pretty, 0);
    return core::Result<std::string>::ok(std::move(out));
}

} // namespace json
} // namespace platform