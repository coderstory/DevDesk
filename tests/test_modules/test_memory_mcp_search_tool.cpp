// tests/test_modules/test_memory_mcp_search_tool.cpp
// memory_search MCP 工具单测（F-07）。夹具：假检索器（观测入参 / 可编结果态）
// + 每用例局部注册表（内置工具先例，防全局串味）；持久化用例走
// DEVDESK_DATA_ROOT 隔离根（resetCaseGlobals 出厂已设构建树内 test_data）。
#include "doctest.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/mcp_tool.h"
#include "memory_mcp_bridge.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/mcp/search_tool.h"
#include "module_memory/panel/settings_store.h"
#include "platform/json.h"
#include "test_support.h"

using module::memory::gateway::IMemoryRetriever;
using module::memory::gateway::MemoryHit;
using module::memory::mcp::MemorySearchTool;

namespace {

// 假检索器：记录最近一次入参，结果态三选一（命中 / 失败码+文案 / 空命中）
class FakeRetriever : public IMemoryRetriever {
public:
    std::vector<MemoryHit> hits;
    bool fail = false;
    core::ErrorCode failCode = core::ErrorCode::NetworkUnreachable;
    std::string failMessage = "Connection refused";
    std::string lastQuery;
    int lastLimit = 0;
    int lastTimeoutMs = 0;
    int calls = 0;

    core::Result<std::vector<MemoryHit>> search(const std::string& query,
                                                int limit,
                                                int timeoutMs) override {
        ++calls;
        lastQuery = query;
        lastLimit = limit;
        lastTimeoutMs = timeoutMs;
        if (fail) {
            return core::Result<std::vector<MemoryHit>>::fail(failCode,
                                                              failMessage);
        }
        return core::Result<std::vector<MemoryHit>>::ok(hits);
    }
    core::Result<std::string> addDirect(const std::string&) override {
        return core::Result<std::string>::ok(std::string());
    }
    core::Result<void> setMemoryForgotten(const std::string&, bool) override {
        return core::Result<void>::ok();
    }
};

MemoryHit hitWithText(const std::string& text) {
    MemoryHit hit;
    hit.text = text;
    return hit;
}

// execute 便捷口：返回 (isError, text)；Result 层失败折成 (true, errorMessage)
bool runTool(MemorySearchTool& tool, const std::string& args, bool* isErrorOut,
             std::string* textOut) {
    auto r = tool.execute(args);
    if (r.isFail()) {
        *isErrorOut = true;
        *textOut = r.errorMessage();
        return true;
    }
    *isErrorOut = r.value().isError;
    *textOut = r.value().text;
    return true;
}

std::string panelSettingsPath() {
    return (module::memory::engine::memoryDataDir() / "panel_settings.json")
        .string();
}

}  // namespace

REGISTER_TEST(memory_mcp_search_schema_contract,
              "memory_search: schema 合法 JSON 对象，query 必填 string、limit integer", {
    FakeRetriever fake;
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    DOCTEST_CHECK_EQ(tool.name(), std::string("memory_search"));
    DOCTEST_CHECK_FALSE(tool.description().empty());

    auto schema = platform::json::parse(tool.inputSchemaJson());
    DOCTEST_REQUIRE(schema.isOk());
    DOCTEST_REQUIRE(schema.value().isObject());
    const auto obj = schema.value().asObject();
    DOCTEST_REQUIRE(obj.get("type").isString());
    DOCTEST_CHECK_EQ(obj.get("type").asString(), std::string("object"));
    const auto props = obj.get("properties");
    DOCTEST_REQUIRE(props.isObject());
    const auto query = props.asObject().get("query");
    DOCTEST_REQUIRE(query.isObject());
    DOCTEST_CHECK_EQ(query.asObject().get("type").asString(),
                     std::string("string"));
    const auto limit = props.asObject().get("limit");
    DOCTEST_REQUIRE(limit.isObject());
    DOCTEST_CHECK_EQ(limit.asObject().get("type").asString(),
                     std::string("integer"));
    const auto required = obj.get("required");
    DOCTEST_REQUIRE(required.isArray());
    DOCTEST_REQUIRE(required.size() == 1);
    DOCTEST_CHECK_EQ(required[0].asString(), std::string("query"));
    return true;
})

