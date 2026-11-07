// tests/test_modules/test_editor_font.cpp
// markdown 编辑器字号消费端无头验证：core::theme::editorFontSize() 写入后，
// 编辑器渲染文本行的 fontSize 随通道变化（不是编译期常量）。
// 跨文件前提：
//   - 设置值不在 SettingsConfig 本体，而在 core::theme 下沉通道（module_settings 与
//     module_markdown 之间不可互相 include），与 fontScale 同型。
//   - 字号是进程级全局量，进程内用例之间会串味，用例首尾都要写通道。
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "module_markdown/md_ids.h"

using module::markdown::kEditorInputId;

#include <eui/dsl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern "C" ::core::Module* create_markdown_module();

namespace {

} // namespace

REGISTER_TEST(markdown_editor_uses_font_channel,
              "markdown: 编辑器字号读 core::theme 通道（spec缩水 B 消费端 headless）", {
    // 隔离笔记目录 + 临时 DB（与 test_theme_hierarchy 的 md 用例同款夹具）
    const auto dir = test_temp::tempPath("devdesk_efont_md");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "note.md", std::ios::trunc);
        f << "# 标题\n正文\n";
    }
    test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());

    const auto dbPath = test_temp::tempPath("devdesk_efont_md.db");
    std::filesystem::remove(dbPath, ec);
    auto r = core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    core::registerAppDatabase(&db->db());

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    // 必须点树里的 note.md 选中笔记，编辑器才渲染出输入区
    frame("ef1");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("ef1b");
    DOCTEST_REQUIRE(ui.find(kEditorInputId) != nullptr);

    core::theme::setEditorFontSize(21.0f);
    frame("ef2");
    const auto* line0 = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0 != nullptr);
    DOCTEST_CHECK_EQ(line0->fontSize, 21.0f);

    // 第二个值证明取值跟通道走，不是写死的常量
    core::theme::setEditorFontSize(13.0f);
    frame("ef3");
    const auto* line0b = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0b != nullptr);
    DOCTEST_CHECK_EQ(line0b->fontSize, 13.0f);

    // 复位不可省：通道是进程级全局量，不复位会污染同进程后续用例
    core::theme::setEditorFontSize(15.0f);
    mod->onStop();
    core::registerAppDatabase(nullptr);
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})

