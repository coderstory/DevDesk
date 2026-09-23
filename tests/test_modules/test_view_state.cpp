// tests/test_modules/test_view_state.cpp
// 审计 [23] 视图状态持久化：markdown / todo 两个 view_state.json store 的
// save/load 往返、缺失/损坏回退默认、净化钳制（等价「重启恢复」的持久层验证）。
#include "doctest.h"

#include "module_markdown/md_view_state.h"
#include "module_todo/todo_view_state.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::filesystem::path tempFile(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

void removeFile(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

void writeGarbage(const std::filesystem::path& p) {
    std::ofstream f(p, std::ios::trunc);
    f << "{ this is ]not json[";
}

// REGISTER_TEST 宏体会按顶层逗号切分参数（尖括号不保护），多参模板类型
// 声明一律经此 helper + auto 规避
std::map<int64_t, float> makeIdFloatMap(std::initializer_list<std::pair<const int64_t, float>> items) {
    return std::map<int64_t, float>(items);
}
} // namespace

// ==================== markdown ====================

REGISTER_TEST(markdown_view_state_roundtrip, "markdown: 视图状态存读往返（[23] 重启恢复）", {
    using namespace module::markdown;
    const auto file = tempFile("devdesk_test_md_view_state.json");
    removeFile(file);
    {
        MarkdownViewStateStore store(file);
        MarkdownViewState st;
        st.lastPath = "工作/周报.md";
        st.mode = 2;  // Preview
        st.collapsedDirs = (std::vector<std::string>{"工作", "草稿"});
        DOCTEST_CHECK(store.save(st).isOk());
    }
    // 新 store 读同一文件 = 模拟重启
    MarkdownViewStateStore reopened(file);
    const MarkdownViewState loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.lastPath, std::string("工作/周报.md"));
    DOCTEST_CHECK_EQ(loaded.mode, 2);
    DOCTEST_CHECK_EQ(loaded.collapsedDirs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(loaded.collapsedDirs[0], std::string("工作"));
    DOCTEST_CHECK_EQ(loaded.collapsedDirs[1], std::string("草稿"));
    removeFile(file);
    return true;
});

REGISTER_TEST(markdown_view_state_fallback, "markdown: 视图状态缺失/损坏回退默认（[23]）", {
    using namespace module::markdown;
    const auto file = tempFile("devdesk_test_md_view_state_missing.json");
    removeFile(file);
    // 文件不存在：首次启动，默认值（Edit / 无选中 / 无收起目录）
    MarkdownViewStateStore store(file);
    const MarkdownViewState fresh = store.load();
    DOCTEST_CHECK(fresh.lastPath.empty());
    DOCTEST_CHECK_EQ(fresh.mode, 0);
    DOCTEST_CHECK(fresh.collapsedDirs.empty());
    // 损坏文件：回退默认，不崩不抛
    writeGarbage(file);
    const MarkdownViewState broken = store.load();
    DOCTEST_CHECK(broken.lastPath.empty());
    DOCTEST_CHECK_EQ(broken.mode, 0);
    // 合法 json 但字段乱型：逐字段容错
    {
        std::ofstream f(file, std::ios::trunc);
        f << R"({"lastPath": 123, "mode": "x", "collapsedDirs": [1, "ok", null]})";
    }
    const MarkdownViewState weird = store.load();
    DOCTEST_CHECK(weird.lastPath.empty());
    DOCTEST_CHECK_EQ(weird.mode, 0);
    DOCTEST_CHECK_EQ(weird.collapsedDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(weird.collapsedDirs[0], std::string("ok"));
    removeFile(file);
    return true;
});

REGISTER_TEST(markdown_view_state_sanitized, "markdown: 视图状态 mode 钳制 + 路径穿越丢弃（[23]）", {
    using namespace module::markdown;
    MarkdownViewState st;
    st.lastPath = "../outside.md";
    st.mode = 7;
    st.collapsedDirs = (std::vector<std::string>{"../../etc", "好目录", ""});
    const MarkdownViewState out = MarkdownViewStateStore::sanitized(st);
    DOCTEST_CHECK(out.lastPath.empty());   // 含 ".." → 整条丢弃
    DOCTEST_CHECK_EQ(out.mode, 0);         // 越界 → Edit
    DOCTEST_CHECK_EQ(out.collapsedDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(out.collapsedDirs[0], std::string("好目录"));  // 空串/穿越剔除
    // 合法域内的 mode 原样保留（三态持久化的前提）
    for (int m = 0; m <= 2; ++m) {
        MarkdownViewState ok;
        ok.mode = m;
        DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(ok).mode, m);
    }
    MarkdownViewState neg;
    neg.mode = -1;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(neg).mode, 0);
    return true;
});