REGISTER_TEST(memory_mcp_search_rejects_bad_arguments,
              "memory_search: 非 JSON/空 query/非字符串 query → isError 且不触检索", {
    FakeRetriever fake;
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    const char* badCases[] = {
        "not json",
        "{}",
        "{\"query\":\"\"}",
        "{\"query\":42}",
        "{\"query\":null}",
    };
    for (const char* args : badCases) {
        bool isError = false;
        std::string text;
        runTool(tool, args, &isError, &text);
        DOCTEST_CHECK_MESSAGE(isError,
                              std::string("参数 ") + args + " 应判错，实际 text=" + text);
        DOCTEST_CHECK_MESSAGE(
            text.find("缺少 query") != std::string::npos ||
                std::string(args) == "not json",
            std::string("参数 ") + args + " 文案不含「缺少 query」: " + text);
    }
    DOCTEST_CHECK_EQ(fake.calls, 0);
    return true;
})

REGISTER_TEST(memory_mcp_search_limit_clamped_and_default,
              "memory_search: limit 缺省 5/超界钳 1..10/非整数（含浮点 3.7）忽略取默认、整值 double 采纳，timeout=3000", {
    FakeRetriever fake;
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    struct Case {
        const char* args;
        int expectedLimit;
    };
    const Case cases[] = {
        {"{\"query\":\"q\"}", 5},
        {"{\"query\":\"q\",\"limit\":7}", 7},
        {"{\"query\":\"q\",\"limit\":0}", 1},
        {"{\"query\":\"q\",\"limit\":99}", 10},
        {"{\"query\":\"q\",\"limit\":\"x\"}", 5},
        // Double 形态：asInt() 恒 0（json.cpp Int/Double 字段互斥写入），
        // 必须走 asDouble 验整值——3.7 非整值取默认 5，2.0 整值采纳
        {"{\"query\":\"q\",\"limit\":3.7}", 5},
        {"{\"query\":\"q\",\"limit\":2.0}", 2},
        {"{\"query\":\"q\",\"limit\":1e20}", 10},
    };
    for (const Case& c : cases) {
        bool isError = false;
        std::string text;
        runTool(tool, c.args, &isError, &text);
        DOCTEST_CHECK_MESSAGE(
            !isError, std::string("参数 ") + c.args + " 不应判错: " + text);
        DOCTEST_CHECK_MESSAGE(fake.lastLimit == c.expectedLimit,
                              std::string("参数 ") + c.args + " limit 应为 " +
                                  std::to_string(c.expectedLimit) + "，实际 " +
                                  std::to_string(fake.lastLimit));
    }
    DOCTEST_CHECK_EQ(fake.lastTimeoutMs, 3000);
    DOCTEST_CHECK_EQ(fake.lastQuery, std::string("q"));
    return true;
})

REGISTER_TEST(memory_mcp_search_success_returns_paragraph,
              "memory_search: 命中 → isError=false，融合段落原文回传不二次加工", {
    FakeRetriever fake;
    fake.hits = {hitWithText("构建用 build-test-run.sh；测试基线 847 条")};
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    bool isError = true;
    std::string text;
    runTool(tool, "{\"query\":\"构建\"}", &isError, &text);
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_EQ(text,
                     std::string("构建用 build-test-run.sh；测试基线 847 条"));
    return true;
})

REGISTER_TEST(memory_mcp_search_empty_hits_text,
              "memory_search: 引擎在但空命中 → isError=false「未命中任何记忆」", {
    FakeRetriever fake;
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    bool isError = false;
    std::string text;
    runTool(tool, "{\"query\":\"不存在词\"}", &isError, &text);
    DOCTEST_CHECK_FALSE(isError);
    DOCTEST_CHECK_EQ(text, std::string("未命中任何记忆"));
    return true;
})

