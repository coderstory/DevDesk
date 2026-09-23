// src/module_pet/pet_window.cpp
// 桌宠窗口：无边框/置顶/透明/不抢焦点/不进任务栏的独立子窗口
//（EUI-NEO G1-G5 扩展，见 ../EUI-NEO docs/DSL.md「子窗口配置」）。
// M2 状态机（设计 §4.4/§4.5）：Idle（眨眼循环）→ 点击 → Poked（播一遍）；
// 无交互 5min → Sleep（闭眼+Zzz）；代理活动流 → Typing（打字帧）。
// 气泡对话（点击/随机闲聊，短语表随皮肤）；右键菜单（穿透/换肤/位置
// 重置/隐藏/退出——退出为二段确认，审计 [25]）；拖拽移动窗口（onDrag + setWindowPos，结束持久化）。
// 代理联动读 core::proxy_stats 共享统计（module_pet → core 合法依赖，
// 不碰 module_claude_proxy）
#include "module_pet/pet_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "core/logging.h"
#include "core/paths.h"
#include "core/platform/context_menu.h"
#include "core/platform/power_events.h"
#include "core/proxy_stats.h"
#include "module_pet/pet_bubble.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"

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
    // M2 Sleep 零渲染：系统挂起置位 / 唤醒清零（回调在主线程，compose 同线程
    // 读，无并发，普通 bool 即可）
    bool renderPaused = false;
};

