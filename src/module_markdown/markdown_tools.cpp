// MarkdownTools 实现：每个工具一个小类，同文件存放。
//   md.search / md.read / md.list_tags（只读）；md.create（写但可逆）；
//   md.delete（破坏性，isDestructive）。
// 出参：结构化数据组对象再 stringify；数组用逐元素 stringify + 拼接 + parse 校验
//（platform::json 无数组追加 API）。
// 路径安全由 MarkdownService::resolve() 收口（containment 校验），本文件不绕过
// service 直碰文件系统（md.create 存在性探测同样走 svc_.read）。
#include "markdown_tools.h"

#include "core/logging.h"
#include "platform/json.h"
#include "platform/utf8.h"

#include <filesystem>
#include <string>
#include <utility>

namespace module::markdown {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;


// 工具级错误：Result ok + isError + {"error":"<中文>"}（回喂 LLM）
Result<ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
// 自有字面量 + 一个字符串字段的序列化失败 = 编程不变量破坏
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "工具错误消息序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, text.value()});
}

Result<ToolResult> toolOk(const std::string& contentJson) {
    return Result<ToolResult>::ok(ToolResult{false, contentJson});
}

// 参数解析：空串视为 {}；其余必须解析成 object。失败 = 参数非法。
Result<pj::JsonObject> parseArgs(const ToolArgs& args) {
    if (args.json.empty()) {
        return Result<pj::JsonObject>::ok(pj::JsonObject());
    }
    auto v = pj::parse(args.json);
    if (v.isFail() || !v.value().isObject()) {
        return Result<pj::JsonObject>::fail(core::ErrorCode::ValidationError,
                                            "参数不是合法 JSON 对象");
    }
    return Result<pj::JsonObject>::ok(v.value().asObject());
}

// 字符串字段：缺失 / 非字符串 → 空串（必填校验由调用方做）
std::string strField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

// NoteMeta → JSON 对象文本（path 恒 POSIX 风格）
Result<std::string> noteToJsonText(const NoteMeta& n) {
    pj::JsonObject o;
    o.set("path", pj::makeString(n.relativePath.generic_string()));
    o.set("title", pj::makeString(n.title));
    o.set("sizeBytes", pj::makeInt(n.sizeBytes));
    o.set("modifiedAt", pj::makeInt(n.modifiedAt));
    auto text = pj::stringify(pj::JsonValue(std::move(o)));
    if (text.isFail()) {
        return Result<std::string>::fail(core::ErrorCode::Internal,
                                         "笔记元数据序列化失败: " +
                                             text.errorMessage());
    }
    return Result<std::string>::ok(text.value());
}


class MdSearchTool : public core::ITool {
public:
    explicit MdSearchTool(MarkdownService& svc) : svc_(svc) {}

    std::string name() const override { return "md.search"; }

    std::string schemaJson() const override {
        return R"({"name":"md.search","description":"全文搜索笔记（FTS5，返回命中笔记的路径与标题）","input_schema":{"type":"object","properties":{"query":{"type":"string","description":"搜索关键词（必填）"}},"required":["query"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const std::string query = strField(argsObj.value(), "query");
        if (query.empty()) {
            return toolError("搜索关键词不能为空（必填字段 query）");
        }

        auto hits = svc_.search(query);
        if (hits.isFail()) {
            return toolError("搜索失败: " + hits.errorMessage());
        }

// 数组组装：逐命中 stringify 后文本拼接
        std::string joined;
        for (const auto& n : hits.value()) {
            auto one = noteToJsonText(n);
            if (one.isFail()) return toolError(one.errorMessage());
            if (!joined.empty()) joined += ",";
            joined += one.value();
        }
        const std::string content = "{\"notes\":[" + joined + "]}";
// 自有拼接必须可解析；失败 = 不变量破坏
        if (pj::parse(content).isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal, "工具结果 JSON 拼接失败（编程错误）");
        }
        return toolOk(content);
    }

private:
    MarkdownService& svc_;
};


class MdReadTool : public core::ITool {
public:
    explicit MdReadTool(MarkdownService& svc) : svc_(svc) {}

    std::string name() const override { return "md.read"; }

    std::string schemaJson() const override {
        return R"({"name":"md.read","description":"读取一篇笔记的全文（UTF-8）","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"笔记相对路径（如 notes/a.md，必填，POSIX 分隔符）"}},"required":["path"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const std::string path = strField(argsObj.value(), "path");
        if (path.empty()) {
            return toolError("缺少必填参数 path（笔记相对路径）");
        }

        auto content = svc_.read(std::filesystem::path(path));
        if (content.isFail()) {
            return toolError("读取笔记失败: " + content.errorMessage());
        }

        // 本工具是唯一回「文件原文」的工具，而 json stringify 放行非法字节、parse 严格
        // 拒绝 → 非 UTF-8 笔记走到 AgentLoop 自检会 LOG_FATAL + abort（整应用死）。
        // 故在边界拦下：非法 → toolError 回喂 LLM（回喂乱码有被原样写回毁笔记的风险；
        // 显式失败可行动，且磁盘原笔记一字不动）。
        if (!platform::utf8::isValid(content.value())) {
            LOG_WARN("md.read: 笔记不是合法 UTF-8，拒绝回喂（path=" + path + "）");
            return toolError(
                "笔记不是 UTF-8 编码（GBK/Latin-1/二进制等），无法作为文本读取；"
                "请先用编辑器另存为 UTF-8 后重试");
        }

        pj::JsonObject o;
        o.set("path", pj::makeString(path));
        o.set("content", pj::makeString(content.value()));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return toolError("读取结果序列化失败: " + text.errorMessage());
        }
        return toolOk(text.value());
    }

