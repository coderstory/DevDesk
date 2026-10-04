// src/module_pet/pet_window.cpp
// 桌宠窗口：无边框/置顶/透明/不抢焦点/不进任务栏的独立子窗口。
// 状态机 Idle→点击 Poked→无交互 5min Sleep→代理活动流 Typing；气泡对话随皮肤短语表；
// 右键菜单（换肤/位置重置/隐藏/退出，退出二段确认）；拖拽 onDrag + setWindowPos 持久化。
// 代理联动只读 core::proxy_stats（module_pet → core 合法依赖，不碰 module_claude_proxy）。
#include "module_pet/pet_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <ratio>
#include <system_error>
#include <utility>

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/database.h"   // core::appDatabase()（pet_config 落点）
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
// 气泡宽自适应：TextPrimitive::measureTextSize 测文本内在宽（显式依赖）
#include <core/render/text.h>

namespace module::pet {

namespace {

// 桌宠窗口的进程级运行时（单窗口；handle 由 onWindowCreated 回填）
struct PetRuntime {
    PetSkin skin;
    core::window::Handle handle = nullptr;
    PetWindowConfig config;
    std::vector<std::string> skins;  // 发现到的皮肤（换肤菜单/设置页消费）
    // 本会话 main 是否装配过桌宠（startPetWindow 置 true）。只置不清：关窗 ≠ 本会话
    // 没装配过，重新开启仍能凭本标志原位重开。
    bool assembled = false;
    // 窗口可见意图：startPetWindow/showPetWindow 置 true，closePetWindow 置 false。
    // 无头/关窗后 handle 恒为 nullptr，本标志是「桌宠该不该显示」的唯一真值。
    bool windowRequested = false;
    // 系统挂起置位 / 唤醒清零（回调与 compose 同线程，普通 bool 即可）
    bool renderPaused = false;
};

PetRuntime& runtime() {
    static PetRuntime instance;
    return instance;
}

// 气泡短语随机源：进程级 mt19937 单例，random_device 播种（std::rand 的全局序列
// 跨进程每次相同）。只被渲染线程与单测调用，无并发保护需求。
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

// 枚举当前全部显示器工作区；为空时回退主屏单元素，也拿不到则空 vector（不动）。
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

// 已知坐标钳进「所在/最近显示器」的工作区再落盘（与启动路径共用同一纯函数）。
// 尺寸传整窗（缩放后帧尺寸 + 气泡让位条），钳制把整窗关进工作区，气泡条上沿才不会
// 越过工作区上缘。多显示器：副屏负坐标可持久化（pet_config 哨兵仅 -1）。
void clampPetPosIntoWorkAreaInPlace(int& x, int& y) {
    const PetSkin& skin = runtime().skin;
    const auto [fw, fh] = scaledFrameSize(skin.frameWidth, skin.frameHeight,
                                          runtime().config.scale);
    const auto [cx, cy] = clampPetPosToMonitors(
        x, y, currentMonitorWorkAreas(), fw, petWindowHeight(fh));
    // 哨兵冲突避让：钳后恰好 -1 会被误读为「未设置」→ 外挪 1px，-1 唯一表示未设置
    x = (cx == -1) ? -2 : cx;
    y = (cy == -1) ? -2 : cy;
}

// ---- 持久化（读-改-写 app_config 的 pet_config 键，只含 pet 字段；模块隔离）----

void savePetPosition() {
    if (runtime().handle == nullptr) return;
    int x = 0;
    int y = 0;
    core::window::getWindowPos(runtime().handle, x, y);
    // 钳进工作区再落盘：负坐标直接写盘会被 -1 哨兵吞掉，重启跳回默认位。
    clampPetPosIntoWorkAreaInPlace(x, y);
    PetConfigStore store(::core::appDatabase());
    PetConfig cfg = store.load();
    cfg.x = x;
    cfg.y = y;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠位置持久化失败（不影响本次会话）");
    }
}

struct PetUiState {
    PetAnimation anim;
    PetBubble bubble;              // 气泡对话（M2 设计 §4.5）
    bool initialized = false;
    bool wasDragged = false;   // 拖拽结束抑制误触 poke
    double lastInteractionMs = 0.0;  // 最近交互时刻（入睡判定用）
    double lastTickMs = 0.0;         // 上次 onTimer 时刻（气泡倒计时 dt 用）
    double lastClickMs = 0.0;        // 上次单击时刻（双击开主窗判定用）
    PetDragAnchor dragAnchor;        // 拖拽锚点（见 pet_window.h，抖动修复）
};

// FontAwesome 码点（fa-solid）：换肤 palette / 位置重置 crosshairs / 隐藏 eye-slash /
// 退出 power-off / 确认 triangle-exclamation / 取消 xmark。
constexpr int kIconSkin = 0xF53F;
constexpr int kIconReset = 0xF05B;
constexpr int kIconHide = 0xF070;
constexpr int kIconExit = 0xF011;
constexpr int kIconConfirm = 0xF071;   // 退出确认：triangle-exclamation
constexpr int kIconCancel = 0xF00D;    // 取消：xmark

// 菜单位置 = 鼠标点击点屏幕坐标（窗口原点 + 窗口内光标，GLFW 屏幕坐标系实时值），
// 以点击点为左上角向下展开，越出工作区由 styledMenuClampPosition 兜底。
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

// 退出二段确认（仅「确认退出」触发 requestExit）：styled 优先，失败降级原生就地弹出。
void showPetExitConfirm() {
    const std::vector<PetExitConfirmItem> confirmData = petExitConfirmItems();
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
    // 退出确认收窄：panelWidth 归零 → 宽度 = 最长行自适应，不超内容。
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
            // 与设置页模块开关关共用一条关闭路径（含可见意图记账）
            closePetWindow();
            break;
        case PetMenuAction::Exit:
            showPetExitConfirm();
            break;
        case PetMenuAction::None:
        default:
            break;
    }
}

