// src/module_settings/project_cards_card.cpp
// 需-11 项目卡设置卡绘制（独立「项目卡」tab）。行序与 projectCardsCardHeightFor
// 逐项对应（改布局两处同步）；间距令牌对齐 github_sync_card（行距 8 / 分段 12 /
// 卡上下留白 16）。表单双列压缩（名称独占一行 + 三行双列）：0..2 张卡在最小窗
//（1000×640 卡区可用高 420）零滚动；列表随卡数增长，超出由页级 scrollView 吸收。
#include "project_cards_card.h"

#include "project_card_store.h"
#include "settings_ui_logic.h"

#include <core/component_theme.h>

#include <components/button.h>
#include <components/input.h>
#include <components/theme.h>

#include <ui_kit/card.h>
#include <ui_kit/form_row.h>

#include <string>
#include <vector>

namespace module::settings {

namespace {

inline constexpr float kPadX = 28.0f;         // 卡左右内边距（github 同步卡同款）
inline constexpr float kPadY = 16.0f;         // 卡上下留白（间距令牌 16）
inline constexpr float kTitleH = 20.0f;       // 标题行高
inline constexpr float kTitleSubGap = 4.0f;   // 标题与说明间距（超小间距令牌 4）
inline constexpr float kSubH = 16.0f;         // 说明行高
inline constexpr float kFormLeadGap = 8.0f;   // 说明与首行表单间距（小间距令牌 8）
inline constexpr float kRowH = 36.0f;         // 表单行高（formRow 同口径）
inline constexpr float kRowGap = 8.0f;        // 表单行距 = 双列列距（github 同款）
inline constexpr float kBtnLeadGap = 8.0f;    // 表单与操作行间距
inline constexpr float kBtnH = 32.0f;         // 按钮高
inline constexpr float kStatusGap = 4.0f;     // 操作行与反馈行间距（超小 4）
inline constexpr float kStatusH = 16.0f;      // 反馈行高（恒占位，防状态抖动）
inline constexpr float kDivLeadGap = 12.0f;   // 反馈行与分段线间距（中间距 12）
inline constexpr float kDivH = 1.0f;          // 分段线
inline constexpr float kListLeadGap = 12.0f;  // 分段线与列表间距（中间距 12）
inline constexpr float kItemH = 28.0f;        // 列表行高
inline constexpr float kItemGap = 4.0f;       // 列表行距（超小 4）
inline constexpr float kEmptyH = 16.0f;       // 空态行高
inline constexpr float kPadBottom = 16.0f;    // 底部留白（内容底 + 16 契约）
inline constexpr float kBtnSaveW = 96.0f;
inline constexpr float kBtnNewW = 72.0f;
inline constexpr float kBtnDelW = 72.0f;
inline constexpr float kBtnGapX = 8.0f;
inline constexpr float kItemPadX = 12.0f;     // 列表行内文本左缩进

// 表单行序（绘制与卡高共用）：名称独占首行，其余六字段双列三行。
struct FieldSpec {
    const char* label;
    const char* placeholder;
    ProjectCardField field;
    bool half;      // false = 独占全宽行；true = 左半列
    bool rightCol;  // half=true 时：false = 左列 / true = 右列
};

const FieldSpec kFields[] = {
    {"名称", "如：订单中心", ProjectCardField::Name, false, false},
    {"仓库路径", "/srv/git/order-center", ProjectCardField::RepoPath, true, false},
    {"启动命令", "make dev", ProjectCardField::StartCommand, true, true},
    {"测试命令", "make test", ProjectCardField::TestCommand, true, false},
    {"部署方式", "k8s 滚动升级 / helm", ProjectCardField::DeployMethod, true, true},
    {"测试库地址", "mysql://user:pwd@host:3306/db", ProjectCardField::TestDbUrl,
     true, false},
    {"关键模块", "order-api / order-worker", ProjectCardField::KeyModules, true,
     true},
};
constexpr int kFieldCount = 7;
constexpr int kFormRowCount = 4;  // 1 全宽 + 3 双列

const std::string& fieldValue(const ProjectCard& c, ProjectCardField f) {
    switch (f) {
        case ProjectCardField::Name: return c.name;
        case ProjectCardField::RepoPath: return c.repoPath;
        case ProjectCardField::StartCommand: return c.startCommand;
        case ProjectCardField::TestCommand: return c.testCommand;
        case ProjectCardField::DeployMethod: return c.deployMethod;
        case ProjectCardField::TestDbUrl: return c.testDbUrl;
        case ProjectCardField::KeyModules: return c.keyModules;
    }
    return c.name;  // 枚举穷举，仅为满足「所有路径有返回」
}

}  // namespace

float projectCardsCardHeightFor(std::size_t cardCount) {
    const float head = roundFs(kPadY) + roundFs(kTitleH) + roundFs(kTitleSubGap) +
                       roundFs(kSubH) + roundFs(kFormLeadGap);
    const float form = static_cast<float>(kFormRowCount) * roundFs(kRowH) +
                       static_cast<float>(kFormRowCount - 1) * roundFs(kRowGap);
    const float ops = roundFs(kBtnLeadGap) + roundFs(kBtnH) +
                      roundFs(kStatusGap) + roundFs(kStatusH);
    const float listLead = roundFs(kDivLeadGap) + roundFs(kDivH) +
                           roundFs(kListLeadGap);
    const float listBody =
        cardCount == 0
            ? roundFs(kEmptyH)
            : static_cast<float>(cardCount) * roundFs(kItemH) +
                  static_cast<float>(cardCount - 1) * roundFs(kItemGap);
    return head + form + ops + listLead + listBody + roundFs(kPadBottom);
}

void drawProjectCardsCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    static const std::vector<ProjectCard> kNoCards;  // in.projectCards 空指针兜底
    const std::vector<ProjectCard>& cards =
        in.projectCards != nullptr ? *in.projectCards : kNoCards;
    const ProjectCard& draft = in.projectCardDraft;

