// 表单弹窗（三页）+ 删除确认弹窗 + 表单状态机；状态经 FormState 引用显式入参。
#include "claude_ui_form.h"

#include "claude_ui_logic.h"
#include "env_var_hints.h"
#include "platform/credential_protection.h"
#include "provider_config.h"

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/tabs.h>
#include <components/tooltip.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/wtab.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace module::claude {

namespace {

// 认证字段下拉选项（顺序必须与 AuthField 枚举一致：Both/Bearer/ApiKey）
constexpr const char* kAuthFieldLabels[] = {
    "双发（默认）", "只发 Authorization: Bearer", "只发 x-api-key"};
constexpr int kAuthFieldCount = 3;

// formError_ 统一写入点：全部写入点必须走这两个 helper。
void setFormError(FormState& f, const std::string& msg) {
    f.formError_ = msg;
}

void clearFormError(FormState& f) {
    f.formError_.clear();
}

} // namespace

void openCreateForm(FormState& f) {
    f.formEditId_ = 0;   // 0 = 新建
    f.formTab_ = 0;
    f.formName_.clear();
    f.formUrl_.clear();
    f.formKey_.clear();
    clearFormError(f);
    f.formKeyHint_.clear();
    f.formModels_ = ModelsConfig{};
    f.formEnvRows_.clear();
    f.formAuthField_ = AuthField::Both;
    f.formAuthFieldOpen_ = false;
    f.formOpen_ = true;
}

void openEditForm(FormState& f, const Profile& p) {
    f.formEditId_ = p.id;
    f.formTab_ = 0;
    f.formName_ = p.name;
    f.formUrl_ = p.baseUrl;
    f.formKey_.clear();   // 留空 = 不换 key（updateProfile 语义）
    clearFormError(f);
    f.formKeyHint_ = p.apiKeyHint;
    // 槽位：models_config（空 / 非法 JSON → 全空槽位）
    f.formModels_ = effectiveModelsConfig(p.modelsConfigJson);
    // 认证字段：编辑必须回显现状，否则用户误以为是默认值、一保存即覆盖
    f.formAuthField_ = authFieldFromText(p.authField);
    f.formAuthFieldOpen_ = false;
    // env 行：非法 JSON 读路径容错为空表
    f.formEnvRows_.clear();
    auto ee = parseExtraEnv(p.extraEnvJson);
    if (ee.isOk()) {
        for (const auto& [k, v] : ee.value()) {
            f.formEnvRows_.emplace_back(k, v);
        }
    }
    f.formOpen_ = true;
}


// 表单 → models_config / extra_env JSON（全空 = 空串，保持「未配置」语义）
bool serializeFormData(FormState& f, std::string* modelsJson, std::string* envJson) {
    bool anySlot = false;
    for (const auto& meta : modelSlotTable()) {   // 槽位表派生
        const ModelSlot& s = f.formModels_.*meta.slot;
        if (!s.model.empty() || !s.name.empty()) { anySlot = true; break; }
    }
    if (anySlot) {
        // 保存前归一（就地改写）：支持 1M 的规范成 [1M]，不支持的强制剥离，其余原样
        normalizeOneMMarkers(f.formModels_);
        auto r = serializeModelsConfig(f.formModels_);
        if (r.isFail()) {
            setFormError(f, "模型配置序列化失败: " + r.errorMessage());
            return false;
        }
        *modelsJson = r.value();
    }
    std::map<std::string, std::string> envMap;   // 空键行跳过
    for (const auto& row : f.formEnvRows_) {
        if (row.first.empty()) continue;
        envMap[row.first] = row.second;   // 同名后行胜（map 语义）
    }
    if (!envMap.empty()) {
        auto r = serializeExtraEnv(envMap);
        if (r.isFail()) {
            setFormError(f, "env 序列化失败: " + r.errorMessage());
            return false;
        }
        *envJson = r.value();
    }
    return true;
}

