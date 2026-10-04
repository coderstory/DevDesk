// src/platform/keychain_macos_scan.h
// macOS 孤儿清理扫描路径的测试缝。
// 为什么需要：决策函数已被纯件用例钉死，但钉不住「macOS 的扫描分支真的调它」——那行一旦回退成裸
// keep.count(account)==0，纯件用例照样全绿。走真实存储的扫描用例在 macOS 全部 SKIP（真 login
// keychain 不可隔离，照跑会写用户钥匙串、枚举整库、可能弹授权框）→ 白名单护栏在事故平台正无人覆盖。
// 缝形态：只把「枚举」「删除」两个边界动作换成可注入函数指针，其余（service 过滤、决策、removed 累积、
// 失败短路）一行不动；默认 nullptr = 真实调用，生产零变化；仅 __APPLE__ 编译。
// 断言：端到端钉住扫描确实走了 shouldDeleteOrphan，且空 keep 集合**不**退化成通配全删。

#pragma once

#include <core/result.h>

#include <string>
#include <utility>
#include <vector>

namespace platform::keychain::macos_scan {

// 枚举出的一条 (service, account)，对应 SecItem 的 kSecAttrService/kSecAttrAccount 属性对
using ScannedEntry = std::pair<std::string, std::string>;

// 扫描路径的两个边界动作：枚举本 service 命名空间下的条目 / 删除单条
struct Hooks {
    core::Result<std::vector<ScannedEntry>> (*enumerate)(
        const std::string& service);
    core::Result<void> (*erase)(const std::string& service,
                                const std::string& account);
};

// 安装 h（nullptr = 恢复真实 Security.framework），返回旧钩子供 RAII 复位。仅测试调用。
const Hooks* setHooksForTest(const Hooks* h);

// 当前钩子（nullptr = 走真实 Security.framework）。只读；扫描路径每次取一次存成局部引用，避免半途被换。
const Hooks* currentHooks();

}  // namespace platform::keychain::macos_scan
