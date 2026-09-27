// src/module_pet/pet_skin.h
// 桌宠皮肤（2026-09-22-desktop-pet-design.md §4.3）：
//   assets/pets/<skin-name>/pet.json + 同尺寸 RGBA PNG 序列帧
//   pet.json = {"name","frameSize":[w,h],"frameRate","license","source",
//               "states":{"idle":["f0.png",...],"poked":[...]}}
//
// 设计决策：
//   - 内置皮肤（robot + ghost）全部**程序生成**：pet.json
//     + 帧由本文件在运行时首次生成（简单形状 + 1px 抗锯齿边缘），格式真实、
//     素材可整体替换为真皮肤包（把 assets/pets/<name> 换成美术资源即可）
//   - 解析是纯函数（parsePetSkinJson），不碰文件系统，供单测
//   - 状态帧：idle/poked/sleep/typing 四态（M2，设计 §4.4/§4.5）；sleep/
//     typing/phrases 缺失时静默忽略（老皮肤包前向兼容）
//   - 气泡短语表（M2 设计 §4.5 内容源①）：pet.json 顶层可选 "phrases"
//     数组；缺失回退内置语料（换肤可换性格）
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::pet {

// 内置占位皮肤名（pet_config 的 skinName 默认值与此一致）
inline constexpr const char* kDefaultSkinName = "robot";

struct PetSkin {
    std::string name;                 // 皮肤名（= 目录名）
    int frameWidth = 128;             // 帧尺寸 = 窗口尺寸（紧包围盒，设计 §4.2）
    int frameHeight = 128;
    double frameRate = 4.0;           // 帧/秒（idle/poked 共用，MVP）
    std::string license;              // 授权信息随包自带（设置页可展示，M2）
    std::string source;
    std::vector<std::string> idleFrames;   // 帧文件名（循环播放）
    std::vector<std::string> pokedFrames;  // 被戳反应帧（播一遍回 idle；可为空）
    std::vector<std::string> sleepFrames;  // 睡眠循环帧（M2；可为空 = 不入睡）
    std::vector<std::string> typingFrames; // 代理流式"打字"帧（M2；空 = 回落 idle）
    std::vector<std::string> phrases;      // 气泡短语表（M2；空 = 内置语料）
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

// 生成内置占位皮肤：pet.json + idle/poked/sleep/typing 帧 + 短语表（程序
// 绘制 PNG，未压缩 deflate，零第三方依赖）。目录已存在且 pet.json 可解析时
// 直接加载；但内置占位皮肤缺 M2 帧时重新生成升级（用户替换过的皮肤不动）
core::Result<PetSkin> ensureDefaultSkin(const std::filesystem::path& dir);

// 全部内置皮肤名（程序生成的 robot/ghost）
const std::vector<std::string>& builtinSkinNames();

// 在 root 下生成/升级全部内置皮肤（ensureDefaultSkin 同款语义对每款适用：
// 已存在且含 M2 帧的直接加载，用户替换过的原样保留）。返回成功清单；
// 供 startPetWindow 落盘后，discoverSkins/设置页下拉/右键换肤菜单零改动发现
std::vector<std::string> ensureBuiltinSkins(const std::filesystem::path& root);

// 皮肤根目录候选（依次：appDataDir/pets → 可执行文件旁 assets/pets）
// 注：DEVDESK_PETS_DIR 覆盖时只返回 appDataDir/pets 一条（测试隔离）
std::vector<std::filesystem::path> skinRootCandidates();

// 发现可用皮肤（M2 多皮肤 UI 消费）：扫皮肤根候选下所有含 pet.json 的
// 目录名，去重（petsDir 优先）。纯文件系统查询，供设置页下拉/右键换肤菜单
std::vector<std::string> discoverSkins();

// 内置气泡语料（皮肤无 phrases 时的回退，设计 §4.5 内容源①）
inline const std::vector<std::string>& defaultPhrases() {
    static const std::vector<std::string> kPhrases = {
        "喵~", "在忙吗？", "摸鱼中…", "休息一下吧", "今天也要加油", "Zzz…"};
    return kPhrases;
}

// 生效短语表：皮肤自带优先，空则内置语料
inline const std::vector<std::string>& phraseTable(const PetSkin& skin) {
    return skin.phrases.empty() ? defaultPhrases() : skin.phrases;
}

} // namespace module::pet