private:
    MarkdownService& svc_;
};


class MdCreateTool : public core::ITool {
public:
    explicit MdCreateTool(MarkdownService& svc) : svc_(svc) {}

    std::string name() const override { return "md.create"; }

    std::string schemaJson() const override {
        return R"({"name":"md.create","description":"新建一篇笔记（路径已存在时拒绝，不覆盖既有内容）","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"笔记相对路径（如 notes/a.md，必填，POSIX 分隔符）"},"content":{"type":"string","description":"笔记正文（UTF-8 Markdown，必填）"}},"required":["path","content"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const std::string path = strField(argsObj.value(), "path");
        if (path.empty()) {
            return toolError("缺少必填参数 path（笔记相对路径）");
        }
        const std::string content = strField(argsObj.value(), "content");
        if (content.empty()) {
            // 空正文多半是漏传 key 的静默错误（agent 无 md.write，建错只能 md.delete 重来）。
            return toolError("笔记正文不能为空（必填字段 content）");
        }

        // 存在性探测：读得到 = 已存在 → 拒绝覆盖。读失败 = 不存在或路径越界（write 会再拒）。
        if (svc_.read(std::filesystem::path(path)).isOk()) {
            return toolError("笔记已存在，拒绝覆盖（如需重建先 md.delete）");
        }

        auto wr = svc_.write(std::filesystem::path(path), content);
        if (wr.isFail()) {
            return toolError("新建笔记失败: " + wr.errorMessage());
        }

        pj::JsonObject o;
        o.set("path", pj::makeString(path));
        o.set("created", pj::makeBool(true));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return toolError("新建结果序列化失败: " + text.errorMessage());
        }
        return toolOk(text.value());
    }

private:
    MarkdownService& svc_;
};


class MdListTagsTool : public core::ITool {
public:
    explicit MdListTagsTool(MarkdownService& svc) : svc_(svc) {}

    std::string name() const override { return "md.list_tags"; }

    std::string schemaJson() const override {
        return R"({"name":"md.list_tags","description":"列出全部标签及各自的笔记数（只读，无入参）","input_schema":{"type":"object","properties":{},"required":[]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        // 无入参，但仍校验 args 合法性（非法 JSON → 工具级错误回喂）
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        auto tags = svc_.listAllTags();
        if (tags.isFail()) {
            return toolError("读取标签失败: " + tags.errorMessage());
        }

        // 数组组装：逐标签 stringify 后文本拼接（同 md.search）
        std::string joined;
        for (const auto& t : tags.value()) {
            pj::JsonObject o;
            o.set("name", pj::makeString(t.name));
            o.set("noteCount", pj::makeInt(t.noteCount));
            auto one = pj::stringify(pj::JsonValue(std::move(o)));
            if (one.isFail()) {
                return toolError("标签序列化失败: " + one.errorMessage());
            }
            if (!joined.empty()) joined += ",";
            joined += one.value();
        }
        const std::string content = "{\"tags\":[" + joined + "]}";
        // 自有拼接必须可解析；失败 = 不变量破坏
        if (pj::parse(content).isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal, "工具结果 JSON 拼接失败（编程错误）");
        }
        return toolOk(content);
    }

private:
    MarkdownService& svc_;
};


// 类名避开 delete 关键字；工具名仍是 "md.delete"
class MdDeleteTool : public core::ITool {
public:
    explicit MdDeleteTool(MarkdownService& svc) : svc_(svc) {}

    std::string name() const override { return "md.delete"; }

    // 删除不可恢复 → allowDestructive=0 时 AgentLoop 直接终止 run
    bool isDestructive() const override { return true; }

    std::string schemaJson() const override {
        return R"({"name":"md.delete","description":"删除一篇笔记（不可恢复的破坏性操作）","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"笔记相对路径（如 notes/a.md，必填，POSIX 分隔符）"}},"required":["path"]}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        auto argsObj = parseArgs(args);
        if (argsObj.isFail()) return toolError(argsObj.errorMessage());

        const std::string path = strField(argsObj.value(), "path");
        if (path.empty()) {
            return toolError("缺少必填参数 path（笔记相对路径）");
        }

        // 越界路径（相对上跳 / 绝对路径）由 service resolve() 拒绝，删外部文件的口子不开在工具层。
        auto rm = svc_.remove(std::filesystem::path(path));
        if (rm.isFail()) {
            return toolError(rm.errorMessage());  // 原始错误消息（含越界说明）回喂
        }

        pj::JsonObject o;
        o.set("path", pj::makeString(path));
        o.set("deleted", pj::makeBool(true));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return toolError("删除结果序列化失败: " + text.errorMessage());
        }
        return toolOk(text.value());
    }

private:
    MarkdownService& svc_;
};

} // namespace


MarkdownTools::MarkdownTools(MarkdownService& svc) {
    tools_.push_back(std::make_shared<MdSearchTool>(svc));
    tools_.push_back(std::make_shared<MdReadTool>(svc));
    tools_.push_back(std::make_shared<MdCreateTool>(svc));
    tools_.push_back(std::make_shared<MdListTagsTool>(svc));
    tools_.push_back(std::make_shared<MdDeleteTool>(svc));
}

void MarkdownTools::registerAll() {
    for (const auto& tool : tools_) {
        auto r = core::ToolRegistry::instance().registerTool(tool);
        if (r.isFail()) {
            LOG_ERROR("MarkdownTools: 注册工具失败: " + r.errorMessage());
        }
    }
}

void MarkdownTools::unregisterAll() {
    for (const auto& tool : tools_) {
        core::ToolRegistry::instance().unregisterTool(tool->name());
    }
}

} // namespace module::markdown
