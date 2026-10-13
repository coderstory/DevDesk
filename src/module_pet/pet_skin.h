// 桌宠皮肤：assets/pets/<skin-name>/pet.json + 同尺寸 RGBA PNG 序列帧；内置皮肤
//（robot/ghost）为随包静态资产，运行期只解析/发现、不程序生成。
// 解析是纯函数（parsePetSkinJson）不碰文件系统；sleep/typing/phrases 缺失时静默忽略。
#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"

namespace module::pet {

// 内置占位皮肤名（pet_config 的 skinName 默认值与此一致）。
inline constexpr const char* kDefaultSkinName = "robot";

// 帧尺寸合法域（parsePetSkinJson 校验与显示大小缩放共用同一条界）。
inline constexpr int kMinFrameSize = 16;
inline constexpr int kMaxFrameSize = 512;

// 显示大小缩放后的帧尺寸（纯函数）：等比单因子 f = min(scale, kMaxFrameSize/w,
// kMaxFrameSize/h)——上界按比例收敛，长宽比恒等原始比、Cover 零裁切；下界不钳。
// 单位 = 点，不乘 Retina contentScale。
inline std::pair<int, int> scaledFrameSize(int frameWidth, int frameHeight,
                                           double scale) {
    if (frameWidth <= 0 || frameHeight <= 0 || scale <= 0.0) {
        return {frameWidth, frameHeight};  // 非法入参防御（上游已保证合法）
    }
    const double f = std::min(
        {scale, static_cast<double>(kMaxFrameSize) / frameWidth,
         static_cast<double>(kMaxFrameSize) / frameHeight});
    return {static_cast<int>(std::round(frameWidth * f)),
            static_cast<int>(std::round(frameHeight * f))};
}

struct PetSkin {
    std::string name;                 // 皮肤名（= 目录名）
    int frameWidth = 128;             // 帧尺寸 = 窗口尺寸（紧包围盒）
    int frameHeight = 128;
    double frameRate = 4.0;           // 帧/秒（idle/poked 共用）
    std::string license;              // 授权信息随包自带（设置页可展示）
    std::string source;
    std::vector<std::string> idleFrames;   // 帧文件名（循环播放）
    std::vector<std::string> pokedFrames;  // 被戳反应帧（播一遍回 idle；可为空）
    std::vector<std::string> sleepFrames;  // 睡眠循环帧（可为空 = 不入睡）
    std::vector<std::string> typingFrames; // 代理流式"打字"帧（空 = 回落 idle）
    std::vector<std::string> phrases;      // 气泡短语表（空 = 内置语料）
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

// 纯函数：pet.json 文本 → PetSkin（dir 留空）。校验 object + states.idle 非空数组；
// frameRate 钳 [1,30]、frameSize 钳 [16,512]。
core::Result<PetSkin> parsePetSkinJson(const std::string& jsonText);

// 读目录下 pet.json 并解析（dir 写进结果）。
core::Result<PetSkin> loadSkin(const std::filesystem::path& dir);

// 默认皮肤 = 随包 robot：按皮肤根候选顺序加载，任一命中即返回，全未命中返回 fail。
core::Result<PetSkin> ensureDefaultSkin();

// 皮肤根目录候选（纯派生，不读环境变量）：petsDir() → 可执行文件旁 assets/pets。
std::vector<std::filesystem::path> skinRootCandidates();

// 发现可用皮肤：扫皮肤根候选下所有含 pet.json 的目录名，去重（petsDir 优先）。
std::vector<std::string> discoverSkins();

// 内置气泡语料（皮肤无 phrases 时的回退）。
inline const std::vector<std::string>& defaultPhrases() {
    static const std::vector<std::string> kPhrases = {
        "喵~", "在忙吗？", "摸鱼中…", "休息一下吧", "今天也要加油", "Zzz…"};
    return kPhrases;
}

inline const std::vector<std::string>& phraseTable(const PetSkin& skin) {
    return skin.phrases.empty() ? defaultPhrases() : skin.phrases;
}

} // namespace module::pet
