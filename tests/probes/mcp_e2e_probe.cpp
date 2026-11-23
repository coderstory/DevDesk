// mcp_e2e_probe.cpp — need#381 ITool→IMcpTool 适配层 E2E（doc50 §4 落点 8 + F-02「46 工具」断言）。
//
// 链路：临时数据根 + 临时库 → 8 个业务模块真 onStart（注册 39 个真 ITool）→ 内置 MCP 工具
// 全量装配（2 内置 + project_info + memory_search + 3 notes = 7 个 IMcpTool）→ 真 McpService
// （127.0.0.1 随机端口，handlePost 入口触发 ToolMirror 惰性同步）→ platform::http 真请求：
// initialize → tools/list（46 工具断言）→ F-01 39 真工具 name/description/inputSchema 逐字段
// diff → F-05 真 7 破坏性工具 annotations 抽查 → tools/call app.now 适配工具往返。
//
// 运行：mcp_e2e_probe（无参数；ctest -L probe 可见）
// 退出码：0 = 全链路通；1 = 断言失败。
//
// 零波及：数据根指向临时目录（禁触真实 ~/DevDesk——memory setSearchEnabled 会写
// panel_settings.json），库用 test_support::TestDb 临时库，服务用临时端口，收尾全停。

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app_registry.h"
#include "core/mcp_tool.h"
#include "core/tool.h"
#include "module_mcp/mcp_service.h"
#include "module_mcp/mcp_tools_builtin.h"
#include "module_claude_proxy/claude_service.h"
#include "module_memory/mcp/search_tool.h"
#include "module_settings/project_info_tool.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "test_support.h"  // TestDb / ScopedAppDatabase / setEnvVar

#include "eui/app.h"      // app 窗控声明（空桩签名同源）
#include "eui/dsl_app.h"  // app::openWindow / DslWindowConfig / DslWindowCompose

// 空桩（先例 wallpaper_probe.cpp 自给 app:: 窗控桩）：本探针无主循环无窗口，
// 模块 onStart 引用面拉进链接的 app 层窗控符号 no-op 化。真定义在
// EUI-NEO glfw_app_main.cpp（自带 main()，不可挂进探针）。
namespace app {
void openWindow(const DslWindowConfig&, DslWindowCompose) {}
void openWindow(const char*, int, int, DslWindowCompose) {}
float contentScale() { return 1.0f; }
void setClearColor(const core::Color&) {}
void setWindowEffect(core::platform::WindowEffect) {}
core::platform::WindowEffect activeWindowEffect() { return {}; }
void closeMainWindow() {}
void iconifyMainWindow() {}
bool mainWindowMaximized() { return false; }
bool mainWindowFullscreen() { return false; }
bool customWindowChromeSupported() { return false; }
bool customWindowChromeButtonsNative() { return false; }
void requestExit() {}
void requestShow() {}
void requestUpdate() {}
void setCustomTitlebarSkin(core::platform::CustomTitlebarSkin) {}
void setTitleBarAppearance(const core::platform::TitleBarAppearance&) {}
void setQuitGuard(std::function<bool()>) {}
} // namespace app

namespace {

using platform::json::JsonValue;

int g_fail = 0;

void check(bool ok, const std::string& msg) {
    if (ok) {
        std::cout << "[probe] PASS " << msg << "\n";
    } else {
        std::cout << "[probe] FAIL " << msg << "\n";
        ++g_fail;
    }
}

std::string strOrEmpty(const JsonValue& v) {
    return v.isString() ? v.asString() : std::string();
}

std::string stringifyOrEmpty(const JsonValue& v) {
    auto s = platform::json::stringify(v);
    return s.isOk() ? s.value() : std::string();
}

// 大小写不敏感取响应头（server 发 "Mcp-Session-Id"，客户端存键大小写不保证）
std::string headerValue(const std::map<std::string, std::string>& headers,
                        const std::string& wanted) {
    for (const auto& [key, value] : headers) {
        if (key.size() != wanted.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < key.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(key[i])) ==
                   std::tolower(static_cast<unsigned char>(wanted[i]));
        }
        if (same) return value;
    }
    return "";
}

