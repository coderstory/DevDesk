// src/module_claude_proxy/claude_ui_detail.cpp
// R4（P-B3）：右栏三 pane + 操作按钮组 + 「最近调用」预览 —— 逐字自
// claude_module.cpp 的 compose 搬出（含注释），只把 this 成员换成显式入参
// （DetailView 只读输入 / DetailActions 回调）。
#include "claude_ui_detail.h"

#include "claude_ui_logic.h"
#include "env_var_hints.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/tabs.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace module::claude {

float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v) {
    const Profile* shown = v.shown;
    // ---- 基础信息 pane：名称 + 未激活徽章 + Base URL + API Key + 接管 ----
    ui.text("claude.field.name.label")
        .position(x0, y0).size(w * 0.5f - 8.0f, 14.0f)
        .text("名称").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.name.bg")
        .position(x0, y0 + 18.0f).size(w * 0.5f - 8.0f, 24.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.name.value")
        .position(x0 + 12.0f, y0 + 18.0f)
        .size(w * 0.5f - 20.0f, 24.0f)
        .text(v.nameValue).fontSize(14.0f).lineHeight(24.0f)
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
            .color(t.colors.warning).build();
    }
    const float urlY = y0 + 56.0f;
    ui.text("claude.field.url.label")
        .position(x0, urlY).size(w, 14.0f)
        .text("Base URL").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.url.bg")
        .position(x0, urlY + 18.0f).size(w, 28.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.url.value")
        .position(x0 + 12.0f, urlY + 18.0f).size(w - 24.0f, 28.0f)
        .text(v.urlValue).fontSize(t.sizes.fontSizeSmall).lineHeight(28.0f)
        .color(t.colors.textPrimary).build();
    const float keyY = urlY + 56.0f;
    ui.text("claude.field.token.label")
        .position(x0, keyY).size(w, 14.0f)
        .text("API Key").fontSize(11.0f).lineHeight(14.0f)
        .color(t.colors.textTertiary).build();
    ui.rect("claude.field.token.bg")
        .position(x0, keyY + 18.0f).size(w, 32.0f)
        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderMuted).build();
    ui.text("claude.field.token.value")
        .position(x0 + 12.0f, keyY + 18.0f).size(w - 24.0f, 32.0f)
        .text(v.tokenValue).fontSize(t.sizes.fontSizeSmall).lineHeight(32.0f)
        .color(t.colors.textSecondary).build();
    ClaudeService* svc = v.svc;
    const bool ccManaged = svc && svc->isClaudeCodeManaged();
    // 审计 CPX-02：接管失败（无 sidecar → 未接管）与上层覆盖
    // 警告都要让用户看见，故不再以 ccManaged 为展示门。读的是
    // 内存 getter（无 IO/DB，符合 H2），启停代理 / 切 profile
    // 后下一帧即刷新
    const auto& ccWarnings = svc ? svc->lastSyncWarnings()
                                : std::vector<std::string>();
    const std::string ccPath =
        settings_sync::defaultPaths().settingsFile.string();
    const float toY = keyY + 56.0f;
    ui.text("claude.field.takeover.value")
        .position(x0, toY).size(w, 16.0f)
        .text(ccManaged ? ("● Claude Code 已接管 " + ccPath)
                        : std::string("○ Claude Code 未接管（启动代理后自动接管，停止时还原）"))
        .fontSize(11.0f).lineHeight(16.0f)
        .color(ccManaged ? t.colors.success : t.colors.textTertiary).build();
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
            .position(x0, toY + 16.0f).size(14.0f, 16.0f).icon(0xF071)
            .fontSize(11.0f).lineHeight(16.0f).color(t.colors.warning).build();
        ui.text("claude.field.takeover.warn")
            .position(x0 + 18.0f, toY + 16.0f).size(w - 18.0f, 16.0f)
            .text(warnText)
            .fontSize(11.0f).lineHeight(16.0f).color(t.colors.warning).build();
    }
    return toY + (!ccWarnings.empty() ? 32.0f : 16.0f);
}

