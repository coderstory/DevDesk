// src/module_claude_proxy/quota/quota_dialog_config.h
// G6：**余量查询弹窗的配置逻辑层**（不含任何 EUI 绘制）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G6）
//
// 为什么把逻辑从绘制里剥出来：弹窗要按 `fields()` **动态**渲染供应商专属字段
// （火山两个、其余三家零个），而「哪些字段 / 每个字段的值从哪来 / 保存成什么行」
// 是**纯数据问题**，与像素无关。剥出来之后：
//   - 单测能直接构造状态断言**具体 key 名**（不经过 EUI 元素树）；
//   - 绘制层退化成「按本层算出的字段表逐行画输入框」，不重复决策；
//   - cc-switch 读不到 / provider 未注册的退化路径集中在这里，绘制层不猜。
//
// 三条本层自带的不变量（下游 UI 与装配层都按这个读）：
//
//   1. **供应商选项不写死字符串**：下拉项由 `quotaProviderOptions()` 从注册表
//      逐行取出（`findProvider` 校验命中），顺序 = 表序（= 探测优先级）。
//      索引 0 恒为「不配置」（providerId 空串），其余 1..N 与注册表同序。
//      加一个 provider = 注册表加一行，弹窗自动多一项，**零处改动**。
//
//   2. **cc-switch 优先，手填兜底，且二者不共存**：`ccswitchProviderId` 非空
//      时 `extra` / `secrets` 一律清空（见 quota_store.h 的说明）——这一条由
//      `buildQuotaConfigRow` **代码保证**，不靠绘制层自觉。
//
//   3. **secret 字段的展示值一律脱敏**：脱敏只有 `maskSecret()` 一个入口
//      （「前 4 + •••• + 尾 2」）。绘制层对 secret 字段**恒用空输入框 +
//      独立的脱敏回显节点**，明文与脱敏串都不进 EUI 的 InputState ——
//      结构上漏不出去，不是注释约定。
//
//      ⚠ 这里**没有** `displayValue` 成员：曾经有一个「已经算好的脱敏串」字段，
//      于是脱敏口径有两个来源（算好的字段 vs 现调 maskSecret），改一处漏一处
//      就是 F1 那类「两份真值源」。现算现用，只留一个函数。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ccswitch_source.h"
#include "quota_error_text.h"   // quotaErrorHeadline（文案表单点，G7 起三方共用）
#include "quota_provider.h"
#include "quota_store.h"

namespace module::claude::quota {

// ============ 供应商下拉选项（不变量 1）============

// 下拉的一项。id 恒为注册表 `QuotaProviderEntry::id`（空串 = 「不配置」）。
struct QuotaProviderOption {
    std::string id;
    std::string displayName;
};

// 下拉全部选项：**索引 0 恒为「不配置」**，其后按注册表表序。
// 每一项都用 `findProvider(id)` 校验过——注册表里写了行但工厂返回 nullptr
// 的情况在这一层就被滤掉（不滤的话下拉里会出现一个点了必然报错的选项）。
std::vector<QuotaProviderOption> quotaProviderOptions();

// 由 providerId 反查下拉索引；未命中（含空串之外的未知 id）→ 0（「不配置」）。
int quotaProviderOptionIndex(const std::string& providerId);

// ============ 弹窗状态 ============

// 一个动态字段在弹窗里的呈现。`secret` 决定绘制层用不用密码框 + 脱敏回显。
struct QuotaDialogField {
    ProviderField spec;      // key / label / secret（key 与 ProviderConfig::fields 同名）
    std::string value;       // **明文**，只在内存
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

    // ---- cc-switch ----
    bool ccSwitchLoaded = false;      ///< 已尝试读取（避免每帧重读真库）
    CcSwitchProviderList ccSwitch;    ///< 读取结果（含 status / unavailableReason）
    int ccSwitchIdx = -1;             ///< 选中的候选下标（-1 = 未选）
    bool ccSwitchDdOpen = false;      ///< 候选下拉展开态

