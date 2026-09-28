// src/module_pet/pet_window.cpp
// 桌宠窗口：无边框/置顶/透明/不抢焦点/不进任务栏的独立子窗口
//（EUI-NEO G1-G5 扩展，见 ../EUI-NEO docs/DSL.md「子窗口配置」）。
// M2 状态机（设计 §4.4/§4.5）：Idle（眨眼循环）→ 点击 → Poked（播一遍）；
// 无交互 5min → Sleep（闭眼+Zzz）；代理活动流 → Typing（打字帧）。
// 气泡对话（点击/随机闲聊，短语表随皮肤）；右键菜单（换肤子组/位置重置/
// 隐藏/退出——退出为二段确认，审计 [25]）；拖拽移动窗口（onDrag + setWindowPos，结束持久化）。
// 代理联动读 core::proxy_stats 共享统计（module_pet → core 合法依赖，
// 不碰 module_claude_proxy）
#include "module_pet/pet_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <utility>

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/paths.h"
#include "core/platform/context_menu.h"
#include "core/platform/power_events.h"
#include "core/proxy_stats.h"
#include "module_pet_bridge.h"
#include "module_pet/pet_bubble.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"

#include <components/styledmenu.h>

#include <eui/app.h>
#include <eui/dsl.h>
#include <eui/dsl_app.h>

namespace module::pet {

namespace {

// 桌宠窗口的进程级运行时（单窗口；handle 由 onWindowCreated 回填）
struct PetRuntime {
    PetSkin skin;
    core::window::Handle handle = nullptr;
    PetWindowConfig config;
    std::vector<std::string> skins;  // 发现到的皮肤（换肤菜单/设置页消费）
    // PAB-13：本会话 main 是否装配过桌宠（startPetWindow 入口置 true）——
    // 设置页「显示桌宠」按钮据此区分「模块开关关 → 拒绝凭空开窗」与
    // 「隐藏后再显示」
    bool windowRequested = false;
    // M2 Sleep 零渲染：系统挂起置位 / 唤醒清零（回调在主线程，compose 同线程
    // 读，无并发，普通 bool 即可）
    bool renderPaused = false;
};

PetRuntime& runtime() {
    static PetRuntime instance;
    return instance;
}

// 气泡短语的随机源（PAB-12）：进程级 mt19937 单例，ctor 用 random_device
// 播种（std::rand 的全局序列跨进程每次相同 = 每次启动同一套短语顺序）。
// 只被渲染线程（onTimer 回调）与单测调用，无并发保护需求
std::mt19937& phraseRng() {
    static std::mt19937 engine(std::random_device{}());
    return engine;
}

double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 默认位：主显示器工作区右下角（留 48px 边距）；查不到工作区时屏内兜底
std::pair<int, int> defaultPetPos(int frameW, int frameH) {
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);
    if (workW > 0 && workH > 0) {
        return {workX + workW - frameW - 48, workY + workH - frameH - 48};
    }
    return {100, 100};
}

// 枚举当前全部显示器工作区（M2 多显示器）；枚举为空（无头/早于 glfwInit/
// 无显示器）时回退主屏单元素——也拿不到则空 vector，clampPetPosToMonitors
// 原样返回，「无工作区信息时不动」的兜底语义保持
std::vector<PetWorkArea> currentMonitorWorkAreas() {
    std::vector<PetWorkArea> areas;
    for (const core::window::MonitorWorkArea& m : core::window::getMonitorWorkAreas()) {
        areas.push_back(PetWorkArea{m.x, m.y, m.width, m.height});
    }
    if (areas.empty()) {
        int workX = 0;
        int workY = 0;
        int workW = 0;
        int workH = 0;
        core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);
        if (workW > 0 && workH > 0) {
            areas.push_back(PetWorkArea{workX, workY, workW, workH});
        }
    }
    return areas;
}

// 审计 [12] + M2 多显示器：已知坐标钳进「所在/最近显示器」的工作区再落盘。
// M1 只按主屏钳，副屏位置（负坐标或超出主屏边界）落盘被吞回主屏，重启跳回
// 主屏；M2 按位置选对应显示器后副屏负坐标可持久化（pet_config 哨兵同步放宽
// 为仅 -1）。与启动路径共用同一纯函数，两处语义单源
void clampPetPosIntoWorkAreaInPlace(int& x, int& y) {
    const PetSkin& skin = runtime().skin;
    const auto [cx, cy] = clampPetPosToMonitors(x, y, currentMonitorWorkAreas(),
                                                skin.frameWidth, skin.frameHeight);
    // 哨兵冲突避让：钳后恰好 -1（如副屏右缘紧贴主屏原点 x=0 的左邻位）会被
    // 存取两侧误读为「未设置」→ 外挪 1px（肉眼无感），-1 从此唯一表示未设置
    x = (cx == -1) ? -2 : cx;
    y = (cy == -1) ? -2 : cy;
}

