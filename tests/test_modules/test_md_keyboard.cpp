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

REGISTER_TEST(md_kbd_tab_indent, "md_keyboard: Tab 4 空格缩进/反缩进（spec §5.2）", {
    // 无选区 Tab = 插入 4 空格
    EditBuffer a = buf("ab");
    a.cursor = a.selStart = a.selEnd = 1;
    module::markdown::applyTab(a, false);
    DOCTEST_CHECK_EQ(a.text, std::string("a    b"));
    DOCTEST_CHECK_EQ(a.cursor, 5);

    // 多行选区 Tab：整块行首 +4，选区覆盖原内容
    EditBuffer m = buf("aa\nbb\ncc");
    m.selStart = 0;
    m.selEnd = 6;  // 选中 aa\nbb\n（cc 行不选——选区到行首为止不含该行）
    module::markdown::applyTab(m, false);
    DOCTEST_CHECK_EQ(m.text, std::string("    aa\n    bb\ncc"));
    DOCTEST_CHECK_EQ(m.selStart, 4);
    DOCTEST_CHECK_EQ(m.selEnd, 14);

    // Shift+Tab 反缩进：每行去掉至多 4 个前导空格（不足 4 有几个去几个）
    EditBuffer r = buf("    aa\n  bb\ncc");
    r.selStart = 0;
    r.selEnd = 12;
    module::markdown::applyTab(r, true);
    DOCTEST_CHECK_EQ(r.text, std::string("aa\nbb\ncc"));
    DOCTEST_CHECK_EQ(r.selStart, 0);
    DOCTEST_CHECK_EQ(r.selEnd, 6);

    // Tab 字符行 Shift+Tab：去 1 个 Tab
    EditBuffer t = buf("\taa");
    t.selStart = 0;
    t.selEnd = 3;
    module::markdown::applyTab(t, true);
    DOCTEST_CHECK_EQ(t.text, std::string("aa"));

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

    // 再按 = 去包裹（unwrap toggle，spec §5.2）：选区两端恰为 ** → 移除
    EditBuffer u = buf("ab**cde**f");
    u.selStart = 4;
    u.selEnd = 7;
    u.cursor = 7;
    module::markdown::applyWrapAround(u, "**");
    DOCTEST_CHECK_EQ(u.text, std::string("abcdef"));
    DOCTEST_CHECK_EQ(u.selStart, 2);
    DOCTEST_CHECK_EQ(u.selEnd, 5);
    DOCTEST_CHECK_EQ(u.cursor, 5);

    // 两端不恰为 marker（只有一端）→ 正常包裹，不误删
    EditBuffer h = buf("ab*cde*f");
    h.selStart = 0;
    h.selEnd = 3;  // 选 "ab*"，前端无 *
    module::markdown::applyWrapAround(h, "*");
    DOCTEST_CHECK_EQ(h.text, std::string("*ab**cde*f"));

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

// ---- 纯函数：表格模板生成 / 光标处插入（审计 [12]）----

REGISTER_TEST(md_kbd_table_build_insert, "md_keyboard: 表格模板生成 / 光标处插入（审计 [12]）", {
    using module::markdown::buildMarkdownTable;
    using module::markdown::applyInsertTable;

    // rows 含表头：3 行 × 2 列 = 表头 + 分隔行 + 2 数据行
    DOCTEST_CHECK_EQ(buildMarkdownTable(3, 2),
                     std::string("|   |   |\n|---|---|\n|   |   |\n|   |   |"));
    // 1 行 = 表头 + 分隔行（GFM 合法）；<1 钳到 1
    DOCTEST_CHECK_EQ(buildMarkdownTable(1, 1), std::string("|   |\n|---|"));
    DOCTEST_CHECK_EQ(buildMarkdownTable(0, -3), std::string("|   |\n|---|"));

    // 空文档插入：整表落位，光标进表头首格（"| " 后）
    EditBuffer e = buf("");
    applyInsertTable(e, 2, 2);
    DOCTEST_CHECK_EQ(e.text, std::string("|   |   |\n|---|---|\n|   |   |"));
    DOCTEST_CHECK_EQ(e.cursor, 2);

    // 光标行非空：行尾插入 + 前置空行与段落隔开
    EditBuffer p = buf("para");
    applyInsertTable(p, 1, 1);
    DOCTEST_CHECK_EQ(p.text, std::string("para\n\n|   |\n|---|"));
    DOCTEST_CHECK_EQ(p.cursor, 8);  // "para\n\n"(6) + "| "(2)

    // 空白行（纯空格）：整行替换，不残留缩进
    EditBuffer b = buf("para\n  \nnext");
    b.cursor = b.selStart = b.selEnd = 7;  // 空格行内
    applyInsertTable(b, 1, 1);
    DOCTEST_CHECK_EQ(b.text, std::string("para\n|   |\n|---|\nnext"));
    return true;
})

// ---- 纯函数：Tab/Shift+Tab 表格跳格（审计 [12]）----

REGISTER_TEST(md_kbd_table_tab_cells, "md_keyboard: Tab/Shift+Tab 表格跳格（审计 [12]）", {
    using module::markdown::applyTabTable;
    const std::string t = "| a | b |\n|---|---|\n| c | d |";
    // 偏移：行0 [0,9) 竖线{0,4,8}；行1 [10,19) 分隔行；行2 [20,29) 竖线{20,24,28}
    // 格内容选区：a=2..3  b=6..7  c=22..23  d=26..27

    // a → b（同行下一格，内容成选区）
    EditBuffer b = buf(t, 2);
    DOCTEST_CHECK(applyTabTable(b, false));
    DOCTEST_CHECK_EQ(b.selStart, 6);
    DOCTEST_CHECK_EQ(b.selEnd, 7);
    DOCTEST_CHECK_EQ(b.cursor, 7);

    // 行尾 → 跳过表头分隔行进下一数据行首格
    DOCTEST_CHECK(applyTabTable(b, false));
    DOCTEST_CHECK_EQ(b.selStart, 22);
    DOCTEST_CHECK_EQ(b.selEnd, 23);

    // c → d → 块尾回绕块首 a
    DOCTEST_CHECK(applyTabTable(b, false));
    DOCTEST_CHECK_EQ(b.selStart, 26);
    DOCTEST_CHECK(applyTabTable(b, false));
    DOCTEST_CHECK_EQ(b.selStart, 2);
    DOCTEST_CHECK_EQ(b.selEnd, 3);

    // Shift+Tab 镜像：格内容右端也识别为本格（否则回退卡死）→ a 反跳回 d
    DOCTEST_CHECK(applyTabTable(b, true));
    DOCTEST_CHECK_EQ(b.selStart, 26);
    DOCTEST_CHECK_EQ(b.selEnd, 27);
    // d → c
    DOCTEST_CHECK(applyTabTable(b, true));
    DOCTEST_CHECK_EQ(b.selStart, 22);

    // 光标在分隔行：Tab 进下一非分隔行首格；Shift+Tab 回上一非分隔行末格
    EditBuffer s = buf(t, 15);
    DOCTEST_CHECK(applyTabTable(s, false));
    DOCTEST_CHECK_EQ(s.selStart, 22);
    EditBuffer s2 = buf(t, 15);
    DOCTEST_CHECK(applyTabTable(s2, true));
    DOCTEST_CHECK_EQ(s2.selStart, 6);
    DOCTEST_CHECK_EQ(s2.selEnd, 7);

    // 非表格行 → 放行（false，交回缩进语义）
    EditBuffer n = buf("plain\n| a | b |");
    n.cursor = n.selStart = n.selEnd = 2;
    DOCTEST_CHECK(!applyTabTable(n, false));

    // 行尾无竖线（首尾不成对）不接管
    EditBuffer l = buf("| a | b");
    l.cursor = l.selStart = l.selEnd = 2;
    DOCTEST_CHECK(!applyTabTable(l, false));

    // 缩进表格行（GFM ≤3 空格缩进合法）仍识别
    EditBuffer i = buf("  | a | b |", 4);  // 竖线{2,6,10}，a 内容 4..5
    DOCTEST_CHECK(applyTabTable(i, false));
    DOCTEST_CHECK_EQ(i.selStart, 8);  // b 内容
    DOCTEST_CHECK_EQ(i.selEnd, 9);
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

    // Tab：无选区插入 4 空格
    DOCTEST_CHECK(pressKey(eui::InputKey::Tab));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-     \n正文\n"));
    }

    // Ctrl+B：无选区插入成对 **（光标放中间）
    DOCTEST_CHECK(pressKey(eui::InputKey::B, true));
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-     ****\n正文\n"));
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
        DOCTEST_CHECK_EQ(st.text, std::string("- 列表项\n-     ****\n正文\n"));
    }

    // 普通按键（A）不拦截：内置继续处理，冒泡不受影响
    DOCTEST_CHECK(!pressKey(eui::InputKey::A));

    mod->onStop();
    return true;
})

