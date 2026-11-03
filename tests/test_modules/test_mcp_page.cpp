// tests/test_modules/test_mcp_page.cpp
// MCP 菜单页（UI 批）用例：PageId/registry 接线、卡片网格、详情弹窗（打开/关闭
// 三通道）、「试一下」后台线程 + compose 收割、服务状态行、空态。
// 防的事故：枚举插错位（侧栏序漂移）、表行与模块虚函数双副本漂移、UI 线程直调
// execute（卡死主线程）、弹窗关不掉、页面注销时在途线程悬垂。
#include "doctest.h"

#include "mcp_page_module.h"

#include "app_registry.h"
#include "core/mcp_tool.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "mcp_service_bridge.h"
#include "memory_mcp_bridge.h"
#include "settings_config.h"

#include <eui/dsl.h>
#include <components/input.h>  // InputModel::InputState（参数输入框全选清空的测试注入用）

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

namespace {

using module::mcp::McpPageModule;

// 可编程假工具：execute 记录调用线程与次数，成功/业务失败两态。
class FakeTool final : public core::mcp::IMcpTool {
public:
    FakeTool(std::string name, std::string desc, std::string schema)
        : name_(std::move(name)), desc_(std::move(desc)),
          schema_(std::move(schema)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return desc_; }
    std::string inputSchemaJson() const override { return schema_; }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        lastArgs = argumentsJson;
        executeThreadId = std::this_thread::get_id();
        ++executeCalls;
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
        if (businessFail) {
            return core::Result<core::mcp::ToolResult>::ok(
                core::mcp::ToolResult{true, "boom:业务失败"});
        }
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false,
                                  okText.empty() ? "ok-42:" + name_ : okText});
    }

    std::string name_;
    std::string desc_;
    std::string schema_;
    int executeCalls = 0;
    int sleepMs = 0;
    bool businessFail = false;
    std::string lastArgs;  // 池146：记录收到的入参 JSON（组装断言）
    std::string okText;    // 非空时替代默认成功文案（多行结果用例）
    std::thread::id executeThreadId;
};

// 桥 recording 桩（模式同 test_mcp_settings.cpp）：状态行用例注入运行态。
struct BridgeState {
    bool running = false;
    int port = 0;
    std::string lastError;
};
BridgeState g_bridge;
app::mcp::LiveStatus recStatus() {
    app::mcp::LiveStatus st;
    st.running = g_bridge.running;
    st.port = g_bridge.port;
    st.lastError = g_bridge.lastError;
    return st;
}
core::Result<void> recStart(int) { return core::Result<void>::ok(); }
void recStop() {}
core::Result<void> recApplyPort(int) { return core::Result<void>::ok(); }

// RAII：装四槽 recording（出厂态回默认桩，防桩残留串扰）。
struct BridgeGuard {
    BridgeGuard() { app::mcp::setHandlers(&recStart, &recStop, &recApplyPort, &recStatus); }
    ~BridgeGuard() { app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr); }
    BridgeGuard(const BridgeGuard&) = delete;
    BridgeGuard& operator=(const BridgeGuard&) = delete;
};

// 全局注册表 RAII：注册进 globalMcpTools，析构注销（默认构造模块的数据源用例）。
struct GlobalToolGuard {
    explicit GlobalToolGuard(std::shared_ptr<core::mcp::IMcpTool> tool)
        : name_(tool->name()), tool_(std::move(tool)) {
        (void)core::mcp::globalMcpTools().registerTool(tool_);
    }
    ~GlobalToolGuard() { core::mcp::globalMcpTools().unregisterTool(name_); }
    GlobalToolGuard(const GlobalToolGuard&) = delete;
    GlobalToolGuard& operator=(const GlobalToolGuard&) = delete;
    std::string name_;
    std::shared_ptr<core::mcp::IMcpTool> tool_;
};

