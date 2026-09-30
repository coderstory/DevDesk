// src/module_pet/pet_config.h
// 桌宠自身持久化配置（2026-09-22-desktop-pet-design.md §4.6）：
//   petsDir()/pet_config.json —— pet 字段从 config.json 迁出（模块隔离：
//   module_pet 不再依赖 module_settings 的 SettingsStore，module_* 互依禁令）
#pragma once

#include <cmath>
#include <filesystem>
#include <string>

#include "core/result.h"
#include "module_pet/pet_skin.h"  // kDefaultSkinName

namespace module::pet {

// 显示大小三档（2026-09-30 用户需求「桌宠显示大小」：设置页下拉，窗口尺寸
// 与精灵渲染同步缩放）。值/文案/默认档的**单源**在此——设置页侧经
// module_pet_bridge.h 的 petScalePresetLabels() 取文案、按下标回传，不自带
// 一份防漂移。窗口尺寸换算见 pet_skin.h scaledFrameSize（缩放后仅钳上界 512，下界不钳）
inline constexpr double kPetScalePresets[] = {0.75, 1.0, 1.5};
inline constexpr const char* kPetScalePresetLabels[] = {"小 0.75×", "中 1.0×",
                                                        "大 1.5×"};
inline constexpr int kPetScalePresetCount = 3;
inline constexpr int kPetScaleDefaultIndex = 1;  // 默认「中」1.0×

// 越界/漂移值收敛：精确命中三档之一（1e-6 容差）原样返回，否则回默认中档
// （防手改 pet_config.json 注入任意缩放）。load 与 applyPetScale 共用
inline double sanitizePetScale(double scale) {
    for (int i = 0; i < kPetScalePresetCount; ++i) {
        if (std::abs(scale - kPetScalePresets[i]) < 1e-6) {
            return kPetScalePresets[i];
        }
    }
    return kPetScalePresets[kPetScaleDefaultIndex];
}

// 缩放值反查档位下标（下拉 selected 用；非法值 → 默认档下标）
inline int petScaleIndexOf(double scale) {
    const double sani = sanitizePetScale(scale);
    for (int i = 0; i < kPetScalePresetCount; ++i) {
        if (sani == kPetScalePresets[i]) {
            return i;
        }
    }
    return kPetScaleDefaultIndex;  // 不可达（sanitize 必命中其一），兜底
}

struct PetConfig {
    std::string skinName = kDefaultSkinName;  // 皮肤目录名（assets/pets/<skinName>）
    int x = -1;                               // 窗口位置（逻辑屏幕坐标；<0 = 未设置，
    int y = -1;                               //  单边独立：只重置未设置的那一边）
    double scale = 1.0;                       // 显示大小档（kPetScalePresets 之一；
                                              //  默认中档 1.0×；sanitized 收敛非法值）
    // 【历史，不是当前字段】挂机穿透（clickThrough）功能已于 2026-09-24 移除：
    // 死锁设计——开启后鼠标穿透导致右键也点不中桌宠，只能回设置页关，用户易被
    // 困住。字段自此不再读写；旧 pet_config.json 里残留的 clickThrough 键被 load
    // 静默忽略（本结构只有 skinName/x/y/scale 四个键），下次 save 时随整文件
    // 重写消失
};

// 默认配置文件位置（core::paths::petsDir() 下；DEVDESK_PETS_DIR 单项覆盖
// 或 DEVDESK_DATA_ROOT 整体隔离）
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
