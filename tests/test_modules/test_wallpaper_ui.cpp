// tests/test_modules/test_wallpaper_ui.cpp
// 壁纸页 UI 无头契约：播放/片库两页、来源与 Tab、四类状态、空态三变体、折叠组、
// 间隔禁用、图片专属卡、片库统计/扫描/目录缺失、1000×640 首屏无滚动条与几何数值。
//
// 每条用例注明防的事故；REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
// 跨文件前提（节点 id 以 wallpaper_ui.cpp 文件头为准）：
//   - 页面根 wallpaper.page（左右 padding 32 / 底部 32 / 区块间距 16）。
//   - 卡片实色 cardFrame：1px borderDefault + 圆角 12（t.sizes.radiusLarge）。
//   - 文案只来自快照/presentation：本文件用「快照哨兵字符串」证明 UI 不回拼状态文案。
#include "doctest.h"
#include "test_support.h"

#include "core/render/text.h"
#include "core/theme.h"
#include "module_wallpaper/core/model.h"
#include "module_wallpaper/core/presentation.h"
#include "module_wallpaper/core/rotation.h"
#include "module_wallpaper/wallpaper_service.h"
#include "module_wallpaper/wallpaper_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <eui/dsl.h>

namespace {

namespace wp = module::wallpaper;

using ::core::theme::Tokens;

// 真实页区：窗口 1600×1000 / 1000×640 减侧栏 208、自绘标题条 40（与
// test_settings.cpp 的 formflex 双档同口径）。
constexpr eui::Screen kPage1000x640{792.0f, 600.0f};
constexpr eui::Screen kPage1600x1000{1392.0f, 960.0f};

wp::WallpaperSnapshot makeSnapshot(wp::WallpaperKind kind,
                                   wp::Availability availability,
                                   wp::PlayMode mode = wp::PlayMode::LoopList,
                                   bool manualPaused = false,
                                   wp::HoldReasonSet systemHolds = 0) {
    wp::WallpaperSnapshot s;
    s.kind = kind;
    s.availability = availability;
    s.folderPath = availability == wp::Availability::FolderUnconfigured
                       ? std::string()
                       : (kind == wp::WallpaperKind::Image
                              ? std::string("D:\\Pictures\\Wallpapers")
                              : std::string("D:\\Wallpapers\\动态壁纸"));
    s.scanning = false;
    s.videoCount = 12;
    s.imageTotal = 24;
    s.imagePassing = 18;
    s.imageFiltered = 6;
    s.mode = mode;
    s.rotationIntervalSec = 600;
    s.manualPaused = manualPaused;
    s.systemHolds = systemHolds;
    s.paused = manualPaused || systemHolds != 0;
    s.rate = 1.0f;
    s.volume = 0.82f;
    s.muted = false;
    s.pauseOnBattery = false;
    s.imageMinPixels = 3686400;
    s.imageFit = wp::ImageFit::Fill;
    s.secondsUntilRotation =
        availability == wp::Availability::Playing ? 272 : -1;
    s.rotationProgress = 0.45f;

    wp::presentation::StatusInput in;
    in.kind = kind;
    in.availability = availability;
    in.mode = mode;
    in.paused = s.paused;
    in.manualPaused = s.manualPaused;
    in.systemHolds = s.systemHolds;
    in.itemCount = kind == wp::WallpaperKind::Video ? s.videoCount
                                                    : s.imagePassing;
    in.rotationIntervalSec = s.rotationIntervalSec;
    s.modeLabel = wp::presentation::playModeLabel(mode, kind);
    s.statusTitle = wp::presentation::statusTitle(in);
    s.statusSubtitle = wp::presentation::statusSubtitle(in);
    s.countdownLabel = wp::presentation::countdownLabel(kind, mode, s.paused,
                                                        s.secondsUntilRotation);
    return s;
}

void composeFrame(eui::Ui& ui, const eui::Screen& screen,
                  const wp::WallpaperSnapshot& snapshot,
                  const wp::UiActions& actions, wp::UiState& state) {
    ui.begin("wallpaper_ui_test");
    wp::composeWallpaperUi(ui, screen, snapshot, actions, state);
    ui.end();
    ui.layout(screen.width, screen.height);
}

bool fireClick(eui::Ui& ui, const std::string& id) {
    auto* element = ui.find(id);
    if (element == nullptr || !element->onClick) return false;
    element->onClick();
    return true;
}

std::string elementText(eui::Ui& ui, const std::string& id) {
    auto* element = ui.find(id);
    return element == nullptr ? std::string() : element->text;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool sameColor(const eui::Color& a, const eui::Color& b) {
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f &&
           std::fabs(a.b - b.b) < 0.002f;
}

float rightOf(const core::dsl::Element* element) {
    return element == nullptr ? 0.0f
                              : element->frame.x + element->frame.width;
}

// 全树遍历：用于「唯一来源分段」「不存在禁用入口」「所有元素不越界」这类整树断言。
void walkElements(const core::dsl::Element& element,
                  const std::function<void(const core::dsl::Element&)>& fn) {
    fn(element);
    for (const auto& child : element.children) {
        walkElements(*child, fn);
    }
}

void walkTree(const eui::Ui& ui,
              const std::function<void(const core::dsl::Element&)>& fn) {
    for (const auto& root : ui.roots()) {
        walkElements(*root, fn);
    }
}

// composeFrame 以 begin("wallpaper_ui_test") 组帧：element.id 存的是 resolveId
// 后的页面级全名，而 Ui::normalizeScopeId 为私有——这里等价剥页前缀还原模块内
// id，供整树计数 / 前缀豁免类断言使用。
std::string scopedId(const core::dsl::Element& element) {
    const std::string kPagePrefix = "wallpaper_ui_test.";
    if (element.id.rfind(kPagePrefix, 0) == 0) {
        return element.id.substr(kPagePrefix.size());
    }
    return element.id;
}

int countExactId(const eui::Ui& ui, const std::string& id) {
    int count = 0;
    walkTree(ui, [&](const core::dsl::Element& element) {
        if (scopedId(element) == id) ++count;
    });
    return count;
}

} // namespace

// 防：改动布局时把页面左右 padding / 卡内 padding / 卡距 / 圆角 / 字号改回近似值，
// 或让 1600×1000 首屏出现滚动条。
REGISTER_TEST(wallpaper_ui_geometry_1600,
              "wallpaper_ui: 1600×1000 页区几何逐值（padding 32/20、卡距 16、圆角 12、字号 24/15/14/12、按钮 40）", {
    const Tokens& t = ::core::theme::current();
    eui::Ui ui;
    wp::UiState state;
    const wp::WallpaperSnapshot snapshot =
        makeSnapshot(wp::WallpaperKind::Video, wp::Availability::Playing,
                     wp::PlayMode::LoopList);
    composeFrame(ui, kPage1600x1000, snapshot, {}, state);

    const auto* page = ui.find("wallpaper.page");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_CHECK_EQ(page->frame.width, 1392.0f);
    DOCTEST_CHECK_EQ(page->frame.height, 960.0f);

    const auto* title = ui.find("wallpaper.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("壁纸"));
    DOCTEST_CHECK_EQ(title->fontSize, 24.0f);
    DOCTEST_CHECK_EQ(title->fontWeight, 700);
    DOCTEST_CHECK_EQ(title->lineHeight, 40.0f);

    const auto* header = ui.find("wallpaper.header");
    const auto* tabs = ui.find("wallpaper.tabs");
    const auto* scroll = ui.find("wallpaper.scroll");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_REQUIRE(tabs != nullptr);
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK_EQ(header->frame.height, 40.0f);
    DOCTEST_CHECK_EQ(tabs->frame.height, 40.0f);
    DOCTEST_CHECK_EQ(tabs->frame.y, 56.0f);          // 页头 40 + 间距 16
    DOCTEST_CHECK_EQ(scroll->frame.y, 112.0f);       // + Tab 40 + 间距 16
    DOCTEST_CHECK_EQ(scroll->frame.x, 32.0f);
    DOCTEST_CHECK_EQ(scroll->frame.width, 1328.0f);
    DOCTEST_CHECK_EQ(scroll->frame.height, 816.0f);  // 960 - 32 - 40 - 16 - 40 - 16
    DOCTEST_CHECK(ui.find("wallpaper.scroll.scroll") == nullptr);

    const auto* card = ui.find("wallpaper.card.status");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_CHECK_EQ(card->frame.x, 32.0f);
    DOCTEST_CHECK_EQ(card->frame.width, 1328.0f);
    DOCTEST_CHECK_EQ(rightOf(card), 1360.0f);        // 1392 - 32
    DOCTEST_CHECK_EQ(card->frame.height, 104.0f);

    const auto* cardBg = ui.find("wallpaper.card.status.bg");
    DOCTEST_REQUIRE(cardBg != nullptr);
    DOCTEST_CHECK_EQ(cardBg->radius, t.sizes.radiusLarge);
    DOCTEST_CHECK_EQ(cardBg->border.width, 1.0f);
    DOCTEST_CHECK(sameColor(cardBg->color, t.colors.surface));
    DOCTEST_CHECK(cardBg->shadow.enabled);

    const auto* dot = ui.find("wallpaper.status.dot");
    DOCTEST_REQUIRE(dot != nullptr);
    DOCTEST_CHECK_EQ(dot->frame.x - card->frame.x, 20.0f);  // 卡内 padding 20
    DOCTEST_CHECK_EQ(dot->frame.width, 8.0f);
    DOCTEST_CHECK(sameColor(dot->color, t.colors.success));

    const auto* statusTitle = ui.find("wallpaper.status.title");
    DOCTEST_REQUIRE(statusTitle != nullptr);
    DOCTEST_CHECK_EQ(statusTitle->text, snapshot.statusTitle);
    DOCTEST_CHECK_EQ(statusTitle->fontSize, 15.0f);
    DOCTEST_CHECK_EQ(statusTitle->fontWeight, 600);
    const auto* subtitle = ui.find("wallpaper.status.subtitle");
    DOCTEST_REQUIRE(subtitle != nullptr);
    DOCTEST_CHECK_EQ(subtitle->text, snapshot.statusSubtitle);
    DOCTEST_CHECK_EQ(subtitle->fontSize, 14.0f);

    const auto* modeCard = ui.find("wallpaper.card.mode");
    const auto* intervalCard = ui.find("wallpaper.card.interval");
    DOCTEST_REQUIRE(modeCard != nullptr);
    DOCTEST_REQUIRE(intervalCard != nullptr);
    DOCTEST_CHECK_EQ(modeCard->frame.y - (card->frame.y + card->frame.height),
                     16.0f);
    DOCTEST_CHECK_EQ(modeCard->frame.width, 656.0f);   // (1328 - 16) / 2
    DOCTEST_CHECK_EQ(intervalCard->frame.x - rightOf(modeCard), 16.0f);
    DOCTEST_CHECK_EQ(rightOf(intervalCard), 1360.0f);
    DOCTEST_CHECK_EQ(modeCard->frame.height, 148.0f);

    const auto* modeTitle = ui.find("wallpaper.card.mode.title");
    DOCTEST_REQUIRE(modeTitle != nullptr);
    DOCTEST_CHECK_EQ(modeTitle->fontSize, 15.0f);
    DOCTEST_CHECK_EQ(modeTitle->fontWeight, 600);
    const auto* modeHint = ui.find("wallpaper.card.mode.hint");
    DOCTEST_REQUIRE(modeHint != nullptr);
    DOCTEST_CHECK_EQ(modeHint->fontSize, 12.0f);

    const auto* pause = ui.find("wallpaper.status.pause");
    const auto* next = ui.find("wallpaper.status.next");
    DOCTEST_REQUIRE(pause != nullptr);
    DOCTEST_REQUIRE(next != nullptr);
    DOCTEST_CHECK_EQ(pause->frame.height, 40.0f);  // ui_kit::kButtonHeight
    DOCTEST_CHECK_EQ(next->frame.height, 40.0f);

    const auto* effects = ui.find("wallpaper.card.speed");
    const auto* holds = ui.find("wallpaper.card.holds");
    DOCTEST_REQUIRE(effects != nullptr);
    DOCTEST_REQUIRE(holds != nullptr);
    DOCTEST_CHECK_EQ(effects->frame.height, 56.0f);  // 默认收起
    DOCTEST_CHECK_EQ(holds->frame.height, 56.0f);
    DOCTEST_CHECK_EQ(effects->frame.y -
                         (modeCard->frame.y + modeCard->frame.height),
                     16.0f);
    DOCTEST_CHECK_EQ(holds->frame.y -
                         (effects->frame.y + effects->frame.height),
                     16.0f);
    return true;
})

// 防：最小窗（页区 792×600）首屏出现纵向滚动条（需求 §7.5），或卡片越出视口。
REGISTER_TEST(wallpaper_ui_min_window_no_scroll,
              "wallpaper_ui: 1000×640 页区 792×600 首屏零纵向滚动条（视频/图片、播放/片库/空态）", {
    struct Scenario {
        const char* name;
        wp::WallpaperKind kind;
        wp::Availability availability;
        wp::PlayMode mode;
        int tab;
    };
    const Scenario scenarios[] = {
        {"video-play", wp::WallpaperKind::Video, wp::Availability::Playing,
         wp::PlayMode::LoopSingle, 0},
        {"video-library", wp::WallpaperKind::Video, wp::Availability::Playing,
         wp::PlayMode::LoopList, 1},
        {"image-play", wp::WallpaperKind::Image, wp::Availability::Playing,
         wp::PlayMode::LoopList, 0},
        {"image-library", wp::WallpaperKind::Image, wp::Availability::Playing,
         wp::PlayMode::LoopList, 1},
        {"video-empty-play", wp::WallpaperKind::Video,
         wp::Availability::FolderUnconfigured, wp::PlayMode::LoopSingle, 0},
        {"video-empty-library", wp::WallpaperKind::Video,
         wp::Availability::FolderMissing, wp::PlayMode::LoopSingle, 1},
    };
    for (const Scenario& scenario : scenarios) {
        eui::Ui ui;
        wp::UiState state;
        state.tab = scenario.tab;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            scenario.kind, scenario.availability, scenario.mode);
        composeFrame(ui, kPage1000x640, snapshot, {}, state);
        DOCTEST_CHECK_MESSAGE(ui.find("wallpaper.scroll") != nullptr,
                              std::string(scenario.name) +
                                  " 缺内容滚动视口");
        DOCTEST_CHECK_MESSAGE(ui.find("wallpaper.scroll.scroll") == nullptr,
                              std::string(scenario.name) +
                                  " 首屏不应出现纵向滚动条");
        // 所有可见卡片必须落在视口内（滚动视口顶 112、高 456）。
        const auto* scroll = ui.find("wallpaper.scroll");
        DOCTEST_REQUIRE(scroll != nullptr);
        const float viewportBottom = scroll->frame.y + scroll->frame.height;
        std::vector<std::string> cardIds;
        walkTree(ui, [&](const core::dsl::Element& element) {
            const std::string id = scopedId(element);
            if (id.rfind("wallpaper.card.", 0) == 0 &&
                id.find(".bg") == std::string::npos &&
                id.find(".title") == std::string::npos &&
                id.find(".hint") == std::string::npos) {
                cardIds.push_back(id);
            }
        });
        for (const std::string& id : cardIds) {
            const auto* element = ui.find(id);
            DOCTEST_REQUIRE(element != nullptr);
            DOCTEST_CHECK_MESSAGE(element->frame.x >= 0.5f,
                                  std::string(scenario.name) + " " + id +
                                      " 左缘越界");
            DOCTEST_CHECK_MESSAGE(rightOf(element) <= 792.0f + 0.5f,
                                  std::string(scenario.name) + " " + id +
                                      " 右缘越界");
            DOCTEST_CHECK_MESSAGE(element->frame.y >= -0.5f &&
                                      element->frame.y + element->frame.height <=
                                          viewportBottom + 0.5f,
                                  std::string(scenario.name) + " " + id +
                                      " 纵向越界（视口底 " +
                                      std::to_string(viewportBottom) + "）");
        }
    }
    return true;
})

// 防：来源开关在片库重复出现（§7.4 明确「来源切换不在卡片里重复」）；切换来源后
// 页面不立刻换布局（草稿状态失效），以及 Tab 在两种状态下消失。
REGISTER_TEST(wallpaper_ui_sources_and_tabs,
              "wallpaper_ui: 来源分段只在页头且切换当帧换布局；播放/片库 Tab 两种来源都可见", {
    wp::UiState state;
    eui::Ui videoUi;
    const wp::WallpaperSnapshot video = makeSnapshot(
        wp::WallpaperKind::Video, wp::Availability::Playing,
        wp::PlayMode::LoopList);
    composeFrame(videoUi, kPage1000x640, video, {}, state);
    DOCTEST_CHECK_EQ(countExactId(videoUi, "wallpaper.source"), 1);
    const auto* header = videoUi.find("wallpaper.header");
    const auto* source = videoUi.find("wallpaper.source");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_REQUIRE(source != nullptr);
    DOCTEST_CHECK(source->frame.x >= header->frame.x &&
                  rightOf(source) <= rightOf(header) + 0.5f);
    const auto* sourceVideo = videoUi.find("wallpaper.source.video");
    const auto* sourceImage = videoUi.find("wallpaper.source.image");
    DOCTEST_REQUIRE(sourceVideo != nullptr);
    DOCTEST_REQUIRE(sourceImage != nullptr);
    DOCTEST_CHECK(sourceVideo->frame.x < sourceImage->frame.x);
    DOCTEST_CHECK_EQ(elementText(videoUi, "wallpaper.source.video.text"),
                     std::string("视频"));
    DOCTEST_CHECK_EQ(elementText(videoUi, "wallpaper.source.image.text"),
                     std::string("图片"));
    DOCTEST_CHECK(videoUi.find("wallpaper.tabs.0") != nullptr);
    DOCTEST_CHECK(videoUi.find("wallpaper.tabs.1") != nullptr);
    DOCTEST_CHECK_EQ(elementText(videoUi, "wallpaper.tabs.0.text"),
                     std::string("播放"));
    DOCTEST_CHECK_EQ(elementText(videoUi, "wallpaper.tabs.1.text"),
                     std::string("片库"));

    // 片库页页头与 Tab 仍可见；片库内部仅一处来源分段。
    DOCTEST_CHECK(fireClick(videoUi, "wallpaper.tabs.1.hit"));
    DOCTEST_CHECK_EQ(state.tab, 1);
    eui::Ui libraryUi;
    composeFrame(libraryUi, kPage1000x640, video, {}, state);
    DOCTEST_CHECK(libraryUi.find("wallpaper.header") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.tabs") != nullptr);
    DOCTEST_CHECK_EQ(countExactId(libraryUi, "wallpaper.source"), 1);
    DOCTEST_CHECK(libraryUi.find("wallpaper.card.status") == nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.card.folder") != nullptr);

    // 切换来源：动作被调用；同一帧（快照仍是视频）按草稿渲染成图片布局。
    wp::WallpaperKind requestedKind = wp::WallpaperKind::Video;
    wp::UiActions actions;
    actions.setKind = [&requestedKind](wp::WallpaperKind kind) {
        requestedKind = kind;
    };
    wp::UiState switchState;
    eui::Ui switchUi;
    composeFrame(switchUi, kPage1000x640, video, actions, switchState);
    DOCTEST_CHECK(fireClick(switchUi, "wallpaper.source.image"));
    DOCTEST_CHECK(requestedKind == wp::WallpaperKind::Image);
    DOCTEST_CHECK(switchState.kindDraft == wp::WallpaperKind::Image);
    // 快照仍是视频（服务下一帧才会变），UI 按草稿立即渲染图片布局。
    eui::Ui draftUi;
    composeFrame(draftUi, kPage1000x640, video, actions, switchState);
    DOCTEST_CHECK_EQ(elementText(draftUi, "wallpaper.card.mode.title"),
                     std::string("轮播方式"));
    DOCTEST_CHECK(draftUi.find("wallpaper.card.speed") == nullptr);
    return true;
})

// 防：四类状态串文案（手动暂停显示原因、系统让路漏原因、空态仍画状态卡、
// 播放/暂停的颜色与进度条条件错位）。状态标题/副题必须逐字等于快照字段。
REGISTER_TEST(wallpaper_ui_status_four_states,
              "wallpaper_ui: 四类状态（播放/手动暂停/系统让路/空态）文案严格取快照", {
    const Tokens& t = ::core::theme::current();
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.title"),
                         snapshot.statusTitle);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.subtitle"),
                         snapshot.statusSubtitle);
        DOCTEST_CHECK(ui.find("wallpaper.status.reasons") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.status.progress") != nullptr);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown"),
                         std::string("04:32"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown.label"),
                         std::string("距下次换片"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.pause.text"),
                         std::string("暂停"));
        const auto* dot = ui.find("wallpaper.status.dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(sameColor(dot->color, t.colors.success));
    }
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList, /*manualPaused=*/true);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.title"),
                         std::string("已暂停"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.subtitle"),
                         std::string("点『继续』恢复播放。"));
        DOCTEST_CHECK(ui.find("wallpaper.status.reasons") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.status.progress") == nullptr);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown"),
                         std::string("—"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown.label"),
                         std::string("等待恢复"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.pause.text"),
                         std::string("继续"));
        const auto* dot = ui.find("wallpaper.status.dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(sameColor(dot->color, t.colors.warning));
    }
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::HoldReasonSet holds =
            wp::holdReasonBit(wp::HoldReason::Fullscreen) |
            wp::holdReasonBit(wp::HoldReason::ScreenLocked);
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList, /*manualPaused=*/false, holds);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        const auto* reasons = ui.find("wallpaper.status.reasons");
        DOCTEST_REQUIRE(reasons != nullptr);
        const std::string reasonsText =
            elementText(ui, "wallpaper.status.reasons.text");
        DOCTEST_CHECK(contains(reasonsText, "检测到全屏/最大化窗口"));
        DOCTEST_CHECK(contains(reasonsText, "屏幕已锁定"));
        DOCTEST_CHECK(contains(reasonsText, "、"));
        DOCTEST_CHECK(contains(elementText(ui, "wallpaper.status.title"),
                               "已暂停 · "));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown"),
                         std::string("—"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.status.countdown.label"),
                         std::string("等待恢复"));
        DOCTEST_CHECK(ui.find("wallpaper.status.progress") == nullptr);
    }
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::FolderUnconfigured,
            wp::PlayMode::LoopSingle);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK(ui.find("wallpaper.card.status") == nullptr);
        const auto* empty = ui.find("wallpaper.card.empty");
        DOCTEST_REQUIRE(empty != nullptr);
        const auto copy =
            wp::presentation::emptyStateCopy(snapshot.availability,
                                             snapshot.kind);
        DOCTEST_REQUIRE(copy.has_value());
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.card.empty.title"),
                         copy->title);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.body"), copy->reason);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.primary.text"),
                         copy->primaryAction);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.secondary.text"),
                         copy->secondaryAction);
        DOCTEST_CHECK(ui.find("wallpaper.card.mode") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.card.interval") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.card.speed") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.card.audio") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.card.holds") != nullptr);
        const auto* primary = ui.find("wallpaper.empty.primary");
        DOCTEST_REQUIRE(primary != nullptr);
        DOCTEST_CHECK_EQ(primary->frame.height, 40.0f);
    }
    return true;
})