// 一帧 compose + layout（frame 断言必须真 layout）。
void composeFrame(McpPageModule& mod, eui::Ui& ui, const eui::Screen& screen,
                  const char* frame) {
    ui.begin(frame);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

eui::Screen makeScreen() {
    eui::Screen s;
    s.width = 1200.0f;
    s.height = 800.0f;
    s.scale = 1.0f;
    return s;
}

// 两个标准假工具（名字字典序 = registry list 序）。
void seedTwoTools(core::mcp::McpToolRegistry& reg,
                  std::shared_ptr<FakeTool>& alpha, std::shared_ptr<FakeTool>& beta) {
    alpha = std::make_shared<FakeTool>("test_alpha_tool", "测试工具甲的说明",
                                       R"({"type":"object","properties":{}})");
    beta = std::make_shared<FakeTool>("test_beta_tool", "测试工具乙的说明",
                                      R"({"type":"object","required":["url"]})");
    (void)reg.registerTool(alpha);
    (void)reg.registerTool(beta);
}

// bug145 后结果区按行渲染（result.line.N 单行元素入滚动区）：拼接全部行文本供断言。
std::string resultTextAll(eui::Ui& ui) {
    std::string joined;
    for (int i = 0;; ++i) {
        const auto* el =
            ui.find("mcp.dialog.detail.result.line." + std::to_string(i));
        if (el == nullptr) break;
        if (!joined.empty()) joined += "\n";
        joined += el->text;
    }
    return joined;
}

// 有界轮询：compose 收割是每帧协作式，worker 置 done 后由下一帧收割。
bool pollUntilResult(McpPageModule& mod, eui::Ui& ui, const eui::Screen& screen,
                     const std::string& marker) {
    for (int i = 0; i < 200; ++i) {
        composeFrame(mod, ui, screen, "mcp_poll");
        if (resultTextAll(ui).find(marker) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

// 防：枚举插错位置导致侧栏/分发顺序漂移（wallpaper_pageid_order 的镜像契约）。
REGISTER_TEST(mcp_page_pageid_order,
              "mcp_page: PageId 序 Wallpaper < Mcp < Settings 且 pageIdToString 为 Mcp", {
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Wallpaper) <
                  static_cast<int>(::core::PageId::Mcp));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Mcp) <
                  static_cast<int>(::core::PageId::Settings));
    DOCTEST_CHECK_EQ(std::string(::core::pageIdToString(::core::PageId::Mcp)),
                     std::string("Mcp"));
    return true;
})

// 防：表行与模块虚函数双副本漂移（gatingKey/短名/图标/工厂四处改一漏三）。
REGISTER_TEST(mcp_page_registry_row,
              "mcp_page: 表第 8 行=Mcp，gatingKey/shortName/icon 与工厂实例一致，默认门控关", {
    using namespace module::settings;
    const ::app::registry::ModuleEntry* row = nullptr;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Mcp) {
            row = &e;
            break;
        }
    }
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->gatingKey != nullptr);
    DOCTEST_CHECK_EQ(std::string(row->gatingKey), std::string(kModuleIdMcp));
    DOCTEST_CHECK_EQ(std::string(row->shortName), std::string("MCP"));
    DOCTEST_CHECK_EQ(std::string(row->iconGlyph), std::string("\xEF\x87\xA6"));

    std::unique_ptr<::core::Module> mod(row->factory());
    DOCTEST_REQUIRE(mod != nullptr);
    DOCTEST_CHECK(mod->pageId() == ::core::PageId::Mcp);
    DOCTEST_CHECK_EQ(mod->shortName(), std::string("MCP"));
    DOCTEST_CHECK_EQ(mod->iconGlyph(), std::string("\xEF\x87\xA6"));

    const SettingsConfig cfg;
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdMcp));  // 默认关（agent/wallpaper 同口径）
    return true;
})