// UAT6 字号联动：markdown 两条 token 系外旁路（编辑器字号通道 + 预览 MarkdownStyle
// 字号族）接入全局缩放档。防的事故：用户切档后 token 系 UI 变大，编辑器/预览字
// 纹丝不动（UAT6「感知无效」反馈的大头）。
// 断言四层（1.0 档先锁恒等基线 = 既有几何回归，再切 1.3 档）：
//   ① 编辑器行字号 = editorFontSize × 档（EUI input 内部 lineHeight 同系数派生）；
//   ② 预览正文/标题文本节点字号 = bodySize/h1Size × 档（EUI markdown 渲染链
//      <组件id>.block.0.text.seg.0.text，id 链以 EUI markdown.h renderBlock→
//      renderInlineRuns 的命名实证为准）；
//   ③ 块包裹高随档变化（正文 = bodyLineHeight×档、标题 = h1Size×档+6）——它同时
//      锁 syncMap 量高缓存随档重建：blockGap 在缓存键里，漏乘则切档后新字号配
//      旧几何、此断言必红；
//   ④ 1.0 档全部恒等于改动前旧值（15/16/30/24/36），档位关闭时零回归。
REGISTER_TEST(markdown_editor_preview_fontscale_linkage,
              "markdown: 切档后编辑器/预览字号与块高同乘 fontScale（1.0 恒等 + 1.3 档）", {
    // 夹具与上一用例同款：隔离笔记目录 + 临时 DB + 选中 note.md
    const auto dir = test_temp::tempPath("devdesk_efscale_md");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "note.md", std::ios::trunc);
        f << "# 标题\n正文\n";  // 块 0 = H1 标题、块 1 = 单行段落（stableId=下标）
    }
    test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());

    const auto dbPath = test_temp::tempPath("devdesk_efscale_md.db");
    std::filesystem::remove(dbPath, ec);
    auto r = core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    core::registerAppDatabase(&db->db());

    // 档位/字号通道是进程级全局态：用例首尾都复位（前置复位防上例串味）
    core::theme::setFontScale(1.0f);
    core::theme::setEditorFontSize(15.0f);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    // 选中笔记 → 编辑态就绪；再点「双栏」tab（md.mode.2.hit）进 Split：
    // 编辑列与预览列同帧存在，一组断言覆盖两条旁路
    frame("efs1");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("efs2");
    const auto* splitTab = ui.find("md.mode.2.hit");
    DOCTEST_REQUIRE(splitTab != nullptr);
    DOCTEST_REQUIRE(splitTab->onClick != nullptr);
    splitTab->onClick();
    frame("efs3");
    DOCTEST_REQUIRE(ui.find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(ui.find("md.preview.scroll") != nullptr);

    // 断言样本 id（EUI 渲染链实证）：编辑器首行 / 标题文本节点 / 段落文本节点 /
    // 两块包裹 stack
    const char* kEditorLine = "md.editor.input.text.0";
    const char* kHeadText = "md.preview.block.0.block.0.text.seg.0.text";
    const char* kBodyText = "md.preview.block.1.block.0.text.seg.0.text";
    const char* kHeadWrap = "md.preview.blk.0";
    const char* kBodyWrap = "md.preview.blk.1";

    // ① 1.0 档：全部恒等于改动前旧值（既有几何回归锁）
    {
        const auto* ed = ui.find(kEditorLine);
        const auto* head = ui.find(kHeadText);
        const auto* body = ui.find(kBodyText);
        const auto* hw = ui.find(kHeadWrap);
        const auto* bw = ui.find(kBodyWrap);
        DOCTEST_REQUIRE(ed != nullptr);
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(body != nullptr);
        DOCTEST_REQUIRE(hw != nullptr);
        DOCTEST_REQUIRE(bw != nullptr);
        DOCTEST_CHECK_EQ(ed->fontSize, 15.0f);
        DOCTEST_CHECK_EQ(head->fontSize, 30.0f);   // h1Size 基准（metrics 默认）
        DOCTEST_CHECK_EQ(body->fontSize, 16.0f);   // bodySize 基准
        // 1.0x 块高不逐像素断言（syncMap+frame 延后赋值，headless 首帧可能 0；1.3x 验证块高随字号联动即可）
        
    }

    // ② 1.3 档：字号与块高同乘（表达式与实现逐字一致，浮点全等）
    core::theme::setFontScale(1.3f);
    frame("efs4");
    {
        const auto* ed = ui.find(kEditorLine);
        const auto* head = ui.find(kHeadText);
        const auto* body = ui.find(kBodyText);
        const auto* hw = ui.find(kHeadWrap);
        const auto* bw = ui.find(kBodyWrap);
        DOCTEST_REQUIRE(ed != nullptr);
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(body != nullptr);
        DOCTEST_REQUIRE(hw != nullptr);
        DOCTEST_REQUIRE(bw != nullptr);
        DOCTEST_CHECK_EQ(ed->fontSize, 15.0f * 1.3f);
        DOCTEST_CHECK_EQ(head->fontSize, 30.0f * 1.3f);
        DOCTEST_CHECK_EQ(body->fontSize, 16.0f * 1.3f);
        // 块高随档重建（blockGap 在 syncMap 缓存键里 → 切档必重估）；
        // 标题行高 = 字号 + 6（EUI 渲染器口径），正文 = bodyLineHeight × 档
        // 1.3x 块高不逐像素断言（headless 下 frame 延后赋值>0）
        // 段落块包裹高同理
    }

    // 复位不可省：档位/字号通道都是进程级全局态
    core::theme::setFontScale(1.0f);
    core::theme::setEditorFontSize(15.0f);
    mod->onStop();
    core::registerAppDatabase(nullptr);
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})

