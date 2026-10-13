// src/module_claude_proxy/claude_ui_ccswitch.h
// 从 cc-switch 导入 Profile 弹窗：绘制层（compose 每帧、纯绘制 + 回调写状态）+ 导入编排。
// 入口：openCcSwitchImportDialog（读库 + 映射 + 全选；读库失败也开弹窗画错误态）；
// confirmCcSwitchImport（逐条 createProfile + 报告 + 标脏）；drawCcSwitchImportDialog（每帧绘制）。
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

// 跑导入：逐条 createProfile（含凭据加密）+ 同名跳过报告 + 失败收集；跑完写
// st.resultLines_ 与三个计数并标脏。toast 无黄色档：有失败 → isError=true。
void confirmCcSwitchImport(CcSwitchImportDialogState& st,
                           const std::vector<Profile>& existingProfiles,
                           const CcSwitchImportDialogHooks& hooks);

// 弹窗绘制（DialogFrame 范式）。复选框手绘（不引 components::checkbox：其头文件
// 在 -Werror 下有 unused-variable 告警）；列表条目 = name + base_url + model，密钥一律不显示。
void drawCcSwitchImportDialog(eui::Ui& ui, const eui::Screen& screen,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& themeTokens,
                              CcSwitchImportDialogState& st,
                              const CcSwitchImportDialogHooks& hooks);

}  // namespace module::claude