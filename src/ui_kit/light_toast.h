// src/ui_kit/light_toast.h
// presentation 能力层：微信式轻提示（spec §4.10）。
// W2d 新增：黑半透明 bg + 白字 + 圆角 6 + 居中 + 1.5s 自动消失。
//
// 既有通知式 toast（toast.h/.cpp）保留不迁移——两种 toast 并存：
//   - 旧 toast：通知式，含 title/body/icon、贴右下角、3.5s+ 自动消失；既有调用不动
//   - 新 light_toast：微信式轻提示，单行 message、居中、1.5s 自动消失；
//     新反馈点（复制成功/保存成功/操作完成）调本 helper。
// 并存依据：spec §4.10「既有通知式 toast 保留不迁移；微信式黑色半透明居中轻提示
// 为 ui_kit 新增小组件。两种 toast 并存，新反馈点用微信式，旧调用不动」。
//
// 色值纪律（D-03 / §8 #11 禁裸 hex）：
//   - 黑半透明 bg 用 eui::Color{0,0,0,0.7} 语义常量（wechat 令牌无黑半透明槽位——
//     wechat.* 全是浅色基底系：pageBg #F7F7F7 / cardBg #FFFFFF 等，无黑半透明档；
//     §4.10 spec 明示「黑色半透明背景」，故用语义常量 + 此注释例外说明）。
//   - 白字用 wechat.onPrimary（#FFFFFF，主色上的前景色——黑底白字同义；onPrimary
//     在 wechat 令牌里就是 #FFFFFF，复用此槽位避免新增裸白色字面量）。
//   - 圆角 t.sizes.radiusWeChat（6px，§3.2 D-12 统一）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <chrono>
#include <string>

namespace ui_kit {

// §4.10 spec 钉 1.5s 自动消失。
constexpr float kLightToastDurationSec = 1.5f;

// §4.10 spec「黑色半透明背景」——0.7 alpha 对齐微信 PC 端 toast 视觉。
// wechat 令牌无对应槽位（见文件头注释），故用语义常量。
constexpr float kLightToastBgAlpha = 0.7f;

// 内部 state：function-local static 单实例（仿 splash.cpp 的 splashState() 模式）。
//
// 不用 ui.state<T>(id+".state")：StateStore.get<T>（dsl.h:270-323）虽然 map 持久，
// 但 stateTypeKey<T>() 用 function template static 局部变量地址（dsl.h:318-321），
// 跨 TU 时依赖链接器 COMDAT fold 保证地址一致——MinGW ld 在 function template
// static 的 fold 上有已知缺陷，测试 TU 与 light_toast.cpp TU 可能拿到不同 state
// entry，导致测试 mock 不生效。function-local static 是 C++ 标准保证持久（单线程
// UI 线程下安全），不依赖链接器行为，更稳。
//
// 单实例语义：§4.10 轻提示居中显示，同屏多个会重叠——后触发覆盖前触发符合
// toast 语义。id 参数保留给 DOM 节点 id（`<id>.bg`/`<id>.text`）。
struct LightToastState {
    bool open = false;
    std::string message;                                // 当前展示的 message
    std::chrono::steady_clock::time_point startTime{};  // open=true 那一刻
};

// §4.10 微信式轻提示：调用方每帧调，函数内部判断是否显示。
//   - 首次调 message=X → open=true, startTime=now，渲染
//   - 后续调 message=X 且 elapsed < 1.5s → 仍渲染（不重新计时）
//   - 后续调 message=Y（不同）→ 重新计时（覆盖前一条，单实例无队列）
//   - elapsed >= 1.5s → open=false，不渲染
// 几何：居中于 (cx, cy)；宽按 message 估算（防 layout 收缩），高 36px。
// 节点 id = `<id>`（stack）/ `<id>.bg`（rect）/ `<id>.text`（text），
// 单测按此定位不得改。
void showLightToast(eui::Ui& ui, const std::string& id, const std::string& message,
                    float cx, float cy, const core::theme::Tokens& t);

// —— 测试 seam（仅测试用，仿 splash.h 的 splashDisabledForTestsFlag 模式）——
// resetLightToastForTests：reset 内部 state（open=false, message="", startTime={}），
//   每个测试用例开始调一次，防止单实例 state 在测试用例间泄漏（场景1/3 都用
//   「已复制」message 会沿用前一个测试的 state）。
// lightToastStateForTests：返回内部 state 引用，测试可改 startTime 来 mock
//   时间流逝（验证 1.5s 自动消失逻辑）。
void resetLightToastForTests();
LightToastState& lightToastStateForTests();

} // namespace ui_kit