// ==================== todo ====================

REGISTER_TEST(todo_view_state_roundtrip, "todo: 视图状态存读往返含整值偏移（[23] 重启恢复）", {
    using namespace module::todo;
    const auto file = tempFile("devdesk_test_todo_view_state.json");
    removeFile(file);
    {
        TodoViewStateStore store(file);
        TodoViewState st;
        st.columnScrolls = (std::map<int64_t, float>{{7, 0.0f}, {11, 512.0f}, {10, 150.25f}});
        st.selectedTaskId = 42;
        DOCTEST_CHECK(store.save(st).isOk());
    }
    TodoViewStateStore reopened(file);
    const TodoViewState loaded = reopened.load();
    // 512.0 序列化经 %.17g 成 "512"（Int 分支）——必须不丢，串了就是滚动位置归零 bug
    DOCTEST_CHECK_EQ(loaded.columnScrolls.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(7), 0.0f);
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(11), 512.0f);
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(10), 150.25f);
    DOCTEST_CHECK_EQ(loaded.selectedTaskId, static_cast<int64_t>(42));
    removeFile(file);
    return true;
});

REGISTER_TEST(todo_view_state_fallback, "todo: 视图状态缺失/损坏回退默认（[23]）", {
    using namespace module::todo;
    const auto file = tempFile("devdesk_test_todo_view_state_missing.json");
    removeFile(file);
    TodoViewStateStore store(file);
    const TodoViewState fresh = store.load();
    DOCTEST_CHECK(fresh.columnScrolls.empty());
    DOCTEST_CHECK_EQ(fresh.selectedTaskId, static_cast<int64_t>(0));
    writeGarbage(file);
    const TodoViewState broken = store.load();
    DOCTEST_CHECK(broken.columnScrolls.empty());
    // 乱键/乱型逐条容错：仅保留合法列 id + 数字偏移
    {
        std::ofstream f(file, std::ios::trunc);
        f << R"({"columnScrolls": {"abc": 1.0, "5": "x", "8": 20.5, "0": 9.0}, "selectedTaskId": true})";
    }
    const TodoViewState weird = store.load();
    DOCTEST_CHECK_EQ(weird.columnScrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(weird.columnScrolls.at(8), 20.5f);
    DOCTEST_CHECK_EQ(weird.selectedTaskId, static_cast<int64_t>(0));
    removeFile(file);
    return true;
});

REGISTER_TEST(todo_view_state_sanitize_prune, "todo: 负值/乱 id 净化 + pruneScrolls 删列收敛（[23]）", {
    using namespace module::todo;
    TodoViewState st;
    st.columnScrolls = (std::map<int64_t, float>{{3, -5.0f}, {4, 80.0f}, {0, 10.0f}, {-2, 10.0f}});
    st.selectedTaskId = -7;
    const TodoViewState out = TodoViewStateStore::sanitized(st);
    DOCTEST_CHECK_EQ(out.columnScrolls.size(), static_cast<size_t>(2));  // id<=0 剔除
    DOCTEST_CHECK_EQ(out.columnScrolls.at(3), 0.0f);                     // 负偏移归 0
    DOCTEST_CHECK_EQ(out.columnScrolls.at(4), 80.0f);
    DOCTEST_CHECK_EQ(out.selectedTaskId, static_cast<int64_t>(0));

    // 审计 [23] 串列 bug 的持久层等价验证：偏移跟 id 走，列重排/删除不串
    auto scrolls = makeIdFloatMap({{10, 200.0f}, {11, 50.0f}});
    const size_t removed = TodoViewStateStore::pruneScrolls(scrolls, (std::vector<int64_t>{11}));  // 列 10 已删
    DOCTEST_CHECK_EQ(removed, static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.at(11), 50.0f);  // 存活列的偏移原样保留
    return true;
});
