// src/module_pet_bridge.h
// 设置页 ↔ 桌宠的进程内自由函数桥（专家审计 2026-09-27 M1 短期项）：
// 把原先只写在 settings_module.cpp 里的 5 个 extern 声明收口到单一头。
// 落点是 **src 根（应用层，与 app_sidebar.h 同级）**而非任一 module_* 目录：
// 模块间零 include 铁律（CLAUDE.md「架构要点」）管的是 module_* 互相依赖，
// 本头不属任何模块——settings include 它不构成"include 兄弟模块的头"。
//
// 边界（审计 [5][36] 的刻意裁决，本次不动）：module_* 之间不互相依赖
// （防循环，CLAUDE.md「架构要点」）。设置页桌宠卡要用这 5 个操作，但 settings
// 不 include pet 的实体头（pet_window.h / pet_skin.h——会拖进 EUI-NEO 类型与
// PetSkin/PetConfig 布局，跨 TU 各自重定义是 ODR 风险），CMake 也不 link
// devdesk_module_pet：只在本头取声明，符号在最终可执行文件（DevDesk /
// devdesk_tests 均直接链接全部 module_*）链接期解析。本头因此只依赖标准库
// （string/vector），不引任何 EUI-NEO / pet 内部类型。全部不抛异常（项目
// Result 口径）：pet 侧失败时记日志并保持原状，调用方无需接错误。
//
// 签名漂移防线（本项新增）：末尾 static_assert 把 6 个声明钉死在函数指针
// 类型上，改本头任一声明立刻编译报错；pet 侧定义 TU（pet_window.cpp /
// pet_skin.cpp）直接 include 本头，故「pet 侧改签名漏改本头」也在编译期炸，
// 而不是拖到链接期才 undefined symbol。
//
// 分组（为审计 M1 中期项「窗口生命周期归还 pet、settings 只发意图」留边界）：
//   A 只读查询——settings 取展示用列表/初值，不改 pet 任何状态；
//   B 改另一个模块状态——写盘并同步操作 pet 窗口（换肤 = 关旧窗开新窗，
//     PET-CLOSE 起「关窗」也在此组：取消勾选模块开关 = 立即关窗）。
#pragma once

#include <string>
#include <type_traits>
#include <vector>

namespace module::pet {

// ---- A 组：只读查询（无副作用）----
// 皮肤清单（扫皮肤根候选目录下含 pet.json 的目录；设置页下拉与右键换肤
// 菜单同一数据源）
std::vector<std::string> discoverSkins();
// 当前皮肤名（读 pet_config.json；applyPetSkin 双写运行时 + 盘，盘即真值）
std::string currentSkinName();
// 当前显示大小档位下标（0=小 0.75× / 1=中 1.0× / 2=大 1.5×；读
// pet_config.json，非法值已由 pet 侧 sanitize 收敛默认档）
int currentPetScaleIndex();
// 显示大小三档文案（与档位下标一一对应；单源在 pet_config.h，设置页
// 下拉 items 直接消费，不自带一份防漂移）
std::vector<std::string> petScalePresetLabels();

// ---- B 组：改 pet 模块状态（写盘 + 同步窗口操作）----
// showPetWindow 的四态结果（Bug B 2026-09-30）：原先 bool 把「未装配 /
// 已开着 / 启动失败」全折成一个 false，settings 侧无从区分反馈口径（把
// 「已开着」误报成「未启用，重启后生效」）。收成枚举后调用方按原因给
// 不同的可见反馈（未装配 = 中性「重启生效」，启动失败 = 错误样式）。
// 只在本头（标准库边界内），不引 pet / EUI-NEO 类型
enum class PetShowOutcome {
    Shown,        // 已按 rt.config 重新入队开窗请求
    AlreadyOpen,  // 窗口本就开着，无需动作
    NotAssembled, // 本会话从未装配桌宠（模块开关关）——注册门控重启后生效
    StartFailed,  // 已装配但重开失败（皮肤资产不可用，未入队开窗）
};

// 换肤：名字必须在 discoverSkins() 发现列表内（会拼进文件系统路径，防手改
// 配置注入），未知名字拒绝不动；已发现则写盘并热切换（关旧窗 → 按新皮肤
// 原位重开）；窗口未开时只持久化（下次启动生效）
void applyPetSkin(const std::string& skinName);
// 位置重置：持久化坐标清回未设置哨兵，窗口开着则立刻移回默认位
void resetPetPosition();
// 重新显示桌宠窗口（PAB-13，波 1 L7）：返回 Shown = 已重新入队开窗请求；
// AlreadyOpen = 窗口已开着（隐藏走右键「隐藏桌宠」，隐藏后本函数可重开）；
// NotAssembled = 本会话从未装配过桌宠（模块开关关，重启后生效）；
// StartFailed = 已装配但重开失败（皮肤不可用，未入队开窗）
PetShowOutcome showPetWindow();
// 关闭桌宠窗口（PET-CLOSE，2026-09-29 真机反馈「设置-模块-桌宠取消勾选，
// 桌宠没消失」）：模块开关取消勾选 → 立即关窗（与右键「隐藏桌宠」同一条关闭
// 路径）。返回 true = 曾开着/有可见意图并已请求关闭；false = 本来就关着。
// 只清「可见意图」，**不清装配标志**——同一会话重新勾选后 showPetWindow 可原位重开
bool closePetWindow();
// 显示大小切档（下标越界静默拒绝不动）：写盘并热切换（窗口开着即关旧开新，
// 尺寸 = 帧尺寸 × 档位缩放 + 气泡让位条，单位 = 点）；窗口未开时只持久化
void applyPetScaleIndex(int presetIndex);

} // namespace module::pet

// ---- 签名漂移哨兵：函数指针类型须与 pet 侧定义逐字一致（参数名不参与
// mangled name，改参数名不算漂移）----
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
