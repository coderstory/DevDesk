// 表单弹窗（tabs 四页）+ 删除/片段确认弹窗 + 表单状态机；状态经 FormState/ImportState 引用显式入参。
#include "core/fs_text.h"
#include "claude_ui_form.h"

#include "claude_ui_logic.h"
#include "env_var_hints.h"
#include "platform/credential_protection.h"
#include "platform/json.h"
#include "provider_config.h"
#include "settings_sync.h"
#include "upstream_dialect.h"

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/tabs.h>
#include <components/tooltip.h>
#include <ui_kit/dialog_form.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace module::claude {

namespace {

// 认证字段下拉选项（顺序必须与 AuthField 枚举一致：Both/Bearer/ApiKey）
constexpr const char* kAuthFieldLabels[] = {
    "双发（默认）", "只发 Authorization: Bearer", "只发 x-api-key"};
constexpr int kAuthFieldCount = 3;

// formError_ 统一写入点：多来源红字，fromJson 标记使「放弃修改」只清 JSON 源那条；
// 全部写入点必须走这两个 helper（直接赋值会留过期标记）。
void setFormError(FormState& f, const std::string& msg, bool fromJson = false) {
    f.formError_ = msg;
    f.formErrorFromJson_ = fromJson;
}

void clearFormError(FormState& f) {
    f.formError_.clear();
    f.formErrorFromJson_ = false;
}

// 「格式化」：parse→pretty stringify（与 settings_sync 同一序列化通道）；失败写行内红字。
void formatAdvancedJson(FormState& f) {
    f.formAdvancedJsonError_.clear();
    if (f.formExtraSettingsJson_.empty()) return;
    auto parsed = platform::json::parse(f.formExtraSettingsJson_);
    if (parsed.isFail()) {
        f.formAdvancedJsonError_ = "解析失败: " + parsed.errorMessage();
        return;
    }
    auto pretty = platform::json::stringify(parsed.value(),
                                            /*pretty=*/true);
    if (pretty.isFail()) {
        f.formAdvancedJsonError_ = "序列化失败: " + pretty.errorMessage();
        return;
    }
    f.formExtraSettingsJson_ = pretty.value();
}

} // namespace

// 全屏 JSON「放弃修改」（按钮/Esc/点遮罩）：恢复 formExtraSettingsJsonBaseline_（落库值）。
void discardJsonFullscreenEdits(FormState& f) {
    f.formExtraSettingsJson_ = f.formExtraSettingsJsonBaseline_;
    // JSON 行内红字随文本回退一并清
    f.formAdvancedJsonError_.clear();
    // 按来源标记清 JSON 校验遗留的幽灵 formError_（其它来源保留）
    if (f.formErrorFromJson_) {
        clearFormError(f);
    }
    f.formJsonFullscreen_ = false;
}

void openCreateForm(FormState& f, ImportState& imp) {
    f.formEditId_ = 0;   // 0 = 新建
    f.formTab_ = 0;
    f.formName_.clear();
    f.formUrl_.clear();
    f.formKey_.clear();
    clearFormError(f);
    f.formKeyHint_.clear();
    f.formImportWarn_.clear();
    f.formModels_ = ModelsConfig{};
    f.formEnvRows_.clear();
    f.formAuthField_ = AuthField::Both;
    f.formAuthFieldOpen_ = false;
    f.formSharedConfigId_ = 0;
    f.formSharedDdOpen_ = false;
    // 基线 = 新建的「上次落库值」= 空
    f.formExtraSettingsJson_.clear();
    f.formExtraSettingsJsonBaseline_.clear();
    f.formAdvancedJsonError_.clear();
    f.formJsonFullscreen_ = false;   // 全屏编辑态复位
    f.formFallbackProfileId_ = 0;
    f.formFallbackDdOpen_ = false;
    f.formDialectOverride_.clear();
    f.formDialectDdOpen_ = false;
    resetImportState(imp);
    f.formOpen_ = true;
}

void openEditForm(FormState& f, ImportState& imp, const Profile& p,
                  const std::vector<SharedConfig>& sharedConfigs) {
    f.formEditId_ = p.id;
    f.formTab_ = 0;
    f.formName_ = p.name;
    f.formUrl_ = p.baseUrl;
    f.formKey_.clear();   // 留空 = 不换 key（updateProfile 语义）
    clearFormError(f);
    f.formKeyHint_ = p.apiKeyHint;
    f.formImportWarn_.clear();
    // 槽位：models_config（空 / 非法 JSON → 全空槽位）
    f.formModels_ = effectiveModelsConfig(p.modelsConfigJson);
    // 认证字段：编辑必须回显现状，否则用户误以为是默认值、一保存即覆盖
    f.formAuthField_ = authFieldFromText(p.authField);
    f.formAuthFieldOpen_ = false;
    // 绑定回显（悬空由下拉逐帧归一为未绑定）
    f.formSharedConfigId_ = p.sharedConfigId;
    f.formSharedDdOpen_ = false;
    // 预填 DB 值；基线 = 同一份（「放弃修改」恢复目标）
    f.formExtraSettingsJson_ = p.extraSettingsJson;
    f.formExtraSettingsJsonBaseline_ = p.extraSettingsJson;
    f.formAdvancedJsonError_.clear();
    f.formJsonFullscreen_ = false;   // 全屏编辑态复位
    // 备用 Profile 回显（悬空/自引用 → 下拉逐帧归一为「无」）
    f.formFallbackProfileId_ = p.fallbackProfileId;
    f.formFallbackDdOpen_ = false;
    // 高级配置页回显（方言覆盖；空/未知 → 下拉逐帧归一为「自动」）
    f.formDialectOverride_ = p.dialectOverride;
    f.formDialectDdOpen_ = false;
    // env 行：非法 JSON 读路径容错为空表
    f.formEnvRows_.clear();
    auto ee = parseExtraEnv(p.extraEnvJson);
    if (ee.isOk()) {
        for (const auto& [k, v] : ee.value()) {
            f.formEnvRows_.emplace_back(k, v);
        }
    }
    resetImportState(imp);
    f.formOpen_ = true;
}

void resetImportState(ImportState& imp) {
    imp.importText_.clear();
    imp.importReady_ = false;
    imp.importError_.clear();
    imp.importPreview_ = ImportedConfig{};
}

// 导入解析：失败只写红字，不动表单
void doParseImport(ImportState& imp) {
    imp.importError_.clear();
    imp.importReady_ = false;
    auto r = parseImportedEnv(imp.importText_);
    if (r.isFail()) {
        imp.importError_ = r.errorMessage();
        return;
    }
    imp.importPreview_ = std::move(r).value();
    imp.importReady_ = true;
}

