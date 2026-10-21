// agent_builtin_tools.cpp：智能体内置工具 14 件（UAT6 特性批）。
// fs 五件（glob / grep / read / write / edit）+ bash 一件 + git 八件
// （clone / status / log / diff / branch / checkout / pull / commit）。
// 全部工具按 corer::ITool 接口实现：name() / schemaJson() / execute()。
// 所有工具无状态 standalone，注册进进程级 ToolRegistry 单例后永不注销。
//
// 契约：工具级失败一律 Result::ok + isError=true + 中文说明（LLM 可读）；
// Result::fail 只留给编程不变量破坏（与 app_now / net_fetch 同口径）。
//
// 依赖裁剪：不依赖 eui/（不碰 UI），不依赖 module_* 兄弟模块（模块隔离铁律）。
// platform 层依赖（process_runner / json / utf8）全经头文件声明。
#include "agent_builtin_tools.h"

#include "core/logging.h"
#include "core/tool.h"
#include "platform/json.h"
#include "platform/process_runner.h"
#include "platform/utf8.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace module::agent {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;
namespace fs = std::filesystem;

// ── 通用 helper ──

// 参数解析：从 ToolArgs 的 JSON 对象中取字符串字段
pj::JsonObject parseArgs(const std::string& json) {
    if (json.empty()) return pj::JsonObject();
    auto parsed = pj::parse(json);
    if (parsed.isFail() || !parsed.value().isObject()) return pj::JsonObject();
    return parsed.value().asObject();
}

std::string optString(const pj::JsonObject& obj, const char* key,
                      const std::string& defaultVal = "") {
    auto v = obj.get(key);
    return v.isString() ? v.asString() : defaultVal;
}

int64_t optInt(const pj::JsonObject& obj, const char* key, int64_t defaultVal = 0) {
    auto v = obj.get(key);
    return v.isNumber() ? v.asInt() : defaultVal;
}

bool optBool(const pj::JsonObject& obj, const char* key, bool defaultVal = false) {
    auto v = obj.get(key);
    return v.isBool() ? v.asBool() : defaultVal;
}

// 返回 JSON 字符串结果（isError=false 即成功内容）
Result<ToolResult> okJson(const std::string& json) {
    return Result<ToolResult>::ok(ToolResult{false, json});
}

