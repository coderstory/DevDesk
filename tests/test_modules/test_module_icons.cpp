// tests/test_modules/test_module_icons.cpp
// 侧边栏图标（FontAwesome 矢量字）回归测试：
//   1. 五个模块的 iconGlyph() 返回约定的 FA7 Free-Solid 码点（UTF-8 编码），
//      不再是 emoji——emoji 在 Windows 上由系统彩色 emoji 字体渲染，风格不统一
//   2. compose 层验证：TextBuilder::icon(glyph) 后元素 fontFamily == "Icon"
//      （EUI-NEO 按此路由到内置 Font Awesome 7 Free-Solid-900.otf，
//      见 ../EUI-NEO/core/render/text.cpp kDefaultIconFontFile）
// 码点是否真在字体文件里已用离线 cmap 解析核对（3236 字形全含），
// 字形观感留给人工点验
#include "doctest.h"

#include "core/module_interface.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <memory>
#include <string>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();

namespace {

struct Entry {
    ::core::Module* (*factory)();
    unsigned int codepoint;  // FA7 Free-Solid
    const char* name;
};

// 实际断言体（REGISTER_TEST 宏体内不能出现裸逗号，逻辑放函数里）
bool checkOneIcon(::core::Module* (*factory)(), unsigned int codepoint) {
    std::unique_ptr<::core::Module> mod(factory());
    DOCTEST_REQUIRE(mod != nullptr);
    const std::string glyph = mod->iconGlyph();

    // 1. 码点约定（eui::utf8(0xF...) 的 UTF-8 编码，非 emoji）
    DOCTEST_CHECK_EQ(glyph, eui::utf8(codepoint));

    // 2. compose 层：模拟 main.cpp 侧边栏的 .icon(glyph) 用法，
    //    断言元素挂到 Icon 字体（FontAwesome）而非正文字体
    eui::Ui ui;
    ui.begin("icon_frame");
    ui.text("probe.icon").icon(glyph).size(28.0f, 40.0f).build();
    ui.end();
    const core::dsl::Element* el = ui.find("probe.icon");
    DOCTEST_REQUIRE(el != nullptr);
    DOCTEST_CHECK_EQ(el->fontFamily, std::string("Icon"));
    DOCTEST_CHECK_EQ(el->text, glyph);
    return true;
}

} // namespace

REGISTER_TEST(module_icons_fa_codepoints,
              "module_icons: iconGlyph 返回 FA 码点且 icon() 路由 Icon 字体", {
    // Home = fa-house / Markdown = fa-pen-to-square / Todo = fa-circle-check
    // Claude = fa-arrows-rotate / Settings = fa-gear
    if (!checkOneIcon(create_home_module, 0xF015)) return false;
    if (!checkOneIcon(create_markdown_module, 0xF044)) return false;
    if (!checkOneIcon(create_todo_module, 0xF058)) return false;
    if (!checkOneIcon(create_claude_module, 0xF021)) return false;
    if (!checkOneIcon(create_settings_module, 0xF013)) return false;
    return true;
})
