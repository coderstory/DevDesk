// src/module_claude_proxy/claude_ui_detail.cpp
// 右栏三 pane（基础信息 / 模型配置 / 环境变量）+ 操作按钮组；this 成员换成显式入参。
// 布局原则：容器可用高单一来源、贴底元素锚容器底、越界判定用真实裁剪下沿
//（本文件容器底 = bodyH，裁剪下沿 = drawDetailPanel 的 contentBottom）。
#include "claude_ui_detail.h"

#include "claude_ui_logic.h"
#include "env_var_hints.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/switch.h>
#include <components/tabs.h>
#include <components/tooltip.h>
#include <core/component_theme.h>
#include <core/logging.h>
#include <core/window/window_backend.h>
#include <ui_kit/wtab.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>

#include <ui_kit/light_toast.h>

namespace module::claude {

namespace {

// 详情面板动作区几何（单一来源）：动作块 = 61（分隔线/按钮行 36/副提示/分隔线）；
// 其上 20 间距、其下 24 呼吸 → 预留 85。由 drawDetailActions 绘制推出，改绘制须同步。
constexpr float kActionBlockH = 65.0f;
constexpr float kActTopGap = 20.0f;     // 内容框底 → actY
constexpr float kActBottomPad = 24.0f;  // actY+65 → 面板底
constexpr float kActReserve = kActionBlockH + kActBottomPad;  // 89

// 复制按钮 toast 单实例状态（onClick 设，drawDetailBasic 每帧查并渲染）
std::string g_copyToastMsg;
std::chrono::steady_clock::time_point g_copyToastTime{};

// 模型映射域的 env 键：键集从槽位表派生（与写入点同源），此处仅语义别名。
bool isModelMappingEnvKey(const std::string& key) {
    return isModelSlotEnvKey(key);
}

}  // namespace

const char* envSourceLabel(EnvSource s) {
    // 公开单一来源（.h 声明），不手抄文案
    switch (s) {
        case EnvSource::Profile: return "本 Profile";
        case EnvSource::Managed: return "托管键";
    }
    return "?";
}

float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v,
                      float contentBottom) {
    (void)contentBottom;  // 固定字段行数，高度与容器无关
    // 基础信息 pane：名称 + Base URL + API Key + 接管
    //（UAT5：原「未激活」徽章删除——选中语义已由列表卡绿圈白勾承担，徽章属噪音）
    // 文本必须带 verticalAlign=Center，否则 runtime 从行框顶沿起排不居中；只加对齐，行距/行宽不变。
    ui.text("claude.field.name.label")
        .position(x0, y0).size(w * 0.5f - 8.0f, 14.0f)
        .text("名称").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.name.bg")
        .position(x0, y0 + 18.0f).size(w * 0.5f - 8.0f, 24.0f)
        .color(t.colors.surface).radius(t.sizes.radiusLarge).build();
    // S3.4 值盒收窄让位右侧复制按钮
    ui.text("claude.field.name.value")
        .position(x0 + 12.0f, y0 + 18.0f)
        .size(w * 0.5f - 40.0f, 24.0f)
        .text(v.nameValue).fontSize(core::theme::scaled(14.0f)).lineHeight(core::theme::scaled(24.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    {
        const float copyX = x0 + w - 28.0f;
        const std::string copyVal = v.nameValue;
        ui.stack("claude.field.name.copy")
            .position(copyX, y0 + 18.0f)
            .size(24.0f, 24.0f)
            .content([&] {
                ui.rect("claude.field.name.copy.bg")
                    .size(24.0f, 24.0f)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.colors.primarySoftBg,
                            t.colors.hover)
                    .radius(4.0f)
                    .onClick([cv = copyVal] {
                        core::window::setClipboardText(cv);
                        g_copyToastMsg = "已复制";
                        g_copyToastTime = std::chrono::steady_clock::now();
                    })
                    .build();
                ui.text("claude.field.name.copy.icon")
                    .size(24.0f, 24.0f)
                    .icon(0xF0C5)
                    .fontSize(core::theme::scaled(14.0f))
                    .lineHeight(core::theme::scaled(24.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }
    const float urlY = y0 + 56.0f;
    ui.text("claude.field.url.label")
        .position(x0, urlY).size(w, 14.0f)
        .text("Base URL").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.url.bg")
        .position(x0, urlY + 18.0f).size(w, 28.0f)
        .color(t.colors.surface).radius(t.sizes.radiusLarge).build();
    // S3.4 值盒收窄让位右侧复制按钮
    ui.text("claude.field.url.value")
        .position(x0 + 12.0f, urlY + 18.0f).size(w - 40.0f, 28.0f)
        .text(v.urlValue).fontSize(t.sizes.fontSizeSmall).lineHeight(28.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    {
        const float copyX = x0 + w - 28.0f;
        const std::string copyVal = v.urlValue;
        ui.stack("claude.field.url.copy")
            .position(copyX, urlY + 20.0f)
            .size(24.0f, 24.0f)
            .content([&] {
                ui.rect("claude.field.url.copy.bg")
                    .size(24.0f, 24.0f)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.colors.primarySoftBg,
                            t.colors.hover)
                    .radius(4.0f)
                    .onClick([cv = copyVal] {
                        core::window::setClipboardText(cv);
                        g_copyToastMsg = "已复制";
                        g_copyToastTime = std::chrono::steady_clock::now();
                    })
                    .build();
                ui.text("claude.field.url.copy.icon")
                    .size(24.0f, 24.0f)
                    .icon(0xF0C5)
                    .fontSize(core::theme::scaled(14.0f))
                    .lineHeight(core::theme::scaled(24.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }
    const float keyY = urlY + 56.0f;
    // §5 API Key 显隐：本字段为只读脱敏提示（apiKeyHint，非明文），已防肩窥；
    // 真正 type=password 明文/掩码切换属编辑表单（claude_ui_form.cpp，不在 W7
    // 文件清单），W7 范围内无可切内容，留待表单工单。
    ui.text("claude.field.token.label")
        .position(x0, keyY).size(w, 14.0f)
        .text("API Key").fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.token.bg")
        .position(x0, keyY + 18.0f).size(w, 32.0f)
        .color(t.colors.surface).radius(t.sizes.radiusLarge).build();
    // S3.4 值盒收窄让位右侧复制按钮
    ui.text("claude.field.token.value")
        .position(x0 + 12.0f, keyY + 18.0f).size(w - 40.0f, 32.0f)
        .text(v.tokenValue).fontSize(t.sizes.fontSizeSmall).lineHeight(32.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary).build();
    {
        const float copyX = x0 + w - 28.0f;
        const std::string copyVal = v.tokenValue;
        ui.stack("claude.field.token.copy")
            .position(copyX, keyY + 22.0f)
            .size(24.0f, 24.0f)
            .content([&] {
                ui.rect("claude.field.token.copy.bg")
                    .size(24.0f, 24.0f)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.colors.primarySoftBg,
                            t.colors.hover)
                    .radius(4.0f)
                    .onClick([cv = copyVal] {
                        core::window::setClipboardText(cv);
                        g_copyToastMsg = "已复制";
                        g_copyToastTime = std::chrono::steady_clock::now();
                    })
                    .build();
                ui.text("claude.field.token.copy.icon")
                    .size(24.0f, 24.0f)
                    .icon(0xF0C5)
                    .fontSize(core::theme::scaled(14.0f))
                    .lineHeight(core::theme::scaled(24.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }
    ClaudeService* svc = v.svc;
    const bool ccManaged = svc && svc->isClaudeCodeManaged();
    // 接管失败与上层覆盖警告都要可见，不以 ccManaged 为展示门；读内存 getter（无 IO）
    const std::vector<std::string> ccWarnings =
        svc ? svc->lastSyncWarnings() : std::vector<std::string>();
    const std::string ccPath =
        settings_sync::defaultPaths().settingsFile.string();
    const float toY = keyY + 56.0f;
    ui.text("claude.field.takeover.value")
        .position(x0, toY).size(w, 16.0f)
        .text(ccManaged ? ("● Claude Code 已接管 " + ccPath)
                        : std::string("○ Claude Code 未接管（启动代理后自动接管，停止时还原）"))
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(ccManaged ? t.colors.success : t.colors.textTertiary).build();
    if (!ccWarnings.empty()) {
        // 摘要 = 首条警告 + 总条数；「详见设置页」恒成立（设置页警告行只看警告）
        std::string warnText = ccWarnings.front();
        if (ccWarnings.size() > 1) {
            warnText += "（共 " +
                std::to_string(ccWarnings.size()) + " 条";
            warnText += "，详见设置页）";
        }
        ui.text("claude.field.takeover.warn.icon")
            .position(x0, toY + 16.0f).size(14.0f, 16.0f).icon(0xF071)
            .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
        ui.text("claude.field.takeover.warn")
            .position(x0 + 18.0f, toY + 16.0f).size(w - 18.0f, 16.0f)
            .text(warnText)
            .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(16.0f))
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
    }
    // 复制按钮轻量 toast：每帧渲染，timer 1.5s 自动消失
    {
        std::string toastMsg;
        if (!g_copyToastMsg.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<float>(now - g_copyToastTime).count();
            if (elapsed < ui_kit::kLightToastDurationSec) {
                toastMsg = g_copyToastMsg;
            } else {
                g_copyToastMsg.clear();
            }
        }
        ui_kit::showLightToast(ui, "claude.detail.copy.toast", toastMsg,
                               x0 + w * 0.5f, y0 + 120.0f, t);
    }
    return toY + (!ccWarnings.empty() ? 32.0f : 16.0f);
}

float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v,
                       const DetailActions& a, float contentBottom) {
    (void)contentBottom;  // 槽位表行数固定，与容器高无关
    const Profile* shown = v.shown;
    // 模型映射 pane：槽位逐行（widget id 序号 = 表序）
    // 每行右端「1M」开关：勾态 = 槽位支持 && 串尾有标记；可用 = 支持且 base 非空，
    // 不可用行置灰但不隐藏、不挂 onChange。存储是「基础名 + 可选 [1M]」单字符串。
    ModelsConfig mc{};
    if (shown && !shown->modelsConfigJson.empty()) {
        auto mr = parseModelsConfig(shown->modelsConfigJson);
        if (mr.isOk()) mc = mr.value();
    }
    // 开关列宽 76（轨道 30 + 间隙 + 标签 + 内缩），值列相应收窄；色走 componentTokens。
    constexpr float kOneMColW = 76.0f;
    const components::SwitchStyle oneMStyle(core::theme::componentTokensWeChat(t));
    components::SwitchStyle oneMDisabledStyle = oneMStyle;
    // 禁用态灰：轨道/标签走 textDisabled（更深的灰轨道承担「禁用」语义），滑块
    // 走 WeChat cardBg 白（与启用态白 knob 同源），与「关」态轨道色可辨；
    //（UAT5 米黄清理：原 knob surfaceElevated 暖米 → WeChat cardBg）
    oneMDisabledStyle.off = t.colors.textDisabled;
    oneMDisabledStyle.on = t.colors.textDisabled;
    oneMDisabledStyle.knob = t.colors.surface;
    oneMDisabledStyle.text = t.colors.textDisabled;
    float sy = y0;
    // §5「1M」tooltip：hover 显示「上下文窗口 1M tokens」。TooltipBuilder 需 bounds/
    // metrics 绑定（待 EUI），暂以静态说明行落地视觉意图（不占额外行高预算外溢）。
    ui.text("claude.slot.onem.hint")
        .position(x0, sy).size(w, 14.0f)
        .text("1M = 上下文窗口 1M tokens（勾选则该槽位按 1M 上下文窗口请求）")
        .fontSize(core::theme::scaled(11.0f)).lineHeight(core::theme::scaled(14.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    sy += 20.0f;
    int slotIdx = 0;
    for (const auto& meta : modelSlotTable()) {
        const ModelSlot& slot = mc.*meta.slot;
        const bool filled = !slot.model.empty();
        const std::string idx = std::to_string(slotIdx);
        // §5 模型映射行：左对齐名 + 右侧开关；行 hover 浅灰（补充项），无边框
        ui.rect("claude.slot.bg." + idx)
            .position(x0, sy).size(w, 32.0f)
            .states(t.colors.surface,
                    t.colors.hover,
                    t.colors.primarySoftBg)
            .radius(t.sizes.radiusLarge).build();
        ui.text("claude.slot.name." + idx)
            .position(x0 + 12.0f, sy).size(80.0f, 32.0f)
            .text(meta.label).fontSize(core::theme::scaled(12.0f)).lineHeight(core::theme::scaled(32.0f))
            // 必须 verticalAlign=Center 才垂直居中；行框几何（32/38 已标定）不动。
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ui.text("claude.slot.val." + idx)
            .position(x0 + 100.0f, sy).size(w - 112.0f - kOneMColW, 32.0f)
            // 显示层剥 [1M]（开关已表达 1M，值再带即重复）；只动显示，存储与发往上游仍带标记。
            .text(filled ? deriveDisplayName(slot.model)
                         : std::string("未配置"))
            .fontSize(core::theme::scaled(13.0f)).lineHeight(core::theme::scaled(32.0f))
            .verticalAlign(eui::VerticalAlign::Center)
            // 长模型名单行省略号（显式 maxWidth 是前提），clip 保留作绘制范围兜底。
            .maxWidth(w - 112.0f - kOneMColW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(filled ? t.colors.textPrimary
                          : t.colors.textTertiary).build();
        // 「1M」开关（不支持/空 base → 不传 onChange）
        const bool oneMEnabled = meta.supportsOneM && filled;
        const bool oneMChecked = meta.supportsOneM && hasOneMMarker(slot.model);
        const int64_t shownId = shown != nullptr ? shown->id : 0;
        const std::size_t slotIndex = static_cast<std::size_t>(slotIdx);
        const std::function<void(bool)> onOneM =
            (oneMEnabled && shown != nullptr && a.setOneM)
                ? std::function<void(bool)>(
                      [cb = a.setOneM, shownId, slotIndex](bool on) {
                          cb(shownId, slotIndex, on);
                      })
                : std::function<void(bool)>();
        components::toggleSwitch(ui, "claude.slot.onem." + idx)
            .position(x0 + w - kOneMColW - 4.0f, sy)
            .size(kOneMColW, 32.0f)
            .checked(oneMChecked)
            .text("1M")
            .fontSize(core::theme::scaled(11.0f))
            .trackSize(30.0f, 16.0f)
            .style(oneMEnabled ? oneMStyle : oneMDisabledStyle)
            .onChange(onOneM)
            .build();
        ++slotIdx;
        sy += 38.0f;
    }
    return sy;
}

float drawDetailEnv(eui::Ui& ui, float x0, float y0, float w,
                    const core::theme::Tokens& t, const DetailView& v,
                    float contentBottom, PaneRows* rows) {
    const Profile* shown = v.shown;
    // 环境变量 pane：生效 env（模型映射键除外）+ 每键来源
    // 这是**将写入的托管 env**，不是 settings.json 的 ground truth；来源由
    // buildProxyEnv 出参给出（合并规则只能一处，复刻必漂移）
    std::map<std::string, std::string> envMap;
    std::map<std::string, EnvSource> envSources;
    if (shown != nullptr) {
        // 本 pane 只列 profile 自身 env ∪ 托管键（片段不携带 env 键）。
        const int port = v.svc ? v.svc->configuredProxyPort() : 0;
        envMap = buildProxyEnv(shown->modelsConfigJson, shown->extraEnvJson,
                               port, &envSources);
        // 模型键出列须在行数预算之前（行数/另有 N 项/PaneRows 都按过滤后计）
        for (auto it = envMap.begin(); it != envMap.end();) {
            if (isModelMappingEnvKey(it->first)) {
                envSources.erase(it->first);
                it = envMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 行几何：全部从 fontScale 派生，禁写死像素
    // 行带 1 = round(键字号×1.7)，行带 2 = round(说明字号×1.6)，行卡 = 4+带1+2+带2+4，
    // 行距 = 行卡+6；键/值同带、来源/说明同带，两带 y 区间不相交。
    const float fs = core::theme::fontScale();
    const float keyFs = 12.0f * fs;    // 键名 / 值字号（设计基准 12px）
    const float smallFs = 11.0f * fs;  // 来源 / 说明 / 空态小字（设计基准 11px）
    const float line1H = std::round(keyFs * 1.7f);
    const float line2H = std::round(smallFs * 1.6f);
    const float rowH = 4.0f + line1H + 2.0f + line2H + 4.0f;
    const float pitch = rowH + 6.0f;
    const float overflowH = line2H + 2.0f;
    const float line1Y = 4.0f;                  // 行带 1 顶（相对行卡顶）
    const float line2Y = 4.0f + line1H + 2.0f;  // 行带 2 顶
    if (envMap.empty()) {
        // 空态两种成因分开说：未选 Profile vs 键全被模型过滤（后者当前不可达，保留分支）
        const float emptyH = line1H;
        ui.text("claude.env.empty")
            .position(x0, y0).size(w, emptyH)
            .text(shown == nullptr
                      ? "无可展示的 env（未选择 Profile）"
                      : "模型类键已在「模型映射」页签单独展示，本页暂无其它生效 env")
            .fontSize(keyFs).lineHeight(emptyH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        if (rows != nullptr) *rows = PaneRows{0, 0};
        return y0 + emptyH;
    }
    const float keyColW = std::min(220.0f, w * 0.45f);
    // 键名超宽走单行省略号（显式 maxWidth 是前提），clip 作几何兜底；来源列
    // 宽 120 够放标签，超出由 clip 硬裁。
    constexpr float kSrcColW = 120.0f;
    constexpr float kSrcGap = 12.0f;
    // 行数预算纯按内容框可用高推；放不下的走「另有 N 项」提示行。
    const float availH = std::max(0.0f, contentBottom - y0);
    size_t shownRows = std::min(
        envMap.size(),
        static_cast<size_t>(static_cast<int>(availH / pitch)));
    // 溢出提示行也要放得下：放不下就让一行，让到 0 为止（0 行只画提示）
    while (shownRows > 0 &&
           static_cast<float>(shownRows) * pitch + overflowH > availH) {
        --shownRows;
    }
    const bool envOverflow = envMap.size() > shownRows;
    float ey = y0;
    size_t ei2 = 0;
    for (const auto& kv : envMap) {
        if (ei2 >= shownRows) break;
        const std::string& k = kv.first;
        const std::string& val = kv.second;
        std::string shownVal = val;
        {
            std::string kl;
            kl.reserve(k.size());
            for (char c : k)
                kl += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool sensitive = kl.find("token") != std::string::npos
                || kl.find("key") != std::string::npos
                || kl.find("secret") != std::string::npos;
            if (sensitive && val.size() > 12)
                shownVal = val.substr(0, 8) + "…";
        }
        // 键名过长由省略号按像素截断
        const char* hint = envVarHint(k);
        const std::string rid = std::to_string(ei2);
        const float valX = x0 + 12.0f + keyColW + 24.0f;  // 键列↔值列间距 24
        const float valW = std::max(40.0f, w - keyColW - 48.0f);
        ui.rect(std::string("claude.env.row.bg.") + rid)
            .position(x0, ey).size(w, rowH)
            .color(t.colors.surface).radius(t.sizes.radiusLarge)
            .border(hint ? 0.0f : 1.0f, hint ? eui::Color{0.0f,0.0f,0.0f,0.0f} : t.colors.error)
            .build();
        // 行带 1：键名 | 值（同带 Center 对齐）；长文本走省略号 + clip，不换行不撑高。
        ui.text(std::string("claude.env.key.") + rid)
            .position(x0 + 12.0f, ey + line1Y).size(keyColW, line1H)
            .maxWidth(keyColW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .text(k).fontSize(keyFs).lineHeight(line1H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.primary).build();
        // 值列框高 = 行带 1（与键名同带，不再跨整卡居中压进行带 2）
        ui.text(std::string("claude.env.val.") + rid)
            .position(valX, ey + line1Y).size(valW, line1H)
            .maxWidth(valW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .text(shownVal).fontSize(keyFs).lineHeight(line1H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary).build();
        // 来源标注：托管键 / 本 Profile。
        const auto srcIt = envSources.find(k);
        const EnvSource src = srcIt != envSources.end()
            ? srcIt->second : EnvSource::Profile;
        std::string srcText = envSourceLabel(src);
        // 行带 2：来源 | 官方说明。说明列走省略号 + clip；来源列有界枚举，clip 足够。
        ui.text(std::string("claude.env.src.") + rid)
            .position(x0 + 12.0f, ey + line2Y).size(kSrcColW, line2H)
            .clip()
            .text(srcText).fontSize(smallFs).lineHeight(line2H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(src == EnvSource::Managed ? t.colors.textTertiary
                                             : t.colors.textSecondary)
            .build();
        const float hintW = std::max(40.0f, w - 24.0f - kSrcColW - kSrcGap);
        ui.text(std::string("claude.env.hint.") + rid)
            .position(x0 + 12.0f + kSrcColW + kSrcGap, ey + line2Y)
            .size(hintW, line2H)
            .maxWidth(hintW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .text(hint ? hint : "（未收录官方清单）")
            .fontSize(smallFs).lineHeight(line2H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ey += pitch;
        ++ei2;
    }
    if (envOverflow) {
        // 溢出提示：未画出的行全量保存在 Profile，编辑时可见；模型键去向一并说明
        ui.text("claude.env.overflow")
            .position(x0, ey).size(w, overflowH)
            .text("另有 " + std::to_string(envMap.size() - ei2) +
                  " 项未显示（编辑该 Profile 可查看全部；模型类键见「模型映射」"
                  "页签，extra_env 手写的除外）")
            .fontSize(smallFs).lineHeight(overflowH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ey += overflowH;
    }
    if (rows != nullptr) {
        *rows = PaneRows{static_cast<int>(shownRows),
                         static_cast<int>(envMap.size())};
    }
    return ey;
}

// 操作按钮组（测试连通/编辑/运行）+ 副提示 + 分隔线。actY 由
// drawDetailPanel 锚定给出；绘制高 = 65 被 kActionBlockH 引用，改几何须同步。
void drawDetailActions(eui::Ui& ui, float x0, float w, float actY,
                       const core::theme::Tokens& t,
                       const DetailView& v, const DetailActions& a) {
    const Profile* shown = v.shown;
    ui.rect("claude.act.divider")
        .position(x0, actY - 10.0f)
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .radius(t.sizes.radius)
        .build();
    const float actGap = 8.0f;
    // 3 键均分：EUI row spacing 只在相邻子项间生效，n 项只有 n−1 个间隙，故扣 2 个 gap。
    const float actW = (w - actGap * 2.0f) / 3.0f;
    ui.row("claude.detail.actions")
        .position(x0, actY)
        .size(w, 40.0f)
        .gap(actGap)
        .content([&] {
            const int64_t shownId = shown ? shown->id : 0;
            const Profile shownCopy = shown
                ? *shown : Profile{};

            // 测试连通（Outline 次按钮：白底绿边绿字）
            components::button(ui, "claude.act.test")
                .size(actW, 40.0f)
                .text("测试连通")
                .fontSize(core::theme::scaled(14.0f))
                .colors(t.colors.surface,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.primary)
                .radius(8.0f)
                .border(1.0f, t.colors.primary)
                .onClick([f = a.test, warn = a.warn, shownCopy] {
                    if (shownCopy.id == 0) {
                        warn("测试连通失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownCopy);
                })
                .build();
            // 编辑（Outline 次按钮：白底绿边绿字）
            components::button(ui, "claude.act.edit")
                .size(actW, 40.0f)
                .text("编辑")
                .fontSize(core::theme::scaled(14.0f))
                .colors(t.colors.surface,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.primary)
                .radius(8.0f)
                .border(1.0f, t.colors.primary)
                .onClick([f = a.edit, warn = a.warn, shownCopy] {
                    if (shownCopy.id == 0) {
                        warn("编辑失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownCopy);
                })
                .build();
            // 运行（Filled 主按钮：绿底白字）
            components::button(ui, "claude.act.run")
                .size(actW, 40.0f)
                .text("运行")
                .fontSize(core::theme::scaled(14.0f))
                .colors(t.colors.primary,
                        t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(8.0f)
                .onClick([f = a.run, warn = a.warn, shownId] {
                    if (shownId == 0) {
                        warn("运行失败", "请先选择 Profile");
                        return;
                    }
                    f(shownId);
                })
                .build();
        })
        .build();

    // S3.4 「运行」tooltip 区分语义
    components::tooltip(ui, "claude.act.run.tooltip")
        .theme(core::theme::componentTokensWeChat(t))
        .source("claude.act.run")
        .value("以该 Profile 直接打开 Claude Code 会话（不改全局设置）")
        .minWidth(280.0f)
        .maxWidth(320.0f)
        .build();

    // §5「运行」副提示
    ui.text("claude.act.run.hint")
        .position(x0, actY + 44.0f)
        .size(w, 16.0f)
        .text("以该 Profile 直接打开 Claude Code（不改当前设置）")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(core::theme::scaled(16.0f))
        .color(t.colors.textTertiary)
        .build();

    // 分隔线
    ui.rect("claude.divider")
        .position(x0, actY + 64.0f)
        .size(w, 1.0f)
        .color(t.colors.hover)
        .radius(t.sizes.radius)
        .build();
}

void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a) {
    // 右栏：详情面板（3 页签 + 操作区）
    ui.column("claude.detail")
        .size(rightW, bodyH)
        .gap(0.0f)
        .content([&] {
            ui.rect("claude.detail.bg")
                .size(rightW, bodyH)
                // §4.14 面板底浅灰（wechat.pageBg），白卡浮起；边框一级分割线
                .color(t.colors.background)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        })
        .build();

    ui.stack("claude.detail.overlay")
        .size(rightW, bodyH)
        .ignoreLayout()  // overlay 不参与 row 流式布局
        .position(xOffset, 0.0f)  // 对齐右栏 detail 列位置
        .content([&] {
            const float x0 = 28.0f;
            const float y0 = 24.0f;
            const float w0 = rightW - 56.0f;

            // 标题
            ui.text("claude.detail.title")
                .position(x0, y0)
                .size(w0, 24.0f)
                .text("当前 Profile 详情")
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(24.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();

            // S3.4 块面式 Tab（wtab 替代原下划线式 components::tabs）
            const float tabY = y0 + 40.0f;
            const float tabH = 40.0f;
            const char* tabLabels[] = {"基础信息", "模型配置", "环境变量"};
            float tabX = x0;
            for (int i = 0; i < 3; ++i) {
                float tabW = 88.0f;
                ui_kit::wtab(ui, "claude.detail.tab." + std::to_string(i),
                             tabLabels[i], st.detailTab_ == i,
                             tabX, tabY, tabW, tabH, t,
                             [sp = &st, idx = i] { sp->detailTab_ = idx; });
                tabX += tabW + 4.0f;
            }

            const float paneY = tabY + tabH + 12.0f;

            // 动作区钉面板底 + 内容框显式边界
            // 全部锚定式（相对 bodyH），不写绝对 y（Windows 与 macOS bodyH 差 15~45px）。
            const float actYFloor = bodyH - kActReserve;   // 贴底动作区的 actY
            const float contentBottom = actYFloor - kActTopGap;  // 内容框可用底

            // 内容框（显式边界）：四页签同一块固定区域，内容少时明确画出「空」而非莫名空白
            const float boxTop = paneY - 12.0f;
            ui.rect("claude.detail.contentbox")
                .position(x0 - 12.0f, boxTop)
                .size(w0 + 24.0f, std::max(0.0f, contentBottom - boxTop))
                // §4.14 白卡浮起于灰底（wechat.cardBg）；二级分割线边框
                // UAT6 玻璃：换半透面 + subtle 玻璃层
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .build();

            float paneBottom = paneY;  // 各 pane 累加真实底沿，供 max() 兜底防重叠
            PaneRows paneRows;         // 行数统计（诊断日志用）
            if (st.detailTab_ == 0) {
                paneBottom = drawDetailBasic(ui, x0, paneY, w0, t, v,
                                             contentBottom);
            } else if (st.detailTab_ == 1) {
                paneBottom = drawDetailModels(ui, x0, paneY, w0, t, v, a,
                                              contentBottom);
            } else {
                paneBottom = drawDetailEnv(ui, x0, paneY, w0, t, v,
                                           contentBottom, &paneRows);
            }
            // 锚定 + 兜底：正常档三页签都取 actYFloor（分隔线 y 全等）；更矮时基础/模型
            // 固定行数使动作区下移、三页签不再同高；再矮用 actYHi 夹住：宁可压内容也不越出容器底
            const float actYHi = std::max(bodyH - kActionBlockH, actYFloor);
            const float actY = std::min(
                std::max(actYFloor, paneBottom + kActTopGap), actYHi);
            drawDetailActions(ui, x0, w0, actY, t, v, a);
            // 诊断日志：一行可 grep——三页签 actY 必须相等。Debug 级，避免每帧刷屏
            LOG_DEBUG("[layout] pane=" + std::to_string(st.detailTab_) +
                      " paneBottom=" + std::to_string(paneBottom) +
                      " actY=" + std::to_string(actY) +
                      " contentBottom=" + std::to_string(contentBottom) +
                      " rows=" + std::to_string(paneRows.shown) + "/" +
                      std::to_string(paneRows.total));
        })
        .build();
}

} // namespace module::claude