// 返回纯文本结果（自动 JSON 字符串化）
Result<ToolResult> okText(const std::string& text) {
    auto encoded = pj::stringify(pj::makeString(text));
    if (encoded.isFail()) {
        return Result<ToolResult>::fail(
            core::ErrorCode::Internal,
            "工具结果序列化失败: " + encoded.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{false, encoded.value()});
}

// 返回错误结果（isError=true，LLM 可读）
Result<ToolResult> errText(const std::string& text) {
    auto encoded = pj::stringify(pj::makeString(text));
    if (encoded.isFail()) {
        return Result<ToolResult>::fail(
            core::ErrorCode::Internal,
            "工具错误文本序列化失败: " + encoded.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, encoded.value()});
}

// ── 路径安全校验：拒绝空串、".." 段、绝对路径与盘符（工具参数里 cwd 是调用方控制的上下文，
// 路径由 LLM 构造，不能放任意绝对路径读写——文件面五工具适用）──
bool isSafeRelative(const std::string& path) {
    // 空串 → 拒绝
    if (path.empty()) return false;
    // 绝对路径（Unix / 或 Windows C:\ 或 \\）→ 拒绝
    if (path[0] == '/' || path[0] == '\\') return false;
    if (path.size() >= 2 && path[1] == ':') return false;
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return false;
    // ".." 段 → 拒绝
    if (path.find("..") != std::string::npos) return false;
    return true;
}

// 工具执行工作目录：application 当前路径
fs::path toolCwd() {
    return fs::current_path();
}

// 将 glob pattern 转成正则表达式（支持 ** / * / ? / {a,b}）
std::string globToRegex(const std::string& pattern) {
    std::string s;
    size_t i = 0;

    while (i < pattern.size()) {
        if (i + 1 < pattern.size() && pattern[i] == '*' && pattern[i + 1] == '*') {
            // ** 递归匹配（包括路径分隔符）
            size_t j = i + 2;
            /* 跳过后续的 / 或 \ */
            if (j < pattern.size() && (pattern[j] == '/' || pattern[j] == '\\')) ++j;
            s += "(.*)";
            i = j;
        } else if (pattern[i] == '*') {
            // *：匹配文件名级（不含 / \）
            s += "([^/\\\\]*)";
            ++i;
        } else if (pattern[i] == '?') {
            // ?：单字符（不含 / \）
            s += "([^/\\\\])";
            ++i;
        } else if (pattern[i] == '{') {
            // {a,b}：大括号展开
            size_t close = pattern.find('}', i + 1);
            if (close == std::string::npos) {
                s += "\\{";
                ++i;
            } else {
                std::string inner = pattern.substr(i + 1, close - i - 1);
                s += '(';
                // 逗号分隔
                size_t start = 0;
                bool firstAlt = true;
                while (start <= inner.size()) {
                    size_t comma = inner.find(',', start);
                    if (comma == std::string::npos) comma = inner.size();
                    std::string alt = inner.substr(start, comma - start);
                    if (!firstAlt) s += '|';
                    // 对每个备选项内部再做一次转义
                    for (char c : alt) {
                        if (c == '.' || c == '^' || c == '$' || c == '+' ||
                            c == '(' || c == ')' || c == '[' || c == ']' ||
                            c == '|' || c == '\\') {
                            s += '\\';
                        }
                        s += c;
                    }
                    firstAlt = false;
                    start = comma + 1;
                }
                s += ')';
                i = close + 1;
            }
        } else if (pattern[i] == '.') {
            s += "\\.";
            ++i;
        } else if (pattern[i] == '\\' || pattern[i] == '^' || pattern[i] == '$' ||
                   pattern[i] == '+' || pattern[i] == '(' || pattern[i] == ')' ||
                   pattern[i] == '[' || pattern[i] == ']' || pattern[i] == '|') {
            s += '\\';
            s += pattern[i];
            ++i;
        } else {
            s += pattern[i];
            ++i;
        }
    }
    return "^" + s + "$";
}

// ========================================================================
// 1. fs.glob
// ========================================================================

class FsGlobTool : public core::ITool {
public:
    std::string name() const override { return "fs.glob"; }

    std::string schemaJson() const override {
        return R"({"name":"fs.glob","description":"文件通配搜索：按 glob 模式匹配工作目录下的文件路径（** 递归、* 匹配文件名不含路径分隔符、? 匹配单字符、{a,b} 大括号展开），按修改时间降序排列，上限 1000 条","input_schema":{"type":"object","properties":{"pattern":{"type":"string","description":"glob 模式，如 \"**/*.cpp\"、\"src/*.{h,cpp}\""},"path":{"type":"string","description":"搜索起始目录，相对于当前工作目录，默认当前工作目录"}},"required":["pattern"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string pattern = optString(obj, "pattern");
        if (pattern.empty()) {
            return errText("fs.glob 参数错误：缺少必填参数 pattern（glob 模式字符串）");
        }
        std::string relPath = optString(obj, "path");
        if (!relPath.empty() && !isSafeRelative(relPath)) {
            return errText("fs.glob 参数错误：path 含不安全路径段（绝对路径或 ..）——" + relPath);
        }

        fs::path baseDir = relPath.empty() ? toolCwd() : toolCwd() / fs::path(relPath);
        std::error_code ec;
        if (!fs::exists(baseDir, ec)) {
            return errText("fs.glob 错误：路径不存在 —— " + baseDir.string());
        }

        std::string regexStr = globToRegex(pattern);
        std::regex re(regexStr, std::regex::ECMAScript);

        // 收集匹配文件
        struct Entry {
            std::string path;
            std::filesystem::file_time_type mtime;
        };
        std::vector<Entry> hits;

        try {
            for (auto it = fs::recursive_directory_iterator(
                     baseDir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                if (ec) { ec.clear(); continue; }

                fs::path rel = fs::relative(it->path(), toolCwd(), ec);
                if (ec) { ec.clear(); continue; }

                std::string relStr = rel.generic_string();

                if (std::regex_match(relStr, re)) {
                    Entry e;
                    e.path = relStr;
                    e.mtime = it->last_write_time(ec);
                    if (!ec) hits.push_back(std::move(e));
                    ec.clear();
                }
            }
        } catch (const std::exception& ex) {
            return errText("fs.glob 扫描异常: " + std::string(ex.what()));
        }

        // mtime 降序
        std::sort(hits.begin(), hits.end(),
                  [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });

        const size_t limit = 1000;
        bool truncated = hits.size() > limit;
        if (truncated) hits.resize(limit);

        std::vector<std::string> filePaths;
        for (const auto& e : hits) filePaths.push_back(e.path);

        pj::JsonObject out;
        out.set("files", pj::makeStringArray(filePaths));
        out.set("count", pj::makeInt(static_cast<int64_t>(hits.size())));
        if (truncated) {
            out.set("truncated", pj::makeBool(true));
        }
        return okJson(pj::stringify(pj::JsonValue(std::move(out))).value());
    }
};

// ========================================================================
// 2. fs.grep
// ========================================================================

class FsGrepTool : public core::ITool {
public:
    std::string name() const override { return "fs.grep"; }

    std::string schemaJson() const override {
        return R"({"name":"fs.grep","description":"文件内容正则搜索：按模式扫描工作目录下的文本文件（UTF-8 优先、GBK 回退），支持文件粒度过滤和行号输出","input_schema":{"type":"object","properties":{"pattern":{"type":"string","description":"正则表达式（ECMAScript 语法）"},"path":{"type":"string","description":"搜索起始目录，相对于当前工作目录，默认当前工作目录"},"glob":{"type":"string","description":"文件名过滤 glob，如 \"*.cpp\""},"mode":{"type":"string","description":"输出模式：content（匹配行，默认）、files（文件路径列表）、count（匹配计数）"},"line_numbers":{"type":"boolean","description":"是否输出行号，默认 true（仅 content 模式有效）"},"head_limit":{"type":"integer","description":"输出行数上限，默认 250；0 不限制"}},"required":["pattern"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string pattern = optString(obj, "pattern");
        if (pattern.empty()) {
            return errText("fs.grep 参数错误：缺少必填参数 pattern（正则表达式）");
        }

        std::string relPath = optString(obj, "path");
        if (!relPath.empty() && !isSafeRelative(relPath)) {
            return errText("fs.grep 参数错误：path 含不安全路径段 —— " + relPath);
        }

        std::string fileGlob = optString(obj, "glob");
        std::string mode = optString(obj, "mode", "content");
        if (mode != "content" && mode != "files" && mode != "count") {
            return errText("fs.grep 参数错误：mode 必须是 content、files 或 count，收到 —— " + mode);
        }

        bool lineNumbers = optBool(obj, "line_numbers", true);
        int64_t headLimit = optInt(obj, "head_limit", 250);
        if (headLimit < 0) headLimit = 0;

        std::regex re;
        try {
            re = std::regex(pattern, std::regex::ECMAScript);
        } catch (const std::regex_error& ex) {
            return errText("fs.grep 参数错误：无效正则表达式 —— " + std::string(ex.what()));
        }

        fs::path baseDir = relPath.empty() ? toolCwd() : toolCwd() / fs::path(relPath);
        std::error_code ec;
        if (!fs::exists(baseDir, ec)) {
            return errText("fs.grep 错误：路径不存在 —— " + baseDir.string());
        }

        // 如果有 glob 过滤，预编译 glob regex
        std::regex globRe;
        bool hasFileGlob = !fileGlob.empty();
        if (hasFileGlob) {
            try {
                globRe = std::regex(globToRegex(fileGlob), std::regex::ECMAScript);
            } catch (const std::regex_error& ex) {
                return errText("fs.grep 参数错误：无效 glob 表达式 —— " + std::string(ex.what()));
            }
        }

        struct MatchEntry {
            std::string file;
            int lineNum;
            std::string line;
        };
        std::vector<MatchEntry> matches;
        std::map<std::string, int> fileCounts;
        int totalLines = 0;

        try {
            for (auto it = fs::recursive_directory_iterator(
                     baseDir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                if (ec) { ec.clear(); continue; }

                fs::path rel = fs::relative(it->path(), toolCwd(), ec);
                if (ec) { ec.clear(); continue; }
                std::string relStr = rel.generic_string();

                if (hasFileGlob) {
                    std::string fname = it->path().filename().string();
                    if (!std::regex_match(fname, globRe)) continue;
                }

                // 读文件（UTF-8 优先）
                std::ifstream f(it->path(), std::ios::binary);
                if (!f) continue;
                std::string body((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                f.close();

                // 清洗非法 UTF-8（GBK 吞不掉就跳过）
                body = platform::utf8::sanitize(body);

                std::istringstream stream(body);
                std::string line;
                int lineNum = 0;
                while (std::getline(stream, line)) {
                    ++lineNum;
                    if (std::regex_search(line, re)) {
                        if (mode == "content" || mode == "files") {
                            matches.push_back({relStr, lineNum, line});
                        }
                        fileCounts[relStr]++;
                        totalLines++;
                        if (headLimit > 0) {
                            if (mode == "content" && (int64_t)matches.size() >= headLimit) goto done;
                            if (mode == "files" && (int64_t)fileCounts.size() >= headLimit) goto done;
                            if (mode == "count" && totalLines >= headLimit) goto done;
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            return errText("fs.grep 扫描异常: " + std::string(ex.what()));
        }

done:
        if (mode == "content") {
            // 格式: file:line:content
            std::ostringstream oss;
            for (const auto& m : matches) {
                if (lineNumbers) {
                    oss << m.file << ":" << m.lineNum << ":" << m.line << "\n";
                } else {
                    oss << m.file << ":" << m.line << "\n";
                }
            }
            std::string text = oss.str();
            if (text.empty()) text = "(无匹配)";
            return okText(text);
        } else if (mode == "files") {
            std::vector<std::string> fnames;
            for (const auto& [fname, cnt] : fileCounts) fnames.push_back(fname);
            pj::JsonObject out;
            out.set("files", pj::makeStringArray(fnames));
            out.set("count", pj::makeInt((int64_t)fileCounts.size()));
            return okJson(pj::stringify(pj::JsonValue(std::move(out))).value());
        } else {  // count
            std::ostringstream oss;
            for (const auto& [fname, cnt] : fileCounts) {
                oss << fname << ": " << cnt << " 处匹配\n";
            }
            std::string text = oss.str();
            if (text.empty()) text = "(无匹配)";
            return okText(text);
        }
    }
};

// ========================================================================
// 3. fs.read
// ========================================================================

class FsReadTool : public core::ITool {
public:
    std::string name() const override { return "fs.read"; }

    std::string schemaJson() const override {
        return R"({"name":"fs.read","description":"读取文件内容：返回带行号的文本（默认上限 2000 行 / 256KB，缺失文件或目录给清晰错误）","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"文件路径，相对于当前工作目录"},"offset":{"type":"integer","description":"起始行号（从 1 开始），默认第 1 行"},"limit":{"type":"integer","description":"读取行数上限，默认 2000"}},"required":["path"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string path = optString(obj, "path");
        if (path.empty()) {
            return errText("fs.read 参数错误：缺少必填参数 path（文件路径）");
        }
        if (!isSafeRelative(path)) {
            return errText("fs.read 参数错误：path 含不安全路径段 —— " + path);
        }

        fs::path filePath = toolCwd() / fs::path(path);
        std::error_code ec;
        if (!fs::exists(filePath, ec)) {
            return errText("fs.read 错误：文件不存在 —— " + filePath.string());
        }
        if (fs::is_directory(filePath, ec)) {
            return errText("fs.read 错误：path 是一个目录，请指定文件 —— " + filePath.string());
        }

        std::ifstream f(filePath, std::ios::binary);
        if (!f) {
            return errText("fs.read 错误：无法打开文件 —— " + filePath.string());
        }
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        f.close();

        body = platform::utf8::sanitize(body);

        // 256KB 硬上限
        constexpr size_t kMaxBytes = 256u * 1024u;
        bool byteTruncated = body.size() > kMaxBytes;
        if (byteTruncated) body.resize(kMaxBytes);

        std::istringstream stream(body);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
        }

        int64_t offset = optInt(obj, "offset", 1);
        if (offset < 1) offset = 1;
        int64_t limit = optInt(obj, "limit", 2000);
        if (limit <= 0) limit = 2000;

        size_t start = static_cast<size_t>(offset - 1);
        if (start >= lines.size()) {
            return okText("(文件共 " + std::to_string(lines.size()) +
                          " 行，offset " + std::to_string(offset) + " 超出范围)");
        }

        std::ostringstream oss;
        size_t end = std::min(start + static_cast<size_t>(limit), lines.size());
        for (size_t i = start; i < end; ++i) {
            oss << (i + 1) << "\t" << lines[i] << "\n";
        }

        if (end < lines.size()) {
            oss << "[fs.read: 已截断，已显示 " << (end - start) << " 行，文件共 "
                << lines.size() << " 行";
            if (byteTruncated) oss << "，256KB 超限已截断";
            oss << "]";
        } else if (byteTruncated) {
            oss << "[fs.read: 256KB 超限已截断]";
        }

        return okText(oss.str());
    }
};

// ========================================================================
// 4. fs.write
// ========================================================================

class FsWriteTool : public core::ITool {
public:
    std::string name() const override { return "fs.write"; }

    std::string schemaJson() const override {
        return R"({"name":"fs.write","description":"写入文件：原子写（先写临时文件再 rename），自动创建父目录，覆盖已有文件","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"文件路径，相对于当前工作目录"},"content":{"type":"string","description":"文件完整文本内容"}},"required":["path","content"]}})";
    }

    bool isDestructive() const override { return true; }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string path = optString(obj, "path");
        if (path.empty()) {
            return errText("fs.write 参数错误：缺少必填参数 path（文件路径）");
        }
        if (!isSafeRelative(path)) {
            return errText("fs.write 参数错误：path 含不安全路径段 —— " + path);
        }

        std::string content = optString(obj, "content");
        // content 可以为空串（清空文件）

        fs::path filePath = toolCwd() / fs::path(path);

        // 建父目录
        std::error_code ec;
        fs::create_directories(filePath.parent_path(), ec);

        // 原子写：先写 .tmp，再 rename
        fs::path tmpPath = filePath;
        tmpPath += ".tmp";
        {
            std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
            if (!f) {
                return errText("fs.write 错误：无法写入临时文件 —— " + tmpPath.string());
            }
            f << content;
            f.close();
            if (f.fail()) {
                fs::remove(tmpPath, ec);
                return errText("fs.write 错误：临时文件写入失败（磁盘满？）—— " + tmpPath.string());
            }
        }
        fs::rename(tmpPath, filePath, ec);
        if (ec) {
            fs::remove(tmpPath, ec);
            return errText("fs.write 错误：rename 失败 —— " + ec.message());
        }

        return okText("写入成功: " + path + " (" +
                      std::to_string(content.size()) + " 字节)");
    }
};

