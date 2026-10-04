// src/module_claude_proxy/claude_ui_detail.cpp
// 右栏四 pane + 操作按钮组 + 「最近调用」页签；this 成员换成显式入参。
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
#include <core/call_rate.h>
#include <core/component_theme.h>
#include <core/logging.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>

namespace module::claude {

namespace {

// 详情面板动作区几何（单一来源）：动作块 = 61（分隔线/按钮行 36/副提示/分隔线）；
// 其上 20 间距、其下 24 呼吸 → 预留 85。由 drawDetailActions 绘制推出，改绘制须同步。
constexpr float kActionBlockH = 61.0f;
constexpr float kActTopGap = 20.0f;     // 内容框底 → actY
constexpr float kActBottomPad = 24.0f;  // actY+61 → 面板底
constexpr float kActReserve = kActionBlockH + kActBottomPad;  // 85

// 模型映射域的 env 键：键集从槽位表派生（与写入点同源），判定下沉
// isModelSlotEnvKey，此处仅语义别名。
bool isModelMappingEnvKey(const std::string& key) {
    return isModelSlotEnvKey(key);
}

}  // namespace

const char* envSourceLabel(EnvSource s) {
    // 公开单一来源（.h 声明；compose 装配高级属性键来源复用，不手抄文案）
    switch (s) {
        case EnvSource::Shared:  return "片段";
        case EnvSource::Profile: return "本 Profile";
        case EnvSource::Managed: return "托管键";
    }
    return "?";
}

float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v,
                      float contentBottom) {
    (void)contentBottom;  // 固定 6 行字段，高度与容器无关
    const Profile* shown = v.shown;
    // ---- 基础信息 pane：名称 + 未激活徽章 + Base URL + API Key + 接管 ----
    // 文本必须带 verticalAlign=Center，否则 runtime 从行框顶沿起排不居中；只加对齐，行距/行宽不变。
    ui.text("claude.field.name.label")
        .position(x0, y0).size(w * 0.5f - 8.0f, 14.0f)
        .text("名称").fontSize(11.0f).lineHeight(14.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.name.bg")
        .position(x0, y0 + 18.0f).size(w * 0.5f - 8.0f, 24.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.name.value")
        .position(x0 + 12.0f, y0 + 18.0f)
        .size(w * 0.5f - 20.0f, 24.0f)
        .text(v.nameValue).fontSize(14.0f).lineHeight(24.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    if (shown && !shown->isActive) {
        ui.rect("claude.field.name.inactive.bg")
            .position(x0 + w * 0.5f - 72.0f, y0 + 4.0f)
            .size(56.0f, 16.0f).color(t.colors.surfaceElevated)
            .radius(t.sizes.radiusPill)
            .border(1.0f, t.colors.warning).build();
        ui.text("claude.field.name.inactive")
            .position(x0 + w * 0.5f - 72.0f, y0 + 4.0f)
            .size(56.0f, 16.0f).text("未激活")
            .fontSize(10.0f).lineHeight(16.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
    }
    const float urlY = y0 + 56.0f;
    ui.text("claude.field.url.label")
        .position(x0, urlY).size(w, 14.0f)
        .text("Base URL").fontSize(11.0f).lineHeight(14.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.url.bg")
        .position(x0, urlY + 18.0f).size(w, 28.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.url.value")
        .position(x0 + 12.0f, urlY + 18.0f).size(w - 24.0f, 28.0f)
        .text(v.urlValue).fontSize(t.sizes.fontSizeSmall).lineHeight(28.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    const float keyY = urlY + 56.0f;
    ui.text("claude.field.token.label")
        .position(x0, keyY).size(w, 14.0f)
        .text("API Key").fontSize(11.0f).lineHeight(14.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.token.bg")
        .position(x0, keyY + 18.0f).size(w, 32.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.token.value")
        .position(x0 + 12.0f, keyY + 18.0f).size(w - 24.0f, 32.0f)
        .text(v.tokenValue).fontSize(t.sizes.fontSizeSmall).lineHeight(32.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary).build();
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
        .fontSize(11.0f).lineHeight(16.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(ccManaged ? t.colors.success : t.colors.textTertiary).build();
    // 共享配置绑定行：片段不携带 env 键，改标「高级属性预设」（来源标注在高级配置 pane）
    const bool bound = shown != nullptr && shown->sharedConfigId != 0 &&
                       v.sharedConfigs != nullptr;
    std::string sharedText = "共享配置：未绑定";
    if (bound) {
        const SharedConfig* sc =
            findSharedById(*v.sharedConfigs, shown->sharedConfigId);
        if (sc != nullptr) {
            sharedText = "共享配置：" + sc->name + "（高级属性预设）";
        } else {
            sharedText = "共享配置：片段已删除（悬空绑定）";
        }
    }
    const float shY = toY + 16.0f;
    ui.text("claude.field.shared.value")
        .position(x0, shY).size(w, 16.0f)
        .text(sharedText)
        .fontSize(11.0f).lineHeight(16.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(bound ? t.colors.textSecondary : t.colors.textTertiary)
        .build();
    if (!ccWarnings.empty()) {
        // 摘要 = 首条警告 + 总条数；「详见设置页」恒成立（设置页警告行只看警告）
        std::string warnText = ccWarnings.front();
        if (ccWarnings.size() > 1) {
            warnText += "（共 " +
                std::to_string(ccWarnings.size()) + " 条";
            warnText += "，详见设置页）";
        }
        ui.text("claude.field.takeover.warn.icon")
            .position(x0, shY + 16.0f).size(14.0f, 16.0f).icon(0xF071)
            .fontSize(11.0f).lineHeight(16.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
        ui.text("claude.field.takeover.warn")
            .position(x0 + 18.0f, shY + 16.0f).size(w - 18.0f, 16.0f)
            .text(warnText)
            .fontSize(11.0f).lineHeight(16.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
    }
    return shY + (!ccWarnings.empty() ? 32.0f : 16.0f);
}

float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v,
                       const DetailActions& a, float contentBottom) {
    (void)contentBottom;  // 槽位表行数固定，与容器高无关
    const Profile* shown = v.shown;
    // ---- 模型映射 pane：槽位逐行（widget id 序号 = 表序）----
    // 每行右端「1M」开关：勾态 = 槽位支持 && 串尾有标记；可用 = 支持且 base 非空，
    // 不可用行置灰但不隐藏、不挂 onChange。存储是「基础名 + 可选 [1M]」单字符串。
    ModelsConfig mc{};
    if (shown && !shown->modelsConfigJson.empty()) {
        auto mr = parseModelsConfig(shown->modelsConfigJson);
        if (mr.isOk()) mc = mr.value();
    }
    // 开关列宽 76（轨道 30 + 间隙 + 标签 + 内缩），值列相应收窄；色走 componentTokens。
    constexpr float kOneMColW = 76.0f;
    const components::SwitchStyle oneMStyle(core::theme::componentTokens(t));
    components::SwitchStyle oneMDisabledStyle = oneMStyle;
    // 禁用态灰：轨道/标签走 textDisabled，滑块走 surfaceElevated，与「关」态可辨
    oneMDisabledStyle.off = t.colors.textDisabled;
    oneMDisabledStyle.on = t.colors.textDisabled;
    oneMDisabledStyle.knob = t.colors.surfaceElevated;
    oneMDisabledStyle.text = t.colors.textDisabled;
    float sy = y0;
    int slotIdx = 0;
    for (const auto& meta : modelSlotTable()) {
        const ModelSlot& slot = mc.*meta.slot;
        const bool filled = !slot.model.empty();
        const std::string idx = std::to_string(slotIdx);
        ui.rect("claude.slot.bg." + idx)
            .position(x0, sy).size(w, 32.0f)
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderMuted).build();
        ui.text("claude.slot.name." + idx)
            .position(x0 + 12.0f, sy).size(80.0f, 32.0f)
            .text(meta.label).fontSize(12.0f).lineHeight(32.0f)
            // 必须 verticalAlign=Center 才垂直居中；行框几何（32/38 已标定）不动。
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ui.text("claude.slot.val." + idx)
            .position(x0 + 100.0f, sy).size(w - 112.0f - kOneMColW, 32.0f)
            // 显示层剥 [1M]（开关已表达 1M，值再带即重复）；只动显示，存储与发往上游仍带标记。
            .text(filled ? deriveDisplayName(slot.model)
                         : std::string("未配置"))
            .fontSize(13.0f).lineHeight(32.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            // 长模型名单行省略号（显式 maxWidth 是前提），clip 保留作绘制范围兜底。
            .maxWidth(w - 112.0f - kOneMColW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(filled ? t.colors.textPrimary : t.colors.textTertiary).build();
        // 「1M」开关（不支持/空 base → 不传 onChange）；组件无 .position，包 stack 定位
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
        ui.stack("claude.slot.onem." + idx + ".wrap")
            .position(x0 + w - kOneMColW - 4.0f, sy)
            .size(kOneMColW, 32.0f)
            .content([&] {
                components::toggleSwitch(ui, "claude.slot.onem." + idx)
                    .size(kOneMColW, 32.0f)
                    .checked(oneMChecked)
                    .text("1M")
                    .fontSize(11.0f)
                    .trackSize(30.0f, 16.0f)
                    .style(oneMEnabled ? oneMStyle : oneMDisabledStyle)
                    .onChange(onOneM)
                    .build();
            })
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
    // ---- 环境变量 pane：生效 env（模型映射键除外）+ 每键来源 ----
    // 这是**将写入的托管 env**，不是 settings.json 的 ground truth；来源由
    // buildProxyEnv 出参给出（合并规则只能一处，复刻必漂移）
    std::map<std::string, std::string> envMap;
    std::map<std::string, EnvSource> envSources;
    if (shown != nullptr) {
        // 片段不再携带 env 键：本 pane 只列 profile 自身 env ∪ 托管键
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
    // ---- 行几何：全部从 fontScale 派生，禁写死 100% 像素（旧写死致缩放后墨迹叠印）----
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
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, hint ? t.colors.borderMuted : t.colors.error)
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
        // 来源标注：托管键 / 本 Profile（Shared 分支随片段 env 消亡移除；枚举保留复用）
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

namespace {

// 空调用记录表（DetailView.recent 为 null 时只读兜底）
const std::vector<ClaudeService::CallLogEntry>& emptyCalls() {
    static const std::vector<ClaudeService::CallLogEntry> kEmpty;
    return kEmpty;
}

// 最近调用 pane：表头 + 行（行数纯按可用高推）+ 错误/空态 + 「查看全部 →」。行/表头
// id 沿用旧底部预览（claude.calls.*，无头用例按此断言）；返回本 pane 内容底沿。
float drawDetailCallsPane(eui::Ui& ui, float x0, float y0, float w,
                          const core::theme::Tokens& t,
                          const DetailView& v,
                          const std::function<void()>& openCalls,
                          float contentBottom, PaneRows* rows) {
    // 调用表头列布局同旧底部预览；「速率」列从「方法/路径」让出 110px
    const float colTimeX = x0;
    const float colPathX = x0 + 80.0f;
    const float colStatusX = x0 + w - 310.0f;
    const float colDurX = x0 + w - 220.0f;
    const float colRateX = x0 + w - 130.0f;
    ui.text("claude.calls.h.time")
        .position(colTimeX, y0)
        .size(72.0f, 14.0f)
        .text("时间")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.path")
        .position(colPathX, y0)
        .size(w - 390.0f, 14.0f)
        .text("方法 / 路径")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.status")
        .position(colStatusX, y0)
        .size(60.0f, 14.0f)
        .text("状态")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.dur")
        .position(colDurX, y0)
        .size(60.0f, 14.0f)
        .text("耗时")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.rate")
        .position(colRateX, y0)
        .size(110.0f, 14.0f)
        .text("速率 (tok/s)")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // 行数预算纯按可用高推；画不满 1 行只画空态，超出的行用「另有 N 条」表达。
    constexpr float kRowPitch = 24.0f;
    constexpr float kOverflowH = 18.0f;   // 「另有 N 条」提示行
    constexpr float kEmptyH = 16.0f;      // 空态 / 错误行（与行高同源，单行文本）
    constexpr float kViewallGap = 8.0f;
    constexpr float kViewallH = 24.0f;
    constexpr float kViewallBand = kViewallGap + kViewallH;  // 「查看全部 →」带
    const float cellsY = y0 + 24.0f;
    const float rowsAvail =
        std::max(0.0f, contentBottom - cellsY - kViewallBand);
    size_t rowsCap = static_cast<size_t>(
        static_cast<int>(rowsAvail / kRowPitch));

    // 最近调用（真实数据，从 claude_call_logs 读）
    const std::vector<ClaudeService::CallLogEntry>& recent =
        v.recent ? *v.recent : emptyCalls();
    const std::string& callsError = v.callsError;
    const std::time_t now = v.now;
    size_t shownRows = 0;
    bool callsOverflow = false;   // 「另有 N 条」是否在场（「查看全部」要让位）
    bool singleLineDrawn = false;  // 空态 / 错误行在场（它占一行）
    if (!callsError.empty()) {
        ui.text("claude.calls.error")
            .position(colTimeX, cellsY)
            .size(w - 80.0f, kEmptyH)
            .text("调用记录加载失败：" + callsError)
            .fontSize(11.0f)
            .lineHeight(kEmptyH)
            .color(t.colors.error)
            .build();
        singleLineDrawn = true;
    } else if (recent.empty()) {
        ui.text("claude.calls.empty")
            .position(colTimeX, cellsY)
            .size(w - 80.0f, kEmptyH)
            .text("(暂无调用记录 —— 经本机代理转发过请求后这里会出现)")
            .fontSize(11.0f)
            .lineHeight(kEmptyH)
            .color(t.colors.textTertiary)
            .build();
        singleLineDrawn = true;
    } else {
        // 溢出提示行也要放得下：放不下就让一行，让到 0 为止
        size_t shown = std::min(recent.size(), rowsCap);
        while (shown > 0 &&
               static_cast<float>(shown) * kRowPitch + kOverflowH > rowsAvail) {
            --shown;
        }
        if (shown == 0) {
            // 内容框连一行都放不下：如实说「空间不足」，不画「另有 N 条」（同坐标叠印）
            ui.text("claude.calls.empty")
                .position(colTimeX, cellsY)
                .size(w - 80.0f, kEmptyH)
                .text("(内容区高度不足，未显示调用记录)")
                .fontSize(11.0f)
                .lineHeight(kEmptyH)
                .color(t.colors.textTertiary)
                .build();
            singleLineDrawn = true;
        }
        for (size_t i = 0; i < shown; ++i) {
            const ClaudeService::CallLogEntry& c = recent[i];
            const float rowY = cellsY + static_cast<float>(i) * kRowPitch;

            ui.text("claude.calls.t." + std::to_string(i))
                .position(colTimeX, rowY)
                .size(72.0f, 16.0f)
                .text(formatCallTime(c.startedAt, now))
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(t.colors.textPrimary)
                .build();

            const std::string mp = c.method + " " + c.path;
            ui.text("claude.calls.p." + std::to_string(i))
                .position(colPathX, rowY)
                .size(w - 390.0f, 16.0f)
                .text(mp)
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(t.colors.textPrimary)
                .build();

            const eui::Color statusColor =
                (c.statusCode >= 200 && c.statusCode < 300)
                    ? t.colors.success :
                c.statusCode == 429
                    ? t.colors.warning
                    : t.colors.error;
            const std::string statusStr = c.statusCode > 0
                ? std::to_string(c.statusCode)
                : std::string("-");
            ui.text("claude.calls.s." + std::to_string(i))
                .position(colStatusX, rowY)
                .size(60.0f, 16.0f)
                .text(statusStr)
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(statusColor)
                .build();

            ui.text("claude.calls.d." + std::to_string(i))
                .position(colDurX, rowY)
                .size(60.0f, 16.0f)
                .text(formatLatency(c.latencyMs))
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(t.colors.textSecondary)
                .build();

            // 本地速率口径与首页「Token 速率」卡同源（core/call_rate.h）：仅 2xx 且
            // output_tokens > 0 有速率，其余显示「—」（0 是合法值，不能用 0 冒充「没有」）
            double rateTps = 0.0;
            const bool hasRate = core::call_rate::tps(
                c.statusCode, c.outputTokens, c.latencyMs, rateTps);
            ui.text("claude.calls.rate." + std::to_string(i))
                .position(colRateX, rowY)
                .size(110.0f, 16.0f)
                .text(hasRate ? core::call_rate::formatTps(rateTps)
                              : std::string(core::call_rate::kNoRate))
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(hasRate ? t.colors.textSecondary
                               : t.colors.textTertiary)
                .build();
        }
        shownRows = shown;
        // 截断必须有可见表达；0 行档不画「另有 N 条」（与其上同坐标叠印）
        if (shown > 0 && recent.size() > shown) {
            callsOverflow = true;
            ui.text("claude.calls.overflow")
                .position(colTimeX, cellsY + static_cast<float>(shown) * kRowPitch)
                .size(w - 80.0f, kOverflowH)
                .text("另有 " + std::to_string(recent.size() - shown) +
                      " 条（点「查看全部」）")
                .fontSize(11.0f)
                .lineHeight(kOverflowH)
                .color(t.colors.textTertiary)
                .build();
        }
    }
    if (rows != nullptr) {
        *rows = PaneRows{static_cast<int>(shownRows),
                         static_cast<int>(recent.size())};
    }

    // 查看全部 →：位置在所有行之后；内容框放不下时不画（宁缺入口，不推 contentBottom）
    const float viewY =
        cellsY + static_cast<float>(shownRows) * kRowPitch +
        (callsOverflow ? kOverflowH : 0.0f) +
        (singleLineDrawn ? kEmptyH + kViewallGap : kViewallGap);
    if (viewY + kViewallH <= contentBottom + 0.5f) {
        // 「查看全部」是自绘按钮（非 components::button）：换组件要改既有单测定位的
        // claude.calls.viewall.hit 并重算行数预算；自绘满足同样交互，破坏面小。
        ui.rect("claude.calls.viewall.hit")
            .position(x0, viewY)
            .size(w, kViewallH)
            .states(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
            .onClick([oc = openCalls] { oc(); })
            .build();
        ui.text("claude.calls.viewall")
            .position(x0, viewY)
            .size(w, kViewallH)
            .text("查看全部 →")
            .fontSize(12.0f)
            .lineHeight(kViewallH)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.primary)
            .build();
        return viewY + kViewallH;
    }
    return viewY;
}

// 高级配置 pane：方言 + 备用 Profile 两行只读 + 生效 JSON 逐顶层键来源行。数据由
// compose 装配进 DetailView，本函数零服务调用；行数纯按可用高推（暂定上限 8），
// 内容底沿恒 ≤ contentBottom，五页签同高不变式保持。
float drawDetailAdvanced(eui::Ui& ui, float x0, float y0, float w,
                         const core::theme::Tokens& t, const DetailView& v,
                         float contentBottom) {
    // ---- 行几何：全部从 fontScale 派生（旧写死致缩放墨迹叠印）；横向列宽不缩放 ----
    const float fs = core::theme::fontScale();
    const auto rfs = [fs](float base) { return std::round(base * fs); };
    const float labelFs = 11.0f * fs;                // 标签 / 来源 / 说明小字
    const float keyFs = 12.0f * fs;                  // 键名
    const float labelH = rfs(14.0f);                 // 标签行盒（100% = 14）
    const float labelGap = rfs(18.0f);               // 标签底 → 盒顶（100% = 18）
    const float fieldH = rfs(28.0f);                 // 方言 / 备用值盒高（100% = 28）
    const float fieldGap = rfs(10.0f);               // 值盒底 → 下一标签顶（100% = 10）
    // 键来源清单行高/行距与行数预算
    const float kKeyRowH = rfs(28.0f);               // 键行卡高（100% = 28）
    const float kKeyPitch = kKeyRowH + rfs(6.0f);    // 行卡 + 6 间距（100% = 34）
    const float kNoteH = rfs(18.0f);                 // 来源说明行（100% = 18）
    const float kOverflowH = rfs(18.0f);             // 「另有 N 项」提示行（100% = 18）
    constexpr int kMaxKeyRows = 8;        // 暂定上限（contentBottom 消费才是护栏）
    // 方言行（固定）
    ui.text("claude.field.dialect.label")
        .position(x0, y0).size(w, labelH)
        .text("生效方言").fontSize(labelFs).lineHeight(labelH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.dialect.bg")
        .position(x0, y0 + labelGap).size(w, fieldH)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.dialect.value")
        .position(x0 + 12.0f, y0 + labelGap).size(w - 24.0f, fieldH)
        .text(v.dialectText).fontSize(t.sizes.fontSizeSmall).lineHeight(fieldH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    const float fbY = y0 + labelGap + fieldH + fieldGap;
    // 备用行（固定）
    ui.text("claude.field.fallback.label")
        .position(x0, fbY).size(w, labelH)
        .text("备用 Profile").fontSize(labelFs).lineHeight(labelH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.fallback.bg")
        .position(x0, fbY + labelGap).size(w, fieldH)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.fallback.value")
        .position(x0 + 12.0f, fbY + labelGap).size(w - 24.0f, fieldH)
        .text(v.fallbackName).fontSize(t.sizes.fontSizeSmall).lineHeight(fieldH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary).build();
    float ey = fbY + labelGap + fieldH;    // = y0 + 102（100% 档既有固定底沿）
    if (v.extraSettingsKeys.empty()) {
        // 空态两种成因：note 空 = 未配置；note 非空 = 解析失败（原样展示）
        const std::string statusText = v.extraSettingsNote.empty()
            ? std::string("未配置高级属性 JSON")
            : v.extraSettingsNote;
        ui.text("claude.advjson.status")
            .position(x0, ey).size(w, kNoteH)
            .text(statusText).fontSize(labelFs).lineHeight(kNoteH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        return std::min(ey + kNoteH, contentBottom);
    }
    ui.text("claude.advjson.label")
        .position(x0, ey).size(w, labelH)
        .text("高级属性（生效 JSON 逐键来源）").fontSize(labelFs).lineHeight(labelH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary).build();
    ey += labelGap;
    // 行数预算：说明行/溢出行要放得下，放不下就让行（让到 0 为止）
    const float availH = std::max(0.0f, contentBottom - ey);
    const bool hasNote = !v.extraSettingsNote.empty();
    const float tailH = (hasNote ? kNoteH : 0.0f) + kOverflowH;
    size_t shownRows = std::min(
        v.extraSettingsKeys.size(),
        static_cast<size_t>(std::max(0, static_cast<int>(availH / kKeyPitch))));
    shownRows = std::min(shownRows, static_cast<size_t>(kMaxKeyRows));
    while (shownRows > 0 &&
           static_cast<float>(shownRows) * kKeyPitch + tailH > availH) {
        --shownRows;
    }
    const bool overflow = v.extraSettingsKeys.size() > shownRows;
    const size_t keyCount = v.extraSettingsKeys.size();
    for (size_t i = 0; i < shownRows; ++i) {
        const auto& [k, src] = v.extraSettingsKeys[i];
        const std::string idx = std::to_string(i);
        ui.rect("claude.advjson.row.bg." + idx)
            .position(x0, ey).size(w, kKeyRowH)
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderMuted).build();
        // 键列单行省略号（显式 maxWidth 是前提）+ clip 兜底；来源列有界枚举，保留 clip。
        ui.text("claude.advjson.row.key." + idx)
            .position(x0 + 12.0f, ey).size(w * 0.4f, kKeyRowH)
            .maxWidth(w * 0.4f)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .text(k).fontSize(keyFs).lineHeight(kKeyRowH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.primary).build();
        ui.text("claude.advjson.row.src." + idx)
            .position(x0 + w * 0.4f + 20.0f, ey).size(w * 0.6f - 32.0f, kKeyRowH)
            .clip()
            .text(src).fontSize(labelFs).lineHeight(kKeyRowH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(src == std::string(envSourceLabel(EnvSource::Shared))
                       ? t.colors.textSecondary
                       : t.colors.textTertiary).build();
        ey += kKeyPitch;
    }
    if (overflow) {
        ui.text("claude.advjson.overflow")
            .position(x0, ey).size(w, kOverflowH)
            .text("另有 " + std::to_string(keyCount - shownRows) +
                  " 项（编辑该 Profile 可查看全部）")
            .fontSize(labelFs).lineHeight(kOverflowH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ey += kOverflowH;
    }
    if (hasNote) {
        ui.text("claude.advjson.note")
            .position(x0, ey).size(w, kNoteH)
            .text(v.extraSettingsNote).fontSize(labelFs).lineHeight(kNoteH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning).build();
        ey += kNoteH;
    }
    // 兜底：内容底沿不越过内容框（min() 是护栏，布局来源是上面的行数预算）
    return std::min(ey, contentBottom);
}

// 操作按钮组（启用/测试连通/编辑/运行/余量查询）+ 副提示 + 分隔线。actY 由
// drawDetailPanel 锚定给出；绘制高 = 61 被 kActionBlockH 引用，改几何须同步。
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
    const float actGap = 12.0f;
    // 5 键均分：EUI row spacing 只在相邻子项间生效，n 项只有 n−1 个间隙，故扣 4 个 gap。
    const float actW = (w - actGap * 4.0f) / 5.0f;
    ui.row("claude.detail.actions")
        .position(x0, actY)
        .size(w, 36.0f)
        .gap(actGap)
        .content([&] {
            const int64_t shownId = shown ? shown->id : 0;
            const Profile shownCopy = shown
                ? *shown : Profile{};

            // 启用（设为激活）
            components::button(ui, "claude.act.enable")
                .size(actW, 36.0f)
                .text("启用")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.primary,
                        t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius)
                .onClick([f = a.enable, warn = a.warn, shownId] {
                    if (shownId == 0) {
                        warn("启用失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownId);
                })
                .build();
            // 测试连通
            components::button(ui, "claude.act.test")
                .size(actW, 36.0f)
                .text("测试连通")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.test, warn = a.warn, shownCopy] {
                    if (shownCopy.id == 0) {
                        warn("测试连通失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownCopy);
                })
                .build();
            // 编辑（预填现有值）
            components::button(ui, "claude.act.edit")
                .size(actW, 36.0f)
                .text("编辑")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.edit, warn = a.warn, shownCopy] {
                    if (shownCopy.id == 0) {
                        warn("编辑失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownCopy);
                })
                .build();
            // 删除/建副本/复制配置已从本行移除（同源逻辑保留在列表右键菜单）；运行 = 新终端打开 claude 会话
            components::button(ui, "claude.act.run")
                .size(actW, 36.0f)
                .text("运行")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.run, warn = a.warn, shownId] {
                    if (shownId == 0) {
                        warn("运行失败", "请先选择 Profile");
                        return;
                    }
                    f(shownId);
                })
                .build();
            // 余量查询常驻：没配时也在（配置入口不能因「还没配」消失）；未选中点了给提示
            components::button(ui, "claude.act.quota")
                .size(actW, 36.0f)
                .text("余量查询")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.quota, warn = a.warn, shownId] {
                    if (shownId == 0) {
                        warn("余量查询", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownId);
                })
                .build();
        })
        .build();

    // 「运行」副提示：副作用必须明示（直连，不启代理、不切激活、零全局改动）
    ui.text("claude.act.run.hint")
        .position(x0, actY + 40.0f)
        .size(w, 16.0f)
        .text("以该 Profile 生成临时配置直接打开 Claude Code 会话"
              "（起始目录 = 用户主目录），不修改当前设置")
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .color(t.colors.textTertiary)
        .build();

    // 分隔线
    ui.rect("claude.divider")
        .position(x0, actY + 60.0f)
        .size(w, 1.0f)
        .color(t.colors.borderMuted)
        .radius(t.sizes.radius)
        .build();
}

} // namespace

void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a) {
    // ---- 右栏：详情面板（5 页签 + 操作区）----
    ui.column("claude.detail")
        .size(rightW, bodyH)
        .gap(0.0f)
        .content([&] {
            ui.rect("claude.detail.bg")
                .size(rightW, bodyH)
                // 右栏详情面板 = 结构性面板 → background
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

            // 页签栏用 EUI-NEO tabs（无 .position 故包 stack 对齐内边距）；高级配置 = idx 3
            const float tabY = y0 + 40.0f;
            const float tabH = 36.0f;
            ui.stack("claude.detail.tabs.wrap")
                .position(x0, tabY)
                .size(w0, tabH)
                .content([&] {
                    components::tabs(ui, "claude.detail.tabs")
                        .size(w0, tabH)
                        .items(std::vector<std::string>{
                            "基础信息", "模型映射", "环境变量", "高级配置",
                            "最近调用"})
                        .selected(st.detailTab_)
                        .fontSize(t.sizes.fontSizeSmall)
                        .theme(themeTokens)
                        .onChange([sp = &st](int idx) { sp->detailTab_ = idx; })
                        .build();
                });

            const float paneY = tabY + tabH + 12.0f;

            // ---- 动作区钉面板底 + 内容框显式边界 ----
            // 全部锚定式（相对 bodyH），不写绝对 y（Windows 与 macOS bodyH 差 15~45px）。
            const float actYFloor = bodyH - kActReserve;   // 贴底动作区的 actY
            const float contentBottom = actYFloor - kActTopGap;  // 内容框可用底

            // 内容框（显式边界）：四页签同一块固定区域，内容少时明确画出「空」而非莫名空白
            const float boxTop = paneY - 12.0f;
            ui.rect("claude.detail.contentbox")
                .position(x0 - 12.0f, boxTop)
                .size(w0 + 24.0f, std::max(0.0f, contentBottom - boxTop))
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderMuted)
                .build();

            float paneBottom = paneY;  // 各 pane 累加真实底沿，供 max() 兜底防重叠
            PaneRows paneRows;         // 行数统计（诊断日志用）
            if (st.detailTab_ == 0) {
                paneBottom = drawDetailBasic(ui, x0, paneY, w0, t, v,
                                             contentBottom);
            } else if (st.detailTab_ == 1) {
                paneBottom = drawDetailModels(ui, x0, paneY, w0, t, v, a,
                                              contentBottom);
            } else if (st.detailTab_ == 2) {
                paneBottom = drawDetailEnv(ui, x0, paneY, w0, t, v,
                                           contentBottom, &paneRows);
            } else if (st.detailTab_ == 3) {
                // 高级配置：固定两行只读，走基础 pane 同款锚定/兜底链
                paneBottom = drawDetailAdvanced(ui, x0, paneY, w0, t, v,
                                                contentBottom);
            } else {
                paneBottom = drawDetailCallsPane(
                    ui, x0, paneY, w0, t, v, a.openCalls,
                    contentBottom, &paneRows);
            }
            // 锚定 + 兜底：正常档五页签都取 actYFloor（分隔线 y 全等）；更矮时基础/模型
            // 固定行数使动作区下移、五页签不再同高；再矮用 actYHi 夹住：宁可压内容也不越出容器底
            const float actYHi = std::max(bodyH - kActionBlockH, actYFloor);
            const float actY = std::min(
                std::max(actYFloor, paneBottom + kActTopGap), actYHi);
            drawDetailActions(ui, x0, w0, actY, t, v, a);
            // 诊断日志：一行可 grep——五页签 actY 必须相等。Debug 级，避免每帧刷屏
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
