// src/ui_kit/toast.h
// presentation 能力层（任务 D / R1）：跨模块复制的「toast 状态位 + 绘制接线」。
//
// 分层依据（spec 2026-09-27-refactor-incremental-design §4.1）：
//   - 不放 core：toast 要调 EUI-NEO 组件（components::toast 是需要 eui_neo
//     实体符号的运行时能力），core 的边界只允许 header-only 类型桥接；
//   - 不放任一 module_*：module_* 之间不可互相依赖（红线 R2）；
//   - 故新增本层：被 module_* 与 app 层（app_sidebar）链接，core / platform
//     不得反向依赖本层。
//
// 收编对象（D1）：claude / markdown / todo / app_sidebar 四处逐字重复的
// toast 绘制接线（原先各自 15 行 builder 链，只有元素 id 与少数绘制参数不同）。
#pragma once

#include <components/theme.h>
#include <eui/dsl.h>

#include <string>
#include <utility>

namespace ui_kit {

// toast 状态位。EUI-NEO toast 是单实例（无队列），「单实例节流」语义由这些位
// 承载——消费方仍按「当前没有 toast 展示时才弹」判断（如 markdown 保存失败
// 风暴的节流），与拆分前各模块的 toastOpen_/toastTitle_/toastMessage_/
// toastError_ 一一对应。
struct ToastState {
    bool open = false;
    std::string title;
    std::string body;
    bool isError = false;

    // 等价于各模块原先的 showToast(title, msg, isError) / setStatus(msg, isError)。
    void show(std::string title, std::string body, bool isError = false) {
        this->title = std::move(title);
        this->body = std::move(body);
        this->isError = isError;
        open = true;
    }

    // 等价于原先 onAutoDismiss / onDismiss 回调里的 toastOpen_ = false。
    void dismiss() { open = false; }
};

// 各站点现状的绘制差异项（R6：迁移不得改动任何站点的时长与图标）。默认值取
// 多数站点取值，少数派在调用点显式覆盖。
//
// 迁入时的逐字现状（R1）：
//   claude.toast   3.5s + fa-circle-check(0xF058) + fa-triangle-exclamation(0xF071)
//   md.toast       3.5s + fa-check(0xF00C) + 0xF071（冲突态另有正文字样，见模块内）
//   todo.toast     3.5s + fa-check(0xF00C) + 0xF071（不设正文，只有标题）
//   app.notice     4.5s + fa-circle-info(0xF05A)（无失败态）
struct ToastOptions {
    float durationSec = 3.5f;         // 自动消失秒数（0 = 不自动消失）
    unsigned int okIcon = 0xF00C;     // 成功态图标（fa-check）
    unsigned int errorIcon = 0xF071;  // 失败态图标（fa-triangle-exclamation）
};

// 绘制单实例 toast：每帧无条件调用，显示与否由 state.open 控制（与拆分前
// 各模块 drawToast 同款用法）。节点 id 与拆分前逐字一致——形如 `<id>` /
// `<id>.bg` / `<id>.icon` / `<id>.title` / `<id>.message` / `<id>.close` /
// `<id>.timer`，单测按这些 id 定位，不得改。
//
// state 取非 const 引用：绘制的同时注册「自动消失 / 右上 ✕ 关闭」回写状态位的
// 回调（各模块现状即如此）。
// 正文无条件以 state.body 覆盖组件自带默认值（**含空串**）：EUI-NEO toast 的
// 默认正文是英文占位串 "Short status message for non-blocking feedback."
//（components/toast.h 的 message_ 成员初值），只在调用方不调 .message() 时
// 落回；空正文站点（todo 的 setStatus 只有标题）必须传空串把占位串顶掉，
// 否则会把它当正文画出来（2026-09-29 真机截图实锤）。空串 → 空文本节点 →
// 视觉无正文。
// screen = 所在页面的逻辑尺寸（toast 据此贴右下角）；Ui 不暴露 screen，故由
// 调用方透传。
void drawToast(eui::Ui& ui, const eui::Screen& screen, const std::string& id,
               ToastState& state,
               const components::theme::ThemeColorTokens& tokens,
               const ToastOptions& options = {});

} // namespace ui_kit