// ========================================================================
// 5. fs.edit
// ========================================================================

class FsEditTool : public core::ITool {
public:
    std::string name() const override { return "fs.edit"; }

    std::string schemaJson() const override {
        return R"({"name":"fs.edit","description":"精确字符串替换（编辑文件）：在文件中查找并替换唯一匹配的 old_string 为 new_string；零命中或多命中（且未开 replace_all）报错","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"文件路径，相对于当前工作目录"},"old_string":{"type":"string","description":"要被替换的精确字符串（必须文件中恰好出现一次，或配合 replace_all）"},"new_string":{"type":"string","description":"替换后的新字符串"},"replace_all":{"type":"boolean","description":"替换所有出现（默认 false，只替换唯一匹配）"}},"required":["path","old_string","new_string"]}})";
    }

    bool isDestructive() const override { return true; }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string path = optString(obj, "path");
        if (path.empty()) {
            return errText("fs.edit 参数错误：缺少必填参数 path（文件路径）");
        }
        if (!isSafeRelative(path)) {
            return errText("fs.edit 参数错误：path 含不安全路径段 —— " + path);
        }

        std::string oldStr = optString(obj, "old_string");
        if (oldStr.empty()) {
            return errText("fs.edit 参数错误：缺少必填参数 old_string（要被替换的字符串）");
        }
        std::string newStr = optString(obj, "new_string");
        bool replaceAll = optBool(obj, "replace_all", false);

        fs::path filePath = toolCwd() / fs::path(path);
        std::error_code ec;
        if (!fs::exists(filePath, ec)) {
            return errText("fs.edit 错误：文件不存在 —— " + filePath.string());
        }

        std::ifstream f(filePath, std::ios::binary);
        if (!f) {
            return errText("fs.edit 错误：无法打开文件 —— " + filePath.string());
        }
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        f.close();

        // 数出现次数
        size_t count = 0;
        size_t pos = 0;
        while ((pos = body.find(oldStr, pos)) != std::string::npos) {
            ++count;
            pos += oldStr.size();
        }

        if (count == 0) {
            return errText("fs.edit 错误：old_string 在文件中未找到任何匹配 —— " +
                           filePath.string());
        }

        if (count > 1 && !replaceAll) {
            return errText("fs.edit 错误：old_string 在文件中出现了 " +
                           std::to_string(count) +
                           " 次，请缩小 old_string 使其唯一匹配，或打开 replace_all");
        }

        // 替换
        if (replaceAll) {
            size_t p = 0;
            while ((p = body.find(oldStr, p)) != std::string::npos) {
                body.replace(p, oldStr.size(), newStr);
                p += newStr.size();
            }
        } else {
            body.replace(body.find(oldStr), oldStr.size(), newStr);
        }

        // 原子写
        fs::path tmpPath = filePath;
        tmpPath += ".tmp";
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out || !(out << body)) {
                fs::remove(tmpPath, ec);
                return errText("fs.edit 错误：写入失败");
            }
            out.close();
        }
        fs::rename(tmpPath, filePath, ec);
        if (ec) {
            fs::remove(tmpPath, ec);
            return errText("fs.edit 错误：rename 失败 —— " + ec.message());
        }

        return okText("替换成功: " + path + "，共 " + std::to_string(count) + " 处替换");
    }
};

