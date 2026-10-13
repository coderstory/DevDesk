// src/module_claude_proxy/quota/quota_dialog_config.h
// 余量查询弹窗的配置逻辑层（不含任何 EUI 绘制）。
// 弹窗按 fields() 动态渲染供应商专属字段，本层管「哪些字段 / 值从哪来 / 存成什么行」。
// 不变量：① 供应商选项不写死，由 quotaProviderOptions() 从注册表取出，索引 0 恒「不配置」；
//   ② cc-switch 优先且与手填不共存：ccswitchProviderId 非空时 extra/secrets 一律清空；③ secret 展示一律走 maskSecret()，没有第二份真值源。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ccswitch_source.h"
#include "quota_error_text.h"   // quotaErrorHeadline（文案表单点，三方共用）
#include "quota_provider.h"
#include "quota_store.h"

namespace module::claude::quota {

// 下拉的一项。id 恒为注册表 QuotaProviderEntry::id（空串 = 「不配置」）。
struct QuotaProviderOption {
    std::string id;
    std::string displayName;
};

// 下拉全部选项：索引 0 恒为「不配置」，其后按注册表表序。每项都用 findProvider 校验过（滤掉工厂返回 nullptr 的行）。
std::vector<QuotaProviderOption> quotaProviderOptions();

// 由 providerId 反查下拉索引；未命中（含空串之外的未知 id）→ 0（「不配置」）。
int quotaProviderOptionIndex(const std::string& providerId);

// 一个动态字段在弹窗里的呈现。`secret` 决定绘制层用不用密码框 + 脱敏回显。
struct QuotaDialogField {
    ProviderField spec;      // key / label / secret（key 与 ProviderConfig::fields 同名）
    std::string value;       // 明文，只在内存
    std::string sourceLabel; ///< 「来自 cc-switch」/「手动填写」
    bool fromCcSwitch = false;
};

// 弹窗的全部状态。绘制层只读它 + 回调写它；保存 / 测试连接走本层函数。
struct QuotaDialogState {
    bool open = false;
    int64_t profileId = 0;
    std::string profileBaseUrl;   ///< 该 profile 的 base_url（cc-switch 自动匹配用）
    std::string profileApiKey;    ///< 明文，仅「测试连接」与「沿用 profile 凭据」用

    int providerIdx = 0;          ///< 下拉索引（0 = 不配置）
    bool providerDdOpen = false;  ///< 供应商下拉展开态
    std::vector<QuotaDialogField> fields;  ///< 按选中 provider 的 fields() 动态生成

    // cc-switch
    bool ccSwitchLoaded = false;      ///< 已尝试读取（避免每帧重读真库）
    CcSwitchProviderList ccSwitch;    ///< 读取结果（含 status / unavailableReason）
    int ccSwitchIdx = -1;             ///< 选中的候选下标（-1 = 未选）
    bool ccSwitchDdOpen = false;      ///< 候选下拉展开态

    // 测试连接
    bool testing = false;             ///< 后台查询在途
    std::string testMessage;         ///< 结果文案（成功/失败都写，空 = 还没测过）
    bool testIsError = false;         ///< testMessage 是否按错误色渲染
    QuotaError testErrorKind = QuotaError::None;

    // 保存
    std::string error;               ///< 校验失败红字
    bool savedWithProvider = false;  ///< 上次保存是否带了 providerId
};

// 打开弹窗：按 base_url 探测默认供应商（detectByBaseUrl），无命中 → 「不配置」；existing 非空则预填，nullptr = 从未配置。
// 默认选中不猜：无命中就是「不配置」，猜错等于把别人的 quota 查到当前 profile 上。
void openQuotaDialog(QuotaDialogState& st, int64_t profileId,
                     const std::string& baseUrl, const std::string& apiKey,
                     const QuotaConfigRow* existing);

// 按当前 providerIdx 重算 fields：清旧、按新 provider 的 fields() 重建（同 key 旧值沿用）。providerIdx == 0 时清空。
void syncQuotaDialogFields(QuotaDialogState& st);

// 读 cc-switch 并按 base_url 自动匹配候选（不猜）。读不到时 ccSwitch.status != Ok 且候选为空，绘制层据 status 提示「可手动填写」，不崩。
void loadQuotaCcSwitch(QuotaDialogState& st);

// 用户改选候选：把该候选字段灌进 fields 并标「来自 cc-switch」；idx 越界 / -1 → 清空来源标注（回到手填）。
void selectQuotaCcSwitch(QuotaDialogState& st, int idx);

// 由弹窗状态装配一行配置（不变量 2 执行点：ccswitchProviderId 非空时 extra/secrets 一律清空）。返回失败 → error 已写好。
core::Result<QuotaConfigRow> buildQuotaConfigRow(const QuotaDialogState& st,
                                                 std::string* error);

// 构造「测试连接」用的 ProviderConfig（字段取自弹窗当前值，不落库）。cc-switch 候选选中时其 AK/SK 覆盖同名字段——测的就是「保存后会发生什么」。
ProviderConfig buildQuotaTestConfig(const QuotaDialogState& st);

// secret 字段脱敏回显：空 → 空；≤6 字符 → 全 •；否则「前 4 + •••• + 尾 2」。比 profile 密钥（前 6 + *** + 尾 4）更保守——AK/SK 有效熵在尾部。
std::string maskSecret(const std::string& plaintext);

// 「测试连接」的完整结果文案：成功给一行摘要，失败给 headline + 供应商原文。
std::string quotaTestMessage(const QuotaResult& result);

}  // namespace module::claude::quota