// ---- 持久化（读-改-写 petsDir()/pet_config.json，只含 pet 字段；模块隔离，
// 不依赖 module_settings）----

void savePetPosition() {
    if (runtime().handle == nullptr) return;
    int x = 0;
    int y = 0;
    core::window::getWindowPos(runtime().handle, x, y);
    // 审计 [12]：钳进工作区再落盘——拖到左/上边缘外（负坐标）直接写盘会被
    // -1「未设置」哨兵吞掉，重启跳回默认位（钳制与启动路径同一规则）
    clampPetPosIntoWorkAreaInPlace(x, y);
    PetConfigStore store(petConfigFile());
    PetConfig cfg = store.load();
    cfg.x = x;
    cfg.y = y;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠位置持久化失败（不影响本次会话）");
    }
}

// ---- compose 状态（ui.state 挂在子窗口自己的 Ui 实例上）----

struct PetUiState {
    PetAnimation anim;
    PetBubble bubble;              // 气泡对话（M2 设计 §4.5）
    bool initialized = false;
    bool wasDragged = false;   // 拖拽结束抑制误触 poke（设计 §4.4 Dragged 语义）
    double lastInteractionMs = 0.0;  // 最近交互时刻（入睡判定用）
    double lastTickMs = 0.0;         // 上次 onTimer 时刻（气泡倒计时 dt 用）
    PetDragAnchor dragAnchor;        // 拖拽锚点（见 pet_window.h，抖动修复）
};

// ---- styled 右键菜单（菜单美化 2026-09-23）----
// FontAwesome 码点（fa-solid）：换肤=palette / 位置重置=crosshairs /
// 隐藏=eye-slash / 退出=power-off
constexpr int kIconSkin = 0xF53F;
constexpr int kIconReset = 0xF05B;
constexpr int kIconHide = 0xF070;
constexpr int kIconExit = 0xF011;
constexpr int kIconConfirm = 0xF071;   // 退出确认：triangle-exclamation
constexpr int kIconCancel = 0xF00D;    // 取消：xmark

// 菜单位置 = 鼠标点击点屏幕坐标：窗口原点 + 窗口内光标（与拖拽 petDragMove
// 同一坐标系换算，均为 GLFW 屏幕坐标实时值）。菜单以点击点为左上角向下
// 展开；越出工作区由 styledMenuClampPosition 兜底（贴右 / 向上翻转贴光标
// 上沿 / 贴顶）。
// 根因：原实现 y 只取窗口顶（winY）并整体抬升「菜单窗实际高」，菜单永远
// 钉在宠物窗顶上方、与点击点无关——点位偏离越大菜单离鼠标越远（"太高距
// 离大"），且实现里 windowScaleForPoint 是给 showStyledMenu 内部折算用的，
// 位置本身必须是物理像素，叠加菜单高再抛回钳制是双重处理。
// 2026-09-28 用户要求「菜单紧贴鼠标点击点」，替代 2026-09-24 的锚窗顶方案。
std::pair<float, float> petMenuAnchorPos() {
    double cursorX = 0.0;
    double cursorY = 0.0;
    core::window::getCursorPosition(runtime().handle, cursorX, cursorY);
    int winX = 0;
    int winY = 0;
    core::window::getWindowPos(runtime().handle, winX, winY);
    return {static_cast<float>(winX) + static_cast<float>(cursorX),
            static_cast<float>(winY) + static_cast<float>(cursorY)};
}

components::StyledMenuStyle petMenuStyle() {
    return components::StyledMenuStyle(
        core::theme::componentTokens(core::theme::current()));
}

// 选中处理（styled 回调与原生降级共用同一选择数学，path 语义一致）
void executePetMenuSelection(const std::vector<int>& path);

