// 基础配置页实现（服务目录 / 环境定义）。
//
// 视觉契约：页面 padding 32（上 0）、卡面白、主色走 theme token、正文 14、
// 页标题 24、按钮 40、圆角 8/12、chip 22/11/pill。
// Tab 必须用 ui_kit::wtab（块面式）；components::tabs 是下划线式，全局已淘汰。
#include "release_config_ui.h"

#include "release_checkbox.h"

#include <core/component_theme.h>
#include <core/render/text.h>
#include <core/theme.h>

#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>

#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/empty_state.h>
#include <ui_kit/form_row.h>
#include <ui_kit/metrics.h>
#include <ui_kit/wtab.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace module::release {

namespace {

using core::theme::Tokens;

constexpr float kPagePad = 32.0f;        // 页面左/右/下 padding；上 = 0（页标题顶格）
constexpr float kRowPadX = 16.0f;
constexpr float kRowH = 56.0f;
constexpr float kCardHeadH = 40.0f;
constexpr float kChipH = 22.0f;
constexpr float kChipFontInset = 1.0f;
constexpr float kColGap = 12.0f;
constexpr float kActionsW = 120.0f;
constexpr float kRowBtnW = 56.0f;
constexpr float kRowBtnGap = 8.0f;
constexpr float kPageHeaderH = ui_kit::kHeaderH;  // 40

// 下拉选项顺序必须与 ServiceKind 枚举一致（后端 / PC 前端 / 移动前端）。
constexpr const char* kServiceKindLabels[] = {"后端", "PC 前端", "移动前端"};
constexpr int kServiceKindCount = 3;

std::string trimText(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

ServiceKind serviceKindFromIndex(int index) {
    switch (index) {
        case 1: return ServiceKind::PcWeb;
        case 2: return ServiceKind::MobileWeb;
        default: return ServiceKind::Backend;
    }
}

int indexFromServiceKind(ServiceKind kind) {
    switch (kind) {
        case ServiceKind::Backend: return 0;
        case ServiceKind::PcWeb: return 1;
        case ServiceKind::MobileWeb: return 2;
    }
    return 0;  // 枚举穷举；兜底仅为满足所有路径有返回
}

bool containsId(const std::vector<std::int64_t>& ids, std::int64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void eraseId(std::vector<std::int64_t>& ids, std::int64_t id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

int nextSortValue(int currentMax) { return currentMax + 1; }

int maxServiceSort(const std::vector<ServiceDef>& services) {
    int value = 0;
    for (const auto& service : services) {
        value = std::max(value, service.sort);
    }
    return value;
}

int maxEnvSort(const std::vector<EnvDef>& envs) {
    int value = 0;
    for (const auto& env : envs) {
        value = std::max(value, env.sort);
    }
    return value;
}

void openNewService(ReleaseConfigUiState& s) {
    s.form = ReleaseConfigUiState::Form::Service;
    s.editServiceId = 0;  // 0 = 新增，id 由 store 分配
    s.editingService = false;
    s.serviceName.clear();
    s.serviceBranch.clear();
    s.serviceKind = ServiceKind::Backend;
    s.serviceKindOpen = false;
    s.serviceSort = nextSortValue(maxServiceSort(s.services));
    s.serviceError.clear();
}

void openServiceEditor(ReleaseConfigUiState& s, const ServiceDef& service) {
    s.form = ReleaseConfigUiState::Form::Service;
    s.editServiceId = service.id;
    s.editingService = true;
    s.serviceName = service.name;
    s.serviceBranch = service.defaultBranch;
    s.serviceKind = service.kind;
    s.serviceKindOpen = false;
    s.serviceSort = service.sort;
    s.serviceError.clear();
}

void openNewEnv(ReleaseConfigUiState& s) {
    s.form = ReleaseConfigUiState::Form::Env;
    s.editEnvId = 0;  // 0 = 新增，id 由 store 分配
    s.editingEnv = false;
    s.envName.clear();
    s.envServiceIds.clear();
    s.envStartText = "09:00";
    s.envEndText = "18:00";
    s.envSort = nextSortValue(maxEnvSort(s.envs));
    s.envError.clear();
}

void openEnvEditor(ReleaseConfigUiState& s, const EnvDef& env) {
    s.form = ReleaseConfigUiState::Form::Env;
    s.editEnvId = env.id;
    s.editingEnv = true;
    s.envName = env.name;
    s.envServiceIds = env.serviceIds;
    s.envStartText = formatMinuteOfDay(env.startMinute);
    s.envEndText = formatMinuteOfDay(env.endMinute);
    s.envSort = env.sort;
    s.envError.clear();
}

void closeForm(ReleaseConfigUiState& s) {
    s.form = ReleaseConfigUiState::Form::None;
    s.serviceKindOpen = false;
    s.serviceError.clear();
    s.envError.clear();
}

// chip 估宽与绘制共用：字号由 fontSizeSmall − 1 派生（100% 档 = 11），改字号必须同步改估宽。
float chipWidth(const std::string& label, float fontSize) {
    return core::TextPrimitive::measureTextWidth(label, {}, fontSize, 600) + 20.0f;
}

float drawChip(eui::Ui& ui, const std::string& id, const std::string& label, float x,
               float y, const Tokens& t, const eui::Color& bg, const eui::Color& fg) {
    const float fontSize = t.sizes.fontSizeSmall - kChipFontInset;
    const float width = chipWidth(label, fontSize);
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, kChipH)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(width, kChipH)
        .text(label)
        .fontSize(fontSize)
        .lineHeight(kChipH)
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .build();
    return width;
}

void fitText(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
             const std::string& text, float fontSize, float lineHeight, int weight,
             const eui::Color& color) {
    ui.text(id)
        .position(x, y)
        .size(w, h)
        .text(text)
        .fontSize(fontSize)
        .lineHeight(lineHeight)
        .fontWeight(weight)
        .maxWidth(w)
        .overflow(core::TextOverflow::Ellipsis)
        .clip()
        .color(color)
        .build();
}

// 弹窗标题契约：18px / 600 / 行高 24；不要换成 20/700 的通用对话框标题。
void dialogHeading(eui::Ui& ui, const std::string& id, float width,
                   const std::string& label, const Tokens& t) {
    ui.text(id)
        .position(ui_kit::kDialogInsetX, 16.0f)
        .size(std::max(0.0f, width - ui_kit::kDialogBodyInsetX), 26.0f)
        .text(label)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(24.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
}

void headCell(eui::Ui& ui, const std::string& id, const std::string& label, float x,
              float w, const Tokens& t) {
    fitText(ui, id, x, 11.0f, w, 18.0f, label, t.sizes.fontSizeSmall, 18.0f, 400,
            t.colors.textTertiary);
}

struct ServiceColumns {
    float nameX = kRowPadX;
    float nameW = 0.0f;
    float kindX = 0.0f;
    float kindW = 88.0f;
    float branchX = 0.0f;
    float branchW = 160.0f;
    float configX = 0.0f;
    float configW = 0.0f;
    float actionsX = 0.0f;
    float actionsW = kActionsW;
};

ServiceColumns serviceColumns(float rowW) {
    ServiceColumns c;
    const float minNameW = 120.0f;
    const float reserved = kRowPadX * 2.0f + c.kindW + c.branchW + c.actionsW +
                           kColGap * 4.0f + minNameW;
    c.configW = std::clamp(rowW - reserved, 0.0f, 340.0f);
    c.nameW = std::max(0.0f, rowW - kRowPadX * 2.0f - c.kindW - c.branchW -
                                 c.actionsW - c.configW - kColGap * 4.0f);
    c.kindX = c.nameX + c.nameW + kColGap;
    c.branchX = c.kindX + c.kindW + kColGap;
    c.configX = c.branchX + c.branchW + kColGap;
    c.actionsX = rowW - kRowPadX - c.actionsW;
    return c;
}

struct EnvColumns {
    float nameX = kRowPadX;
    float nameW = 0.0f;
    float countX = 0.0f;
    float countW = 140.0f;
    float windowX = 0.0f;
    float windowW = 260.0f;
    float actionsX = 0.0f;
    float actionsW = kActionsW;
};

EnvColumns envColumns(float rowW) {
    EnvColumns c;
    c.nameW = std::max(0.0f, rowW - kRowPadX * 2.0f - c.countW - c.windowW -
                                 c.actionsW - kColGap * 4.0f);
    c.countX = c.nameX + c.nameW + kColGap;
    c.windowX = c.countX + c.countW + kColGap;
    c.actionsX = rowW - kRowPadX - c.actionsW;
    return c;
}

void drawRowActions(eui::Ui& ui, const std::string& base, float x, const Tokens& t,
                    const std::function<void()>& onEdit,
                    const std::function<void()>& onDelete) {
    ui_kit::ButtonOptions edit;
    edit.onClick = onEdit;
    ui_kit::button(ui, base + ".edit", "编辑", ui_kit::ButtonLevel::Text, x, 8.0f,
                   kRowBtnW, ui_kit::kButtonHeight, t, edit);

    ui_kit::ButtonOptions remove;
    remove.onClick = onDelete;
    remove.hasTextColorOverride = true;
    remove.textColorOverride = t.colors.errorText;
    ui_kit::button(ui, base + ".delete", "删除", ui_kit::ButtonLevel::Text,
                   x + kRowBtnW + kRowBtnGap, 8.0f, kRowBtnW,
                   ui_kit::kButtonHeight, t, remove);
}

void drawServiceRow(eui::Ui& ui, const ServiceDef& service, float rowW,
                    const Tokens& t, ReleaseConfigUiState& state) {
    const ServiceColumns c = serviceColumns(rowW);
    const std::string base = "release.config.service." + std::to_string(service.id);
    const bool backend = service.kind == ServiceKind::Backend;

    ui.stack(base).size(rowW, kRowH).content([&] {
        fitText(ui, base + ".name", c.nameX, 17.0f, c.nameW, 22.0f, service.name,
                t.sizes.fontSizeBody, 22.0f, 600, t.colors.textPrimary);

        drawChip(ui, base + ".kind", serviceKindLabel(service.kind), c.kindX, 17.0f, t,
                 backend ? t.colors.primarySoftBg : t.colors.hover,
                 backend ? t.colors.primaryText : t.colors.textSecondary);

        const bool hasBranch = !service.defaultBranch.empty();
        fitText(ui, base + ".branch", c.branchX, 17.0f, c.branchW, 22.0f,
                hasBranch ? service.defaultBranch : std::string("—"),
                t.sizes.fontSizeBody, 22.0f, 400,
                hasBranch ? t.colors.textSecondary : t.colors.textTertiary);

        // 配置项由类型固定，不逐服务勾选：后端 5 个 tag，前端只显示「无配置项」。
        const auto& items = configItemLabels(service.kind);
        if (items.empty()) {
            fitText(ui, base + ".noConfig", c.configX, 19.0f, c.configW, 18.0f,
                    "无配置项", t.sizes.fontSizeSmall, 18.0f, 400,
                    t.colors.textTertiary);
        } else {
            // 窄窗（最小 1000×640）放不下 5 个 tag 时折叠为 "+N"，不得压到操作列。
            const float chipFontSize = t.sizes.fontSizeSmall - kChipFontInset;
            const float tagRight = c.configX + c.configW;
            float tagX = c.configX;
            std::size_t drawn = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const float width = chipWidth(items[i], chipFontSize);
                const std::size_t remaining = items.size() - i;
                const float moreWidth =
                    remaining > 1
                        ? chipWidth("+" + std::to_string(remaining - 1), chipFontSize) +
                              4.0f
                        : 0.0f;
                if (tagX + width + moreWidth > tagRight) {
                    break;
                }
                tagX += drawChip(ui, base + ".tag." + std::to_string(i), items[i],
                                 tagX, 17.0f, t, t.colors.hover,
                                 t.colors.textSecondary) +
                        4.0f;
                ++drawn;
            }
            if (drawn < items.size()) {
                const std::string more = "+" + std::to_string(items.size() - drawn);
                if (tagX + chipWidth(more, chipFontSize) <= tagRight) {
                    drawChip(ui, base + ".tag.more", more, tagX, 17.0f, t,
                             t.colors.hover, t.colors.textSecondary);
                }
            }
        }

        drawRowActions(
            ui, base, c.actionsX, t,
            [&state, service]() { openServiceEditor(state, service); },
            [&state, id = service.id]() {
                state.confirmDelete = true;
                state.deleteIsService = true;
                state.deleteId = id;
                state.deleteError.clear();
            });

        ui.rect(base + ".sep")
            .position(kRowPadX, kRowH - 1.0f)
            .size(std::max(0.0f, rowW - kRowPadX * 2.0f), 1.0f)
            .color(t.colors.borderMuted)
            .build();
    }).build();
}

void drawEnvRow(eui::Ui& ui, const EnvDef& env, float rowW, const Tokens& t,
                ReleaseConfigUiState& state) {
    const EnvColumns c = envColumns(rowW);
    const std::string base = "release.config.env." + std::to_string(env.id);

    ui.stack(base).size(rowW, kRowH).content([&] {
        fitText(ui, base + ".name", c.nameX, 17.0f, c.nameW, 22.0f, env.name,
                t.sizes.fontSizeBody, 22.0f, 600, t.colors.textPrimary);
        fitText(ui, base + ".count", c.countX, 17.0f, c.countW, 22.0f,
                std::to_string(env.serviceIds.size()) + " 个服务",
                t.sizes.fontSizeBody, 22.0f, 400, t.colors.textSecondary);
        fitText(ui, base + ".window", c.windowX, 17.0f, c.windowW, 22.0f,
                formatTimeWindow(env.startMinute, env.endMinute),
                t.sizes.fontSizeBody, 22.0f, 400, t.colors.textPrimary);

        drawRowActions(
            ui, base, c.actionsX, t,
            [&state, env]() { openEnvEditor(state, env); },
            [&state, id = env.id]() {
                state.confirmDelete = true;
                state.deleteIsService = false;
                state.deleteId = id;
                state.deleteError.clear();
            });

        ui.rect(base + ".sep")
            .position(kRowPadX, kRowH - 1.0f)
            .size(std::max(0.0f, rowW - kRowPadX * 2.0f), 1.0f)
            .color(t.colors.borderMuted)
            .build();
    }).build();
}

void drawListHeader(eui::Ui& ui, float rowW, ReleaseConfigUiState::Tab tab,
                    const Tokens& t) {
    if (tab == ReleaseConfigUiState::Tab::Services) {
        const ServiceColumns c = serviceColumns(rowW);
        headCell(ui, "release.config.card.head.name", "名称", c.nameX, c.nameW, t);
        headCell(ui, "release.config.card.head.kind", "类型", c.kindX, c.kindW, t);
        headCell(ui, "release.config.card.head.branch", "默认分支", c.branchX,
                 c.branchW, t);
        headCell(ui, "release.config.card.head.config", "配置项", c.configX,
                 std::max(0.0f, c.actionsX - c.configX - kColGap), t);
    } else {
        const EnvColumns c = envColumns(rowW);
        headCell(ui, "release.config.card.head.name", "名称", c.nameX, c.nameW, t);
        headCell(ui, "release.config.card.head.count", "包含服务", c.countX,
                 c.countW, t);
        headCell(ui, "release.config.card.head.window", "时间窗", c.windowX,
                 c.windowW, t);
    }
    headCell(ui, "release.config.card.head.actions", "操作",
             rowW - kRowPadX - kActionsW, kActionsW, t);
}

// 列表区。data 来自 state 快照；本函数不查库。
void drawListCard(eui::Ui& ui, float contentW, float cardH,
                  ReleaseConfigUiState& state, const Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
    const bool servicesTab = state.tab == ReleaseConfigUiState::Tab::Services;
    const float listH = std::max(0.0f, cardH - kCardHeadH);
    const std::size_t count =
        servicesTab ? state.services.size() : state.envs.size();
    const std::string contentKey = std::string("release.config.list.") +
                                   (servicesTab ? "s" : "e") + "." +
                                   std::to_string(count) +
                                   (state.loadError.empty() ? "0" : "1");

    ui.stack("release.config.card").size(contentW, std::max(0.0f, cardH)).content([&] {
        // 内容卡走设置卡档：白面半透 + 12 圆角 + 1px 描边 + (0,1)/3/0.02 轻阴影；
        // 不要换 cardFrame（panel 阴影档偏重，原型并排目检会判定阴影差异）。
        ui.rect("release.config.card.bg")
            .position(0.0f, 0.0f)
            .size(contentW, cardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .shadow(components::theme::shadow(componentTheme, 3.0f, 1.0f, 0.04f,
                                              0.02f))
            .build();
        drawListHeader(ui, contentW, state.tab, t);
        ui.rect("release.config.card.headsep")
            .position(kRowPadX, kCardHeadH - 1.0f)
            .size(std::max(0.0f, contentW - kRowPadX * 2.0f), 1.0f)
            .color(t.colors.borderMuted)
            .build();

        components::scrollView(ui, "release.config.list")
            .position(0.0f, kCardHeadH)
            .size(contentW, listH)
            .gap(0.0f)
            .step(kRowH)
            .theme(componentTheme)
            .scrollbarOverlay(true)
            .contentKey(contentKey)
            .content([&](eui::Ui& inner, float rowW, float viewportH) {
                if (!state.loadError.empty()) {
                    inner.stack("release.config.error.row").size(rowW, 24.0f).content([&] {
                        fitText(inner, "release.config.error", kRowPadX, 3.0f,
                                std::max(0.0f, rowW - kRowPadX * 2.0f), 18.0f,
                                state.loadError, t.sizes.fontSizeSmall, 18.0f, 400,
                                t.colors.errorText);
                    }).build();
                }

                const bool empty =
                    servicesTab ? state.services.empty() : state.envs.empty();
                if (empty) {
                    const std::string emptyId =
                        servicesTab ? "release.config.empty" : "release.config.env.empty";
                    const float emptyH = std::clamp(viewportH - 40.0f, 160.0f, 260.0f);
                    inner.stack(emptyId + ".wrap").size(rowW, emptyH).content([&] {
                        std::optional<std::pair<std::string, std::function<void()>>> action;
                        action.emplace(servicesTab ? std::string("新增服务")
                                                   : std::string("新增环境"),
                                       [&state, servicesTab]() {
                                           if (servicesTab) {
                                               openNewService(state);
                                           } else {
                                               openNewEnv(state);
                                           }
                                       });
                        ui_kit::emptyState(inner, emptyId,
                                           servicesTab ? "暂无服务" : "暂无环境",
                                           action, 0.0f, 12.0f, rowW,
                                           std::max(0.0f, emptyH - 24.0f), t);
                    }).build();
                    return;
                }

                if (servicesTab) {
                    for (const auto& service : state.services) {
                        drawServiceRow(inner, service, rowW, t, state);
                    }
                } else {
                    for (const auto& env : state.envs) {
                        drawEnvRow(inner, env, rowW, t, state);
                    }
                }
            })
            .build();
    }).build();
}

void drawPageHeader(eui::Ui& ui, float contentW, bool servicesTab,
                    ReleaseConfigUiState& state, const Tokens& t) {
    ui.stack("release.config.header").size(contentW, kPageHeaderH).content([&] {
        ui.text("release.page.title")
            .position(0.0f, 0.0f)
            .size(std::max(0.0f, contentW - 140.0f), kPageHeaderH)
            .text("基础配置")
            .fontSize(t.sizes.fontSizePage)
            .lineHeight(32.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();

        ui_kit::ButtonOptions add;
        add.onClick = [&state, servicesTab]() {
            if (servicesTab) {
                openNewService(state);
            } else {
                openNewEnv(state);
            }
        };
        ui_kit::button(ui, "release.config.btn.add",
                       servicesTab ? "新增服务" : "新增环境",
                       ui_kit::ButtonLevel::Primary, contentW - 112.0f, 0.0f, 112.0f,
                       ui_kit::kButtonHeight, t, add);
    }).build();
}

void drawTabs(eui::Ui& ui, float contentW, ReleaseConfigUiState& state,
              const Tokens& t) {
    ui.stack("release.config.tabs").size(contentW, ui_kit::kHeaderH).content([&] {
        const char* labels[2] = {"服务目录", "环境定义"};
        float x = 0.0f;
        for (int i = 0; i < 2; ++i) {
            const float tabW =
                core::TextPrimitive::measureTextWidth(labels[i], {},
                                                      ui_kit::kWtabFontSize, 400) +
                32.0f;  // 水平内距 16 × 2
            const bool active =
                (i == 0) == (state.tab == ReleaseConfigUiState::Tab::Services);
            ui_kit::wtab(ui, "release.config.tab." + std::to_string(i), labels[i],
                         active, x, 0.0f, tabW, ui_kit::kHeaderH, t,
                         [&state, i]() {
                             state.tab = i == 0
                                             ? ReleaseConfigUiState::Tab::Services
                                             : ReleaseConfigUiState::Tab::Envs;
                         });
            x += tabW + 4.0f;
        }
    }).build();
}

void saveServiceDraft(
    ReleaseConfigUiState& s,
    const std::function<core::Result<void>(const ServiceDef&)>& upsertService) {
    const std::string name = trimText(s.serviceName);
    if (name.empty()) {
        s.serviceError = "名称不能为空";
        return;
    }
    for (const auto& other : s.services) {
        if (other.id != s.editServiceId && other.name == name) {
            s.serviceError = "名称已存在，请换一个";
            return;
        }
    }
    if (!upsertService) {
        s.serviceError = "保存通道未就绪";
        return;
    }

    ServiceDef def;
    def.id = s.editServiceId;
    def.name = name;
    def.kind = s.serviceKind;
    def.defaultBranch = trimText(s.serviceBranch);
    def.sort = s.serviceSort;

    const auto result = upsertService(def);
    if (result.isFail()) {
        s.serviceError = result.errorMessage();
        return;
    }
    s.form = ReleaseConfigUiState::Form::None;
    s.serviceKindOpen = false;
    s.serviceError.clear();
    s.dirty = true;
}

void saveEnvDraft(
    ReleaseConfigUiState& s,
    const std::function<core::Result<void>(const EnvDef&)>& upsertEnv) {
    const std::string name = trimText(s.envName);
    if (name.empty()) {
        s.envError = "名称不能为空";
        return;
    }
    int startMinute = 0;
    int endMinute = 0;
    if (!parseTimeText(s.envStartText, &startMinute)) {
        s.envError = "开始时间格式应为 HH:MM（00:00–23:59）";
        return;
    }
    if (!parseTimeText(s.envEndText, &endMinute)) {
        s.envError = "结束时间格式应为 HH:MM（00:00–23:59）";
        return;
    }
    if (!upsertEnv) {
        s.envError = "保存通道未就绪";
        return;
    }

    EnvDef def;
    def.id = s.editEnvId;
    def.name = name;
    def.serviceIds = s.envServiceIds;
    def.startMinute = startMinute;
    def.endMinute = endMinute;
    def.sort = s.envSort;

    const auto result = upsertEnv(def);
    if (result.isFail()) {
        s.envError = result.errorMessage();
        return;
    }
    s.form = ReleaseConfigUiState::Form::None;
    s.envError.clear();
    s.dirty = true;
}

void drawServiceDialog(eui::Ui& ui, const eui::Screen& screen,
                       ReleaseConfigUiState& state,
                       const ReleaseConfigActions& actions, const Tokens& t,
                       const components::theme::ThemeColorTokens& componentTheme) {
    ui_kit::DialogFrame frame;
    frame.id = "release.dialog.service";
    frame.width = ui_kit::DialogWidth::Standard;  // 460
    frame.height = 360.0f;
    frame.open = state.form == ReleaseConfigUiState::Form::Service;
    frame.onOpenChange = [&state](bool open) {
        if (!open) {
            closeForm(state);
        }
    };

    const float w = frame.widthPx();
    const float innerX = ui_kit::kDialogInsetX;  // 24
    const float rowW = w - ui_kit::kDialogBodyInsetX;
    const auto upsertService = actions.upsertService;
    // 弹窗外框圆角取 16（主题组件默认 18）；只改对话框的本地副本。
    auto dialogTheme = componentTheme;
    dialogTheme.metrics.radius.section = t.sizes.radiusOverlay;

    ui_kit::drawDialogFrame(ui, screen, frame, dialogTheme, [&] {
        dialogHeading(ui, "release.dialog.service.title", w,
                      state.editingService ? "编辑服务" : "新增服务", t);

        ui_kit::formRow(
            ui, "release.dialog.service.name", "名称", innerX, 60.0f, rowW, 40.0f,
            [&](eui::Ui& u, float cx, float cy, float cw, float ch) {
                components::input(u, "release.dialog.service.name.input")
                    .position(cx, cy)
                    .size(cw, ch)
                    .value(state.serviceName)
                    .placeholder("必填，同一目录内不可重名")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([&state](const std::string& value) {
                        state.serviceName = value;
                        state.serviceError.clear();
                    })
                    .onCancel([&state]() { closeForm(state); })
                    .build();
            },
            t);

        ui_kit::formRow(
            ui, "release.dialog.service.kind", "类型", innerX, 116.0f, rowW, 40.0f,
            [&](eui::Ui& u, float cx, float cy, float cw, float ch) {
                u.stack("release.dialog.service.kind.wrap")
                    .position(cx, cy)
                    .size(cw, ch)
                    .content([&] {
                        components::dropdown(u, "release.dialog.service.kind.dd")
                            .size(cw, ch)
                            .theme(componentTheme)
                            .items(std::vector<std::string>(kServiceKindLabels,
                                                            kServiceKindLabels +
                                                                kServiceKindCount))
                            .selected(indexFromServiceKind(state.serviceKind))
                            .open(state.serviceKindOpen)
                            .onOpenChange([&state](bool open) {
                                state.serviceKindOpen = open;
                            })
                            .onChange([&state](int index) {
                                state.serviceKind = serviceKindFromIndex(index);
                                state.serviceError.clear();
                            })
                            .build();
                    })
                    .build();
            },
            t);

        ui_kit::formRow(
            ui, "release.dialog.service.branch", "默认分支", innerX, 172.0f, rowW,
            40.0f,
            [&](eui::Ui& u, float cx, float cy, float cw, float ch) {
                components::input(u, "release.dialog.service.branch.input")
                    .position(cx, cy)
                    .size(cw, ch)
                    .value(state.serviceBranch)
                    .placeholder("新建任务时预填，可改；不做格式校验")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([&state](const std::string& value) {
                        state.serviceBranch = value;
                        state.serviceError.clear();
                    })
                    .onCancel([&state]() { closeForm(state); })
                    .build();
            },
            t);

        const std::string hint =
            state.serviceKind == ServiceKind::Backend
                ? "该类型固定 5 项配置：nacos · 数据库 · 定时任务 · 菜单 · 网关"
                : "该类型无配置项";
        fitText(ui, "release.dialog.service.hint", innerX, 224.0f, rowW, 18.0f, hint,
                t.sizes.fontSizeSmall, 18.0f, 400, t.colors.textTertiary);

        fitText(ui, "release.dialog.service.error", innerX, 248.0f, rowW, 18.0f,
                state.serviceError, t.sizes.fontSizeSmall, 18.0f, 400,
                t.colors.errorText);

        ui_kit::drawDialogButtons(
            ui, frame.id, w, frame.height,
            {.label = "取消",
             .style = ui_kit::DialogButtonStyle::Secondary,
             .onClick = [&state]() { closeForm(state); }},
            {.label = "保存",
             .style = ui_kit::DialogButtonStyle::Primary,
             .onClick = [&state, upsertService]() {
                 saveServiceDraft(state, upsertService);
             }},
            t, dialogTheme);
    });
}

void drawEnvDialog(eui::Ui& ui, const eui::Screen& screen,
                   ReleaseConfigUiState& state,
                   const ReleaseConfigActions& actions, const Tokens& t,
                   const components::theme::ThemeColorTokens& componentTheme) {
    ui_kit::DialogFrame frame;
    frame.id = "release.dialog.env";
    frame.width = ui_kit::DialogWidth::Wide;  // 640
    frame.height = 460.0f;
    frame.open = state.form == ReleaseConfigUiState::Form::Env;
    frame.onOpenChange = [&state](bool open) {
        if (!open) {
            closeForm(state);
        }
    };

    const float w = frame.widthPx();
    const float innerX = ui_kit::kDialogInsetX;  // 24
    const float rowW = w - ui_kit::kDialogBodyInsetX;
    const auto upsertEnv = actions.upsertEnv;
    auto dialogTheme = componentTheme;
    dialogTheme.metrics.radius.section = t.sizes.radiusOverlay;  // 16

    ui_kit::drawDialogFrame(ui, screen, frame, dialogTheme, [&] {
        dialogHeading(ui, "release.dialog.env.title", w,
                      state.editingEnv ? "编辑环境" : "新增环境", t);

        ui_kit::formRow(
            ui, "release.dialog.env.name", "名称", innerX, 60.0f, rowW, 40.0f,
            [&](eui::Ui& u, float cx, float cy, float cw, float ch) {
                components::input(u, "release.dialog.env.name.input")
                    .position(cx, cy)
                    .size(cw, ch)
                    .value(state.envName)
                    .placeholder("如 EM / EU / AM / US")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([&state](const std::string& value) {
                        state.envName = value;
                        state.envError.clear();
                    })
                    .onCancel([&state]() { closeForm(state); })
                    .build();
            },
            t);

        ui.text("release.dialog.env.services.label")
            .position(innerX, 112.0f)
            .size(160.0f, 20.0f)
            .text("包含服务")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .color(t.colors.textPrimary)
            .build();
        ui.text("release.dialog.env.services.count")
            .position(innerX + rowW - 140.0f, 112.0f)
            .size(140.0f, 20.0f)
            .text("已选 " + std::to_string(state.envServiceIds.size()) + " 个")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(20.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textTertiary)
            .build();

        // 复选列表：自绘勾选框支持禁用态；服务目录项没有禁用条件，此处全部可用。
        components::scrollView(ui, "release.dialog.env.services.scroll")
            .position(innerX, 136.0f)
            .size(rowW, 144.0f)
            .gap(2.0f)
            .step(34.0f)
            .theme(componentTheme)
            .scrollbarOverlay(true)
            .contentKey("release.dialog.env.services." +
                        std::to_string(state.services.size()) + "." +
                        std::to_string(state.envServiceIds.size()))
            .content([&](eui::Ui& inner, float cw, float) {
                if (state.services.empty()) {
                    inner.text("release.dialog.env.services.empty")
                        .size(cw, 32.0f)
                        .text("服务目录为空，请先到「服务目录」新增服务")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(32.0f)
                        .color(t.colors.textTertiary)
                        .build();
                    return;
                }
                for (const auto& service : state.services) {
                    const std::string boxId =
                        "release.dialog.env.services.check." + std::to_string(service.id);
                    ReleaseCheckboxOptions box;
                    box.checked = containsId(state.envServiceIds, service.id);
                    box.label = service.name;
                    box.fontSize = core::theme::scaled(13.0f);
                    box.boxSize = core::theme::scaled(16.0f);
                    box.width = cw;
                    box.height = 32.0f;
                    drawReleaseCheckbox(
                        inner, boxId, box,
                        [&state, id = service.id]() {
                            const bool next = !containsId(state.envServiceIds, id);
                            eraseId(state.envServiceIds, id);
                            if (next) {
                                state.envServiceIds.push_back(id);
                            }
                            state.envError.clear();
                        },
                        t);
                }
            })
            .build();

        ui.text("release.dialog.env.window.label")
            .position(innerX, 292.0f)
            .size(110.0f, 40.0f)
            .text("时间窗")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(40.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();

        const float inputX = innerX + 110.0f + t.sizes.spacing;  // label 110 + 8
        const float timeW = 120.0f;
        components::input(ui, "release.dialog.env.start.input")
            .position(inputX, 292.0f)
            .size(timeW, 40.0f)
            .value(state.envStartText)
            .placeholder("HH:MM")
            .fontSize(t.sizes.fontSizeBody)
            .theme(componentTheme)
            .onChange([&state](const std::string& value) {
                state.envStartText = value;
                state.envError.clear();
            })
            .onCancel([&state]() { closeForm(state); })
            .build();
        ui.text("release.dialog.env.window.dash")
            .position(inputX + timeW, 292.0f)
            .size(16.0f, 40.0f)
            .text("–")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(40.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        components::input(ui, "release.dialog.env.end.input")
            .position(inputX + timeW + 16.0f, 292.0f)
            .size(timeW, 40.0f)
            .value(state.envEndText)
            .placeholder("HH:MM")
            .fontSize(t.sizes.fontSizeBody)
            .theme(componentTheme)
            .onChange([&state](const std::string& value) {
                state.envEndText = value;
                state.envError.clear();
            })
            .onCancel([&state]() { closeForm(state); })
            .build();
        fitText(ui, "release.dialog.env.window.note", inputX + timeW + 16.0f + timeW +
                                                          t.sizes.spacing,
               292.0f, std::max(0.0f, innerX + rowW - (inputX + timeW * 2.0f + 24.0f)),
                40.0f, "结束 ≤ 开始 = 次日", t.sizes.fontSizeSmall, 40.0f, 400,
                t.colors.textTertiary);

        fitText(ui, "release.dialog.env.hint", innerX, 340.0f, rowW, 18.0f,
                "跨零点示例：22:00–02:00 显示为 22:00–02:00(次日)；09:00–09:00 = 24 小时窗",
                t.sizes.fontSizeSmall, 18.0f, 400, t.colors.textTertiary);
        fitText(ui, "release.dialog.env.error", innerX, 366.0f, rowW, 18.0f,
                state.envError, t.sizes.fontSizeSmall, 18.0f, 400,
                t.colors.errorText);

        ui_kit::drawDialogButtons(
            ui, frame.id, w, frame.height,
            {.label = "取消",
             .style = ui_kit::DialogButtonStyle::Secondary,
             .onClick = [&state]() { closeForm(state); }},
            {.label = "保存",
             .style = ui_kit::DialogButtonStyle::Primary,
             .onClick = [&state, upsertEnv]() { saveEnvDraft(state, upsertEnv); }},
            t, dialogTheme);
    });
}

std::string deleteMessage(const ReleaseConfigUiState& state) {
    if (state.deleteIsService) {
        for (const auto& service : state.services) {
            if (service.id != state.deleteId) {
                continue;
            }
            int refCount = 0;
            for (const auto& env : state.envs) {
                if (containsId(env.serviceIds, state.deleteId)) {
                    ++refCount;
                }
            }
            if (refCount > 0) {
                return "「" + service.name + "」被 " + std::to_string(refCount) +
                       " 个环境引用；删除后将自动从这些环境摘除。确认删除？";
            }
            return "「" + service.name + "」将从服务目录移除，此操作不可恢复。";
        }
        return "该服务将从服务目录移除，此操作不可恢复。";
    }
    for (const auto& env : state.envs) {
        if (env.id == state.deleteId) {
            return "「" + env.name + "」将从环境定义移除，此操作不可恢复。";
        }
    }
    return "该环境将从环境定义移除，此操作不可恢复。";
}

void drawDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseConfigUiState& state,
                      const ReleaseConfigActions& actions, const Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
    ui_kit::DialogFrame frame;
    frame.id = "release.dialog.delete";
    frame.width = ui_kit::DialogWidth::Standard;  // 460
    frame.height = 240.0f;
    frame.open = state.confirmDelete;
    frame.onOpenChange = [&state](bool open) {
        if (!open) {
            state.confirmDelete = false;
            state.deleteError.clear();
        }
    };

    const float w = frame.widthPx();
    const float innerX = ui_kit::kDialogInsetX;
    const float rowW = w - ui_kit::kDialogBodyInsetX;
    const auto deleteService = actions.deleteService;
    const auto deleteEnv = actions.deleteEnv;
    auto dialogTheme = componentTheme;
    dialogTheme.metrics.radius.section = t.sizes.radiusOverlay;  // 16

    ui_kit::drawDialogFrame(ui, screen, frame, dialogTheme, [&] {
        dialogHeading(ui, "release.dialog.delete.title", w,
                      state.deleteIsService ? "删除服务" : "删除环境", t);

        ui.text("release.dialog.delete.message")
            .position(innerX, 52.0f)
            .size(rowW, 88.0f)
            .text(deleteMessage(state))
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(22.0f)
            .wrap(true)
            .color(t.colors.textSecondary)
            .build();

        fitText(ui, "release.dialog.delete.error", innerX, 144.0f, rowW, 18.0f,
                state.deleteError, t.sizes.fontSizeSmall, 18.0f, 400,
                t.colors.errorText);

        ui_kit::drawDialogButtons(
            ui, frame.id, w, frame.height,
            {.label = "取消",
             .style = ui_kit::DialogButtonStyle::Secondary,
             .onClick = [&state]() {
                 state.confirmDelete = false;
                 state.deleteError.clear();
             }},
            {.label = "删除",
             .style = ui_kit::DialogButtonStyle::Danger,
             .onClick = [&state, deleteService, deleteEnv]() {
                 if (!state.confirmDelete) {
                     return;
                 }
                 if (state.deleteIsService) {
                     if (!deleteService) {
                         state.deleteError = "删除通道未就绪";
                         return;
                     }
                     const auto result = deleteService(state.deleteId);
                     if (result.isFail()) {
                         state.deleteError = result.errorMessage();
                         return;
                     }
                 } else {
                     if (!deleteEnv) {
                         state.deleteError = "删除通道未就绪";
                         return;
                     }
                     const auto result = deleteEnv(state.deleteId);
                     if (result.isFail()) {
                         state.deleteError = result.errorMessage();
                         return;
                     }
                 }
                 // 确认后才允许落库；成功后置 dirty 让宿主下一帧重读（含服务被环境摘除）。
                 state.confirmDelete = false;
                 state.deleteError.clear();
                 state.deleteId = 0;
                 state.dirty = true;
             }},
            t, dialogTheme);
    });
}

} // namespace

const std::vector<std::string>& configItemLabels(ServiceKind kind) {
    static const std::vector<std::string> kBackend = {"nacos", "数据库", "定时任务",
                                                      "菜单", "网关"};
    static const std::vector<std::string> kNone;
    // 类型决定配置项：两类前端恒为空表，不要为了“统一”给前端补标签。
    return kind == ServiceKind::Backend ? kBackend : kNone;
}

const char* serviceKindLabel(ServiceKind kind) {
    switch (kind) {
        case ServiceKind::Backend: return kServiceKindLabels[0];
        case ServiceKind::PcWeb: return kServiceKindLabels[1];
        case ServiceKind::MobileWeb: return kServiceKindLabels[2];
    }
    return kServiceKindLabels[0];
}

std::string formatMinuteOfDay(int minute) {
    const int clamped = std::clamp(minute, 0, 1439);
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", clamped / 60, clamped % 60);
    return std::string(buffer);
}

bool isCrossMidnight(int startMinute, int endMinute) {
    // 相等也算次日：09:00–09:00 是 24 小时窗，不是零长窗。
    return endMinute <= startMinute;
}

bool parseTimeText(const std::string& text, int* minute) {
    if (minute == nullptr) {
        return false;
    }
    const std::string trimmed = trimText(text);
    const std::size_t colon = trimmed.find(':');
    if (colon == std::string::npos ||
        trimmed.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    const std::string hourPart = trimmed.substr(0, colon);
    const std::string minutePart = trimmed.substr(colon + 1);
    if (hourPart.empty() || hourPart.size() > 2 || minutePart.empty() ||
        minutePart.size() > 2) {
        return false;
    }
    for (char c : hourPart) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    for (char c : minutePart) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const int hour = (hourPart.size() == 2)
                         ? (hourPart[0] - '0') * 10 + (hourPart[1] - '0')
                         : hourPart[0] - '0';
    const int mins = (minutePart.size() == 2)
                         ? (minutePart[0] - '0') * 10 + (minutePart[1] - '0')
                         : minutePart[0] - '0';
    if (hour > 23 || mins > 59) {
        return false;
    }
    *minute = hour * 60 + mins;
    return true;
}

std::string formatTimeWindow(int startMinute, int endMinute) {
    return formatMinuteOfDay(startMinute) + "–" + formatMinuteOfDay(endMinute) +
           (isCrossMidnight(startMinute, endMinute) ? "(次日)" : "");
}

void composeReleaseConfigPage(eui::Ui& ui, const eui::Screen& screen,
                              ReleaseStore& store, ReleaseConfigUiState& state) {
    if (state.dirty) {
        state.dirty = false;
        state.loadError.clear();
        const auto services = store.listServices();
        if (services.isOk()) {
            state.services = services.value();
        } else {
            state.loadError = services.errorMessage();
        }
        const auto envs = store.listEnvs();
        if (envs.isOk()) {
            state.envs = envs.value();
        } else if (state.loadError.empty()) {
            state.loadError = envs.errorMessage();
        }
    }

    // ReleaseConfigActions 的回调契约是 Result<void>；store 的 upsert* 要回传
    // 新 id（Result<int64_t>），此处按「成功丢 id、失败原样透传错误码与文案」对齐。
    ReleaseConfigActions actions;
    actions.upsertService = [&store](const ServiceDef& service) -> core::Result<void> {
        const auto result = store.upsertService(service);
        if (result.isFail()) {
            return core::Result<void>::fail(result.errorCode(), result.errorMessage());
        }
        return core::Result<void>::ok();
    };
    actions.deleteService = [&store](std::int64_t id) {
        return store.deleteService(id);
    };
    actions.upsertEnv = [&store](const EnvDef& env) -> core::Result<void> {
        const auto result = store.upsertEnv(env);
        if (result.isFail()) {
            return core::Result<void>::fail(result.errorCode(), result.errorMessage());
        }
        return core::Result<void>::ok();
    };
    actions.deleteEnv = [&store](std::int64_t id) {
        return store.deleteEnv(id);
    };

    drawReleaseConfigPage(ui, screen, state, actions);
}

void drawReleaseConfigPage(eui::Ui& ui, const eui::Screen& screen,
                           ReleaseConfigUiState& state,
                           const ReleaseConfigActions& actions) {
    const Tokens& t = core::theme::current();
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    const bool servicesTab = state.tab == ReleaseConfigUiState::Tab::Services;
    const float contentW = std::max(0.0f, screen.width - kPagePad * 2.0f);
    const float gap = t.sizes.spacingLarge;  // 16
    const float cardH = std::max(
        0.0f, screen.height - kPagePad - kPageHeaderH - gap - kPageHeaderH - gap);

    // 页面底色 = 主窗口 clearColor（theme background #FAFAF9），不自铺不透明底。
    ui.column("release.config.page")
        .size(screen.width, screen.height)
        .padding(kPagePad, 0.0f, kPagePad, kPagePad)  // (left, top, right, bottom)
        .gap(gap)
        .content([&] {
            drawPageHeader(ui, contentW, servicesTab, state, t);
            drawTabs(ui, contentW, state, t);
            drawListCard(ui, contentW, cardH, state, t, componentTheme);
        })
        .build();

    // 弹层必须画在页面根之外：嵌套进 column 会被 padding/流式布局改坐标。
    drawServiceDialog(ui, screen, state, actions, t, componentTheme);
    drawEnvDialog(ui, screen, state, actions, t, componentTheme);
    drawDeleteDialog(ui, screen, state, actions, t, componentTheme);
}

} // namespace module::release