// 防：网格不渲染 / 卡片缺件（图标、名称、描述、双 chip、命中面）。
REGISTER_TEST(mcp_page_grid_cards,
              "mcp_page: 卡片网格按字典序出卡，名称/描述/chip/hit 齐全，列宽自适应，计数正确", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_grid");

    const auto* grid = ui.find("mcp.grid");
    DOCTEST_REQUIRE(grid != nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.card.test_alpha_tool"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.card.test_beta_tool"), nullptr);
    DOCTEST_CHECK_EQ(ui.find("mcp.empty.message"), nullptr);  // 非空态：空态不得建
    const auto* name = ui.find("mcp.card.test_alpha_tool.name");
    DOCTEST_REQUIRE(name != nullptr);
    DOCTEST_CHECK_EQ(name->text, std::string("test_alpha_tool"));
    const auto* desc = ui.find("mcp.card.test_alpha_tool.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK_EQ(desc->text, std::string("测试工具甲的说明"));
    DOCTEST_CHECK_EQ(desc->overflow, core::TextOverflow::Ellipsis);  // 单行截断
    DOCTEST_REQUIRE_NE(ui.find("mcp.card.test_alpha_tool.tag.builtin"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.card.test_alpha_tool.tag.svc"), nullptr);
    const auto* hit = ui.find("mcp.card.test_alpha_tool.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onClick != nullptr);
    const auto* card = ui.find("mcp.card.test_alpha_tool");
    // 2026-11-01 F-13：卡宽不再定死 340，改为随内容宽自适应（区间 340–420）
    DOCTEST_CHECK((card->width.value >= 340.0f));
    DOCTEST_CHECK((card->width.value <= 420.0f));
    const auto* count = ui.find("mcp.section.count");
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_CHECK_EQ(count->text, std::string("2"));
    return true;
})

// 防：F-01 描述列与状态列咬合——长描述末尾「…」与「未启用」粘连成一句。
// 判据：描述行右端到状态圆点左端 ≥ 16px。
REGISTER_TEST(mcp_page_desc_status_gap,
              "mcp_page: 描述列右端与状态列左端间距 ≥16px，不咬合", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_gap");

    const auto* desc = ui.find("mcp.card.test_alpha_tool.desc");
    const auto* dot = ui.find("mcp.card.test_alpha_tool.status.dot");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_REQUIRE(dot != nullptr);
    const float descRight = desc->frame.x + desc->frame.width;
    DOCTEST_CHECK((dot->frame.x - descRight >= 16.0f));
    return true;
})

// 防：F-13 计数 chip 离标题太远（曾相距 700px，等于两处独立信息）。
// 判据：计数 chip 左缘紧跟标题右缘（间距 ≤ 24px）。
REGISTER_TEST(mcp_page_count_chip_near_title,
              "mcp_page: 计数 chip 紧跟区标题，间距 ≤24px", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_chip");

    const auto* title = ui.find("mcp.section.title");
    const auto* chip = ui.find("mcp.section.count.bg");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(chip != nullptr);
    const float titleRight = title->frame.x + title->frame.width;
    DOCTEST_CHECK((chip->frame.x - titleRight <= 24.0f));
    DOCTEST_CHECK((chip->frame.x - titleRight >= 0.0f));
    return true;
})

// 防：F-12 服务 chip 借用 purple（用户可选强调色槽位）→ 微信绿白体系里冒出唯一紫。
// 判据：服务 chip 底色不是紫色系（蓝分量显著高于红分量）。
REGISTER_TEST(mcp_page_no_purple_tag,
              "mcp_page: 服务 chip 用中性灰底，不用 purple 强调色", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_purple");

    const auto* svc = ui.find("mcp.card.test_alpha_tool.tag.svc.bg");
    DOCTEST_REQUIRE(svc != nullptr);
    // 精确判定：底色就是主题的 borderMuted（中性灰），而不是 purple
    const auto& expect = core::theme::current().colors.borderMuted;
    const auto& c = svc->color;
    DOCTEST_CHECK(std::fabs(c.r - expect.r) < 0.001f);
    DOCTEST_CHECK(std::fabs(c.g - expect.g) < 0.001f);
    DOCTEST_CHECK(std::fabs(c.b - expect.b) < 0.001f);
    return true;
})