void submitForm(FormState& f, const FormSubmitHooks& hooks) {
    ClaudeService* svc = ClaudeService::global();
    if (!svc) {
        setFormError(f, "服务未就绪（ClaudeService 未注册）");
        return;
    }
    hooks.markDataDirty();   // 写操作前即标脏（失败也不伤，下帧重查）
    if (f.formName_.empty() || f.formUrl_.empty()) {
        setFormError(f, "名称和 Base URL 不能为空");
        return;
    }
    std::string modelsJson;
    std::string envJson;
    if (!serializeFormData(f, &modelsJson, &envJson)) return;

    if (f.formEditId_ == 0) {
        if (f.formKey_.empty()) {
            setFormError(f, "新建 Profile 必须填写 API Key");
            return;
        }
        // activateIfNoneActive=true：无激活 Profile 时自动激活这条；已有激活不抢占
        auto r = svc->createProfile(f.formName_, f.formUrl_, f.formKey_,
                                    f.formModels_.def.model, /*activateIfNoneActive=*/true,
                                    authFieldToText(f.formAuthField_));
        if (r.isFail()) {
            setFormError(f, r.errorMessage());
            return;
        }
        // createProfile 无槽位/env 参数，二段补写
        if (!modelsJson.empty() || !envJson.empty()) {
            Profile p = r.value();
            p.model = f.formModels_.def.model;
            p.modelsConfigJson = modelsJson;
            p.extraEnvJson = envJson;
            auto u = svc->updateProfile(p);
            if (u.isFail()) {
                setFormError(f, "已创建，但模型/env 写入失败: " +
                                  u.errorMessage());
                return;
            }
        }
        f.formOpen_ = false;
        hooks.selectProfile(r.value().id);
        // 按是否自动激活分文案（只说「已保存」会让用户以为点「启动代理」就能用）
        if (r.value().isActive) {
            hooks.toast("已创建并启用",
                        "Profile「" + f.formName_ + "」已设为激活（ACTIVE）",
                        false);
        } else {
            hooks.toast("已创建",
                        "Profile「" + f.formName_ +
                            "」已保存，点击「启用」使其生效",
                        false);
        }
    } else {
        // 编辑：先取现有行（保留凭据/片段绑定），再覆盖表单值
        auto got = svc->getProfile(f.formEditId_);
        if (got.isFail()) {
            setFormError(f, got.errorMessage());
            return;
        }
        Profile p = std::move(got).value();
        p.name = f.formName_;
        p.baseUrl = f.formUrl_;
        p.model = f.formModels_.def.model;   // model 列与 default 槽位同步
        p.modelsConfigJson = modelsJson;
        p.extraEnvJson = envJson;
        p.apiKeyPlain = f.formKey_;   // 空 = 不换 key
        p.authField = authFieldToText(f.formAuthField_);
        auto r = svc->updateProfile(p);
        if (r.isFail()) {
            setFormError(f, r.errorMessage());
            return;
        }
        f.formOpen_ = false;
        hooks.toast("已保存", "Profile「" + f.formName_ + "」已更新", false);
    }
}

namespace {

void drawFormBasicTab(eui::Ui& ui, float w,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens,
                      bool isEdit, FormState& f) {
    // UAT5：还原「名称」「认证字段」各占一整行全宽纵排（UAT3 双列 344 被用户推翻）；
    // 行节奏沿用 cols2 前布局（label 18 + 输入 40，段距 12/8），弹高四页统一 600。
    ui.text("claude.form.basic.name.label")
        .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
        .text("名称").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.dialog.form.name")
        .position(28.0f, 122.0f).size(w - 56.0f, 40.0f)
        .value(f.formName_).placeholder("例如：官方 / 中转 A")
        .fontSize(core::theme::scaled(14.0f)).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formName_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消（关表单不提交，各输入框统一）
        .build();

    ui.text("claude.form.basic.url.label")
        .position(28.0f, 174.0f).size(w - 56.0f, 16.0f)
        .text("Base URL").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.dialog.form.url")
        .position(28.0f, 192.0f).size(w - 56.0f, 40.0f)
        .value(f.formUrl_).placeholder("https://api.anthropic.com")
        .fontSize(core::theme::scaled(14.0f)).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formUrl_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
        .build();

    ui.text("claude.form.basic.key.label")
        .position(28.0f, 244.0f).size(w - 56.0f, 16.0f)
        .text("API Key（" + std::string(platform::credential::protectionDescription()) + "）")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    const std::string keyPlaceholder = isEdit
        ? ("留空 = 保持现有 Key（" + f.formKeyHint_ + "）")
        : std::string("sk-ant-...");
    components::input(ui, "claude.dialog.form.key")
        .position(28.0f, 262.0f).size(w - 56.0f, 40.0f)
        .value(f.formKey_).placeholder(keyPlaceholder)
        .fontSize(core::theme::scaled(14.0f)).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formKey_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
        .build();

    // 认证字段（鉴权头三态）还原独占一行：标签与灰字说明同行，下拉紧随全宽
    ui.text("claude.form.basic.authfield.label")
        .position(28.0f, 314.0f).size(72.0f, 16.0f)
        .text("认证字段").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.basic.authfield.hint")
        .position(104.0f, 314.0f).size(w - 132.0f, 16.0f)
        .text("网关只接受一种鉴权头时切换；默认两种都发")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    ui.stack("claude.form.basic.authfield.ddwrap")
        .position(28.0f, 332.0f)
        .size(w - 56.0f, 40.0f)
        .content([&] {
            components::dropdown(ui, "claude.dialog.form.authfield.dd")
                .size(w - 56.0f, 40.0f)
                .theme(themeTokens)
                .items(std::vector<std::string>(
                    kAuthFieldLabels, kAuthFieldLabels + kAuthFieldCount))
                .selected(static_cast<int>(f.formAuthField_))
                .open(f.formAuthFieldOpen_)
                .onOpenChange([fp = &f](bool v) { fp->formAuthFieldOpen_ = v; })
                .onChange([fp = &f](int idx) {
                    fp->formAuthField_ = static_cast<AuthField>(
                        std::clamp(idx, 0, kAuthFieldCount - 1));
                })
                .build();
        })
        .build();

    // 行节奏沿用原布局：hint 380 / 错误 404 / 接管警告 426（弹高 600 下按钮带 472 起，呼吸区充足）
    ui.text("claude.form.basic.hint.icon")
        .position(28.0f, 380.0f).size(14.0f, 18.0f)
        .icon(0xF05A).fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(18.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.basic.hint")
        .position(46.0f, 380.0f).size(w - 74.0f, 18.0f)
        .text("模型配置和环境变量在另外两个标签页配置")
        .fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(18.0f))
        .color(t.colors.textTertiary).build();

    if (!f.formError_.empty()) {
        ui.text("claude.dialog.form.error")
            .position(28.0f, 404.0f).size(w - 56.0f, 18.0f)
            .text(f.formError_).fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(18.0f))
            .color(t.colors.errorText).build();
    }
}

