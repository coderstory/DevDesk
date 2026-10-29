// src/module_markdown/notes_mcp_tools.cpp
#include "notes_mcp_tools.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>

#include "core/logging.h"
#include "core/time.h"
#include "platform/json.h"
#include "platform/utf8.h"

namespace module::markdown::mcp {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

constexpr int kDefaultSearchLimit = 10;
constexpr int kMaxSearchLimit = 50;
// topic 码点上限：文件名长度保守值（日期前缀与 .md 由路由另计）
constexpr std::size_t kMaxTopicChars = 64;

core::mcp::ToolResult okToolResult(std::string text) {
    return core::mcp::ToolResult{false, platform::utf8::sanitize(std::move(text))};
}

core::mcp::ToolResult errToolResult(std::string text) {
    return core::mcp::ToolResult{true, platform::utf8::sanitize(std::move(text))};
}

// UTF-8 码点计数（不砍多字节中段）
std::size_t utf8Codepoints(const std::string& s) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (lead >= 0xF0) {
            len = 4;
        } else if (lead >= 0xE0) {
            len = 3;
        } else if (lead >= 0xC0) {
            len = 2;
        }
        if (i + len > s.size()) len = 1;  // 容错：sanitize 后不应出现
        ++chars;
        i += len;
    }
    return chars;
}

// ── notes_search：直走 MarkdownService::search（FTS5 + LIKE 双路，零检索逻辑）──
class NotesSearchTool final : public core::mcp::IMcpTool {
public:
    explicit NotesSearchTool(MarkdownService& service) : service_(service) {}

    std::string name() const override { return kNotesSearchToolName; }
    std::string description() const override {
        return "搜索 DevDesk 笔记库（标题与全文，中文按子串匹配），"
               "返回标题与相对路径列表，路径可供 notes_read 取全文";
    }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{"query":{"type":"string","description":"搜索词（中文按子串匹配标题与正文）"},"limit":{"type":"integer","description":"返回条数上限，默认 10，范围 1..50"}},"required":["query"]})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        auto parsed = platform::json::parse(argumentsJson);
        if (parsed.isFail() || !parsed.value().isObject()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("参数必须是 JSON 对象"));
        }
        const JsonObject args = parsed.value().asObject();
        const JsonValue queryValue = args.get("query");
        if (!queryValue.isString() || queryValue.asString().empty()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("缺少 query（必须是非空字符串）"));
        }
        int limit = kDefaultSearchLimit;
        const JsonValue limitValue = args.get("limit");
        if (limitValue.isNumber()) {
            // JsonImpl 的 Int/Double 字段互斥写入，asInt()==0 时走 asDouble 验整值
            //（memory_search 同款钳制口径）
            const std::int64_t raw = limitValue.asInt();
            const double d = limitValue.asDouble();
            if (raw != 0) {
                std::int64_t clamped = raw;
                if (clamped < 1) clamped = 1;
                if (clamped > kMaxSearchLimit) clamped = kMaxSearchLimit;
                limit = static_cast<int>(clamped);
            } else if (std::floor(d) == d) {
                double clamped = d < 1.0 ? 1.0 : d;
                if (clamped > static_cast<double>(kMaxSearchLimit)) {
                    clamped = static_cast<double>(kMaxSearchLimit);
                }
                limit = static_cast<int>(clamped);
            }
        }

        auto found = service_.search(queryValue.asString());
        if (found.isFail()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("笔记搜索失败：" + found.errorMessage()));
        }
        const auto& hits = found.value();
        if (hits.empty()) {
            return core::Result<core::mcp::ToolResult>::ok(
                okToolResult("未命中任何笔记"));
        }
        std::string text = "命中 " + std::to_string(hits.size()) + " 篇";
        if (hits.size() > static_cast<std::size_t>(limit)) {
            text += "（显示前 " + std::to_string(limit) + " 篇）";
        }
        text += "：\n";
        for (std::size_t i = 0;
             i < hits.size() && i < static_cast<std::size_t>(limit); ++i) {
            text += "- " + hits[i].title + "（" +
                    MarkdownService::toPosix(hits[i].relativePath) + "）\n";
        }
        return core::Result<core::mcp::ToolResult>::ok(okToolResult(text));
    }

private:
    MarkdownService& service_;
};

// ── notes_read：MarkdownService::read 全文原样回传 ──
class NotesReadTool final : public core::mcp::IMcpTool {
public:
    explicit NotesReadTool(MarkdownService& service) : service_(service) {}

    std::string name() const override { return kNotesReadToolName; }
    std::string description() const override {
        return "读取一篇 DevDesk 笔记的全文（Markdown 原文）";
    }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","description":"笔记相对路径（POSIX 风格，取自 notes_search 结果）"}},"required":["path"]})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        auto parsed = platform::json::parse(argumentsJson);
        if (parsed.isFail() || !parsed.value().isObject()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("参数必须是 JSON 对象"));
        }
        const JsonObject args = parsed.value().asObject();
        const JsonValue pathValue = args.get("path");
        if (!pathValue.isString() || pathValue.asString().empty()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("缺少 path（必须是非空字符串）"));
        }
        const std::string& path = pathValue.asString();
        auto content = service_.read(path);
        if (content.isFail()) {
            return core::Result<core::mcp::ToolResult>::ok(errToolResult(
                "读取笔记失败（" + path + "）：" + content.errorMessage()));
        }
        return core::Result<core::mcp::ToolResult>::ok(
            okToolResult(std::move(content.value())));
    }