PetRuntime& runtime() {
    static PetRuntime instance;
    return instance;
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

// 随机挑一句短语（短语表随皮肤，见 pet_skin.h phraseTable）
std::string pickPhrase(const PetSkin& skin) {
    const std::vector<std::string>& table = phraseTable(skin);
    return table[static_cast<size_t>(std::rand()) % table.size()];
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

void savePetClickThrough(bool enabled) {
    PetConfigStore store(petConfigFile());
    PetConfig cfg = store.load();
    cfg.clickThrough = enabled;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠穿透状态持久化失败（不影响本次会话）");
    }
}

// ---- compose 状态（ui.state 挂在子窗口自己的 Ui 实例上）----

struct PetUiState {
    PetAnimation anim;
    PetBubble bubble;              // 气泡对话（M2 设计 §4.5）
    bool initialized = false;
    bool wasDragged = false;   // 拖拽结束抑制误触 poke（设计 §4.4 Dragged 语义）
    // 穿透状态在 PetRuntime::config（原生菜单切换，热换肤重开窗后仍在）
    double lastInteractionMs = 0.0;  // 最近交互时刻（入睡判定用）
    double lastTickMs = 0.0;         // 上次 onTimer 时刻（气泡倒计时 dt 用）
    PetDragAnchor dragAnchor;        // 拖拽锚点（见 pet_window.h，抖动修复）
};

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
                             // 随机闲聊：Idle 且无气泡时，平均 ~60s 一句
                             if (!state.bubble.visible &&
                                 state.anim.state == PetAnimState::Idle) {
                                 if (static_cast<double>(std::rand()) / RAND_MAX <
                                     dt / 60.0) {
                                     state.bubble.show(pickPhrase(s), 3.0);
                                 }
                             }
                         })
                .onClick([&state]() {
                    state.lastInteractionMs = steadyMs();
                    if (!state.wasDragged) {
                        state.anim.poke(runtime().skin);
                    }
                    state.wasDragged = false;
                    // 点击说一句话（M2 §4.5：交互触发的气泡）
                    state.bubble.show(pickPhrase(runtime().skin), 3.0);
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
                    // 原生右键菜单（EUI-NEO core::platform::showContextMenu，
                    // Win32 TrackPopupMenuEx 光标处弹出）：自绘菜单画在 128px
                    // sprite 窗内被窗口 bounds 裁剪，只露出一点（用户实测）。
                    // 同步模态：主循环在菜单期间不推进（与托盘菜单同一模式）。
                    // 开菜单前刷新皮肤清单（新放进目录的皮肤立即可选）
                    runtime().skins = discoverSkins();
                    if (runtime().handle == nullptr) return;
                    const core::window::NativeWindowInfo info =
                        core::window::nativeWindowInfo(runtime().handle);
                    if (info.platformWindow == nullptr) return;  // 无原生句柄（stub 平台）

                    std::vector<core::platform::ContextMenuItem> items;
                    core::platform::ContextMenuItem through;
                    through.text = runtime().config.clickThrough
                                       ? "关闭鼠标穿透"
                                       : "挂机穿透（点击透到下层；锁死后到设置页桌宠卡关闭）";
                    items.push_back(std::move(through));
                    if (!runtime().skins.empty()) {
                        core::platform::ContextMenuItem skinMenu;
                        skinMenu.text = "换肤";
                        for (const std::string& name : runtime().skins) {
                            core::platform::ContextMenuItem skin;
                            skin.text = name;
                            skinMenu.children.push_back(std::move(skin));
                        }
                        items.push_back(std::move(skinMenu));
                    }
                    core::platform::ContextMenuItem reset;
                    reset.text = "位置重置（回主屏右下角）";
                    items.push_back(std::move(reset));
                    core::platform::ContextMenuItem hide;
                    hide.text = "隐藏桌宠（本次会话；重启后恢复）";
                    items.push_back(std::move(hide));
                    items.push_back(core::platform::ContextMenuItem{"-"});
                    core::platform::ContextMenuItem exitApp;
                    exitApp.text = "退出 DevDesk";
                    items.push_back(std::move(exitApp));

                    const std::optional<std::vector<int>> selection =
                        core::platform::showContextMenu(info.platformWindow, items);
                    if (!selection.has_value() || selection->empty()) return;  // 不支持/取消

                    // 下标随「换肤」子菜单有无偏移（与旧自绘菜单同一套数学）
                    const int idxThrough = 0;
                    const int idxReset = runtime().skins.empty() ? 1 : 2;
                    const int idxHide = idxReset + 1;
                    const int idxExit = idxHide + 1;
                    const std::vector<int>& path = *selection;
                    if (path.size() == 2) {
                        if (path[0] == 1 && path[1] >= 0 &&
                            path[1] < static_cast<int>(runtime().skins.size())) {
                            applyPetSkin(runtime().skins[static_cast<size_t>(path[1])]);
                        }
                        return;
                    }
                    if (path.size() != 1) return;
                    if (path[0] == idxThrough) {
                        setPetClickThrough(!runtime().config.clickThrough);
                    } else if (path[0] == idxReset) {
                        resetPetPosition();
                    } else if (path[0] == idxHide) {
                        savePetPosition();
                        core::window::requestWindowClose(runtime().handle);
                    } else if (path[0] == idxExit) {
                        // 审计 [25]：退出是应用级破坏性操作（断代理/丢弹窗
                        // 状态），不能与「隐藏桌宠」同权重零确认。原生菜单
                        // 就地二段确认（dialog 组件在 sprite 小窗被 bounds
                        // 裁剪，不可用——见本 lambda 头注释的同一先例）；
                        // 取消/Esc/平台不支持一律不退出
                        std::vector<core::platform::ContextMenuItem> confirm;
                        for (const PetExitConfirmItem& ci : petExitConfirmItems()) {
                            if (ci.text == "-") {
                                confirm.push_back(
                                    core::platform::ContextMenuItem{"-"});
                                continue;
                            }
                            core::platform::ContextMenuItem item;
                            item.text = ci.text;
                            item.disabled = ci.disabled;
                            confirm.push_back(std::move(item));
                        }
                        const std::optional<std::vector<int>> chosen =
                            core::platform::showContextMenu(info.platformWindow,
                                                            confirm);
                        if (chosen.has_value() && !chosen->empty() &&
                            petExitConfirmChosen(*chosen)) {
                            app::requestExit();  // 与托盘 Exit 同一退出路径
                        }
                    }
                })
                .build();

            // 气泡对话（M2 设计 §4.5）：宠物窗内自绘圆角矩形 + 小三角 + text，
            // 挂在头顶，超时自动消失（不用 toast——位置不可控）
            if (state.bubble.visible) {
                const core::Color kBubbleBg{0.16f, 0.15f, 0.13f, 0.92f};
                const core::Color kBubbleBorder{1.0f, 1.0f, 1.0f, 0.22f};
                ui.rect("pet.bubble.bg")
                    .position(2.0f, 2.0f)
                    .size(screen.width - 4.0f, 32.0f)
                    .color(kBubbleBg)
                    .radius(8.0f)
                    .border(1.0f, kBubbleBorder)
                    .build();
                ui.rect("pet.bubble.tail")
                    .position(screen.width * 0.5f - 3.0f, 31.0f)
                    .size(6.0f, 6.0f)
                    .color(kBubbleBg)
                    .rotate(0.785f)  // 45° 小方块当三角尾巴
                    .build();
                ui.text("pet.bubble.text")
                    .position(6.0f, 2.0f)
                    .size(screen.width - 12.0f, 32.0f)
                    .text(state.bubble.text)
                    .fontSize(11.0f)
                    .lineHeight(32.0f)
                    .color(core::Color{1.0f, 1.0f, 1.0f, 0.95f})
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
    rt.skin = std::move(skin.unsafeValue());
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
                        .clickThrough(config.clickThrough)  // 挂机穿透（持久化恢复）
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
            PetWindowConfig{skinName, cfg.x, cfg.y, cfg.clickThrough});
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

void setPetClickThrough(bool enabled) {
    PetRuntime& rt = runtime();
    rt.config.clickThrough = enabled;
    if (rt.handle != nullptr) {
        // 即时生效：开 = 进入挂机锁死态；关 = 解锁（设置页恢复路径的核心
        // 诉求——锁死态下这是唯一能碰到桌宠窗口的入口）
        core::window::setWindowMousePassthrough(rt.handle, enabled);
    }
    savePetClickThrough(enabled);
    LOG_INFO(std::string("桌宠鼠标穿透 → ") +
             (enabled ? "开（挂机态；锁死后在设置页桌宠卡关闭）" : "关"));
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

} // namespace module::pet