void drawFormModelsTab(eui::Ui& ui, float w,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       FormState& f) {
    const auto slotTable = modelSlotTable();

    ui.text("claude.form.slots.h.slot")
        .position(28.0f, 104.0f).size(110.0f, 14.0f)
        .text("槽位").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.h.model")
        .position(150.0f, 104.0f).size(330.0f, 14.0f)
        .text("上游模型名（发往上游）").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.h.name")
        .position(492.0f, 104.0f).size(240.0f, 14.0f)
        .text("显示名（CC 界面）").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .color(t.colors.textTertiary).build();

    for (size_t i = 0; i < slotTable.size(); ++i) {
        const float y = 126.0f + static_cast<float>(i) * 52.0f;
        const auto& row = slotTable[i];
        const ModelSlot& slot = f.formModels_.*row.slot;
        const std::string idx = std::to_string(i);
        ui.text("claude.form.slot.label." + idx)
            .position(28.0f, y + 10.0f).size(110.0f, 16.0f)
            .text(row.formLabel).fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(16.0f))
            .color(t.colors.textSecondary).build();
        components::input(ui, "claude.form.slot." + idx + ".model")
            .position(150.0f, y).size(330.0f, 36.0f)
            .value(slot.model).placeholder("fangzhou/glm-5.3[1M]")
            .fontSize(core::theme::scaled(13.0f)).theme(themeTokens)
            .onChange([fp = &f, m = row.slot](const std::string& v) {
                (fp->formModels_.*m).model = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
        // 显示名留空 → placeholder 显示派生预览
        const std::string derived = deriveDisplayName(slot.model);
        components::input(ui, "claude.form.slot." + idx + ".name")
            .position(492.0f, y).size(240.0f, 36.0f)
            .value(slot.name)
            .placeholder(derived.empty() ? "（自动派生）" : derived)
            .fontSize(core::theme::scaled(13.0f)).theme(themeTokens)
            .onChange([fp = &f, m = row.slot](const std::string& v) {
                (fp->formModels_.*m).name = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
    }

    ui.text("claude.form.slots.hint1.icon")
        .position(28.0f, 446.0f).size(14.0f, 16.0f)
        .icon(0xF05A).fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.hint1")
        .position(46.0f, 446.0f).size(w - 74.0f, 16.0f)
        .text("显示名留空时自动剥除模型名的 [1M]/[200K] 等上下文标记")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.hint2")
        .position(28.0f, 464.0f).size(w - 56.0f, 16.0f)
        .text("    槽位留空 = 不写入该 env 键，Claude Code 用内置映射")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();
}

// 环境变量页（键值编辑器）；可见行数上限 5（弹窗高度约束），超出部分仍在状态里全量保存
void drawFormEnvTab(eui::Ui& ui, float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& themeTokens,
                    FormState& f) {
    constexpr size_t kMaxVisibleRows = 5;

    ui.text("claude.form.env.label")
        .position(28.0f, 104.0f).size(348.0f, 16.0f)
        .text("额外环境变量（原样写入 settings.json env 块）")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary).build();

    const size_t visible =
        f.formEnvRows_.size() < kMaxVisibleRows ? f.formEnvRows_.size()
                                                : kMaxVisibleRows;
    for (size_t i = 0; i < visible; ++i) {
        const float y = 124.0f + static_cast<float>(i) * 38.0f;
        const std::string idx = std::to_string(i);
        components::input(ui, "claude.form.env.row." + idx + ".key")
            .position(28.0f, y).size(220.0f, 40.0f)
            .value(f.formEnvRows_[i].first).placeholder("ENV_KEY")
            .fontSize(core::theme::scaled(12.0f)).theme(themeTokens)
            .onChange([fp = &f, i](const std::string& v) {
                fp->formEnvRows_[i].first = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
        // 变量名识别：已知→hover 说明 tooltip；未知且非空→红边框（仍可导入，解析侧已放开键名校验）
        if (const char* hint = envVarHint(f.formEnvRows_[i].first)) {
            components::tooltip(ui, "claude.form.env.row." + idx + ".tip")
                .source("claude.form.env.row." + idx + ".key.hit")
                .value(hint)
                .anchor(138.0f, y + 16.0f)
                .bounds(w, 420.0f)
                .theme(themeTokens)
                .zIndex(60)
                .build();
        } else if (!f.formEnvRows_[i].first.empty()) {
            ui.rect("claude.form.env.row." + idx + ".key.unknown")
                .position(28.0f, y).size(220.0f, 40.0f)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .border(1.0f, t.colors.error)
                .radius(t.sizes.radiusSmall)
                .build();
        }
        components::input(ui, "claude.form.env.row." + idx + ".value")
            .position(258.0f, y).size(414.0f, 40.0f)
            .value(f.formEnvRows_[i].second).placeholder("值（字符串）")
            .fontSize(core::theme::scaled(12.0f)).theme(themeTokens)
            .onChange([fp = &f, i](const std::string& v) {
                fp->formEnvRows_[i].second = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
        // S2.1 Danger Ghost：浅红底（error 20%）+ 红字，无实心红块；hover 红底白字由
        // ButtonVariant::Danger 默认实现（EUI-NEO button.h）。
        components::button(ui, "claude.form.env.row." + idx + ".del",
                           components::ButtonVariant::Danger)
            .position(700.0f, y).size(32.0f, 40.0f)
            .icon(0xF00D)  // fa-xmark
            .fontSize(core::theme::scaled(13.0f))
            .theme(themeTokens)
            // UAT6 阴影统一：ButtonStyle 默认已无阴影（EUI-NEO 根修），此处不再
            // 逐钮覆盖透明阴影（原 workaround 已被根修吸收）。
            .onClick([fp = &f, i] {
                if (i < fp->formEnvRows_.size()) {
                    fp->formEnvRows_.erase(fp->formEnvRows_.begin() +
                                           static_cast<long>(i));
                }
            })
            .build();
    }

    // S3.9 #4：「+ 添加变量」改 Outline 次按钮 32px 高（白底绿边绿字，hover 淡绿底）
    components::button(ui, "claude.form.env.add", components::ButtonVariant::Outline)
        .position(28.0f, 314.0f).size(120.0f, 32.0f)
        .text("+ 添加变量").fontSize(core::theme::scaled(12.0f))
        .theme(themeTokens)
        .onClick([fp = &f] { fp->formEnvRows_.emplace_back(); })
        .build();
    if (f.formEnvRows_.size() > kMaxVisibleRows) {
        // S3.9 #6：前置 ℹ 图标（14px 信息蓝）+ 12px 三级灰字（原 11px 过浅）
        ui.text("claude.form.env.overflow.icon")
            .position(150.0f, 322.0f).size(14.0f, 16.0f)
            .icon(0xF05A).fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(16.0f))
            .color(t.colors.info).build();
        ui.text("claude.form.env.overflow")
            .position(168.0f, 322.0f).size(w - 168.0f - 28.0f, 32.0f)
            .text("另有 " + std::to_string(f.formEnvRows_.size() -
                                           kMaxVisibleRows) +
                  " 项已保存（保存时全量写入，列表仅显示前 " +
                  std::to_string(kMaxVisibleRows) + " 项）")
            .fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(16.0f))
            .color(t.colors.textTertiary).build();
    }
}

} // namespace

void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           FormState& f,
                           const FormSubmitHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    // UAT6 O 批：最小窗 1000×640（页高 600）下弹窗 600 恒高填满全页、按钮出框；
// 540 档（600−60 余量）使弹窗在最小窗下不溢出、仍有呼吸区。三 tab 高度仍统一。
const float h = 540.0f;
    const bool isEdit = (f.formEditId_ != 0);

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.form";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = f.formOpen_;
    frame.onOpenChange = [fp = &f](bool v) { fp->formOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(
                ui, "claude.dialog.form.title", w,
                isEdit ? "编辑 Profile" : "新建 Profile", 20.0f, t.colors.textPrimary,
                {.y = 16.0f, .height = 28.0f, .lineHeight = 28.0f});

            // S3.10 弹窗级小 Tab：块面式 32px/12px/13px（淘汰下划线式 components::tabs）
            {
                const char* kTabLabels[] = {"基础信息", "模型配置", "环境变量"};
                const float kTabW[] = {88.0f, 88.0f, 88.0f};
                constexpr float kTabH = 32.0f;
                constexpr float kTabGap = 4.0f;
                constexpr float kTabY = 52.0f;
                float xPos = 28.0f;
                for (int i = 0; i < 3; ++i) {
                    ui_kit::wtab(ui, "claude.form.tabs." + std::to_string(i),
                                 kTabLabels[i], f.formTab_ == i, xPos, kTabY,
                                 kTabW[i], kTabH, t,
                                 [fp = &f, idx = i] { fp->formTab_ = idx; },
                                 ui_kit::kWtabSmallFontSize);
                    xPos += kTabW[i] + kTabGap;
                }
            }

            if (f.formTab_ == 0) {
                drawFormBasicTab(ui, w, t, themeTokens, isEdit, f);
            } else if (f.formTab_ == 1) {
                drawFormModelsTab(ui, w, t, themeTokens, f);
            } else {
                drawFormEnvTab(ui, w, t, themeTokens, f, imp);
            }

            // 错误信息（基础页之外也要看得到校验错误）
            if (f.formTab_ != 0) {
                ui_kit::drawDialogText(ui, "claude.dialog.form.error", w, f.formError_,
                                       12.0f, t.colors.errorText,
                                       {.y = h - 104.0f, .height = 18.0f,
                                        .lineHeight = 18.0f, .skipIfEmpty = true});
            }

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = 14.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [fp = &f] { fp->formOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = isEdit ? "保存" : "创建";
            confirm.fontSize = 14.0f;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [fp = &f, hooks] { submitForm(*fp, hooks); };
            ui_kit::drawDialogButtons(ui, "claude.dialog.form", w, h, cancel, confirm, t,
                                      themeTokens);
    });
}

void drawDeleteConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& themeTokens,
                             FormState& f,
                             const std::function<void(int64_t id)>& onConfirmDelete) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    const float h = 190.0f;

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.delete";
    frame.width = ui_kit::DialogWidth::Compact;
    frame.height = h;
    frame.open = f.deleteConfirmOpen_;
    frame.onOpenChange = [fp = &f](bool v) { fp->deleteConfirmOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "claude.dialog.delete.title", w, "确认删除？",
                                    20.0f, t.colors.textPrimary);
            // 文本 wrap + 高 60：长 Profile 名换行，避免 EUI 文本不裁剪被画出右缘
            ui_kit::drawDialogText(
                ui, "claude.dialog.delete.msg", w,
                "将删除 Profile「" + f.deleteTargetName_ +
                    "」及其保存的 API Key，不可恢复。",
                t.sizes.fontSizeSmall, t.colors.textSecondary,
                {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f,
                 .wrap = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = 14.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [fp = &f] { fp->deleteConfirmOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "确认删除";
            confirm.fontSize = 14.0f;
            confirm.style = ui_kit::DialogButtonStyle::DangerSoftThemed;
            confirm.onClick = [fp = &f, onConfirmDelete] {
                const int64_t id = fp->deleteTargetId_;
                fp->deleteConfirmOpen_ = false;
                onConfirmDelete(id);
            };
            ui_kit::drawDialogButtons(ui, "claude.dialog.delete", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

} // namespace module::claude
