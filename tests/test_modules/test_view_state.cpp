// tests/test_modules/test_view_state.cpp
// 审计 [23] 视图状态持久化：markdown / todo 两个 view_state.json store 的
// save/load 往返、缺失/损坏回退默认、净化钳制（等价「重启恢复」的持久层验证）。
#include "doctest.h"

#include "core/atomic_write.h"
#include "module_markdown/md_view_state.h"
#include "module_todo/todo_view_state.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path tempFile(const std::string& name) {
    return test_temp::tempPath(name);
}

void removeFile(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

void writeGarbage(const std::filesystem::path& p) {
    std::ofstream f(p, std::ios::trunc);
    f << "{ this is ]not json[";
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
        st.collapsedDirs = {"工作", "草稿"};
        st.selectedDir = "工作/周会";   // 2026-09-29：目录树选中目录（新建条目的父目录）
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
    // 选中目录跨会话恢复（丢了它 = 重启后新建笔记又散回根目录）
    DOCTEST_CHECK_EQ(loaded.selectedDir, std::string("工作/周会"));
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
    DOCTEST_CHECK(fresh.selectedDir.empty());   // "" = 根目录（合法默认，不是"无选中"）
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
    DOCTEST_CHECK(weird.selectedDir.empty());   // 乱型 selectedDir → 回默认
    removeFile(file);
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
    DOCTEST_CHECK_EQ(out.mode, 0);         // 越界 → Edit
    DOCTEST_CHECK_EQ(out.collapsedDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(out.collapsedDirs[0], std::string("好目录"));  // 空串/穿越剔除
    DOCTEST_CHECK(out.selectedDir.empty());  // 含 ".." → 丢回根目录（""）
    // 合法选中目录原样保留；"" 也保留（根目录是合法值）
    MarkdownViewState okSel;
    okSel.selectedDir = "工作/周会";
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(okSel).selectedDir,
                     std::string("工作/周会"));
    MarkdownViewState rootSel;
    DOCTEST_CHECK(MarkdownViewStateStore::sanitized(rootSel).selectedDir.empty());
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
        st.columnScrolls = {{7, 0.0f}, {11, 512.0f}, {10, 150.25f}};
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
    st.columnScrolls = {{3, -5.0f}, {4, 80.0f}, {0, 10.0f}, {-2, 10.0f}};
    st.selectedTaskId = -7;
    const TodoViewState out = TodoViewStateStore::sanitized(st);
    DOCTEST_CHECK_EQ(out.columnScrolls.size(), static_cast<size_t>(2));  // id<=0 剔除
    DOCTEST_CHECK_EQ(out.columnScrolls.at(3), 0.0f);                     // 负偏移归 0
    DOCTEST_CHECK_EQ(out.columnScrolls.at(4), 80.0f);
    DOCTEST_CHECK_EQ(out.selectedTaskId, static_cast<int64_t>(0));

    // 审计 [23] 串列 bug 的持久层等价验证：偏移跟 id 走，列重排/删除不串
    std::map<int64_t, float> scrolls = {{10, 200.0f}, {11, 50.0f}};
    const size_t removed = TodoViewStateStore::pruneScrolls(scrolls, std::vector<int64_t>{11});  // 列 10 已删
    DOCTEST_CHECK_EQ(removed, static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(scrolls.at(11), 50.0f);  // 存活列的偏移原样保留
    return true;
});

// ==================== core 原子写（审计 THS-03）====================
// 失败注入不靠 sleep/杀进程：把临时文件路径 <target>.devdesk-tmp 用目录占住
// （打开必失败），或让目标本身是不可替换的非空目录（remove/rename 双失败）——
// 两种都稳定可重复，且能顺带验证"失败时目标原内容一字未变 + 不留临时文件"。

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
    // 目标已存在 → 走 remove + rename 替换分支；新内容完整替换旧内容
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

REGISTER_TEST(todo_view_state_save_failure_keeps_old, "todo: 视图状态原子写失败不破坏旧文件（THS-03）", {
    using namespace module::todo;
    const auto file = tempFile("devdesk_test_todo_view_state_atomic.json");
    const auto tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    removeFile(file);

    TodoViewStateStore store(file);
    TodoViewState st;
    st.columnScrolls = {{7, 120.0f}};
    st.selectedTaskId = 42;
    DOCTEST_REQUIRE(store.save(st).isOk());

    // 落盘通道被占住 → 保存失败，但上一次成功落盘的状态必须原样可读
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmp, ec));
    TodoViewState changed;
    changed.columnScrolls = {{9, 999.0f}};
    changed.selectedTaskId = 7;
    DOCTEST_CHECK(store.save(changed).isFail());

    TodoViewStateStore reopened(file);
    const TodoViewState loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.columnScrolls.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(loaded.columnScrolls.at(7), 120.0f);
    DOCTEST_CHECK_EQ(loaded.selectedTaskId, static_cast<int64_t>(42));

    std::filesystem::remove(tmp, ec);
    removeFile(file);
    return true;
});

REGISTER_TEST(markdown_view_state_save_failure_keeps_old, "markdown: 视图状态原子写失败不破坏旧文件（THS-03）", {
    using namespace module::markdown;
    const auto file = tempFile("devdesk_test_md_view_state_atomic.json");
    const auto tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    removeFile(file);

    MarkdownViewStateStore store(file);
    MarkdownViewState st;
    st.lastPath = "工作/周报.md";
    st.mode = 2;
    st.collapsedDirs = {"工作", "笔记"};
    DOCTEST_REQUIRE(store.save(st).isOk());
    // 成功后字节与旧实现一致：serialize 结果 + "\n" 尾
    {
        const std::string bytes = readAllBytes(file);
        DOCTEST_CHECK(!bytes.empty());
        DOCTEST_CHECK(bytes.find("工作/周报.md") != std::string::npos);
        DOCTEST_CHECK_EQ(bytes.back(), '\n');
    }

    // 落盘通道被占住 → 保存失败，但上一次成功落盘的状态必须原样可读
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmp, ec));
    MarkdownViewState changed;
    changed.lastPath = "别的.md";
    changed.mode = 0;
    DOCTEST_CHECK(store.save(changed).isFail());

    MarkdownViewStateStore reopened(file);
    const MarkdownViewState loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.lastPath, std::string("工作/周报.md"));
    DOCTEST_CHECK_EQ(loaded.mode, 2);
    DOCTEST_CHECK_EQ(loaded.collapsedDirs.size(), static_cast<size_t>(2));

    std::filesystem::remove(tmp, ec);
    removeFile(file);
    return true;
});