float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v) {
    const Profile* shown = v.shown;
    // ---- 模型映射 pane：槽位逐行（遍历槽位表派生；widget id 序号 = 表序）----
    ModelsConfig mc{};
    if (shown && !shown->modelsConfigJson.empty()) {
        auto mr = parseModelsConfig(shown->modelsConfigJson);
        if (mr.isOk()) mc = mr.value();
    }
    if (mc.def.model.empty() && shown && !shown->model.empty())
        mc.def.model = shown->model;  // 旧列 fallback
    float sy = y0;
    int slotIdx = 0;
    for (const auto& meta : modelSlotTable()) {
        const ModelSlot& slot = mc.*meta.slot;
        const bool filled = !slot.model.empty();
        const std::string idx = std::to_string(slotIdx++);
        ui.rect("claude.slot.bg." + idx)
            .position(x0, sy).size(w, 32.0f)
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderMuted).build();
        ui.text("claude.slot.name." + idx)
            .position(x0 + 12.0f, sy).size(80.0f, 32.0f)
            .text(meta.label).fontSize(12.0f).lineHeight(32.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.slot.val." + idx)
            .position(x0 + 100.0f, sy).size(w - 112.0f, 32.0f)
            .text(filled ? slot.model : std::string("未配置"))
            .fontSize(13.0f).lineHeight(32.0f)
            .color(filled ? t.colors.textPrimary : t.colors.textTertiary).build();
        sy += 38.0f;
    }
    return sy;
}