// styled 菜单构建（PetMenuItemData → StyledMenuItem，图标按语义注入）；换肤子组设
// collapsedByDefault=true（默认只显"换肤 ▸"，点组头/chevron 展开）。
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
    // 系统挂起期间整帧不进 compose（挂起的窗口无屏可看），唤醒回调清标志后自动恢复。
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
            // 精灵帧让位到气泡条下方（见 pet_bubble.h）；窗口高 = petWindowHeight(帧高)。
            const PetRect spriteRect =
                petSpriteRect(screen.width, screen.height);
            ui.image("pet.sprite")
                .source(state.anim.currentPath(skin).string())
                .position(spriteRect.x, spriteRect.y)
                .size(spriteRect.w, spriteRect.h)
                // 帧推进：onTimer 周期 = 1/frameRate；顺序即优先级：代理流式 > 入睡 > 帧推进
                .onTimer(1.0f / static_cast<float>(skin.frameRate),
                         [&state] {
                             const PetSkin& s = runtime().skin;
                             const double now = steadyMs();
                             const double dt =
                                 std::max(0.0, (now - state.lastTickMs) / 1000.0);
                             state.lastTickMs = now;
                             const bool streaming =
                                 core::proxy_stats::snapshot().activeStreams > 0;
                             state.anim.setStreaming(streaming, s);
                             state.anim.considerSleep(
                                 (now - state.lastInteractionMs) / 1000.0, s);
                             state.anim.tick(s);
                             state.bubble.tick(dt);
                             // 三级内容源调度：本轮弹哪一级 / 停多久由 petBubbleSchedule 纯函数决定
                             //（节拍与命中口径见 pet_bubble.h）。数据源经 main 注入的 app 级只读桥；
                             // ③为 take 语义——只在命中数据槽位时才读桥；桥都无数据则降级①静态语料。
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
                    const double now = steadyMs();
                    // 双击（间隔 <350ms）→ 打开/还原主窗口；首击仍走单击语义
                    if (now - state.lastClickMs < 350.0) {
                        state.lastClickMs = 0.0;  // 复位计时，防三连击再触发
                        app::requestShow();
                        return;
                    }
                    state.lastClickMs = now;
                    state.lastInteractionMs = now;
                    if (!state.wasDragged) {
                        state.anim.poke(runtime().skin);
                    }
                    state.wasDragged = false;
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
                    // 锚点跟随（见 pet_window.h）：不能用 event.delta 累加——窗口自身移动会漂移
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
                    savePetPosition();  // 拖拽结束才写盘
                })
                .onContextMenu([](const eui::PointerEvent&, const eui::Rect&) {
                    // 优先 styled 独立菜单窗（不受 128px sprite 窗 bounds 裁剪）；构建失败
                    // 降级原生菜单。开菜单前刷新皮肤清单（新放进目录的皮肤立即可选）。
                    runtime().skins = discoverSkins();
                    if (runtime().handle == nullptr) return;

                    const std::vector<PetMenuItemData> menu =
                        petContextMenuItems(runtime().skins);
                    const std::pair<float, float> pos = petMenuAnchorPos();
                    if (showPetStyledMenu(menu, pos.first, pos.second)) {
                        return;  // 已入队开窗，选择经 executePetMenuSelection 回调
                    }

                    // 原生降级：Win32 TrackPopupMenuEx 光标处弹出，同步模态
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

            // 气泡对话：宠物窗内自绘圆角矩形 + 小三角 + text，画在顶部气泡条内，超时自动消失。
            if (state.bubble.visible) {
                const core::theme::Tokens& t = core::theme::current();
                // 深色半透底 + 白字/白描边；白字取自 theme onPrimary，不硬编码白色。
                const core::Color kBubbleBg{0.16f, 0.15f, 0.13f, 0.55f};
                const core::Color kBubbleBorder(t.colors.onPrimary.r,
                                                t.colors.onPrimary.g,
                                                t.colors.onPrimary.b, 0.22f);
                // 气泡宽自适应文本：内在文本宽 + 左右各 6px padding（上限 280、下限 80）；
                // 测量用 measureTextSize，与下方 ui.text 同字号 11px。
                core::TextStyle bubbleStyle;
                bubbleStyle.text = state.bubble.text;
                bubbleStyle.fontSize = 11.0f;
                const float bubbleW = std::clamp(
                    core::TextPrimitive::measureTextSize(bubbleStyle).x + 12.0f,
                    80.0f, 280.0f);
                const PetRect bubble = petBubbleRect(screen.width, bubbleW);
                ui.rect("pet.bubble.bg")
                    .position(bubble.x, bubble.y)
                    .size(bubble.w, bubble.h)
                    .color(kBubbleBg)
                    .radius(8.0f)
                    .border(1.0f, kBubbleBorder)
                    .build();
                ui.rect("pet.bubble.tail")
                    .position(bubble.x + bubble.w * 0.5f - 3.0f,
                              kBubbleHeight - 3.0f)
                    .size(6.0f, 6.0f)
                    .color(kBubbleBg)
                    .radius(t.sizes.radiusSmall)  // 统一圆角：6px 会被钳到 3px
                    .rotate(0.785f)  // 45° 小方块当三角尾巴
                    .build();
                ui.text("pet.bubble.text")
                    .position(bubble.x + 6.0f, bubble.y)
                    .size(bubble.w - 12.0f, bubble.h)
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

// 解析皮肤目录：按候选根找 <skinName>；找不到落回 petsDir()（仍没有则由 ensureDefaultSkin 兜底）。
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

// 随机挑一句气泡短语（声明见 pet_window.h；短语表随皮肤 phraseTable）。
std::string pickPetPhrase(const PetSkin& skin) {
    const std::vector<std::string>& table = phraseTable(skin);
    return table[std::uniform_int_distribution<size_t>(0, table.size() - 1)(
        phraseRng())];
}

std::pair<int, int> petDragMove(PetDragAnchor& anchor, int windowX, int windowY,
                                double cursorX, double cursorY) {
    // 光标屏幕位 = 窗口原点 + 窗口内光标（同为 GLFW 屏幕坐标实时值——这就是 delta 反馈回路的解法）
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
    // 选屏：含住点的取之（多块含住取枚举序首个）；都不含则取点-矩形距离平方最小的
    //（= Win32 MONITOR_DEFAULTTONEAREST），并列取先枚举者。
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

bool startPetWindow(const PetWindowConfig& config) {
    PetRuntime& rt = runtime();
    rt.config = config;
    // 显示大小档防御收敛（窗口尺寸换算永远吃合法档）
    rt.config.scale = sanitizePetScale(config.scale);
    // 本函数同时置装配标志与可见意图（装配 = 想显示）
    rt.assembled = true;
    rt.windowRequested = true;

    // 皮肤：候选根找 → 找不到回退默认皮肤 → 加载随包 robot
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
        skin = ensureDefaultSkin();
    }
    if (skin.isFail()) {
        LOG_ERROR("桌宠皮肤不可用（" + skin.errorMessage() + "），不开窗口");
        // 失败不再无声——返回值让 showPetWindow 能区分 StartFailed（装配段 main 忽略）。
        return false;
    }
    rt.skin = std::move(skin.value());
    rt.config.skinName = skinName;
    rt.skins = discoverSkins();

    // 注册进程级电源回调（换肤重开重复注册 = 单实例覆盖，幂等；回调只动 renderPaused）。
    core::platform::setSystemPowerHandler([](core::platform::SystemPowerEvent ev) {
        setPetRenderPaused(ev == core::platform::SystemPowerEvent::Suspending);
    });

    // 位置：未设置 → 主显示器工作区右下角；已知位置钳进所在/最近显示器的工作区。
    // 钳制/默认位都按整窗算（缩放后帧尺寸 + 气泡让位条），单位 = 点。
    const auto [winW, winHFrame] =
        scaledFrameSize(rt.skin.frameWidth, rt.skin.frameHeight,
                        rt.config.scale);
    const int winH = petWindowHeight(winHFrame);
    int x = config.x;
    int y = config.y;
    const auto [defX, defY] = defaultPetPos(winW, winH);
    // 单边独立处理 -1 哨兵：只重置未设置的那一边；其他负值是左/上副屏的合法坐标。
    if (x == -1) {
        x = defX;
    }
    if (y == -1) {
        y = defY;
    }
    // 与落盘路径共用同一钳制纯函数（默认位本就在工作区内，再钳幂等）。
    const auto [cx, cy] =
        clampPetPosToMonitors(x, y, currentMonitorWorkAreas(), winW, winH);
    x = cx;
    y = cy;
    rt.config.x = x;
    rt.config.y = y;

    app::openWindow(app::DslWindowConfig{}
                        .title("DevDesk Pet")
                        .pageId("pet")
                        // 窗口 = 缩放后帧宽 × (缩放后帧高 + 气泡让位条)，单位 = 点
                        .windowSize(winW, winH)
                        .windowPosition(x, y)
                        .decorated(false)              // 无边框
                        .alwaysOnTop(true)             // 置顶
                        .resizable(false)
                        .focusOnShow(false)            // 显示不抢焦点
                        .hideFromTaskbar(true)         // 不进任务栏/Alt+Tab
                        .transparentFramebuffer(true)  // sprite 窗逐像素透明
                                                       //（黑底修复：不随全局
                                                       // 窗口效果档位回落）
                        // backdrop 档位显式 None（灰底修复）：不跟随全局磨砂/半透档，
                        // 否则 Acrylic 材质画在透明像素后，四角透出的是材质灰。
                        .windowEffect(core::platform::WindowEffect::None)
                        .ignoreClearColorOverride()    // 全透底不被主题广播冲掉
                        .clearColor({0.0f, 0.0f, 0.0f, 0.0f})
                        .onWindowCreated([](core::window::Handle handle) {
                            runtime().handle = handle;
                        })
                        // 任何销毁路径都先于 glfwDestroyWindow 清空缓存句柄（UAF 根治）；
                        // 比较句柄是必需的：热切换新窗创建早于旧窗销毁，无条件清会误清新句柄。
                        .onWindowDestroyed([](core::window::Handle destroyed) {
                            if (runtime().handle == destroyed) {
                                runtime().handle = nullptr;
                            }
                        }),
                    petCompose);

    LOG_INFO("桌宠窗口已请求开启（皮肤=" + skinName + "，帧=" +
             std::to_string(winW) + "x" + std::to_string(winH) + "（含气泡条，"
             "缩放档 " + kPetScalePresetLabels[petScaleIndexOf(rt.config.scale)] +
             "），帧率=" + std::to_string(static_cast<int>(rt.skin.frameRate)) +
             "fps，位置=" + std::to_string(x) + "," + std::to_string(y) + "）");
    return true;
}

// 重新显示桌宠窗口（设置页「显示桌宠」按钮 + 模块开关重新勾选）。四态见声明。
PetShowOutcome showPetWindow() {
    PetRuntime& rt = runtime();
    if (!rt.assembled) {
        return PetShowOutcome::NotAssembled;  // 本会话从未装配——不凭空开窗
    }
    if (rt.handle != nullptr) {
        return PetShowOutcome::AlreadyOpen;  // 窗口已开着（隐藏后句柄被清空）
    }
    if (!startPetWindow(rt.config)) {  // 皮肤/位置/气泡数据源桥沿用上次装配
        return PetShowOutcome::StartFailed;  // 皮肤不可用，未入队开窗
    }
    return PetShowOutcome::Shown;
}

// 关闭桌宠窗口（模块开关取消勾选 → 立即隐藏）。窗口开着走与右键「隐藏桌宠」同一条
// 关闭路径；无头/已关时只清可见意图，不动装配标志（重新勾选仍可原位重开）。
bool closePetWindow() {
    PetRuntime& rt = runtime();
    const bool hadIntent = rt.windowRequested;
    rt.windowRequested = false;
    if (rt.handle == nullptr) {
        return hadIntent;  // 无窗口可关（无头 / 已隐藏）：只记状态
    }
    core::window::requestWindowClose(rt.handle);
    LOG_INFO("桌宠窗口已请求关闭（模块开关关闭）");
    return true;
}

bool petWindowRequested() { return runtime().windowRequested; }

void resetPetRuntimeForTests() { runtime() = PetRuntime{}; }

void applyPetSkin(const std::string& skinName) {
    PetRuntime& rt = runtime();
    // 只接受发现到的皮肤名（名字会拼进文件系统路径，防手改配置注入）
    const std::vector<std::string> skins = discoverSkins();
    if (std::find(skins.begin(), skins.end(), skinName) == skins.end()) {
        LOG_WARN("桌宠换肤被拒绝：皮肤 \"" + skinName + "\" 未在皮肤目录中发现");
        return;
    }
    PetConfigStore store(::core::appDatabase());
    PetConfig cfg = store.load();
    cfg.skinName = skinName;
    if (rt.handle != nullptr) {
        // 记住当前窗口位置（换肤后帧尺寸可能不同，startPetWindow 会重新钳制）
        int x = 0;
        int y = 0;
        core::window::getWindowPos(rt.handle, x, y);
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
        // 热切换：关旧窗 → 按新皮肤原位重开（位置沿用，显示大小档原样携带）。
        core::window::requestWindowClose(rt.handle);
        rt.handle = nullptr;  // 等新窗 onWindowCreated 回填
        startPetWindow(
            PetWindowConfig{skinName, cfg.x, cfg.y, rt.config.scale,
                            rt.config.getOverdueCount,
                            rt.config.getProxyEvent});
    }
    LOG_INFO("桌宠换肤 → " + skinName + (hotSwap ? "（已热切换）" : "（下次启动生效）"));
}

void resetPetPosition() {
    PetRuntime& rt = runtime();
    // 坐标清回 -1 哨兵：显示器布局变化后下次启动重算默认位
    PetConfigStore store(::core::appDatabase());
    PetConfig cfg = store.load();
    cfg.x = -1;
    cfg.y = -1;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠位置重置持久化失败（不影响本次会话）");
    }
    if (rt.handle == nullptr) return;
    // 整窗口径同 startPetWindow：宽/高 = 缩放后帧尺寸 + 气泡让位条
    const auto [fw, fh] = scaledFrameSize(rt.skin.frameWidth, rt.skin.frameHeight,
                                          rt.config.scale);
    const auto [x, y] = defaultPetPos(fw, petWindowHeight(fh));
    core::window::setWindowPos(rt.handle, x, y);
    rt.config.x = x;
    rt.config.y = y;
    LOG_INFO("桌宠位置已重置（" + std::to_string(x) + "," + std::to_string(y) + "）");
}

// 设置页桌宠卡只读桥：读 app_config 的 pet_config 键而非 rt.config——设置页可能在窗口
// 未开时读，且 apply 双写保证盘与运行时一致。
std::string currentSkinName() {
    return PetConfigStore(::core::appDatabase()).load().skinName;
}

// ---- 显示大小三档（声明见 pet_window.h；设置页经 module_pet_bridge.h 消费）----

int currentPetScaleIndex() {
    return petScaleIndexOf(PetConfigStore(::core::appDatabase()).load().scale);
}

std::vector<std::string> petScalePresetLabels() {
    return {kPetScalePresetLabels[0], kPetScalePresetLabels[1],
            kPetScalePresetLabels[2]};
}

void applyPetScaleIndex(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= kPetScalePresetCount) {
        return;  // 越界下标（含空清单误回传）静默拒绝，配置不动
    }
    PetRuntime& rt = runtime();
    const double scale = kPetScalePresets[presetIndex];
    PetConfigStore store(::core::appDatabase());
    PetConfig cfg = store.load();
    cfg.scale = scale;
    if (rt.handle != nullptr) {
        // 记住当前窗口位置（切档后窗口尺寸变化，重开时 startPetWindow 会重新钳制）
        int x = 0;
        int y = 0;
        core::window::getWindowPos(rt.handle, x, y);
        clampPetPosIntoWorkAreaInPlace(x, y);
        cfg.x = x;
        cfg.y = y;
    }
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠显示大小持久化失败（不影响本次会话）");
    }
    const bool hotSwap = rt.handle != nullptr;
    rt.config.scale = scale;
    if (hotSwap) {
        // 运行中改档即时生效：关旧窗 → 按新档位重开。位置传新鲜值 cfg.x/y（刚读回钳制）——
        // rt.config.x/y 拖拽只落盘不回写运行时，用它重开会让桌宠跳回拖拽前位置。
        core::window::requestWindowClose(rt.handle);
        rt.handle = nullptr;
        startPetWindow(
            PetWindowConfig{rt.config.skinName, cfg.x, cfg.y, scale,
                            rt.config.getOverdueCount,
                            rt.config.getProxyEvent});
    }
    LOG_INFO(std::string("桌宠显示大小 → ") + kPetScalePresetLabels[presetIndex] +
             (hotSwap ? "（已热切换）" : "（下次启动生效）"));
}


// ---- 退出二次确认（纯数据层，声明见 pet_window.h）----
// 下标语义与 petExitConfirmChosen 耦合：0=说明(disabled) 1=分隔线 2=确认退出 3=取消。
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

// ---- 右键菜单结构（纯数据层，声明见 pet_window.h）----
// 顺序 = path 语义基础：换肤子组 / 位置重置 / 隐藏 / 分隔线 / 退出。
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
    // 顶层下标随换肤子组有无整体偏移；退出项在「隐藏」+ 分隔线之后（分隔线占下标）。
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
