// src/ui_kit/wtab.cpp — Tab helper 实现（need#501 2026-11-24 定稿 B「精修下划线」）。
// 色值全走 core::theme::Tokens 槽位，禁裸 hex（D-03）。
// 选中 = 底部圆角指示条（高 2.5 / 宽=文字宽 / radius 999 / primaryText 槽）+
// textPrimary 文字；未选 = 透明底 + textSecondary 文字；hover = primarySoftBg
// 槽底（OutQuad 120ms 淡入）；按压 = scale 0.97 果冻（下 100ms OutQuad /
// 松开 200ms OutBack）。非照搬旧 components::tabs 参数（need501 §4 差异表）。
// 指示条跨 tab 滑移 160ms OutCubic 自实现（框架无滑块组件）：立即态 UI 单线程
// 逐帧重建，EUI Frame 过渡只在同 id 元素间生效，跨 tab 滑移靠静态缓存记录
// 组内上次激活 tab 的指示条目标几何，本帧按 elapsed 手工插值画 <id>.ind。
#include <ui_kit/wtab.h>

#include <components/theme.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ui_kit {
namespace {

constexpr float kIndHeight = 2.5f;      // need501 §4：指示条高 2.5
constexpr float kIndRadius = 999.0f;    // 胶囊条
constexpr float kSlideSeconds = 0.16f;  // need501 §10：OutCubic 160ms
constexpr float kHoverSeconds = 0.12f;  // need501 §10：hover 淡入 OutQuad 120ms
constexpr float kPressScale = 0.97f;    // need501 §10：果冻按压
constexpr float kPressDownSeconds = 0.1f;
constexpr float kPressUpSeconds = 0.2f;
constexpr float kHoverRadiusPage = 8.0f;   // need501 §4：页面级 hover 底圆角
constexpr float kHoverRadiusDialog = 6.0f; // need501 §4：弹窗级 hover 底圆角

float outCubic(float p) {
    const float q = 1.0f - p;
    return 1.0f - q * q * q;
}

float lerp(float from, float to, float p) {
    return from + (to - from) * p;
}

// 滑移缓存：group（=tab id 去最后一段）→ 当前激活 tab 的指示条目标几何。
// shortcut: 组上限 64，超限整表清空重来（可观察事件=新页面 tab 组数超 64）。
struct IndTarget {
    std::string activeId;
    float x = 0.0f;  // 绝对 x（组内各 tab 的 stack 原点不同，必须用绝对坐标）
    float w = 0.0f;
    std::chrono::steady_clock::time_point t0{};
};
std::unordered_map<std::string, IndTarget>& slideCache() {
    static std::unordered_map<std::string, IndTarget> cache;
    return cache;
}

// 按压态 id 集合（hit rect 的 onPress/onRelease 维护）。
// shortcut: 上限 64，超限清空（泄漏场景=元素带按压销毁，正常交互不会积压）。
std::unordered_set<std::string>& pressedTabs() {
    static std::unordered_set<std::string> pressed;
    return pressed;
}

// P2 F7：键加窗口实例限定——多窗（app::openWindow）同页渲染时静态缓存
// 按 Ui 实例隔离，防同 id 互踩 t0/pressed（onPress/onRelease 捕获 &ui 为
// 引用安全：调用期在 ui 生存期内）。
std::string windowKey(const eui::Ui& ui) {
    return std::to_string(reinterpret_cast<std::uintptr_t>(&ui));
}

// 文本宽度估算：release chipWidth / schedule textWidth 同式（CJK 0.43 /
// ASCII 0.56 × 字号 + 1.6 字号基宽；略偏大只多留白、不裁字）。
float wtabTextWidth(const std::string& text, float fontSize) {
    float w = fontSize * 1.6f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? fontSize * 0.43f : fontSize * 0.56f;
    }
    return w;
}

} // namespace

