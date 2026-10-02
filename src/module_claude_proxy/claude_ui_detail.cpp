// src/module_claude_proxy/claude_ui_detail.cpp
// R4（P-B3）：右栏四 pane + 操作按钮组 + 「最近调用」页签（2026-09-28 由
// 底部固定预览块抽成独立 tab）—— 逐字自 claude_module.cpp 的 compose
// 搬出（含注释），只把 this 成员换成显式入参（DetailView 只读输入 /
// DetailActions 回调）。
//
// 2026-09-28（macOS 布局统一方案 §1 组 B）——本文件是本组的主战场，三条改动：
//   P1 动作区钉面板底 + 内容框显式边界（drawDetailPanel / drawDetailActions）
//   P2 「最近调用」去掉 10 行硬上限与 3 行硬下限，行数纯按可用高推
//   P3 「环境变量」改列全部生效 env + 每键来源（来源由 buildProxyEnv 透出；
//      E1 起模型映射键收窄至「模型映射」页签，见下方 2026-09-30 块）
// 2026-09-30（详情页「环境变量」pane 两缺陷修复）：
//   E1 模型映射键不在本 pane 重复展示 —— 键集自槽位表派生（isModelMappingEnvKey），
//      独立「模型映射」页签已列同一批键；空态区分「未选 Profile」与「全被滤掉」
//   E2 行几何全部从生效字号缩放（core::theme::fontScale）派生 + 行内两带网格，
//      修真机「缩放 130 文本重叠 / item 布局不合理」——旧代码字号与行盒全按
//      100% 缩放写死，130% 档下墨迹越出行盒互相叠印
// 统一原则（后续维护照此判，方案 §1 组 B 的「一条原则」）：
//   **容器可用高只有一个来源**；贴底元素锚容器底；内容视口 = 容器底 − 已知
//   chrome；所有「向下展开 / 是否越界」的判定用**真实裁剪下沿**，不用推导值。
// 本文件的容器底 = bodyH（claude_module 由 screen.height 减已知 chrome 得到），
// 真实内容裁剪下沿 = drawDetailPanel 算出的 contentBottom。
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

// ---- 详情面板的内容框 / 动作区几何（P1，单一来源）----
// 动作块自身 = 61（量取自 drawDetailActions：actY−10 分隔线 / actY..actY+36
// 按钮行 / actY+40..+56 副提示 / actY+60..+61 分隔线），其上加 20 间距、
// 其下留 24 呼吸（与面板顶部内边距 y0=24 对称）→ 从 bodyH 量 = 85。
// 三个数都由 drawDetailActions 的实际绘制推出，改绘制就要同步改这里
//（不是拍脑袋的「145」——方案初稿给的 145 与源码不符，已按实测量取）。
constexpr float kActionBlockH = 61.0f;
constexpr float kActTopGap = 20.0f;     // 内容框底 → actY
constexpr float kActBottomPad = 24.0f;  // actY+61 → 面板底
constexpr float kActReserve = kActionBlockH + kActBottomPad;  // 85

