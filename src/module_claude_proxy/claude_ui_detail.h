// src/module_claude_proxy/claude_ui_detail.h
// 右栏单元：详情面板四 pane + 操作按钮组 + 「最近调用」页签。
// 契约：每个 pane 函数返回本 pane 内容底沿 y（与 y0 同坐标系）。
// 布局锚定式：贴底元素锚容器底（actY = bodyH − 动作块预留），行数预算用真实裁剪下沿 contentBottom。
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
    // 共享配置片段快照（基础 pane 绑定行 + 生效 env 来源标注）
    const std::vector<SharedConfig>* sharedConfigs = nullptr;
    // 没有 bodyH 字段：行数预算一律用 contentBottom（真实裁剪下沿）。
};

// 生效来源短标签（compose 装配与 pane 共用的单一来源）：三类来源 → 中文标签
const char* envSourceLabel(EnvSource s);

// pane 绘制副产物（诊断日志用）：实际画出的行数 / 可画的总行数。
// 只给「行数预算型」pane（环境变量 / 最近调用）填，把「为什么只画了 N 行」客观化。
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
    std::function<void(int64_t id)> run;                             // 运行
    std::function<void()> openCalls;                                 // 「查看全部 →」
    // 模型映射 pane 的「1M」开关落库：入参 = profile id + 槽位表下标（表序）+ 目标状态。
    // 只有可用行（支持 1M 且 base 非空）会触发（其余置灰不挂 onChange）。
    std::function<void(int64_t profileId, std::size_t slotIndex, bool on)> setOneM;
};

// 四个 pane：各自返回内容底沿（见文件头契约）
// contentBottom = 内容框可用底（drawDetailPanel 由 bodyH 推出）：pane 画出的
// 内容不得越过它（行数预算按它推）。rows 非空时回填行数统计（诊断日志用）。
// id: claude.field.* / claude.slot.* / claude.env.* / claude.calls.*
float drawDetailBasic(eui::Ui& ui, float x0, float y0, float w,
                      const core::theme::Tokens& t, const DetailView& v,
                      float contentBottom);
// a = 动作回调；模型映射 pane 的「1M」开关经 a.setOneM 落库
float drawDetailModels(eui::Ui& ui, float x0, float y0, float w,
                       const core::theme::Tokens& t, const DetailView& v,
                       const DetailActions& a, float contentBottom);
float drawDetailEnv(eui::Ui& ui, float x0, float y0, float w,
                    const core::theme::Tokens& t, const DetailView& v,
                    float contentBottom, PaneRows* rows = nullptr);
// 「最近调用」pane 在 .cpp 的匿名命名空间内（drawDetailCallsPane）
//；其行数统计经 PaneRows 回填同上。

// 整个右栏（底板 column + overlay stack）：标题 + tabs 四页 + pane 分派 + 操作按钮组。
// xOffset = 右栏在 claude.body row 内的 x 起点（= 左栏宽 + gap）。
void drawDetailPanel(eui::Ui& ui, float rightW, float bodyH, float xOffset,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     DetailState& st, const DetailView& v,
                     const DetailActions& a);

} // namespace module::claude