void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick, float fontSize) {
    // need501 §4：弹窗级（fontSize=小档）hover 底圆角 6，页面级 8。
    // F10-2：浮点等值判档前提=调用方传字面常量（kWtabFontSize/kWtabSmallFontSize），
    // 传计算值会静默失判——改显式 level 参数需动公开签名，此处钉死前提。
    const bool dialogLevel = (fontSize == kWtabSmallFontSize);
    const float hoverRadius = dialogLevel ? kHoverRadiusDialog : kHoverRadiusPage;
    const bool pressed = pressedTabs().count(id) > 0;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .scale(pressed ? kPressScale : 1.0f)
        // 果冻按压：按下 OutQuad 100ms、松开 OutBack 200ms（need501 §10）
        .transition(pressed ? kPressDownSeconds : kPressUpSeconds,
                    pressed ? core::Ease::OutQuad : core::Ease::OutBack)
        .animate(core::AnimProperty::Transform)
        .content([&] {
            // hover 底：常驻 rect（透明，hover 时 primarySoftBg 槽）。
            // 常驻而非 active 专属：B 无静止底，hover 态需要未选 tab 也有载体。
            ui.rect(id + ".bg")
                .size(w, h)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .hoverColor(t.colors.primarySoftBg)
                .radius(hoverRadius)
                .transition(kHoverSeconds, core::Ease::OutQuad)
                .animate(core::AnimProperty::Color)
                .build();

            // 指示条：仅激活 tab，底部贴缘、宽=文字宽、跨 tab 滑移手工插值。
            // 首次激活无滑移（§9：首次打开无动画初始位），起点=终点。
            if (active) {
                const float indW = wtabTextWidth(label, fontSize);
                const float restAbsX = x + (w - indW) * 0.5f;
                const auto now = std::chrono::steady_clock::now();
                auto& cache = slideCache();
                if (cache.size() >= 64) cache.clear();
                const std::string group =
                    windowKey(ui) + '|' + id.substr(0, id.find_last_of('.'));
                IndTarget& slot = cache[group];
                if (slot.activeId != id) {
                    if (slot.activeId.empty()) {
                        slot.x = restAbsX;  // 组内首个激活：无动画初始位
                        slot.w = indW;
                    }
                    slot.activeId = id;
                    slot.t0 = now;
                }
                const float elapsed =
                    std::chrono::duration<float>(now - slot.t0).count();
                const float p = outCubic(std::min(1.0f, elapsed / kSlideSeconds));
                const float curX = lerp(slot.x, restAbsX, p);
                const float curW = lerp(slot.w, indW, p);
                slot.x = curX;  // 记录本帧插值位：下次切换从此起步（中断动画不跳变）
                slot.w = curW;

                ui.rect(id + ".ind")
                    .position(curX - x, h - kIndHeight)
                    .size(curW, kIndHeight)
                    .color(t.colors.primaryText)
                    .radius(kIndRadius)
                    .build();
            }

            // hit area：透明 rect 挂点击 + 按压态
            auto hit = ui.rect(id + ".hit")
                .size(w, h)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f});
            if (onClick) {
                hit.onClick(onClick);
            }
            // 按值捕获 id：回调存入常驻 element，事件分发期 wtab 已返回，
            // 按引用捕获 = 悬垂 UB（审计 F1，仓内范式 app_sidebar.cpp:762）。
            hit.onPress([id, &ui](const eui::PointerEvent&, const eui::Rect&) {
                auto& pressed = pressedTabs();
                if (pressed.size() >= 64) pressed.clear();
                pressed.insert(windowKey(ui) + '|' + id);
            });
            hit.onRelease([id, &ui](const eui::PointerEvent&, const eui::Rect&) {
                pressedTabs().erase(windowKey(ui) + '|' + id);
            });
            hit.build();

            // 文字：水平居中 + 垂直居中。选中不加粗（need501 §3.1：防文字宽
            // 跳动，选中态靠指示条+色表达）。
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(core::theme::scaled(fontSize))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? t.colors.textPrimary : t.colors.textSecondary)
                .build();
        })
        .build();
}

// ---- Tab 行 + 右端固定计数（池191/171 统一范式，规格见 wtab.h） ----

void wtabRow(eui::Ui& ui, const std::string& idBase,
             const std::vector<std::string>& labels, int selected,
             const std::string& countText, float x, float y, float rowW,
             float h, const core::theme::Tokens& t,
             const std::function<void(int)>& onTab, float fontSize) {
    const float tabH = kWtabHeight;
    const float tabY = y + (h - tabH) * 0.5f;
    const float gap = core::theme::scaled(8.0f);  // spacing 8 档
    float cx = x;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const bool active = static_cast<int>(i) == selected;
        const float tabW = wtabTextWidth(labels[i], fontSize) + 24.0f;
        wtab(ui, idBase + ".tab." + std::to_string(i), labels[i], active, cx,
             tabY, tabW, tabH, t,
             [onTab, i] {
                 if (onTab) onTab(static_cast<int>(i));
             },
             fontSize);
        cx += tabW + gap;
    }

    if (countText.empty()) return;
    const float countFont = t.sizes.fontSizeSmall;
    const float countW = wtabTextWidth(countText, countFont);
    ui.text(idBase + ".tabcount")
        .position(x + rowW - countW, y)
        .size(countW, h)
        .text(countText)
        .fontSize(countFont)
        .lineHeight(h)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace ui_kit