// 退出二段确认（审计 [25] 语义保持：仅「确认退出」触发 requestExit）：
// styled 优先，失败降级原生就地弹出（dialog 组件在 sprite 小窗被裁剪的
// 先例注释依然成立——独立菜单窗不受裁剪，styled 可用时确认走独立窗）
void showPetExitConfirm() {
    const std::vector<PetExitConfirmItem> confirmData = petExitConfirmItems();
    // 确认菜单与主菜单同锚点（鼠标点击点）
    const std::pair<float, float> pos = petMenuAnchorPos();

    std::vector<components::StyledMenuItem> styled;
    for (const PetExitConfirmItem& ci : confirmData) {
        components::StyledMenuItem item;
        item.text = ci.text;
        item.disabled = ci.disabled;
        if (ci.text == "确认退出") {
            item.iconCodepoint = kIconConfirm;
        } else if (ci.text == "取消") {
            item.iconCodepoint = kIconCancel;
        }
        styled.push_back(std::move(item));
    }
    // 退出确认收窄（2026-09-28 用户反馈：两行短项撑 220 默认宽太宽）：
    // 面板宽下限归零 → 宽度 = 最长行（文本+图标+边距）自适应，不超内容
    components::StyledMenuStyle style = petMenuStyle();
    style.panelWidth = 0.0f;
    if (components::showStyledMenu("pet-exit-confirm", std::move(styled), style,
                                   pos.first, pos.second,
                                   [](const std::vector<int>& path) {
                                       if (petExitConfirmChosen(path)) {
                                           app::requestExit();  // 与托盘 Exit 同一退出路径
                                       }
                                   })) {
        return;
    }

    // 原生降级（winfx ③ 原路径）
    std::vector<core::platform::ContextMenuItem> items;
    for (const PetExitConfirmItem& ci : confirmData) {
        if (ci.text == "-") {
            items.push_back(core::platform::ContextMenuItem{"-"});
            continue;
        }
        core::platform::ContextMenuItem item;
        item.text = ci.text;
        item.disabled = ci.disabled;
        items.push_back(std::move(item));
    }
    if (runtime().handle == nullptr || items.empty()) {
        return;
    }
    const core::window::NativeWindowInfo info =
        core::window::nativeWindowInfo(runtime().handle);
    if (info.platformWindow == nullptr) {
        return;
    }
    const std::optional<std::vector<int>> chosen =
        core::platform::showContextMenu(info.platformWindow, items);
    if (chosen.has_value() && !chosen->empty() && petExitConfirmChosen(*chosen)) {
        app::requestExit();
    }
}

void executePetMenuSelection(const std::vector<int>& path) {
    int skinIndex = -1;
    const PetMenuAction action =
        petMenuActionForPath(path, runtime().skins.size(), skinIndex);
    switch (action) {
        case PetMenuAction::Skin:
            if (skinIndex >= 0 &&
                static_cast<size_t>(skinIndex) < runtime().skins.size()) {
                applyPetSkin(runtime().skins[static_cast<size_t>(skinIndex)]);
            }
            break;
        case PetMenuAction::ResetPosition:
            resetPetPosition();
            break;
        case PetMenuAction::Hide:
            savePetPosition();
            if (runtime().handle != nullptr) {
                core::window::requestWindowClose(runtime().handle);
            }
            break;
        case PetMenuAction::Exit:
            showPetExitConfirm();
            break;
        case PetMenuAction::None:
        default:
            break;
    }
}

// styled 菜单构建（PetMenuItemData → StyledMenuItem，图标按语义注入）
// 换肤子组（有 children）设 collapsedByDefault=true：默认只显"换肤 ▸"，
// 点组头/chevron 展开才列皮肤（不破坏其他菜单——opt-in 仅此组折叠）
bool showPetStyledMenu(const std::vector<PetMenuItemData>& menu, float x, float y) {
    std::vector<components::StyledMenuItem> items;
    for (const PetMenuItemData& data : menu) {
        components::StyledMenuItem item;
        item.text = data.text;
        item.disabled = data.disabled;
        item.iconCodepoint = data.iconCodepoint;
        if (!data.children.empty()) {
            item.collapsedByDefault = true;  // 子项组默认折叠
        }
        for (const PetMenuItemData& child : data.children) {
            components::StyledMenuItem childItem;
            childItem.text = child.text;
            childItem.disabled = child.disabled;
            item.children.push_back(std::move(childItem));
        }
        items.push_back(std::move(item));
    }
    return components::showStyledMenu("pet", std::move(items), petMenuStyle(), x, y,
                                      [](const std::vector<int>& path) {
                                          executePetMenuSelection(path);
                                      });
}