    ui.stack(id)
        .size(w, h)
        .content([&] {
            // 白卡 + Sm 阴影 + 描边（github_sync_card 卡底同款口径）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f,
                                                  0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            float y = roundFs(kPadY);
            ui.text(id + ".title")
                .position(kPadX, y)
                .size(w - kPadX * 2.0f, roundFs(kTitleH))
                .text("项目卡")
                .fontSize(fs(15.0f))
                .lineHeight(roundFs(kTitleH))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            y += roundFs(kTitleH) + roundFs(kTitleSubGap);
            ui.text(id + ".subtitle")
                .position(kPadX, y)
                .size(w - kPadX * 2.0f, roundFs(kSubH))
                .text("每个业务系统一张档案；MCP 客户端经 project_info(name) 按名查询")
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(kSubH))
                .color(t.colors.textTertiary)
                .maxWidth(w - kPadX * 2.0f)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();
            y += roundFs(kSubH) + roundFs(kFormLeadGap);

            // 表单：名称独占一行 + 六字段双列三行（label 定宽 110，列几何与
            // github 分支|Token 双列同式）
            const float formW = w - kPadX * 2.0f;
            const float colGap = roundFs(kRowGap);
            const float colW = (formW - colGap) * 0.5f;
            const float col2X = kPadX + colW + colGap;
            const float rowH = roundFs(kRowH);
            int rowIndex = 0;
            for (int i = 0; i < kFieldCount; ++i) {
                const FieldSpec& f = kFields[i];
                const float x = !f.half ? kPadX : (f.rightCol ? col2X : kPadX);
                const float fw = !f.half ? formW : colW;
                // 双列成对推进行游标：左列不动、右列前进一行
                const float rowY = y + static_cast<float>(rowIndex) *
                                              (rowH + roundFs(kRowGap));
                const std::string rowId =
                    id + ".form." + std::to_string(static_cast<int>(f.field));
                ui_kit::formRow(ui, rowId, f.label, x, rowY, fw, rowH,
                    [&](eui::Ui& ui2, float cx, float cy, float cw, float ch) {
                        auto input = components::input(ui2, rowId + ".input")
                            .position(cx, cy)
                            .size(cw, ch)
                            .placeholder(f.placeholder)
                            .value(fieldValue(draft, f.field))
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([cb = a.setProjectCardField,
                                       field = f.field](const std::string& v) {
                                if (cb) cb(field, v);
                            });
                        // 名称行 Enter = 保存 / Esc = 回新建态（端口草稿行同款
                        // 提交语义）；其余行 Enter 保持默认（不触发）。
                        if (f.field == ProjectCardField::Name) {
                            input.onEnter([save = a.projectCardSave] {
                                if (save) save();
                            }).onCancel([reset = a.projectCardNew] {
                                if (reset) reset();
                            });
                        }
                        input.build();
                    },
                    t);
                if (!f.half || f.rightCol) ++rowIndex;
            }
            y += static_cast<float>(kFormRowCount) * rowH +
                 static_cast<float>(kFormRowCount - 1) * roundFs(kRowGap);

            // 操作行：三按钮右锚卡内容右缘（github 操作组同构；删除仅选中已有卡可用）
            y += roundFs(kBtnLeadGap);
            const float btnH = roundFs(kBtnH);
            float bx = w - kPadX;
            components::button(ui, id + ".btn.delete")
                .theme(compTheme, false)
                .position(bx - kBtnDelW, y)
                .size(kBtnDelW, btnH)
                .text("删除")
                .fontSize(fs(13.0f))
                .disabled(!in.projectCardEditingExisting)
                .onClick([del = a.projectCardDelete] {
                    if (del) del();
                })
                .build();
            bx -= kBtnDelW + kBtnGapX;
            components::button(ui, id + ".btn.new")
                .theme(compTheme, false)
                .position(bx - kBtnNewW, y)
                .size(kBtnNewW, btnH)
                .text("新建")
                .fontSize(fs(13.0f))
                .onClick([reset = a.projectCardNew] {
                    if (reset) reset();
                })
                .build();
            bx -= kBtnNewW + kBtnGapX;
            components::button(ui, id + ".btn.save")
                .theme(compTheme, true)
                .position(bx - kBtnSaveW, y)
                .size(kBtnSaveW, btnH)
                .text("保存")
                .fontSize(fs(13.0f))
                .onClick([save = a.projectCardSave] {
                    if (save) save();
                })
                .build();
            y += btnH + roundFs(kStatusGap);

            // 反馈行：恒占位（成功灰 / 失败红，github_sync_error 行同口径）
            ui.text(id + ".status")
                .position(kPadX, y)
                .size(w - kPadX * 2.0f, roundFs(kStatusH))
                .text(in.projectCardStatus)
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(kStatusH))
                .color(in.projectCardStatusError ? t.colors.errorText
                                                 : t.colors.textTertiary)
                .maxWidth(w - kPadX * 2.0f)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();
            y += roundFs(kStatusH) + roundFs(kDivLeadGap);