// 模型映射域的 env 键（E1，2026-09-30）：详情页已有独立的「模型映射」页签，
// 槽位派生的键（各槽位 envKey + 官方 *_NAME 变体）在那里逐行展示，本 pane
// 再列一遍纯属重复。键集**从槽位表派生**（provider_config 的
// DEVDESK_MODEL_SLOT_LIST 单一声明处），不手写键名清单：加槽位自动跟随，
// 与写入点同源 —— buildProxyEnv 的槽位循环写的就是 m.envKey / m.nameKey
//（provider_config.cpp），过滤集合与实际写入的键不可能漂移。
// 按键名过滤而非按来源（Managed）过滤：用户手写在 extra_env / 共享片段里的
// 模型键（如 ANTHROPIC_MODEL）同属模型域，一样不在本页重复展示。
// F-10（2026-09-30 advanced-json-editor）：本函数自 provider_config 的
// isModelSlotEnvKey 转发（判定下沉纯函数层——applyExtraSettingsToSettingsDoc
// 的模型抑制判定同源），此处仅保留语义别名，不手抄键名。
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
    (void)contentBottom;  // 固定 6 行字段，高度与容器无关（不越 contentBottom，
                         // 见 drawDetailPanel 的 max() 兜底）
    const Profile* shown = v.shown;
    // ---- 基础信息 pane：名称 + 未激活徽章 + Base URL + API Key + 接管 ----
    // 真机反馈 ⑤（2026-09-29）：本页文本没垂直居中 —— 文本默认从行框顶沿起排，
    // 只有 verticalAlign=Center 时 runtime 才把 ink 中心对齐到 frame 中心
    //（EUI-NEO runtime_render.h renderText）。故本 pane 每个文本都带 Center；
    // **只加对齐，行高/行框几何与水平对齐（标签在左、值在右）一律不变**。
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
    // 审计 CPX-02：接管失败（无 sidecar → 未接管）与上层覆盖
    // 警告都要让用户看见，故不再以 ccManaged 为展示门。读的是
    // 内存 getter（F2 起按值返回快照——写者可能是 agent 线程，见
    // ClaudeService::lastSyncWarnings 头注；无 IO/DB，符合 H2），
    // 启停代理 / 切 profile 后下一帧即刷新
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
    // 批 2（设计 §2.10「详情/列表」）建立、advanced-json-editor §8.2 改造：
    // 共享配置绑定行——片段不再携带 env 键，「N 个键」计数语义失效，改标
    // 「高级属性预设」（来源标注由高级配置 pane 的键清单承接，§8.5）
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
        // 摘要 = 首条警告（失败路径只有一条 = 失败原因）
        // + 总条数；「详见设置页」恒成立——设置页的警告行
        // 已改为只看警告（不再按 managed 门控，见
        // settings_module.cpp 同审计项），未接管态下
        // 接管失败原因同样列在那里
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
    (void)contentBottom;  // 槽位表行数固定（= 表行数），与容器高无关
    const Profile* shown = v.shown;
    // ---- 模型映射 pane：槽位逐行（遍历槽位表派生；widget id 序号 = 表序）----
    // 批次 F（2026-10-02）：每行右端加「1M」开关 —— 把模型名尾部的 [1M] 后缀
    // 变成可点的开关（语义照 cc-switch，判定/剥离/设置三函数在 provider_config，
    // 存储仍是「基础名 + 可选 [1M]」一个字符串，零 schema 迁移）。
    //   勾态 = **槽位支持位 && 串尾（大小写不敏感）有标记**（双重条件）
    //   可用 = 槽位支持位 && base 非空（空 base 不许勾）
    // 不可用的行**置灰但不隐藏**（用户能看到「有这个能力、这个槽位不支持」），
    // 且不挂 onChange —— 点它不落库（手输的标记由保存前归一闪除）。
    ModelsConfig mc{};
    if (shown && !shown->modelsConfigJson.empty()) {
        auto mr = parseModelsConfig(shown->modelsConfigJson);
        if (mr.isOk()) mc = mr.value();
    }
    // 开关列宽（右端固定列）：轨道 30 + 间隙 + 「1M」标签 + 两侧内缩 = 76；
    // 值文本列相应收窄同宽（两列互不重叠，探针 ⑦ 有几何断言）。
    // 色源走 core::theme::componentTokens（core → 组件主题的唯一映射，不手拼色）
    constexpr float kOneMColW = 76.0f;
    const components::SwitchStyle oneMStyle(core::theme::componentTokens(t));
    components::SwitchStyle oneMDisabledStyle = oneMStyle;
    // 禁用态（灰）：轨道/标签都走 textDisabled，滑块走 surfaceElevated ——
    // 与可用行的「关」态（surfaceHover/surfaceActive 混色）色值不同，肉眼可辨
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
            // 真机反馈 ④（2026-09-29）：槽位行文本没垂直居中 —— 文本从 32 高
            // 行框的顶沿起排（runtime 只在 verticalAlign=Center 时把 ink 中心
            // 对齐到 frame 中心，见 EUI-NEO runtime_render.h renderText）。
            // 只加对齐，**行高/行框几何一律不动**（32/38 是别的修复标定过的）
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ui.text("claude.slot.val." + idx)
            .position(x0 + 100.0f, sy).size(w - 112.0f - kOneMColW, 32.0f)
            .text(filled ? slot.model : std::string("未配置"))
            .fontSize(13.0f).lineHeight(32.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .clip()   // 值文本列变窄后长出列宽的长模型名硬裁（不压开关列）
            .color(filled ? t.colors.textPrimary : t.colors.textTertiary).build();
        // 「1M」开关（不支持 / 空 base → 不传 onChange = 点了没反应）。
        // 组件 builder 无 .position()（与 tabs 同款）→ 包一层定位 stack 对齐行右端
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
    // ---- 环境变量 pane：生效 env（**模型映射键除外**，E1）+ 每键来源 ----
    // P3（2026-09-28）改为列 buildProxyEnv 的合并结果 + 每键来源；2026-09-30
    // （E1）起把模型映射域的键从本页滤掉（isModelMappingEnvKey，集合自槽位表
    // 派生）—— 独立「模型映射」页签已逐行展示同一批键，两处重复。
    // 其余口径不变（审计 F-05 收敛，别夸大）：这是**将写入的托管 env**，不是
    // settings.json 的 ground truth —— ① 接管写入的是「既有 env ∪ 托管键」，
    // settings.json 里的非托管键在这里看不到；② 被更高优先级文件压过的托管键
    // 在接管时会被跳过；③ 代理未运行时 settings.json 里根本没有这份表。故它
    // 只回答「这个 Profile 会往上游发什么（模型键除外）」，排查 live 文件请
    // 直接看文件本身。每行标出来源（托管键 / 本 Profile / 片段），来源由
    // buildProxyEnv 的可选出参给出。
    // 为什么用可选出参而不是在 UI 侧重算：合并规则（profile 自身 → 片段 →
    // 托管键，托管键最高）只能有一处，复刻一遍必然漂移。
    std::map<std::string, std::string> envMap;
    std::map<std::string, EnvSource> envSources;
    if (shown != nullptr) {
        //（原「所绑片段」取数随 advanced-json-editor §8.4 拆除：片段不再携带
        // env 键，env pane 只列 profile 自身 env ∪ 托管键；片段高级属性的
        // 来源标注在高级配置 pane，§8.5）
        const int port = v.svc ? v.svc->configuredProxyPort() : 0;
        envMap = buildProxyEnv(shown->modelsConfigJson, shown->extraEnvJson,
                               port, &envSources);
        // 模型映射键出列（E1，按键名不按来源）。必须在行数预算之前滤：
        // 行数、「另有 N 项」、PaneRows 的口径都按过滤后的可见键计。
        for (auto it = envMap.begin(); it != envMap.end();) {
            if (isModelMappingEnvKey(it->first)) {
                envSources.erase(it->first);
                it = envMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    // ---- 行几何（E2，2026-09-30）：全部从生效字号缩放派生，禁写死 100% 像素 ----
    // 根因（真机反馈「缩放 130 文本重叠」）：旧代码字号 12/11 与行盒 20/18/48
    // 全按 100% 缩放写死；全局字号缩放（core::theme::fontScale，设置页档位，
    // 用户机 130%）放大字号但不动布局 → 墨迹越出行盒互相叠印。现在字号随缩放、
    // 行盒从字号推导（行盒系数 > 1.25×字号 = 最坏 CJK 墨迹 ascent+descent，
    // 恒包住墨迹；100% 档下各值与旧常量逐一相等，不是重排）：
    //   行带 1（键名 | 值）高 = round(键字号×1.7)   100% = round(20.4) = 20（旧值）
    //   行带 2（来源 | 官方说明）高 = round(说明字号×1.6)
    //                                     100% = round(17.6) = 18（旧值）
    //   行卡高 = 上留白 4 + 行带 1 + 带间 2 + 行带 2 + 下留白 4
    //                                     100% = 4+20+2+18+4 = 48（旧值）
    //   行距 = 行卡高 + 6；溢出提示行高 = 行带 2 + 2
    //                                     100% = 54 / 20（旧值）
    // 旧布局叠印的第二个根因也在此修：旧值文本框高 48（跨整卡垂直居中），墨迹
    // 带（约 ey+18..30）压进行带 2（ey+24 起），来源/说明文本又无 clip，凡超过
    // ~112px 的官方说明横向伸进值列 → 值的下半与说明的上半叠印。现改**行内两带
    // 网格**：键/值同行带（「键 值」成一行，且与基础信息/模型映射页签的 Center
    // 对齐口径一致），来源/说明同行带，两带 y 区间结构不相交。
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
        // 空态两种成因分开说（P1 要求「空态画明确文案」）：未选 Profile vs
        // 键全被模型过滤拿走。后者当前不可达 —— 有 Profile 必有 BASE_URL /
        // AUTH_TOKEN 两个非模型托管键在，仍保留分支：后续口径若变（如托管键
        // 也收归各页签），本页画说明文字而不是一片空白
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
    // 键名截断估宽（长文本策略 ①）：大写 Latin@12px 每字符实宽 ~7.5-8.5px，
    // 取 8（旧 7 偏小 —— 35 字符键截到 31 字符后仍漏出键列 ~12px）；随缩放
    // 同步放大，再加 .clip() 硬界兜底（估宽是估的，clip 是保证）
    constexpr float kKeyPxPerChar = 8.0f;
    const int keyMaxChars =
        std::max(6, static_cast<int>(keyColW / (kKeyPxPerChar * fs)));
    // 来源标注列（第二行左侧固定列）：宽 120 够放「本 Profile」/「片段・<名>」，
    // 超出部分由 .clip() 硬裁（不压到右侧官方说明列）
    constexpr float kSrcColW = 120.0f;
    constexpr float kSrcGap = 12.0f;
    // 行数预算（P2 口径，2026-09-28）：**纯按内容框可用高推**，不再有与内容
    // 无关的硬上限/硬下限。现在内容框底由容器底给出（contentBottom），预算
    // 直接除行距；行距随缩放派生（130% 档行距 66，能放的行数相应变少 ——
    // 放不下的行走「另有 N 项」提示，不越界）。溢出提示行只在真溢出时占位。
    const float availH = std::max(0.0f, contentBottom - y0);
    size_t shownRows = std::min(
        envMap.size(),
        static_cast<size_t>(static_cast<int>(availH / pitch)));
    // 溢出提示行也要放得下：放不下就让出一行（让到 0 行为止 —— 0 行时只画
    // 提示，不画任何越界行）
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
        // 键名过长则截断，避免压到右侧值列
        std::string kDisp = k;
        if (static_cast<int>(kDisp.size()) > keyMaxChars)
            kDisp = kDisp.substr(0, keyMaxChars - 1) + "…";
        const char* hint = envVarHint(k);
        const std::string rid = std::to_string(ei2);
        const float valX = x0 + 12.0f + keyColW + 24.0f;  // 键列↔值列间距 24（2026-09-25：12→24，反馈太挤）
        const float valW = std::max(40.0f, w - keyColW - 48.0f);
        ui.rect(std::string("claude.env.row.bg.") + rid)
            .position(x0, ey).size(w, rowH)
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, hint ? t.colors.borderMuted : t.colors.error)
            .build();
        // 行带 1：键名 | 值（同一行框同带，Center 对齐 → 「键 值」成一行）。
        // .clip() 兜底：截断估宽 / 单行策略都是估的，clip 是硬界 —— 长文本
        // 策略（E2）= 键按 kKeyPxPerChar 截断、值/来源/说明单行硬裁，**不换行
        // 不撑高**（换行会把第二行挤出固定行带，与下行叠印）
        ui.text(std::string("claude.env.key.") + rid)
            .position(x0 + 12.0f, ey + line1Y).size(keyColW, line1H)
            .clip()
            .text(kDisp).fontSize(keyFs).lineHeight(line1H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.primary).build();
        // 值列：框高 = 行带 1（旧实现 48 跨整卡居中，墨迹压进行带 2 —— 见
        // 上方几何注释；现在与键名同带，不再与说明行相交）
        ui.text(std::string("claude.env.val.") + rid)
            .position(valX, ey + line1Y).size(valW, line1H)
            .clip()
            .text(shownVal).fontSize(keyFs).lineHeight(line1H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary).build();
        // 来源标注（P3）：托管键 / 本 Profile 两类（原 Shared 带名分支随
        // advanced-json-editor §8.4 片段 env 消亡自然移除；枚举与 envSourceLabel
        // 保留，§8.5 复用于高级属性键标注）
        const auto srcIt = envSources.find(k);
        const EnvSource src = srcIt != envSources.end()
            ? srcIt->second : EnvSource::Profile;
        std::string srcText = envSourceLabel(src);
        // 行带 2：来源 | 官方说明。说明列加 .clip()（旧无 —— 长说明横穿值列，
        // 是「文本都重叠」的元凶之一）
        ui.text(std::string("claude.env.src.") + rid)
            .position(x0 + 12.0f, ey + line2Y).size(kSrcColW, line2H)
            .clip()
            .text(srcText).fontSize(smallFs).lineHeight(line2H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(src == EnvSource::Managed ? t.colors.textTertiary
                                             : t.colors.textSecondary)
            .build();
        ui.text(std::string("claude.env.hint.") + rid)
            .position(x0 + 12.0f + kSrcColW + kSrcGap, ey + line2Y)
            .size(std::max(40.0f, w - 24.0f - kSrcColW - kSrcGap), line2H)
            .clip()
            .text(hint ? hint : "（未收录官方清单）")
            .fontSize(smallFs).lineHeight(line2H)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary).build();
        ey += pitch;
        ++ei2;
    }
    if (envOverflow) {
        // 溢出提示（与表单 env 页「另有 N 项」同款口径）：未画出的行全量保存在
        // Profile 里，编辑时可见；模型键的去向一并说明（E1，本页已滤掉模型键）
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

// 空调用记录表（DetailView.recent 为 null 时的只读兜底；常量空表，无副作用）
const std::vector<ClaudeService::CallLogEntry>& emptyCalls() {
    static const std::vector<ClaudeService::CallLogEntry> kEmpty;
    return kEmpty;
}

// 最近调用 pane（tab 4，2026-09-28 由详情面板底部固定预览块抽成**独立页签**，
// 2026-09-30 batch-fix B 起 idx 3 由「高级配置」接手、本 pane 由 3 顺移至 4，
// 与「基础信息/模型映射/环境变量/高级配置」同级）：表头 + 行（行数纯按内容框
// 可用高推）
// + 错误/空态 + 「查看全部 →」（开完整调用记录弹窗）。行/表头 id 沿用旧底部
// 预览（claude.calls.*，无头用例按这些 id 断言），列布局逐字不变；返回本 pane
// 内容底沿（操作按钮组按此定位）。
float drawDetailCallsPane(eui::Ui& ui, float x0, float y0, float w,
                          const core::theme::Tokens& t,
                          const DetailView& v,
                          const std::function<void()>& openCalls,
                          float contentBottom, PaneRows* rows) {
    // 调用表头（列布局同旧底部预览；真机反馈 ② 2026-09-29：新增「速率」列
    // ——列宽从「方法/路径」让出 110px，右侧三列整体左移，路径列仍有
    // w−390（本机 1262 页宽下 ≈528px）不挤既有信息）
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

    // 行数预算（P2，2026-09-28）：**纯按内容框可用高推**，去掉两个与内容无关
    // 的兜底 —— 旧式 `clamp((bodyH − y0 − 160) / 24, 3, 10)`：
    //   · 硬上限 10：面板可容 14~21 行时也锁死 10 行，且截断无提示；
    //   · 硬下限 3：可用高不足时照样画 3 行，越界盖进动作区。
    // 现在：可用高 = 内容框底 − 表头底 − 「查看全部」固定带；画不满 1 行就
    // 只画空态（不画越界行），超出的行用「另有 N 条」表达。
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
    bool singleLineDrawn = false;  // 空态 / 错误行在场（它占一行，见下）
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
        // 溢出提示行也要放得下：放不下就让出一行（让到 0 行为止 —— 0 行时
        // 只画空态文案，不画任何越界行）
        size_t shown = std::min(recent.size(), rowsCap);
        while (shown > 0 &&
               static_cast<float>(shown) * kRowPitch + kOverflowH > rowsAvail) {
            --shown;
        }
        if (shown == 0) {
            // 内容框连一行都放不下：如实说「空间不足」，不硬塞越界行
            //（审计 F-02：这一档下**不再**画「另有 N 条」—— 它与本行同坐标
            // 会叠印，且会把「查看全部」带推出内容框；N 条的去向由下面那行
            // 「查看全部」入口表达，入口自己也放不下时文案不再承诺链接）
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

            // 本地调用速率（真机反馈 ②）：口径与首页「Token 速率」卡同一处
            //（core/call_rate.h）——仅 2xx 且 output_tokens > 0 的调用有速率；
            // 其余（错误响应 / 未采集用量）显示「—」：0 是合法速率值，不能拿
            // 0 或负数冒充「没有」
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
        // 截断必须有可见表达（方案 §1 组 C 第 3 条）：与 env 页签同款提示。
        // 0 行档不画（审计 F-02）：它与上面的空态行同坐标会叠印，且没有行可
        // 「另有」—— N 条的去向由下方「查看全部」入口表达
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

    // 查看全部 →（可点击：弹完整调用记录）——位置在所有行之后（空态行 / 溢出
    // 提示行在场时一并让位）。**越界一律不画**（审计 F-02）：内容框放不下时
    // 宁可不画入口，也不把它推出 contentBottom（空态行因此不再承诺该链接）
    const float viewY =
        cellsY + static_cast<float>(shownRows) * kRowPitch +
        (callsOverflow ? kOverflowH : 0.0f) +
        (singleLineDrawn ? kEmptyH + kViewallGap : kViewallGap);
    if (viewY + kViewallH <= contentBottom + 0.5f) {
        // 真机反馈 ⑥（2026-09-29）：「查看全部」原本是纯文本链接（透明底 +
        // 只有 hover 变色），用户要求做成按钮。样式取详情页既有次级按钮的口径
        //（surfaceElevated 底 + borderDefault 描边 + radiusSmall 圆角 +
        // surfaceElevated/hover/active 三态），文案居中于行。
        // 为什么自绘而不是 components::button：该组件的点击层 id 是 `<id>.bg`
        // 且尺寸由组件内部派生 —— 换组件要改掉既有单测/probe 定位的
        // claude.calls.viewall.hit，并重算本 pane 的行数预算让位逻辑；自绘同样
        // 满足「有 hover/点击态 + 圆角 + 与既有按钮同风格」，破坏面小得多。
        // **行高 kViewallH 不变**（它参与上方 rowsCap 的行数预算）。
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

// 高级配置 pane（2026-09-30 batch-fix B 建立；advanced-json-editor §8.5 加
// JSON 键来源清单，0edd3cf 形态复用）：方言 + 备用 Profile 两行只读 + 生效
// JSON 的逐顶层键来源行。行样式复用基础 pane 的 label+值盒写法（同款
// Center 对齐 + surfaceElevated 盒）。数据由 compose 装配进 DetailView
//（dialectText / fallbackName / extraSettingsKeys / extraSettingsNote，
// 注释见 .h），本函数零服务调用。
//
// **contentBottom 真消费（裁决⑤，变异⑥护栏）**：键清单行数纯按内容框可用高
// 推（8 行为暂定上限，溢出走「另有 N 项」）——本 pane 不再是「固定行数、高度
// 与容器无关」的 (void)contentBottom 形态；内容底沿恒 ≤ contentBottom，动作区
// 五页签同高不变式因此保持（分发区注释与 actY 相等断言同步成立）
float drawDetailAdvanced(eui::Ui& ui, float x0, float y0, float w,
                         const core::theme::Tokens& t, const DetailView& v,
                         float contentBottom) {
    // ---- 行几何（merge-review R2，2026-09-30）：全部从生效字号缩放派生 ----
    // 根因（与 env pane E2 同一台病）：旧代码标签字号 11/12 与行盒 14/18/28/34
    // 全按 100% 写死；全局字号缩放（core::theme::fontScale，设置页档位，最小档
    // kFontPresets[0] = 85%）缩小字号却不动行盒 / 行距 → 墨迹越出行盒、与下一块
    // 叠印。现在**字号随缩放、盒与纵向间距从 100% 基准 × fs 取整派生**：
    //   字号：标签 / 来源 / 说明 / 空态小字 = 11×fs；键名 = 12×fs
    //         （不取整，与 env pane 同款 —— 取整只发生在盒 / 间距上，像素对齐稳定）
    //   盒 / 间距 = round(基准 × fs)：标签行盒 14、标签底→盒顶 18、值盒高 28、
    //         值盒底→下一标签顶 10、行卡高 28、行间距 6、说明行 18、溢出行 18。
    //         **100% 档 round(x×1.0) == x，与旧字面常量逐一相等**（零回归）；
    //         85% 档 = 12 / 15 / 24 / 9 / 5，行盒恒包住墨迹（基准盒高 ≥ 1.25×
    //         基准字号，缩放后比值不变）。
    // 横向（列宽分割比例、内边距 12 / 20 / 32）按 env pane 既有做法**不缩放**。
    const float fs = core::theme::fontScale();
    const auto rfs = [fs](float base) { return std::round(base * fs); };
    const float labelFs = 11.0f * fs;                // 标签 / 来源 / 说明小字
    const float keyFs = 12.0f * fs;                  // 键名
    const float labelH = rfs(14.0f);                 // 标签行盒（100% = 14）
    const float labelGap = rfs(18.0f);               // 标签底 → 盒顶（100% = 18）
    const float fieldH = rfs(28.0f);                 // 方言 / 备用值盒高（100% = 28）
    const float fieldGap = rfs(10.0f);               // 值盒底 → 下一标签顶（100% = 10）
    // ---- 键来源清单（§8.5）：行高/行距与行数预算（P2 口径）----
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
        // 空态两种成因（compose 约定：note 空 = 未配置；note 非空 = 解析失败
        // 提示原样展示）
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
    // 行数预算：可用高 = contentBottom − 当前底沿；说明行/溢出行要放得下
    //（放不下就让行，让到 0 行为止——0 行时只画提示，不画越界行）
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
        ui.text("claude.advjson.row.key." + idx)
            .position(x0 + 12.0f, ey).size(w * 0.4f, kKeyRowH)
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
    // 真消费兜底：内容底沿不越过内容框（理论可达性低——行数预算已按
    // contentBottom 推；min() 是护栏不是布局来源）
    return std::min(ey, contentBottom);
}

// 操作按钮组（4 键：启用/测试连通/编辑/运行，batch-C C-3）+ 「运行」副提示
// + 分隔线。actY 由 drawDetailPanel
// **锚定容器底**给出（P1，2026-09-28）：旧式 `actY = paneBottom + 20` 跟着内容
// 底跑，数据少时动作区浮在上部、下方 60% 空白，四个页签的分隔线互差 171px。
// 本函数只按传入的 actY 画，不再自己算锚点（锚点是容器级决策）。
// 绘制高度 = 61（actY−10 分隔线 / actY..+36 按钮行 / actY+40..+56 副提示 /
// actY+60..+61 分隔线）—— 这个数被 drawDetailPanel 的 kActionBlockH 引用，
// 改这里的几何必须同步改那里（两处都在本文件，见文件顶部常量块）。
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
    // 4 键均分（batch-C C-3：删除/建副本/复制配置三键自本行动作组删除——它们
    // 已在 profile 列表右键菜单里（drawProfileContextMenu，动作同源委托），
    // 详情页三键属重复入口；余 启用/测试连通/编辑/运行）。按钮数改一处，
    // gap 数也要跟着改：扣 **3** 个 gap —— EUI 的 row spacing 只在相邻子项之间
    // 生效，n 项只有 n−1 个间隙，故 (w-actGap*3)/4 恰好齐右缘
    const float actW = (w - actGap * 3.0f) / 4.0f;
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
            // 删除 / 建副本 / 复制配置（batch-C C-3）：三个重复入口已从本行
            // 删除——同源业务逻辑（requestDelete → 确认弹窗 / doDuplicate /
            // doCopyConfig）保留在列表右键菜单（claude_ui_list.cpp 的
            // drawProfileContextMenu，五项菜单），详情页不再重复渲染
            // 运行（第 4 键，2026-09-28 profile-run 设计
            // D-1/D-2/D-4）：在新终端里用该 Profile 打开
            // 一个 claude 会话
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
        })
        .build();

    // 「运行」副提示（设计 §6 / V3 D-1）：副作用必须明示——生成临时
    // settings 直连，不启代理、不切激活、零全局改动
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
    // ---- 右栏：详情面板（三 pane + 最近调用页签）----
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

            // 页签栏（2026-09-25：改用 EUI-NEO tabs 组件，自带动画指示器+
            // 底部分隔线+居中文字；色源走 componentTokens 桥接。tabs 无
            // .position 故包一层定位 stack 对齐右栏内边距，沿用 settings
            // 模块 .wrap stack 同款写法）。
            // 2026-09-28：最近调用从底部固定预览块抽成页签
            //（与基础信息/模型映射/环境变量同级）。
            // 2026-09-30（batch-fix B）：插「高级配置」为第 4 页签（idx 3，
            // 属性展示层），最近调用是时序记录顺移 idx 4——既有用例/探针经
            // claude.detail.tabs.hit.3 导航锚按索引点击处 +1（spec §3.5 风险 3）
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

            // ---- P1（2026-09-28）：动作区钉面板底 + 内容框显式边界 ----
            // 统一原则（方案 §1 组 B，后续维护照此判）：**容器可用高只有一个
            // 来源**；贴底元素锚容器底；内容视口 = 容器底 − 已知 chrome；
            // 所有「向下展开 / 是否越界」的判定用**真实裁剪下沿**，不用推导值。
            // 这里的容器底 = bodyH（唯一来源，由 claude_module 从 screen.height
            // 减已知 chrome 得到）；贴底元素 = 动作区；已知 chrome = 动作块
            // 61 + 下方呼吸 24；真实裁剪下沿 = contentBottom（内容框可用底）。
            // 全部**锚定式**（相对 bodyH），不写任何绝对 y —— Windows 侧
            // bodyH 与 macOS 差 15~45px，写死绝对值会在 Windows 上错位。
            const float actYFloor = bodyH - kActReserve;   // 贴底动作区的 actY
            const float contentBottom = actYFloor - kActTopGap;  // 内容框可用底

            // 内容框（显式边界）：四个页签同一块固定区域 —— 内容少时它把
            // 「这块是内容区、现在是空的」画出来，而不是一片无法解释的空白
            //（用户主诉）。底色走 surface（面板底是 background），描边与圆角
            // 走既有 token，不引魔法色值。画在 pane 内容之前 = 作内容底板。
            const float boxTop = paneY - 12.0f;
            ui.rect("claude.detail.contentbox")
                .position(x0 - 12.0f, boxTop)
                .size(w0 + 24.0f, std::max(0.0f, contentBottom - boxTop))
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderMuted)
                .build();

            float paneBottom = paneY;  // 各 pane 累加真实底沿，供 max() 兜底防重叠
            PaneRows paneRows;         // 行数统计（诊断日志用，见下方 LOG_DEBUG）
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
                // 高级配置（2026-09-30 batch-fix B）：固定两行只读，走 ② 档
                // 既有锚定/兜底链（同基础 pane 路径），零改动
                paneBottom = drawDetailAdvanced(ui, x0, paneY, w0, t, v,
                                                contentBottom);
            } else {
                paneBottom = drawDetailCallsPane(
                    ui, x0, paneY, w0, t, v, a.openCalls,
                    contentBottom, &paneRows);
            }
            // 锚定 + 两处兜底（审计 F-03 把口径写准）：
            //   ① 正常档（bodyH ≥ 445 ⟺ 客户区高 ≥ 639）：内容被内容框约束住，
            //      五个页签都取 actYFloor → **分隔线 y 全等**（这是不变量成立的条件，
            //      不是无条件等式 —— 曾经这么写是错的）；
            //   ② 更矮时：基础信息 / 模型映射两个 pane 是**固定行数内容**
            //     （6 字段 / 6 槽位表行，paneBottom 恒 312 / 340，都无视
            //     contentBottom），`paneBottom + kActTopGap > actYFloor` → 左支
            //     接管，动作区随内容下移，五页签**不再同高**。高级配置 pane 自
            //     advanced-json-editor §8.5（裁决⑤）起**真消费 contentBottom**
            //     （键清单行数按可用高推 + 底沿 clamp），不再有与容器无关的固定
            //     底沿——正常档与更矮档都吃右支锚定，五页签同高不变式因此保持
            //    （§10 变异⑥的护栏 = contentBottom 消费，行数上限 8 只是暂定）；
            //   ③ 再矮（内容底已越过 bodyH − kActionBlockH）：用 actYHi 夹住 ——
            //      宁可让动作区压在内容上（可见的重叠），也不让它越出容器底
            //      （贴底锚定的下限语义；旧式纯 max() 会把它推出面板）。
            // 组 A 之后最小窗客户区 ≈805 点 → bodyH ≈ 611，②③ 余量 ~166px 不可达。
            const float actYHi = std::max(bodyH - kActionBlockH, actYFloor);
            const float actY = std::min(
                std::max(actYFloor, paneBottom + kActTopGap), actYHi);
            drawDetailActions(ui, x0, w0, actY, t, v, a);
            // 诊断日志（方案 §5 组 B 判据 ②）：一行可 grep —— 五个页签的
            // actY 必须相等。级别 Debug（设置页可切；默认 Info 不打，避免
            // 每帧刷屏）
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