// ========================================================================
// 6. bash.run
// ========================================================================

class BashRunTool : public core::ITool {
public:
    std::string name() const override { return "bash.run"; }

    std::string schemaJson() const override {
        return R"({"name":"bash.run","description":"执行 shell 命令：在当前工作目录下通过系统默认 shell 执行命令（Windows 用 Git Bash、macOS 用 zsh、Linux 用 sh），stdout/stderr 全捕获，输出上限 10 MiB","input_schema":{"type":"object","properties":{"cmd":{"type":"string","description":"要执行的 shell 命令（单行或多行字符串）"},"cwd":{"type":"string","description":"工作目录，相对于当前工作目录，默认当前工作目录"},"timeout_ms":{"type":"integer","description":"超时毫秒数，默认 30000（30 秒），上限 600000（10 分钟）"},"stdin":{"type":"string","description":"写入子进程 stdin 的文本内容"}},"required":["cmd"]}})";
    }

    bool isDestructive() const override { return true; }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string cmd = optString(obj, "cmd");
        if (cmd.empty()) {
            return errText("bash.run 参数错误：缺少必填参数 cmd（shell 命令）");
        }

        std::string cwdRel = optString(obj, "cwd");
        std::string stdinText = optString(obj, "stdin");
        int64_t timeoutMs = optInt(obj, "timeout_ms", 30000);
        if (timeoutMs <= 0) timeoutMs = 30000;
        if (timeoutMs > 600000) timeoutMs = 600000;