void petCompose(eui::Ui& ui, const eui::Screen& screen) {
    // M2 Sleep 零渲染：系统挂起期间整帧不进 compose——不重建成命令、
    // 不推动画、不跑 onTimer、不计气泡倒计时（挂起的窗口本就无屏可看），
    // 唤醒回调清标志后自动恢复
    if (runtime().renderPaused) {
        return;
    }
    PetUiState& state = ui.state<PetUiState>("pet.state");
    if (!state.initialized) {
        state.initialized = true;
        state.lastInteractionMs = steadyMs();
        state.lastTickMs = state.lastInteractionMs;
    }
    const PetSkin& skin = runtime().skin;

    ui.stack("pet.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 窗口尺寸 = 帧紧包围盒（设计 §4.2）；MVP 命中 = 整窗（O1 逐像素 alpha 是 M3）
            ui.image("pet.sprite")
                .source(state.anim.currentPath(skin).string())
                .size(screen.width, screen.height)
                // 帧推进：onTimer 周期 = 1/frameRate（等待期主循环低功耗睡眠）。
                // 顺序即优先级：代理流式 > 入睡 > 帧推进
                .onTimer(1.0f / static_cast<float>(skin.frameRate),
                         [&state] {
                             const PetSkin& s = runtime().skin;
                             const double now = steadyMs();
                             const double dt =
                                 std::max(0.0, (now - state.lastTickMs) / 1000.0);
                             state.lastTickMs = now;
                             // 代理流式联动（M2 §4.5③）：读 core 共享统计
                             const bool streaming =
                                 core::proxy_stats::snapshot().activeStreams > 0;
                             state.anim.setStreaming(streaming, s);
                             state.anim.considerSleep(
                                 (now - state.lastInteractionMs) / 1000.0, s);
                             state.anim.tick(s);
                             state.bubble.tick(dt);
                             // 三级内容源调度（M2 §4.5，审计 [18]）：本轮弹哪一级 /
                             // 停多久由 pet_bubble.h 的 petBubbleSchedule 纯函数决定
                             //（PAB-05：从本闭包抽出以便单测，节拍与命中口径见该头，
                             // 注释与代码数字同源）。数据源都经 main 注入的 app 级只读桥
                             //（pet 不 include claude_proxy/home，隔离不破）；③为 take
                             // 语义——只在调度命中数据槽位时才读桥，未命中不消费事件；
                             // 桥都无数据（空串）降级①静态语料，不显示死数据（验收条款）
                             const double roll =
                                 std::uniform_real_distribution<double>(0.0, 1.0)(
                                     phraseRng());
                             const PetBubblePlan bubblePlan = petBubbleSchedule(
                                 dt, state.bubble.visible,
                                 state.anim.state == PetAnimState::Idle, roll);
                             if (bubblePlan.source == PetBubbleSource::Data) {
                                 const auto [dataText, dataSec] = petDataBubbleText(
                                     runtime().config.getProxyEvent,
                                     runtime().config.getOverdueCount);
                                 state.bubble.show(
                                     dataText.empty() ? pickPetPhrase(s) : dataText,
                                     dataText.empty() ? bubblePlan.holdSec : dataSec);
                             } else if (bubblePlan.source == PetBubbleSource::Static) {
                                 state.bubble.show(pickPetPhrase(s),
                                                   bubblePlan.holdSec);
                             }
                         })
                .onClick([&state]() {
                    state.lastInteractionMs = steadyMs();
                    if (!state.wasDragged) {
                        state.anim.poke(runtime().skin);
                    }
                    state.wasDragged = false;
                    // 点击说一句话（M2 §4.5：交互触发的气泡）
                    state.bubble.show(pickPetPhrase(runtime().skin), 3.0);
                })
                .onDrag([&state](const core::dsl::DragEvent& event) {
                    state.lastInteractionMs = steadyMs();  // 拖拽也是交互
                    // 拖过阈值 → 标记（释放时不触发 poke）
                    state.wasDragged =
                        std::abs(event.totalX) + std::abs(event.totalY) > 4.0;
                    if (runtime().handle == nullptr) return;
                    // 异常取消路径防御：拖拽键已不在按住状态 → 重锚定（防跳变）
                    if (!event.buttons.contains(event.button)) {
                        state.dragAnchor.valid = false;
                    }
                    // 锚点跟随（pet_window.h）：不能用 event.delta 累加——
                    // delta 是窗口内相对坐标，窗口自身移动会漂移参考系（抖动）
                    double cursorX = 0.0;
                    double cursorY = 0.0;
                    core::window::getCursorPosition(runtime().handle, cursorX,
                                                    cursorY);
                    int winX = 0;
                    int winY = 0;
                    core::window::getWindowPos(runtime().handle, winX, winY);
                    const auto [x, y] = petDragMove(state.dragAnchor, winX, winY,
                                                    cursorX, cursorY);
                    core::window::setWindowPos(runtime().handle, x, y);
                })
                .onRelease([&state](const eui::PointerEvent&, const eui::Rect&) {
                    petDragEnd(state.dragAnchor);  // 释放/取消 → 清锚点
                    savePetPosition();  // 拖拽结束才写盘（设计 §4.6）
                })
                .onContextMenu([](const eui::PointerEvent&, const eui::Rect&) {
                    // 菜单美化（2026-09-23）：优先 styled 独立菜单窗（圆角/主题/
                    // FA 图标，app::openWindow 弹窗不受 128px sprite 窗 bounds
                    // 裁剪——winfx ③ 换原生正是为躲裁剪，独立窗从根上解决）。
                    // styled 构建失败 → 降级原生菜单（winfx ③ 路径保留）。
                    // 开菜单前刷新皮肤清单（新放进目录的皮肤立即可选）
                    runtime().skins = discoverSkins();
                    if (runtime().handle == nullptr) return;

                    const std::vector<PetMenuItemData> menu =
                        petContextMenuItems(runtime().skins);
                    const std::pair<float, float> pos = petMenuAnchorPos();
                    if (showPetStyledMenu(menu, pos.first, pos.second)) {
                        return;  // 已入队开窗，选择经 executePetMenuSelection 回调
                    }

                    // ---- 原生降级：Win32 TrackPopupMenuEx 光标处弹出，同步模态 ----
                    std::vector<core::platform::ContextMenuItem> items;
                    for (const PetMenuItemData& data : menu) {
                        core::platform::ContextMenuItem item;
                        item.text = data.text;
                        item.disabled = data.disabled;
                        for (const PetMenuItemData& child : data.children) {
                            core::platform::ContextMenuItem childItem;
                            childItem.text = child.text;
                            childItem.disabled = child.disabled;
                            item.children.push_back(std::move(childItem));
                        }
                        items.push_back(std::move(item));
                    }
                    if (items.empty()) return;
                    const core::window::NativeWindowInfo info =
                        core::window::nativeWindowInfo(runtime().handle);
                    if (info.platformWindow == nullptr) return;  // 无原生句柄（stub 平台）
                    const std::optional<std::vector<int>> selection =
                        core::platform::showContextMenu(info.platformWindow, items);
                    if (!selection.has_value() || selection->empty()) return;  // 不支持/取消
                    executePetMenuSelection(*selection);
                })
                .build();

            // 气泡对话（M2 设计 §4.5）：宠物窗内自绘圆角矩形 + 小三角 + text，
            // 挂在头顶，超时自动消失（不用 toast——位置不可控）
            if (state.bubble.visible) {
                const core::theme::Tokens& t = core::theme::current();
                // 气泡衬深色底（kBubbleBg 深浅主题都保持深帽），白字/白描边
                // 取自 theme onPrimary（两档主题 onPrimary 都是「彩色面文字」
                // 反面色），不再硬编码 1,1,1 白色（2026-09-28 无硬编码白）
                const core::Color kBubbleBg{0.16f, 0.15f, 0.13f, 0.92f};
                const core::Color kBubbleBorder(t.colors.onPrimary.r,
                                                t.colors.onPrimary.g,
                                                t.colors.onPrimary.b, 0.22f);
                ui.rect("pet.bubble.bg")
                    .position(2.0f, 0.0f)
                    .size(screen.width - 4.0f, 28.0f)
                    .color(kBubbleBg)
                    .radius(8.0f)
                    .border(1.0f, kBubbleBorder)
                    .build();
                ui.rect("pet.bubble.tail")
                    .position(screen.width * 0.5f - 3.0f, 25.0f)
                    .size(6.0f, 6.0f)
                    .color(kBubbleBg)
                    .rotate(0.785f)  // 45° 小方块当三角尾巴
                    .build();
                ui.text("pet.bubble.text")
                    .position(6.0f, 0.0f)
                    .size(screen.width - 12.0f, 28.0f)
                    .text(state.bubble.text)
                    .fontSize(11.0f)
                    .lineHeight(28.0f)
                    .color(core::Color(t.colors.onPrimary.r, t.colors.onPrimary.g,
                                       t.colors.onPrimary.b, 0.95f))
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            }
        })
        .build();

}

