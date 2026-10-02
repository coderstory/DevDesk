// src/module_pet/pet_config.h
// 桌宠自身持久化配置（2026-09-22-desktop-pet-design.md §4.6）：
//   **SQLite app_config 表**（键 `pet_config`）—— 原 petsDir()/pet_config.json
//   已于 2026-10-03（批次 E2，用户第 17 条「所有 app 配置全部进 SQLite」）
//   退役：不读、不写、不迁移。pet 字段当初从 config.json 迁出过一次
//   （模块隔离：module_pet 不依赖 module_settings 的 SettingsStore），本次是
//   迁到**中立层** app_config 表，模块隔离禁令依旧不破。
//
// 为什么整份存一行、而不像 config.json 那样按顶层键拆行（裁决）：
//   本配置**只有一个写者**（pet_window 的 applyPetSkin / applyPetScale /
//   applyPetPosition），且每次都是「load 整个结构 → 改一两个字段 → 整个存回」。
//   拆行的收益（多写者互不覆盖）在这里不存在，成本却是每次 save 4 次 UPSERT
//   + 一次清理。config.json 那边收益真实（main / app_sidebar /
//   settings_module / app_assembly / app_tools 各写各的），故那边拆。
#pragma once

#include <cmath>
#include <string>

#include "core/result.h"
#include "core/sqlite_db.h"
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

// 档位数与两个数组静态绑定：petScalePresetLabels()（pet_window.cpp）以裸下标
// [0][1][2] 取文案、sanitizePetScale/petScaleIndexOf 以 count 为界循环——
// 档位漂移（改数组不改 count）编译期即红，不再运行期越界读
static_assert(kPetScalePresetCount ==
                  sizeof(kPetScalePresets) / sizeof(kPetScalePresets[0]),
              "kPetScalePresets 元素数必须等于 kPetScalePresetCount");
static_assert(kPetScalePresetCount == sizeof(kPetScalePresetLabels) /
                                          sizeof(kPetScalePresetLabels[0]),
              "kPetScalePresetLabels 元素数必须等于 kPetScalePresetCount");

// 越界/漂移值收敛：精确命中三档之一（1e-6 容差）原样返回，否则回默认中档
// （防脏值注入任意缩放）。load 与 applyPetScale 共用
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
    // 困住。字段自此不再读写；旧配置里残留的 clickThrough 键被 load
    // 静默忽略（本结构只有 skinName/x/y/scale 四个键），下次 save 时随整文件
    // 重写消失
};

// 默认 app_config 键名（供测试与工具层拼键名；实现与 save/load 同一来源）
inline constexpr const char* kPetConfigKey = "pet_config";

// pet_config 读写（不抛异常；键缺失 / 值损坏一律回退默认值）。
// db：应用共享连接（生产传 ::core::appDatabase()，测试传临时库句柄）。允许为
// nullptr，但那不是「没配置」而是「数据库没就绪」：save() 返回 fail Result，
// load() 回退默认值并打 LOG_ERROR。
class PetConfigStore {
public:
    explicit PetConfigStore(core::data::SqliteDb* db);

    PetConfig load() const;

    // 与 load() 同源，把「库未就绪 / 表不存在 / 值损坏」如实报出来
    core::Result<PetConfig> loadChecked() const;

    core::Result<void> save(const PetConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）
    static PetConfig sanitized(const PetConfig& cfg);

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::pet