float drawDetailEnv(eui::Ui& ui, float x0, float y0, float w,
                    const core::theme::Tokens& t, const DetailView& v) {
    const Profile* shown = v.shown;
    // ---- 环境变量 pane：键（截断）+ 值（脱敏）同列不重叠；下方官方说明；未知键红框 ----
    std::map<std::string, std::string> envMap;
    if (shown && !shown->extraEnvJson.empty()) {
        auto er = parseExtraEnv(shown->extraEnvJson);
        if (er.isOk()) envMap = er.value();
    }
    if (envMap.empty()) {
        ui.text("claude.env.empty")
            .position(x0, y0).size(w, 20.0f)
            .text("无额外环境变量").fontSize(12.0f).lineHeight(20.0f)
            .color(t.colors.textTertiary).build();
        return y0 + 20.0f;
    }
    const float keyColW = std::min(220.0f, w * 0.45f);
    const int keyMaxChars = std::max(6, static_cast<int>(keyColW / 7.0f));
    // 行数上限（2026-09-27 修）：此前无上限，行一多 paneBottom
    // 一路下探，把下方按钮组/最近调用表/「查看全部」顶出面板
    // 下沿，被 app.page clip 裁掉点不到。下方固定占用 236px
    //（actY = paneBottom+20，查看全部命中区底 = actY+236）+
    // 20px「另有 N 项」提示；超出部分不画只提示（不引 scrollView：
    // 行绘制用 overlay 绝对坐标，进 content lambda 需整段改相对
    // 坐标 + 新增滚动 offset 成员，改动面大；表单 env 编辑器
    // 已有 kMaxVisibleRows + 溢出提示同款先例）
    const size_t envMaxRows = std::max<size_t>(
        1, static_cast<size_t>(
               std::max(0.0f, v.bodyH - y0 - 276.0f) / 54.0f));
    float ey = y0;
    int ei2 = 0;
    for (const auto& kv : envMap) {
        if (static_cast<size_t>(ei2) >= envMaxRows) break;
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
            .position(x0, ey).size(w, 48.0f)
            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
            .border(1.0f, hint ? t.colors.borderMuted : t.colors.error)
            .build();
        ui.text(std::string("claude.env.key.") + rid)
            .position(x0 + 12.0f, ey).size(keyColW, 20.0f)
            .text(kDisp).fontSize(12.0f).lineHeight(20.0f)
            .color(t.colors.primary).build();
        // 值列单行：超宽（最小窗 valW≈194px）裁掉，
        // 不溢出面板/压到右栏外（clip = 自身 frame 取景）
        ui.text(std::string("claude.env.val.") + rid)
            .position(valX, ey).size(valW, 48.0f)
            .clip()
            .text(shownVal).fontSize(12.0f).lineHeight(20.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary).build();
        ui.text(std::string("claude.env.hint.") + rid)
            .position(x0 + 12.0f, ey + 24.0f).size(w - 24.0f, 18.0f)
            .text(hint ? hint : "（未收录官方清单）")
            .fontSize(11.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();
        ey += 54.0f;
        ++ei2;
    }
    if (envMap.size() > static_cast<size_t>(ei2)) {
        // 溢出提示（与表单 env 页「另有 N 项」同款口径）：
        // 未画出的行全量保存在 Profile 里，编辑时可见
        ui.text("claude.env.overflow")
            .position(x0, ey).size(w, 18.0f)
            .text("另有 " +
                  std::to_string(envMap.size() -
                                 static_cast<size_t>(ei2)) +
                  " 项未显示（编辑该 Profile 可查看全部）")
            .fontSize(11.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();
        ey += 20.0f;
    }
    return ey;
}

namespace {

// 空调用记录表（DetailView.recent 为 null 时的只读兜底；常量空表，无副作用）
const std::vector<ClaudeService::CallLogEntry>& emptyCalls() {
    static const std::vector<ClaudeService::CallLogEntry> kEmpty;
    return kEmpty;
}

// 操作按钮组（全量接线）+ 「运行」副提示 + 分隔线 + 最近调用预览 + 「查看全部」。
// actY 跟随 pane 真实底沿（= 入参 paneBottom + 20），避免与上方内容重叠
void drawDetailActionsAndCalls(eui::Ui& ui, float x0, float w, float paneBottom,
                               const core::theme::Tokens& t,
                               const DetailView& v, const DetailActions& a) {
    const Profile* shown = v.shown;
    const float actY = paneBottom + 20.0f;
    ui.rect("claude.act.divider")
        .position(x0, actY - 10.0f)
        .size(w, 1.0f)
        .color(t.colors.borderDefault).build();
    const float actGap = 12.0f;
    // 6 键均分（设计 §4.1，M5 + profile-run：启用/测试连通/
    // 编辑/删除/复制/运行）——按钮数改一处，gap 数也要跟着改。
    // 扣 **5** 个 gap：EUI 的 row spacing 只在相邻子项之间生效，
    // n 项只有 n−1 个间隙；旧 5 键式 (w0-actGap*4)/5 恰好齐右缘。
    const float actW = (w - actGap * 5.0f) / 6.0f;
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
            // 删除（确认 dialog）
            components::button(ui, "claude.act.del")
                .size(actW, 36.0f)
                .text("删除")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.error)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.requestDelete, warn = a.warn, shownCopy] {
                    if (shownCopy.id == 0) {
                        warn("删除失败", "请先新建或选择一个 Profile");
                        return;
                    }
                    f(shownCopy.id, shownCopy.name);
                })
                .build();
            // 复制（第 5 键，设计 §4.1/§4.2）：副本不激活
            components::button(ui, "claude.act.dup")
                .size(actW, 36.0f)
                .text("复制")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.duplicate, warn = a.warn, shownId] {
                    if (shownId == 0) {
                        warn("复制失败", "请先选择 Profile");
                        return;
                    }
                    f(shownId);
                })
                .build();
            // 运行（第 6 键，2026-09-28 profile-run 设计
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

    // 「运行」副提示（设计 §6：D-1 副作用必须明示；§3 措辞
    // 按「选中 ≠ 激活才切」表述，避免用户以为每次都改全局）
    ui.text("claude.act.run.hint")
        .position(x0, actY + 40.0f)
        .size(w, 16.0f)
        .text("以该 Profile 打开新的 Claude Code 会话"
              "（起始目录 = 用户主目录）；未激活会先切为激活")
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .color(t.colors.textTertiary)
        .build();

    // 分隔线
    ui.rect("claude.divider")
        .position(x0, actY + 60.0f)
        .size(w, 1.0f)
        .color(t.colors.borderMuted)
        .build();

    // 最近调用
    ui.text("claude.calls.title")
        .position(x0, actY + 76.0f)
        .size(w, 20.0f)
        .text("最近调用")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    // 调用表头
    const float tableY = actY + 108.0f;
    const float colTimeX = x0;
    const float colPathX = x0 + 80.0f;
    const float colStatusX = x0 + w - 200.0f;
    const float colDurX = x0 + w - 100.0f;
    ui.text("claude.calls.h.time")
        .position(colTimeX, tableY)
        .size(72.0f, 14.0f)
        .text("时间")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.path")
        .position(colPathX, tableY)
        .size(w - 280.0f, 14.0f)
        .text("方法 / 路径")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.status")
        .position(colStatusX, tableY)
        .size(60.0f, 14.0f)
        .text("状态")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();
    ui.text("claude.calls.h.dur")
        .position(colDurX, tableY)
        .size(60.0f, 14.0f)
        .text("耗时")
        .fontSize(10.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // 最近调用（真实数据，从 claude_call_logs 读）
    const std::vector<ClaudeService::CallLogEntry>& recent =
        v.recent ? *v.recent : emptyCalls();
    const std::string& callsError = v.callsError;
    const std::time_t now = v.now;
    if (!callsError.empty()) {
        ui.text("claude.calls.error")
            .position(colTimeX, tableY + 24.0f)
            .size(w - 80.0f, 16.0f)
            .text("调用记录加载失败：" + callsError)
            .fontSize(11.0f)
            .lineHeight(16.0f)
            .color(t.colors.error)
            .build();
    } else if (recent.empty()) {
        ui.text("claude.calls.empty")
            .position(colTimeX, tableY + 24.0f)
            .size(w - 80.0f, 16.0f)
            .text("(暂无调用记录)")
            .fontSize(11.0f)
            .lineHeight(16.0f)
            .color(t.colors.textTertiary)
            .build();
    } else {
        for (size_t i = 0; i < recent.size() && i < 3; ++i) {
            const ClaudeService::CallLogEntry& c = recent[i];
            const float rowY = tableY + 24.0f +
                static_cast<float>(i) * 24.0f;

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
                .size(w - 280.0f, 16.0f)
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
        }
    }

    // 查看全部 →（可点击：弹完整调用记录）
    ui.rect("claude.calls.viewall.hit")
        .position(x0, tableY + 104.0f)
        .size(w, 24.0f)
        // 命中区底 = 所在详情面板底（background）
        .color(t.colors.background)
        .hoverColor(t.colors.hover)
        .radius(t.sizes.radiusSmall)
        .onClick([oc = a.openCalls] { oc(); })
        .build();
    ui.text("claude.calls.viewall")
        .position(x0, tableY + 108.0f)
        .size(w, 18.0f)
        .text("查看全部 →")
        .fontSize(12.0f)
        .lineHeight(18.0f)
        .color(t.colors.primary)
        .build();
}

} // namespace

