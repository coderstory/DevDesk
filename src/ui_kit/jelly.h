// src/ui_kit/jelly.h
// presentation 能力层：果冻卡（L2 业务卡 = 容器内一层条目）。半透底 + 描边 + 阴影 +
// hover 形变（圆角变大 + 轻微放大）。只给选择项 / 按钮这类 L2 交互控件用，L1 容器卡不用。
#pragma once

#include <core/component_theme.h>  // core::theme::componentTokensWeChat（panelShadow 入参）
#include <core/theme.h>            // core::theme::Tokens
#include <components/theme.h>      // components::theme::panelShadow
#include <eui/dsl.h>               // eui::Ui / RectBuilder
#include <ui_kit/glass.h>          // glassFill（与 itemSurface 同源）

#include <functional>
#include <string>
#include <utility>

namespace ui_kit {

struct JellyOptions {
    float radius = 12.0f;         // 常态圆角
    float hoverRadius = 16.0f;    // hover 更圆 = 更软
    float pressedRadius = 8.0f;   // 按下压扁
    float hoverScale = 1.02f;
    float pressedScale = 0.97f;
    // 按压形变来源元素 id（通常 = 卡的命中面 <id>.hit，命中面是兄弟中最上层）。
    // 非空时对卡本体挂 visualStateFrom(该 id, pressedScale)——按下果冻由运行时
    // 依赖式驱动（源元素须同帧存在），hover 形变仍走上面每帧换值的字段。
    std::string pressedFrom;
};

// 果冻卡：`hovered` 必须由调用方**每帧 compose 时**传（来源见方案 §10.0.1 的 hover 状态集合）。
// 传常量 false 的话本函数就是个静态玻璃卡——果冻永远不会动，这是设计如此，见下面坑①。
//
// 五条坑（逐条读过 EUI-NEO 源码，任一条不满足都是静默失效：不报错、不崩、只是没效果）：
// ① `.transition()` + `.animate()` 只是「允许动」，不会自己动。目标值来自 compose 期
//    写入的 element.radius（runtime_update.h 的 instance.radius.setTarget(element.radius)），
//    而 `.states()` 只改颜色（dsl.h）改不了圆角 → 圆角想动必须每帧 compose 出不同的
//    radius 值，动画只负责把「上一帧的值」平滑到「这一帧的值」。
// ② `.scale()` 同理是 compose 期写入的静态值 → hover 缩放也必须每帧换值。
// ③ `.transformOrigin` 是归一化坐标，默认 (0.5, 0.5) 即卡中心；不写也对，但**不能改成
//    0/1 之类**，否则 hover 时整卡朝角落缩、看着像错位。
// ④ 按下态别在这里自己算 scale：EUI 的 `.visualStateFrom(id + ".bg", pressedScale)` 会把
//    pressedScale clamp 到 [0.80, 1.0]（dsl.h）——0.97 合法，0.9 会被抬回 0.97 一档。
//    且它是依赖式的，源元素必须同帧存在。
// ⑤ `hoverOpacityFrom` 与 `runtimePointerTransformFrom` 不要挂在同一个元素上（都写
//    opacity/transform 同一通道会打架）。卡本体形变走本函数，操作组显隐走前者。
inline void jellyCard(eui::Ui& ui, const std::string& id, float x, float y,
                      float w, float h, const core::theme::Tokens& t,
                      const JellyOptions& opt = {},
                      bool hovered = false,
                      std::function<void()> onClick = {}) {
    auto r = ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(glassFill(t))
        // 坑①：圆角目标值每帧重新给，transition + animate(Radius) 只负责平滑过去
        .radius(hovered ? opt.hoverRadius : opt.radius)
        // 坑③：缩放中心必须在卡中心
        .transformOrigin(0.5f, 0.5f)
        // 坑②：缩放目标值同样每帧重新给
        .scale(hovered ? opt.hoverScale : 1.0f)
        .border(1.0f, t.colors.borderDefault)
        .shadow(components::theme::panelShadow(
            ::core::theme::componentTokensWeChat(t)))
        .transition(core::Transition::make(0.18f, core::Ease::OutBack))
        .animate(core::AnimProperty::Radius | core::AnimProperty::Transform);
    if (!opt.pressedFrom.empty()) {
        r.visualStateFrom(opt.pressedFrom, opt.pressedScale);
    }
    if (onClick) r.onClick(std::move(onClick));
    r.build();
}

} // namespace ui_kit