            // 分段线（表单区与已登记列表分节）
            ui.rect(id + ".div")
                .position(kPadX, y)
                .size(formW, roundFs(kDivH))
                .color(t.colors.borderDefault)
                .build();
            y += roundFs(kDivH) + roundFs(kListLeadGap);

            if (cards.empty()) {
                ui.text(id + ".empty")
                    .position(kPadX, y)
                    .size(w - kPadX * 2.0f, roundFs(kEmptyH))
                    .text("暂无项目卡：填写名称后点「保存」登记第一张")
                    .fontSize(fs(12.0f))
                    .lineHeight(roundFs(kEmptyH))
                    .color(t.colors.textTertiary)
                    .build();
                return;  // 游标到头：卡高由 projectCardsCardHeightFor(0) 收口
            }

            // 已登记列表：名称（选中高亮）+ 仓库路径（muted 尾省略）；整行点选装入表单
            const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
            const float itemH = roundFs(kItemH);
            const float textY = (itemH - 16.0f) * 0.5f;
            for (std::size_t i = 0; i < cards.size(); ++i) {
                const ProjectCard& c = cards[i];
                const std::string rowId = id + ".item." + std::to_string(i);
                const bool selected =
                    in.projectCardEditingExisting &&
                    c.name == in.projectCardSelectedName;
                const float nameW = std::max(60.0f, formW * 0.4f);
                const float repoX = kItemPadX + nameW + 8.0f;
                const float repoW =
                    std::max(0.0f, formW - kItemPadX - nameW - 8.0f - kItemPadX);
                ui.stack(rowId)
                    .size(formW, itemH)
                    .content([&] {
                        ui.rect(rowId + ".bg")
                            .size(formW, itemH)
                            .color(selected ? t.colors.primarySoftBg
                                            : t.colors.background)
                            .radius(t.sizes.radius)
                            .build();
                        ui.text(rowId + ".name")
                            .position(kItemPadX, textY)
                            .size(nameW, 16.0f)
                            .text(c.name)
                            .fontSize(fs(13.0f))
                            .fontWeight(selected ? 600 : 400)
                            .color(selected ? t.colors.primaryText
                                            : t.colors.textPrimary)
                            .maxWidth(nameW)
                            .wrap(false)
                            .overflow(core::TextOverflow::Ellipsis)
                            .build();
                        ui.text(rowId + ".repo")
                            .position(repoX, textY + 1.0f)
                            .size(repoW, 16.0f)
                            .text(c.repoPath.empty() ? "（仓库路径未填写）"
                                                     : c.repoPath)
                            .fontSize(fs(12.0f))
                            .color(t.colors.textTertiary)
                            .maxWidth(repoW)
                            .wrap(false)
                            .overflow(core::TextOverflow::Ellipsis)
                            .build();
                        ui.rect(rowId + ".hit")
                            .size(formW, itemH)
                            .states(kTransparent, kTransparent, kTransparent)
                            .onClick([sel = a.projectCardSelect,
                                      name = c.name] {
                                if (sel) sel(name);
                            })
                            .build();
                    })
                    .build();
                y += itemH + roundFs(kItemGap);
            }
        })
        .build();
}

}  // namespace module::settings