void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a) {
    // ---- 右栏：详情面板 + 最近调用 ----
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
            // 模块 .wrap stack 同款写法）
            const float tabY = y0 + 40.0f;
            const float tabH = 36.0f;
            ui.stack("claude.detail.tabs.wrap")
                .position(x0, tabY)
                .size(w0, tabH)
                .content([&] {
                    components::tabs(ui, "claude.detail.tabs")
                        .size(w0, tabH)
                        .items(std::vector<std::string>{
                            "基础信息", "模型映射", "环境变量"})
                        .selected(st.detailTab_)
                        .fontSize(t.sizes.fontSizeSmall)
                        .theme(themeTokens)
                        .onChange([sp = &st](int idx) { sp->detailTab_ = idx; })
                        .build();
                });

            const float paneY = tabY + tabH + 12.0f;
            float paneBottom = paneY;  // 各 pane 累加真实底沿，供 actions 定位防重叠
            if (st.detailTab_ == 0) {
                paneBottom = drawDetailBasic(ui, x0, paneY, w0, t, v);
            } else if (st.detailTab_ == 1) {
                paneBottom = drawDetailModels(ui, x0, paneY, w0, t, v);
            } else {
                paneBottom = drawDetailEnv(ui, x0, paneY, w0, t, v);
            }
            drawDetailActionsAndCalls(ui, x0, w0, paneBottom, t, v, a);
        })
        .build();
}

} // namespace module::claude
