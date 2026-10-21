// src/ui_kit/toast.h
// presentation 能力层：toast 状态位 + 绘制接线。toast 需 EUI-NEO 组件，不放 core；
// module_* 间不可互相依赖，故归本层（core / platform 不得反向依赖）。
#pragma once

#include <components/theme.h>
#include <eui/dsl.h>

#include <string>
#include <utility>

namespace ui_kit {

// toast 状态位。EUI-NEO toast 是单实例（无队列），「单实例节流」语义由这些位承载——
// 消费方按「当前没有 toast 展示时才弹」判断。
struct ToastState {
    bool open = false;
    std::string title;
    std::string body;
    bool isError = false;

    // 等价于 showToast(title, msg, isError) / setStatus(msg, isError)。
    void show(std::string title, std::string body, bool isError = false) {
        this->title = std::move(title);
        this->body = std::move(body);
        this->isError = isError;
        open = true;
    }

    // 等价于 onAutoDismiss / onDismiss 回调里的 toastOpen_ = false。
    void dismiss() { open = false; }
};

// 各站点绘制差异项（不得改动任何站点的时长与图标）；默认值取多数站点取值，
// 少数派在调用点显式覆盖（claude 3.5s/0xF058；md·todo 3.5s/0xF00C；app.notice 4.5s/0xF05A）。
struct ToastOptions {
    float durationSec = 3.5f;         // 自动消失秒数（0 = 不自动消失）
    unsigned int okIcon = 0xF00C;     // 成功态图标（fa-check）
    unsigned int errorIcon = 0xF071;  // 失败态图标（fa-triangle-exclamation）
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