        fs::path workDir;
        if (!cwdRel.empty()) {
            if (!isSafeRelative(cwdRel)) {
                return errText("bash.run 参数错误：cwd 含不安全路径段 —— " + cwdRel);
            }
            workDir = toolCwd() / fs::path(cwdRel);
            std::error_code ec;
            if (!fs::exists(workDir, ec)) {
                return errText("bash.run 错误：工作目录不存在 —— " + workDir.string());
            }
        } else {
            workDir = toolCwd();
        }

        std::string shell = platform::defaultShellPath();
        platform::ProcessRunner runner = platform::systemProcessRunner();

        // 构建 argv: <shell> -c <cmd>
        std::vector<std::string> shellArgv;
        shellArgv.push_back(shell);
        shellArgv.push_back("-c");
        shellArgv.push_back(cmd);

        auto spawned = runner(shellArgv, stdinText, workDir.string(),
                              static_cast<long>(timeoutMs));
        if (spawned.isFail()) {
            return errText("bash.run 执行失败: " + spawned.errorMessage());
        }

        const platform::ProcessResult& pr = spawned.value();

        // 输出上限 10 MiB
        constexpr size_t kMaxOutput = 10u * 1024u * 1024u;
        bool outTrunc = pr.stdoutText.size() > kMaxOutput;
        bool errTrunc = pr.stderrText.size() > kMaxOutput;

