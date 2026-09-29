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

// 构建桌宠右键菜单：（有皮肤时）换肤子组 / 位置重置 / 隐藏 /
// 分隔线 / 退出。顺序即 path 语义基础
std::vector<PetMenuItemData> petContextMenuItems(const std::vector<std::string>& skins);

enum class PetMenuAction {
    None,          // 未识别路径（含组头本身）
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

// 气泡内容源③的跨模块只读桥（M2 §4.5，产品审计 [18]）：main 装配的最近
// 代理请求事件读取器（ClaudeService::takeProxyEvent 同款 app 级桥，main 层
// 查 claude_proxy 经 PetWindowConfig 喂给 pet）。返回值为该请求回给客户端
// 的 HTTP 状态码：2xx/3xx = 转发完成，>=400 = 服务端出错，0 = 上游不可达
//（网络失败无响应码）；nullopt = 无待消费事件（桥未注入 = claude_proxy 关闭、
// 无请求、或事件过期丢弃）。契约要点：读取即消费（take 语义，claude_proxy
// 侧读走清空），保证一个事件最多弹一次气泡、不刷屏。pet 只见 std::function
// 签名、不 include module_claude_proxy——module_* 隔离不破（同 OverdueCountGetter
// 先例）。nullopt 时调用方按 ②逾期 → ①静态语料顺序回退
using ProxyEventGetter = std::function<std::optional<int>()>;

struct PetWindowConfig {
    std::string skinName;   // 空 = kDefaultSkinName
    int x = -1;             // -1 = 未设置（取主显示器工作区右下角）；其他负值是
    int y = -1;             // 左/上副屏的合法坐标（M2 多显示器，钳制按位置选屏后保留）
    // 显示大小档（kPetScalePresets 之一，默认中档 1.0×）。窗口尺寸 =
    // scaledFrameSize(帧尺寸, scale)（仍钳 [16,512]）+ 气泡让位条，单位 = 点
    double scale = 1.0;
    // 内容源②回调（main 注入；applyPetSkin 热换肤重开时从 rt.config 原样
    // 携带不丢桥）。空 = 无数据源，气泡只出静态语料①
    OverdueCountGetter getOverdueCount;
    // 内容源③回调（main 注入；换肤重开同样携带）。空 = 无代理事件源，
    // 气泡按 ②→① 回退
    ProxyEventGetter getProxyEvent;
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
// frameW/frameH = 被钳矩形尺寸——生产调用方传**整窗**尺寸（高 =
// petWindowHeight(帧高)，气泡让位条一并关进工作区，见 pet_bubble.h）。
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

// 皮肤四态定义见 pet_skin.h——只作函数参数的前置声明，避免本头拖进
// core/result.h（本头刻意只依赖标准库）
struct PetSkin;

// 随机挑一句气泡短语（短语表随皮肤 = phraseTable(skin)，皮肤无 phrases 时
// 回退内置语料）。纯函数，单测断言「随机源可真出多种短语」用；随机数走
// <random>（mt19937 + 进程级单例种子），不再用 std::rand 的全局序列
std::string pickPetPhrase(const PetSkin& skin);

// M2 Sleep 零渲染：系统挂起（PBT_APMSUSPEND）置 true、唤醒置 false。
// petCompose 入口检查 isPetRenderPaused()，true 则整帧跳过（不重建成命令 /
// 不推动画 / 不跑 onTimer / 不计气泡倒计时），唤醒后自动恢复。回调由
// startPetWindow 注册到 EUI-NEO core::platform::setSystemPowerHandler
void setPetRenderPaused(bool paused);
bool isPetRenderPaused();

// 打开桌宠窗口（主循环起来前调用；openWindow 入队、首帧消费）。
// 皮肤缺失时自动生成内置占位皮肤；位置钳制进主显示器工作区。
void startPetWindow(const PetWindowConfig& config);

// 重新显示桌宠窗口（PAB-13，波 1 L7 + PET-CLOSE）：设置页「显示桌宠」按钮
// 与模块开关重新勾选用（经 module_pet_bridge.h 的声明桥，settings 不
// include 本头）。门控语义：
// ① 本会话 main 从未装配过桌宠（模块开关关 = startPetWindow 从未被调用）
//    → 返回 false，不凭空开窗（该开关本就「重启生效」）；
// ② 窗口已开着（handle 非空）→ 返回 false（隐藏走右键「隐藏桌宠」或
//    closePetWindow，隐藏后 handle 被 onWindowDestroyed 清空，本函数即可重开）。
// 返回 true = 已按 rt.config（皮肤/位置/气泡数据源桥原样）重新入队开窗请求
bool showPetWindow();

// 关闭桌宠窗口（PET-CLOSE，设置页模块开关「桌宠」取消勾选 → 立即隐藏；
// 经 module_pet_bridge.h 的声明桥调用）。关闭路径与右键「隐藏桌宠」同一条
//（requestWindowClose → 主循环销毁 → onWindowDestroyed 清句柄）。
// 返回 true = 曾有窗口/可见意图并已置为关闭；false = 本来就关着（幂等）。
// 只改「可见意图」，**不动装配标志**——重新勾选后 showPetWindow 仍能原位重开
bool closePetWindow();

// 桌宠窗口可见意图（PET-CLOSE）：startPetWindow / showPetWindow 置 true，
// closePetWindow 置 false。无头（handle 恒 nullptr）与窗口已关时本标志是
// 「桌宠该不该显示」的唯一可读真值——设置页模块开关的即时性断言与单测读它
bool petWindowRequested();

// 换肤（M2）：校验 skinName 是 discoverSkins() 发现到的皮肤后写盘并热切换
//（关旧窗 → 按新皮肤重开，位置沿用当前窗口）；未知名字拒绝不动。
// 窗口未开时只持久化（下次启动生效）
void applyPetSkin(const std::string& skinName);

// 位置重置（M2）：持久化坐标清回 -1（未设置），窗口开着则立刻移回
// 主显示器工作区右下角默认位
void resetPetPosition();

// 审计 [5][36] 设置页桥（只读）：当前皮肤名，读 pet_config.json
// （applyPetSkin 双写运行时 + 盘，盘即真值；与设置页「初值读 pet_config」
// 原语义一致）。标量返回值——module_settings 侧只 extern 声明该自由函数
// 即可调用（不 include 本头、不 link 本模块），避免共享 PetConfig 结构体
// 布局（跨 TU 重定义是 ODR 风险）。
std::string currentSkinName();

// ---- 显示大小三档（2026-09-30 用户需求；设置页经 module_pet_bridge.h
// 消费，声明/签名哨兵同 currentSkinName 先例）----

// 当前显示大小档位下标（0=小 0.75× / 1=中 1.0× / 2=大 1.5×），读
// pet_config.json（非法值已由 sanitize 收敛默认档）
int currentPetScaleIndex();

// 三档文案（与档位下标一一对应；单源 = pet_config.h kPetScalePresetLabels，
// 设置页下拉 items 直接消费，不自带一份防漂移）
std::vector<std::string> petScalePresetLabels();

// 显示大小切档（设置页回传下标；越界静默拒绝不动）：写盘并热切换——窗口
// 开着即关旧开新（尺寸 = 帧尺寸 × 档位缩放 + 气泡让位条，位置沿用并重新
// 钳制），窗口未开时只持久化（下次启动生效）
void applyPetScaleIndex(int presetIndex);

} // namespace module::pet