// 确认导入：按解析结果填三页表单（BASE_URL/token → 基础页，槽位非空覆盖，env 同名替换/新键追加）
void doApplyImport(FormState& f, ImportState& imp) {
    if (!imp.importReady_) return;
    if (!imp.importPreview_.baseUrl.empty()) f.formUrl_ = imp.importPreview_.baseUrl;
    if (!imp.importPreview_.authToken.empty()) {
        f.formKey_ = imp.importPreview_.authToken;
    }
    for (const auto& meta : modelSlotTable()) {
        const ModelSlot& src = imp.importPreview_.models.*meta.slot;
        if (!src.model.empty()) f.formModels_.*meta.slot = src;
    }
    for (const auto& [k, v] : imp.importPreview_.extraEnv) {
        bool replaced = false;
        for (auto& row : f.formEnvRows_) {
            if (row.first == k) {
                row.second = v;
                replaced = true;
                break;
            }
        }
        if (!replaced) f.formEnvRows_.emplace_back(k, v);
    }
    // 认证字段：按键名证据填（空串 = 无证据，不覆盖表单现值）
    if (!imp.importPreview_.authFieldFromEnv.empty()) {
        f.formAuthField_ = authFieldFromText(imp.importPreview_.authFieldFromEnv);
    }
    imp.importReady_ = false;
    imp.importText_.clear();
    clearFormError(f);   // 导入成功清掉旧校验错误
}

// 全自动「粘贴导入」：读用户级 settings.json → parseImportedEnv → 新建弹窗预填；接管态导入的是代理托管值，仅警告不阻断。
void startImportCurrentConfig(FormState& f, ImportState& imp,
                              const FormSubmitHooks& hooks) {
    // 路径解析：service 注入优先（测试=临时目录），未注入回落 defaultPaths()
    settings_sync::Paths paths = settings_sync::defaultPaths();
    ClaudeService* svc = ClaudeService::global();
    if (svc && !svc->settingsSyncPaths().settingsFile.empty()) {
        paths = svc->settingsSyncPaths();
    }

    std::error_code ec;
    if (!std::filesystem::exists(paths.settingsFile, ec) || ec) {
        hooks.toast("导入失败",
                    "未找到 ~/.claude/settings.json（" +
                        paths.settingsFile.string() + "）",
                    true);
        return;
    }
    const std::string content = core::readTextFile(paths.settingsFile);
    // 无 env 键 = 无可导入配置（给明确提示，避免误报「键名非法」）
    if (auto doc = platform::json::parse(content);
        doc.isOk() && doc.value().isObject() &&
        !doc.value().asObject().has("env")) {
        hooks.toast("导入失败",
                    "settings.json 没有 env 块，无配置可导入", true);
        return;
    }
    auto parsed = parseImportedEnv(content);
    if (parsed.isFail()) {
        hooks.toast("导入失败",
                    "settings.json 解析失败: " + parsed.errorMessage(), true);
        return;
    }

    openCreateForm(f, imp);
    imp.importPreview_ = std::move(parsed).value();
    imp.importReady_ = true;
    doApplyImport(f, imp);       // 预填三页表单（结束时清 importReady_）
    imp.importReady_ = true;   // 保留高级 env 页预览标记（识别数 + 警告角标）
    f.formName_ = suggestImportName(imp.importPreview_.baseUrl);

    // 非 env 的顶层属性 parseImportedEnv/doApplyImport 都不管，必须在此独立抽取为「高级设置 JSON」
    f.formExtraSettingsJson_ = nonEnvTopLevelJson(content);
    f.formExtraSettingsJsonBaseline_ = f.formExtraSettingsJson_;

    // 接管态警告：sidecar 存在 → 导入的是代理配置而非原始上游
    if (settings_sync::hasPendingSnapshot(paths)) {
        f.formImportWarn_ =
            "当前是接管态，导入的是代理配置而非原始上游"
            "（BASE_URL=127.0.0.1:8888）；建议先停止代理再导入";
    }
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

void submitForm(FormState& f, const FormSubmitHooks& hooks,
                const std::vector<SharedConfig>& sharedConfigs) {
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
    // 悬空绑定防御（片段已删 / 快照未到位）：按未绑定保存，不留幽灵 id
    if (f.formSharedConfigId_ != 0 &&
        findSharedById(sharedConfigs, f.formSharedConfigId_) == nullptr) {
        f.formSharedConfigId_ = 0;
    }
    std::string modelsJson;
    std::string envJson;
    if (!serializeFormData(f, &modelsJson, &envJson)) return;

    // 保存前校验：失败→formError_+行内红字（不关弹窗不落库）；通过→统一 pretty 落库（空串跳过）
    std::string extraSettingsPretty;
    if (!f.formExtraSettingsJson_.empty()) {
        auto v = validateExtraSettingsJson(f.formExtraSettingsJson_);
        if (v.isFail()) {
            // 唯一 fromJson=true 的写入点（放弃修改按来源清）
            setFormError(f, v.errorMessage(), /*fromJson=*/true);
            f.formAdvancedJsonError_ = v.errorMessage();
            return;
        }
        auto parsed = platform::json::parse(f.formExtraSettingsJson_);
        if (parsed.isOk()) {
            auto pretty = platform::json::stringify(parsed.value(),
                                                    /*pretty=*/true);
            if (pretty.isOk()) extraSettingsPretty = pretty.value();
        }
        // 解析失败理论不可达（validate 已保证）；退化为原文落库
        if (extraSettingsPretty.empty()) extraSettingsPretty =
            f.formExtraSettingsJson_;
    }

    if (f.formEditId_ == 0) {
        if (f.formKey_.empty()) {
            setFormError(f, "新建 Profile 必须填写 API Key");
            return;
        }
        // activateIfNoneActive=true：无激活 Profile 时自动激活这条；已有激活不抢占
        auto r = svc->createProfile(f.formName_, f.formUrl_, f.formKey_,
                                    f.formModels_.def.model, /*activateIfNoneActive=*/true,
                                    /*sharedConfigId=*/f.formSharedConfigId_,
                                    authFieldToText(f.formAuthField_),
                                    /*dialectOverride=*/f.formDialectOverride_,
                                    /*fallbackProfileId=*/f.formFallbackProfileId_,
                                    /*extraSettingsJson=*/extraSettingsPretty);
        if (r.isFail()) {
            setFormError(f, r.errorMessage());
            return;
        }
        // createProfile 无槽位/env 参数，二段补写（纯 JSON profile 也进补写）
        if (!modelsJson.empty() || !envJson.empty() ||
            !extraSettingsPretty.empty()) {
            Profile p = r.value();
            p.model = f.formModels_.def.model;
            p.modelsConfigJson = modelsJson;
            p.extraEnvJson = envJson;
            p.extraSettingsJson = extraSettingsPretty;
            auto u = svc->updateProfile(p);
            if (u.isFail()) {
                setFormError(f, "已创建，但模型/env 写入失败: " +
                                  u.errorMessage());
                return;
            }
        }
        f.formOpen_ = false;
        // 落库成功 → 基线随落库值刷新（「放弃修改」目标恒等于持久化值）
        f.formExtraSettingsJsonBaseline_ = extraSettingsPretty;
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
        p.extraSettingsJson = extraSettingsPretty;   // 高级设置 JSON（统一 pretty）
        p.apiKeyPlain = f.formKey_;   // 空 = 不换 key
        p.authField = authFieldToText(f.formAuthField_);
        p.sharedConfigId = f.formSharedConfigId_;   // 绑定/解绑随保存落库
        p.fallbackProfileId = f.formFallbackProfileId_;   // 备用随保存落库（自引用由服务层校验兜底）
        // 新下拉值须真正写入编辑路径（不接线则落不了盘）；p 来自 getProfile 已含既有覆盖
        p.dialectOverride = f.formDialectOverride_;
        auto r = svc->updateProfile(p);
        if (r.isFail()) {
            setFormError(f, r.errorMessage());
            return;
        }
        f.formOpen_ = false;
        f.formExtraSettingsJsonBaseline_ = extraSettingsPretty;
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
        .text("名称").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.dialog.form.name")
        .position(28.0f, 122.0f).size(w - 56.0f, 40.0f)
        .value(f.formName_).placeholder("例如：官方 / 中转 A")
        .fontSize(14.0f).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formName_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消（关表单不提交，各输入框统一）
        .build();

    ui.text("claude.form.basic.url.label")
        .position(28.0f, 174.0f).size(w - 56.0f, 16.0f)
        .text("Base URL").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.dialog.form.url")
        .position(28.0f, 192.0f).size(w - 56.0f, 40.0f)
        .value(f.formUrl_).placeholder("https://api.anthropic.com")
        .fontSize(14.0f).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formUrl_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
        .build();

    ui.text("claude.form.basic.key.label")
        .position(28.0f, 244.0f).size(w - 56.0f, 16.0f)
        .text("API Key（" + std::string(platform::credential::protectionDescription()) + "）")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    const std::string keyPlaceholder = isEdit
        ? ("留空 = 保持现有 Key（" + f.formKeyHint_ + "）")
        : std::string("sk-ant-...");
    components::input(ui, "claude.dialog.form.key")
        .position(28.0f, 262.0f).size(w - 56.0f, 40.0f)
        .value(f.formKey_).placeholder(keyPlaceholder)
        .fontSize(14.0f).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formKey_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
        .build();

    // 认证字段（鉴权头三态）还原独占一行：标签与灰字说明同行，下拉紧随全宽
    ui.text("claude.form.basic.authfield.label")
        .position(28.0f, 314.0f).size(72.0f, 16.0f)
        .text("认证字段").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.basic.authfield.hint")
        .position(104.0f, 314.0f).size(w - 132.0f, 16.0f)
        .text("网关只接受一种鉴权头时切换；默认两种都发")
        .fontSize(11.0f).lineHeight(16.0f)
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
        .icon(0xF05A).fontSize(12.0f).lineHeight(18.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.basic.hint")
        .position(46.0f, 380.0f).size(w - 74.0f, 18.0f)
        .text("模型映射、高级 env 和高级配置在另外几个标签页配置")
        .fontSize(12.0f).lineHeight(18.0f)
        .color(t.colors.textTertiary).build();

    if (!f.formError_.empty()) {
        ui.text("claude.dialog.form.error")
            .position(28.0f, 404.0f).size(w - 56.0f, 18.0f)
            .text(f.formError_).fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.error).build();
    }

    // 「导入配置」接管态警告（不阻断，仅提示）
    if (!f.formImportWarn_.empty()) {
        ui.text("claude.dialog.form.importwarn.icon")
            .position(28.0f, 426.0f).size(14.0f, 18.0f)
            .icon(0xF071).fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.warning).build();
        ui.text("claude.dialog.form.importwarn")
            .position(46.0f, 426.0f).size(w - 74.0f, 36.0f)
            .text(f.formImportWarn_).fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.warning).build();
    }
}