// 防：空注册表白页（无任何提示、计数残留旧值）。
REGISTER_TEST(mcp_page_empty_state,
              "mcp_page: 空注册表 → 统一空态在、无卡片、计数 0", {
    core::mcp::McpToolRegistry reg;
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_empty");

    DOCTEST_REQUIRE_NE(ui.find("mcp.empty.message"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.empty.guide"), nullptr);
    DOCTEST_CHECK_EQ(ui.find("mcp.card.test_alpha_tool"), nullptr);
    const auto* count = ui.find("mcp.section.count");
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_CHECK_EQ(count->text, std::string("0"));
    return true;
})

// 防：点卡无响应 / 弹窗内容缺 schema。
REGISTER_TEST(mcp_page_card_click_opens_dialog,
              "mcp_page: 点卡开详情弹窗——backdrop opacity 1、hero 名称/schema JSON/试一下钮在", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_open_1");
    const auto* backdrop = ui.find("mcp.dialog.detail.backdrop");
    DOCTEST_REQUIRE(backdrop != nullptr);
    DOCTEST_CHECK_EQ(backdrop->opacity, 0.0f);  // 初始关闭态

    const auto* hit = ui.find("mcp.card.test_beta_tool.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onClick != nullptr);
    hit->onClick();
    composeFrame(mod, ui, screen, "mcp_open_2");

    const auto* backdrop2 = ui.find("mcp.dialog.detail.backdrop");
    DOCTEST_REQUIRE(backdrop2 != nullptr);
    DOCTEST_CHECK_EQ(backdrop2->opacity, 1.0f);
    const auto* heroName = ui.find("mcp.dialog.detail.hero.name");
    DOCTEST_REQUIRE(heroName != nullptr);
    DOCTEST_CHECK_EQ(heroName->text, std::string("test_beta_tool"));
    const auto* schema = ui.find("mcp.dialog.detail.schema.text");
    DOCTEST_REQUIRE(schema != nullptr);
    DOCTEST_CHECK(schema->text.find("\"required\"") != std::string::npos);
    const auto* tryBtn = ui.find("mcp.dialog.detail.btn.try.bg");
    DOCTEST_REQUIRE(tryBtn != nullptr);
    DOCTEST_REQUIRE(tryBtn->onClick != nullptr);
    const auto* closeBtn = ui.find("mcp.dialog.detail.btn.close.bg");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
    return true;
})

// 防：弹窗只能开不能关（关闭按钮通道）。
REGISTER_TEST(mcp_page_dialog_close_button,
              "mcp_page: 弹窗「关闭」按钮 → backdrop opacity 0", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_close_1");
    ui.find("mcp.card.test_alpha_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_close_2");
    DOCTEST_REQUIRE_EQ(ui.find("mcp.dialog.detail.backdrop")->opacity, 1.0f);

    const auto* closeBtn = ui.find("mcp.dialog.detail.btn.close.bg");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
    closeBtn->onClick();
    composeFrame(mod, ui, screen, "mcp_close_3");
    DOCTEST_CHECK_EQ(ui.find("mcp.dialog.detail.backdrop")->opacity, 0.0f);
    return true;
})

// 防：弹窗只能开不能关（Esc 通道；Router 派发当前页模块）。
REGISTER_TEST(mcp_page_dialog_esc_close,
              "mcp_page: Esc 按键 → 弹窗关闭且消费事件；弹窗未开时 Esc 不消费", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_esc_1");

    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK_FALSE(mod.onKeyEvent(esc));  // 未开弹窗：不消费（Esc 让全局语义走）

    ui.find("mcp.card.test_alpha_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_esc_2");
    DOCTEST_REQUIRE_EQ(ui.find("mcp.dialog.detail.backdrop")->opacity, 1.0f);
    DOCTEST_CHECK(mod.onKeyEvent(esc));
    composeFrame(mod, ui, screen, "mcp_esc_3");
    DOCTEST_CHECK_EQ(ui.find("mcp.dialog.detail.backdrop")->opacity, 0.0f);
    return true;
})

// 防：UI 线程直调 execute（卡死主线程）——execute 必须发生在另一线程，经 compose 收割。
REGISTER_TEST(mcp_page_try_runs_on_background_thread,
              "mcp_page: 试一下 → execute 在后台线程、结果区出现成功前缀与文本、只调一次", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_try_1");
    ui.find("mcp.card.test_alpha_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_try_2");

    const auto mainTid = std::this_thread::get_id();
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    DOCTEST_CHECK(pollUntilResult(mod, ui, screen, "ok-42:test_alpha_tool"));
    DOCTEST_CHECK_EQ(alpha->executeCalls, 1);
    DOCTEST_CHECK(alpha->executeThreadId != mainTid);  // 禁 UI 线程直调
    DOCTEST_CHECK(resultTextAll(ui).find("成功") != std::string::npos);
    return true;
})

