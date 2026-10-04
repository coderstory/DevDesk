// src/module_claude_proxy/claude_ui_quota.h
// 余量查询弹窗绘制层（逻辑在 quota/quota_dialog_config）：compose 每帧调一次，
// 纯绘制 + 回调写状态。几何集中在 computeQuotaDialogLayout 纯函数，便于无头断言。
#pragma once

#include "claude_ui_state.h"
#include "quota/quota_dialog_config.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <cstddef>
#include <functional>
#include <string>

namespace module::claude {

// 弹窗的回调（compose 每帧装配；自由函数不捕获 this）
struct QuotaDialogHooks {
    // 保存（已由本层 buildQuotaConfigRow 装配好；失败时带中文错误）
    std::function<void(const quota::QuotaConfigRow&)> save;
    // 「测试连接」：入参 = 弹窗当前凭据（不落库的那一份）
    std::function<void(const quota::ProviderConfig&)> testConnection;
    // 用户改了字段值（key + 明文）
    std::function<void(const std::string& key, const std::string& value)> setField;
    // 供应商下拉改选（索引；0 = 不配置）
    std::function<void(int index)> selectProvider;
    // 「从 cc-switch 读取」
    std::function<void()> loadCcSwitch;
    // cc-switch 候选改选（索引；-1 = 未选）
    std::function<void(int index)> selectCcSwitch;
    std::function<void(const std::string& title, const std::string& msg,
                       bool isError)> toast;
};

// 弹窗几何。**纯函数**（无 UI 状态、无绘制）——无头用例直接对它断言，绘制层只消费。
// 布局自上而下 y 累加，动态字段增减只影响其下方；底部按钮对在 h−60。
struct QuotaDialogLayout {
    float height = 0.0f;        ///< 弹窗总高（随字段数变；由本函数算出）
    float providerLabelY = 0.0f;
    float providerDdY = 0.0f;
    float fieldsY = 0.0f;      ///< 第一个字段 label 的 y
    float fieldPitch = 0.0f;   ///< 每个字段占的总高（label+input+gap）
    float ccSwitchY = 0.0f;    ///< 「从 cc-switch 读取」按钮
    float ccNoteY = 0.0f;      ///< cc-switch 状态 / 来源说明行
    float ccDdY = 0.0f;        ///< cc-switch 候选下拉
    float testBtnY = 0.0f;     ///< 「测试连接」按钮
    float resultY = 0.0f;      ///< 测试结果行
    float buttonsY = 0.0f;     ///< 底部按钮对（= height − 60）
    float contentBottom = 0.0f;///< 内容可用底（按钮行上沿 − 8）
};

// 按字段数算几何。**字段数是唯一输入**（cc-switch 是否可读、候选条数都不改
// 布局——候选条数由下拉自身弹层承载，不占行高）。
QuotaDialogLayout computeQuotaDialogLayout(std::size_t fieldCount);

// 弹窗绘制（DialogFrame 范式）。
void drawQuotaConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           quota::QuotaDialogState& st,
                           const QuotaDialogHooks& hooks);

}  // namespace module::claude