// 高级配置页：方言覆盖 + 备用 Profile + 片段绑定 + 高级设置 JSON；下拉逐帧归一悬空/自引用为「无」
void drawFormAdvancedTab(eui::Ui& ui, float w,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& themeTokens,
                         FormState& f,
                         const std::vector<Profile>& profiles,
                         const std::vector<SharedConfig>& sharedConfigs) {
    // 方言覆盖：idx 0=未覆盖 / 1=anthropic_default / 2=opencode_zen；显示名与详情页共用 dialectKindDisplayName
    ui.text("claude.form.advanced.dialect.label")
        .position(28.0f, 104.0f).size(72.0f, 16.0f)
        .text("上游方言").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.advanced.dialect.hint")
        .position(104.0f, 104.0f).size(w - 132.0f, 16.0f)
        .text("网关行为差异（鉴权头/会话头/模型名标记）不一致时切换；默认按站点推断")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    {
        std::vector<std::string> items;
        items.reserve(3);
        items.emplace_back("自动（按站点推断）");
        items.emplace_back(
            dialectKindDisplayName(UpstreamDialect::Kind::AnthropicDefault));
        items.emplace_back(
            dialectKindDisplayName(UpstreamDialect::Kind::OpencodeZen));
        // 选中态逐帧归算（空串/未知 → 0）
        int selIdx = 0;
        if (const auto kind = dialectKindFromText(f.formDialectOverride_)) {
            selIdx = *kind == UpstreamDialect::Kind::OpencodeZen ? 2 : 1;
        }
        ui.stack("claude.form.advanced.dialect.ddwrap")
            .position(28.0f, 122.0f)
            .size(w - 56.0f, 40.0f)
            .content([&] {
                components::dropdown(ui, "claude.form.advanced.dialect.dd")
                    .size(w - 56.0f, 40.0f)
                    .theme(themeTokens)
                    .items(std::move(items))
                    .selected(selIdx)
                    .open(f.formDialectDdOpen_)
                    .onOpenChange([fp = &f](bool v) { fp->formDialectDdOpen_ = v; })
                    .onChange([fp = &f](int idx) {
                        switch (idx) {
                            case 1:
                                fp->formDialectOverride_ = dialectKindToText(
                                    UpstreamDialect::Kind::AnthropicDefault);
                                return;
                            case 2:
                                fp->formDialectOverride_ = dialectKindToText(
                                    UpstreamDialect::Kind::OpencodeZen);
                                return;
                            default:
                                fp->formDialectOverride_.clear();
                                return;
                        }
                    })
                    .build();
            })
            .build();
    }

    // 备用 Profile（0=不启用）：主 Profile 返回可重试错误时换发备用（可能按另一家计费）
    ui.text("claude.form.basic.fallback.label")
        .position(28.0f, 174.0f).size(72.0f, 16.0f)
        .text("备用 Profile").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.basic.fallback.hint")
        .position(104.0f, 174.0f).size(w - 132.0f, 16.0f)
        .text("401/403/429/5xx/网络失败时换发备用（可能按另一家计费）")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    {
        // 候选 = 全部 profile 排除自身（新建 formEditId_==0 = 全部可选）
        std::vector<int64_t> candidateIds;
        std::vector<std::string> items;
        items.reserve(profiles.size() + 1);
        items.emplace_back("无（不启用 failover）");
        for (const auto& p : profiles) {
            if (p.id == f.formEditId_) continue;   // 自引用不提供
            candidateIds.push_back(p.id);
            items.push_back(p.name);
        }
        // 选中态逐帧归算；悬空归一为无（幂等）
        int selIdx = 0;
        if (f.formFallbackProfileId_ != 0) {
            for (size_t k = 0; k < candidateIds.size(); ++k) {
                if (candidateIds[k] == f.formFallbackProfileId_) {
                    selIdx = static_cast<int>(k) + 1;
                    break;
                }
            }
            if (selIdx == 0) f.formFallbackProfileId_ = 0;
        }
        ui.stack("claude.form.basic.fallback.ddwrap")
            .position(28.0f, 192.0f)
            .size(w - 56.0f, 40.0f)
            .content([&] {
                // candidateIds 按值捕获：onChange 只取 id，不悬垂引用快照
                components::dropdown(ui, "claude.form.basic.fallback.dd")
                    .size(w - 56.0f, 40.0f)
                    .theme(themeTokens)
                    .items(std::move(items))
                    .selected(selIdx)
                    .open(f.formFallbackDdOpen_)
                    .onOpenChange([fp = &f](bool v) { fp->formFallbackDdOpen_ = v; })
                    .onChange([fp = &f, ids = std::move(candidateIds)](int idx) {
                        if (idx <= 0) {
                            fp->formFallbackProfileId_ = 0;
                            return;
                        }
                        const size_t k = static_cast<size_t>(idx - 1);
                        if (k < ids.size()) fp->formFallbackProfileId_ = ids[k];
                    })
                    .build();
            })
            .build();
    }

    // 共享配置片段绑定（idx 0 = 未绑定）
    ui.text("claude.form.advanced.shared.label")
        .position(28.0f, 244.0f).size(72.0f, 16.0f)
        .text("绑定片段").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.advanced.shared.hint")
        .position(104.0f, 244.0f).size(w - 132.0f, 16.0f)
        .text("片段属性在写入时覆盖本页同名键，解绑回落")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    {
        std::vector<std::string> items;
        items.reserve(sharedConfigs.size() + 1);
        items.emplace_back("无（不绑定片段）");
        for (const auto& sc : sharedConfigs) items.push_back(sc.name);
        // 选中态逐帧归算；悬空绑定归一为未绑定（幂等，不把幽灵 id 留在表单态）
        int selIdx = 0;
        if (f.formSharedConfigId_ != 0) {
            for (size_t k = 0; k < sharedConfigs.size(); ++k) {
                if (sharedConfigs[k].id == f.formSharedConfigId_) {
                    selIdx = static_cast<int>(k) + 1;
                    break;
                }
            }
            if (selIdx == 0) f.formSharedConfigId_ = 0;
        }
        ui.stack("claude.form.advanced.shared.ddwrap")
            .position(28.0f, 262.0f)
            .size(w - 56.0f, 40.0f)
            .content([&] {
                components::dropdown(ui, "claude.form.advanced.shared.dd")
                    .size(w - 56.0f, 40.0f)
                    .theme(themeTokens)
                    .items(std::move(items))
                    .selected(selIdx)
                    .open(f.formSharedDdOpen_)
                    .onOpenChange([fp = &f](bool v) { fp->formSharedDdOpen_ = v; })
                    .onChange([fp = &f, scs = &sharedConfigs](int idx) {
                        if (idx <= 0) {
                            fp->formSharedConfigId_ = 0;
                            return;
                        }
                        const size_t k = static_cast<size_t>(idx - 1);
                        if (k < scs->size()) fp->formSharedConfigId_ = (*scs)[k].id;
                    })
                    .build();
            })
            .build();
    }

    ui.text("claude.form.advanced.json.label")
        .position(28.0f, 314.0f).size(110.0f, 16.0f)
        .text("高级设置 JSON").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.advanced.json.hint")
        .position(142.0f, 314.0f).size(w - 170.0f, 16.0f)
        .text("settings.json 除 env 外的属性：model / permissions / statusLine / hooks / …")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.form.advanced.json")
        .position(28.0f, 332.0f).size(w - 56.0f, 100.0f)
        .value(f.formExtraSettingsJson_)
        .placeholder(R"({ "model": "...", "permissions": { ... } })")
        .multiline(true).scrollbar(true)
        .fontSize(12.0f).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) {
            fp->formExtraSettingsJson_ = v;
            fp->formAdvancedJsonError_.clear();
        })
        .onCancel([fp = &f] { fp->formOpen_ = false; })   // Esc=取消（导入框同款）
        .build();

    // 「格式化」：与全屏覆盖层按钮共用 formatAdvancedJson；失败写行内红字
    //（UAT5 米黄清理：surfaceElevated 暖橙 → WeChat Secondary 白底灰边）
    components::button(ui, "claude.form.advanced.json.format")
        .position(28.0f, 440.0f).size(110.0f, 32.0f)
        .text("格式化").fontSize(12.0f)
        .colors(t.colors.hover,
                t.colors.hover,
                t.colors.hover)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusWeChat)
        .border(1.0f, t.colors.borderDefault)
        .onClick([fp = &f] { formatAdvancedJson(*fp); })
        .build();
    // 「全屏编辑」入口：只置位 formJsonFullscreen_，覆盖层由 compose 绘制，表单态本帧不动
    components::button(ui, "claude.form.advanced.json.fs")
        .position(150.0f, 440.0f).size(110.0f, 32.0f)
        .text("全屏编辑").fontSize(12.0f)
        .colors(t.colors.hover,
                t.colors.hover,
                t.colors.hover)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusWeChat)
        .border(1.0f, t.colors.borderDefault)
        .onClick([fp = &f] { fp->formJsonFullscreen_ = true; })
        .build();
    // 已绑定片段提示行：文本框无逐键行，片段赢语义在此明示
    if (const SharedConfig* boundSc =
            findSharedById(sharedConfigs, f.formSharedConfigId_);
        boundSc != nullptr) {
        ui.text("claude.form.advanced.json.boundhint")
            .position(272.0f, 448.0f).size(w - 300.0f, 16.0f)
            .text("已绑定片段「" + truncateUtf8(boundSc->name, 16) +
                  "」：同名键以片段为准（解绑后回落本页值）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
    }

    // 行内红字（格式化/校验失败共用；单行，与底部 formError 行相邻不重叠）
    if (!f.formAdvancedJsonError_.empty() && f.formError_.empty()) {
        ui.text("claude.form.advanced.json.error")
            .position(28.0f, 480.0f).size(w - 56.0f, 16.0f)
            .text(f.formAdvancedJsonError_).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.error).build();
    }
}