REGISTER_TEST(memory_mcp_search_timeout_text,
              "memory_search: 检索超时（NetworkTimeout）→ 专项超时文案", {
    FakeRetriever fake;
    fake.fail = true;
    fake.failCode = core::ErrorCode::NetworkTimeout;
    fake.failMessage = "Timeout was reached";
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    bool isError = false;
    std::string text;
    runTool(tool, "{\"query\":\"q\"}", &isError, &text);
    DOCTEST_CHECK_TRUE(isError);
    DOCTEST_CHECK_MESSAGE(
        text.find("记忆检索超时") != std::string::npos,
        "超时文案缺失: " + text);
    return true;
})

REGISTER_TEST(memory_mcp_search_connection_fail_text,
              "memory_search: 连接失败 → 连接失败文案透传底层原因并指向记忆页", {
    FakeRetriever fake;
    fake.fail = true;
    fake.failCode = core::ErrorCode::NetworkUnreachable;
    fake.failMessage = "Connection refused";
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    bool isError = false;
    std::string text;
    runTool(tool, "{\"query\":\"q\"}", &isError, &text);
    DOCTEST_CHECK_TRUE(isError);
    DOCTEST_CHECK_MESSAGE(
        text.find("记忆引擎未运行或连接失败") != std::string::npos,
        "连接失败文案缺失: " + text);
    DOCTEST_CHECK_MESSAGE(text.find("Connection refused") != std::string::npos,
                          "底层原因未透传: " + text);
    DOCTEST_CHECK_MESSAGE(text.find("记忆」页主控台") != std::string::npos,
                          "未给出口（记忆页主控台）: " + text);
    return true;
})

REGISTER_TEST(memory_mcp_search_truncates_long_output,
              "memory_search: 输出超 20000 码点 → UTF-8 边界截断 + 附注", {
    FakeRetriever fake;
    // 19999 个 ASCII + 1 个三字节 CJK 起始正好落在边界：截断必须保住完整码点
    std::string longText(19999, 'A');
    longText += "记记记记";
    fake.hits = {hitWithText(longText)};
    MemorySearchTool tool(std::shared_ptr<IMemoryRetriever>(
        &fake, [](IMemoryRetriever*) {}));
    bool isError = false;
    std::string text;
    runTool(tool, "{\"query\":\"q\"}", &isError, &text);
    DOCTEST_CHECK_FALSE(isError);
    const std::string suffix = "（已截断到 20000 字符）";
    DOCTEST_CHECK_MESSAGE(text.size() == 19999 + 3 + suffix.size(),
                          "截断后字节数异常: " + std::to_string(text.size()));
    DOCTEST_CHECK_MESSAGE(text.compare(19999, 3, "记") == 0,
                          "截断切进多字节序列中段");
    DOCTEST_CHECK_MESSAGE(
        text.size() >= suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0,
        "截断附注缺失");
    return true;
})

REGISTER_TEST(memory_mcp_search_registry_round_trip,
              "memory_search: 局部注册表注册→查找→注销→再注销 no-op→再注册", {
    core::mcp::McpToolRegistry registry;
    auto registered = module::memory::mcp::registerSearchTool(registry);
    DOCTEST_REQUIRE(registered.isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{1});
    auto found = registry.find("memory_search");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_FALSE(found.value()->description().empty());

    // 重名注册 fail（注册表既有语义），不致命
    auto dup = module::memory::mcp::registerSearchTool(registry);
    DOCTEST_CHECK_TRUE(dup.isFail());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{1});

    module::memory::mcp::unregisterSearchTool(registry);
    DOCTEST_CHECK_TRUE(registry.find("memory_search").isFail());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{0});
    // 注销不存在的名 = no-op 不崩
    module::memory::mcp::unregisterSearchTool(registry);
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{0});

    auto re = module::memory::mcp::registerSearchTool(registry);
    DOCTEST_REQUIRE(re.isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{1});
    return true;
})

