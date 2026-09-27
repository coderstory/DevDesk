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

    // JsonImpl::Kind → JsonValue::Type（对外类型由 kind 派生，见 CP-02 注释）
    JsonValue::Type kindToType(JsonImpl::Kind kind) {
        switch (kind) {
            case JsonImpl::Kind::Null: return JsonValue::T_Null;
            case JsonImpl::Kind::Bool: return JsonValue::T_Bool;
            case JsonImpl::Kind::Int:
            case JsonImpl::Kind::Double: return JsonValue::T_Number;
            case JsonImpl::Kind::String: return JsonValue::T_String;
            case JsonImpl::Kind::Array: return JsonValue::T_Array;
            case JsonImpl::Kind::Object: return JsonValue::T_Object;
        }
        return JsonValue::T_Null;  // 枚举全覆盖，仅为消除 -Wreturn-type
    }

    // 唯一的值装配入口：把 JsonImpl 子树搬进新 JsonValue（镜像字段已删，
    // 这里就是"值"本身，无需再同步任何副本）
    JsonValue makeFromImpl(JsonImpl impl) {
        JsonValue v;
        *getImpl(v.internal()) = std::move(impl);
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
JsonValue::JsonValue(bool b) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Bool; i->boolVal = b;
}
JsonValue::JsonValue(int v) : JsonValue(static_cast<int64_t>(v)) {}
JsonValue::JsonValue(int64_t v) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Int; i->intVal = v;
}
JsonValue::JsonValue(double d) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::Double; i->dblVal = d;
}
JsonValue::JsonValue(const std::string& s) : data_(new JsonImpl()) {
    auto* i = static_cast<JsonImpl*>(data_); i->kind = JsonImpl::Kind::String; i->strVal = s;
}
JsonValue::JsonValue(const char* s) : JsonValue(std::string(s)) {}
JsonValue::JsonValue(JsonObject obj) : data_(new JsonImpl(*getImpl(obj.internal()))) {
    // 强制 Object：被移动过的 JsonObject 持有的空 JsonImpl kind 是 Null，
    // 但 JsonValue(JsonObject) 的类型契约始终是 T_Object（与旧镜像语义一致）
    static_cast<JsonImpl*>(data_)->kind = JsonImpl::Kind::Object;
}

// CP-02（2026-09-27 全量审计，high）：4 个特殊成员只需搬 data_ —— 值与类型全部
// 存放在 JsonImpl 里（唯一真值源），不再有需要逐个同步的镜像字段。此前漏同步
// 镜像会让左值拷贝/移动后的 asString()/asInt() 静默返回默认值。
JsonValue::JsonValue(const JsonValue& o) : data_(new JsonImpl(*getImpl(o.data_))) {}
JsonValue& JsonValue::operator=(const JsonValue& o) {
    if (this != &o) {
        delete getImpl(data_);
        data_ = new JsonImpl(*getImpl(o.data_));
    }
    return *this;
}
// 被移动的源对象换上一份新的空 JsonImpl（保持合法 Null 值，不留悬垂/旧值）
JsonValue::JsonValue(JsonValue&& o) noexcept : data_(o.data_) { o.data_ = new JsonImpl(); }
JsonValue& JsonValue::operator=(JsonValue&& o) noexcept {
    if (this != &o) {
        delete getImpl(data_);
        data_ = o.data_;
        o.data_ = new JsonImpl();
    }
    return *this;
}
JsonValue::~JsonValue() { delete getImpl(data_); }

// 类型与取值（读 JsonImpl；见 json.h 的 CP-02 注释）
JsonValue::Type JsonValue::type() const { return kindToType(getImpl(data_)->kind); }
bool JsonValue::asBool() const { return getImpl(data_)->boolVal; }
int64_t JsonValue::asInt() const { return getImpl(data_)->intVal; }
double JsonValue::asDouble() const { return getImpl(data_)->dblVal; }
std::string JsonValue::asString() const { return getImpl(data_)->strVal; }

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