void drawFormModelsTab(eui::Ui& ui, float w,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       FormState& f) {
    const auto slotTable = modelSlotTable();

    ui.text("claude.form.slots.h.slot")
        .position(28.0f, 104.0f).size(110.0f, 14.0f)
        .text("槽位").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.h.model")
        .position(150.0f, 104.0f).size(330.0f, 14.0f)
        .text("上游模型名（发往上游）").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.h.name")
        .position(492.0f, 104.0f).size(240.0f, 14.0f)
        .text("显示名（CC 界面）").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();

    for (size_t i = 0; i < slotTable.size(); ++i) {
        const float y = 126.0f + static_cast<float>(i) * 52.0f;
        const auto& row = slotTable[i];
        const ModelSlot& slot = f.formModels_.*row.slot;
        const std::string idx = std::to_string(i);
        ui.text("claude.form.slot.label." + idx)
            .position(28.0f, y + 10.0f).size(110.0f, 16.0f)
            .text(row.formLabel).fontSize(12.0f).lineHeight(16.0f)
            .color(t.colors.textSecondary).build();
        components::input(ui, "claude.form.slot." + idx + ".model")
            .position(150.0f, y).size(330.0f, 36.0f)
            .value(slot.model).placeholder("fangzhou/glm-5.3[1M]")
            .fontSize(13.0f).theme(themeTokens)
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
            .fontSize(13.0f).theme(themeTokens)
            .onChange([fp = &f, m = row.slot](const std::string& v) {
                (fp->formModels_.*m).name = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
    }

    ui.text("claude.form.slots.hint1.icon")
        .position(28.0f, 446.0f).size(14.0f, 16.0f)
        .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.hint1")
        .position(46.0f, 446.0f).size(w - 74.0f, 16.0f)
        .text("显示名留空时自动剥除模型名的 [1M]/[200K] 等上下文标记")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.slots.hint2")
        .position(28.0f, 464.0f).size(w - 56.0f, 16.0f)
        .text("    槽位留空 = 不写入该 env 键，Claude Code 用内置映射")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
}

// 高级 env 页（键值编辑器 + 粘贴导入）；可见行数上限 5（弹窗高度约束），超出部分仍在状态里全量保存
void drawFormEnvTab(eui::Ui& ui, float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& themeTokens,
                    FormState& f, ImportState& imp,
                    const std::vector<SharedConfig>& sharedConfigs) {
    (void)sharedConfigs;   // 签名保留，参数不使用
    constexpr size_t kMaxVisibleRows = 5;

    ui.text("claude.form.env.label")
        .position(28.0f, 104.0f).size(348.0f, 16.0f)
        .text("额外环境变量（原样写入 settings.json env 块）")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();

    const size_t visible =
        f.formEnvRows_.size() < kMaxVisibleRows ? f.formEnvRows_.size()
                                                : kMaxVisibleRows;
    for (size_t i = 0; i < visible; ++i) {
        const float y = 124.0f + static_cast<float>(i) * 38.0f;
        const std::string idx = std::to_string(i);
        components::input(ui, "claude.form.env.row." + idx + ".key")
            .position(28.0f, y).size(220.0f, 32.0f)
            .value(f.formEnvRows_[i].first).placeholder("ENV_KEY")
            .fontSize(12.0f).theme(themeTokens)
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
                .position(28.0f, y).size(220.0f, 32.0f)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .border(1.0f, t.colors.error)
                .radius(t.sizes.radiusSmall)
                .build();
        }
        components::input(ui, "claude.form.env.row." + idx + ".value")
            .position(258.0f, y).size(414.0f, 32.0f)
            .value(f.formEnvRows_[i].second).placeholder("值（字符串）")
            .fontSize(12.0f).theme(themeTokens)
            .onChange([fp = &f, i](const std::string& v) {
                fp->formEnvRows_[i].second = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
            .build();
        components::button(ui, "claude.form.env.row." + idx + ".del")
            .position(682.0f, y).size(50.0f, 32.0f)
            .icon(0xF00D)  // fa-xmark
            .fontSize(12.0f)
            .colors(t.colors.hover,
                    t.colors.hover,
                    t.colors.hover)
            .textColor(t.colors.error)
            .iconColor(t.colors.error)
            .radius(t.sizes.radiusWeChat)
            .border(1.0f, t.colors.borderDefault)
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

    components::button(ui, "claude.form.env.add")
        .position(28.0f, 314.0f).size(110.0f, 30.0f)
        .text("+ 添加变量").fontSize(12.0f)
        .colors(t.colors.hover,
                t.colors.hover,
                t.colors.hover)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusWeChat)
        .border(1.0f, t.colors.borderDefault)
        .onClick([fp = &f] { fp->formEnvRows_.emplace_back(); })
        .build();
    if (f.formEnvRows_.size() > kMaxVisibleRows) {
        ui.text("claude.form.env.overflow")
            .position(150.0f, 320.0f).size(w - 150.0f - 28.0f, 16.0f)
            .text("另有 " + std::to_string(f.formEnvRows_.size() -
                                           kMaxVisibleRows) +
                  " 项已保存（保存时全量写入，列表仅显示前 " +
                  std::to_string(kMaxVisibleRows) + " 项）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
    }

    ui.text("claude.form.import.label")
        .position(28.0f, 356.0f).size(w - 56.0f, 16.0f)
        .text("── 从 CC / settings.json 粘贴导入 ──")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.form.import.text")
        .position(28.0f, 376.0f).size(w - 56.0f, 82.0f)
        .value(imp.importText_)
        .placeholder(R"({ "env": { "ANTHROPIC_BASE_URL": "...", ... } }  粘贴整段 settings.json 或仅 env 对象)")
        .multiline(true).scrollbar(true)
        .fontSize(12.0f).theme(themeTokens)
        .onChange([ip = &imp](const std::string& v) { ip->importText_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // Esc=取消
        .build();

    components::button(ui, "claude.form.import.parse")
        .position(28.0f, 468.0f).size(110.0f, 32.0f)
        .text("解析导入").fontSize(12.0f)
        .colors(t.colors.hover,
                t.colors.hover,
                t.colors.hover)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusWeChat)
        .border(1.0f, t.colors.borderDefault)
        .onClick([ip = &imp] { doParseImport(*ip); })
        .build();
    if (imp.importReady_) {
        // 主动作 → WeChat Primary 绿填充白字（原 legacy 橙 primary 一并换）
        components::button(ui, "claude.form.import.apply")
            .position(148.0f, 468.0f).size(110.0f, 32.0f)
            .text("应用到表单").fontSize(12.0f)
            .colors(t.colors.primary, t.colors.primaryHover,
                    t.colors.primaryPressed)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radiusWeChat)
            .onClick([fp = &f, ip = &imp] { doApplyImport(*fp, *ip); })
            .build();
    }

    // 解析结果：失败红字 / 成功预览（识别数 + 别名/未识别提示）
    if (!imp.importError_.empty()) {
        ui.text("claude.form.import.error.icon")
            .position(270.0f, 468.0f).size(13.0f, 16.0f)
            .icon(0xF00D).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.error).build();
        ui.text("claude.form.import.error")
            .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 48.0f)
            .text(imp.importError_).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.error).build();
    } else if (imp.importReady_) {
        ui.text("claude.form.import.preview")
            .position(270.0f, 468.0f).size(w - 270.0f - 28.0f, 16.0f)
            .text("识别：槽位 " + std::to_string(imp.importPreview_.recognizedSlots) +
                  "/" + std::to_string(kModelSlotCount) +
                  " · env " + std::to_string(imp.importPreview_.extraEnv.size()) +
                  " 项 · 别名映射 " +
                  std::to_string(imp.importPreview_.aliasMappedKeys.size()) +
                  " · 未识别 " +
                  std::to_string(imp.importPreview_.unrecognizedKeys.size()))
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textSecondary).build();
        // 警告角标最多 3 条（弹窗高度约束；全量信息在保存后的 settings.json 与日志可查）
        std::vector<std::string> notes;
        for (size_t i = 0; i < imp.importPreview_.aliasMappedKeys.size() && i < 2; ++i) {
            notes.push_back("已映射为官方变量: " +
                            imp.importPreview_.aliasMappedKeys[i]);
        }
        for (size_t i = 0;
             i < imp.importPreview_.unrecognizedKeys.size() && notes.size() < 3;
             ++i) {
            notes.push_back("未识别键将透传: " +
                            imp.importPreview_.unrecognizedKeys[i]);
        }
        for (size_t i = 0;
             i < imp.importPreview_.warnings.size() && notes.size() < 3; ++i) {
            notes.push_back(imp.importPreview_.warnings[i]);
        }
        // 角标与底部校验错误行同屏重叠：有校验错误时让位，只显示更可行动的校验错误
        if (f.formError_.empty()) {
            for (size_t i = 0; i < notes.size(); ++i) {
                ui.text("claude.form.import.note.icon." + std::to_string(i))
                    .position(270.0f, 486.0f + static_cast<float>(i) * 15.0f)
                    .size(13.0f, 15.0f)
                    .icon(0xF071).fontSize(10.0f).lineHeight(15.0f)
                    .color(t.colors.warning).build();
                ui.text("claude.form.import.note." + std::to_string(i))
                    .position(286.0f, 486.0f + static_cast<float>(i) * 15.0f)
                    .size(w - 286.0f - 28.0f, 15.0f)
                    .text(notes[i]).fontSize(10.0f).lineHeight(15.0f)
                    .color(t.colors.warning).build();
            }
        }
    } else {
        ui.text("claude.form.import.hint.icon")
            .position(270.0f, 468.0f).size(13.0f, 16.0f)
            .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.import.hint")
            .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 32.0f)
            .text("BASE_URL/AUTH_TOKEN 归基础页，模型键归模型映射页，"
                  "其余进入上表")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
    }
}

} // namespace