        pj::JsonObject out;
        out.set("exit_code", pj::makeInt(pr.exitCode));
        if (!pr.stdoutText.empty()) {
            std::string text = outTrunc ? pr.stdoutText.substr(0, kMaxOutput) + "\n[stdout 已截断]"
                                        : pr.stdoutText;
            out.set("stdout", pj::makeString(text));
        }
        if (!pr.stderrText.empty()) {
            std::string text = errTrunc ? pr.stderrText.substr(0, kMaxOutput) + "\n[stderr 已截断]"
                                        : pr.stderrText;
            out.set("stderr", pj::makeString(text));
        }
        if (pr.timedOut) {
            out.set("timed_out", pj::makeBool(true));
        }

        bool isError = pr.exitCode != 0;
        return Result<ToolResult>::ok(
            ToolResult{isError, pj::stringify(pj::JsonValue(std::move(out))).value()});
    }
};

// ========================================================================
// git 工具公共 helper
// ========================================================================

// 跑 git 子命令，返回 ProcessResult。repo 是工作目录（绝对或相对路径）。
core::Result<platform::ProcessResult> runGit(const std::string& repo,
                              const std::vector<std::string>& gitArgs,
                              const std::string& stdinText = "",
                              long timeoutMs = 30000) {
    fs::path repoPath;
    if (repo.empty()) {
        repoPath = toolCwd();
    } else if (repo.size() >= 2 && repo[1] == ':') {
        // 绝对盘符路径
        repoPath = repo;
    } else if (!repo.empty() && (repo[0] == '/' || repo[0] == '\\')) {
        repoPath = repo;
    } else {
        // 相对路径 → 以当前工作目录为根
        repoPath = toolCwd() / fs::path(repo);
    }

    // 校验是 git 仓库（有 .git 目录或文件——worktree 用 .git 文件）
    fs::path gitDir = repoPath / ".git";
    std::error_code ec;
    if (!fs::exists(gitDir, ec)) {
        return core::Result<platform::ProcessResult>::fail(
            core::ErrorCode::NotFound,
            "不是 git 仓库: " + repoPath.string() + "（缺 .git 目录/文件）");
    }

    std::vector<std::string> argv;
    argv.push_back("git");
    for (const auto& a : gitArgs) argv.push_back(a);

    return platform::systemProcessRunner()(argv, stdinText, repoPath.string(), timeoutMs);
}

// 跑 git 并取结果，返回 okText 或 errText
Result<ToolResult> gitResult(const std::string& repo,
                              const std::vector<std::string>& gitArgs,
                              const std::string& errorPrefix = "") {
    auto r = runGit(repo, gitArgs);
    if (r.isFail()) return errText(r.errorMessage());
    const auto& pr = r.value();
    std::string out;
    if (!pr.stdoutText.empty()) out += pr.stdoutText;
    if (!pr.stderrText.empty()) {
        if (!out.empty()) out += "\n";
        out += pr.stderrText;
    }
    bool isErr = pr.exitCode != 0;
    if (isErr && !errorPrefix.empty()) {
        out = errorPrefix + "\n" + out;
    }
    return isErr ? errText(out) : okText(out);
}

// ========================================================================
// 7. git.clone
// ========================================================================

class GitCloneTool : public core::ITool {
public:
    std::string name() const override { return "git.clone"; }

