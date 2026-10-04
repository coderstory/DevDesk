// src/module_pet_bridge.h
// 设置页 ↔ 桌宠的进程内自由函数桥：把 extern 声明收口到单一中立头（settings include 它不构成
// include 兄弟模块头）。settings 不 include pet 实体头、CMake 不 link devdesk_module_pet，符号在
// 链接期解析。全部不抛异常。末尾 static_assert 把声明钉死在函数指针类型上，改声明立刻编译报错。
#pragma once

#include <string>
#include <type_traits>
#include <vector>

namespace module::pet {

// A 组：只读查询（无副作用）
// 皮肤清单（设置页下拉与右键换肤菜单同一数据源）
std::vector<std::string> discoverSkins();
// 当前皮肤名（读 app_config 的 pet_config 键）
std::string currentSkinName();
// 当前显示大小档位下标（0=小 0.75×/1=中 1.0×/2=大 1.5×）
int currentPetScaleIndex();
// 显示大小三档文案（单源在 pet_config.h）
std::vector<std::string> petScalePresetLabels();

// B 组：改 pet 模块状态（写盘 + 同步窗口操作）
// showPetWindow 四态结果：bool 会把「未装配/已开着/启动失败」折成一个 false，收成枚举以便区分
enum class PetShowOutcome {
    Shown,        // 已按 rt.config 重新入队开窗请求
    AlreadyOpen,  // 窗口本就开着，无需动作
    NotAssembled, // 本会话从未装配桌宠（模块开关关）——注册门控重启后生效
    StartFailed,  // 已装配但重开失败（皮肤资产不可用，未入队开窗）
};

// 换肤：名字必须在 discoverSkins() 内（会拼进文件系统路径，防配置注入）；写盘并热切换，
// 窗口未开时只持久化
void applyPetSkin(const std::string& skinName);
// 位置重置：持久化坐标清回未设置哨兵，窗口开着则移回默认位
void resetPetPosition();
// 重新显示桌宠窗口（四态见 PetShowOutcome）
PetShowOutcome showPetWindow();
// 关闭桌宠窗口（模块开关取消勾选 → 立即关窗，与右键「隐藏桌宠」同路径）。
// true = 曾开着/有可见意图并已请求关闭；只清可见意图，不清装配标志
bool closePetWindow();
// 显示大小切档（越界静默拒绝）：写盘并热切换，窗口未开时只持久化
void applyPetScaleIndex(int presetIndex);

} // namespace module::pet

// 签名漂移哨兵：函数指针类型须与 pet 侧定义逐字一致
static_assert(std::is_same_v<decltype(&module::pet::discoverSkins),
                             std::vector<std::string> (*)()>,
              "module_pet_bridge: discoverSkins 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::currentSkinName),
                             std::string (*)()>,
              "module_pet_bridge: currentSkinName 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::currentPetScaleIndex),
                             int (*)()>,
              "module_pet_bridge: currentPetScaleIndex 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::petScalePresetLabels),
                             std::vector<std::string> (*)()>,
              "module_pet_bridge: petScalePresetLabels 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::applyPetSkin),
                             void (*)(const std::string&)>,
              "module_pet_bridge: applyPetSkin 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::resetPetPosition),
                             void (*)()>,
              "module_pet_bridge: resetPetPosition 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::showPetWindow),
                             module::pet::PetShowOutcome (*)()>,
              "module_pet_bridge: showPetWindow 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::closePetWindow),
                             bool (*)()>,
              "module_pet_bridge: closePetWindow 签名漂移");
static_assert(std::is_same_v<decltype(&module::pet::applyPetScaleIndex),
                             void (*)(int)>,
              "module_pet_bridge: applyPetScaleIndex 签名漂移");