void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           FormState& f, ImportState& imp,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::vector<Profile>& profiles,
                           const FormSubmitHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    // UAT6 O 批：最小窗 1000×640（页高 600）下弹窗 600 恒高填满全页、按钮出框；
// 540 档（600−60 余量）使弹窗在最小窗下不溢出、仍有呼吸区。四 tab 高度仍统一。
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
                {.y = 20.0f, .height = 26.0f, .lineHeight = 26.0f});

            // tabs 四页；TabsBuilder 无 position，套 stack 定位
            ui.stack("claude.form.tabs.wrap")
                .position(28.0f, 52.0f)
                .size(w - 56.0f, 34.0f)
                .content([&] {
                    components::tabs(ui, "claude.form.tabs")
                        .size(w - 56.0f, 34.0f)
                        .items({"基础", "模型映射", "高级 env", "高级配置"})
                        .selected(f.formTab_)
                        .fontSize(13.0f)
                        .theme(themeTokens)
                        .onChange([fp = &f](int idx) { fp->formTab_ = idx; })
                        .build();
                })
                .build();

            if (f.formTab_ == 0) {
                drawFormBasicTab(ui, w, t, themeTokens, isEdit, f);
            } else if (f.formTab_ == 1) {
                drawFormModelsTab(ui, w, t, themeTokens, f);
            } else if (f.formTab_ == 3) {
                drawFormAdvancedTab(ui, w, t, themeTokens, f, profiles,
                                    sharedConfigs);
            } else {
                drawFormEnvTab(ui, w, t, themeTokens, f, imp, sharedConfigs);
            }

            // 错误信息（基础页之外也要看得到校验错误）
            if (f.formTab_ != 0) {
                ui_kit::drawDialogText(ui, "claude.dialog.form.error", w, f.formError_,
                                       12.0f, t.colors.error,
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
            confirm.onClick = [fp = &f, hooks, scs = &sharedConfigs] {
                submitForm(*fp, hooks, *scs);
            };
            ui_kit::drawDialogButtons(ui, "claude.dialog.form", w, h, cancel, confirm, t,
                                      themeTokens);
    });
}