// 解析皮肤目录：按候选根找 <skinName>；找不到（或名字是默认皮肤）落到
// petsDir() 下生成占位皮肤
std::filesystem::path resolveSkinDir(const std::string& skinName) {
    for (const std::filesystem::path& root : skinRootCandidates()) {
        const std::filesystem::path dir = root / skinName;
        std::error_code ec;
        if (std::filesystem::exists(dir / "pet.json", ec) && !ec) {
            return dir;
        }
    }
    return core::paths::petsDir() / skinName;
}

} // namespace

// 随机挑一句气泡短语（声明见 pet_window.h；短语表随皮肤，见 pet_skin.h
// phraseTable）。PAB-12：改用 <random>（phraseRng = mt19937 + random_device
// 播种），替换 std::rand 的进程固定序列
std::string pickPetPhrase(const PetSkin& skin) {
    const std::vector<std::string>& table = phraseTable(skin);
    return table[std::uniform_int_distribution<size_t>(0, table.size() - 1)(
        phraseRng())];
}

std::pair<int, int> petDragMove(PetDragAnchor& anchor, int windowX, int windowY,
                                double cursorX, double cursorY) {
    // 光标屏幕位 = 窗口原点 + 窗口内光标（两参同为 GLFW 屏幕坐标系实时值，
    // setWindowPos 造成的窗口移动立刻反映——这就是 delta 反馈回路的解法）
    const double screenX = static_cast<double>(windowX) + cursorX;
    const double screenY = static_cast<double>(windowY) + cursorY;
    if (!anchor.valid) {
        anchor.valid = true;
        anchor.offsetX = static_cast<double>(windowX) - screenX;
        anchor.offsetY = static_cast<double>(windowY) - screenY;
    }
    return {static_cast<int>(screenX + anchor.offsetX + 0.5),
            static_cast<int>(screenY + anchor.offsetY + 0.5)};
}

void petDragEnd(PetDragAnchor& anchor) {
    anchor.valid = false;
    anchor.offsetX = 0.0;
    anchor.offsetY = 0.0;
}

