// src/module_claude_proxy/quota/quota_registry.h
// 供应商余量的显式注册表（不用静态自注册：静态库不拉未被引用的 TU，构造函数不跑，注册表会静默缺项）。
#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// 注册表一行 = 一个供应商。
//   id     稳定标识（ProviderConfig::providerId；落库，改了等于换供应商）；
//   create 工厂：返回进程内单例（provider 无状态），调用方只借不还、不得 delete。nullptr 工厂按「该行无效」处理。
struct QuotaProviderEntry {
    const char* id;
    QuotaProvider* (*create)();
};

// 显式表本体（定义在 quota_registry.cpp）。哨兵结尾：id == nullptr 的行 = 表尾；新行插到哨兵之前。
// 行序 = 探测优先级（detectByBaseUrl 返回首个命中行），也是 UI 下拉序。
extern const QuotaProviderEntry kQuotaProviders[];

// 表的物理长度（含表尾哨兵），由定义处就地 std::size 得出（头文件的 extern 数组边界未知，sizeof 编译不过）。
// 结构守卫用它比对「表长」与「哨兵前缀长度」：插行插到哨兵之后会让 provider 永久不可达却照常编译。
std::size_t quotaProviderTableLength();

// 按 id 精确查（整串相等）。返回命中行的单例；空 id / 未命中 / 空表 / 工厂返 nullptr → nullptr。重复 id 取表序首行。
const QuotaProvider* findProvider(const std::string& id);

// 测试 / 复用缝：对任意显式表查找，不碰全局表（注册表语义本身可被逐条断言）。
const QuotaProvider* findProvider(const std::string& id,
                                  std::span<const QuotaProviderEntry> table);

// 按 base_url 自动选中：返回表序首个 matchesBaseUrl() 为真的行；空串 / 无命中 → nullptr（宁可不选中也不猜）。
const QuotaProvider* detectByBaseUrl(const std::string& baseUrl);

const QuotaProvider* detectByBaseUrl(const std::string& baseUrl,
                                     std::span<const QuotaProviderEntry> table);

}  // namespace module::claude::quota
