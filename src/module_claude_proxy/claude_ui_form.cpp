// src/module_claude_proxy/claude_ui_form.cpp
// R4（P-B5）：表单弹窗（tabs 四页，2026-09-30 batch-fix B 尾插「高级配置」）
// + 删除确认弹窗 + 表单状态机 —— 逐字自
// claude_module.cpp 搬出（含注释），只把 this 成员换成显式入参
// （FormState / ImportState 引用 + FormSubmitHooks 回调）。
#include "claude_ui_form.h"

#include "claude_ui_logic.h"
#include "env_var_hints.h"
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

// 认证字段下拉选项（**顺序必须与 AuthField 枚举声明一致**：Both/Bearer/ApiKey）
constexpr const char* kAuthFieldLabels[] = {
    "双发（默认）", "只发 Authorization: Bearer", "只发 x-api-key"};
constexpr int kAuthFieldCount = 3;

} // namespace

// ============ 新建/编辑 Profile 表单弹窗（tabs 三页，§6.1） ============
void openCreateForm(FormState& f, ImportState& imp) {
    f.formEditId_ = 0;   // 0 = 新建
    f.formTab_ = 0;
    f.formName_.clear();
    f.formUrl_.clear();
    f.formKey_.clear();
    f.formError_.clear();
    f.formKeyHint_.clear();
    f.formImportWarn_.clear();
    f.formModels_ = ModelsConfig{};
    f.formEnvRows_.clear();
    f.formAuthField_ = AuthField::Both;
    f.formAuthFieldOpen_ = false;
    // 批 2：绑定与片段赢预览复位（新建默认未绑定）
    f.formSharedConfigId_ = 0;
    f.formSharedDdOpen_ = false;
    f.formFrozenKeys_.clear();
    f.formOriginalOwnKeys_.clear();
    // 批 3：备用 Profile 复位（新建默认不启用 failover）
    f.formFallbackProfileId_ = 0;
    f.formFallbackDdOpen_ = false;
    // 高级配置页复位（2026-09-30 batch-fix B：新建默认不覆盖方言）
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
    f.formError_.clear();
    f.formKeyHint_ = p.apiKeyHint;
    f.formImportWarn_.clear();
    // 槽位：models_config + 旧 model 列 fallback（§4.2 兼容读取）
    f.formModels_ = effectiveModelsConfig(p.modelsConfigJson, p.model);
    // 认证字段：编辑必须回显当前值（否则用户以为看到的是默认值，一保存就坐实覆盖）
    f.formAuthField_ = authFieldFromText(p.authField);
    f.formAuthFieldOpen_ = false;
    // 批 2：绑定回显 + 片段赢预览初值（K = 所绑片段 extra_env 的键，
    // 设计 §2.3 优先级 2 > 3）
    f.formSharedConfigId_ = p.sharedConfigId;
    f.formSharedDdOpen_ = false;
    f.formFrozenKeys_.clear();
    f.formOriginalOwnKeys_.clear();
    if (p.sharedConfigId != 0) {
        const SharedConfig* sc = findSharedById(sharedConfigs, p.sharedConfigId);
        if (sc != nullptr) f.formFrozenKeys_ = sharedFrozenKeys(*sc);
    }
    // 批 3：备用 Profile 回显（悬空/自引用 → 下拉逐帧归一为「无」，见绘制处）
    f.formFallbackProfileId_ = p.fallbackProfileId;
    f.formFallbackDdOpen_ = false;
    // 高级配置页回显（2026-09-30 batch-fix B：方言覆盖；空/未知值 → 下拉逐帧
    // 归一为「自动」，见绘制处）
    f.formDialectOverride_ = p.dialectOverride;
    f.formDialectDdOpen_ = false;
    // env 行：非法 JSON 读路径容错为空表（不炸）；自有键（归一后）记入
    // formOriginalOwnKeys_——「新增同键」拦截只拦打开后新加的行，存量键
    // 非破坏保留、冻结显示（解绑后恢复生效，设计 §2.3）
    f.formEnvRows_.clear();
    auto ee = parseExtraEnv(p.extraEnvJson);
    if (ee.isOk()) {
        for (const auto& [k, v] : ee.value()) {
            f.formEnvRows_.emplace_back(k, v);
            f.formOriginalOwnKeys_.push_back(normalizedEnvKey(k));
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

// 导入解析（纯函数消费，Phase D）：失败只写红字，不动表单
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

// 确认导入：按解析结果填充三页表单（BASE_URL/token → 基础页，
// 槽位非空覆盖，env 键同名替换 / 新键追加）
void doApplyImport(FormState& f, ImportState& imp) {
    if (!imp.importReady_) return;
    if (!imp.importPreview_.baseUrl.empty()) f.formUrl_ = imp.importPreview_.baseUrl;
    if (!imp.importPreview_.authToken.empty()) {
        f.formKey_ = imp.importPreview_.authToken;
    }
    // 槽位：非空则覆盖（遍历槽位表派生，CPX-08）
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
    // 认证字段：按键名证据填（空串 = 无证据，不覆盖表单现值；设计 §5）
    if (!imp.importPreview_.authFieldFromEnv.empty()) {
        f.formAuthField_ = authFieldFromText(imp.importPreview_.authFieldFromEnv);
    }
    imp.importReady_ = false;
    imp.importText_.clear();
    f.formError_.clear();   // 导入成功清掉旧校验错误
}

// ============ 导入当前配置（一键读 settings.json → 预填新建表单）============
// 等价于用户手动「从 cc-switch / settings.json 粘贴导入」但全自动：
// 读用户级 settings.json → parseImportedEnv → 打开新建弹窗并预填
// （基础页 BASE_URL/Token、模型映射页槽位、高级 env 页 + 预览标记）。
// 接管态（sidecar 快照存在）时 settings.json 里是代理托管值
// （BASE_URL=127.0.0.1:8888）而非原始上游 → 弹窗内给警告，不阻断。
void startImportCurrentConfig(FormState& f, ImportState& imp,
                              const FormSubmitHooks& hooks) {
    // 路径解析：service 注入优先（生产 = main 注入的 defaultPaths，
    // 测试 = 临时目录注入），未注入（service 为 null 的防御分支）回落
    // defaultPaths()——与设置页/引导条的路径口径一致
    settings_sync::Paths paths = settings_sync::defaultPaths();
    ClaudeService* svc = ClaudeService::global();
    if (svc && !svc->settingsSyncPaths().settingsFile.empty()) {
        paths = svc->settingsSyncPaths();
    }

    // 读文件：不存在 → toast，不弹窗
    std::error_code ec;
    if (!std::filesystem::exists(paths.settingsFile, ec) || ec) {
        hooks.toast("导入失败",
                    "未找到 ~/.claude/settings.json（" +
                        paths.settingsFile.string() + "）",
                    true);
        return;
    }
    std::ifstream in(paths.settingsFile, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    // 整段 settings.json 没有 env 键 = 没有可导入的配置（区别于粘贴导入
    // 认裸 env 对象的形状启发：文件只会是 settings.json 形状），给明确
    // 提示而非共享解析器的「键名非法: permissions」误报
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

    // 全自动「粘贴导入」：新建表单 → 解析结果 → 应用 → 名称建议
    openCreateForm(f, imp);
    imp.importPreview_ = std::move(parsed).value();
    imp.importReady_ = true;
    doApplyImport(f, imp);       // 预填三页表单（结束时清 importReady_）
    imp.importReady_ = true;   // 保留高级 env 页预览标记（识别数 + 警告角标）
    f.formName_ = suggestImportName(imp.importPreview_.baseUrl);

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
    for (const auto& meta : modelSlotTable()) {   // 槽位表派生（CPX-08）
        const ModelSlot& s = f.formModels_.*meta.slot;
        if (!s.model.empty() || !s.name.empty()) { anySlot = true; break; }
    }
    if (anySlot) {
        auto r = serializeModelsConfig(f.formModels_);
        if (r.isFail()) {
            f.formError_ = "模型配置序列化失败: " + r.errorMessage();
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
            f.formError_ = "env 序列化失败: " + r.errorMessage();
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
        f.formError_ = "服务未就绪（ClaudeService 未注册）";
        return;
    }
    hooks.markDataDirty();   // 审计 perf [13]：写操作前即标脏（失败也不伤，下帧重查）
    if (f.formName_.empty() || f.formUrl_.empty()) {
        f.formError_ = "名称和 Base URL 不能为空";
        return;
    }
    // 批 2：悬空绑定防御（片段已删 / 快照未到位）——按未绑定保存，不留幽灵 id
    if (f.formSharedConfigId_ != 0 &&
        findSharedById(sharedConfigs, f.formSharedConfigId_) == nullptr) {
        f.formSharedConfigId_ = 0;
        f.formFrozenKeys_.clear();
    }
    // 批 2：新增同键拦截（设计 §2.3「拦截优于事后被覆盖，避免以为存上了」）——
    // 行键 ∈ 绑定片段 K 且非打开时自有存量（存量非破坏保留：冻结显示、
    // 解绑后恢复生效）；命中给与冻结行同源的提示
    if (!f.formFrozenKeys_.empty()) {
        for (const auto& row : f.formEnvRows_) {
            if (row.first.empty()) continue;
            const std::string nk = normalizedEnvKey(row.first);
            if (keyIn(f.formFrozenKeys_, nk) &&
                !keyIn(f.formOriginalOwnKeys_, nk)) {
                const SharedConfig* sc =
                    findSharedById(sharedConfigs, f.formSharedConfigId_);
                f.formError_ =
                    "环境变量 " + nk + " 由共享配置片段「" +
                    (sc != nullptr ? sc->name : std::string("?")) +
                    "」冻结（片段赢），请在共享配置片段里修改";
                return;
            }
        }
    }
    std::string modelsJson;
    std::string envJson;
    if (!serializeFormData(f, &modelsJson, &envJson)) return;

    if (f.formEditId_ == 0) {
        // 新建：API Key 必填
        if (f.formKey_.empty()) {
            f.formError_ = "新建 Profile 必须填写 API Key";
            return;
        }
        // 产品审计 2026-09-23 [6]：activateIfNoneActive=true——当前无激活
        // Profile 时 service 层自动激活这条（首次体验闭环）；已有激活则不抢占
        auto r = svc->createProfile(f.formName_, f.formUrl_, f.formKey_,
                                    f.formModels_.def.model, /*activateIfNoneActive=*/true,
                                    /*sharedConfigId=*/f.formSharedConfigId_,
                                    authFieldToText(f.formAuthField_),
                                    /*dialectOverride=*/f.formDialectOverride_,
                                    /*fallbackProfileId=*/f.formFallbackProfileId_);
        if (r.isFail()) {
            f.formError_ = r.errorMessage();
            return;
        }
        // createProfile 无槽位/env 参数（最小 diff 不动签名），二段补写
        if (!modelsJson.empty() || !envJson.empty()) {
            Profile p = r.value();
            p.model = f.formModels_.def.model;
            p.modelsConfigJson = modelsJson;
            p.extraEnvJson = envJson;
            auto u = svc->updateProfile(p);
            if (u.isFail()) {
                f.formError_ = "已创建，但模型/env 写入失败: " +
                               u.errorMessage();
                return;
            }
        }
        f.formOpen_ = false;
        hooks.selectProfile(r.value().id);
        // 审计 2026-09-23 [6]：按是否自动激活分文案——只说「已保存」会让
        // 用户以为点「启动代理」就能用（无激活时 Claude Code 直接 503）
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
        // 编辑：先取现有行（保留 keychain 引用等字段），再覆盖表单值
        auto got = svc->getProfile(f.formEditId_);
        if (got.isFail()) {
            f.formError_ = got.errorMessage();
            return;
        }
        Profile p = std::move(got).value();
        p.name = f.formName_;
        p.baseUrl = f.formUrl_;
        p.model = f.formModels_.def.model;   // 旧列与 default 槽位同步（§4.2）
        p.modelsConfigJson = modelsJson;
        p.extraEnvJson = envJson;
        p.apiKeyPlain = f.formKey_;   // 空 = 不换 key
        p.authField = authFieldToText(f.formAuthField_);
        p.sharedConfigId = f.formSharedConfigId_;   // 批 2：绑定/解绑随保存落库
        p.fallbackProfileId = f.formFallbackProfileId_;   // 批 3：备用随保存落库（自引用由服务层校验兜底）
        // 高级配置页接线（2026-09-30 batch-fix B）：把新下拉的值真正写进编辑路径
        //（不接线则下拉改动落不了盘）。对「不动该下拉」场景行为与接线前等价——
        // 现路径 p 来自 getProfile，已隐式保留既有覆盖（profile_repository.cpp
        // 读侧归一 + updateProfile 直写该字段），用例按「接线回归钉」定位
        p.dialectOverride = f.formDialectOverride_;
        auto r = svc->updateProfile(p);
        if (r.isFail()) {
            f.formError_ = r.errorMessage();
            return;
        }
        f.formOpen_ = false;
        hooks.toast("已保存", "Profile「" + f.formName_ + "」已更新", false);
    }
}

namespace {

// ---- 基础页（§6.1 线框）----
void drawFormBasicTab(eui::Ui& ui, float w,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens,
                      bool isEdit, FormState& f) {
    ui.text("claude.form.basic.name.label")
        .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
        .text("名称").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.dialog.form.name")
        .position(28.0f, 122.0f).size(w - 56.0f, 40.0f)
        .value(f.formName_).placeholder("例如：官方 / 中转 A")
        .fontSize(14.0f).theme(themeTokens)
        .onChange([fp = &f](const std::string& v) { fp->formName_ = v; })
        // 审计 [21]：Esc=取消（关表单不提交，同「取消」按钮）；表单各输入框统一
        .onCancel([fp = &f] { fp->formOpen_ = false; })
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
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
        .build();

    ui.text("claude.form.basic.key.label")
        .position(28.0f, 244.0f).size(w - 56.0f, 16.0f)
        .text("API Key（存入 Keychain，不落数据库）")
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
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
        .build();

    // 认证字段（鉴权头三态；2026-09-27 profile-auth-field 设计 §6）：
    // 标签与灰字说明同行，下拉紧随其后；其下三块（副提示/错误/接管警告）
    // 整体下移 64px 让位
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

    // 「备用 Profile（failover）」下拉已整段迁入高级配置页（2026-09-30
    // batch-fix B，设计 §3.2①/②）——其后元素整体上移 66px（原 failover 块占
    // y 380..438：label 380 / 下拉 398+40）。认证字段下拉底（332+40=372）与
    // 下方提示行 380 间距 8px，与既有行距一致

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

    // 「导入当前配置」接管态警告（不阻断，仅提示；⚠ 图标 + 文字两元素）
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

// ---- 高级配置页（2026-09-30 batch-fix B，设计 §3.2①）：方言覆盖下拉 +
// 备用 Profile 下拉（自基础页整段迁入：label/hint/候选归算/dropdown 逐字
// 保留，含 profiles 快照入参与悬空归一逻辑；节点 id 同步保留——无头用例
// 经 claude.form.basic.fallback.dd.* 导航锚定位，改名即测试返工）----
// profiles：备用 Profile 下拉的数据源（编辑态排除自身；悬空/自引用逐帧
// 归一为「无」，不把幽灵 id 留在表单态里传播——同批 2 片段绑定的归一口径）
void drawFormAdvancedTab(eui::Ui& ui, float w,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& themeTokens,
                         FormState& f,
                         const std::vector<Profile>& profiles) {
    // 方言覆盖（审计 CPX-13 的 UI 侧出口）：idx↔值映射 0↔""（未覆盖）/
    // 1↔anthropic_default / 2↔opencode_zen。items 显示名与详情 pane 文案共用
    // dialectKindDisplayName（单一来源，不手抄字符串）；onChange 直接写规范
    // 文本（dialectKindToText 产物，映射表以其为源），DB 层
    // normalizeDialectOverride 再兜底（双层保险，符合 CPX-13「写侧归一」）
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
        // 选中态逐帧归算：经 dialectKindFromText 反查 idx（空串/未知 → 0，
        // 读侧兜底同口径）
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

    // 备用 Profile（failover，设计 §3.4/§3.7）：0 = 不启用。启用后主 Profile
    // 返回可重试错误时请求换发备用（§3.4 表单提示——可能按另一家计费在
    // 副提示里点明）。选项 idx 0 = 无；其余 = 候选 profile 表序 +1；编辑态
    // 排除自身（自引用由服务层 updateProfile 校验兜底，这里列表直接不给）
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
        // 选中态逐帧归算；悬空（备用已删 / 快照未到位）归一为无（幂等）
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
}

// ---- 模型映射页（槽位逐行 × model/name，§6.1 线框；行 = 槽位表派生，CPX-08）----
void drawFormModelsTab(eui::Ui& ui, float w,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       FormState& f) {
    const auto slotTable = modelSlotTable();

    // 表头
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
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();
        // 显示名留空 → placeholder 渲染派生预览（§6.1）
        const std::string derived = deriveDisplayName(slot.model);
        components::input(ui, "claude.form.slot." + idx + ".name")
            .position(492.0f, y).size(240.0f, 36.0f)
            .value(slot.name)
            .placeholder(derived.empty() ? "（自动派生）" : derived)
            .fontSize(13.0f).theme(themeTokens)
            .onChange([fp = &f, m = row.slot](const std::string& v) {
                (fp->formModels_.*m).name = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
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

// ---- 高级 env 页（键值编辑器 + 共享配置绑定 + cc-switch 粘贴导入，§6.1）----
// ponytail: 可见行数上限 5（760×600 弹窗高度约束），超出部分保存在
// 状态里全量保存；需要编辑超长列表时再引入 scrollView
//
// 批 2（设计 §2.10）：顶部一行 = 共享配置绑定下拉（与 env 标题同行、右对齐，
// 不挤动下方行几何）；绑定后 K 键行置灰只读 + chip「来自片段 X」。
void drawFormEnvTab(eui::Ui& ui, float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& themeTokens,
                    FormState& f, ImportState& imp,
                    const std::vector<SharedConfig>& sharedConfigs) {
    constexpr size_t kMaxVisibleRows = 5;

    // ---- 批 2：共享配置绑定行（顶部一行，设计 §2.10「profile 绑定」）----
    ui.text("claude.form.env.label")
        .position(28.0f, 104.0f).size(348.0f, 16.0f)
        .text("额外环境变量（原样写入 settings.json env 块）")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    ui.text("claude.form.env.shared.label")
        .position(384.0f, 104.0f).size(58.0f, 16.0f)
        .text("共享配置：").fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    // 片段下拉：idx 0 = 未绑定；其余 = 片段表序 +1。绑定保存时随表单落库
    //（新建 = createProfile 尾参，编辑 = updateProfile 的 sharedConfigId）
    {
        std::vector<std::string> items;
        items.reserve(sharedConfigs.size() + 1);
        items.emplace_back("无（不绑定片段）");
        for (const auto& sc : sharedConfigs) items.push_back(sc.name);
        // 选中态逐帧归算；悬空绑定（片段已删 / 快照未到位）归一为未绑定
        //（幂等：值为 0 后不再重设）——不把幽灵 id 留在表单态里传播
        int selIdx = 0;
        if (f.formSharedConfigId_ != 0) {
            for (size_t k = 0; k < sharedConfigs.size(); ++k) {
                if (sharedConfigs[k].id == f.formSharedConfigId_) {
                    selIdx = static_cast<int>(k) + 1;
                    break;
                }
            }
            if (selIdx == 0) {
                f.formSharedConfigId_ = 0;
                f.formFrozenKeys_.clear();
            }
        }
        ui.stack("claude.form.env.shared.ddwrap")
            .position(446.0f, 92.0f)
            .size(w - 446.0f - 28.0f, 32.0f)
            .content([&] {
                components::dropdown(ui, "claude.form.env.shared.dd")
                    .size(w - 446.0f - 28.0f, 32.0f)
                    .theme(themeTokens)
                    .items(std::move(items))
                    .selected(selIdx)
                    .open(f.formSharedDdOpen_)
                    .onOpenChange([fp = &f](bool v) { fp->formSharedDdOpen_ = v; })
                    .onChange([fp = &f, scs = &sharedConfigs](int idx) {
                        if (idx <= 0) {
                            fp->formSharedConfigId_ = 0;
                            fp->formFrozenKeys_.clear();
                            return;
                        }
                        const size_t k = static_cast<size_t>(idx - 1);
                        if (k < scs->size()) {
                            const SharedConfig& sc = (*scs)[k];
                            fp->formSharedConfigId_ = sc.id;
                            // K 键集当场重算（片段赢预览/新增同键拦截判定源）
                            fp->formFrozenKeys_ = sharedFrozenKeys(sc);
                        }
                    })
                    .build();
            })
            .build();
    }
    // 冻结行 chip 的片段名（K 键行共用的来源标记）
    const SharedConfig* boundSc =
        findSharedById(sharedConfigs, f.formSharedConfigId_);
    const std::string boundScName =
        boundSc != nullptr ? boundSc->name : std::string();
    const std::string chipText =
        "来自片段 " + truncateUtf8(boundScName, 18);

    const size_t visible =
        f.formEnvRows_.size() < kMaxVisibleRows ? f.formEnvRows_.size()
                                                : kMaxVisibleRows;
    for (size_t i = 0; i < visible; ++i) {
        const float y = 124.0f + static_cast<float>(i) * 38.0f;
        const std::string idx = std::to_string(i);
        // 片段赢预览（设计 §2.3/§2.10）：行键 ∈ 绑定片段 K → 置灰只读 + chip。
        // EUI-NEO input 无禁改态（components/ 下无 disabled），DevDesk 侧用
        // 灰字 text+chip 渲染（非 input），只读语义由「不可编辑控件」直接
        // 表达；值 = profile 自身保留值（非破坏，解绑后恢复生效）
        const bool frozen = !f.formEnvRows_[i].first.empty() &&
            keyIn(f.formFrozenKeys_,
                  normalizedEnvKey(f.formEnvRows_[i].first));
        if (frozen) {
            ui.rect("claude.form.env.row." + idx + ".frozen.bg")
                .position(28.0f, y).size(644.0f, 32.0f)
                .color(t.colors.surfaceElevated)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderMuted)
                .build();
            ui.text("claude.form.env.row." + idx + ".frozen.key")
                .position(40.0f, y).size(196.0f, 32.0f)
                .text(f.formEnvRows_[i].first)
                .fontSize(12.0f).lineHeight(32.0f)
                .color(t.colors.textTertiary)
                .build();
            ui.rect("claude.form.env.row." + idx + ".frozen.chip.bg")
                .position(248.0f, y + 5.0f).size(130.0f, 22.0f)
                .color(t.colors.surfaceElevated).radius(t.sizes.radiusPill)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.text("claude.form.env.row." + idx + ".frozen.chip")
                .position(248.0f, y + 5.0f).size(130.0f, 22.0f)
                .text(chipText)
                .fontSize(10.0f).lineHeight(22.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            ui.text("claude.form.env.row." + idx + ".frozen.val")
                .position(386.0f, y).size(258.0f, 32.0f)
                .clip()
                .text(f.formEnvRows_[i].second)
                .fontSize(12.0f).lineHeight(32.0f)
                .color(t.colors.textTertiary)
                .build();
            continue;
        }
        components::input(ui, "claude.form.env.row." + idx + ".key")
            .position(28.0f, y).size(220.0f, 32.0f)
            .value(f.formEnvRows_[i].first).placeholder("ENV_KEY")
            .fontSize(12.0f).theme(themeTokens)
            .onChange([fp = &f, i](const std::string& v) {
                fp->formEnvRows_[i].first = v;
            })
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();
        // 变量名识别（2026-09-24，官方 env-vars 文档全量清单）：
        // 已知 → hover 显示说明 tooltip；未知且非空 → 红边框提示
        //（仍可导入，键名格式校验已在解析侧放开）
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
            .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();
        components::button(ui, "claude.form.env.row." + idx + ".del")
            .position(682.0f, y).size(50.0f, 32.0f)
            .icon(0xF00D)  // fa-xmark（✕ 换 FA）
            .fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover,
                    t.colors.active)
            .textColor(t.colors.error)
            .iconColor(t.colors.error)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([fp = &f, i] {
                if (i < fp->formEnvRows_.size()) {
                    fp->formEnvRows_.erase(fp->formEnvRows_.begin() +
                                           static_cast<long>(i));
                }
            })
            .build();
    }

    // 底部：添加按钮 + 溢出说明
    components::button(ui, "claude.form.env.add")
        .position(28.0f, 314.0f).size(110.0f, 30.0f)
        .text("+ 添加变量").fontSize(12.0f)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusSmall)
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

    // ---- 粘贴导入（cc-switch / settings.json）----
    ui.text("claude.form.import.label")
        .position(28.0f, 356.0f).size(w - 56.0f, 16.0f)
        .text("── 从 cc-switch / settings.json 粘贴导入 ──")
        .fontSize(11.0f).lineHeight(16.0f)
        .color(t.colors.textTertiary).build();
    components::input(ui, "claude.form.import.text")
        .position(28.0f, 376.0f).size(w - 56.0f, 82.0f)
        .value(imp.importText_)
        .placeholder(R"({ "env": { "ANTHROPIC_BASE_URL": "...", ... } }  粘贴整段 settings.json 或仅 env 对象)")
        .multiline(true).scrollbar(true)
        .fontSize(12.0f).theme(themeTokens)
        .onChange([ip = &imp](const std::string& v) { ip->importText_ = v; })
        .onCancel([fp = &f] { fp->formOpen_ = false; })  // 审计 [21]：Esc=取消
        .build();

    components::button(ui, "claude.form.import.parse")
        .position(28.0f, 468.0f).size(110.0f, 32.0f)
        .text("解析导入").fontSize(12.0f)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderDefault)
        .onClick([ip = &imp] { doParseImport(*ip); })
        .build();
    if (imp.importReady_) {
        components::button(ui, "claude.form.import.apply")
            .position(148.0f, 468.0f).size(110.0f, 32.0f)
            .text("应用到表单").fontSize(12.0f)
            .colors(t.colors.primary, t.colors.primaryHover,
                    t.colors.primaryPressed)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radiusSmall)
            .onClick([fp = &f, ip = &imp] { doApplyImport(*fp, *ip); })
            .build();
    }

    // 解析结果：失败红字 / 成功预览（槽位/env 识别数 + 别名/未识别提示）
    //（✕/⚠ 换 FA：图标/文字拆两个元素）
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
        // 警告角标透传（§4.3 规则 6）：别名映射 / 未识别键 / 解析警告，
        // 最多 3 条（弹窗高度约束——再往下会压到底部按钮；全量信息在
        // 保存后的 settings.json 与日志可查）
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
        // 角标与底部校验错误行（dialog 里 y=h-104，formTab_!=0 时画）
        // 同屏重叠：有校验错误时让位，只显示校验错误（提交失败更可行动；
        // 角标信息全量在保存后的 settings.json 与日志可查）
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
    const float h = 600.0f;
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

            // tabs（§6.1：基础 / 模型映射 / 高级 env；2026-09-30 batch-fix B
            // 尾插第 4 页「高级配置」——前三页序是既有拍板，尾插保住
            // form.tabs.hit.0/1/2 既有测试导航零破坏，D-B1）。
            // TabsBuilder 无 position，套 stack 定位（字号下拉同款包装）
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
                // 高级配置（2026-09-30 batch-fix B）：catch-all else 前插 ==3
                // 分支，0/1/2 分支语义不动
                drawFormAdvancedTab(ui, w, t, themeTokens, f, profiles);
            } else {
                drawFormEnvTab(ui, w, t, themeTokens, f, imp, sharedConfigs);
            }

            // 错误信息（基础页之外也要能看到校验错误）
            if (f.formTab_ != 0) {
                ui_kit::drawDialogText(ui, "claude.dialog.form.error", w, f.formError_,
                                       12.0f, t.colors.error,
                                       {.y = h - 104.0f, .height = 18.0f,
                                        .lineHeight = 18.0f, .skipIfEmpty = true});
            }

            // 底部按钮
            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = 14.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [fp = &f] { fp->formOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = isEdit ? "保存" : "创建";
            confirm.fontSize = 14.0f;
            confirm.style = ui_kit::DialogButtonStyle::Primary;
            confirm.onClick = [fp = &f, hooks, scs = &sharedConfigs] {
                submitForm(*fp, hooks, *scs);
            };
            ui_kit::drawDialogButtons(ui, "claude.dialog.form", w, h, cancel, confirm, t,
                                      themeTokens);
    });
}

// ============ 删除确认弹窗 ============
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
            // 2026-09-28：文本加 wrap + 高 60（同共享片段删除确认弹窗口径）
            // —— 原单行写死 40px 不换行，长 Profile 名把正文挤出弹窗右缘
            //（EUI 文本不裁剪，超宽直接画出弹窗）
            ui_kit::drawDialogText(
                ui, "claude.dialog.delete.msg", w,
                "将删除 Profile「" + f.deleteTargetName_ +
                    "」及其 Keychain 中的 API Key，不可恢复。",
                t.sizes.fontSizeSmall, t.colors.textSecondary,
                {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f,
                 .wrap = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = 14.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [fp = &f] { fp->deleteConfirmOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "确认删除";
            confirm.fontSize = 14.0f;
            confirm.style = ui_kit::DialogButtonStyle::DangerOutline;
            confirm.onClick = [fp = &f, onConfirmDelete] {
                const int64_t id = fp->deleteTargetId_;
                fp->deleteConfirmOpen_ = false;
                onConfirmDelete(id);
            };
            ui_kit::drawDialogButtons(ui, "claude.dialog.delete", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// ============ 共享配置片段弹窗（批 2，设计 §2.10）============
// 独立状态 SharedConfigDialogState（待拍板②：不复用 FormState，不污染 profile
// 表单状态）；键值行写入模式照 formEnvRows_（kMaxVisibleRows 同款）。
void openSharedCreate(SharedConfigDialogState& s) {
    s.editId_ = 0;
    s.name_.clear();
    s.envRows_.clear();
    s.error_.clear();
    s.open_ = true;
}

void openSharedEdit(SharedConfigDialogState& s, const SharedConfig& c) {
    s.editId_ = c.id;
    s.name_ = c.name;
    s.error_.clear();
    s.envRows_.clear();
    auto ee = parseExtraEnv(c.extraEnvJson);
    if (ee.isOk()) {
        for (const auto& [k, v] : ee.value()) s.envRows_.emplace_back(k, v);
    }
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
    std::map<std::string, std::string> envMap;   // 空键行跳过（同表单口径）
    for (const auto& row : s.envRows_) {
        if (row.first.empty()) continue;
        envMap[row.first] = row.second;
    }
    std::string envJson;
    if (!envMap.empty()) {
        auto r = serializeExtraEnv(envMap);
        if (r.isFail()) {
            s.error_ = "env 序列化失败: " + r.errorMessage();
            return;
        }
        envJson = r.value();
    }
    if (s.editId_ == 0) {
        auto r = svc->createSharedConfig(s.name_, envJson);
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
        c.extraEnvJson = envJson;
        auto r = svc->updateSharedConfig(c);
        if (r.isFail()) {
            s.error_ = r.errorMessage();
            return;
        }
        s.open_ = false;
        // 服务层已处理「代理在跑且激活 profile 绑定了它 → 立即 re-takeover」
        //（§2.6，claude_service.cpp 的 updateSharedConfig）——UI 不重复写
        hooks.toast("已保存",
                    "共享配置片段「" + s.name_ + "」已更新，绑定 Profile 下次接管生效",
                    false);
    }
}

// 共享配置片段编辑弹窗（name 行 + 键值编辑器；编辑态底部多一枚「删除」）
void drawSharedConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens,
                            SharedConfigDialogState& s,
                            const SharedConfigSubmitHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    const float h = 460.0f;
    const bool isEdit = (s.editId_ != 0);
    constexpr size_t kMaxVisibleRows = 5;

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

        // 片段名
        ui.text("claude.dialog.shared.name.label")
            .position(28.0f, 64.0f).size(w - 56.0f, 16.0f)
            .text("片段名（共享给所有 Profile 的 env 装饰层，不含密钥/Base URL）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.shared.name")
            .position(28.0f, 82.0f).size(w - 56.0f, 36.0f)
            .value(s.name_).placeholder("例如：企业内部网关 env")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([sp = &s](const std::string& v) { sp->name_ = v; })
            .onCancel([sp = &s] { sp->open_ = false; })   // Esc=取消（同表单各输入框）
            .build();

        // 键值编辑器（kMaxVisibleRows 同款；行 = 该片段的冻结键集 K 的来源）
        ui.text("claude.dialog.shared.env.label")
            .position(28.0f, 128.0f).size(w - 56.0f, 16.0f)
            .text("额外环境变量（绑定它的 Profile 将冻结这些键，片段单点决定）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        const size_t visible = s.envRows_.size() < kMaxVisibleRows
            ? s.envRows_.size() : kMaxVisibleRows;
        for (size_t i = 0; i < visible; ++i) {
            const float y = 148.0f + static_cast<float>(i) * 36.0f;
            const std::string idx = std::to_string(i);
            components::input(ui, "claude.dialog.shared.env.row." + idx + ".key")
                .position(28.0f, y).size(220.0f, 30.0f)
                .value(s.envRows_[i].first).placeholder("ENV_KEY")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([sp = &s, i](const std::string& v) {
                    sp->envRows_[i].first = v;
                })
                .onCancel([sp = &s] { sp->open_ = false; })
                .build();
            if (!s.envRows_[i].first.empty() &&
                envVarHint(s.envRows_[i].first) == nullptr) {
                ui.rect("claude.dialog.shared.env.row." + idx + ".key.unknown")
                    .position(28.0f, y).size(220.0f, 30.0f)
                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .border(1.0f, t.colors.error)
                    .radius(t.sizes.radiusSmall)
                    .build();
            }
            components::input(ui, "claude.dialog.shared.env.row." + idx + ".value")
                .position(258.0f, y).size(414.0f, 30.0f)
                .value(s.envRows_[i].second).placeholder("值（字符串）")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([sp = &s, i](const std::string& v) {
                    sp->envRows_[i].second = v;
                })
                .onCancel([sp = &s] { sp->open_ = false; })
                .build();
            components::button(ui, "claude.dialog.shared.env.row." + idx + ".del")
                .position(682.0f, y).size(50.0f, 30.0f)
                .icon(0xF00D)   // fa-xmark（同 profile 表单 env 行）
                .fontSize(12.0f)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.error)
                .iconColor(t.colors.error)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderDefault)
                .onClick([sp = &s, i] {
                    if (i < sp->envRows_.size()) {
                        sp->envRows_.erase(sp->envRows_.begin() +
                                           static_cast<long>(i));
                    }
                })
                .build();
        }
        components::button(ui, "claude.dialog.shared.env.add")
            .position(28.0f, 330.0f).size(110.0f, 30.0f)
            .text("+ 添加变量").fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([sp = &s] { sp->envRows_.emplace_back(); })
            .build();
        if (s.envRows_.size() > kMaxVisibleRows) {
            ui.text("claude.dialog.shared.env.overflow")
                .position(150.0f, 336.0f).size(w - 150.0f - 28.0f, 16.0f)
                .text("另有 " + std::to_string(s.envRows_.size() -
                                               kMaxVisibleRows) +
                      " 项已保存（保存时全量写入，列表仅显示前 " +
                      std::to_string(kMaxVisibleRows) + " 项）")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
        }
        if (!s.error_.empty()) {
            ui.text("claude.dialog.shared.error")
                .position(28.0f, 368.0f).size(w - 56.0f, 18.0f)
                .text(s.error_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.error).build();
        }

        // 底部按钮：编辑态 = 删除（左）/ 取消 / 保存；新建态 = 取消 / 创建。
        // 删除键另开删除确认弹窗（删除 = 同事务解绑绑定它的 Profile）
        const float py = h - 60.0f;
        if (isEdit) {
            components::button(ui, "claude.dialog.shared.delete")
                .position(28.0f, py).size(88.0f, 40.0f)
                .text("删除").fontSize(14.0f)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.error)
                .radius(t.sizes.radius)
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
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([sp = &s] { sp->open_ = false; })
            .build();
        components::button(ui, "claude.dialog.shared.confirm")
            .position(confirmX, py).size(pairW, 40.0f)
            .text(isEdit ? "保存" : "创建").fontSize(14.0f)
            .colors(t.colors.primary, t.colors.primaryHover,
                    t.colors.primaryPressed)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radius)
            .onClick([sp = &s, hooks] { submitSharedForm(*sp, hooks); })
            .build();
    });
}

// 片段删除确认弹窗（删除 = 同事务解绑绑定它的 profile，设计 §2.6；
// 「N 个 Profile 已解绑」计数由模块侧在确认回调里给出）
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
        cancel.style = ui_kit::DialogButtonStyle::Secondary;
        cancel.onClick = [sp = &s] { sp->deleteConfirmOpen_ = false; };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认删除";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::DangerOutline;
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
