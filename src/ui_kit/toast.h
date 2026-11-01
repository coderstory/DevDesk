// src/ui_kit/toast.h
// presentation 能力层：toast 状态位 + 绘制接线。toast 需 EUI-NEO 组件，不放 core；
// module_* 间不可互相依赖，故归本层（core / platform 不得反向依赖）。
#pragma once

#include <components/theme.h>
#include <eui/dsl.h>

#include <string>
#include <utility>

namespace ui_kit {

// 2026-11-01 F-11：反馈四态（与 platform::notification::Level 对齐）。
// 此前 ToastState 只有 bool isError，只能表达成功/失败两态——「信息」「警告」
// 无处安放，反馈图标也只能在两套码点里挑。
enum class ToastLevel { Info, Success, Warning, Error };

// 四态图标槽（2026-11-01 F-09 统一）：此前各站点各挑一个码点（settings/claude
// 用 0xF058、app.notice 用 0xF05A、其余 0xF00C），同一个「成功」在不同页面
// 长得不一样。改为一态一码点，全站一致。
constexpr unsigned int kToastIconInfo = 0xF05A;     // fa-circle-info
constexpr unsigned int kToastIconSuccess = 0xF00C;  // fa-check
constexpr unsigned int kToastIconWarning = 0xF071;  // fa-triangle-exclamation
constexpr unsigned int kToastIconError = 0xF06A;    // fa-circle-xmark

// 全站统一自动消失时长（2026-11-01 F-09：此前 3.5 / 4.5 两档并存）
constexpr float kToastDurationSec = 3.5f;

// 走「轻提示」形态的正文长度上限：短反馈居中轻提示，长反馈留在右下通知式。
constexpr std::size_t kToastLightBodyMaxChars = 24;

// toast 状态位。EUI-NEO toast 是单实例（无队列），「单实例节流」语义由这些位承载——
// 消费方按「当前没有 toast 展示时才弹」判断。
struct ToastState {
    bool open = false;
    std::string title;
    std::string body;
    ToastLevel level = ToastLevel::Success;
    bool isError = false;  // 兼容镜像（level == Error）；旧调用点仍读此字段

    // 等价于 showToast(title, msg, isError) / setStatus(msg, isError)。
    void show(std::string title, std::string body, bool isError = false) {
        show(std::move(title), std::move(body),
             isError ? ToastLevel::Error : ToastLevel::Success);
    }

    void show(std::string title, std::string body, ToastLevel level) {
        this->title = std::move(title);
        this->body = std::move(body);
        this->level = level;
        this->isError = (level == ToastLevel::Error);
        open = true;
    }

    // 等价于 onAutoDismiss / onDismiss 回调里的 toastOpen_ = false。
    void dismiss() { open = false; }
};

// 各站点绘制差异项。此前 okIcon / errorIcon 允许各站点各挑码点，是「同一个成功
// 长得不一样」的根因，已删除：图标改由 ToastLevel 统一决定。
struct ToastOptions {
    float durationSec = kToastDurationSec;  // 自动消失秒数（0 = 不自动消失）
    // 右下贴角的抬升量：把 toast 底沿从 screen.height 上收这么多（逻辑 px），
    // 用于避开页面底部按钮行（默认 0 = 贴底；claude 详情传动作区预留高度）。
    float bottomInset = 0.0f;
};

// 绘制单实例 toast：每帧无条件调用，显隐由 state.open 控制；state 取非 const 引用以回写
// 自动消失 / 关闭回调。节点 id（`<id>` / `<id>.bg` / `.icon` / `.title` /
// `.message` / `.close` / `.timer`）单测按此定位不得改。
// 正文无条件以 state.body 覆盖组件默认值（含空串）——否则 EUI-NEO 的英文占位正文会在
// 无正文站点（如 todo 的 setStatus）被画出来；空串 → 空文本节点 → 视觉无正文。
// screen = 所在页面逻辑尺寸（toast 据此贴右下角），Ui 不暴露 screen 故由调用方透传。
//
// 尺寸（UAT6 内容驱动）：宽 = min(内容宽+边距, 360 上限)、高 = 行数×行高+边距
//（长文本按最大宽换行不跑出框；短文本一行就一行高）；宽下限 180、高下限 58，
// 上限钳制防巨框。签名不变，调用方零改动；字体测量不可用时退组件默认 360×88。
void drawToast(eui::Ui& ui, const eui::Screen& screen, const std::string& id,
               ToastState& state,
               const components::theme::ThemeColorTokens& tokens,
               const ToastOptions& options = {});

} // namespace ui_kit