platform::http::Response postMcp(int port, const std::string& body,
                                 const std::string& sessionId) {
    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.headers["content-type"] = "application/json";
    if (!sessionId.empty()) req.headers["mcp-session-id"] = sessionId;
    req.body = body;
    req.timeoutMs = 15000;
    auto r = platform::http::send(req);
    if (r.isFail()) {
        std::cout << "[probe] FAIL HTTP 请求失败: " << r.errorMessage() << "\n";
        ++g_fail;
        return platform::http::Response{};
    }
    return std::move(r).value();
}

JsonValue parseJson(const std::string& text) {
    auto parsed = platform::json::parse(text);
    return parsed.isOk() ? parsed.value() : JsonValue();
}

// tools/list 响应里按 name 找工具 entry；缺 → 空 JsonValue
JsonValue toolEntryByName(const JsonValue& root, const std::string& name) {
    if (!root.isObject()) return JsonValue();
    const auto result = root.asObject().get("result");
    if (!result.isObject()) return JsonValue();
    const auto tools = result.asObject().get("tools");
    if (!tools.isArray()) return JsonValue();
    for (int i = 0; i < tools.size(); ++i) {
        const auto entry = tools[i];
        if (entry.isObject() && strOrEmpty(entry.asObject().get("name")) == name) {
            return entry;
        }
    }
    return JsonValue();
}

// 业务模块收尾：逆序 onStop（工具注销等依赖逆序释放）
struct ModuleScope {
    std::vector<std::unique_ptr<core::Module>> modules;

    void start(core::Module* m, const char* what) {
        if (m == nullptr) {
            check(false, std::string(what) + " 工厂返回 nullptr");
            return;
        }
        modules.emplace_back(m);
        m->onStart();
        std::cout << "[probe] started " << what << "\n";
    }
    ~ModuleScope() {
        for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
            (*it)->onStop();
        }
    }
};

} // namespace