// 高级设置 JSON 全屏编辑覆盖层：zIndex 1050，介于表单弹窗(1000)与 toast(1100)之间；文本/落库复用表单既有状态
void drawJsonFullscreenOverlay(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& themeTokens,
                               FormState& f, const FormSubmitHooks& hooks,
                               const std::vector<SharedConfig>& sharedConfigs) {
    // 结构性不变式：表单不在场 ⇒ 全屏旗必复位（任何关表单路径后下一帧即清）
    if (!f.formJsonFullscreen_ || !f.formOpen_) {
        f.formJsonFullscreen_ = false;
        return;
    }
    const float w = screen.width;
    const float h = screen.height;

    ui.stack("claude.jsonfs.root")
        .size(w, h)
        .zIndex(1050)
        .content([&] {
            // 整页不透明底 + blockPointer 模态（点击不穿透）；点遮罩 = 「放弃修改」（同 Esc）
            //（UAT5 米黄清理：legacy background 暖底 → wechat.pageBg）
            ui.rect("claude.jsonfs.bg")
                .size(w, h)
                .color(t.colors.background)
                .blockPointer()
                .onClick([fp = &f] { discardJsonFullscreenEdits(*fp); })
                .build();

            ui_kit::drawDialogTitle(ui, "claude.jsonfs.title", w,
                                    "高级设置 JSON（全屏）", 20.0f,
                                    t.colors.textPrimary,
                                    {.y = 20.0f, .height = 26.0f,
                                     .lineHeight = 26.0f});
            // 基础项（名称/URL/Key）缺失的修复入口在基础页、全屏态不可达，末尾明示回表单补填
            ui.text("claude.jsonfs.hint")
                .position(28.0f, 48.0f).size(w - 56.0f, 16.0f)
                .text("与表单弹窗同一份编辑缓冲：格式化 / 保存（= 整表单提交）在此生效；"
                      "「放弃修改」、Esc 或点击遮罩 = 丢弃本次改动并回到上次保存值；"
                      "基础项（名称/URL/Key）缺失需回表单基础页补填")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();

            // 大号多行编辑框；与表单框同接 formExtraSettingsJson_（同一编辑缓冲），Esc = 放弃修改
            components::input(ui, "claude.jsonfs.editor")
                .position(28.0f, 72.0f).size(w - 56.0f, h - 176.0f)
                .value(f.formExtraSettingsJson_)
                .placeholder(R"({ "model": "...", "permissions": { ... } })")
                .multiline(true).scrollbar(true)
                .fontSize(13.0f).theme(themeTokens)
                .onChange([fp = &f](const std::string& v) {
                    fp->formExtraSettingsJson_ = v;
                    fp->formAdvancedJsonError_.clear();
                })
                .onCancel([fp = &f] { discardJsonFullscreenEdits(*fp); })
                .build();

            // 红字分层同表单侧：优先 formError_ 提交闸门，其次行内 JSON 红字
            const std::string& fsError = !f.formError_.empty()
                                             ? f.formError_
                                             : f.formAdvancedJsonError_;
            if (!fsError.empty()) {
                ui.text("claude.jsonfs.error")
                    .position(28.0f, h - 96.0f).size(w - 56.0f, 16.0f)
                    .text(fsError).fontSize(12.0f).lineHeight(16.0f)
                    .color(t.colors.error).build();
            }

            // 底部三键：格式化 / 保存（=submitForm 整条路径）/ 放弃修改（恢复落库值并退全屏）
            //（UAT5 米黄清理：surfaceElevated/legacy primary → WeChat Secondary/Primary）
            components::button(ui, "claude.jsonfs.format")
                .position(28.0f, h - 68.0f).size(110.0f, 40.0f)
                .text("格式化").fontSize(12.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .onClick([fp = &f] { formatAdvancedJson(*fp); })
                .build();
            components::button(ui, "claude.jsonfs.apply")
                .position(150.0f, h - 68.0f).size(110.0f, 40.0f)
                .text("保存").fontSize(12.0f)
                .colors(t.colors.primary, t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radiusWeChat)
                .onClick([fp = &f, hooks, scs = &sharedConfigs] {
                    submitForm(*fp, hooks, *scs);
                })
                .build();
            components::button(ui, "claude.jsonfs.discard")
                .position(272.0f, h - 68.0f).size(110.0f, 40.0f)
                .text("放弃修改").fontSize(12.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .onClick([fp = &f] { discardJsonFullscreenEdits(*fp); })
                .build();
        })
        .build();
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
            confirm.style = ui_kit::DialogButtonStyle::DangerOutlineThemed;
            confirm.onClick = [fp = &f, onConfirmDelete] {
                const int64_t id = fp->deleteTargetId_;
                fp->deleteConfirmOpen_ = false;
                onConfirmDelete(id);
            };
            ui_kit::drawDialogButtons(ui, "claude.dialog.delete", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// 共享配置片段弹窗：独立 SharedConfigDialogState（不复用 FormState，不污染 profile 表单态）
void openSharedCreate(SharedConfigDialogState& s) {
    s.editId_ = 0;
    s.name_.clear();
    s.extraSettingsJson_.clear();
    s.error_.clear();
    s.open_ = true;
}

void openSharedEdit(SharedConfigDialogState& s, const SharedConfig& c) {
    s.editId_ = c.id;
    s.name_ = c.name;
    s.error_.clear();
    // 预填片段 JSON 原文（外部改库原文按原样，格式化按钮负责规范化）
    s.extraSettingsJson_ = c.extraSettingsJson;
    s.open_ = true;
}

void submitSharedForm(SharedConfigDialogState& s,
                      const SharedConfigSubmitHooks& hooks) {
    ClaudeService* svc = ClaudeService::global();
    if (!svc) {
        s.error_ = "服务未就绪（ClaudeService 未注册）";
        return;
    }
    hooks.markDataDirty();   // 写操作前即标脏（失败也不伤，下帧重查）
    if (s.name_.empty()) {
        s.error_ = "片段名不能为空";
        return;
    }
    // 保存走 validateExtraSettingsJson 同一套规则（含拒绝 env 键）；通过→统一 pretty 落库
    std::string settingsPretty;
    if (!s.extraSettingsJson_.empty()) {
        auto v = validateExtraSettingsJson(s.extraSettingsJson_);
        if (v.isFail()) {
            s.error_ = v.errorMessage();
            return;
        }
        auto parsed = platform::json::parse(s.extraSettingsJson_);
        if (parsed.isOk()) {
            auto pretty = platform::json::stringify(parsed.value(),
                                                    /*pretty=*/true);
            if (pretty.isOk()) settingsPretty = pretty.value();
        }
        if (settingsPretty.empty()) settingsPretty = s.extraSettingsJson_;
    }
    if (s.editId_ == 0) {
        auto r = svc->createSharedConfig(s.name_, settingsPretty);
        if (r.isFail()) {
            s.error_ = r.errorMessage();   // 重名等校验错误红字回显
            return;
        }
        s.open_ = false;
        hooks.toast("已创建", "共享配置片段「" + s.name_ + "」已创建", false);
        // 新片段无人绑定，无需 re-takeover
    } else {
        SharedConfig c;
        c.id = s.editId_;
        c.name = s.name_;
        c.extraSettingsJson = settingsPretty;
        auto r = svc->updateSharedConfig(c);
        if (r.isFail()) {
            s.error_ = r.errorMessage();
            return;
        }
        s.open_ = false;
        // 服务层已处理绑定 profile 的即时 re-takeover（claude_service.cpp），UI 不重复写
        hooks.toast("已保存",
                    "共享配置片段「" + s.name_ + "」已更新，绑定 Profile 下次接管生效",
                    false);
    }
}

// 共享配置片段编辑弹窗：name + 高级设置 JSON 四件；编辑态底部多一枚「删除」
void drawSharedConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens,
                            SharedConfigDialogState& s,
                            const SharedConfigSubmitHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    const float h = 460.0f;
    const bool isEdit = (s.editId_ != 0);

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.shared";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = s.open_;
    frame.onOpenChange = [sp = &s](bool v) { sp->open_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        ui_kit::drawDialogTitle(
            ui, "claude.dialog.shared.title", w,
            isEdit ? "编辑共享配置片段" : "新建共享配置片段", 20.0f,
            t.colors.textPrimary, {.y = 20.0f, .height = 26.0f, .lineHeight = 26.0f});

        ui.text("claude.dialog.shared.name.label")
            .position(28.0f, 64.0f).size(w - 56.0f, 16.0f)
            .text("片段名（共享给所有 Profile 的高级属性预设，不含密钥/Base URL）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.shared.name")
            .position(28.0f, 82.0f).size(w - 56.0f, 36.0f)
            .value(s.name_).placeholder("例如：企业内部网关 env")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([sp = &s](const std::string& v) { sp->name_ = v; })
            .onCancel([sp = &s] { sp->open_ = false; })   // Esc=取消（同表单各输入框）
            .build();

        // 片段即 settings.json 属性预设：绑定它的 Profile 写入时逐键叠加、片段赢，解绑回落
        ui.text("claude.dialog.shared.json.label")
            .position(28.0f, 128.0f).size(w - 56.0f, 16.0f)
            .text("高级设置 JSON（settings.json 除 env 外的属性，绑定它的 Profile 写入时覆盖同名键）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.shared.json")
            .position(28.0f, 146.0f).size(w - 56.0f, 170.0f)
            .value(s.extraSettingsJson_)
            .placeholder(R"({ "model": "...", "permissions": { ... } })")
            .multiline(true).scrollbar(true)
            .fontSize(12.0f).theme(themeTokens)
            .onChange([sp = &s](const std::string& v) {
                sp->extraSettingsJson_ = v;
                sp->error_.clear();
            })
            .onCancel([sp = &s] { sp->open_ = false; })   // Esc=取消（同表单各输入框）
            .build();
        //（UAT5 米黄清理：surfaceElevated 暖橙 → WeChat Secondary 白底灰边，同表单高级页格式化钮）
        components::button(ui, "claude.dialog.shared.json.format")
            .position(28.0f, 324.0f).size(110.0f, 32.0f)
            .text("格式化").fontSize(12.0f)
            .colors(t.colors.hover,
                    t.colors.hover,
                    t.colors.hover)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusWeChat)
            .border(1.0f, t.colors.borderDefault)
            .onClick([sp = &s] {
                sp->error_.clear();
                if (sp->extraSettingsJson_.empty()) return;
                auto parsed = platform::json::parse(sp->extraSettingsJson_);
                if (parsed.isFail()) {
                    sp->error_ = "解析失败: " + parsed.errorMessage();
                    return;
                }
                auto pretty = platform::json::stringify(parsed.value(),
                                                        /*pretty=*/true);
                if (pretty.isFail()) {
                    sp->error_ = "序列化失败: " + pretty.errorMessage();
                    return;
                }
                sp->extraSettingsJson_ = pretty.value();
            })
            .build();
        if (!s.error_.empty()) {
            ui.text("claude.dialog.shared.error")
                .position(28.0f, 364.0f).size(w - 56.0f, 18.0f)
                .text(s.error_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.error).build();
        }

        // 底部按钮：编辑态删除/取消/保存，新建态取消/创建；删除另开确认弹窗（同事务解绑绑定 Profile）
        //（UAT5 米黄清理：整组换 WeChat——Secondary 白底灰边 / Primary 绿，同表单弹窗先例）
        const float py = h - 60.0f;
        if (isEdit) {
            components::button(ui, "claude.dialog.shared.delete")
                .position(28.0f, py).size(88.0f, 40.0f)
                .text("删除").fontSize(14.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.error)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .onClick([sp = &s] {
                    sp->deleteConfirmId_ = sp->editId_;
                    sp->deleteConfirmName_ = sp->name_;
                    sp->deleteConfirmOpen_ = true;
                })
                .build();
        }
        const float pairW = (w - 124.0f - 28.0f - 12.0f) * 0.5f;
        const float cancelX = isEdit ? 124.0f : 28.0f;
        const float confirmX = isEdit ? 124.0f + pairW + 12.0f
                                      : 28.0f + pairW + 12.0f;
        components::button(ui, "claude.dialog.shared.cancel")
            .position(cancelX, py).size(pairW, 40.0f)
            .text("取消").fontSize(14.0f)
            .colors(t.colors.hover,
                    t.colors.hover,
                    t.colors.hover)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusWeChat)
            .border(1.0f, t.colors.borderDefault)
            .onClick([sp = &s] { sp->open_ = false; })
            .build();
        components::button(ui, "claude.dialog.shared.confirm")
            .position(confirmX, py).size(pairW, 40.0f)
            .text(isEdit ? "保存" : "创建").fontSize(14.0f)
            .colors(t.colors.primary, t.colors.primaryHover,
                    t.colors.primaryPressed)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radiusWeChat)
            .onClick([sp = &s, hooks] { submitSharedForm(*sp, hooks); })
            .build();
    });
}