REGISTER_TEST(memory_mcp_search_persistence_round_trip,
              "memory_search: 持久化往返 + setSearchEnabled 开关联动注册态 + 缺键默认 true + 写盘失败不注册", {
    // 前置：出厂根内落一份已知设置（防其它用例写过的 panel_settings.json 串味）
    module::memory::panel::MemoryPanelSettings seed;
    seed.mcpSearchEnabled = false;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(seed).isOk());
    DOCTEST_CHECK_FALSE(module::memory::mcp::searchEnabledBySetting());

    core::mcp::McpToolRegistry registry;
    auto on = module::memory::mcp::setSearchEnabled(registry, true);
    DOCTEST_REQUIRE(on.isOk());
    DOCTEST_CHECK_TRUE(registry.find("memory_search").isOk());
    DOCTEST_CHECK_TRUE(module::memory::mcp::searchEnabledBySetting());

    auto off = module::memory::mcp::setSearchEnabled(registry, false);
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_TRUE(registry.find("memory_search").isFail());
    DOCTEST_CHECK_FALSE(module::memory::mcp::searchEnabledBySetting());

    // 开关回写真实文件：缺键旧文件（只含 sanitizeEnabled）→ mcpSearchEnabled 默认 true
    FILE* f = std::fopen(panelSettingsPath().c_str(), "wb");
    DOCTEST_REQUIRE(f != nullptr);
    const char* legacy = R"({"sanitizeEnabled":true,"recallThresholdPct":85})";
    std::fwrite(legacy, 1, std::strlen(legacy), f);
    std::fclose(f);
    DOCTEST_CHECK_TRUE(module::memory::mcp::searchEnabledBySetting());

    // 写盘失败 → fail 且注册态不动（§6.2 回弹依据）：把数据根指到普通文件上，
    // memoryDataDir 展开进文件路径之下 → fopen 必败（ScopedEnv 析构还原，不泄漏）
    const std::string blockerFile =
        (module::memory::engine::memoryDataDir().parent_path() / "not_a_dir")
            .string();
    {
        std::error_code ec;
        std::filesystem::remove(blockerFile, ec);  // 幂等清理上轮残留
        std::ofstream creator(blockerFile);
        DOCTEST_REQUIRE(creator.good());
    }
    {
        test_support::ScopedEnv scopedRoot("DEVDESK_DATA_ROOT", blockerFile);
        auto failed = module::memory::mcp::setSearchEnabled(registry, true);
        DOCTEST_CHECK_MESSAGE(failed.isFail(),
                              "写盘失败必须上抛 fail，实际 ok");
        DOCTEST_CHECK_MESSAGE(registry.find("memory_search").isFail(),
                              "写盘失败时注册态必须保持不变");
    }
    std::error_code removeEc;
    std::filesystem::remove(blockerFile, removeEc);
    return true;
})

REGISTER_TEST(memory_mcp_bridge_default_and_injection,
              "memory_mcp 桥: 未装配走默认桩；注入后转发并复位", {
    // 出厂态（resetCaseGlobals 已复位两槽为 nullptr）：status 全默认、setEnabled fail
    DOCTEST_CHECK_FALSE(app::memory_mcp::status().switchOn);
    auto stub = app::memory_mcp::setEnabled(true);
    DOCTEST_REQUIRE(stub.isFail());
    DOCTEST_CHECK_EQ(stub.errorCode(), core::ErrorCode::Internal);

    static bool recordedOn = false;
    static int calls = 0;
    recordedOn = false;
    calls = 0;
    app::memory_mcp::setHandlers(
        [] { return app::memory_mcp::LiveStatus{true}; },
        [](bool on) {
            ++calls;
            recordedOn = on;
            return core::Result<void>::ok();
        });
    DOCTEST_CHECK_TRUE(app::memory_mcp::status().switchOn);
    auto r = app::memory_mcp::setEnabled(false);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_FALSE(recordedOn);

    // 恢复出厂（等价 resetCaseGlobals 的复位动作，防串味到后续用例）
    app::memory_mcp::setHandlers(nullptr, nullptr);
    DOCTEST_CHECK_FALSE(app::memory_mcp::status().switchOn);
    return true;
})
