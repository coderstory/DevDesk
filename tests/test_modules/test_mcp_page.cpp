// tests/test_modules/test_mcp_page.cpp
// MCP 菜单页（UI 批）用例：PageId/registry 接线、卡片网格、详情弹窗（打开/关闭
// 三通道）、「试一下」后台线程 + compose 收割、服务状态行、空态。
// 防的事故：枚举插错位（侧栏序漂移）、表行与模块虚函数双副本漂移、UI 线程直调
// execute（卡死主线程）、弹窗关不掉、页面注销时在途线程悬垂。
#include "doctest.h"

#include "mcp_page_module.h"

#include "app_registry.h"
#include "core/fs_text.h"  // need244：读回写盘内容断言
#include "core/mcp_tool.h"
#include "core/module_interface.h"
#include "core/paths.h"  // need244/审计 P1-2：真文件定位走生产契约
#include "core/theme.h"
#include "mcp_service_bridge.h"
#include "memory_mcp_bridge.h"
#include "settings_config.h"
#include "test_support.h"  // need244：setEnvVar 隔离 ~/.claude.json

#include <eui/dsl.h>
#include <components/input.h>  // InputModel::InputState（参数输入框全选清空的测试注入用）

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <cstdio>
#include <vector>

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

// ── need343「文本全左侧」结构断言 ──
// 真值源 = 设计稿具名常量（docs/design/2026-11-18-mcp-card-redesign.md §4），
// 断言引用常量名不写旧布位魔数（复盘机制②教训：布位迁移不破断言）。
namespace mcpDesign {
constexpr float kCardH = 96.0f;        // §4.1 卡高（原 120）
constexpr float kTextX = 16.0f;        // §4.2 文本列单基线（卡内相对）
constexpr float kNameY = 16.0f;        // §4.2 名称行盒顶
constexpr float kStatusY = 42.0f;      // §4.2 状态行盒顶
constexpr float kDescY = 64.0f;        // §4.2 描述行盒顶
constexpr float kRightTrackW = 56.0f;  // §4.4 右轨宽（卡右缘到右轨外缘）
constexpr float kIconSize = 40.0f;     // §4.4 图标位
constexpr float kArrowSize = 16.0f;    // §4.4 hover 箭头
constexpr float kDotSize = 8.0f;       // §4.3 状态点
constexpr float kGapDotWord = 6.0f;    // §4.3 点↔词间距
constexpr float kGapWordChip = 12.0f;  // §4.3 词↔chip 间距
constexpr float kGapChipChip = 6.0f;   // §4.3 chip↔chip 间距
constexpr float kEps = 0.5f;           // 浮点比较容差
} // namespace mcpDesign