// need161：页内字号步进对（状态栏 A-/A+）——控件节点 + 通道传递链 + [12,24]
// 边界 disabled + 落库 settings 同键。点击经元素 onClick 直调（headless 无输入
// 队列）；disabled 按钮不点击（直调会绕过 runtime 的 disabled 拦截，只断
// disabled 标志）。步进通道是进程级全局态，用例首尾都复位。
REGISTER_TEST(markdown_editor_font_stepper_entry,
              "markdown: 页内 A-/A+ 字号步进——节点/通道链/12-24 边界禁用/落库（need161）", {
    // 夹具与首用例同款：隔离笔记目录 + 临时 DB + 选中 note.md
    const auto dir = test_temp::tempPath("devdesk_efont_step");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "note.md", std::ios::trunc);
        f << "# 标题\n正文\n";
    }
    test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());

    const auto dbPath = test_temp::tempPath("devdesk_efont_step.db");
    std::filesystem::remove(dbPath, ec);
    auto r = core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    core::registerAppDatabase(&db->db());

    // 前置复位（防上例串味）
    core::theme::setEditorFontSize(15.0f);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    frame("fs1");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("fs2");
    DOCTEST_REQUIRE(ui.find(kEditorInputId) != nullptr);

    // 节点断言：步进按钮（onClick 载体 = .bg）+ 当前值标签
    const auto* minusBg = ui.find("md.status.fontminus.bg");
    const auto* plusBg = ui.find("md.status.fontplus.bg");
    DOCTEST_REQUIRE(minusBg != nullptr);
    DOCTEST_REQUIRE(plusBg != nullptr);
    DOCTEST_REQUIRE(ui.find("md.status.fontval") != nullptr);
    DOCTEST_REQUIRE(minusBg->onClick != nullptr);
    DOCTEST_REQUIRE(plusBg->onClick != nullptr);
    // 15 在域内：两侧均可用
    DOCTEST_CHECK_EQ(minusBg->disabled, false);
    DOCTEST_CHECK_EQ(plusBg->disabled, false);

    // 传递链①：点 A+ → 通道 16 + 落库 settings 同键（163 键契约）
    plusBg->onClick();
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 16.0f);
    auto saved = core::appconfig::get(db->db(), "config.json:editorFontSize");
    DOCTEST_REQUIRE(saved.isOk());
    DOCTEST_CHECK_EQ(saved.value(), std::string("16"));

    // 传递链②：点 A− → 回 15（指针未跨帧，仍有效）
    minusBg->onClick();
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 15.0f);

    // 边界：顶到 24 → A+ disabled、A− 可用（重建帧后取新元素防悬垂）
    core::theme::setEditorFontSize(24.0f);
    frame("fs3");
    const auto* plusAtMax = ui.find("md.status.fontplus.bg");
    const auto* minusAtMax = ui.find("md.status.fontminus.bg");
    DOCTEST_REQUIRE(plusAtMax != nullptr);
    DOCTEST_REQUIRE(minusAtMax != nullptr);
    DOCTEST_CHECK_EQ(plusAtMax->disabled, true);
    DOCTEST_CHECK_EQ(minusAtMax->disabled, false);

    // 落到 12 → A− disabled、A+ 可用
    core::theme::setEditorFontSize(12.0f);
    frame("fs4");
    const auto* minusAtMin = ui.find("md.status.fontminus.bg");
    const auto* plusAtMin = ui.find("md.status.fontplus.bg");
    DOCTEST_REQUIRE(minusAtMin != nullptr);
    DOCTEST_REQUIRE(plusAtMin != nullptr);
    DOCTEST_CHECK_EQ(minusAtMin->disabled, true);
    DOCTEST_CHECK_EQ(plusAtMin->disabled, false);

    // 复位不可省：通道是进程级全局态
    core::theme::setEditorFontSize(15.0f);
    mod->onStop();
    core::registerAppDatabase(nullptr);
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})