// ---- 审计 [12] 接线：Tab 表格跳格 + 右键「插入表格」级联（headless）----

REGISTER_TEST(md_kbd_wiring_table_tab, "md_keyboard: 编辑器 Tab 表格跳格接线（headless）", {
    MdKbEnv env("tabjump");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "tb.md", "| a | b |\n|---|---|\n| c | d |\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&]() {
        ui.begin("tt");
        mod->compose(ui, screen);
        ui.end();
    };
    using InputState = components::input_detail::InputModel::InputState;

    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    fileRow->onClick();
    frame();

    auto press = [](eui::InputKey key, bool shift = false) {
        eui::KeyEvent ev;
        ev.key = key;
        ev.action = eui::KeyAction::Press;
        ev.modifiers.shift = shift;
        return ev;
    };

    // 光标进 'a' 格 → Tab 跳 'b'：只动光标，文本不变、不转「未保存」
    {
        auto& st = ui.state<InputState>("md.editor.input");
        st.cursor = st.selectionStart = st.selectionEnd = 2;
    }
    {
        const auto* hit = ui.find("md.editor.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        DOCTEST_CHECK(hit->onKeyEvent(press(eui::InputKey::Tab)));
    }
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("| a | b |\n|---|---|\n| c | d |\n"));
        DOCTEST_CHECK_EQ(st.selectionStart, 6);
        DOCTEST_CHECK_EQ(st.selectionEnd, 7);
        const auto* save = ui.find("md.status.save");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_CHECK(save->text.find("未保存") == std::string::npos);  // 纯跳格不打脏
    }

    // 表外（文末空行，光标越过末行 '\n'）Tab 仍是 4 空格缩进（[12] 不回归缩进语义）
    {
        auto& st = ui.state<InputState>("md.editor.input");
        st.cursor = st.selectionStart = st.selectionEnd = 30;  // 末空行（EOF）
    }
    {
        const auto* hit = ui.find("md.editor.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_CHECK(hit->onKeyEvent(press(eui::InputKey::Tab)));
    }
    frame();
    {
        const auto& st = ui.state<InputState>("md.editor.input");
        DOCTEST_CHECK_EQ(st.text, std::string("| a | b |\n|---|---|\n| c | d |\n    "));
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(md_kbd_wiring_table_insert_menu,
              "md_keyboard: 编辑器右键插入表格级联（headless，审计 [12]）", {
    MdKbEnv env("tabmenu");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "menu.md", "# t\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&]() {
        ui.begin("tm");
        mod->compose(ui, screen);
        ui.end();
    };
    using InputState = components::input_detail::InputModel::InputState;

    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    fileRow->onClick();
    frame();

    // 编辑器右键 → 挂到 .hit 的 onContextMenu 置菜单打开态（审计 [12]）
    const auto* hit = ui.find("md.editor.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onContextMenu != nullptr);
    eui::PointerEvent pe;
    pe.x = 300.0;
    pe.y = 300.0;
    pe.button = eui::PointerButton::Right;
    pe.action = eui::PointerAction::Release;
    hit->onContextMenu(pe, eui::Rect{});
    frame();

    // 级联三级：插入表格 → 3 行 → 5 列（子级菜单点击后下一帧才渲染）
    auto* root = ui.find("md.editor.ctx.level.0.item.0");
    DOCTEST_REQUIRE(root != nullptr);
    root->onClick();
    frame();
    auto* rowItem = ui.find("md.editor.ctx.level.1.item.2");
    DOCTEST_REQUIRE(rowItem != nullptr);
    rowItem->onClick();
    frame();
    auto* colItem = ui.find("md.editor.ctx.level.2.item.4");
    DOCTEST_REQUIRE(colItem != nullptr);
    colItem->onClick();  // 叶子 → onSelectPath([0,2,4]) → 插 3 行 × 5 列
    frame();

    {
        const auto& st = ui.state<InputState>("md.editor.input");
        // 光标在文末空行 → 表格整块落在那里（行首含 5 列分隔行）
        DOCTEST_CHECK_EQ(st.text,
                         std::string("# t\n"
                                     "|   |   |   |   |   |\n"
                                     "|---|---|---|---|---|\n"
                                     "|   |   |   |   |   |\n"
                                     "|   |   |   |   |   |"));
        DOCTEST_CHECK_EQ(st.cursor, 6);  // 表头首格内（"| " 后）
        const auto* save = ui.find("md.status.save");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_CHECK(save->text.find("未保存") != std::string::npos);  // 插入算编辑
    }
    // 选完即收起（onOpenChange(false) → 下一帧不构建）
    DOCTEST_CHECK(ui.find("md.editor.ctx.level.0.item.0") == nullptr);

    mod->onStop();
    return true;
})
