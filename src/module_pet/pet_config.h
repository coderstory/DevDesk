// src/module_pet/pet_config.h
// 桌宠自身持久化配置（2026-09-22-desktop-pet-design.md §4.6）：
//   petsDir()/pet_config.json —— pet 字段从 config.json 迁出（模块隔离：
//   module_pet 不再依赖 module_settings 的 SettingsStore，module_* 互依禁令）
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"
#include "module_pet/pet_skin.h"  // kDefaultSkinName

namespace module::pet {

struct PetConfig {
    std::string skinName = kDefaultSkinName;  // 皮肤目录名（assets/pets/<skinName>）
    int x = -1;                               // 窗口位置（逻辑屏幕坐标；<0 = 未设置，
    int y = -1;                               //  单边独立：只重置未设置的那一边）
    // 【历史，不是当前字段】挂机穿透（clickThrough）功能已于 2026-09-24 移除：
    // 死锁设计——开启后鼠标穿透导致右键也点不中桌宠，只能回设置页关，用户易被
    // 困住。字段自此不再读写；旧 pet_config.json 里残留的 clickThrough 键被 load
    // 静默忽略（本结构只有 skinName/x/y 三个键），下次 save 时随整文件重写消失
};

// 默认配置文件位置（core::paths::petsDir() 下；DEVDESK_PETS_DIR 可隔离）
std::filesystem::path petConfigFile();

// pet_config.json 读写（不抛异常；文件不存在/损坏一律回退默认值）。
// 文件路径由构造注入（生产传 petConfigFile()，测试传临时文件）
class PetConfigStore {
public:
    explicit PetConfigStore(std::filesystem::path file);

    PetConfig load() const;
    core::Result<void> save(const PetConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）
    static PetConfig sanitized(const PetConfig& cfg);

private:
    std::filesystem::path file_;
};

} // namespace module::pet