// 防：空态三变体文案混用（未配置/目录不存在/0 可播），或图片空态宣称已按像素档过滤。
REGISTER_TEST(wallpaper_ui_empty_variants,
              "wallpaper_ui: 空态三变体 × 视频/图片，文案逐字来自 presentation::emptyStateCopy", {
    const wp::Availability variants[] = {
        wp::Availability::FolderUnconfigured, wp::Availability::FolderMissing,
        wp::Availability::NoPlayableVideos};
    const wp::WallpaperKind kinds[] = {wp::WallpaperKind::Video,
                                       wp::WallpaperKind::Image};
    for (wp::Availability availability : variants) {
        for (wp::WallpaperKind kind : kinds) {
            wp::UiState state;
            eui::Ui ui;
            const wp::WallpaperSnapshot snapshot =
                makeSnapshot(kind, availability, wp::PlayMode::LoopSingle);
            composeFrame(ui, kPage1000x640, snapshot, {}, state);
            const auto copy =
                wp::presentation::emptyStateCopy(availability, kind);
            DOCTEST_REQUIRE(copy.has_value());
            DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.card.empty.title"),
                             copy->title);
            DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.body"),
                             copy->reason);
            DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.primary.text"),
                             copy->primaryAction);
            DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.empty.secondary.text"),
                             copy->secondaryAction);
            if (kind == wp::WallpaperKind::Image) {
                DOCTEST_CHECK(contains(
                    elementText(ui, "wallpaper.empty.support"), "jpg"));
            } else {
                DOCTEST_CHECK(contains(
                    elementText(ui, "wallpaper.empty.support"), "mp4"));
            }
        }
    }
    return true;
})