// 防：need343 P3/P4 双左缘回潮——名称/描述/状态行行首必须同一条左缘 x=16
//（状态词与双 chip 在状态行内按 §4.3 间距右链，不另起基线），行序与行盒顶 =
// 设计稿 §4.2（名称 16 / 状态 42 / 描述 64），卡高 96。
REGISTER_TEST(mcp_page_card_text_all_left,
              "mcp_page: need343 文本列单基线——名称/描述/状态行首同 x=16、行盒顶 16/42/64、卡高 96", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_left_1");

    const auto* card = ui.find("mcp.card.test_alpha_tool");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_CHECK_EQ(card->height.value, mcpDesign::kCardH);
    const auto* nameEl = ui.find("mcp.card.test_alpha_tool.name");
    const auto* word = ui.find("mcp.card.test_alpha_tool.status.text");
    const auto* chipB = ui.find("mcp.card.test_alpha_tool.tag.builtin");
    const auto* chipS = ui.find("mcp.card.test_alpha_tool.tag.svc");
    const auto* desc = ui.find("mcp.card.test_alpha_tool.desc");
    DOCTEST_REQUIRE(nameEl != nullptr);
    DOCTEST_REQUIRE(word != nullptr);
    DOCTEST_REQUIRE(chipB != nullptr);
    DOCTEST_REQUIRE(chipS != nullptr);
    DOCTEST_REQUIRE(desc != nullptr);
    // 单基线（§4.2/§4.3）：名称/描述左缘 = 卡左缘+16；状态行行首=点 x+16，
    // 词与双 chip 按 §4.3 间距右链（不另起左缘）。（frame 为窗口绝对坐标）
    const float baseX = card->frame.x + mcpDesign::kTextX;
    const auto* dot = ui.find("mcp.card.test_alpha_tool.status.dot");
    DOCTEST_REQUIRE(dot != nullptr);
    DOCTEST_CHECK(std::fabs(nameEl->frame.x - baseX) < mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(desc->frame.x - baseX) < mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(dot->frame.x - baseX) < mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(word->frame.x -
                            (baseX + mcpDesign::kDotSize + mcpDesign::kGapDotWord)) <
                  mcpDesign::kEps);
    const float wordRight = word->frame.x + word->frame.width;
    DOCTEST_CHECK(std::fabs(chipB->frame.x - (wordRight + mcpDesign::kGapWordChip)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(chipS->frame.x -
                            (chipB->frame.x + chipB->frame.width +
                             mcpDesign::kGapChipChip)) < mcpDesign::kEps);
    // 行序钉板：名称 16 / 状态行（词+chips）42 / 描述 64（相对卡顶）
    DOCTEST_CHECK(std::fabs(nameEl->frame.y - (card->frame.y + mcpDesign::kNameY)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(word->frame.y - (card->frame.y + mcpDesign::kStatusY)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(chipB->frame.y - (card->frame.y + mcpDesign::kStatusY)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(desc->frame.y - (card->frame.y + mcpDesign::kDescY)) <
                  mcpDesign::kEps);
    // 名称单行省略号（§4.2 溢出规则；长名不折行不撑高）
    DOCTEST_CHECK_EQ(nameEl->overflow, core::TextOverflow::Ellipsis);
    return true;
})

// 防：need343 P1/P2 右侧文本回潮——右轨只能有图标底/图标字形/hover 纯图标
// 箭头；旧「查看详情」文字 CTA 节点必须消失，右侧不出现任何文字。
REGISTER_TEST(mcp_page_card_right_track_icon_only,
              "mcp_page: need343 右轨零文本——图标 40×40 在右上(卡宽−56,16)、hover 箭头纯图标右下(卡宽−32,64)、旧文字 CTA 消失", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_rtrack_1");

    const auto* card = ui.find("mcp.card.test_alpha_tool");
    DOCTEST_REQUIRE(card != nullptr);
    const float cardW = card->width.value;
    const auto* iconBg = ui.find("mcp.card.test_alpha_tool.icon.bg");
    const auto* arrow = ui.find("mcp.card.test_alpha_tool.cta");
    DOCTEST_REQUIRE(iconBg != nullptr);
    DOCTEST_REQUIRE(arrow != nullptr);
    // 图标：右上 (卡宽−56, 16) 40×40（§4.4）
    DOCTEST_CHECK(std::fabs(iconBg->frame.x -
                            (card->frame.x + cardW - mcpDesign::kRightTrackW)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(iconBg->frame.y - (card->frame.y + mcpDesign::kTextX)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(iconBg->frame.width - mcpDesign::kIconSize) <
                  mcpDesign::kEps);
    // hover 箭头：右下 (卡宽−32, 64) 16×16（§4.4）
    DOCTEST_CHECK(std::fabs(arrow->frame.x -
                            (card->frame.x + cardW - mcpDesign::kArrowSize -
                             mcpDesign::kTextX)) < mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(arrow->frame.y - (card->frame.y + mcpDesign::kDescY)) <
                  mcpDesign::kEps);
    DOCTEST_CHECK(std::fabs(arrow->frame.width - mcpDesign::kArrowSize) <
                  mcpDesign::kEps);
    // 旧文字 CTA（.cta.label「查看详情」）废除 = 右侧零文本的结构证据
    DOCTEST_CHECK(ui.find("mcp.card.test_alpha_tool.cta.label") == nullptr);
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

// bug180：弹窗描述区是固定高（y=88 h=48）+ wrap 多行盒。长描述折行溢出若不按
// 节点框 clip，墨迹越过 88..136 盒直铺，压到 148 的「参数 schema」标题/框——
// 「固定高度/绝对坐标 + 内容自增溢出」族（同 bug145 schema.text 的不变量：
// 折行溢出不越框压后续区）。节点帧不随溢出变化，可检验代理 = desc 节点 clip
// 置位；用 6 行长描述构造溢出态。
REGISTER_TEST(mcp_page_dialog_desc_clip_overflow,
              "mcp_page: 弹窗长描述折行溢出按节点框 clip（bug180），不越 desc 盒压参数 schema 区", {
    core::mcp::McpToolRegistry reg;
    const std::string longDesc =
        "这是一个用于回归的超长工具描述：第一行内容足够长会折行；"
        "第二行继续铺满整行宽度迫使换行；第三行仍在增长，"
        "用来把墨迹推出 48 高的固定盒；第四行、第五行、第六行继续加码，"
        "确保溢出量远超一行，足以压到下方的参数 schema 展示区。";
    auto fat = std::make_shared<FakeTool>("test_fat_desc_tool", longDesc,
                                          R"({"type":"object","properties":{}})");
    (void)reg.registerTool(fat);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_desc_1");
    const auto* hit = ui.find("mcp.card.test_fat_desc_tool.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    hit->onClick();
    composeFrame(mod, ui, screen, "mcp_desc_2");

    const auto* desc = ui.find("mcp.dialog.detail.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK_EQ(desc->text, longDesc);
    // 红证锚点：修前 desc 无 clip（溢出墨迹直铺）；修后必须按节点框 clip。
    DOCTEST_CHECK(desc->clip);
    // 几何不随内容漂移：desc 盒固定，下方 schema 标题不与其重叠。
    const auto* schemaTitle = ui.find("mcp.dialog.detail.schema.title");
    DOCTEST_REQUIRE(schemaTitle != nullptr);
    DOCTEST_CHECK((schemaTitle->frame.y >= desc->frame.y + desc->frame.height));
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

// bug216 回归：参数行 label 与输入框必须同轴。全站表单行口径（ui_kit/form_row.cpp）
// = label 盒高=行高、墨迹 verticalAlign Center 垂直居中；旧实现用 lineHeight 顶置
// 凑数，墨迹落点与 components::input 的盒内居中不一致（老板真机实测错位）。
REGISTER_TEST(mcp_page_param_row_label_centered_like_formrow,
              "mcp_page: 参数行 label 走 formRow 同款墨迹垂直居中（bug216）", {
    core::mcp::McpToolRegistry reg;
    auto tool = std::make_shared<FakeTool>(
        "test_axis_tool", "对齐用工具",
        R"({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})");
    (void)reg.registerTool(tool);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_axis_1");
    ui.find("mcp.card.test_axis_tool.hit")->onClick();
    composeFrame(mod, ui, screen, "mcp_axis_2");

    const auto* label = ui.find("mcp.dialog.detail.param.url.label");
    const auto* input = ui.find("mcp.dialog.detail.param.url.input");
    DOCTEST_REQUIRE_NE(label, nullptr);
    DOCTEST_REQUIRE_NE(input, nullptr);
    // DSL 契约：label 与 form_row.cpp 同款——墨迹 Center 垂直居中
    DOCTEST_CHECK_EQ(label->verticalAlign, eui::VerticalAlign::Center);
    // 盒级同轴：label 行盒中心与 input 盒中心同落行中（30/2 与 +2+26/2）
    const float labelCenterY = label->frame.y + label->frame.height * 0.5f;
    const float inputCenterY = input->frame.y + input->frame.height * 0.5f;
    DOCTEST_CHECK(std::fabs(labelCenterY - inputCenterY) < 0.5f);
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

// 防：hover 箭头恒显或恒隐（need343：.cta 已从文字 CTA 收敛为纯图标箭头，
// onHover 通道失效会退回恒显/恒隐）。
REGISTER_TEST(mcp_page_hover_cta,
              "mcp_page: 卡片 hover → 右下箭头浮现（opacity 1）；移出 → 隐藏（opacity 0）", {
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

// ── need244：「安装到 Claude Code」按钮 ──
// 防的事故：三钮重叠遮挡、服务未运行仍可点安装（写出连不上的地址）、
// 没有 ~/.claude.json 时凭空创建文件、装/卸后按钮态不刷新。
namespace {

// need244 点击链夹具：贴近真实 ~/.claude.json 形态（自身状态 + 既有 server，无 devdesk）
const char* kCcClickFixture = R"({
  "numStartups": 183,
  "machineID": "abc123",
  "installMethod": "npm",
  "userID": "u-demo",
  "firstStartTime": "2026-01-02T03:04:05.000Z",
  "hasCompletedOnboarding": true,
  "projects": {"/tmp/a": {"allowedTools": ["Read", "Write"], "history": [{"display": "hi"}]}},
  "oauthAccount": {"emailAddress": "demo@example.com", "organizationName": "demo"},
  "customApiKeyResponses": {"approved": [], "rejected": []},
  "tipsHistory": {"new-user-warmup": 1},
  "autoUpdates": false,
  "mcpServers": {
    "codegraph": {"type": "stdio", "command": "codegraph", "args": ["serve"]},
    "rea": {"type": "http", "url": "http://127.0.0.1:7777/mcp"},
    "fetch": {"type": "stdio", "command": "fetch"}
  }
})";

// 隔离用户真实 ~/.claude.json：DEVDESK_CLAUDE_CONFIG_DIR 覆盖到临时目录，
// 目录里按需放/不放 .claude.json（paths::claudeConfigFile 的覆盖契约）
struct CcConfigGuard {
    explicit CcConfigGuard(bool createFile) {
        dir_ = test_temp::tempPath("cc_ui_cfg");
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", dir_.string());
        if (createFile) {
            std::ofstream f(dir_ / ".claude.json", std::ios::binary | std::ios::trunc);
            f << R"({"numStartups": 7, "mcpServers": {"codegraph": {"type": "stdio"}}})";
        }
    }
    ~CcConfigGuard() {
        test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    CcConfigGuard(const CcConfigGuard&) = delete;
    CcConfigGuard& operator=(const CcConfigGuard&) = delete;
    std::filesystem::path configFile() const { return dir_ / ".claude.json"; }
    std::filesystem::path dir_;
};

// need244 测试辅助：JSON 解析成对象（失败返回空对象，由断言处判红）与键拼接打印
platform::json::JsonObject pjParse(const std::string& text) {
    const auto parsed = platform::json::parse(text);
    if (parsed.isFail() || !parsed.value().isObject()) return platform::json::JsonObject();
    return parsed.value().asObject();
}

std::string joinKeys(const std::vector<std::string>& keys) {
    std::string out = "[";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out += ",";
        out += keys[i];
    }
    return out + "]";
}

// components::button 的文案节点：有 hover/pressed 逐态字时为 <id>.text.normal，
// 否则为 <id>.text（见 EUI-NEO/components/button.h 的 hasText 两分支）
const core::dsl::Element* buttonLabel(const eui::Ui& ui, const std::string& id) {
    if (const auto* n = ui.find(id + ".text.normal"); n != nullptr) return n;
    return ui.find(id + ".text");
}

// 打开某工具详情弹窗并回一帧
void openDetailOf(McpPageModule& mod, eui::Ui& ui, const eui::Screen& screen,
                  const char* toolName, const char* frame) {
    composeFrame(mod, ui, screen, frame);
    ui.find(std::string("mcp.card.") + toolName + ".hit")->onClick();
    composeFrame(mod, ui, screen, (std::string(frame) + "_d").c_str());
}

} // namespace

// 设计真值源（need394 设计 §3.1/§3.3）：页头宿主安装区行盒与右锛钮尺寸。
// 断言引用常量名不写布位魔数（布位迁移不破断言）。
namespace hostDesign {
constexpr float kRowH = 36.0f;
constexpr float kRowGap = 8.0f;
constexpr float kBtnW = 160.0f;
constexpr float kBtnH = 32.0f;
constexpr float kCardH = 248.0f;  // §3.1（含底距 24）
constexpr float kHeadPad = 24.0f;
}  // namespace hostDesign

// need394 F-3：安装/卸载入口外置页头固定区——两宿主行镜像（Claude 上 / ZCode
// 下，行距 8），右锛钮 160×32 右缘对齐卡内边距，页头卡高 248
REGISTER_TEST(mcp_page_host_rows_geometry,
              "mcp_page: need394 页头两宿主行镜像——钮 160×32 右锛对齐，行序 Claude 上/ZCode 下，卡高 248", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    BridgeGuard bridge;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8888;
    CcConfigGuard cfg(true);
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_host_geo");

    const auto* claude = ui.find("mcp.head.host.claude.btn.bg");
    const auto* zcode = ui.find("mcp.head.host.zcode.btn.bg");
    DOCTEST_REQUIRE(claude != nullptr);
    DOCTEST_REQUIRE(zcode != nullptr);
    DOCTEST_CHECK_EQ(claude->frame.width, hostDesign::kBtnW);
    DOCTEST_CHECK_EQ(claude->frame.height, hostDesign::kBtnH);
    DOCTEST_CHECK_EQ(zcode->frame.width, hostDesign::kBtnW);
    // 两宿主互为镜像：钮右缘同一垂线（右锛 = 卡右 − 24）
    const float claudeRight = claude->frame.x + claude->frame.width;
    const float zcodeRight = zcode->frame.x + zcode->frame.width;
    DOCTEST_CHECK_EQ(claudeRight, zcodeRight);
    // 行序与行距：Claude 行在 ZCode 行正上方，差值 = 行高 + 行距
    DOCTEST_CHECK_EQ(zcode->frame.y - claude->frame.y,
                     hostDesign::kRowH + hostDesign::kRowGap);
    // 钉位锛链齐备（dot / 宿主名 / 状态词）+ 标签行/分隔线
    DOCTEST_CHECK_NE(ui.find("mcp.head.host.claude.dot"), nullptr);
    const auto* claudeName = ui.find("mcp.head.host.claude.name");
    const auto* zcodeName = ui.find("mcp.head.host.zcode.name");
    DOCTEST_REQUIRE(claudeName != nullptr);
    DOCTEST_REQUIRE(zcodeName != nullptr);
    DOCTEST_CHECK_EQ(claudeName->text, std::string("Claude Code"));
    DOCTEST_CHECK_EQ(zcodeName->text, std::string("ZCode"));
    DOCTEST_CHECK_NE(ui.find("mcp.head.host.claude.state"), nullptr);
    DOCTEST_CHECK_NE(ui.find("mcp.head.host.zcode.state"), nullptr);
    DOCTEST_CHECK_NE(ui.find("mcp.head.host.label"), nullptr);
    DOCTEST_CHECK_NE(ui.find("mcp.head.divider"), nullptr);
    // 页头卡高 248（宿主区进固定区，卡随内容长高）
    const auto* head = ui.find("mcp.head");
    DOCTEST_REQUIRE(head != nullptr);
    DOCTEST_CHECK_EQ(head->frame.height, hostDesign::kCardH);
    // 钮在卡内（右缘不越卡内边距 24）
    DOCTEST_CHECK(claudeRight <= head->frame.x + head->frame.width -
                                       hostDesign::kHeadPad + 0.5f);
    return true;
})

// need394 F-3：未装 + 服务运行 → 页头「安装到 Claude Code」可点；点击写盘后立刻
// 翻卸载文案 + 待重启 hint，再点恢复（入口已从弹窗迁到页头固定区）
REGISTER_TEST(mcp_page_host_install_then_uninstall,
              "mcp_page: need394 页头未装=安装文案；点击写盘后立刻变卸载文案，再点恢复", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    BridgeGuard bridge;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8888;
    CcConfigGuard cfg(true);
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_cc_inst");

    const auto* label = buttonLabel(ui, "mcp.head.host.claude.btn");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("安装到 Claude Code"));
    DOCTEST_CHECK_FALSE(ui.find("mcp.head.host.claude.btn.bg")->disabled);

    ui.find("mcp.head.host.claude.btn.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_cc_inst_2");
    const auto* label2 = buttonLabel(ui, "mcp.head.host.claude.btn");
    DOCTEST_REQUIRE(label2 != nullptr);
    DOCTEST_CHECK_EQ(label2->text, std::string("从 Claude Code 卸载"));
    // 落盘内容：devdesk 条目 = http + 当前端口，其余字段保留
    const std::string written = core::readTextFile(cfg.configFile());
    DOCTEST_CHECK(written.find("\"devdesk\"") != std::string::npos);
    DOCTEST_CHECK(written.find("http://127.0.0.1:8888/mcp") != std::string::npos);
    DOCTEST_CHECK(written.find("\"codegraph\"") != std::string::npos);
    DOCTEST_CHECK(written.find("\"numStartups\"") != std::string::npos);
    // 审计 P1-4：安装成功就地提示「需重启 Claude Code 生效」（写入 ≠ 立即生效）
    {
        const auto* hint = ui.find("mcp.head.host.claude.hint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("重启 Claude Code") != std::string::npos);
    }

    ui.find("mcp.head.host.claude.btn.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_cc_inst_3");
    DOCTEST_CHECK_EQ(buttonLabel(ui, "mcp.head.host.claude.btn")->text,
                     std::string("安装到 Claude Code"));
    const std::string after = core::readTextFile(cfg.configFile());
    DOCTEST_CHECK(after.find("\"devdesk\"") == std::string::npos);
    DOCTEST_CHECK(after.find("\"codegraph\"") != std::string::npos);
    // 卸载成功后「待重启」提示清空（hint 槽不再显示过期提示）
    DOCTEST_CHECK(ui.find("mcp.head.host.claude.hint") == nullptr);
    return true;
})

// 服务未运行 → 置灰 + 提示；没有 ~/.claude.json → 置灰 + 未检测到，且不创建文件
REGISTER_TEST(mcp_page_host_btn_gating,
              "mcp_page: need394 服务未运行/无 .claude.json → 页头钮置灰并给提示，不建文件", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    const eui::Screen screen = makeScreen();

    {  // 服务未运行（有文件）
        McpPageModule mod(reg);
        BridgeGuard bridge;
        g_bridge = BridgeState{};  // running=false
        CcConfigGuard cfg(true);
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_cc_gate_1");
        DOCTEST_CHECK(ui.find("mcp.head.host.claude.btn.bg")->disabled);
        const auto* hint = ui.find("mcp.head.host.claude.hint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("请先启动 MCP 服务") != std::string::npos);
        // 两宿主各有一行提示槽（文案随宿主文件存在性而变化，只验在场）
        DOCTEST_CHECK_NE(ui.find("mcp.head.host.zcode.hint"), nullptr);
    }
    {  // 没有 .claude.json
        McpPageModule mod(reg);
        BridgeGuard bridge;
        g_bridge = BridgeState{};
        g_bridge.running = true;
        g_bridge.port = 8888;
        CcConfigGuard cfg(false);
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_cc_gate_2");
        DOCTEST_CHECK(ui.find("mcp.head.host.claude.btn.bg")->disabled);
        const auto* hint = ui.find("mcp.head.host.claude.hint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("未检测到 Claude Code") != std::string::npos);
        // 禁自动创建
        DOCTEST_CHECK_FALSE(std::filesystem::exists(cfg.configFile()));
    }
    return true;
})

// 点击链 × 真实 105KB 配置副本（need244 实测证据）：把用户真实 ~/.claude.json
// 整份复制进隔离目录后走**真按钮点击**（onClick → toggleClaudeInstall →
// installServerFile/removeServerFile），断言安装/卸载两侧内容。不碰真文件。
REGISTER_TEST(mcp_page_claude_click_on_real_config_copy,
              "mcp_page: need244 真点击安装/卸载（自洽夹具副本），其余顶层键与既有 server 全保", {
    // 2026-11-25 改为自洽夹具（kCcClickFixture）：本机真实 ~/.claude.json 已被
    // 运行中的 App 装上 devdesk（8931），页头初始态变「从 Claude Code 卸载」、真点击
    // 走卸载而非安装（环境漂移，非实现回归）。夹具保留原防回归判据：安装只加
    // devdesk，其余顶层键/既有 server 全保；卸载回到原状。
    const std::string original = kCcClickFixture;

    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    BridgeGuard bridge;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8888;
    CcConfigGuard cfg(false);
    {  // 真文件副本入隔离目录
        std::ofstream f(cfg.configFile(), std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(static_cast<bool>(f));
        f << original;
    }
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_cc_real");

    const auto* label = buttonLabel(ui, "mcp.head.host.claude.btn");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("安装到 Claude Code"));
    DOCTEST_CHECK_FALSE(ui.find("mcp.head.host.claude.btn.bg")->disabled);
    DOCTEST_CHECK(ui.find("mcp.head.host.claude.hint") == nullptr);  // 可操作态无提示

    ui.find("mcp.head.host.claude.btn.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_cc_real_2");
    const std::string installedText = core::readTextFile(cfg.configFile());
    DOCTEST_CHECK(installedText.find("\"devdesk\"") != std::string::npos);
    DOCTEST_CHECK(installedText.find("http://127.0.0.1:8888/mcp") != std::string::npos);
    DOCTEST_CHECK_EQ(buttonLabel(ui, "mcp.head.host.claude.btn")->text,
                     std::string("从 Claude Code 卸载"));
    // 其余顶层字段与既有 server 全保（真文件 51 个顶层键）
    {
        const auto before = pjParse(original);
        const auto after = pjParse(installedText);
        DOCTEST_CHECK_EQ(after.keys().size(), before.keys().size());
        for (const std::string& key : before.keys()) {
            DOCTEST_CHECK_MESSAGE(after.has(key), "安装后丢顶层键: " + key);
        }
        const auto servers = after.get("mcpServers").asObject();
        DOCTEST_CHECK(servers.has("devdesk"));
        for (const std::string& key : before.get("mcpServers").asObject().keys()) {
            DOCTEST_CHECK_MESSAGE(servers.has(key), "安装后丢既有 server: " + key);
        }
        std::printf("[click-e2e] after install: bytes=%zu topKeys=%zu servers=%zu\n",
                    installedText.size(), after.keys().size(), servers.keys().size());
    }

    ui.find("mcp.head.host.claude.btn.bg")->onClick();
    composeFrame(mod, ui, screen, "mcp_cc_real_3");
    const std::string restoredText = core::readTextFile(cfg.configFile());
    DOCTEST_CHECK(restoredText.find("\"devdesk\"") == std::string::npos);
    DOCTEST_CHECK_EQ(buttonLabel(ui, "mcp.head.host.claude.btn")->text,
                     std::string("安装到 Claude Code"));
    {
        const auto before = pjParse(original);
        const auto after = pjParse(restoredText);
        DOCTEST_CHECK_EQ(after.keys().size(), before.keys().size());
        const auto beforeServers = before.get("mcpServers").asObject();
        const auto afterServers = after.get("mcpServers").asObject();
        DOCTEST_CHECK_EQ(afterServers.keys().size(), beforeServers.keys().size());
        for (const std::string& key : beforeServers.keys()) {
            DOCTEST_CHECK_MESSAGE(afterServers.has(key), "卸载后丢既有 server: " + key);
        }
        std::printf("[click-e2e] after remove: bytes=%zu topKeys=%zu servers=%s\n",
                    restoredText.size(), after.keys().size(),
                    joinKeys(afterServers.keys()).c_str());
    }
    return true;
})

// need394 F-3/D-2：外置=迁移非复制——详情弹窗不再绘制任何安装/卸载/提示槽元素，
// 弹窗从 516 收缩到 480，结果区回到 86 高，操作行只剩单行 [关闭][试一下]
REGISTER_TEST(mcp_page_dialog_no_install_rows,
              "mcp_page: need394 弹窗 600×480——无安装行/提示槽，结果区 86 高，操作行单行", {
    core::mcp::McpToolRegistry reg;
    std::shared_ptr<FakeTool> alpha, beta;
    seedTwoTools(reg, alpha, beta);
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    BridgeGuard bridge;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8888;
    CcConfigGuard cfg(true);
    eui::Ui ui;
    openDetailOf(mod, ui, screen, "test_alpha_tool", "mcp_dlg_nofix");

    const auto* panel = ui.find("mcp.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_EQ(panel->frame.width, 600.0f);
    DOCTEST_CHECK_EQ(panel->frame.height, 480.0f);  // D-2：516→480
    // 安装行/hint 槽不做任何绘制（迁移非复制）
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.btn.claude.bg") == nullptr);
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.btn.zcode.bg") == nullptr);
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.claude.hint") == nullptr);
    DOCTEST_CHECK(ui.find("mcp.dialog.detail.zcode.hint") == nullptr);
    // 结果区：盒高 94（480−318−52−16），滚动区高 86（480−326−52−16）
    const auto* resultBox = ui.find("mcp.dialog.detail.result.box");
    DOCTEST_REQUIRE(resultBox != nullptr);
    DOCTEST_CHECK_EQ(resultBox->frame.height, 94.0f);
    const auto* scroll = ui.find("mcp.dialog.detail.result.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK_EQ(scroll->frame.height, 86.0f);
    // 操作行：关闭与试一下同行（h−52），右缘贴弹窗内边距 20
    const auto* close = ui.find("mcp.dialog.detail.btn.close.bg");
    const auto* tryBtn = ui.find("mcp.dialog.detail.btn.try.bg");
    DOCTEST_REQUIRE(close != nullptr);
    DOCTEST_REQUIRE(tryBtn != nullptr);
    DOCTEST_CHECK_EQ(close->frame.y, tryBtn->frame.y);
    DOCTEST_CHECK(close->frame.x + close->frame.width <= tryBtn->frame.x);
    DOCTEST_CHECK(tryBtn->frame.x + tryBtn->frame.width <=
                  panel->frame.x + 600.0f - 20.0f + 0.5f);
    return true;
})

// need394 F-4：卡片图标族新增 todo_*/md_* 两族（设计 §4.1 具体 FA 码点）——
// 名族匹配顺序 todo → md_ → memory → fetch/web → server/info → cube 兜底
REGISTER_TEST(mcp_page_tool_icon_family,
              "mcp_page: need394 图标族——todo_*=list-check、md_*=file-lines、memory=brain、fetch/web=globe、server/info=server", {
    core::mcp::McpToolRegistry reg;
    struct Entry {
        const char* name;
        unsigned int glyph;
    };
    const Entry entries[] = {
        {"todo_list", 0xF0AE},          // fa-list-check
        {"todo_complete", 0xF0AE},
        {"md_search", 0xF15C},          // fa-file-lines
        {"md_list_tags", 0xF15C},
        {"memory_search", 0xF5DC},      // fa-brain
        {"web_fetch_markdown", 0xF0AC},  // fa-globe
        {"devdesk_server_info", 0xF233},  // fa-server
        {"mystery_tool", 0xF1B2},        // fa-cube 兜底
    };
    for (const auto& e : entries) {
        (void)reg.registerTool(std::make_shared<FakeTool>(
            std::string(e.name), "x", R"({"type":"object","properties":{}})"));
    }
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    BridgeGuard bridge;
    g_bridge = BridgeState{};
    g_bridge.running = true;
    g_bridge.port = 8888;
    CcConfigGuard cfg(true);
    eui::Ui ui;
    composeFrame(mod, ui, screen, "mcp_icon_family");
    for (const auto& e : entries) {
        const auto* icon =
            ui.find(std::string("mcp.card.") + e.name + ".icon");
        DOCTEST_REQUIRE_MESSAGE(icon != nullptr,
                                std::string("缺图标: ") + e.name);
        DOCTEST_CHECK_MESSAGE(icon->text == eui::utf8(e.glyph),
                              std::string("码点不符: ") + e.name);
    }
    return true;
})
