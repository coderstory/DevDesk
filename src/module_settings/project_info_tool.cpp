// src/module_settings/project_info_tool.cpp
#include "project_info_tool.h"

#include <cstddef>
#include <utility>

#include "platform/json.h"
#include "platform/utf8.h"
#include "project_card_store.h"

namespace module::settings {

namespace {

core::mcp::ToolResult okToolResult(std::string text) {
    return core::mcp::ToolResult{false, platform::utf8::sanitize(std::move(text))};
}

core::mcp::ToolResult errToolResult(std::string text) {
    return core::mcp::ToolResult{true, platform::utf8::sanitize(std::move(text))};
}

// 空字段显示占位：查询方（多半是 agent）不需要区分「没填」和「填了空串」。
const char* orUnfilled(const std::string& s) {
    return s.empty() ? "（未填写）" : s.c_str();
}

}  // namespace

ProjectInfoTool::ProjectInfoTool(core::data::SqliteDb* db) : db_(db) {}

std::string ProjectInfoTool::name() const { return kProjectInfoToolName; }

std::string ProjectInfoTool::description() const {
    return "按名称查询 DevDesk 登记的项目卡（一个业务系统一张卡：仓库路径、"
           "启动命令、测试命令、部署方式、测试库地址、关键模块），接手或排障时"
           "先查这里再动手";
}

std::string ProjectInfoTool::inputSchemaJson() const {
    return R"({"type":"object","properties":{"name":{"type":"string","description":"项目卡名称（登记时的名字，支持中文）"}},"required":["name"]})";
}

core::Result<core::mcp::ToolResult> ProjectInfoTool::execute(
    const std::string& argumentsJson) {
    auto parsed = platform::json::parse(argumentsJson);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("参数必须是 JSON 对象"));
    }
    const platform::json::JsonObject args = parsed.value().asObject();
    const platform::json::JsonValue nameValue = args.get("name");
    if (!nameValue.isString() || nameValue.asString().empty()) {
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("缺少 name（必须是非空字符串）"));
    }
    if (db_ == nullptr) {
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("数据库未就绪，项目卡暂不可查询"));
    }

    const std::string queryName = nameValue.asString();
    auto card = getProjectCard(*db_, queryName);
    if (card.isFail()) {
        if (card.errorCode() == core::ErrorCode::ValidationError) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("name 不能是空白串：" + card.errorMessage()));
        }
        if (card.errorCode() == core::ErrorCode::NotFound) {
            // 附已登记清单：agent 拿到可重试的名字，而不是干瞪错误
            std::string hint = "未找到名为「" + queryName + "」的项目卡。";
            auto all = listProjectCards(*db_);
            if (all.isSuccess() && !all.value().empty()) {
                hint += "已登记：";
                for (std::size_t i = 0; i < all.value().size(); ++i) {
                    if (i > 0) hint += "、";
                    hint += all.value()[i].name;
                }
                hint += "。";
            } else {
                hint += "当前没有已登记的项目卡（可在 DevDesk 设置 → 项目卡 tab 登记）。";
            }
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult(std::move(hint)));
        }
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("项目卡查询失败：" + card.errorMessage()));
    }

    const ProjectCard& c = card.value();
    std::string text = "项目卡「" + c.name + "」\n";
    text += "- 仓库路径：" + std::string(orUnfilled(c.repoPath)) + "\n";
    text += "- 启动命令：" + std::string(orUnfilled(c.startCommand)) + "\n";
    text += "- 测试命令：" + std::string(orUnfilled(c.testCommand)) + "\n";
    text += "- 部署方式：" + std::string(orUnfilled(c.deployMethod)) + "\n";
    text += "- 测试库地址：" + std::string(orUnfilled(c.testDbUrl)) + "\n";
    text += "- 关键模块：" + std::string(orUnfilled(c.keyModules));
    return core::Result<core::mcp::ToolResult>::ok(okToolResult(std::move(text)));
}

core::Result<void> registerProjectInfoTool(core::mcp::McpToolRegistry& registry,
                                           core::data::SqliteDb* db) {
    return registry.registerTool(std::make_shared<ProjectInfoTool>(db));
}

void unregisterProjectInfoTool(core::mcp::McpToolRegistry& registry) {
    registry.unregisterTool(kProjectInfoToolName);
}

}  // namespace module::settings
