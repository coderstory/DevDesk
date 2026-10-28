// tests/test_modules/test_module_icons.cpp
// 侧边栏与页内图标：iconGlyph() 返回约定的 FA7 Free-Solid 码点，icon() 把元素挂到 Icon 字体。
// 跨文件前提：
//   - glyph 是 FA 码点的 UTF-8 编码（eui::utf8(0xF...)），不是 emoji——emoji 在 Windows 上
//     由系统彩色 emoji 字体渲染，风格不统一。
//   - fontFamily=="Icon"（EUI-NEO 也收 "Font Awesome 7 Free"）路由到内置
//     Font Awesome 7 Free-Solid-900.otf（../EUI-NEO/core/render/text.cpp
//     kDefaultIconFontFile，改那个匹配前先确认 EUI-NEO 侧仍收 "Icon"）。
//   - 字体里码点是否齐全、字形观感均不作断言（属人工点验），本文件只钉码点与字体路由两件事。
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
extern "C" ::core::Module* create_about_module();
extern "C" ::core::Module* create_agent_module();
extern "C" ::core::Module* create_memory_module();
extern "C" void claude_test_force_show_guide(::core::Module*);

namespace {

struct Entry {
    ::core::Module* (*factory)();
    unsigned int codepoint;  // FA7 Free-Solid
    const char* name;
};

// 断言体：码点约定 + icon() 路由到 Icon 字体
bool checkOneIcon(::core::Module* (*factory)(), unsigned int codepoint) {
    std::unique_ptr<::core::Module> mod(factory());
    DOCTEST_REQUIRE(mod != nullptr);
    const std::string glyph = mod->iconGlyph();

    DOCTEST_CHECK_EQ(glyph, eui::utf8(codepoint));

    // 复刻 app_sidebar.cpp 两处 .icon(m->iconGlyph()) 的用法（展开态 / 折叠态）
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
    if (!checkOneIcon(create_home_module, 0xF015)) return false;
    if (!checkOneIcon(create_markdown_module, 0xF044)) return false;
    if (!checkOneIcon(create_todo_module, 0xF058)) return false;
    if (!checkOneIcon(create_claude_module, 0xF021)) return false;
    if (!checkOneIcon(create_settings_module, 0xF013)) return false;
    if (!checkOneIcon(create_about_module, 0xF05A)) return false;
    if (!checkOneIcon(create_agent_module, 0xF544)) return false;
    if (!checkOneIcon(create_memory_module, 0xF5DC)) return false;
    return true;
})

namespace {

struct PageIconProbe {
    ::core::Module* (*factory)();
    const char* elId;
    unsigned int codepoint;  // FA7 Free-Solid
};

bool checkPageIcon(::core::Module* (*factory)(), const char* elId,
                   unsigned int codepoint) {
    std::unique_ptr<::core::Module> mod(factory());
    DOCTEST_REQUIRE(mod != nullptr);
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("probe_page");
    mod->compose(ui, screen);
    ui.end();
    const core::dsl::Element* el = ui.find(elId);
    DOCTEST_REQUIRE(el != nullptr);
    DOCTEST_CHECK_EQ(el->fontFamily, std::string("Icon"));
    DOCTEST_CHECK_EQ(el->text, eui::utf8(codepoint));
    return true;
}

} // namespace

REGISTER_TEST(module_icons_page_emoji_replaced,
              "module_icons: 三模块页内 emoji 已换 FA 图标元素", {
    if (!checkPageIcon(create_home_module, "home.btn.refresh.icon", 0xF021)) return false;
    // W9: 右上「设置」按钮已移除（与侧边栏入口重复），反证元素不存在
    {
        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        eui::Ui ui;
        ui.begin("probe_home_no_settings");
        mod->compose(ui, screen);
        ui.end();
        if (ui.find("home.btn.settings") != nullptr) return false;
        if (ui.find("home.btn.settings.icon") != nullptr) return false;
    }
    if (!checkPageIcon(create_home_module, "home.act.claude.icon", 0xF46D)) return false;
    if (!checkPageIcon(create_claude_module, "claude.btn.proxy.icon", 0xF04B)) return false;
    // H14: guide 图标门控确定性——test 进程直接 compose 时 proxyEverSeen_/guideDismissed_
    // 默认值与真实进程（经 onStart 演化）状态不同，门控不可靠。显式重置后保锚点命中。
    {
        std::unique_ptr<::core::Module> mod(create_claude_module());
        claude_test_force_show_guide(mod.get());
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        eui::Ui ui;
        ui.begin("probe_guide_icon");
        mod->compose(ui, screen);
        ui.end();
        const core::dsl::Element* el = ui.find("claude.guide.icon");
        DOCTEST_REQUIRE(el != nullptr);
        DOCTEST_CHECK_EQ(el->fontFamily, std::string("Icon"));
        DOCTEST_CHECK_EQ(el->text, eui::utf8(0xF05A));
    }
    // 设置页无 FA 元素：主题模式下拉无图标通道，「重新生成 Token」按钮已随代理鉴权移除。
    // 别把它当成漏测的用例补断言——先确认这两处是否还在。
    // ficon/fa-folder 树在 test_markdown.cpp 的空目录用例里断言：icons 探针无服务无数据
    if (!checkPageIcon(create_markdown_module, "md.search.icon", 0xF002)) return false;
    // S3.3#5 逾期入口已改为 chip 小标签（纯文本「逾期 N」，无图标元素）——旧 FA 图标锚点退役
    // detail dialog 常驻 compose，无数据也渲染，故截止日期按钮的元素存在性无需造数据
    if (!checkPageIcon(create_todo_module, "todo.search.icon", 0xF002)) return false;
    if (!checkPageIcon(create_todo_module,
                       "todo.dialog.detail.due.pick.icon", 0xF133)) return false;
    return true;
})
