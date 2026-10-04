// 桌宠自身持久化配置：整份存 SQLite app_config 表的一行（键 pet_config），不读文件。
// 整份存一行而非按顶层键拆行：只有一个写者，每次都是「读整结构→改字段→写回」。
#pragma once

#include <cmath>
#include <string>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "module_pet/pet_skin.h"  // kDefaultSkinName

namespace module::pet {

// 显示大小三档（值/文案/默认档单源在此；设置页经 petScalePresetLabels 取文案）。
// 窗口尺寸换算见 pet_skin.h scaledFrameSize（缩放后仅钳上界 512，下界不钳）。
inline constexpr double kPetScalePresets[] = {0.75, 1.0, 1.5};
inline constexpr const char* kPetScalePresetLabels[] = {"小 0.75×", "中 1.0×",
                                                        "大 1.5×"};
inline constexpr int kPetScalePresetCount = 3;
inline constexpr int kPetScaleDefaultIndex = 1;  // 默认「中」1.0×

// 档位数与两个数组静态绑定：档位漂移（改数组不改 count）编译期即红。
static_assert(kPetScalePresetCount ==
                  sizeof(kPetScalePresets) / sizeof(kPetScalePresets[0]),
              "kPetScalePresets 元素数必须等于 kPetScalePresetCount");
static_assert(kPetScalePresetCount == sizeof(kPetScalePresetLabels) /
                                          sizeof(kPetScalePresetLabels[0]),
              "kPetScalePresetLabels 元素数必须等于 kPetScalePresetCount");

// 越界/漂移值收敛：精确命中三档之一（1e-6 容差）原样返回，否则回默认中档。
inline double sanitizePetScale(double scale) {
    for (int i = 0; i < kPetScalePresetCount; ++i) {
        if (std::abs(scale - kPetScalePresets[i]) < 1e-6) {
            return kPetScalePresets[i];
        }
    }
    return kPetScalePresets[kPetScaleDefaultIndex];
}

// 缩放值反查档位下标（下拉 selected 用；非法值 → 默认档下标）。
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
    // clickThrough 已不支持，勿加回：穿透后右键也点不中，用户易被困住。
};

// 默认 app_config 键名（供测试与工具层拼键名）。
inline constexpr const char* kPetConfigKey = "pet_config";

// pet_config 读写（不抛异常；键缺失/值损坏一律回退默认）。db 可为 nullptr——那
// 不是「没配置」而是「库没就绪」：save() 返回 fail，load() 回退默认并打 LOG_ERROR。
class PetConfigStore {
public:
    explicit PetConfigStore(core::data::SqliteDb* db);

    PetConfig load() const;

    // 与 load() 同源，把「库未就绪/表不存在/值损坏」如实报出来。
    core::Result<PetConfig> loadChecked() const;

    core::Result<void> save(const PetConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）。
    static PetConfig sanitized(const PetConfig& cfg);

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::pet
