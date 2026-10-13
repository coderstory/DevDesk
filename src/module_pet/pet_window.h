// 桌宠子窗口装配。桌宠不是 Router 页面模块（无 PageId/侧边栏），开窗走 app::openWindow；
// 门控在 main.cpp（enabledModules 含 "pet" 才调 startPetWindow）。
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "module_pet_bridge.h"  // PetShowOutcome（仍只依赖标准库，本头边界不变）

namespace module::pet {

// 退出二次确认（全应用删单条任务都要确认，退整个应用却零确认）。确认 UI 走原生菜单
// 二段式而非 dialog 组件（桌宠窗是 ~128px sprite 窗，自绘 dialog 会被窗口 bounds 裁剪）。

struct PetExitConfirmItem {
    std::string text;      // "-" = 分隔线
    bool disabled = false; // 说明行不可点
};

std::vector<PetExitConfirmItem> petExitConfirmItems();

// 选择路径 = 确认菜单里「确认退出」项（下标 2）；其余任何路径 = 不退出。
bool petExitConfirmChosen(const std::vector<int>& path);

// 右键菜单结构（styled 独立菜单窗 + 原生降级共用同一数据源与选择语义），
// 头文件不依赖 EUI-NEO。
struct PetMenuItemData {
    std::string text;      // "-" = 分隔线
    int iconCodepoint = 0; // FontAwesome 码点（仅 styled 用；0 = 无图标）
    bool disabled = false;
    std::vector<PetMenuItemData> children;  // 非空 = 子项组（换肤）
};

// 构建桌宠右键菜单：换肤子组 / 位置重置 / 隐藏 / 分隔线 / 退出（顺序即 path 语义基础）。
std::vector<PetMenuItemData> petContextMenuItems(const std::vector<std::string>& skins);

enum class PetMenuAction {
    None,          // 未识别路径（含组头本身）
    Skin,          // 换肤（skinIndex = 子项下标）
    ResetPosition,
    Hide,
    Exit,          // 需再过二段确认（petExitConfirmItems）
};

// path（{i} 或 {i,j}）→ 语义动作 + 皮肤下标（仅 Skin 有效，越界回 None）。纯函数。
PetMenuAction petMenuActionForPath(const std::vector<int>& path, size_t skinCount,
                                   int& skinIndex);

// 内容源②的跨模块只读桥：main 装配的逾期任务计数回调；nullopt = 数据不可用（todo
// 关闭 / 查询失败），pet 侧回退内容源①。pet 不 include module_home。
using OverdueCountGetter = std::function<std::optional<int64_t>()>;

// 内容源③的跨模块只读桥：main 装配的最近代理请求事件读取器，返回 HTTP 状态码。
// 契约：读取即消费（take 语义），一个事件最多弹一次。pet 不 include module_claude_proxy。
using ProxyEventGetter = std::function<std::optional<int>()>;

struct PetWindowConfig {
    std::string skinName;   // 空 = kDefaultSkinName
    int x = -1;             // -1 = 未设置（取主屏右下角默认位）；其他负值是
    int y = -1;             // 左/上副屏的合法坐标（多显示器，钳制按位置选屏后保留）
    // 显示大小档：窗口尺寸 = scaledFrameSize(帧尺寸, scale) + 气泡让位条，单位 = 点。
    double scale = 1.0;
    // 内容源②回调（main 注入；热换肤重开时原样携带）。空 = 气泡只出静态语料①。
    OverdueCountGetter getOverdueCount;
    // 内容源③回调（main 注入；换肤重开同样携带）。空 = 气泡按 ②→① 回退。
    ProxyEventGetter getProxyEvent;
};

// 无边框窗拖拽锚点：DragEvent.delta/totalX 是「窗口内相对坐标」，窗口被回调自身移动后
// 参考系漂移（+10/-5/+10 振荡 = 抖动）。改锚定「窗口原点 − 光标屏幕位」，1:1 跟随、DPI 无关。
struct PetDragAnchor {
    bool valid = false;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

// 拖拽跟随一步（纯函数）：首次调用捕获锚点；返回目标窗口位置（GLFW 屏幕坐标）。
std::pair<int, int> petDragMove(PetDragAnchor& anchor, int windowX, int windowY,
                                double cursorX, double cursorY);

// 拖拽结束清锚点，下次拖拽重新捕获（防跨会话锚点跳变）。
void petDragEnd(PetDragAnchor& anchor);

// 已知坐标钳进主显示器工作区（纯函数）；workW/workH <= 0（查询失败）原样返回。
// frameW/frameH 传整窗尺寸；落盘与启动路径共用本钳制——负坐标落盘会被 -1 哨兵吞掉。
std::pair<int, int> clampPetPosToWorkArea(int x, int y, int workX, int workY,
                                          int workW, int workH, int frameW,
                                          int frameH);

// 单显示器工作区矩形（与 core::window::MonitorWorkArea 同构，映射在 .cpp）。
struct PetWorkArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 多显示器钳制（纯函数）：按「位置所在或最近的显示器」选工作区再钳
//（等价 Win32 MONITOR_DEFAULTTONEAREST）；areas 为空或选中块宽高 <=0 原样返回。
std::pair<int, int> clampPetPosToMonitors(int x, int y,
                                          const std::vector<PetWorkArea>& areas,
                                          int frameW, int frameH);

// 皮肤四态定义见 pet_skin.h——此处只作前置声明，避免本头拖进 core/result.h。
struct PetSkin;

// 随机挑一句气泡短语（短语表随皮肤）。纯函数；随机数走 <random>。
std::string pickPetPhrase(const PetSkin& skin);

// Sleep 零渲染：系统挂起置 true、唤醒置 false；compose 入口检查后整帧跳过。
void setPetRenderPaused(bool paused);
bool isPetRenderPaused();

// 打开桌宠窗口（主循环起来前调用）。皮肤缺失先回退默认皮肤，默认皮肤也不可用则返回 false。
bool startPetWindow(const PetWindowConfig& config);

// 重新显示桌宠窗口。四态：Shown = 已重新入队；AlreadyOpen = 窗口已开着；
// NotAssembled = 本会话从未装配；StartFailed = 已装配但重开失败（皮肤不可用）。
PetShowOutcome showPetWindow();

// 关闭桌宠窗口（模块开关取消勾选 → 立即隐藏）。返回 true = 曾有窗口/可见意图并已关闭。
bool closePetWindow();

// 桌宠窗口可见意图：start/show 置 true，close 置 false；无头与窗口已关时的唯一真值。
bool petWindowRequested();

// 测试缝：进程级桌宠运行时整体回厂；装配标志只置不清，单测只能经这里表达
//「本会话未装配」态。生产代码不得调用。
void resetPetRuntimeForTests();

// 换肤：校验名字在 discoverSkins() 内后写盘并热切换；未知名字拒绝不动。
void applyPetSkin(const std::string& skinName);

// 位置重置：持久化坐标清回 -1（未设置），窗口开着则立刻移回主屏右下角默认位。
void resetPetPosition();

// 设置页桥（只读）：当前皮肤名，读 pet_config 键。标量返回值避免共享 PetConfig
// 结构体布局（ODR 风险）。
std::string currentSkinName();

// 显示大小三档（设置页经 module_pet_bridge.h 消费，声明/签名哨兵同 currentSkinName 先例）。

// 当前显示大小档位下标（0=小 0.75×/1=中 1.0×/2=大 1.5×），读 pet_config 键。
int currentPetScaleIndex();

// 三档文案（与下标一一对应；单源 = pet_config.h kPetScalePresetLabels）。
std::vector<std::string> petScalePresetLabels();

// 显示大小切档（越界静默拒绝）：写盘并热切换；窗口未开时只持久化。
void applyPetScaleIndex(int presetIndex);

} // namespace module::pet
