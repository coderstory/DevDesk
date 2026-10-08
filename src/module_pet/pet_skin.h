// src/module_pet/pet_skin.h
// 桌宠皮肤（2026-09-22-desktop-pet-design.md §4.3）：
//   assets/pets/<skin-name>/pet.json + 同尺寸 RGBA PNG 序列帧
//   pet.json = {"name","frameSize":[w,h],"frameRate","license","source",
//               "states":{"idle":["f0.png",...],"poked":[...]}}
//
// 设计决策：
//   - 内置皮肤（robot + ghost）为**随包静态资产**（assets/pets/<name>，git
//     追踪，CMake POST_BUILD 随分发拷贝到可执行文件旁）；运行期只解析/发现，
//     不程序生成（生成器与自研 PNG 编码器已于 2026-09-28 删除）
//   - 解析是纯函数（parsePetSkinJson），不碰文件系统，供单测
//   - 状态帧：idle/poked/sleep/typing 四态（M2，设计 §4.4/§4.5）；sleep/
//     typing/phrases 缺失时静默忽略（老皮肤包前向兼容）
//   - 气泡短语表（M2 设计 §4.5 内容源①）：pet.json 顶层可选 "phrases"
//     数组；缺失回退内置语料（换肤可换性格）
#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"

namespace module::pet {

// 内置占位皮肤名（pet_config 的 skinName 默认值与此一致）
inline constexpr const char* kDefaultSkinName = "robot";

// 帧尺寸合法域（PAB-05 数字对齐口径：parsePetSkinJson 的帧尺寸校验与显示
// 大小缩放共用同一条界——原两处裸字面量 16/512 收成命名常量单源）
inline constexpr int kMinFrameSize = 16;
inline constexpr int kMaxFrameSize = 512;

// 显示大小缩放后的帧尺寸（纯函数，「档位 → 窗口尺寸换算」单测断言用）：
// **等比单因子** f = min(scale, kMaxFrameSize/w, kMaxFrameSize/h)（审计 F1，
// 2026-09-30）——上界按比例收敛：缩放后长宽比恒等帧原始宽高比，sprite
//（ui.image 未设 imageFit = 默认 Cover）恰好贴合零裁切；不越界时 f=scale，
// 其余档位/皮肤行为不变。旧实现逐维独立钳 [16,512] 会破宽高比：kirari
// 360×480 大档 540×720→512×512（0.75→1.0），Cover 中心裁掉头脚各 12.5%，
// 已废弃。下界不钳：16 是皮肤解析的帧域下限（本文件 parsePetSkinJson），
// 16×0.75=12 的小窗无害且等比优先。窗口单位 = 点（GLFW windowSize 同
// 口径），不乘 Retina contentScale
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

// 默认皮肤 = 随包 robot（kDefaultSkinName）：按皮肤根候选（petsDir → 可执行
// 文件旁 assets/pets）顺序加载 assets/pets/robot/pet.json + 帧；任一候选命中
// 即返回，全部未命中返回 fail（2026-09-28 起生成器已删，不写盘生成）
core::Result<PetSkin> ensureDefaultSkin();

// 皮肤根目录候选（纯派生，不读任何环境变量：依次 petsDir() → 可执行文件旁
// assets/pets；exe 目录解析失败时跳过后者——空路径拼接会退化成相对 cwd）
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
