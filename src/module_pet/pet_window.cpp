// src/module_pet/pet_window.cpp
// 桌宠窗口：无边框/置顶/透明/不抢焦点/不进任务栏的独立子窗口
//（EUI-NEO G1-G5 扩展，见 ../EUI-NEO docs/DSL.md「子窗口配置」）。
// MVP 状态机（设计 §4.4）：Idle（眨眼循环）→ 点击 → Poked（反应帧播一遍）→ Idle；
// 拖拽移动窗口（onDrag + setWindowPos，结束持久化）；右键菜单（穿透/隐藏/退出）。
#include "module_pet/pet_window.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/logging.h"
#include "core/paths.h"
#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"
#include "module_settings/settings_config.h"

#include <components/contextmenu.h>
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
};

PetRuntime& runtime() {
    static PetRuntime instance;
    return instance;
}

// ---- 持久化（读-改-写 config.json，只动 pet 字段）----

void savePetPosition() {
    if (runtime().handle == nullptr) return;
    int x = 0;
    int y = 0;
    core::window::getWindowPos(runtime().handle, x, y);
    module::settings::SettingsStore store(core::paths::configFile());
    auto cfg = store.load();
    cfg.petX = x;
    cfg.petY = y;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠位置持久化失败（不影响本次会话）");
    }
}

void savePetClickThrough(bool enabled) {
    module::settings::SettingsStore store(core::paths::configFile());
    auto cfg = store.load();
    cfg.petClickThrough = enabled;
    if (store.save(cfg).isFail()) {
        LOG_WARN("桌宠穿透状态持久化失败（不影响本次会话）");
    }
}

// ---- compose 状态（ui.state 挂在子窗口自己的 Ui 实例上）----

struct PetUiState {
    PetAnimation anim;
    bool initialized = false;
    bool wasDragged = false;   // 拖拽结束抑制误触 poke（设计 §4.4 Dragged 语义）
    bool clickThrough = false;
    bool menuOpen = false;
    float menuX = 0.0f;
    float menuY = 0.0f;
};

void petCompose(eui::Ui& ui, const eui::Screen& screen) {
    PetUiState& state = ui.state<PetUiState>("pet.state");
    if (!state.initialized) {
        state.initialized = true;
        state.clickThrough = runtime().config.clickThrough;
    }
    const PetSkin& skin = runtime().skin;

    ui.stack("pet.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 窗口尺寸 = 帧紧包围盒（设计 §4.2）；MVP 命中 = 整窗（O1 逐像素 alpha 是 M3）
            ui.image("pet.sprite")
                .source(state.anim.currentPath(skin).string())
                .size(screen.width, screen.height)
                // 帧推进：onTimer 周期 = 1/frameRate（等待期主循环低功耗睡眠）
                .onTimer(1.0f / static_cast<float>(skin.frameRate),
                         [&state] { state.anim.tick(runtime().skin); })
                .onClick([&state]() {
                    if (!state.wasDragged) {
                        state.anim.poke(runtime().skin);
                    }
                    state.wasDragged = false;
                })
                .onDrag([&state](const core::dsl::DragEvent& event) {
                    // 拖过阈值 → 标记（释放时不触发 poke）
                    state.wasDragged =
                        std::abs(event.totalX) + std::abs(event.totalY) > 4.0;
                    if (runtime().handle == nullptr) return;
                    int x = 0;
                    int y = 0;
                    core::window::getWindowPos(runtime().handle, x, y);
                    core::window::setWindowPos(
                        runtime().handle,
                        x + static_cast<int>(event.deltaX),
                        y + static_cast<int>(event.deltaY));
                })
                .onRelease([](const eui::PointerEvent&, const eui::Rect&) {
                    savePetPosition();  // 拖拽结束才写盘（设计 §4.6）
                })
                .onContextMenu([&state](const eui::PointerEvent& event, const eui::Rect&) {
                    state.menuX = event.x;
                    state.menuY = event.y;
                    state.menuOpen = true;
                })
                .build();
        })
        .build();

    // 右键菜单（设计 §4.5 MVP 裁剪）：挂机穿透 / 隐藏 / 退出
    components::contextMenu(ui, "pet.menu")
        .theme(components::theme::dark())
        .screen(screen.width, screen.height)
        .position(state.menuX, state.menuY)
        .items(std::vector<components::ContextMenuItem>{
            {state.clickThrough ? "关闭鼠标穿透" : "挂机穿透（点击透到下层）"},
            {"隐藏桌宠（设置页可重新开启）"},
            {"退出 DevDesk"},
        })
        .open(state.menuOpen)
        .onSelect([&state](int index) {
            state.menuOpen = false;
            switch (index) {
            case 0: {
                state.clickThrough = !state.clickThrough;
                core::window::setWindowMousePassthrough(runtime().handle,
                                                        state.clickThrough);
                savePetClickThrough(state.clickThrough);
                break;
            }
            case 1:
                savePetPosition();
                core::window::requestWindowClose(runtime().handle);  // 主循环正常销毁
                break;
            case 2:
                app::requestExit();  // 与托盘 Exit 同一退出路径
                break;
            default:
                break;
            }
        })
        .onOpenChange([&state](bool open) { state.menuOpen = open; })
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

void startPetWindow(const PetWindowConfig& config) {
    PetRuntime& rt = runtime();
    rt.config = config;

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

    // 位置：未设置 → 主显示器工作区右下角；已知位置钳制进工作区（设计 §4.6，
    // M1 先主显示器，多显示器钳制是 M2）。工作区查询失败（无头/早于 glfwInit）
    // 时不动已知位置，未设置则给一个屏内兜底值
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);
    const bool haveWorkArea = workW > 0 && workH > 0;
    int x = config.x;
    int y = config.y;
    if (x < 0 || y < 0) {
        if (haveWorkArea) {
            x = workX + workW - rt.skin.frameWidth - 48;
            y = workY + workH - rt.skin.frameHeight - 48;
        } else {
            x = 100;
            y = 100;
        }
    } else if (haveWorkArea) {
        x = std::clamp(x, workX, workX + std::max(0, workW - rt.skin.frameWidth));
        y = std::clamp(y, workY, workY + std::max(0, workH - rt.skin.frameHeight));
    }
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
                        .clickThrough(config.clickThrough)  // 挂机穿透（持久化恢复）
                        .ignoreClearColorOverride()    // 全透底不被主题广播冲掉
                        .clearColor({0.0f, 0.0f, 0.0f, 0.0f})
                        .onWindowCreated([](core::window::Handle handle) {
                            runtime().handle = handle;
                        }),
                    petCompose);

    LOG_INFO("桌宠窗口已请求开启（皮肤=" + skinName + "，帧率=" +
             std::to_string(static_cast<int>(rt.skin.frameRate)) +
             "fps，位置=" + std::to_string(x) + "," + std::to_string(y) + "）");
}

} // namespace module::pet