    // ---- 测试连接 ----
    bool testing = false;             ///< 后台查询在途
    std::string testMessage;         ///< 结果文案（成功/失败都写，空 = 还没测过）
    bool testIsError = false;         ///< testMessage 是否按错误色渲染
    QuotaError testErrorKind = QuotaError::None;

    // ---- 保存 ----
    std::string error;               ///< 校验失败红字
    bool savedWithProvider = false;  ///< 上次保存是否带了 providerId（G7 消费）
};

// ============ 生命周期 ============

// 打开弹窗：按 base_url 探测默认供应商（`detectByBaseUrl`），无命中 → 「不配置」。
// 已保存过的配置经 `existing` 预填（providerIdx / ccswitchProviderId /
// 手填字段值）；`existing` 传 nullptr = 该 profile 从未配置。
//
// ⚠ **默认选中不猜**：探测无命中就是「不配置」，让用户手动挑。猜错等于把
// 别人的 quota 查到当前 profile 上（quota_registry.h 同款裁定）。
void openQuotaDialog(QuotaDialogState& st, int64_t profileId,
                     const std::string& baseUrl, const std::string& apiKey,
                     const QuotaConfigRow* existing);

// 按当前 providerIdx 重算 `fields`：清掉旧字段、按新 provider 的 fields()
// 逐个重建（值尽量沿用同 key 的旧值——切回切去不丢用户填的东西）。
// **providerIdx == 0（不配置）时清空字段表**（不配供应商就没有字段）。
void syncQuotaDialogFields(QuotaDialogState& st);

// ============ cc-switch ============

// 读 cc-switch（`listCcSwitchProviders`）并按 base_url 自动匹配候选
// （`matchCcSwitchProviderByBaseUrl`）。命中 → 选中并把字段填好；
// 未命中 → 只列出候选让用户挑（**不猜**）。
// **读不到（库不存在 / 打不开）时 `ccSwitch.status != Ok` 且候选为空**，
// 绘制层据 status 显示「未找到 cc-switch 配置，可手动填写」，不崩、不静默。
void loadQuotaCcSwitch(QuotaDialogState& st);

// 用户改选候选（`ccSwitchIdx`）：把该候选的字段灌进 `fields` 并标「来自
// cc-switch」。idx 越界 / -1 → 清空 cc-switch 来源标注（回到手填）。
void selectQuotaCcSwitch(QuotaDialogState& st, int idx);

// ============ 保存 ============

// 由弹窗状态装配一行配置。**不变量 2 的执行点**：`ccswitchProviderId` 非空时
// extra / secrets 一律清空。返回失败 → `error` 已写好（供绘制层显示红字）。
core::Result<QuotaConfigRow> buildQuotaConfigRow(const QuotaDialogState& st,
                                                 std::string* error);

// ============ 测试连接 ============

// 构造「测试连接」用的 ProviderConfig：字段取自弹窗当前值（**不落库**，
// 用户还没点保存也能测）。cc-switch 候选选中时该候选的 AK/SK 覆盖同名字段
// ——测的就是「保存后会发生什么」。
ProviderConfig buildQuotaTestConfig(const QuotaDialogState& st);

// ============ 展示 ============

// secret 字段的脱敏回显：空 → 空；≤6 字符 → 全 •；否则「前 4 + •••• + 尾 2」。
// 与 profile 密钥的脱敏口径（profile_repository.cpp 的 makeApiKeyHint，
// 前 6 + *** + 尾 4）**刻意不同**：这里更保守——AK/SK 的有效熵在尾部，
// 露尾 4 等于把签名密钥的尾巴摆出来。
std::string maskSecret(const std::string& plaintext);

// ⚠ `quotaErrorHeadline` 的声明**已搬到 quota_error_text.h**（G7：这张文案表
// 有了弹窗之外的第二、第三个消费者——首页错误行 + 落库兜底，留在弹窗层会让
// store / home 反向依赖「弹窗配置」头）。本头照旧 include 它，既有调用点
// （quota_dialog_config.cpp / claude_ui_quota.cpp / 用例）零改动。

// 「测试连接」的完整结果文案：成功给一行摘要，失败给 headline + 供应商原文。
std::string quotaTestMessage(const QuotaResult& result);

}  // namespace module::claude::quota