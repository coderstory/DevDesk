// src/platform/keychain_macos_scan.h
// macOS 孤儿清理扫描路径的**测试缝**（merge-review R2，2026-09-30）。
//
// 为什么需要（P0 护栏②在事故平台上没有端到端断言面）：
//   shouldDeleteOrphan / deletableOrphanAccount 两个纯决策函数在 keychain.h，
//   三平台共用，纯件用例（test_keychain.cpp:keychain_orphan_sweep_prefix_whitelist）
//   已把语义逐条钉死。但那只钉住**决策函数**，钉不住「macOS 的扫描分支真的
//   调它」——keychain_macos.cpp:237 那行 `if (!shouldDeleteOrphan(...)) continue;`
//   一旦被回退成裸 `keep.count(account) == 0`，纯件用例照样全绿：白名单前缀护栏
//   在 P0 事故发生的那个平台上正是**无人覆盖**的（走真实存储的扫描用例在 macOS
//   全部 SKIP_ON_MACOS_KEYCHAIN——真 login keychain 不可隔离，照跑会写用户钥匙串、
//   按 kSecMatchLimitAll 枚举整库，可能弹授权框）。
//
// 缝的形态（保持接口隔离、零业务影响）：
//   · 只把「枚举」与「删除」两个**边界动作**换成可注入函数指针，其余（service
//     过滤、shouldDeleteOrphan 决策、removed 累积、失败短路）一行不动；
//   · 默认 nullptr = 真实 Security.framework 调用，生产路径零变化；
//   · 只在本文件（keychain_macos.cpp）实现；仅 __APPLE__ 编译，Windows/Linux
//     的构建里连声明都不出现（测试侧同样按 __APPLE__ 门控）。
//
// 用法（测试侧，ScopedMacosScanHooks RAII 同款）：
//   struct FakeScan { ... };   // 假 keychain 命名空间 = (service, account) 对列表
//   platform::keychain::macos_scan::Hooks h{&fakeEnumerate, &countingErase};
//   auto* prev = platform::keychain::macos_scan::setHooksForTest(&h);
//   auto r = platform::keychain::realDeleteSecretsNotIn("Svc", {"claude:1"});
//   platform::keychain::macos_scan::setHooksForTest(prev);   // 复位
//
// 断言口径：这样能端到端钉住 macOS 扫描路径确实走了 shouldDeleteOrphan，且空
// keep 集合**不**退化成通配全删（空 keep 时前缀外条目仍必须保留）——P0 护栏②的
// 两条真实语义，且全程不碰真钥匙串。
#pragma once

#include <core/result.h>

#include <string>
#include <utility>
#include <vector>

namespace platform::keychain::macos_scan {

// 枚举出的一条 (service, account) —— 与 SecItem 读到的 kSecAttrService /
// kSecAttrAccount 属性对一一对应
using ScannedEntry = std::pair<std::string, std::string>;

// 扫描路径的两个边界动作：枚举本 service 命名空间下的条目 / 删除单条
struct Hooks {
    core::Result<std::vector<ScannedEntry>> (*enumerate)(
        const std::string& service);
    core::Result<void> (*erase)(const std::string& service,
                                const std::string& account);
};

// 安装 h（nullptr = 恢复真实 Security.framework 调用），返回被替换的旧钩子
// 供 RAII 复位。仅测试调用（生产代码不碰）。
const Hooks* setHooksForTest(const Hooks* h);

// 当前钩子（nullptr = 走真实 Security.framework）。只读；扫描路径每次取一次，
// 局部存成 const 引用后使用，避免半途被换。
const Hooks* currentHooks();

}  // namespace platform::keychain::macos_scan