std::pair<int, int> clampPetPosToWorkArea(int x, int y, int workX, int workY,
                                          int workW, int workH, int frameW,
                                          int frameH) {
    if (workW <= 0 || workH <= 0) {
        return {x, y};  // 工作区不可用（无头/早于 glfwInit）：不动
    }
    return {std::clamp(x, workX, workX + std::max(0, workW - frameW)),
            std::clamp(y, workY, workY + std::max(0, workH - frameH))};
}

namespace {

// 点到工作区矩形（闭包 [lo, lo+extent-1]）的轴距离，超出才计
long long petAxisDist(int p, int lo, int extent) {
    if (static_cast<long long>(p) < lo) {
        return static_cast<long long>(lo) - p;
    }
    const long long hi = static_cast<long long>(lo) + extent - 1;
    if (static_cast<long long>(p) > hi) {
        return static_cast<long long>(p) - hi;
    }
    return 0;
}

} // namespace

std::pair<int, int> clampPetPosToMonitors(int x, int y,
                                          const std::vector<PetWorkArea>& areas,
                                          int frameW, int frameH) {
    // 选屏：某工作区含住点（半开 [x, x+w)）取之（多块含住取枚举序首个，
    // 实际布局不重叠）；都不含则取点-矩形距离平方最小的一块（= Win32
    // MONITOR_DEFAULTTONEAREST，处理拖过边缘/两屏之间），并列取先枚举者
    int best = -1;
    long long bestDist = 0;
    for (size_t i = 0; i < areas.size(); ++i) {
        const PetWorkArea& a = areas[i];
        if (a.width <= 0 || a.height <= 0) {
            continue;  // 无效块不参与（防把钳界算成 [x, x)）
        }
        if (x >= a.x && x < a.x + a.width && y >= a.y && y < a.y + a.height) {
            best = static_cast<int>(i);
            break;
        }
        const long long dx = petAxisDist(x, a.x, a.width);
        const long long dy = petAxisDist(y, a.y, a.height);
        const long long d = dx * dx + dy * dy;
        if (best < 0 || d < bestDist) {
            best = static_cast<int>(i);
            bestDist = d;
        }
    }
    if (best < 0) {
        return {x, y};  // 无任何有效工作区（无头兜底）：不动
    }
    const PetWorkArea& a = areas[static_cast<size_t>(best)];
    return clampPetPosToWorkArea(x, y, a.x, a.y, a.width, a.height, frameW,
                                 frameH);
}

void setPetRenderPaused(bool paused) { runtime().renderPaused = paused; }

bool isPetRenderPaused() { return runtime().renderPaused; }