    std::string schemaJson() const override {
        return R"({"name":"git.clone","description":"克隆 Git 仓库：将远程仓库克隆到当前工作目录下的子目录（默认目录名 = URL 尾段去 .git），不覆盖非空目录","input_schema":{"type":"object","properties":{"url":{"type":"string","description":"Git 仓库 URL（支持 https/git 协议）"},"name":{"type":"string","description":"克隆目标目录名（相对于当前工作目录），默认从 URL 推断"}},"required":["url"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string url = optString(obj, "url");
        if (url.empty()) {
            return errText("git.clone 参数错误：缺少必填参数 url（仓库 URL）");
        }

        std::string name = optString(obj, "name");
        if (name.empty()) {
            // 从 URL 尾段推断：去掉尾部 .git
            size_t lastSlash = url.find_last_of('/');
            name = (lastSlash != std::string::npos) ? url.substr(lastSlash + 1) : url;
            if (name.size() > 4 && name.substr(name.size() - 4) == ".git") {
                name = name.substr(0, name.size() - 4);
            }
        }

        fs::path destPath = toolCwd() / fs::path(name);
        std::error_code ec;
        if (fs::exists(destPath, ec)) {
            if (fs::is_directory(destPath, ec) && !fs::is_empty(destPath, ec)) {
                return errText("git.clone 错误：目标目录已存在且非空 —— " + destPath.string());
            }
        }

        return gitResult("", {"clone", url, name}, "git.clone 失败");
    }
};

// ========================================================================
// 8. git.status
// ========================================================================

class GitStatusTool : public core::ITool {
public:
    std::string name() const override { return "git.status"; }

    std::string schemaJson() const override {
        return R"({"name":"git.status","description":"Git 仓库状态：显示当前分支名、领先/落后提交数、工作区状态（porcelain 格式，含未跟踪文件）","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");

        // git status --porcelain -b 同时给分支名和 porcelain 状态
        auto r = runGit(repo, {"status", "--porcelain", "-b"});
        if (r.isFail()) return errText(r.errorMessage());
        const auto& pr = r.value();
        bool isErr = pr.exitCode != 0;

        // 解析首行 "## <branch>...<remote>/<branch> [ahead N] [behind N]"
        std::string out;
        std::istringstream stream(pr.stdoutText);
        std::string line;
        if (std::getline(stream, line) && line.size() > 3 && line[0] == '#' && line[1] == '#') {
            out += line + "\n";
        }
        // 其余行是 porcelain 状态
        while (std::getline(stream, line)) {
            out += line + "\n";
        }
        if (!pr.stderrText.empty()) out += pr.stderrText;

        return isErr && out.empty() ? errText("git.status 失败") : okText(out);
    }
};

// ========================================================================
// 9. git.log
// ========================================================================

class GitLogTool : public core::ITool {
public:
    std::string name() const override { return "git.log"; }

    std::string schemaJson() const override {
        return R"({"name":"git.log","description":"Git 提交日志：显示最近 N 条提交（哈希、作者、日期、消息）","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"},"n":{"type":"integer","description":"显示最近 N 条提交，默认 10"},"oneline":{"type":"boolean","description":"单行模式，默认 false（显示完整信息）"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        int64_t n = optInt(obj, "n", 10);
        if (n <= 0) n = 10;
        bool oneline = optBool(obj, "oneline", false);

        std::vector<std::string> gitArgs;
        gitArgs.push_back("log");
        if (oneline) {
            gitArgs.push_back("--oneline");
        }
        gitArgs.push_back("-n");
        gitArgs.push_back(std::to_string(n));

        return gitResult(repo, gitArgs);
    }
};

// ========================================================================
// 10. git.diff
// ========================================================================

class GitDiffTool : public core::ITool {
public:
    std::string name() const override { return "git.diff"; }

    std::string schemaJson() const override {
        return R"({"name":"git.diff","description":"Git 差异：显示工作区未暂存的改动（unstaged），或暂存区改动（cached），或指定文件改动","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"},"cached":{"type":"boolean","description":"显示暂存区改动（staged），默认 false（unstaged）"},"path":{"type":"string","description":"限定文件路径"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        bool cached = optBool(obj, "cached", false);
        std::string path = optString(obj, "path");

        std::vector<std::string> gitArgs;
        gitArgs.push_back("diff");
        if (cached) gitArgs.push_back("--cached");
        if (!path.empty()) gitArgs.push_back(path);

        return gitResult(repo, gitArgs);
    }
};

// ========================================================================
// 11. git.branch
// ========================================================================

class GitBranchTool : public core::ITool {
public:
    std::string name() const override { return "git.branch"; }

    std::string schemaJson() const override {
        return R"({"name":"git.branch","description":"Git 分支列表：显示本地分支（当前分支标 *）","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        return gitResult(repo, {"branch"});
    }
};

// ========================================================================
// 12. git.checkout
// ========================================================================

class GitCheckoutTool : public core::ITool {
public:
    std::string name() const override { return "git.checkout"; }

