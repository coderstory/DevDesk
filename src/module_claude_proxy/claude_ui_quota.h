// src/module_claude_proxy/claude_ui_quota.h
// G6：余量查询弹窗的**绘制层**（逻辑在 quota/quota_dialog_config.{h,cpp}）
//
// 分层依据同 claude_ui_form / claude_ui_logs：compose 每帧调一次、纯绘制 +
// 回调写状态，不做业务判断。**本文件零决策**：字段表 / 候选表 / 保存行全部
// 由 quota_dialog_config 算好送进来；这里只负责把它们画成行。
//
// 为什么几何算在别处（computeQuotaDialogLayout 单列）：弹窗要按 provider 的
// fields() **动态**增减行（火山 2 行、其余三家 0 行），下方 cc-switch 区块、
// 测试结果行、按钮行都得跟着挪。几何若散在本文件里逐处 `y += ...`，改一处
// 漏一处就是「动态字段增减后元信息行错位」（简报点名要 probe 覆盖的那类）。
// 几何集中成一个**纯函数**后，无头用例能直接对它断言，绘制层只是消费者。
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

// 弹窗的回调（compose 每帧装配；自由函数不捕获 this，见 R4 纪律）
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

// 弹窗几何。**纯函数**（无 UI 状态、无绘制）——无头/probe 用例直接对它断言，
// 绘制层只消费结果，两者不可能漂移。
//
// 布局（自上而下，全部自 y 起累加，**动态字段增减只影响其下方**）：
//   0   标题
//   1   说明行（两行）
//   2   供应商类型 label + 下拉
//   3.. 动态字段（每项 = label 16 + input 40 + 12 gap）
//   ..  cc-switch 区块（读取按钮 + 状态行 + 候选下拉）
//   ..  「测试连接」按钮 + 结果行
//   ..  底部按钮对（h−60，与既有 13 处弹窗同款）
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

// 弹窗绘制（DialogFrame 范式，与既有 13 处同款）。
void drawQuotaConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           quota::QuotaDialogState& st,
                           const QuotaDialogHooks& hooks);

}  // namespace module::claude