// tests/test_modules/test_md_keyboard.cpp
// 编辑器键盘流（spec §5.2 Phase 2）测试：
//   1. md_keyboard 纯函数单测（listPrefixOf / applyEnterList / applyTab /
//      applyWrapAround / applyLinkSkeleton —— 字节偏移语义、undo 无关）
//   2. 无头接线冒烟：markdown_module 的 input.onKeyEvent passthrough ——
//      Tab/Enter/Ctrl+B 变换写回 InputState + onChange 副作用（状态栏转「未保存」）
// 真实 IME / 焦点行为留给人工点验（见批次报告）
#include "doctest.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "module_markdown/md_keyboard.h"

#include <components/input.h>
#include <eui/dsl.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

extern "C" ::core::Module* create_markdown_module();

namespace fs = std::filesystem;
using module::markdown::EditBuffer;

namespace {

// 便捷构造：默认光标在文本末尾、无选区
EditBuffer buf(std::string text, int cursor = -1, int selStart = -2, int selEnd = -2) {
    if (cursor < 0) cursor = static_cast<int>(text.size());
    if (selStart == -2) selStart = cursor;
    if (selEnd == -2) selEnd = cursor;
    return {std::move(text), cursor, selStart, selEnd};
}

fs::path makeTempNotesDir(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir = fs::temp_directory_path() /
        ("devdesk_mdkb_" + std::string(tag) + "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool writeTextFile(const fs::path& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

struct MdKbEnv {
    fs::path dir;
    std::unique_ptr<core::Database> db;

    static void setEnvVar(const std::string& value) {
#if defined(_WIN32)
        ::_putenv_s("DEVDESK_NOTES_DIR", value.c_str());
#else
        if (value.empty()) {
            ::unsetenv("DEVDESK_NOTES_DIR");
        } else {
            ::setenv("DEVDESK_NOTES_DIR", value.c_str(), 1);
        }
#endif
    }

    explicit MdKbEnv(const char* tag) {
        dir = makeTempNotesDir(tag);
        auto r = core::Database::open(dir / "kbd_test.db");
        if (r.isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        if (db->migrate().isFail()) {
            db.reset();
            return;
        }
        core::registerAppDatabase(&db->db());
        setEnvVar(dir.string());
    }

    ~MdKbEnv() {
        setEnvVar("");
        core::registerAppDatabase(nullptr);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

// ---- 纯函数：listPrefixOf / nextListPrefix ----

REGISTER_TEST(md_kbd_list_prefix, "md_keyboard: 列表前缀识别", {
    using module::markdown::listPrefixOf;
    DOCTEST_CHECK_EQ(listPrefixOf("- a"), std::string("- "));
    DOCTEST_CHECK_EQ(listPrefixOf("* b"), std::string("* "));
    DOCTEST_CHECK_EQ(listPrefixOf("1. a"), std::string("1. "));
    DOCTEST_CHECK_EQ(listPrefixOf("12. a"), std::string("12. "));
    DOCTEST_CHECK_EQ(listPrefixOf("  - 嵌套"), std::string("  - "));
    DOCTEST_CHECK_EQ(listPrefixOf("\t2. tab"), std::string("\t2. "));
    // 非列表：普通行 / 无空格 / 标题 / 空行
    DOCTEST_CHECK_EQ(listPrefixOf("普通行"), std::string(""));
    DOCTEST_CHECK_EQ(listPrefixOf("-a"), std::string(""));
    DOCTEST_CHECK_EQ(listPrefixOf("1.a"), std::string(""));
    DOCTEST_CHECK_EQ(listPrefixOf("# 标题"), std::string(""));
    DOCTEST_CHECK_EQ(listPrefixOf(""), std::string(""));

    using module::markdown::nextListPrefix;
    DOCTEST_CHECK_EQ(nextListPrefix("1. "), std::string("2. "));
    DOCTEST_CHECK_EQ(nextListPrefix("9. "), std::string("10. "));
    DOCTEST_CHECK_EQ(nextListPrefix("  3. "), std::string("  4. "));
    DOCTEST_CHECK_EQ(nextListPrefix("- "), std::string("- "));  // 无序原样
    return true;
})

// ---- 纯函数：Enter 列表续行 ----

REGISTER_TEST(md_kbd_enter_list, "md_keyboard: Enter 列表续行/空项退出/普通行放行", {
    // 无序续行：前缀继承，光标落新行前缀后
    EditBuffer b = buf("- 项目一");
    DOCTEST_CHECK(module::markdown::applyEnterList(b));
    DOCTEST_CHECK_EQ(b.text, std::string("- 项目一\n- "));
    DOCTEST_CHECK_EQ(b.cursor, static_cast<int>(b.text.size()));

    // 有序续行：编号递增
    EditBuffer o = buf("1. 第一");
    DOCTEST_CHECK(module::markdown::applyEnterList(o));
    DOCTEST_CHECK_EQ(o.text, std::string("1. 第一\n2. "));

    // 空列表项：取消前缀退出，不换行
    EditBuffer e = buf("- ");
    DOCTEST_CHECK(module::markdown::applyEnterList(e));
    DOCTEST_CHECK_EQ(e.text, std::string(""));
    DOCTEST_CHECK_EQ(e.cursor, 0);

    // 行中回车（光标在项中间，UTF-8 边界）：换行 + 继承前缀
    EditBuffer m = buf("- 项目");
    m.cursor = m.selStart = m.selEnd = 5;  // 「项」后（2 + 3 字节）
    DOCTEST_CHECK(module::markdown::applyEnterList(m));
    DOCTEST_CHECK_EQ(m.text, std::string("- 项\n- 目"));

    // 普通行 / 有选区：返回 false 交回内置
    EditBuffer p = buf("普通文本");
    DOCTEST_CHECK(!module::markdown::applyEnterList(p));
    EditBuffer s = buf("- a");
    s.selStart = 0;
    s.selEnd = 2;
    DOCTEST_CHECK(!module::markdown::applyEnterList(s));
    return true;
})

// ---- 纯函数：Tab 缩进 ----

REGISTER_TEST(md_kbd_tab_indent, "md_keyboard: Tab 两空格缩进/反缩进", {
    // 无选区 Tab = 插入两空格
    EditBuffer a = buf("ab");
    a.cursor = a.selStart = a.selEnd = 1;
    module::markdown::applyTab(a, false);
    DOCTEST_CHECK_EQ(a.text, std::string("a  b"));
    DOCTEST_CHECK_EQ(a.cursor, 3);

    // 多行选区 Tab：整块行首 +2，选区覆盖原内容
    EditBuffer m = buf("aa\nbb\ncc");
    m.selStart = 0;
    m.selEnd = 6;  // 选中 aa\nbb\n（cc 行不选——选区到行首为止不含该行）
    module::markdown::applyTab(m, false);
    DOCTEST_CHECK_EQ(m.text, std::string("  aa\n  bb\ncc"));
    DOCTEST_CHECK_EQ(m.selStart, 2);
    DOCTEST_CHECK_EQ(m.selEnd, 10);

    // Shift+Tab 反缩进：去掉行首两空格
    EditBuffer r = buf("  aa\n  bb\ncc");
    r.selStart = 0;
    r.selEnd = 10;
    module::markdown::applyTab(r, true);
    DOCTEST_CHECK_EQ(r.text, std::string("aa\nbb\ncc"));
    DOCTEST_CHECK_EQ(r.selStart, 0);
    DOCTEST_CHECK_EQ(r.selEnd, 6);

    // 无前导空格的 Shift+Tab：无变化（接线层据此不动状态）
    EditBuffer n = buf("aa\nbb");
    n.selStart = 0;
    n.selEnd = 5;
    module::markdown::applyTab(n, true);
    DOCTEST_CHECK_EQ(n.text, std::string("aa\nbb"));
    DOCTEST_CHECK_EQ(n.selStart, 0);
    DOCTEST_CHECK_EQ(n.selEnd, 5);

    // 无选区 Shift+Tab：反缩进当前行；光标落在被删区间内也钳回行首
    EditBuffer c = buf("x\n  yy");
    c.cursor = c.selStart = c.selEnd = 3;  // 第二行行首 +1（被删区间内）
    module::markdown::applyTab(c, true);
    DOCTEST_CHECK_EQ(c.text, std::string("x\nyy"));
    DOCTEST_CHECK_EQ(c.cursor, 2);
    return true;
})

// ---- 纯函数：Ctrl+B/I 包裹 + Ctrl+K 链接骨架 ----

REGISTER_TEST(md_kbd_wrap_and_link, "md_keyboard: 选区包裹 / 成对标记 / 链接骨架", {
    // 无选区 Ctrl+I：成对 * 光标放中间
    EditBuffer a = buf("ab");
    a.cursor = a.selStart = a.selEnd = 1;
    module::markdown::applyWrapAround(a, "*");
    DOCTEST_CHECK_EQ(a.text, std::string("a**b"));
    DOCTEST_CHECK_EQ(a.cursor, 2);

    // 选区 Ctrl+B：**bold**，选区仍选中原文
    EditBuffer b = buf("abcdef");
    b.selStart = 2;
    b.selEnd = 5;
    b.cursor = 5;
    module::markdown::applyWrapAround(b, "**");
    DOCTEST_CHECK_EQ(b.text, std::string("ab**cde**f"));
    DOCTEST_CHECK_EQ(b.selStart, 4);
    DOCTEST_CHECK_EQ(b.selEnd, 7);
    DOCTEST_CHECK_EQ(b.cursor, 7);

    // 选区 Ctrl+K：[sel](链接)，选「链接」占位（占位词 2 个 CJK 字 = 6 字节）
    EditBuffer k = buf("abcdef");
    k.selStart = 0;
    k.selEnd = 2;  // ab
    module::markdown::applyLinkSkeleton(k);
    DOCTEST_CHECK_EQ(k.text, std::string("[ab](链接)cdef"));
    DOCTEST_CHECK_EQ(k.selStart, 5);
    DOCTEST_CHECK_EQ(k.selEnd, 11);

    // 无选区 Ctrl+K：[文本](链接)，选「文本」占位
    EditBuffer k2 = buf("xy");
    k2.cursor = k2.selStart = k2.selEnd = 1;
    module::markdown::applyLinkSkeleton(k2);
    DOCTEST_CHECK_EQ(k2.text, std::string("x[文本](链接)y"));
    DOCTEST_CHECK_EQ(k2.selStart, 2);
    DOCTEST_CHECK_EQ(k2.selEnd, 8);
    return true;
})

// ---- 接线：markdown_module onKeyEvent passthrough（headless）----

REGISTER_TEST(md_kbd_wiring_input_passthrough,
              "md_keyboard: 编辑器 onKeyEvent 接线（Tab/Enter/Ctrl+B 写回 InputState）", {
    MdKbEnv env("wiring");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "- 列表项\n正文\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&]() {
        ui.begin("kbd");
        mod->compose(ui, screen);
        ui.end();
    };

    // 帧1：打开笔记（注意：ui.begin() 每帧清空元素树，hit 指针跨帧失效，
    // 每次 frame() 后必须重新 find——W 踩坑同 test_todo_ui 的 composeLayout）
    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    fileRow->onClick();
    frame();
    using InputState = components::input_detail::InputModel::InputState;

    auto press = [](eui::InputKey key, bool ctrl = false, bool shift = false) {
        eui::KeyEvent ev;
        ev.key = key;
        ev.action = eui::KeyAction::Press;
        ev.modifiers.control = ctrl;
        ev.modifiers.shift = shift;
        return ev;
    };
    // 当前帧的编辑器命中元素（带 onKeyEvent 回调）；CHECK 宏带 return false，
    // 不能进 lambda —— 断言留在用例体内
    auto editorHit = [&]() { return ui.find("md.editor.input.hit"); };
    auto pressKey = [&](eui::InputKey key, bool ctrl = false, bool shift = false) -> bool {
        const auto* h = editorHit();
        DOCTEST_REQUIRE(h != nullptr);
        DOCTEST_REQUIRE(h->onKeyEvent != nullptr);
        return h->onKeyEvent(press(key, ctrl, shift));
    };

    // 光标挪到第一行末（tree 行点击后光标默认文末）
    {
        auto& st = ui.state<InputState>("md.editor.input");
        st.cursor = st.selectionStart = st.selectionEnd = 11;  // "- 列表项"行末（2+9 字节）
    }

    // Enter：列表续行（拦截 + 写回）
    DOCTEST_CHECK(pressKey(eui::InputKey::Enter));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n- \n正文\n"));
    }
    // 副作用：onChange 链路跑了（状态栏转「未保存」）
    const auto* save = ui.find("md.status.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_CHECK(save->text.find("未保存") != std::string::npos);

    // Tab：无选区插入两空格
    DOCTEST_CHECK(pressKey(eui::InputKey::Tab));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-   \n正文\n"));
    }

    // Ctrl+B：无选区插入成对 **（光标放中间）
    DOCTEST_CHECK(pressKey(eui::InputKey::B, true));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-   ****\n正文\n"));
        DOCTEST_CHECK_EQ(st.cursor, st.selectionStart);
    }

    // Shift+Tab（无前导空格的行）：消费但无变化（不制造脏标记）
    {
        auto& st = ui.state<InputState>("md.editor.input");
        st.cursor = st.selectionStart = st.selectionEnd = 2;  // 第一行「列」前
    }
    DOCTEST_CHECK(pressKey(eui::InputKey::Tab, false, true));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-   ****\n正文\n"));
    }

    // 普通按键（A）不拦截：内置继续处理，冒泡不受影响
    DOCTEST_CHECK(!pressKey(eui::InputKey::A));

    mod->onStop();
    return true;
})