void startPetWindow(const PetWindowConfig& config) {
    PetRuntime& rt = runtime();
    rt.config = config;
    // PAB-13：本会话已装配桌宠——设置页「显示桌宠」据此区分「模块开关关
    // （从未装配 → 拒绝凭空开窗）」与「隐藏后再显示」
    rt.windowRequested = true;

    // 内置皮肤全集落盘（幂等；用户替换过的原样保留）——换肤菜单/设置页
    // 下拉靠 discoverSkins 发现，皮肤须先存在于 petsDir
    (void)ensureBuiltinSkins(core::paths::petsDir());

    // 皮肤：候选根找 → 找不到回退默认皮肤 → 生成/加载占位皮肤
    std::string skinName =
        config.skinName.empty() ? kDefaultSkinName : config.skinName;
    auto skin = loadSkin(resolveSkinDir(skinName));
    if (skin.isFail() && skinName != kDefaultSkinName) {
        LOG_WARN("桌宠皮肤 \"" + skinName + "\" 加载失败（" + skin.errorMessage() +
                 "），回退默认皮肤");
        skinName = kDefaultSkinName;
        skin = loadSkin(resolveSkinDir(skinName));
    }
    if (skin.isFail()) {
        skin = ensureDefaultSkin(core::paths::petsDir() / kDefaultSkinName);
    }
    if (skin.isFail()) {
        LOG_ERROR("桌宠皮肤不可用（" + skin.errorMessage() + "），不开窗口");
        return;
    }
    rt.skin = std::move(skin.value());
    rt.config.skinName = skinName;
    rt.skins = discoverSkins();

    // M2 Sleep 零渲染：注册进程级电源回调（换肤重开重复注册 = 单实例覆盖，
    // 幂等；回调只动 renderPaused 标志，不碰窗口句柄，窗口销毁后触发也安全）。
    // Windows 由 EUI-NEO 主循环 WndProc 链上抛；其他平台 stub 永不触发
    core::platform::setSystemPowerHandler([](core::platform::SystemPowerEvent ev) {
        setPetRenderPaused(ev == core::platform::SystemPowerEvent::Suspending);
    });

    // 位置：未设置 → 主显示器工作区右下角；已知位置钳制进所在/最近显示器的
    // 工作区（设计 §4.6；M2 多显示器——M1 只按主屏钳，副屏位置落盘被吞）。
    // 工作区查询失败（无头/早于 glfwInit）时不动已知位置，未设置则给屏内兜底值
    int x = config.x;
    int y = config.y;
    const auto [defX, defY] = defaultPetPos(rt.skin.frameWidth, rt.skin.frameHeight);
    // 单边独立处理 -1 哨兵：只重置未设置的那一边（只存了 x 时 y 不丢）。
    // M2 起哨兵是「== -1」而非「< 0」：其他负值是左/上副屏的合法坐标
    if (x == -1) {
        x = defX;
    }
    if (y == -1) {
        y = defY;
    }
    // 审计 [12]：与落盘路径共用同一钳制纯函数（默认位本就在工作区内，
    // 再钳幂等；无头工作区查不到时原样）
    const auto [cx, cy] = clampPetPosToMonitors(x, y, currentMonitorWorkAreas(),
                                                rt.skin.frameWidth,
                                                rt.skin.frameHeight);
    x = cx;
    y = cy;
    rt.config.x = x;
    rt.config.y = y;

    app::openWindow(app::DslWindowConfig{}
                        .title("DevDesk Pet")
                        .pageId("pet")
                        .windowSize(rt.skin.frameWidth, rt.skin.frameHeight)
                        .windowPosition(x, y)
                        .decorated(false)              // 无边框
                        .alwaysOnTop(true)             // 置顶
                        .resizable(false)
                        .focusOnShow(false)            // 显示不抢焦点
                        .hideFromTaskbar(true)         // 不进任务栏/Alt+Tab
                        .transparentFramebuffer(true)  // sprite 窗逐像素透明
                                                       //（黑底修复：不随全局
                                                       // 窗口效果档位回落）
                        // backdrop 档位显式 None（灰底修复）：不跟随全局磨砂/
                        // 半透档——否则 Acrylic 材质画在透明像素后面，四角透出
                        // 的是材质灰而非桌面（真机采样 #D3D3D3 = 材质色）
                        .windowEffect(core::platform::WindowEffect::None)
                        .ignoreClearColorOverride()    // 全透底不被主题广播冲掉
                        .clearColor({0.0f, 0.0f, 0.0f, 0.0f})
                        .onWindowCreated([](core::window::Handle handle) {
                            runtime().handle = handle;
                        })
                        // 审计 [0][1]（UAF 根治）：任何销毁路径（菜单隐藏 /
                        // Alt+F4 / 应用退出）都先于 glfwDestroyWindow 清空缓存
                        // 句柄——设置页 applyPetSkin/resetPetPosition 与 compose
                        // 各回调都按 handle==nullptr 分支保护。比较句柄是必需的：
                        // 热切换（applyPetSkin）新窗创建早于旧窗销毁（主循环
                        // create→prune 同轮），无条件清空会误清新句柄
                        .onWindowDestroyed([](core::window::Handle destroyed) {
                            if (runtime().handle == destroyed) {
                                runtime().handle = nullptr;
                            }
                        }),
                    petCompose);

    LOG_INFO("桌宠窗口已请求开启（皮肤=" + skinName + "，帧率=" +
             std::to_string(static_cast<int>(rt.skin.frameRate)) +
             "fps，位置=" + std::to_string(x) + "," + std::to_string(y) + "）");
}

// PAB-13：重新显示桌宠窗口（设置页「显示桌宠」按钮，声明见 pet_window.h）
bool showPetWindow() {
    PetRuntime& rt = runtime();
    if (!rt.windowRequested) {
        return false;  // 本会话从未装配（模块开关关）——不凭空开窗
    }
    if (rt.handle != nullptr) {
        return false;  // 窗口已开着（隐藏后 handle 被 onWindowDestroyed 清空）
    }
    startPetWindow(rt.config);  // 皮肤/位置/气泡数据源桥沿用上次装配
    return true;
}

void applyPetSkin(const std::string& skinName) {
    PetRuntime& rt = runtime();
    // 只接受发现到的皮肤名（名字会拼进文件系统路径，防手改配置注入）
    const std::vector<std::string> skins = discoverSkins();
    if (std::find(skins.begin(), skins.end(), skinName) == skins.end()) {
        LOG_WARN("桌宠换肤被拒绝：皮肤 \"" + skinName + "\" 未在皮肤目录中发现");
        return;
    }
    PetConfigStore store(petConfigFile());
    PetConfig cfg = store.load();
    cfg.skinName = skinName;
    if (rt.handle != nullptr) {
        // 记住当前窗口位置（换肤后帧尺寸可能不同，startPetWindow 会重新钳制）
        int x = 0;
        int y = 0;
        core::window::getWindowPos(rt.handle, x, y);
        // 审计 [12]：同 savePetPosition，负坐标钳进工作区再落盘防哨兵吞掉
        clampPetPosIntoWorkAreaInPlace(x, y);
        cfg.x = x;
        cfg.y = y;
    }
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠皮肤持久化失败（不影响本次会话）");
    }
    rt.skins = skins;
    const bool hotSwap = rt.handle != nullptr;
    if (hotSwap) {
        // 热切换：关旧窗 → 按新皮肤原位重开（compose 状态随旧窗销毁重建）
        core::window::requestWindowClose(rt.handle);
        rt.handle = nullptr;  // 等新窗 onWindowCreated 回填
        startPetWindow(
            PetWindowConfig{skinName, cfg.x, cfg.y,
                            rt.config.getOverdueCount,
                            rt.config.getProxyEvent});
    }
    LOG_INFO("桌宠换肤 → " + skinName + (hotSwap ? "（已热切换）" : "（下次启动生效）"));
}

