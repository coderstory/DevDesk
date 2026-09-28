// src/module_claude_proxy/claude_ui_detail.h
// R4（P-B3）右栏单元：详情面板三 pane（基础 / 模型映射 / 环境变量）+ 操作
// 按钮组 + 「最近调用」页签（tab 4，2026-09-28 由底部固定预览抽成独立
// 页签，行数随 bodyH 自适应）—— 逐字自 claude_module.cpp 的 compose 搬出。
//
// 「返回内容底沿」契约（spec §3.2）：每个 pane 函数各自返回**本 pane 内容的
// 底沿 y**（绝对屏幕坐标，与入参 y0 同一坐标系）；操作按钮组由
// drawDetailPanel 按该底沿 + 20 定位（`actY = paneBottom + 20`），取代原先
// 手算 y 偏移 —— 与拆分前逐字等价（原 compose 内联版同样是
// `paneBottom` 累加后 `actY = paneBottom + 20.0f`）。
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
    ClaudeService* svc = nullptr;      // 只读 getter（接管态 / 警告行）
    const Profile* shown = nullptr;    // 优先用户选中，其次激活；可为 null
    std::string nameValue;             // 名称（无 profile → "(未启用)"）
    std::string modelValue;            // 模型摘要（modelsSummary）
    std::string urlValue;              // Base URL
    std::string tokenValue;            // API Key 脱敏提示
    const std::vector<ClaudeService::CallLogEntry>* recent = nullptr;  // 最近调用快照
    std::string callsError;            // 最近调用查询错误（空 = 成功）
    std::time_t now = 0;               // 调用时间列「今天/昨天」判定
    float bodyH = 0.0f;                // 环境变量 pane 的行数上限判据
    // 批 2：共享配置片段快照（基础 pane 的「共享配置：片段 A（N 个键）」行）
    const std::vector<SharedConfig>* sharedConfigs = nullptr;
};

// 右栏操作按钮组的回调（compose 每帧装配）
struct DetailActions {
    // 未选中 Profile 时的统一红字提示（标题 + 说明；各键标题不同）
    std::function<void(const std::string& title, const std::string& msg)> warn;
    std::function<void(int64_t id)> enable;                          // 启用
    std::function<void(const Profile& p)> test;                      // 测试连通
    std::function<void(const Profile& p)> edit;                      // 编辑
    std::function<void(int64_t id, const std::string& name)> requestDelete;  // 删除（确认弹窗）
    std::function<void(int64_t id)> duplicate;                       // 复制
    std::function<void(int64_t id)> run;                             // 运行
    std::function<void()> openCalls;                                 // 「查看全部 →」
};

// ---- 三个 pane：各自返回内容底沿（见文件头契约）----
// id: claude.field.* / claude.slot.*
float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v);
float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v);
float drawDetailEnv(eui::Ui& ui, float x0, float y0, float w,
                    const core::theme::Tokens& t, const DetailView& v);

// 整个右栏（底板 column + overlay stack）：标题 + tabs 四页（基础信息 /
// 模型映射 / 环境变量 / 最近调用）+ pane 分派 + 操作按钮组。xOffset =
// 右栏在 claude.body row 内的 x 起点（= 左栏宽 + gap）。
void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a);

} // namespace module::claude
