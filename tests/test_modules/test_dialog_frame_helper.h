// tests/test_modules/test_dialog_frame_helper.h
// F1 弹窗「元素在框内」防回潮断言（todo / claude / agent 三测试文件共享，header-only）。
// 契约（设计规格 S2.4）：开态弹窗 panel（components::dialog 的 <id>.panel）的全部
// 可见子孙元素 frame 必须完全落在 panel frame 内；标题行上缘 y=16、fontSize=20。
// 不变量：仅在 panel 开态（opacity == 1）下调用——关态 panel 带 translateY/scale
// 收缩动画，layout frame 不含 transform，关态断言无意义。
#pragma once

#include <eui/dsl.h>

#include <cmath>
#include <string>

namespace dialog_frame {

inline constexpr float kPxTol = 0.5f;  // 亚像素舍入容差
inline constexpr float kTitleY = 16.0f;  // 规格 §三.4：标题上缘距弹窗顶
inline constexpr float kTitleFontSize = 20.0f;  // 规格 S2.1/S2.4：标题 20px

// 违规描述（空串 = 通过）。visible = opacity > 0 且未 disabled。
// clip 子树不下钻：滚动视口等内容被裁剪到容器自身 frame，容器在框内即可。
inline void walkInside(const core::dsl::Element& panel,
                       const core::dsl::Element& e, std::string& violation) {
    for (const auto& child : e.children) {
        if (!violation.empty() || child == nullptr) continue;
        if (child->opacity <= 0.0f || child->disabled) continue;
        const auto& f = child->frame;
        const auto& p = panel.frame;
        if (f.x < p.x - kPxTol || f.y < p.y - kPxTol ||
            f.x + f.width > p.x + p.width + kPxTol ||
            f.y + f.height > p.y + p.height + kPxTol) {
            violation = "元素 " + child->id + " frame(" +
                        std::to_string(f.x) + "," + std::to_string(f.y) + " " +
                        std::to_string(f.width) + "x" + std::to_string(f.height) +
                        ") 超出 panel(" + std::to_string(p.x) + "," +
                        std::to_string(p.y) + " " + std::to_string(p.width) +
                        "x" + std::to_string(p.height) + ")";
            return;
        }
        if (!child->clip) {
            walkInside(panel, *child, violation);
        }
    }
}

// 全部可见子孙在框内：返回首个违规描述（空串 = 通过）。
inline std::string allInside(eui::Ui& ui, const std::string& panelId) {
    const auto* panel = ui.find(panelId);
    if (panel == nullptr) return "panel 未找到: " + panelId;
    if (panel->opacity != 1.0f)
        return "panel 非开态（opacity=" + std::to_string(panel->opacity) + "）";
    std::string violation;
    walkInside(*panel, *panel, violation);
    return violation;
}

// 标题锚（y=16 / fontSize=20）：返回违规描述（空串 = 通过）。
inline std::string titleAnchor(eui::Ui& ui, const std::string& panelId,
                               const std::string& titleId) {
    const auto* panel = ui.find(panelId);
    if (panel == nullptr) return "panel 未找到: " + panelId;
    const auto* title = ui.find(titleId);
    if (title == nullptr) return "title 未找到: " + titleId;
    const float dy = title->frame.y - panel->frame.y;
    if (std::fabs(dy - kTitleY) > kPxTol)
        return titleId + " 标题 y 偏移应为 16，实测 " + std::to_string(dy);
    if (std::fabs(title->fontSize - kTitleFontSize) > 0.01f)
        return titleId + " 标题字号应为 20，实测 " +
               std::to_string(title->fontSize);
    return {};
}

}  // namespace dialog_frame
