// src/module_claude_proxy/claude_ui_ccswitch.h
// 从 cc-switch 导入 Profile 弹窗的**绘制层 + 导入编排**（2026-10-03）
//
// 分层依据同 claude_ui_quota / claude_ui_logs：compose 每帧调一次、纯绘制 +
// 回调写状态，不做业务判断的「绘制」部分在这里；导入编排（读库 → 映射 →
// createProfile → 结果报告）也在本文件，因为它要持有 ClaudeService 句柄与
// 现有 profile 名单 —— 这两样都从模块 compose 装配进来，自由函数捕获 this
// 的禁令不适用（见 R4 纪律：状态显式入参、回调显式绑）。
//
// 入口三件（模块 compose 装配时调用）：
//   openCcSwitchImportDialog —— 用户点「从 cc-switch 导入」按钮：读库 + 映射 +
//                               全选，弹窗 open=true。读库失败也开弹窗
//                               （画错误态，不让用户对着无反应的按钮发愣）。
//   confirmCcSwitchImport    —— 用户点「导入选中」：逐条 createProfile（含
//                               凭据加密）+ 同名跳过报告 + 失败收集；写完
//                               落结果汇总（resultLines_），标脏刷新列表。
//   drawCcSwitchImportDialog —— 每帧绘制弹窗（DialogFrame 范式，与既有 13
//                               处同款）。
#pragma once

#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>
#include <vector>

namespace module::claude {

// 弹窗的回调（compose 每帧装配；自由函数不捕获 this）
struct CcSwitchImportDialogHooks {
    // 导入完成后标脏刷新列表（与表单保存同款 markDataDirty）
    std::function<void()> markDataDirty;
    // toast 顶栏反馈（导入结束的 headline：成功 N / 跳过 M / 失败 K）
    std::function<void(const std::string& title, const std::string& msg,
                       bool isError)> toast;
};

// 打开弹窗：读库 + 映射 + 全选。读库失败也开弹窗（画错误态）。
void openCcSwitchImportDialog(CcSwitchImportDialogState& st);

// 跑导入：逐条 createProfile（含凭据加密）+ 同名跳过报告 + 失败收集。
// existingProfiles 用来做同名冲突判定（映射层 existingNames 入参）。
// 跑完写 st.resultLines_ 与三个计数；成功 ≥1 → markDataDirty + toast 绿；
// 全失败 → toast 红；有跳过 → toast 黄（不阻断）。
void confirmCcSwitchImport(CcSwitchImportDialogState& st,
                           const std::vector<Profile>& existingProfiles,
                           const CcSwitchImportDialogHooks& hooks);

// 弹窗绘制（DialogFrame 范式）。复选框用 components::checkbox（与既有 agent
// 表单同款），列表条目 = name + base_url + model（**密钥一律不显示**）。
void drawCcSwitchImportDialog(eui::Ui& ui, const eui::Screen& screen,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& themeTokens,
                              CcSwitchImportDialogState& st,
                              const CcSwitchImportDialogHooks& hooks);

}  // namespace module::claude