    std::string schemaJson() const override {
        return R"({"name":"git.checkout","description":"Git 切换分支或检出：切换到指定 ref（分支名 / tag / 提交哈希），拒绝以 '-' 开头的 ref（防 dwim 建分支）","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"},"ref":{"type":"string","description":"要切换到的 ref（分支名、tag 名或提交哈希）"}},"required":["ref"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        std::string ref = optString(obj, "ref");
        if (ref.empty()) {
            return errText("git.checkout 参数错误：缺少必填参数 ref");
        }

        // 拒绝以 '-' 开头的 ref：防 git 把它当选项解析
        if (ref[0] == '-') {
            return errText("git.checkout 参数错误：ref 不能以 '-' 开头（会被当成 git 选项）—— " + ref);
        }

        // rev-parse 预检：验证 ref 存在
        auto check = runGit(repo, {"rev-parse", "--verify", ref + "^{commit}"});
        if (check.isFail()) return errText(check.errorMessage());
        if (check.value().exitCode != 0) {
            return errText("git.checkout 错误：ref 不存在或非提交 —— " + ref + "\n" +
                           check.value().stderrText);
        }

        return gitResult(repo, {"checkout", ref});
    }
};

// ========================================================================
// 13. git.pull
// ========================================================================

class GitPullTool : public core::ITool {
public:
    std::string name() const override { return "git.pull"; }

    std::string schemaJson() const override {
        return R"({"name":"git.pull","description":"Git 拉取更新：git pull --ff-only（仅快进合并，避免自动 merge 冲突）","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"}},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        return gitResult(repo, {"pull", "--ff-only"});
    }
};

// ========================================================================
// 14. git.commit
// ========================================================================

class GitCommitTool : public core::ITool {
public:
    std::string name() const override { return "git.commit"; }

    std::string schemaJson() const override {
        return R"({"name":"git.commit","description":"Git 提交：提交指定的文件列表（显式 pathspec，不自动 -A），提交信息为必填","input_schema":{"type":"object","properties":{"repo":{"type":"string","description":"仓库路径，相对于当前工作目录，默认当前工作目录"},"message":{"type":"string","description":"提交信息（commit message）"},"files":{"type":"array","items":{"type":"string"},"description":"要提交的文件路径列表（相对仓库根），必填"}},"required":["message","files"]}})";
    }

    bool isDestructive() const override { return true; }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto obj = parseArgs(args.json);
        std::string repo = optString(obj, "repo");
        std::string message = optString(obj, "message");
        if (message.empty()) {
            return errText("git.commit 参数错误：缺少必填参数 message（提交信息）");
        }

        auto filesVal = obj.get("files");
        std::vector<std::string> files;
        if (filesVal.isArray()) {
            for (int idx = 0; idx < filesVal.size(); ++idx) {
                auto v = filesVal[idx];
                if (v.isString()) files.push_back(v.asString());
            }
        }
        if (files.empty()) {
            return errText("git.commit 参数错误：缺少必填参数 files（文件列表，至少一个）");
        }

        // 先 stage 文件
        std::vector<std::string> addArgs = {"add", "--"};
        for (const auto& f : files) addArgs.push_back(f);
        auto addResult = runGit(repo, addArgs);
        if (addResult.isFail()) return errText(addResult.errorMessage());
        if (addResult.value().exitCode != 0) {
            return errText("git.commit 错误：git add 失败\n" + addResult.value().stderrText);
        }

        // commit
        return gitResult(repo, {"commit", "-m", message});
    }
};

} // namespace

// ── 注册入口 ──

void registerAgentBuiltinTools() {
    auto& reg = core::ToolRegistry::instance();

    const auto oneReg = [&](std::shared_ptr<core::ITool> tool, const char* label) {
        auto r = reg.registerTool(std::move(tool));
        if (r.isFail()) {
            LOG_ERROR("Agent: " + std::string(label) + " 注册失败: " + r.errorMessage());
        }
    };

    oneReg(std::make_shared<FsGlobTool>(), "fs.glob");
    oneReg(std::make_shared<FsGrepTool>(), "fs.grep");
    oneReg(std::make_shared<FsReadTool>(), "fs.read");
    oneReg(std::make_shared<FsWriteTool>(), "fs.write");
    oneReg(std::make_shared<FsEditTool>(), "fs.edit");
    oneReg(std::make_shared<BashRunTool>(), "bash.run");
    oneReg(std::make_shared<GitCloneTool>(), "git.clone");
    oneReg(std::make_shared<GitStatusTool>(), "git.status");
    oneReg(std::make_shared<GitLogTool>(), "git.log");
    oneReg(std::make_shared<GitDiffTool>(), "git.diff");
    oneReg(std::make_shared<GitBranchTool>(), "git.branch");
    oneReg(std::make_shared<GitCheckoutTool>(), "git.checkout");
    oneReg(std::make_shared<GitPullTool>(), "git.pull");
    oneReg(std::make_shared<GitCommitTool>(), "git.commit");
}

} // namespace module::agent