void resetPetPosition() {
    PetRuntime& rt = runtime();
    // 坐标清回 -1（未设置哨兵）：显示器布局变化后下次启动重算默认位
    PetConfigStore store(petConfigFile());
    PetConfig cfg = store.load();
    cfg.x = -1;
    cfg.y = -1;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠位置重置持久化失败（不影响本次会话）");
    }
    if (rt.handle == nullptr) return;
    const auto [x, y] = defaultPetPos(rt.skin.frameWidth, rt.skin.frameHeight);
    core::window::setWindowPos(rt.handle, x, y);
    rt.config.x = x;
    rt.config.y = y;
    LOG_INFO("桌宠位置已重置（" + std::to_string(x) + "," + std::to_string(y) + "）");
}

// 审计 [5][36]：设置页桌宠卡只读桥（契约见 pet_window.h 同名声明）。
// 读 pet_config.json 而非 rt.config——设置页可能在窗口未开时读取（桌宠关/
// 隐藏），且 apply 双写保证盘与运行时一致
std::string currentSkinName() {
    return PetConfigStore(petConfigFile()).load().skinName;
}


// ---- 退出二次确认（审计 [25]，纯数据层，声明见 pet_window.h）----
// 下标语义与 petExitConfirmChosen 耦合：0=说明（disabled）1=分隔线
// 2=确认退出 3=取消。改结构必须同步改 petExitConfirmChosen 与单测
std::vector<PetExitConfirmItem> petExitConfirmItems() {
    return {
        {"将退出 DevDesk：断开代理、丢失未保存弹窗状态，确定？", true},
        {"-"},
        {"确认退出"},
        {"取消"},
    };
}

bool petExitConfirmChosen(const std::vector<int>& path) {
    return path.size() == 1 && path[0] == 2;
}

// ---- 右键菜单结构（菜单美化 2026-09-23，纯数据层，声明见 pet_window.h）----
// 顺序 = path 语义基础：（有皮肤时）换肤子组 / 位置重置 / 隐藏 / 分隔线 /
// 退出。图标码点只被 styled 菜单消费，原生降级忽略。
std::vector<PetMenuItemData> petContextMenuItems(const std::vector<std::string>& skins) {
    std::vector<PetMenuItemData> items;
    if (!skins.empty()) {
        PetMenuItemData skinMenu;
        skinMenu.text = "换肤";
        skinMenu.iconCodepoint = kIconSkin;
        for (const std::string& name : skins) {
            skinMenu.children.push_back({name, 0, false, {}});
        }
        items.push_back(std::move(skinMenu));
    }
    items.push_back({"位置重置（回主屏右下角）", kIconReset, false, {}});
    items.push_back({"隐藏桌宠（本次会话；重启后恢复）", kIconHide, false, {}});
    items.push_back({"-", 0, false, {}});
    items.push_back({"退出 DevDesk", kIconExit, false, {}});
    return items;
}

PetMenuAction petMenuActionForPath(const std::vector<int>& path, size_t skinCount,
                                   int& skinIndex) {
    skinIndex = -1;
    const bool hasSkinMenu = skinCount > 0;
    // 子项组 path = {0, j}（换肤组居首项）
    if (path.size() == 2) {
        if (hasSkinMenu && path[0] == 0 && path[1] >= 0 &&
            static_cast<size_t>(path[1]) < skinCount) {
            skinIndex = path[1];
            return PetMenuAction::Skin;
        }
        return PetMenuAction::None;
    }
    if (path.size() != 1) {
        return PetMenuAction::None;
    }
    // 顶层下标随换肤子组有无整体偏移（有子组时各 +1）。
    // 退出项在「隐藏」+ 分隔线之后 = idxHide + 2——分隔线不可选但在
    // items 向量占下标（styled/native 两条 path 语义都是向量原始下标）。
    const int idxReset = hasSkinMenu ? 1 : 0;
    const int idxHide = idxReset + 1;
    const int idxExit = idxHide + 2;
    if (path[0] == idxReset) {
        return PetMenuAction::ResetPosition;
    }
    if (path[0] == idxHide) {
        return PetMenuAction::Hide;
    }
    if (path[0] == idxExit) {
        return PetMenuAction::Exit;
    }
    return PetMenuAction::None;  // 换肤组头 / 分隔线 / 未知下标
}

} // namespace module::pet
