// tests/test_modules/test_view_state.cpp
// 视图状态持久化：markdown / todo 两个 view state store 的存读往返、缺失/损坏回退
// 默认、净化钳制（等价「重启恢复」的持久层验证）。
// 跨文件前提：
//   - 两份状态各存成 app_config 表的一行（键见 kMarkdownViewStateKey / kTodoViewStateKey），
//     经显式传入的 SqliteDb 连接读写；库未就绪（nullptr）时 save/loadChecked 如实失败。
//   - core::atomicWriteFile 的用例与配置落点无关，仍走临时文件。
#include "doctest.h"

#include "core/atomic_write.h"
#include "core/app_config.h"
#include "module_markdown/md_view_state.h"
#include "module_todo/todo_view_state.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// 往 app_config 行里塞一段非 JSON 文本
void writeGarbageRow(core::data::SqliteDb& db, const char* key) {
    test_support::writeRawConfigRow(&db, key, "{ this is ]not json[");
}

std::filesystem::path tempFile(const std::string& name) {
    return test_temp::tempPath(name);
}

void removeFile(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace

REGISTER_TEST(markdown_view_state_roundtrip, "markdown: 视图状态存读往返（[23] 重启恢复）", {
    using namespace module::markdown;
    test_support::TestDb env("devdesk_test_md_view_state_", "rt");
    core::data::SqliteDb& db = env.handle();
    {
        MarkdownViewStateStore store(&db);
        MarkdownViewState st;
        st.lastPath = "工作/周报.md";
        st.mode = 2;  // Preview
        st.collapsedDirs = {"工作", "草稿"};
        st.selectedDir = "工作/周会";   // 目录树选中目录（新建条目的父目录）
        DOCTEST_CHECK(store.save(st).isOk());
    }
    // 新 store 读同一行 = 模拟重启
    MarkdownViewStateStore reopened(&db);
    const MarkdownViewState loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.lastPath, std::string("工作/周报.md"));
    DOCTEST_CHECK_EQ(loaded.mode, 2);
    DOCTEST_CHECK_EQ(loaded.collapsedDirs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(loaded.collapsedDirs[0], std::string("工作"));
    DOCTEST_CHECK_EQ(loaded.collapsedDirs[1], std::string("草稿"));
    // 选中目录跨会话恢复：丢了它 = 重启后新建笔记又散回根目录
    DOCTEST_CHECK_EQ(loaded.selectedDir, std::string("工作/周会"));
    DOCTEST_CHECK(test_support::configRowExists(&db, kMarkdownViewStateKey));
    return true;
});

REGISTER_TEST(markdown_view_state_fallback, "markdown: 视图状态缺失/损坏回退默认（[23]）", {
    using namespace module::markdown;
    test_support::TestDb env("devdesk_test_md_view_state_", "missing");
    core::data::SqliteDb& db = env.handle();

    // 键不存在 = 首次启动，读出默认值
    MarkdownViewStateStore store(&db);
    const MarkdownViewState fresh = store.load();
    DOCTEST_CHECK(fresh.lastPath.empty());
    DOCTEST_CHECK_EQ(fresh.mode, 0);
    DOCTEST_CHECK(fresh.collapsedDirs.empty());
    DOCTEST_CHECK(fresh.selectedDir.empty());   // "" = 根目录（合法默认，不是"无选中"）
    // 损坏内容：load() 回落默认值不崩不抛；loadChecked() 如实报错（两者口径不同）
    writeGarbageRow(db, kMarkdownViewStateKey);
    const MarkdownViewState broken = store.load();
    DOCTEST_CHECK(broken.lastPath.empty());
    DOCTEST_CHECK_EQ(broken.mode, 0);
    auto brokenChecked = store.loadChecked();
    DOCTEST_CHECK(brokenChecked.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(brokenChecked.errorCode()),
                     static_cast<int>(core::ErrorCode::JsonParseError));
    // 合法 json 但字段乱型：逐字段容错
    test_support::writeRawConfigRow(
        &db, kMarkdownViewStateKey,
        R"({"lastPath": 123, "mode": "x", "collapsedDirs": [1, "ok", null]})");
    const MarkdownViewState weird = store.load();
    DOCTEST_CHECK(weird.lastPath.empty());
    DOCTEST_CHECK_EQ(weird.mode, 0);
    DOCTEST_CHECK_EQ(weird.collapsedDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(weird.collapsedDirs[0], std::string("ok"));
    DOCTEST_CHECK(weird.selectedDir.empty());   // 乱型 selectedDir → 回默认
    return true;
});

REGISTER_TEST(markdown_view_state_sanitized, "markdown: 视图状态 mode 钳制 + 路径穿越丢弃（[23]）", {
    using namespace module::markdown;
    MarkdownViewState st;
    st.lastPath = "../outside.md";
    st.mode = 7;
    st.collapsedDirs = {"../../etc", "好目录", ""};
    st.selectedDir = "../outside";
    const MarkdownViewState out = MarkdownViewStateStore::sanitized(st);
    DOCTEST_CHECK(out.lastPath.empty());   // 含 ".." → 整条丢弃
    DOCTEST_CHECK_EQ(out.mode, 0);         // 越界 → 回落 Edit
    DOCTEST_CHECK_EQ(out.collapsedDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(out.collapsedDirs[0], std::string("好目录"));  // 空串/穿越剔除
    DOCTEST_CHECK(out.selectedDir.empty());  // 含 ".." → 丢回根目录（""）
    // selectedDir 合法值原样保留；"" 也保留（根目录是合法值，不是"无选中"）
    MarkdownViewState okSel;
    okSel.selectedDir = "工作/周会";
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(okSel).selectedDir,
                     std::string("工作/周会"));
    MarkdownViewState rootSel;
    DOCTEST_CHECK(MarkdownViewStateStore::sanitized(rootSel).selectedDir.empty());
    // mode 持久化域是 0..2（0=Edit）；域内值原样保留是三态恢复的前提
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

REGISTER_TEST(markdown_view_state_db_not_ready, "markdown: 库未就绪 → save 失败 / load 回默认（E2）", {
    using namespace module::markdown;
    MarkdownViewStateStore store(nullptr);
    MarkdownViewState st;
    st.lastPath = "a.md";
    st.mode = 1;
    auto saved = store.save(st);
    DOCTEST_CHECK(saved.isFail());   // 绝不静默成功：用户改的视图状态会被丢掉
    auto loaded = store.loadChecked();
    DOCTEST_CHECK(loaded.isFail());
    DOCTEST_CHECK_EQ(loaded.valueOr(MarkdownViewState{}).mode, 0);
    // load() 本身不返回 Result，但必须给的是默认值而不是残留脏值
    DOCTEST_CHECK(store.load().lastPath.empty());
    return true;
});

REGISTER_TEST(todo_view_state_roundtrip, "todo: 视图状态存读往返含整值偏移（[23] 重启恢复）", {
    using namespace module::todo;
    test_support::TestDb env("devdesk_test_todo_view_state_", "rt");
    core::data::SqliteDb& db = env.handle();
    {
        TodoViewStateStore store(&db);
        TodoViewState st;
        st.columnScrolls = {{7, 0.0f}, {11, 512.0f}, {10, 150.25f}};
        st.selectedTaskId = 42;
        DOCTEST_CHECK(store.save(st).isOk());
    }
    TodoViewStateStore reopened(&db);
    const TodoViewState loaded = reopened.load();
    // 512.0 序列化后可能写成 "512"（Int 分支）或 "512.0"（Double 分支）——两种
    // 都要认，只读 asDouble() 会丢值，串了就是滚动位置归零 bug
    DOCTEST_CHECK_EQ(loaded.columnScrolls.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(7), 0.0f);
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(11), 512.0f);
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(10), 150.25f);
    DOCTEST_CHECK_EQ(loaded.selectedTaskId, static_cast<int64_t>(42));
    DOCTEST_CHECK(test_support::configRowExists(&db, kTodoViewStateKey));
    return true;
});

REGISTER_TEST(todo_view_state_fallback, "todo: 视图状态缺失/损坏回退默认（[23]）", {
    using namespace module::todo;
    test_support::TestDb env("devdesk_test_todo_view_state_", "missing");
    core::data::SqliteDb& db = env.handle();

    TodoViewStateStore store(&db);
    const TodoViewState fresh = store.load();
    DOCTEST_CHECK(fresh.columnScrolls.empty());
    DOCTEST_CHECK_EQ(fresh.selectedTaskId, static_cast<int64_t>(0));
    writeGarbageRow(db, kTodoViewStateKey);
    const TodoViewState broken = store.load();
    DOCTEST_CHECK(broken.columnScrolls.empty());
    DOCTEST_CHECK(store.loadChecked().isFail());
    // 乱键/乱型逐条容错：仅保留合法列 id + 数字偏移
    test_support::writeRawConfigRow(
        &db, kTodoViewStateKey,
        R"({"columnScrolls": {"abc": 1.0, "5": "x", "8": 20.5, "0": 9.0}, "selectedTaskId": true})");
    const TodoViewState weird = store.load();
    DOCTEST_CHECK_EQ(weird.columnScrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(weird.columnScrolls.at(8), 20.5f);
    DOCTEST_CHECK_EQ(weird.selectedTaskId, static_cast<int64_t>(0));
    return true;
});

REGISTER_TEST(todo_view_state_sanitize_prune, "todo: 负值/乱 id 净化 + pruneScrolls 删列收敛（[23]）", {
    using namespace module::todo;
    TodoViewState st;
    st.columnScrolls = {{3, -5.0f}, {4, 80.0f}, {0, 10.0f}, {-2, 10.0f}};
    st.selectedTaskId = -7;
    const TodoViewState out = TodoViewStateStore::sanitized(st);
    DOCTEST_CHECK_EQ(out.columnScrolls.size(), static_cast<size_t>(2));  // id<=0 剔除
    DOCTEST_CHECK_EQ(out.columnScrolls.at(3), 0.0f);                     // 负偏移归 0
    DOCTEST_CHECK_EQ(out.columnScrolls.at(4), 80.0f);
    DOCTEST_CHECK_EQ(out.selectedTaskId, static_cast<int64_t>(0));

    // 串列 bug 的持久层等价验证：偏移跟 id 走，列重排/删除不串
    std::map<int64_t, float> scrolls = {{10, 200.0f}, {11, 50.0f}};
    const size_t removed = TodoViewStateStore::pruneScrolls(scrolls, std::vector<int64_t>{11});  // 列 10 已删
    DOCTEST_CHECK_EQ(removed, static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.at(11), 50.0f);  // 存活列的偏移原样保留
    return true;
});

REGISTER_TEST(todo_view_state_db_not_ready, "todo: 库未就绪 → save 失败 / load 回默认（E2）", {
    using namespace module::todo;
    TodoViewStateStore store(nullptr);
    TodoViewState st;
    st.columnScrolls = {{1, 10.0f}};
    st.selectedTaskId = 5;
    DOCTEST_CHECK(store.save(st).isFail());
    DOCTEST_CHECK(store.loadChecked().isFail());
    DOCTEST_CHECK(store.load().columnScrolls.empty());
    return true;
});

// 后一次 save 整份替换（旧列 id 的滚动记录不残留），且写入是原子的（中途失败不
// 留半份状态）
REGISTER_TEST(todo_view_state_save_replaces_wholesale,
              "todo: 再次 save 整份替换（旧列 id 记录不残留，E2）", {
    using namespace module::todo;
    test_support::TestDb env("devdesk_test_todo_view_state_", "replace");
    core::data::SqliteDb& db = env.handle();
    TodoViewStateStore store(&db);

    TodoViewState first;
    first.columnScrolls = {{7, 120.0f}, {11, 512.0f}};
    first.selectedTaskId = 42;
    DOCTEST_REQUIRE(store.save(first).isOk());

    TodoViewState second;
    second.columnScrolls = {{9, 999.0f}};   // 列 7 / 11 都没了
    second.selectedTaskId = 7;
    DOCTEST_REQUIRE(store.save(second).isOk());

    const TodoViewState loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.columnScrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(loaded.columnScrolls.find(7) == loaded.columnScrolls.end());
    DOCTEST_CHECK(loaded.columnScrolls.find(11) == loaded.columnScrolls.end());
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(9), 999.0f);
    DOCTEST_CHECK_EQ(loaded.selectedTaskId, static_cast<int64_t>(7));
    return true;
})

// 失败注入不靠 sleep/杀进程：把临时文件路径 <target>.devdesk-tmp 用目录占住
// （打开必失败），或让目标本身是不可替换的非空目录（remove/rename 双失败）——
// 两种都稳定可重复，且能顺带验证「失败时目标原内容一字未变 + 不留临时文件」。

namespace {

std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::filesystem::path tmpPathOf(const std::filesystem::path& p) {
    return std::filesystem::path(p.string() + ".devdesk-tmp");
}

} // namespace

REGISTER_TEST(core_atomic_write_roundtrip, "core: atomicWriteFile 往返逐字节一致 + 不留临时文件（THS-03）", {
    const auto file = tempFile("devdesk_test_atomic_write.json");
    const auto tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    removeFile(file);

    DOCTEST_REQUIRE(core::atomicWriteFile(file, "{\"a\":1}\n").isOk());
    DOCTEST_CHECK_EQ(readAllBytes(file), std::string("{\"a\":1}\n"));
    // 目标已存在 → 走 remove + rename 替换分支
    DOCTEST_REQUIRE(core::atomicWriteFile(file, "second\n").isOk());
    DOCTEST_CHECK_EQ(readAllBytes(file), std::string("second\n"));
    // 替换是可观测的原子形式：tmp 换名进目标，成功后无残留
    DOCTEST_CHECK_FALSE(std::filesystem::exists(tmp));

    removeFile(file);
    return true;
});

REGISTER_TEST(core_atomic_write_failure_keeps_old, "core: 原子写失败不破坏旧内容、不留临时文件（THS-03）", {
    const auto file = tempFile("devdesk_test_atomic_write_fail.json");
    const auto tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    removeFile(file);
    DOCTEST_REQUIRE(core::atomicWriteFile(file, "OLD\n").isOk());

    // 场景 1：临时文件路径被目录占住 → 打开临时文件即失败
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmp, ec));
    const auto r = core::atomicWriteFile(file, "NEW\n");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(readAllBytes(file), std::string("OLD\n"));  // 原文件一字未变
    std::filesystem::remove(tmp, ec);  // 占位目录是测试自己造的，手动清掉

    // 场景 2：目标本身是不可替换的非空目录 → remove/rename 双失败
    const auto dirTarget = tempFile("devdesk_test_atomic_write_dir");
    const auto dirTmp = tmpPathOf(dirTarget);
    std::filesystem::remove_all(dirTarget, ec);
    std::filesystem::remove_all(dirTmp, ec);
    DOCTEST_REQUIRE(std::filesystem::create_directories(dirTarget, ec));
    {
        std::ofstream inner(dirTarget / "inner.txt", std::ios::trunc);
        inner << "keep";
    }
    const auto r2 = core::atomicWriteFile(dirTarget, "X\n");
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK(std::filesystem::is_directory(dirTarget));
    DOCTEST_CHECK_EQ(readAllBytes(dirTarget / "inner.txt"), std::string("keep"));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(dirTmp));  // 失败路径也清掉了临时文件

    std::filesystem::remove_all(dirTarget, ec);
    removeFile(file);
    return true;
});
