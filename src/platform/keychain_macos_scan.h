// src/platform/keychain_macos_scan.h
// macOS 孤儿清理扫描路径的测试缝：真实存储用例在 macOS 不可隔离（会写用户钥匙串、可能弹授权框），
// 需通过缝断言「扫描确实走了 shouldDeleteOrphan，且空 keep 集合不退化成通配全删」。
// 缝形态：只把「枚举」「删除」两个边界动作换成可注入函数指针，其余逻辑不动；默认 nullptr = 真实调用；仅 __APPLE__ 编译。

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
