// src/module_claude_proxy/json_scan.h
// 顶层 JSON 字段原始字节区间扫描：只定位「顶层 key → 值的字节区间」，不做类型
// 判定、不做全量反序列化。消费方：streaming_proxy::wantsStream、upstream_dialect
// 的 [1M] 定点替换。全 inline：多 TU。
#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace module::claude::json_scan {

// 跳过 JSON 空白
inline void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

// 从 s[i]（应指向 '"'）读出一个 JSON 字符串，返回其内容并推进 i
// 简单实现：不支持 \uXXXX 转义（对检测 key 名足够）
inline std::string readJsonString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i;  // 跳过开引号
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') {
            if (i + 1 < s.size()) {
                out.push_back(s[i + 1]);
                i += 2;
                continue;
            }
            break;
        }
        if (c == '"') {
            ++i;  // 跳过闭引号
            break;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// 跳过 JSON 值（用于跨过我们不需要的字段）
// 返回 false 表示解析到非法字符
inline bool skipJsonValue(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '"') {
        readJsonString(s, i);
        return true;
    }
    if (c == '{' || c == '[') {
        const char open = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            const char x = s[i];
            if (x == '"') {
                readJsonString(s, i);
                continue;
            }
            if (x == open) {
                ++depth;
            } else if (x == close) {
                --depth;
                ++i;
                if (depth == 0) return true;
                continue;
            }
            ++i;
        }
        return false;
    }
    // null / true / false / 数字：吃到分隔符
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
        ++i;
    }
    return true;
}

// 在顶层对象里找 key，回填其值的原始字节区间 [begin, end)（begin 已跳 ':' 后空白）。
// 不做类型判定（由调用方读区间自决）；找不到 / 结构非法 → false（begin/end 不写）。
inline bool findTopLevelValueSpan(const std::string& s, const std::string& key,
                                  size_t& begin, size_t& end) {
    size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;  // 进入对象

    int depth = 1;
    while (i < s.size()) {
        skipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] == '}') {
            --depth;
            ++i;
            if (depth == 0) return false;
            continue;
        }
        if (s[i] != '"') {
            ++i;  // 非预期 token：跳过，防死循环
            continue;
        }

        const std::string fieldKey = readJsonString(s, i);
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') {
            if (i < s.size()) ++i;  // 非法结构：跳过
            continue;
        }
        ++i;  // ':'
        skipWs(s, i);

        if (depth == 1 && fieldKey == key) {
            const size_t valueBegin = i;
            if (!skipJsonValue(s, i)) return false;
            begin = valueBegin;
            end = i;
            return true;
        }
        if (!skipJsonValue(s, i)) return false;
    }
    return false;
}

// 去掉字符串首尾空白
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace module::claude::json_scan
