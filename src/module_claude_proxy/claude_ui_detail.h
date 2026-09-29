// src/module_claude_proxy/claude_ui_detail.h
// R4（P-B3）右栏单元：详情面板四 pane（基础 / 模型映射 / 环境变量 / 高级配置）
// + 操作按钮组 + 「最近调用」页签（tab 4，2026-09-28 由底部固定预览抽成独立
// 页签，行数随 bodyH 自适应；高级配置 pane = 2026-09-30 batch-fix B 新增
// tab 3，最近调用由 3 顺移至 4）—— 逐字自 claude_module.cpp 的 compose 搬出。
//
// 「返回内容底沿」契约（spec §3.2）：每个 pane 函数各自返回**本 pane 内容的
// 底沿 y**（绝对屏幕坐标，与入参 y0 同一坐标系）；操作按钮组由
// drawDetailPanel 按该底沿 + 20 定位（`actY = paneBottom + 20`），取代原先
// 手算 y 偏移 —— 与拆分前逐字等价（原 compose 内联版同样是
// `paneBottom` 累加后 `actY = paneBottom + 20.0f`）。
//
// 2026-09-28（macOS 布局统一方案 §1 组 B，P1）：上面这条「actY 跟随内容底」
// 正是「容器底与内容底不同源」的根因 —— 面板底恒为 bodyH，动作区却跟着数据
// 量跑，数据少时动作区浮在上部、下方 60% 空白，四个页签的分隔线互差 171px。
// 现在改为**锚定式**：
//   统一原则（后续维护照此判）——**容器可用高只有一个来源**（本模块 = 由
//   screen.height 减已知 chrome 得到的 bodyH）；**贴底元素锚容器底**（动作区
//   actY = bodyH − 动作块预留）；**内容视口 = 容器底 − 已知 chrome**
//   （contentBottom = actY − 间距，随 paneBottom 取 min 兜底）；**所有「向下
//   展开 / 是否越界」的判定用真实裁剪下沿**（contentBottom），不用推导值。
// 锚定而非写死绝对 y：Windows 侧 bodyH 与 macOS 差 15~45px，绝对值会错位。
//
// 因此 pane 现在多收一个 `contentBottom`（内容框可用底，由 drawDetailPanel
// 从 bodyH 推出）—— 行数预算必须按它推，不得再用与内容无关的硬上限/硬下限
// （见 claude_ui_detail.cpp 的 P2 注释）。
//
// 零行为变化：节点 id、文案、字号、颜色、几何、控制流逐字保留。
#pragma once

#include "claude_service.h"
#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace module::claude {

// 详情面板的只读输入（compose 每帧装配；指针指向调用方存活对象）
struct DetailView {
    ClaudeService* svc = nullptr;      // 只读 getter（接管态 / 警告行 / 生效 env 端口）
    const Profile* shown = nullptr;    // 优先用户选中，其次激活；可为 null
    std::string nameValue;             // 名称（无 profile → "(未启用)"）
    std::string modelValue;            // 模型摘要（modelsSummary）
    std::string urlValue;              // Base URL
    std::string tokenValue;            // API Key 脱敏提示
    const std::vector<ClaudeService::CallLogEntry>* recent = nullptr;  // 最近调用快照
    std::string callsError;            // 最近调用查询错误（空 = 成功）
    std::time_t now = 0;               // 调用时间列「今天/昨天」判定
    // 批 2：共享配置片段快照（基础 pane 的「共享配置：片段 A（N 个键）」行 +
    // P3 生效 env 的来源标注）
    const std::vector<SharedConfig>* sharedConfigs = nullptr;
    // 高级配置 pane（2026-09-30 batch-fix B，设计 §3.2④）：
    //   dialectText = 生效方言展示串。compose 装配：显式覆盖 → 「名（显式）」；
    //   空 → 「自动（按站点推断）→ 生效名」（推断结果可审计，正是本 pane 的
    //   价值）。显示名统一走 dialectKindDisplayName（与表单下拉单一来源，
    //   不直接用 dialectKindToText 内部值）
    //   fallbackName = 备用 Profile 名。compose 从既有 profiles 快照按
    //   fallbackProfileId 解析（悬空/0 → 「无」/「（已删除）」），与列表快照
    //   同源、pane 内不做服务调用（避免每帧 DB 查询）
    std::string dialectText;
    std::string fallbackName;
    // 注：这里**没有** bodyH 字段（2026-09-29 审计 F-01 删除）。容器可用高只由
    // drawDetailPanel 的 bodyH 形参持有，pane 侧的行数预算一律用传下去的
    // contentBottom —— 曾经的 `DetailView::bodyH` 全仓零读、注释却宣称它是
    // 「行数预算唯一来源」，按它算会得到与真实裁剪下沿差 105px 的另一个量，
    // 正是本次修的病根换个马甲。要算预算就往 contentBottom 走。
};

// pane 绘制副产物（诊断日志用）：实际画出的行数 / 可画的总行数。
// 只给「行数预算型」pane（环境变量 / 最近调用）填 —— 它们是本次修复的核心，
// 日志要能把「为什么只画了 N 行」客观化（方案 §5 组 B 判据 ②）。
struct PaneRows {
    int shown = 0;
    int total = 0;
};

// 右栏操作按钮组的回调（compose 每帧装配）
struct DetailActions {
    // 未选中 Profile 时的统一红字提示（标题 + 说明；各键标题不同）
    std::function<void(const std::string& title, const std::string& msg)> warn;
    std::function<void(int64_t id)> enable;                          // 启用
    std::function<void(const Profile& p)> test;                      // 测试连通
    std::function<void(const Profile& p)> edit;                      // 编辑
    std::function<void(int64_t id, const std::string& name)> requestDelete;  // 删除（确认弹窗）
    std::function<void(int64_t id)> duplicate;                       // 建副本
    // 复制配置（2026-09-30 batch-fix C，设计 §4.2②）：profile → settings.json
    // 形态 JSON → 系统剪贴板（占位不导明文 key）。第 7 键 + 右键菜单双入口
    std::function<void(const Profile& p)> copyConfig;
    std::function<void(int64_t id)> run;                             // 运行
    std::function<void()> openCalls;                                 // 「查看全部 →」
};

// ---- 五个 pane：各自返回内容底沿（见文件头契约）----
// contentBottom = 内容框可用底（drawDetailPanel 由 bodyH 推出）：pane 画出的
// 内容不得越过它（行数预算按它推）。rows 非空时回填行数统计（诊断日志用）。
// id: claude.field.* / claude.slot.* / claude.env.* / claude.calls.*
float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v,
                      float contentBottom);
float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v,
                       float contentBottom);
float drawDetailEnv(eui::Ui& ui, float x0, float y0, float w,
                    const core::theme::Tokens& t, const DetailView& v,
                    float contentBottom, PaneRows* rows = nullptr);
// 「最近调用」pane 与「高级配置」pane（2026-09-30 batch-fix B）在 .cpp 的
// 匿名命名空间内（drawDetailCallsPane / drawDetailAdvanced）；
// 其行数统计经 PaneRows 回填同上。

// 整个右栏（底板 column + overlay stack）：标题 + tabs 五页（基础信息 /
// 模型映射 / 环境变量 / 高级配置 / 最近调用）+ pane 分派 + 操作按钮组。
// xOffset = 右栏在 claude.body row 内的 x 起点（= 左栏宽 + gap）。
void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a);

} // namespace module::claude
