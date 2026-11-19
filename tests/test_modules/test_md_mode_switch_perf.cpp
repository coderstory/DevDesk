// tests/test_modules/test_md_mode_switch_perf.cpp
// 池369（P1）：md 模式 tab 切换冻结量化探针（纯码模式波末验证批的测量仪器）。
// 修复（视口裁剪 + contentHeightProvider 供高）前后各跑一次，对比逐帧耗时与
// 每帧文本元素数（修复前基线：60KB 文档切换帧 3.1-11.3s / 稳态帧 0.9-13.7s /
// 25.5K 文本元素每帧；render 实例重建 2.4s——2026-11-19 修复前实测，Debug）。
// 输出走 stderr（doctest 不吞）；断言只做存在性守卫，不做耗时阈值（阈值判定
// 由波末验证批按数字人工裁决，避免机器抖动假红）。
#include "doctest.h"
#include "test_support.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <core/render/text.h>
#include <core/dsl_runtime.h>
#include <eui/dsl.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

extern "C" ::core::Module* create_markdown_module();

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

void collectTexts(const core::dsl::Element* el,
                  std::vector<const core::dsl::Element*>* out) {
    if (el == nullptr) return;
    if (el->kind == core::dsl::ElementKind::Text && !el->text.empty()) {
        out->push_back(el);
    }
    for (const core::dsl::Element* child : el->orderedChildren) {
        collectTexts(child, out);
    }
}

size_t countTextElements(const eui::Ui& ui) {
    std::vector<const core::dsl::Element*> texts;
    for (const core::dsl::Element* root : ui.orderedRoots()) {
        collectTexts(root, &texts);
    }
    return texts.size();
}

// 模拟 runtime_render.h prepareText：新 TextInstance 从零建（被剪枝后的重建）
double rebuildTextInstances(const eui::Ui& ui, size_t* count, size_t* glyphBytes) {
    std::vector<const core::dsl::Element*> texts;
    for (const core::dsl::Element* root : ui.orderedRoots()) {
        collectTexts(root, &texts);
    }
    *count = texts.size();
    *glyphBytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const core::dsl::Element* el : texts) {
        core::TextPrimitive prim;
        prim.initialize();
        prim.setText(el->text);
        prim.setFontFamily(el->fontFamily);
        prim.setFontSize(el->fontSize);
        prim.setMaxWidth(el->maxWidth);
        prim.setWrap(el->wrap);
        prim.setLineHeight(el->lineHeight);
        prim.prepare();
        *glyphBytes += el->text.size();
    }
    return msSince(t0);
}

} // namespace

// 真正的探针实现（小文档档）
REGISTER_TEST(md_mode_switch_perf_probe_small,
              "markdown_module: 模式切换冻结量化·小文档（池369探针）", {
    std::string text;
    for (int s = 0; s < 4; ++s) {
        text += "# 节" + std::to_string(s) + "\n";
        for (int l = 0; l < 12; ++l) {
            text += "内容行 " + std::to_string(s) + "-" +
                    std::to_string(l) + " 一二三四五六七八九十\n";
        }
        text += "\n";
    }
    test_support::MdHeadlessEnv env("devdesk_md_ui_", "modesw_s", "ui_test.db");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(test_support::writeTextFile(env.dir / "note.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* tag) -> double {
        const auto t0 = std::chrono::steady_clock::now();
        ui.begin("msw");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const double composeMs = msSince(t0);
        std::fprintf(stderr,
                     "[modesw][small] %-10s compose+layout=%.1fms texts=%zu\n",
                     tag, composeMs, countTextElements(ui));
        return composeMs;
    };
    const auto pressNext = [&]() -> bool {
        const core::KeyEvent p =
            test_support::pressShortcut(core::InputKey::P, /*shift=*/true);
        const bool ok = mod->onKeyEvent(p);
        if (!ok) {
            std::fprintf(stderr, "[modesw][small] pressNext: onKeyEvent 未消费\n");
        }
        return ok;
    };

    frame("open");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("edit");   // 打开笔记：编辑态
    frame("settle");

    const char* names[] = {"live", "split", "preview", "edit"};
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            pressNext();
            frame(cycle == 0 ? names[i] : names[i]);
            frame("settle");
            frame("settle");
        }
    }

    // render 侧重建账：各模式当帧元素树 → TextPrimitive 全新建+prepare
    for (int i = 0; i < 4; ++i) {
        pressNext();   // 回到起点后再依次进各模式
        frame(names[i]);
        size_t count = 0, bytes = 0;
        const double rebuildMs = rebuildTextInstances(ui, &count, &bytes);
        std::fprintf(stderr,
                     "[modesw][small] %-10s render-rebuild: %zu texts, %zu "
                     "bytes, %.1fms\n",
                     names[i], count, bytes, rebuildMs);
        frame("settle");
    }

    mod->onStop();
    return true;
})

// 大文档档（同构，规模放大：~1800 行 / ~60KB 文本）
REGISTER_TEST(md_mode_switch_perf_probe_large,
              "markdown_module: 模式切换冻结量化·大文档（池369探针）", {
    std::string text;
    for (int s = 0; s < 150; ++s) {
        text += "# 节" + std::to_string(s) + "\n";
        for (int l = 0; l < 12; ++l) {
            text += "内容行 " + std::to_string(s) + "-" +
                    std::to_string(l) + " 一二三四五六七八九十\n";
        }
        text += "\n";
    }
    test_support::MdHeadlessEnv env("devdesk_md_ui_", "modesw_l", "ui_test.db");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(test_support::writeTextFile(env.dir / "note.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* tag) -> double {
        const auto t0 = std::chrono::steady_clock::now();
        ui.begin("mswl");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const double composeMs = msSince(t0);
        std::fprintf(stderr,
                     "[modesw][large] %-10s compose+layout=%.1fms texts=%zu\n",
                     tag, composeMs, countTextElements(ui));
        return composeMs;
    };
    const auto pressNext = [&]() -> bool {
        const core::KeyEvent p =
            test_support::pressShortcut(core::InputKey::P, /*shift=*/true);
        const bool ok = mod->onKeyEvent(p);
        if (!ok) {
            std::fprintf(stderr, "[modesw][large] pressNext: onKeyEvent 未消费\n");
        }
        return ok;
    };

    frame("open");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("edit");
    frame("settle");

    const char* names[] = {"live", "split", "preview", "edit"};
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            pressNext();
            frame(names[i]);
            frame("settle");
            frame("settle");
        }
    }

    for (int i = 0; i < 4; ++i) {
        pressNext();
        frame(names[i]);
        size_t count = 0, bytes = 0;
        const double rebuildMs = rebuildTextInstances(ui, &count, &bytes);
        std::fprintf(stderr,
                     "[modesw][large] %-10s render-rebuild: %zu texts, %zu "
                     "bytes, %.1fms\n",
                     names[i], count, bytes, rebuildMs);
        frame("settle");
    }

    mod->onStop();
    return true;
})
