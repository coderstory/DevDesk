// src/ui_kit/light_toast.h
// presentation 能力层：微信式轻提示（spec §4.10）。
// W2d 新增：居中轻提示 + 1.5s 自动消失；need328（2026-11-18 老板验收回调）
// 配色改版：绿半透底 + 墨绿描边 + 墨绿字（原黑半透底 + 白字）。
//
// 既有通知式 toast（toast.h/.cpp）保留不迁移——两种 toast 并存：
//   - 旧 toast：通知式，含 title/body/icon、贴右下角、3.5s+ 自动消失；既有调用不动
//   - 新 light_toast：微信式轻提示，单行 message、居中、1.5s 自动消失；
//     新反馈点（复制成功/保存成功/操作完成）调本 helper。
// 并存依据：spec §4.10「既有通知式 toast 保留不迁移；微信式居中轻提示
// 为 ui_kit 新增小组件。两种 toast 并存，新反馈点用微信式，旧调用不动」。
//
// 色值纪律（D-03 / §8 #11 禁裸 hex，need328 两档口径）：
//   - 填充 = theme success 令牌压半透 alpha（深色档 #34D399 / 浅色档 #10B981，
//     alpha 0.88）：成功反馈语义绿，恒绿不随 accent 漂移——primary/onAccent 是
//     accent 依赖槽位（非微信绿 accent 下 primary 失绿、onAccent 漂白），禁用于
//     本组件。
//   - 描边/文字 = 墨绿语义常量 #06251A（值同 onAccent 深墨档，但不借该 accent
//     依赖槽位，理由见上）：两档绿底上均 ≥6:1 ≥ WCAG AA 4.5:1；绿底白字仅
//     ~1.9:1 不达标（need261「绿底深墨不用白字」同裁决）。
//   - 圆角 t.sizes.radiusWeChat（6px，§3.2 D-12 统一）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <chrono>
#include <string>

namespace ui_kit {

// §4.10 spec 钉 1.5s 自动消失。
constexpr float kLightToastDurationSec = 1.5f;

// need328 绿半透档：success 令牌压 0.88 alpha（底层背景 12% 透出）。
constexpr float kLightToastGreenAlpha = 0.88f;

// need328 墨绿描边宽 + 描边/文字色 #06251A（值同 onAccent 深墨档；不借该
// accent 依赖槽位，见文件头色值纪律）。
constexpr float kLightToastBorderWidth = 1.0f;
constexpr eui::Color kLightToastInk{6.0f / 255.0f, 37.0f / 255.0f,
                                    26.0f / 255.0f, 1.0f};

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

// 纯绘制（无状态机）：绿半透底 + 墨绿描边 + 墨绿字 + 圆角，居中于 (cx, cy)。
// 两处复用：showLightToast（自带 1.5s 状态机）与 ui_kit::drawToast 的轻反馈分支
// （2026-11-01 R5：无标题的短反馈统一走居中轻提示，不再各自贴右下角）。
// 节点 id = `<id>`（stack）/ `<id>.bg`（rect）/ `<id>.text`（text）。
void drawLightToastPill(eui::Ui& ui, const std::string& id,
                        const std::string& message, float cx, float cy,
                        const core::theme::Tokens& t);

// —— 测试 seam（仅测试用，仿 splash.h 的 splashDisabledForTestsFlag 模式）——
// resetLightToastForTests：reset 内部 state（open=false, message="", startTime={}），
//   每个测试用例开始调一次，防止单实例 state 在测试用例间泄漏（场景1/3 都用
//   「已复制」message 会沿用前一个测试的 state）。
// lightToastStateForTests：返回内部 state 引用，测试可改 startTime 来 mock
//   时间流逝（验证 1.5s 自动消失逻辑）。
void resetLightToastForTests();
LightToastState& lightToastStateForTests();

} // namespace ui_kit
