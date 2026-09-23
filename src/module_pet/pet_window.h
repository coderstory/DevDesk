// src/module_pet/pet_window.h
// 桌宠子窗口装配（2026-09-22-desktop-pet-design.md §4.1/§4.2）。
// 桌宠不是 Router 页面模块（无 PageId/侧边栏），开窗走 app::openWindow；
// 门控在 main.cpp（enabledModules 含 "pet" 才调 startPetWindow，零执行语义）。
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace module::pet {

// ---- 退出二次确认（审计 [25]：全应用删单条任务都要确认，退整个应用却零
// 确认，且入口埋在桌宠小窗右键菜单紧邻低危项，误触即断代理/丢弹窗状态）----
// 确认 UI 走原生菜单二段式而非 dialog 组件：桌宠窗是 ~128px sprite 窗，
// 自绘 dialog 会被窗口 bounds 裁剪（右键菜单当年因此换原生，见
// pet_window.cpp 同一先例注释）。以下为无头可测的纯数据层：
// petExitConfirmItems() 出菜单文本结构，petExitConfirmChosen(path) 判
// 「确认退出」被选中（true 才许 app::requestExit）。头文件不依赖
// EUI-NEO，pet_window.cpp 里再转成 core::platform::ContextMenuItem

struct PetExitConfirmItem {
    std::string text;      // "-" = 分隔线
    bool disabled = false; // 说明行不可点
};

std::vector<PetExitConfirmItem> petExitConfirmItems();

// 选择路径 = 确认菜单里「确认退出」项（分隔线不占位、说明行占 0 号位，
// 「确认退出」下标 2、「取消」下标 3）；其余任何路径（取消/Esc/嵌套）= 不退出
bool petExitConfirmChosen(const std::vector<int>& path);

// ---- 右键菜单结构（菜单美化 2026-09-23：styled 独立菜单窗 + 原生降级
// 共用同一数据源与同一选择语义）----
// 照 PetExitConfirmItem 先例，头文件不依赖 EUI-NEO：纯数据 + 语义动作枚举，
// pet_window.cpp 里分别映射成 components::StyledMenuItem（带 FA 图标）与
// core::platform::ContextMenuItem（原生降级）。path → 动作走
// petMenuActionForPath，两条 UI 路径共享（原生子菜单 {i,j} 与 styled 内联
// 子项组 path 语义一致）。
struct PetMenuItemData {
    std::string text;      // "-" = 分隔线
    int iconCodepoint = 0; // FontAwesome 码点（仅 styled 用；0 = 无图标）
    bool disabled = false;
    std::vector<PetMenuItemData> children;  // 非空 = 子项组（换肤）
};

// 构建桌宠右键菜单：穿透切换 /（有皮肤时）换肤子组 / 位置重置 / 隐藏 /
// 分隔线 / 退出。顺序即 path 语义基础
std::vector<PetMenuItemData> petContextMenuItems(bool clickThrough,
                                                 const std::vector<std::string>& skins);

enum class PetMenuAction {
    None,          // 未识别路径（含组头本身）
    ToggleThrough, // 鼠标穿透开关
    Skin,          // 换肤（skinIndex = 子项下标）
    ResetPosition,
    Hide,
    Exit,          // 需再过二段确认（petExitConfirmItems）
};

// path（{i} 或 {i,j}）→ 语义动作 + 皮肤下标（仅 Skin 有效，越界回 None）。
// 纯函数，单测断言用（styled / 原生共用选择数学，杜绝双实现漂移）
PetMenuAction petMenuActionForPath(const std::vector<int>& path, size_t skinCount,
                                   int& skinIndex);

// 气泡内容源②的跨模块只读桥（§4.5，审计 [18]）：main 装配的逾期任务计数
// 回调（getClaudeService 同型的 app 级桥——main 层查 Home 数据经 PetWindowConfig
// 喂给 pet）。返回 nullopt = 数据不可用（todo 模块关闭 / 查询失败），pet 侧
// 回退内容源①静态语料（M2 验收降级条款：不显示死数据）。
// pet 只见 std::function 签名、不 include module_home——module_* 隔离不破
using OverdueCountGetter = std::function<std::optional<int64_t>()>;

struct PetWindowConfig {
    std::string skinName;   // 空 = kDefaultSkinName
    int x = -1;             // -1 = 未设置（取主显示器工作区右下角）；其他负值是
    int y = -1;             // 左/上副屏的合法坐标（M2 多显示器，钳制按位置选屏后保留）
    bool clickThrough = false;  // 挂机穿透（右键菜单可切换，持久化）
    // 内容源②回调（main 注入；applyPetSkin 热换肤重开时从 rt.config 原样
    // 携带不丢桥）。空 = 无数据源，气泡只出静态语料①
    OverdueCountGetter getOverdueCount;
};

// 无边框窗拖拽锚点（拖拽抖动修复，2026-09 真机验证症状 1）：
// DragEvent.delta/totalX 是「窗口内相对坐标」——窗口被拖拽回调自身移动后，
// 后续 delta 的参考系随之漂移（窗口 +d 移动使下一次光标读数偏 -d，
// +10/-5/+10 振荡 = 抖动）；且 delta 单位是帧缓冲像素，DPI≠100% 时与
// setWindowPos 的屏幕坐标差一个 contentScale。锚定「窗口原点 - 光标屏幕位」
//（getWindowPos + getCursorPosition，同为 GLFW 屏幕坐标系、同为实时值），
// 窗口位 = 光标屏幕位 + 锚点，精确 1:1 跟随、无反馈回路、DPI 无关。
struct PetDragAnchor {
    bool valid = false;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

// 拖拽跟随一步（纯函数，test_pet.cpp 断言用）：首次调用（或锚点无效）捕获
// 锚点。cursorX/Y = 窗口内光标原始值（getCursorPosition），windowX/Y = 当前
// 窗口位置（getWindowPos）。返回目标窗口位置（GLFW 屏幕坐标）
std::pair<int, int> petDragMove(PetDragAnchor& anchor, int windowX, int windowY,
                                double cursorX, double cursorY);

// 拖拽结束（释放/取消）清锚点，下次拖拽重新捕获（防止跨会话锚点跳变）
void petDragEnd(PetDragAnchor& anchor);

// 已知坐标钳制进主显示器工作区（纯函数，单测断言用；设计 §4.6，M1 仅主显
// 示器）。workW/workH <= 0（工作区查询失败：无头/早于 glfwInit）原样返回。
// 审计 [12]：落盘路径（savePetPosition/applyPetSkin）与启动路径共用本钳制
// ——负坐标若直接落盘，会被 pet_config 的 -1「未设置」哨兵吞掉，重启后桌宠
// 跳回默认位。已知坐标（>=0）才许进本函数，哨兵判断归调用方
std::pair<int, int> clampPetPosToWorkArea(int x, int y, int workX, int workY,
                                          int workW, int workH, int frameW,
                                          int frameH);

// 单显示器工作区矩形（与 core::window::MonitorWorkArea 同构，头文件不依赖
// EUI-NEO——照 PetExitConfirmItem 先例，映射在 pet_window.cpp）
struct PetWorkArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// M2 多显示器钳制（纯函数，单测断言用）：按「位置所在或最近的显示器」选工作
// 区再钳——点在某工作区矩形内取该块（多块含住取枚举序首个，实际不会重叠）；
// 都不含则取点-矩形欧氏距离最近的一块（等价 Win32 MONITOR_DEFAULTTONEAREST，
// 处理拖过边缘/两屏之间）。副屏负坐标因此不再被主屏钳吞掉。areas 为空或
// 选中块宽高 <=0：原样返回（无头兜底，与 clampPetPosToWorkArea 同语义）
std::pair<int, int> clampPetPosToMonitors(int x, int y,
                                          const std::vector<PetWorkArea>& areas,
                                          int frameW, int frameH);

// M2 Sleep 零渲染：系统挂起（PBT_APMSUSPEND）置 true、唤醒置 false。
// petCompose 入口检查 isPetRenderPaused()，true 则整帧跳过（不重建成命令 /
// 不推动画 / 不跑 onTimer / 不计气泡倒计时），唤醒后自动恢复。回调由
// startPetWindow 注册到 EUI-NEO core::platform::setSystemPowerHandler
void setPetRenderPaused(bool paused);
bool isPetRenderPaused();

// 打开桌宠窗口（主循环起来前调用；openWindow 入队、首帧消费）。
// 皮肤缺失时自动生成内置占位皮肤；位置钳制进主显示器工作区。
void startPetWindow(const PetWindowConfig& config);

// 换肤（M2）：校验 skinName 是 discoverSkins() 发现到的皮肤后写盘并热切换
//（关旧窗 → 按新皮肤重开，位置沿用当前窗口）；未知名字拒绝不动。
// 窗口未开时只持久化（下次启动生效）
void applyPetSkin(const std::string& skinName);

// 位置重置（M2）：持久化坐标清回 -1（未设置），窗口开着则立刻移回
// 主显示器工作区右下角默认位
void resetPetPosition();

// 鼠标穿透（任务⑥「穿透锁死恢复路径」）：写 runtime 状态 + 即时
// setWindowMousePassthrough + 持久化 pet_config.json。设置页桌宠卡调用
// ——桌宠锁死（clickThrough=true 时右键也点不中自己）后，设置页是唯一
// 恢复入口。窗口未开（桌宠关/隐藏）时只持久化，下次 startPetWindow 生效
void setPetClickThrough(bool enabled);

} // namespace module::pet