int main() {
    // 临时数据根必须先于一切 paths 消费设置（仓库惯例 DataRootEnv：进临时 .data
    // 目录、析构还原+删目录；禁触真实数据根——memory_search 开关会写
    // panel_settings.json）
    test_support::DataRootEnv dataRoot("need381-mcp-e2e", "probe");

    test_support::TestDb db("need381-mcp-e2e", "probe");
    test_support::ScopedAppDatabase dbGuard(db.handle());

    // MCP 端口 7 工具装配（main.cpp:581-602 同款；memory_search 经
    // setSearchEnabled 显式开启，不读开关配置）
    module::mcp::registerBuiltinMcpTools(core::mcp::globalMcpTools());
    auto projectReg = module::settings::registerProjectInfoTool(
        core::mcp::globalMcpTools(), ::core::appDatabase());
    check(projectReg.isOk(), "project_info 注册");
    auto searchReg = module::memory::mcp::setSearchEnabled(
        core::mcp::globalMcpTools(), true);
    check(searchReg.isOk(), "memory_search 注册");

    // 8 个业务模块真 onStart → 39 个真 ITool 注册（todo/markdown/claude/agent/
    // memory/schedule/settings/mcp 各自跟随模块启停）。
    // claude.* 三工具经 ClaudeService::global() 门控（claude_module.cpp onStart
    // 早退）：与 main.cpp 启动前置段同款，先创建服务并 setGlobal 再 onStart。
    module::claude::ClaudeService claudeService(db.handle());
    module::claude::ClaudeService::setGlobal(&claudeService);
    ModuleScope scope;
    scope.start(create_markdown_module(), "markdown");
    scope.start(create_todo_module(), "todo");
    scope.start(create_schedule_module(), "schedule");
    scope.start(create_claude_module(), "claude");
    scope.start(create_agent_module(), "agent");
    scope.start(create_memory_module(), "memory");
    scope.start(create_settings_module(), "settings");
    scope.start(create_mcp_page_module(), "mcp");

    const auto realTools = core::ToolRegistry::instance().listTools();
    check(realTools.size() == 39,
          "源 ITool 注册数 = 39（实际 " + std::to_string(realTools.size()) + "）");
    if (realTools.size() != 39) {
        std::cout << "[probe] 源清单：";
        for (const auto& t : realTools) std::cout << " " << t->name();
        std::cout << "\n";
    }

    // 真服务：target = globalMcpTools()，mirror 于首个 POST 同步
    auto startR = module::mcp::globalMcpService().start(0);
    check(startR.isOk(),
          "MCP 服务启动（临时端口）" +
              std::string(startR.isOk() ? " :" + std::to_string(startR.value())
                                        : ": " + startR.errorMessage()));
    if (startR.isFail()) return g_fail == 0 ? 0 : 1;
    const int port = startR.value();

    // initialize → 会话
    const auto init = postMcp(
        port,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp_e2e_probe","version":"0"}}})",
        "");
    check(init.statusCode == 200, "initialize 状态 200（实际 " +
                                       std::to_string(init.statusCode) + "）");

    // F-08（D-06 定稿）：instructions 文案泛化——含新口径、不再含失真的旧文案
    const auto initRoot = parseJson(init.body);
    std::string instructions;
    if (initRoot.isObject()) {
        const auto initResult = initRoot.asObject().get("result");
        if (initResult.isObject()) {
            instructions = strOrEmpty(initResult.asObject().get("instructions"));
        }
    }
    check(instructions.find("DevDesk 内置工具集") != std::string::npos,
          "F-08 instructions 含 D-06 定稿文案（前 40 字节: " +
              instructions.substr(0, 40) + "）");
    check(instructions.find("网页抓取") == std::string::npos,
          "F-08 instructions 不再含「网页抓取」旧文案");

    const std::string sessionId = headerValue(init.headers, "Mcp-Session-Id");
    check(!sessionId.empty(), "initialize 下发 Mcp-Session-Id");

    // tools/list：54 = 39 镜像 + 15 直注（4 常驻内置/直注 + md_*×5 + todo_*×6）。
    // bug#485 基线对齐：need394 双适配层并存态（notes_*×3 下线，md_*×5 + todo_*×6
    // 经 tool_kit::registerToolsAsMcp 上线，净 +8）；#478 双适配层收敛裁决后需重数。
    const auto list = postMcp(
        port, R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})",
        sessionId);
    check(list.statusCode == 200, "tools/list 状态 200");
    const auto root = parseJson(list.body);
    const auto result = root.isObject() ? root.asObject().get("result")
                                        : JsonValue();
    const auto tools = result.isObject() ? result.asObject().get("tools")
                                         : JsonValue();
    check(tools.isArray() && tools.size() == 54,
          "tools/list 共 54 工具（实际 " +
              std::to_string(tools.isArray() ? tools.size() : -1) + "）");

    // F-01：39 真工具 name/description/inputSchema 逐字段 diff
    int diffCount = 0;
    int destructiveCount = 0;
    bool checkedNonDestructive = false;
    bool checkedBuiltinNoAnnotations = false;
    for (const auto& tool : realTools) {
        const std::string toolName = tool->name();
        const auto schema = parseJson(tool->schemaJson());
        if (!schema.isObject()) {
            std::cout << "[probe]   diff " << toolName << ": schemaJson 非法\n";
            ++diffCount;
            continue;
        }
        const auto entry = toolEntryByName(root, toolName);
        if (!entry.isObject()) {
            std::cout << "[probe]   diff " << toolName << ": tools/list 缺失\n";
            ++diffCount;
            continue;
        }
        const std::string srcDesc = strOrEmpty(schema.asObject().get("description"));
        const auto srcInput = schema.asObject().get("input_schema");
        const std::string entryDesc = strOrEmpty(entry.asObject().get("description"));
        const std::string entrySchema =
            stringifyOrEmpty(entry.asObject().get("inputSchema"));
        if (entryDesc != srcDesc) {
            std::cout << "[probe]   diff " << toolName << ": description 不一致\n";
            ++diffCount;
        }
        if (entrySchema != stringifyOrEmpty(srcInput)) {
            std::cout << "[probe]   diff " << toolName << ": inputSchema 不一致\n";
            ++diffCount;
        }

        // F-05：真破坏性工具 annotations 抽查 + 非破坏镜像无 annotations 键
        if (tool->isDestructive()) {
            ++destructiveCount;
            const auto annotations = entry.asObject().get("annotations");
            const bool hintOk =
                annotations.isObject() &&
                annotations.asObject().get("destructiveHint").isBool() &&
                annotations.asObject().get("destructiveHint").asBool() &&
                annotations.asObject().get("readOnlyHint").isBool() &&
                !annotations.asObject().get("readOnlyHint").asBool();
            if (!hintOk) {
                std::cout << "[probe]   annotations " << toolName
                          << ": 缺 destructiveHint=true/readOnlyHint=false\n";
                ++diffCount;
            }
        } else if (!checkedNonDestructive) {
            checkedNonDestructive = true;
            if (entry.asObject().has("annotations")) {
                std::cout << "[probe]   annotations " << toolName
                          << ": 非破坏工具不应带 annotations\n";
                ++diffCount;
            }
        }
    }
    check(diffCount == 0,
          "F-01 39 真工具逐字段 diff + F-05 annotations 零差异（差异 " +
              std::to_string(diffCount) + "）");
    check(destructiveCount == 7,
          "真破坏性工具数 = 7（实际 " + std::to_string(destructiveCount) + "）");

    // 内置（非镜像）工具无 annotations 键：取一个不在真 ITool 名单里的 entry 抽查
    for (int i = 0; i < tools.size() && !checkedBuiltinNoAnnotations; ++i) {
        const auto entry = tools[i];
        const std::string name = strOrEmpty(entry.asObject().get("name"));
        bool mirrored = false;
        for (const auto& t : realTools) mirrored |= (t->name() == name);
        if (!mirrored) {
            checkedBuiltinNoAnnotations = true;
            check(!entry.asObject().has("annotations"),
                  "内置工具 " + name + " 无 annotations 键");
        }
    }

    // tools/call：app.now（纯读快照，无参）经适配层往返
    const auto call = postMcp(
        port,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.now","arguments":{}}})",
        sessionId);
    check(call.statusCode == 200, "tools/call app.now 状态 200");
    const auto callRoot = parseJson(call.body);
    const auto callResult = callRoot.isObject()
                                ? callRoot.asObject().get("result")
                                : JsonValue();
    const auto content = callResult.isObject()
                             ? callResult.asObject().get("content")
                             : JsonValue();
    std::string callText;
    if (content.isArray() && content.size() > 0) {
        const auto block = content[0];
        if (block.isObject()) callText = strOrEmpty(block.asObject().get("text"));
    }
    const auto isError = callResult.isObject()
                             ? callResult.asObject().get("isError")
                             : JsonValue();
    check(!callText.empty(),
          "tools/call app.now 文本回传非空（前 60 字节: " + callText.substr(0, 60) +
              "）");
    check(!(isError.isBool() && isError.asBool()),
          "tools/call app.now isError=false");

    module::mcp::globalMcpService().stop();
    // 与 setGlobal 成对复位（claude_service.h 契约）；模块已 onStop，无在途消费者
    module::claude::ClaudeService::setGlobal(nullptr);
    std::cout << "[probe] === mcp_e2e_probe 结束：fail=" << g_fail << " ===\n";
    return g_fail == 0 ? 0 : 1;
}