// 防：失败态被当成功渲染（isError 与基础设施失败都要走失败文案）。
REGISTER_TEST(mcp_page_try_error_path,
              "mcp_page: 业务失败（isError）→ 结果区失败前缀 + 错误文本", {
    core::mcp::McpToolRegistry reg;
    auto tool = std::make_shared<FakeTool>("test_fail_tool", "会失败的工具",
                                           R"({"type":"object"})");
    tool->businessFail = true;
    (void)reg.registerTool(tool);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_err_1");
    ui.find("mcp.card.test_fail_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_err_2");
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    DOCTEST_CHECK(pollUntilResult(mod, ui, screen, "boom:业务失败"));
    DOCTEST_CHECK(resultTextAll(ui).find("失败") != std::string::npos);
    return true;
})

// 池146：参数表单——扁平 schema 出表单（label/输入/开关齐全、schema JSON 框退位）；
// 缺必填拦截（execute 不发起 + 红字点名参数）；填参试跑 → execute 收到组装入参。
REGISTER_TEST(mcp_page_param_form_validation_and_args,
              "mcp_page: 扁平 schema → 参数表单齐全；缺必填 → 不发起且红字点名参数；填参试跑 → execute 收到组装入参", {
    core::mcp::McpToolRegistry reg;
    auto tool = std::make_shared<FakeTool>(
        "test_form_tool", "带参工具",
        R"({"type":"object","properties":{"url":{"type":"string"},"limit":{"type":"integer"},"dry":{"type":"boolean"}},"required":["url"]})");
    (void)reg.registerTool(tool);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_form_1");
    ui.find("mcp.card.test_form_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_form_2");

    // 表单件齐全；schema JSON 框退位
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.param.url.label"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.param.url.input"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.param.limit.input"), nullptr);
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.param.dry.sw.track"), nullptr);
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.schema.text") == nullptr);

    // 缺必填点击「试一下」：execute 不发起 + 红字点名参数
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_form_3");
    DOCTEST_CHECK_EQ(tool->executeCalls, 0);
    const auto* err = ui.find("mcp.dialog.detail.params.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("url") != std::string::npos);

    // 填参（TextInput 注入 input 的 .hit，走真实 onChange 草稿链）+ 拨开关
    const auto typeIn = [&](const char* param, const char* text) {
        core::TextInputEvent ev;
        ev.text = text;
        ui.find(std::string("mcp.dialog.detail.param.") + param + ".input.hit")
            ->onTextInput(ev);
    };
    typeIn("url", "https://example.com");
    typeIn("limit", "7");
    ui.find("mcp.dialog.detail.param.dry.sw.track")->onClick();
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    DOCTEST_CHECK(pollUntilResult(mod, ui, screen, "ok-42:test_form_tool"));
    DOCTEST_CHECK_EQ(tool->executeCalls, 1);
    DOCTEST_CHECK(tool->lastArgs.find("https://example.com") !=
                  std::string::npos);
    DOCTEST_CHECK(tool->lastArgs.find("limit") != std::string::npos);
    DOCTEST_CHECK(tool->lastArgs.find("7") != std::string::npos);
    DOCTEST_CHECK(tool->lastArgs.find("dry") != std::string::npos);
    DOCTEST_CHECK(tool->lastArgs.find("true") != std::string::npos);
    return true;
})

// bug145 + 池146 数字校验：integer 填非数字 → execute 不发起且文案点名参数与错因；
// 多行结果按行渲染进滚动区（result.line.N 逐行元素、滚动区在）。
REGISTER_TEST(mcp_page_number_validation_and_result_lines,
              "mcp_page: integer 填非数字 → 不发起且红字点名；多行结果按行渲染进 result.scroll", {
    core::mcp::McpToolRegistry reg;
    auto tool = std::make_shared<FakeTool>(
        "test_num_tool", "数字参数工具",
        R"({"type":"object","properties":{"limit":{"type":"integer"}}})");
    tool->okText = "l0\nl1\nl2";
    (void)reg.registerTool(tool);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_num_1");
    ui.find("mcp.card.test_num_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_num_2");

    // 非数字：拦截 + 文案点名参数与错因
    const auto typeIn = [&](const char* text) {
        core::TextInputEvent ev;
        ev.text = text;
        ui.find("mcp.dialog.detail.param.limit.input.hit")->onTextInput(ev);
    };
    typeIn("abc");
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_num_3");
    DOCTEST_CHECK_EQ(tool->executeCalls, 0);
    const auto* err = ui.find("mcp.dialog.detail.params.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("limit") != std::string::npos);
    DOCTEST_CHECK(err->text.find("整数") != std::string::npos);

    // 改填合法值试跑：先退格清掉「abc」（TextInput 是光标插入语义，不清会拼成
    // 「abc3」仍非法——走 input.hit 的 onKeyEvent 真实删除链），再填 3 试跑；
    // 结果按行渲染（前缀行 + 3 数据行），滚动区在
    // 改填合法值试跑：全选退格清掉「abc」再填 3——TextInput 注入后光标停在 0 位，
    // 裸退格是空操作、「3」会插到最前拼成「3abc」仍非法；须经 InputState 全选
    // 再退一格走真实删除链（test_markdown clearInput 同款）。
    // 结果按行渲染（前缀行 + 3 数据行），滚动区在
    {
        auto& st = ui.state<components::input_detail::InputModel::InputState>(
            "mcp.dialog.detail.param.limit.input");
        st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        eui::KeyEvent bs;
        bs.key = eui::InputKey::Backspace;
        ui.find("mcp.dialog.detail.param.limit.input.hit")->onKeyEvent(bs);
    }
    typeIn("3");
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    DOCTEST_CHECK(pollUntilResult(mod, ui, screen, "l1"));
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.result.scroll"), nullptr);
    const auto* line0 = ui.find("mcp.dialog.detail.result.line.0");
    DOCTEST_REQUIRE(line0 != nullptr);
    DOCTEST_CHECK_EQ(line0->text, std::string("成功："));
    DOCTEST_REQUIRE_NE(ui.find("mcp.dialog.detail.result.line.3"), nullptr);
    DOCTEST_CHECK_EQ(ui.find("mcp.dialog.detail.result.line.3")->text,
                     std::string("l2"));
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.result.line.4") == nullptr);
    return true;
})

// 防：页面注销（模块开关关）时在途线程被硬杀或悬垂——onStop 必须 join 收束。
REGISTER_TEST(mcp_page_onstop_joins_running_try,
              "mcp_page: 试一下在途时 onStop → 等待执行完成（join 收束）不悬垂", {
    core::mcp::McpToolRegistry reg;
    auto tool = std::make_shared<FakeTool>("test_slow_tool", "慢工具",
                                           R"({"type":"object"})");
    tool->sleepMs = 150;
    (void)reg.registerTool(tool);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_stop_1");
    ui.find("mcp.card.test_slow_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_stop_2");
    ui.find("mcp.dialog.detail.btn.try.bg")->onClick();
    mod.onStop();  // 立即停：必须跨过 150ms 睡眠真 join
    DOCTEST_CHECK_EQ(tool->executeCalls, 1);
    composeFrame(mod, ui, screen, "mcp_stop_3");
    DOCTEST_CHECK(resultTextAll(ui).find("ok-42:test_slow_tool") !=
                  std::string::npos);
    return true;
})

// 防：状态行不读桥或文案不带端点。
REGISTER_TEST(mcp_page_status_line,
              "mcp_page: 状态行默认「已停止」；桥运行态 → 「运行中 · 127.0.0.1:端口」且状态点为 success", {
    core::mcp::McpToolRegistry reg;
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    {
        eui::Ui ui;  // 默认桩 = 未运行
        composeFrame(mod, ui, screen, "mcp_status_1");
        const auto* text = ui.find("mcp.status.text");
        DOCTEST_REQUIRE(text != nullptr);
        DOCTEST_CHECK(text->text.find("已停止") != std::string::npos);
    }
    BridgeGuard guard;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8931;
    {
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_status_2");
        const auto* text = ui.find("mcp.status.text");
        DOCTEST_REQUIRE(text != nullptr);
        DOCTEST_CHECK(text->text.find("运行中") != std::string::npos);
        DOCTEST_CHECK(text->text.find("127.0.0.1:8931") != std::string::npos);
        const auto* dot = ui.find("mcp.status.dot");
        DOCTEST_REQUIRE(dot != nullptr);
        const auto& t = core::theme::current();
        DOCTEST_CHECK(dot->color.r == t.colors.success.r);
        DOCTEST_CHECK(dot->color.g == t.colors.success.g);
        DOCTEST_CHECK(dot->color.b == t.colors.success.b);
    }
    return true;
})

// 防：hover CTA 恒显或恒隐（.states+onHover 通道失效）。
REGISTER_TEST(mcp_page_hover_cta,
              "mcp_page: 卡片 hover → CTA 浮现（opacity 1）；移出 → 隐藏（opacity 0）", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_hover_1");
    const auto* cta = ui.find("mcp.card.test_alpha_tool.cta");
    DOCTEST_REQUIRE(cta != nullptr);
    DOCTEST_CHECK_EQ(cta->opacity, 0.0f);
    {
        // ⚠ 每帧重建树：回调指针只在本帧有效，跨 compose 必须重新 find
        const auto* hit = ui.find("mcp.card.test_alpha_tool.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onHoverChanged != nullptr);
        hit->onHoverChanged(true);
    }
    composeFrame(mod, ui, screen, "mcp_hover_2");
    DOCTEST_CHECK_EQ(ui.find("mcp.card.test_alpha_tool.cta")->opacity, 1.0f);
    {
        const auto* hit = ui.find("mcp.card.test_alpha_tool.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        hit->onHoverChanged(false);
    }
    composeFrame(mod, ui, screen, "mcp_hover_3");
    DOCTEST_CHECK_EQ(ui.find("mcp.card.test_alpha_tool.cta")->opacity, 0.0f);
    return true;
})

// 防：默认构造读不到全局注册表（生产工厂的数据源契约）。
REGISTER_TEST(mcp_page_default_ctor_reads_global_registry,
              "mcp_page: 默认构造模块消费 core::mcp::globalMcpTools() 的工具快照", {
    auto tool = std::make_shared<FakeTool>("test_global_tool", "全局注册表工具",
                                           R"({"type":"object"})");
    GlobalToolGuard guard(tool);
    McpPageModule mod;  // 默认构造 = 全局注册表（生产工厂同参）
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_global");
    DOCTEST_REQUIRE_NE(ui.find("mcp.card.test_global_tool"), nullptr);
    return true;
})

// 记忆 MCP 桥 recording 桩（RAII 复位，防桩残留串扰）。
bool g_memSwitchOn = false;
app::memory_mcp::LiveStatus memStatusRec() {
    return app::memory_mcp::LiveStatus{g_memSwitchOn};
}
core::Result<void> memSetEnabledRec(bool on) {
    g_memSwitchOn = on;
    return core::Result<void>::ok();
}
struct MemoryBridgeGuard {
    MemoryBridgeGuard() {
        app::memory_mcp::setHandlers(&memStatusRec, &memSetEnabledRec);
    }
    ~MemoryBridgeGuard() { app::memory_mcp::setHandlers(nullptr, nullptr); }
    MemoryBridgeGuard(const MemoryBridgeGuard&) = delete;
    MemoryBridgeGuard& operator=(const MemoryBridgeGuard&) = delete;
};

// 开关轨道色：checked == tokens.primary（SwitchStyle.on），unchecked 为灰混合。
bool trackIsPrimary(const core::dsl::Element* track) {
    const auto& t = core::theme::current();
    return track->color.r == t.colors.primary.r &&
           track->color.g == t.colors.primary.g &&
           track->color.b == t.colors.primary.b;
}

// 防：模块行缺件 / 开关不读桥 / 失败反馈缺失或视觉不回弹（记忆 MCP 行）。
REGISTER_TEST(mcp_page_memory_module_row,
              "mcp_page: 记忆模块行全宽 64 高、标题/说明/开关齐全；桥失败 → 行内红字且轨道回弹；桥成功 → 轨道点亮", {
    core::mcp::McpToolRegistry reg;
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    {
        eui::Ui ui;  // 默认桩（未装配）：开关 = 关
        composeFrame(mod, ui, screen, "mcp_memrow_1");
        const auto* row = ui.find("mcp.module.memory");
        DOCTEST_REQUIRE(row != nullptr);
        // 全宽行卡（非 340 网格卡）：contentW = 1200 - 页边距 64，行高 64
        DOCTEST_CHECK_EQ(row->width.value, 1136.0f);
        DOCTEST_CHECK_EQ(row->height.value, 64.0f);
        const auto* title = ui.find("mcp.module.memory.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("记忆"));
        const auto* desc = ui.find("mcp.module.memory.desc");
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_CHECK(desc->text.find("memory_search") != std::string::npos);
        const auto* toggle = ui.find("mcp.module.memory.toggle.hit");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        const auto* track = ui.find("mcp.module.memory.toggle.track");
        DOCTEST_REQUIRE(track != nullptr);
        DOCTEST_CHECK_FALSE(trackIsPrimary(track));  // 默认桩 = 关
    }
    {
        eui::Ui ui;  // 桥未装配时点击：setEnabled fail → 红字 + 轨道回弹（仍灰）
        composeFrame(mod, ui, screen, "mcp_memrow_2");
        ui.find("mcp.module.memory.toggle.hit")->onClick();
        composeFrame(mod, ui, screen, "mcp_memrow_3");
        const auto* desc = ui.find("mcp.module.memory.desc");
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_CHECK_MESSAGE(
            desc->text.find("保存失败") != std::string::npos,
            "写盘失败红字缺失: " + desc->text);
        const auto& t = core::theme::current();
        DOCTEST_CHECK(desc->textColor.r == t.colors.errorText.r);
        DOCTEST_CHECK(desc->textColor.g == t.colors.errorText.g);
        DOCTEST_CHECK(desc->textColor.b == t.colors.errorText.b);
        const auto* track = ui.find("mcp.module.memory.toggle.track");
        DOCTEST_REQUIRE(track != nullptr);
        DOCTEST_CHECK_FALSE(trackIsPrimary(track));  // 回弹
    }
    {
        MemoryBridgeGuard guard;
        g_memSwitchOn = false;
        eui::Ui ui;  // 桥已装配：点击 → 成功 → 下一帧轨道点亮、红字让位回说明
        composeFrame(mod, ui, screen, "mcp_memrow_4");
        ui.find("mcp.module.memory.toggle.hit")->onClick();
        DOCTEST_CHECK(g_memSwitchOn);  // onChange(true) 真到达桥
        composeFrame(mod, ui, screen, "mcp_memrow_5");
        const auto* track = ui.find("mcp.module.memory.toggle.track");
        DOCTEST_REQUIRE(track != nullptr);
        DOCTEST_CHECK_TRUE(trackIsPrimary(track));
        const auto* desc = ui.find("mcp.module.memory.desc");
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_CHECK(desc->text.find("保存失败") == std::string::npos);
    }
    {
        // 130% 字号档行盒不重叠（PRD §7 自设判据）：说明行 y 必须随字号缩放，
        // 与标题盒保持间隙且行底不越 64 盒（R4 防回归）
        core::theme::setFontScale(1.3f);
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_memrow_6");
        const auto* title = ui.find("mcp.module.memory.title");
        const auto* desc = ui.find("mcp.module.memory.desc");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_CHECK_MESSAGE(
            desc->y >= title->y + title->height.value,
            "130% 档标题/说明行盒重叠：标题底 " +
                std::to_string(title->y + title->height.value) + " 说明顶 " +
                std::to_string(desc->y));
        DOCTEST_CHECK_MESSAGE(desc->y + desc->height.value <= 64.0f,
                              "130% 档说明行越出 64 行盒：底 " +
                                  std::to_string(desc->y + desc->height.value));
        core::theme::setFontScale(1.0f);
    }
    return true;
})
