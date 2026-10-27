// MCP 会话表：只测内存表语义（id 格式、淘汰、注入生成器）。服务级生命周期在
// test_mcp_service.cpp（B3）覆盖。
#include "doctest.h"

#include <cctype>
#include <string>
#include <vector>

#include "module_mcp/mcp_session_store.h"

REGISTER_TEST(mcp_session_store_default_id_is_32_hex,
              "mcp session store: 默认生成器产出 32 位十六进制且只含可见 ASCII", {
    module::mcp::McpSessionStore store;
    const std::string id = store.create();
    DOCTEST_CHECK_EQ(id.size(), std::size_t{32});
    for (char c : id) {
        DOCTEST_CHECK(std::isxdigit(static_cast<unsigned char>(c)) != 0);
        DOCTEST_CHECK(c >= 0x21 && c <= 0x7E);
    }
    DOCTEST_CHECK(store.has(id));
    DOCTEST_CHECK_EQ(store.size(), std::size_t{1});
    store.remove(id);
    DOCTEST_CHECK_FALSE(store.has(id));
    DOCTEST_CHECK_EQ(store.size(), std::size_t{0});
    return true;
})

REGISTER_TEST(mcp_session_store_two_ids_differ,
              "mcp session store: 连续 create 产出不同 id（防会话互相顶替）", {
    module::mcp::McpSessionStore store;
    const std::string first = store.create();
    const std::string second = store.create();
    DOCTEST_CHECK_NE(first, second);
    DOCTEST_CHECK(store.has(first));
    DOCTEST_CHECK(store.has(second));
    DOCTEST_CHECK_EQ(store.size(), std::size_t{2});
    return true;
})

REGISTER_TEST(mcp_session_store_injected_generator,
              "mcp session store: 注入生成器决定 create 返回值（测试可确定性断言）", {
    int counter = 0;
    module::mcp::McpSessionStore store([&counter] {
        ++counter;
        return "session-" + std::to_string(counter);
    });
    DOCTEST_CHECK_EQ(store.create(), std::string("session-1"));
    DOCTEST_CHECK_EQ(store.create(), std::string("session-2"));
    DOCTEST_CHECK_EQ(store.size(), std::size_t{2});
    DOCTEST_CHECK(store.has("session-1"));
    DOCTEST_CHECK(store.has("session-2"));
    return true;
})

REGISTER_TEST(mcp_session_store_evicts_oldest_beyond_64,
              "mcp session store: 第 65 个会话淘汰最旧（有界内存，防会话无限增长）", {
    int counter = 0;
    module::mcp::McpSessionStore store([&counter] {
        return "id_" + std::to_string(counter++);
    });
    std::vector<std::string> ids;
    ids.reserve(65);
    for (int i = 0; i < 65; ++i) {
        ids.push_back(store.create());
    }
    DOCTEST_CHECK_EQ(store.size(), std::size_t{64});
    DOCTEST_CHECK_FALSE(store.has(ids[0]));   // 最旧被淘汰
    DOCTEST_CHECK(store.has(ids[1]));
    DOCTEST_CHECK(store.has(ids[64]));
    // 淘汰后新会话照常工作
    const std::string next = store.create();
    DOCTEST_CHECK(store.has(next));
    DOCTEST_CHECK_FALSE(store.has(ids[1]));
    return true;
})

REGISTER_TEST(mcp_session_store_remove_unknown_is_noop,
              "mcp session store: remove 未知 id 不崩且不影响已有会话", {
    module::mcp::McpSessionStore store;
    const std::string id = store.create();
    store.remove("never-created");
    DOCTEST_CHECK(store.has(id));
    DOCTEST_CHECK_EQ(store.size(), std::size_t{1});
    return true;
})

REGISTER_TEST(mcp_session_store_clear_all,
              "mcp session store: clear 清空全部会话（服务 stop/restart 即全部失效）", {
    module::mcp::McpSessionStore store;
    const std::string a = store.create();
    const std::string b = store.create();
    store.clear();
    DOCTEST_CHECK_EQ(store.size(), std::size_t{0});
    DOCTEST_CHECK_FALSE(store.has(a));
    DOCTEST_CHECK_FALSE(store.has(b));
    const std::string fresh = store.create();
    DOCTEST_CHECK(store.has(fresh));
    return true;
})