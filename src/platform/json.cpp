#include "json.h"

#include <yyjson.h>

#include <cstdlib>
#include <map>
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

    // JsonImpl → yyjson 可变文档（写侧）。键序照 std::map 序（= 旧手写实现的输出序），
    // 缩进/分隔交给 yyjson 的 YYJSON_WRITE_PRETTY_TWO_SPACES。
    // 与旧手写实现（implToString + escapeTo，2026-09-27 删除）的字节差异只有两类，
    // 均为文本形态、语义等价（解析结果不变）：
    //   ① double：旧用 %.17g（512.0→"512"、1e17→"1e+17"），新用 yyjson 最短形
    //      （512.0→"512.0"、1e17→"100000000000000000.0"、0.1→"0.1"）；
    //   ② 控制字符转义十六进制大小写：旧 "\u000b" 小写，新 "\u000B" 大写。
    // 其余形态（两空格缩进 / 键序 / ": " 分隔 / 空容器 [] {} / 无尾换行 / "/" 不转义 /
    // UTF-8 原样透传 / 整数）逐字节一致；期望值锁在 tests/test_platform/test_json.cpp
    // 的 json_stringify_bytes_snapshot 用例（U8 消费侧）。
    yyjson_mut_val* implToMut(yyjson_mut_doc* doc, const JsonImpl& impl) {
        switch (impl.kind) {
            case JsonImpl::Kind::Null:
                return yyjson_mut_null(doc);
            case JsonImpl::Kind::Bool:
                return yyjson_mut_bool(doc, impl.boolVal);
            case JsonImpl::Kind::Int:
                return yyjson_mut_sint(doc, impl.intVal);
            case JsonImpl::Kind::Double:
                return yyjson_mut_real(doc, impl.dblVal);
            case JsonImpl::Kind::String:
                return yyjson_mut_strncpy(doc, impl.strVal.data(), impl.strVal.size());
            case JsonImpl::Kind::Array: {
                yyjson_mut_val* arr = yyjson_mut_arr(doc);
                if (arr == nullptr) return nullptr;
                for (const auto& item : impl.arrVal) {
                    yyjson_mut_val* child = implToMut(doc, item);
                    if (child == nullptr || !yyjson_mut_arr_add_val(arr, child)) {
                        return nullptr;
                    }
                }
                return arr;
            }
            case JsonImpl::Kind::Object: {
                yyjson_mut_val* obj = yyjson_mut_obj(doc);
                if (obj == nullptr) return nullptr;
                for (const auto& [k, v] : impl.objVal) {
                    yyjson_mut_val* child = implToMut(doc, v);
                    if (child == nullptr ||
                        !yyjson_mut_obj_add_val(doc, obj, k.c_str(), child)) {
                        return nullptr;
                    }
                }
                return obj;
            }
        }
        return nullptr;  // 枚举全覆盖，仅为消除 -Wreturn-type
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

// 解析（yyjson；T11 实装——此前是返回 null 的桩，应用配置持久化首个消费者）
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

// 序列化（yyjson writer；上游 U8 已放开 YYJSON_DISABLE_WRITER，DevDesk 不再自实现。
// 与旧手写实现的字节差异、以及锁定的期望值见 implToMut 注释与
// json_stringify_bytes_snapshot 用例）
core::Result<std::string> stringify(const JsonValue& value, bool pretty) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (doc == nullptr) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "JSON 序列化失败（yyjson 文档创建失败）");
    }
    yyjson_mut_val* root = implToMut(doc, *getImpl(value.internal()));
    if (root == nullptr) {
        yyjson_mut_doc_free(doc);
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "JSON 序列化失败（yyjson 节点构造失败）");
    }
    yyjson_mut_doc_set_root(doc, root);
    // ALLOW_INVALID_UNICODE / ALLOW_INF_AND_NAN：与旧实现的"原样透传"语义对齐，
    // 不让此前能成功序列化的值变成失败（非法字节转义为替换字符、inf/nan 写
    // Infinity/NaN，旧实现分别写原字节与 inf/nan——均非 DevDesk 读写路径的形状）
    yyjson_write_flag flags =
        YYJSON_WRITE_ALLOW_INVALID_UNICODE | YYJSON_WRITE_ALLOW_INF_AND_NAN;
    if (pretty) flags |= YYJSON_WRITE_PRETTY_TWO_SPACES;  // 两空格缩进 = 旧实现缩进
    yyjson_write_err err{};
    size_t len = 0;
    char* text = yyjson_mut_write_opts(doc, flags, nullptr, &len, &err);
    yyjson_mut_doc_free(doc);
    if (text == nullptr) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            std::string("JSON 序列化失败（yyjson_mut_write: ") +
                (err.msg != nullptr ? err.msg : "unknown") + "）");
    }
    std::string out(text, len);
    std::free(text);
    return core::Result<std::string>::ok(std::move(out));
}

} // namespace json
} // namespace platform