private:
    MarkdownService& service_;
};

// ── notes_write：新文件路由（红线：只建新文件，已存在一律拒绝）──
class NotesWriteTool final : public core::mcp::IMcpTool {
public:
    NotesWriteTool(MarkdownService& service,
                   std::shared_ptr<NoteGitAutoCommit> gitAutoCommit)
        : service_(service), gitAutoCommit_(std::move(gitAutoCommit)) {}

    std::string name() const override { return kNotesWriteToolName; }
    std::string description() const override {
        return "把内容写成笔记库新文件（来自Claude/日期-主题.md）。只创建新文件，"
               "不改写、不追加已有笔记";
    }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{"topic":{"type":"string","description":"新笔记主题，作文件名（不可含 / 或 ..，不超过 64 字符）"},"content":{"type":"string","description":"笔记全文（Markdown，UTF-8）"}},"required":["topic","content"]})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        auto parsed = platform::json::parse(argumentsJson);
        if (parsed.isFail() || !parsed.value().isObject()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("参数必须是 JSON 对象"));
        }
        const JsonObject args = parsed.value().asObject();
        const JsonValue topicValue = args.get("topic");
        if (!topicValue.isString() || topicValue.asString().empty()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("缺少 topic（必须是非空字符串）"));
        }
        if (!args.get("content").isString()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("缺少 content（必须是字符串，可为空）"));
        }
        const std::string& topic = topicValue.asString();
        if (topic.find('/') != std::string::npos ||
            topic.find('\\') != std::string::npos) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("topic 不能包含 '/' 或 '\\'（主题只作文件名，"
                              "不接受路径）"));
        }
        if (topic.find("..") != std::string::npos) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("topic 不能包含 '..'"));
        }
        if (utf8Codepoints(topic) > kMaxTopicChars) {
            return core::Result<core::mcp::ToolResult>::ok(errToolResult(
                "topic 过长（超过 " + std::to_string(kMaxTopicChars) + " 字符）"));
        }

        const std::string relPath =
            notesMcpRelPath(notesMcpTodayYmd(), topic);
        if (!MarkdownService::isSafeNewNotePath(relPath)) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("topic 不合法（拒绝路径逃逸）: " + topic));
        }
        // 红线闸：存在性以磁盘为准（fileMtime 失败 = 不存在，noteExists 同口径）。
        // 检查与写之间的窗口：来自Claude/ 命名空间归本工具独占，编辑器不自动在其
        // 下建档；残余竞态仅剩同日同主题并发双写，接受（记日志可见）。
        if (service_.fileMtime(relPath).isOk()) {
            return core::Result<core::mcp::ToolResult>::ok(errToolResult(
                "目标文件已存在，拒绝覆盖：" + relPath +
                "。MCP 写只创建新文件（编辑器自动保存会覆盖外部写入，防冲突）；"
                "请换一个 topic，或到 DevDesk 应用内编辑该笔记"));
        }
        auto wrote = service_.write(relPath, args.get("content").asString());
        if (wrote.isFail()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("写入新笔记失败：" + wrote.errorMessage()));
        }
        // 新文件收编进既有 git 自动提交链（5 分钟节流照旧），白拿历史回溯
        if (gitAutoCommit_ != nullptr) {
            gitAutoCommit_->onFileSaved(core::time::nowUnix());
        }
        LOG_INFO("notes_write 新建笔记: " + relPath);
        return core::Result<core::mcp::ToolResult>::ok(okToolResult(
            "已写入新笔记：" + relPath + "（FTS 已同步，notes_search 立即可搜）"));
    }

private:
    MarkdownService& service_;
    std::shared_ptr<NoteGitAutoCommit> gitAutoCommit_;
};

}  // namespace

core::Result<void> registerNotesMcpTools(core::mcp::McpToolRegistry& registry,
                                         MarkdownService& service,
                                         std::shared_ptr<NoteGitAutoCommit> gitAutoCommit) {
    auto search =
        registry.registerTool(std::make_shared<NotesSearchTool>(service));
    if (search.isFail()) {
        return search;
    }
    auto read = registry.registerTool(std::make_shared<NotesReadTool>(service));
    if (read.isFail()) {
        return read;
    }
    return registry.registerTool(
        std::make_shared<NotesWriteTool>(service, std::move(gitAutoCommit)));
}

void unregisterNotesMcpTools(core::mcp::McpToolRegistry& registry) {
    registry.unregisterTool(kNotesSearchToolName);
    registry.unregisterTool(kNotesReadToolName);
    registry.unregisterTool(kNotesWriteToolName);
}

std::string notesMcpTodayYmd() {
    std::tm local{};
    core::time::localTime(core::time::nowUnix(), &local);
    char buf[16];
    const std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return n == 0 ? std::string() : std::string(buf, n);
}

std::string notesMcpRelPath(const std::string& ymd, const std::string& topic) {
    return std::string(kNotesMcpInboxDir) + "/" + ymd + "-" + topic + ".md";
}

}  // namespace module::markdown::mcp
