// src/module_pet/pet_skin.h
// 桌宠皮肤（2026-09-22-desktop-pet-design.md §4.3）：
//   assets/pets/<skin-name>/pet.json + 同尺寸 RGBA PNG 序列帧
//   pet.json = {"name","frameSize":[w,h],"frameRate","license","source",
//               "states":{"idle":["f0.png",...],"poked":[...]}}
//
// 设计决策：
//   - 内置默认皮肤（clay-cat）是**程序生成的占位皮肤**：pet.json + 帧全部
//     由本文件在运行时首次生成（简单形状 + 1px 抗锯齿边缘），格式真实、
//     素材可整体替换为真皮肤包（把 assets/pets/clay-cat 换成美术资源即可）
//   - 解析是纯函数（parsePetSkinJson），不碰文件系统，供单测
//   - MVP 只消费 idle/poked 两个状态；pet.json 里其他状态名静默忽略
//     （前向兼容，M2 多状态机直接吃）
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::pet {

// 内置占位皮肤名（pet_config 的 skinName 默认值与此一致）
inline constexpr const char* kDefaultSkinName = "clay-cat";

struct PetSkin {
    std::string name;                 // 皮肤名（= 目录名）
    int frameWidth = 128;             // 帧尺寸 = 窗口尺寸（紧包围盒，设计 §4.2）
    int frameHeight = 128;
    double frameRate = 4.0;           // 帧/秒（idle/poked 共用，MVP）
    std::string license;              // 授权信息随包自带（设置页可展示，M2）
    std::string source;
    std::vector<std::string> idleFrames;   // 帧文件名（循环播放）
    std::vector<std::string> pokedFrames;  // 被戳反应帧（播一遍回 idle；可为空）
    std::filesystem::path dir;             // 皮肤目录（解析帧路径；纯解析时为空）

    // poked 无帧时回落 idle（皮肤可以只有一套帧）
    const std::vector<std::string>& frames(bool poked) const {
        return (poked && !pokedFrames.empty()) ? pokedFrames : idleFrames;
    }
    std::filesystem::path framePath(bool poked, size_t index) const {
        const std::vector<std::string>& list = frames(poked);
        if (list.empty()) return {};
        return dir / list[index % list.size()];
    }
};

// 纯函数：pet.json 文本 → PetSkin（dir 留空）。校验：必须是 object、states.idle
// 必须是非空字符串数组；frameRate 钳到 [1,30]、frameSize 钳到 [16,512]。
core::Result<PetSkin> parsePetSkinJson(const std::string& jsonText);

// 读目录下 pet.json 并解析（dir 写进结果）
core::Result<PetSkin> loadSkin(const std::filesystem::path& dir);

// 生成内置占位皮肤：pet.json + idle 4 帧 + poked 2 帧（程序绘制 PNG，
// 未压缩 deflate，零第三方依赖）。目录已存在且 pet.json 可解析时直接加载。
core::Result<PetSkin> ensureDefaultSkin(const std::filesystem::path& dir);

// 皮肤根目录候选（依次：exe 旁 assets/pets → appDataDir/pets）
std::vector<std::filesystem::path> skinRootCandidates();

} // namespace module::pet