// 片段删除确认弹窗（删除 = 同事务解绑绑定它的 profile；解绑计数由模块侧在回调里给出）
void drawSharedConfigDeleteConfirmDialog(
    eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens,
    SharedConfigDialogState& s,
    const std::function<void(int64_t id, const std::string& name)>&
        onConfirmDelete) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    const float h = 190.0f;

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.shared.delete";
    frame.width = ui_kit::DialogWidth::Compact;
    frame.height = h;
    frame.open = s.deleteConfirmOpen_;
    frame.onOpenChange = [sp = &s](bool v) { sp->deleteConfirmOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        ui_kit::drawDialogTitle(ui, "claude.dialog.shared.delete.title", w,
                                "确认删除片段？", 20.0f, t.colors.textPrimary);
        ui_kit::drawDialogText(
            ui, "claude.dialog.shared.delete.msg", w,
            "将删除共享配置片段「" + s.deleteConfirmName_ +
                "」，绑定它的 Profile 会自动解绑（同名键回落 Profile 自身值），"
                "不可恢复。",
            t.sizes.fontSizeSmall, t.colors.textSecondary,
            {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f, .wrap = true});

        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
        cancel.onClick = [sp = &s] { sp->deleteConfirmOpen_ = false; };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认删除";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::DangerOutlineThemed;
        confirm.onClick = [sp = &s, onConfirmDelete] {
            const int64_t id = sp->deleteConfirmId_;
            const std::string name = sp->deleteConfirmName_;
            sp->deleteConfirmOpen_ = false;
            onConfirmDelete(id, name);
        };
        ui_kit::drawDialogButtons(ui, "claude.dialog.shared.delete", w, h,
                                  cancel, confirm, t, themeTokens);
    });
}

} // namespace module::claude