// 防：折叠组退化成单卡展开（速度与音频必须整组开合）、让路组与效果组联动、
// 默认展开、以及折叠状态被持久化（跨 UiState 重建仍收起是本条的反向锚）。
REGISTER_TEST(wallpaper_ui_fold_groups,
              "wallpaper_ui: 速度+音频共用折叠组（点任一标题整组开合）、让路独立、默认收起", {
    eui::Ui ui;
    wp::UiState state;
    const wp::WallpaperSnapshot snapshot = makeSnapshot(
        wp::WallpaperKind::Video, wp::Availability::Playing,
        wp::PlayMode::LoopList);
    composeFrame(ui, kPage1600x1000, snapshot, {}, state);
    DOCTEST_REQUIRE(ui.find("wallpaper.card.speed") != nullptr);
    DOCTEST_REQUIRE(ui.find("wallpaper.card.audio") != nullptr);
    DOCTEST_REQUIRE(ui.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("wallpaper.card.speed")->frame.height, 56.0f);
    DOCTEST_CHECK_EQ(ui.find("wallpaper.card.audio")->frame.height, 56.0f);
    DOCTEST_CHECK_EQ(ui.find("wallpaper.card.holds")->frame.height, 56.0f);
    DOCTEST_CHECK(ui.find("wallpaper.speed.slider") == nullptr);
    DOCTEST_CHECK(ui.find("wallpaper.audio.slider") == nullptr);
    DOCTEST_CHECK(ui.find("wallpaper.holds.reason.0") == nullptr);

    DOCTEST_CHECK(fireClick(ui, "wallpaper.speed.title"));
    eui::Ui expandedUi;
    composeFrame(expandedUi, kPage1600x1000, snapshot, {}, state);
    DOCTEST_REQUIRE(expandedUi.find("wallpaper.card.speed") != nullptr);
    DOCTEST_REQUIRE(expandedUi.find("wallpaper.card.audio") != nullptr);
    DOCTEST_REQUIRE(expandedUi.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK_EQ(expandedUi.find("wallpaper.card.speed")->frame.height,
                     150.0f);
    DOCTEST_CHECK_EQ(expandedUi.find("wallpaper.card.audio")->frame.height,
                     150.0f);
    DOCTEST_CHECK(expandedUi.find("wallpaper.speed.slider") != nullptr);
    DOCTEST_CHECK(expandedUi.find("wallpaper.audio.slider") != nullptr);
    DOCTEST_CHECK(expandedUi.find("wallpaper.audio.mute.track") != nullptr);
    DOCTEST_CHECK_EQ(
        expandedUi.find("wallpaper.card.holds")->frame.height, 56.0f);

    DOCTEST_CHECK(fireClick(expandedUi, "wallpaper.holds.title"));
    eui::Ui holdsUi;
    composeFrame(holdsUi, kPage1600x1000, snapshot, {}, state);
    DOCTEST_REQUIRE(holdsUi.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK_EQ(holdsUi.find("wallpaper.card.holds")->frame.height,
                     192.0f);
    for (int i = 0; i < 4; ++i) {
        DOCTEST_CHECK_MESSAGE(
            holdsUi.find("wallpaper.holds.reason." + std::to_string(i)) !=
                nullptr,
            "让路原因 chip " + std::to_string(i) + " 缺失");
    }
    DOCTEST_CHECK(holdsUi.find("wallpaper.holds.battery.track") != nullptr);
    DOCTEST_REQUIRE(holdsUi.find("wallpaper.card.speed") != nullptr);
    DOCTEST_CHECK_EQ(holdsUi.find("wallpaper.card.speed")->frame.height,
                     150.0f);

    // 点音频标题 → 整组收起；让路保持展开（互不联动）。
    DOCTEST_CHECK(fireClick(holdsUi, "wallpaper.audio.title"));
    eui::Ui collapsedUi;
    composeFrame(collapsedUi, kPage1600x1000, snapshot, {}, state);
    DOCTEST_REQUIRE(collapsedUi.find("wallpaper.card.speed") != nullptr);
    DOCTEST_REQUIRE(collapsedUi.find("wallpaper.card.audio") != nullptr);
    DOCTEST_REQUIRE(collapsedUi.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK_EQ(collapsedUi.find("wallpaper.card.speed")->frame.height,
                     56.0f);
    DOCTEST_CHECK_EQ(collapsedUi.find("wallpaper.card.audio")->frame.height,
                     56.0f);
    DOCTEST_CHECK(collapsedUi.find("wallpaper.speed.slider") == nullptr);
    DOCTEST_CHECK(collapsedUi.find("wallpaper.audio.slider") == nullptr);
    DOCTEST_CHECK_EQ(collapsedUi.find("wallpaper.card.holds")->frame.height,
                     192.0f);

    // 新会话状态 = 默认收起（不持久化）。
    wp::UiState freshState;
    eui::Ui freshUi;
    composeFrame(freshUi, kPage1600x1000, snapshot, {}, freshState);
    DOCTEST_REQUIRE(freshUi.find("wallpaper.card.speed") != nullptr);
    DOCTEST_REQUIRE(freshUi.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK_EQ(freshUi.find("wallpaper.card.speed")->frame.height,
                     56.0f);
    DOCTEST_CHECK_EQ(freshUi.find("wallpaper.card.holds")->frame.height,
                     56.0f);
    return true;
})

// 防：单循环 / 单张不变仍能改间隔（禁用失效）、间隔档位与秒数映射错、模式标签串来源。
REGISTER_TEST(wallpaper_ui_interval_and_modes,
              "wallpaper_ui: 单循环/单张不变禁用间隔；档位 5/10/15/30/60/120；模式标签随来源", {
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopSingle);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK(ui.find("wallpaper.interval.dropdown") != nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.interval.dropdown.disabled") != nullptr);
        DOCTEST_CHECK(contains(
            elementText(ui, "wallpaper.card.interval.hint"), "间隔不可用"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.0.text"),
                         std::string("单循环"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.1.text"),
                         std::string("列表循环"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.2.text"),
                         std::string("随机"));
    }
    {
        int requestedSeconds = 0;
        wp::UiActions actions;
        actions.setInterval = [&requestedSeconds](int seconds) {
            requestedSeconds = seconds;
        };
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList);
        composeFrame(ui, kPage1600x1000, snapshot, actions, state);
        DOCTEST_CHECK(ui.find("wallpaper.interval.dropdown.disabled") == nullptr);
        DOCTEST_CHECK(contains(
            elementText(ui, "wallpaper.card.interval.hint"), "换片"));
        DOCTEST_CHECK_EQ(
            elementText(ui, "wallpaper.interval.dropdown.item.label.0"),
            std::string("5 分钟"));
        DOCTEST_CHECK_EQ(
            elementText(ui, "wallpaper.interval.dropdown.item.label.5"),
            std::string("2 小时"));
        DOCTEST_CHECK(fireClick(ui, "wallpaper.interval.dropdown.item.3"));
        DOCTEST_CHECK_EQ(requestedSeconds, 1800);
    }
    {
        wp::UiState state;
        eui::Ui ui;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Image, wp::Availability::Playing,
            wp::PlayMode::LoopSingle);
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK(ui.find("wallpaper.interval.dropdown.disabled") != nullptr);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.0.text"),
                         std::string("单张不变"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.1.text"),
                         std::string("顺序轮播"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.mode.2.text"),
                         std::string("随机轮播"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.card.mode.title"),
                         std::string("轮播方式"));
    }
    return true;
})

// 防：图片模式仍显示速度/音频、片库图片仍显示格式卡、分辨率/适配档位没接线。
REGISTER_TEST(wallpaper_ui_image_and_library_cards,
              "wallpaper_ui: 图片模式隐藏速度+音频；片库图片显示分辨率/适配卡并接线，视频显示格式卡", {
    int requestedPixels = 0;
    wp::ImageFit requestedFit = wp::ImageFit::Fill;
    wp::PlayMode requestedMode = wp::PlayMode::LoopSingle;
    wp::UiActions actions;
    actions.setImageMinPixels = [&requestedPixels](int pixels) {
        requestedPixels = pixels;
    };
    actions.setImageFit = [&requestedFit](wp::ImageFit fit) {
        requestedFit = fit;
    };
    actions.setMode = [&requestedMode](wp::PlayMode mode) {
        requestedMode = mode;
    };

    wp::UiState state;
    eui::Ui playUi;
    const wp::WallpaperSnapshot image = makeSnapshot(
        wp::WallpaperKind::Image, wp::Availability::Playing,
        wp::PlayMode::LoopSingle);
    composeFrame(playUi, kPage1600x1000, image, actions, state);
    DOCTEST_CHECK(playUi.find("wallpaper.card.speed") == nullptr);
    DOCTEST_CHECK(playUi.find("wallpaper.card.audio") == nullptr);
    DOCTEST_CHECK(playUi.find("wallpaper.card.mode") != nullptr);
    DOCTEST_CHECK(playUi.find("wallpaper.card.holds") != nullptr);
    DOCTEST_CHECK(fireClick(playUi, "wallpaper.mode.2"));
    DOCTEST_CHECK(requestedMode == wp::PlayMode::Shuffle);

    state.tab = 1;
    eui::Ui libraryUi;
    composeFrame(libraryUi, kPage1600x1000, image, actions, state);
    DOCTEST_CHECK(libraryUi.find("wallpaper.card.formats") == nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.card.resolution") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.card.fit") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.folder.stat.total") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.folder.stat.passing") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.folder.stat.filtered") != nullptr);
    DOCTEST_CHECK(libraryUi.find("wallpaper.folder.stat.videos") == nullptr);
    DOCTEST_CHECK_EQ(elementText(libraryUi, "wallpaper.folder.stat.total"),
                     std::string("24"));
    DOCTEST_CHECK_EQ(elementText(libraryUi, "wallpaper.folder.stat.passing"),
                     std::string("18"));
    DOCTEST_CHECK_EQ(elementText(libraryUi, "wallpaper.folder.stat.filtered"),
                     std::string("6"));
    DOCTEST_CHECK(fireClick(libraryUi, "wallpaper.resolution.2"));
    DOCTEST_CHECK_EQ(requestedPixels, 8294400);
    DOCTEST_CHECK(fireClick(libraryUi, "wallpaper.fit.1"));
    DOCTEST_CHECK(requestedFit == wp::ImageFit::Fit);

    wp::WallpaperSnapshot fitted = image;
    fitted.imageFit = wp::ImageFit::Fit;
    eui::Ui fittedUi;
    composeFrame(fittedUi, kPage1600x1000, fitted, {}, state);
    DOCTEST_CHECK(contains(elementText(fittedUi, "wallpaper.card.fit.hint"),
                           "黑边"));

    // 视频片库：格式卡，无分辨率/适配。
    wp::UiState videoState;
    videoState.tab = 1;
    eui::Ui videoLibrary;
    const wp::WallpaperSnapshot video = makeSnapshot(
        wp::WallpaperKind::Video, wp::Availability::Playing,
        wp::PlayMode::LoopList);
    composeFrame(videoLibrary, kPage1600x1000, video, {}, videoState);
    DOCTEST_CHECK(videoLibrary.find("wallpaper.card.formats") != nullptr);
    DOCTEST_CHECK(videoLibrary.find("wallpaper.card.resolution") == nullptr);
    DOCTEST_CHECK(videoLibrary.find("wallpaper.card.fit") == nullptr);
    DOCTEST_CHECK_EQ(elementText(videoLibrary, "wallpaper.formats.chip.0.text"),
                     std::string(".mp4"));
    DOCTEST_CHECK_EQ(elementText(videoLibrary, "wallpaper.formats.chip.2.text"),
                     std::string(".m4v"));
    return true;
})

// 防：片库统计单位错（画「12」而非「12 个视频」）、扫描中仍显示统计/按钮可点、
// 目录缺失不提示、未配置路径画成 mono 路径。
REGISTER_TEST(wallpaper_ui_library_folder_states,
              "wallpaper_ui: 片库文件夹卡（统计/扫描禁用/目录缺失后缀/未配置占位/选择与重扫接线）", {
    int selectCalls = 0;
    int rescanCalls = 0;
    wp::UiActions actions;
    actions.selectFolder = [&selectCalls] { ++selectCalls; };
    actions.rescan = [&rescanCalls] { ++rescanCalls; };

    {
        wp::UiState state;
        state.tab = 1;
        wp::UiActions unconfiguredActions = actions;
        wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::FolderUnconfigured,
            wp::PlayMode::LoopSingle);
        snapshot.folderPath.clear();
        eui::Ui ui;
        composeFrame(ui, kPage1600x1000, snapshot, unconfiguredActions, state);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.folder.path"),
                         std::string("尚未选择文件夹"));
        DOCTEST_CHECK(ui.find("wallpaper.folder.stat.videos") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.folder.scanning") == nullptr);
    }
    {
        wp::UiState state;
        state.tab = 1;
        wp::UiActions liveActions = actions;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList);
        eui::Ui ui;
        composeFrame(ui, kPage1600x1000, snapshot, liveActions, state);
        DOCTEST_CHECK(contains(elementText(ui, "wallpaper.folder.path"),
                               "D:\\Wallpapers"));
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.folder.stat.videos"),
                         std::string("12 个视频"));
        DOCTEST_CHECK(ui.find("wallpaper.folder.scanning") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.folder.select.disabled") == nullptr);
        DOCTEST_CHECK(fireClick(ui, "wallpaper.folder.select.bg"));
        DOCTEST_CHECK(fireClick(ui, "wallpaper.folder.rescan.bg"));
        DOCTEST_CHECK_EQ(selectCalls, 1);
        DOCTEST_CHECK_EQ(rescanCalls, 1);
    }
    {
        wp::UiState state;
        state.tab = 1;
        wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::Playing,
            wp::PlayMode::LoopList);
        snapshot.scanning = true;
        eui::Ui ui;
        composeFrame(ui, kPage1600x1000, snapshot, actions, state);
        DOCTEST_CHECK(ui.find("wallpaper.folder.scanning") != nullptr);
        DOCTEST_CHECK_EQ(elementText(ui, "wallpaper.folder.scanning.text"),
                         std::string("正在扫描…"));
        DOCTEST_CHECK(ui.find("wallpaper.folder.stat.videos") == nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.folder.select.disabled") != nullptr);
        DOCTEST_CHECK(ui.find("wallpaper.folder.rescan.disabled") != nullptr);
        const auto* selectBg = ui.find("wallpaper.folder.select.bg");
        DOCTEST_REQUIRE(selectBg != nullptr);
        DOCTEST_CHECK_FALSE(static_cast<bool>(selectBg->onClick));
    }
    {
        wp::UiState state;
        state.tab = 1;
        const wp::WallpaperSnapshot snapshot = makeSnapshot(
            wp::WallpaperKind::Video, wp::Availability::FolderMissing,
            wp::PlayMode::LoopSingle);
        eui::Ui ui;
        composeFrame(ui, kPage1600x1000, snapshot, {}, state);
        DOCTEST_CHECK(contains(elementText(ui, "wallpaper.folder.path"),
                               "· 目录不见了"));
        DOCTEST_CHECK(ui.find("wallpaper.folder.stat.videos") == nullptr);
    }
    return true;
})

// 防：壁纸页回潮出删除/维护/转码/队列入口（需求 §7.4 明确没有），以及页内出现
// 文件名（本页面只应显示文件夹路径）。
REGISTER_TEST(wallpaper_ui_no_maintenance_entry,
              "wallpaper_ui: 不出现删除/维护/转码/队列入口（只保留文件夹路径 + 选择/重扫）", {
    wp::UiState state;
    eui::Ui ui;
    const wp::WallpaperSnapshot snapshot = makeSnapshot(
        wp::WallpaperKind::Video, wp::Availability::Playing,
        wp::PlayMode::LoopList);
    composeFrame(ui, kPage1600x1000, snapshot, {}, state);
    state.tab = 1;
    eui::Ui libraryUi;
    composeFrame(libraryUi, kPage1600x1000, snapshot, {}, state);
    for (eui::Ui* target : {&ui, &libraryUi}) {
        // 断言宏展开含 return false，只允许出现在返回 bool 的用例体（doctest.h
        // 头注第 3 条）；void 遍历回调里违规先收集，出循环后由用例体统一断言。
        std::vector<std::string> violations;
        walkTree(*target, [&](const core::dsl::Element& element) {
            const std::string id = scopedId(element);
            const std::string lower = [&] {
                std::string out = id;
                std::transform(out.begin(), out.end(), out.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                return out;
            }();
            if (contains(lower, "delete") || contains(lower, "remove") ||
                contains(lower, "maintain") || contains(lower, "queue") ||
                contains(lower, "transcode")) {
                violations.push_back("出现禁用入口 id: " + element.id);
            }
            if (contains(element.text, "删除") ||
                contains(element.text, "维护") ||
                contains(element.text, "队列")) {
                violations.push_back("出现禁用入口文案: " + element.text);
            }
            // 不显示文件名：所有文本里不应出现媒体扩展名（格式卡里的
            // ".mp4 / .mov / .m4v" 是格式说明而非文件名，单独放行 id 前缀）。
            if (id.rfind("wallpaper.formats.", 0) != 0 &&
                id.rfind("wallpaper.card.formats.", 0) != 0 &&
                id != "wallpaper.folder.hint" &&
                id != "wallpaper.card.formats.hint" &&
                id != "wallpaper.empty.support" &&
                id != "wallpaper.empty.body") {
                if (contains(element.text, ".mp4") ||
                    contains(element.text, ".mov") ||
                    contains(element.text, ".m4v")) {
                    violations.push_back("疑似显示文件名: " + element.text);
                }
            }
        });
        std::string joined;
        for (const auto& violation : violations) {
            if (!joined.empty()) joined += "; ";
            joined += violation;
        }
        DOCTEST_CHECK_MESSAGE(violations.empty(), joined);
    }
    return true;
})