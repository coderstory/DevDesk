// src/module_claude_proxy/quota/quota_registry.h
// 供应商余量 —— **显式注册表**（G0 抽象层）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G0）
//
// 为什么是显式表，不用静态自注册（本文件存在的全部理由）：
//   静态自注册靠 TU 里的静态对象构造来「顺手登记」。但静态库不拉未被引用的 TU，
//   链接器认为那个 .o 没人用就不进来 → 构造函数不跑 → 注册表**神秘地少一个
//   provider**，且没有任何编译错误、没有任何运行期警告。仓库既有模式就是显式
//   constexpr 表（src/app_registry.h:42 的 kModuleRegistry），这里沿用同一条。
//   加一个供应商 = 在 quota_registry.cpp 的表里加一行 + 在 tests 里加断言，
//   漏加会以「测试红」而不是「上线后才知道」的形式暴露。
#pragma once

#include <span>
#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// 注册表一行 = 一个供应商。
//   id     稳定标识（ProviderConfig::providerId 就是它；落库，改了等于换供应商）
//   create 工厂：返回**进程内单例**（provider 无状态），调用方**只借不还**、
//          不得 delete（findProvider 返回 const 指针，调用方本来也无从释放；
//          每次 new 就是稳定泄漏）。真 provider 在各自 TU 里用函数局部 static
//          持有实例。nullptr 工厂按「该行无效」处理，不崩。
struct QuotaProviderEntry {
    const char* id;
    QuotaProvider* (*create)();
};

// 显式表本体（定义在 quota_registry.cpp）。两条约定：
//   - **哨兵结尾**：`id == nullptr` 的行 = 表尾，查找到此为止。零元素数组
//     （`const T arr[] = {}`）非标准 C++（-Wpedantic -Werror 直接拦），而 G0
//     阶段表里还没有真实 provider，故用哨兵表达「当前为空」。
//     G1-G4 各自把行插到哨兵**之前**。
//   - 行序 = 探测优先级（detectByBaseUrl 返回首个命中行），也是 UI 下拉序。
extern const QuotaProviderEntry kQuotaProviders[];

// 按 id 精确查（整串相等，不做前缀 / 大小写折叠）。
// 返回：命中行的单例；空 id / 未命中 / 空表 / 该行工厂返回 nullptr → nullptr。
// 重复 id（表本身不允许，test_claude_quota 的结构守卫会报）取**表序首行**，
// 保证确定性。
const QuotaProvider* findProvider(const std::string& id);

// 同上的测试 / 复用缝：对**任意**显式表查找，不碰全局表。
// （G0 没有真 provider，全局表查不出东西；有了这条，注册表语义本身可被逐条断言）
const QuotaProvider* findProvider(const std::string& id,
                                  std::span<const QuotaProviderEntry> table);

// 按 base_url 自动选中（G6 默认选中 / 导入 profile 时匹配）。
// 返回表序**首个** matchesBaseUrl() 为真的行；空串 / 无命中 → nullptr
//（宁可不选中让用户手动挑，也不猜——猜错等于把别人的 quota 查到当前 profile 上）。
const QuotaProvider* detectByBaseUrl(const std::string& baseUrl);

const QuotaProvider* detectByBaseUrl(const std::string& baseUrl,
                                     std::span<const QuotaProviderEntry> table);

}  // namespace module::claude::quota
