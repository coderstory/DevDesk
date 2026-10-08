#pragma once
// probe_harness.h — DevDesk probes 基础设施（header-only，namespace probe）
// 用法：probe main 直接 include 本头，按「初始化后端 → makeWindow →
// 帧循环（pollOnce / composeModuleFrame）→ 扫描断言」编排。仅用 EUI-NEO core
// 层 API；唯一 DevDesk 侧依赖是 ui_kit 的 splash 测试缝（initializeWindowBackend
// 里禁 splash，probe dump 不该拍到 splash——compose 路径同 app_sidebar）。

#include "core/dsl_runtime.h"
#include "core/input/input_state.h"
#include "core/window/window_backend.h"

#include <ui_kit/splash.h>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <utility>

namespace probe {

/// 本工程（DevDesk）统一 GLFW + OpenGL 渲染配置，恒返回 OpenGL。
inline core::window::RenderApi configuredRenderApi() {
    return core::window::RenderApi::OpenGL;
}

/// 窗口后端生命周期（GLFW），成对调用。
inline bool initializeWindowBackend() {
    // splash 测试缝：probe 进程同样活不过 splash 2.3s 时序，禁掉后 compose
    // 路径（若后续 probe 直连 app::compose / drawSidebar）不会拍到 splash
    ::ui_kit::setSplashDisabledForTests(true);
    return glfwInit() == GLFW_TRUE;
}

inline void terminateWindowBackend() {
    glfwTerminate();
}

/// 创建探针窗口：不可缩放 + OpenGL；失败返回 nullptr。
inline core::window::Handle makeWindow(const std::string& title, int width, int height) {
    core::window::WindowCreateRequest request;
    request.title = title.c_str();
    request.width = width;
    request.height = height;
    request.resizable = false;
    request.renderApi = configuredRenderApi();
    return core::window::createWindow(request);
}

/// 命令行探针参数。
struct ProbeArgs {
    int frames = 3;          // 渲染帧数
    int windowW = 1200;      // 窗口宽（逻辑像素）
    int windowH = 1000;      // 窗口高
    bool manual = false;     // 手动驱动模式
};

inline int parseLeadingInt(const std::string& text, int fallback) {
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10 + (c - '0');
    }
    return value > 0 ? value : fallback;
}

/// 解析 --frames N / --size WxH / --manual；未知参数忽略。
inline ProbeArgs parseProbeArgs(int argc, char* argv[]) {
    ProbeArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            args.frames = parseLeadingInt(argv[++i], args.frames);
        } else if (arg == "--size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const std::size_t sep = size.find_first_of("xX");
            if (sep != std::string::npos) {
                args.windowW = parseLeadingInt(size.substr(0, sep), args.windowW);
                args.windowH = parseLeadingInt(size.substr(sep + 1), args.windowH);
            }
        } else if (arg == "--manual") {
            args.manual = true;
        }
    }
    return args;
}

/// 单帧驱动：Runtime::update（输入派发 + 交互 + 布局/重绘标脏），
/// pointerScale = dpiScale = 1（逻辑 = 物理，探针窗口无缩放）。
inline bool pollOnce(core::window::Handle window,
                     core::dsl::Runtime& runtime,
                     float dt = 1.0f / 60.0f) {
    return runtime.update(window, dt, 1.0f, 1.0f);
}

/// 帧循环标准单元：先 pollOnce 再 compose（成对使用——update 不重建元素树，
/// 弹层等条件元素要 recompose 才进 DOM，requestFocus 等帧末请求也在 compose
/// 消费）。composeFn 形如 [&](Ui& ui, const Screen& screen) { ... }。
template <typename ComposeFn>
inline void composeModuleFrame(core::dsl::Runtime& runtime,
                               core::window::Handle window,
                               const std::string& pageId,
                               float logicalWidth,
                               float logicalHeight,
                               ComposeFn&& composeFn) {
    pollOnce(window, runtime);
    runtime.compose(pageId, logicalWidth, logicalHeight, std::forward<ComposeFn>(composeFn));
}

namespace detail {

/// GLFW 键码 → core::InputKey。与 EUI-NEO core/window/window_input_backend.cpp
/// mapGlfwKey 保持一致（该函数是 TU 内 static 不导出，harness 自含一份）。
/// 未知键 → InputKey::Unknown（queueKeyInput 会按 Unknown 忽略按键态）。
inline core::InputKey glfwKeyToInputKey(int key) {
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::Digit0) + key - GLFW_KEY_0);
    }
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::A) + key - GLFW_KEY_A);
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::F1) + key - GLFW_KEY_F1);
    }
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::Numpad0) + key - GLFW_KEY_KP_0);
    }
    switch (key) {
        case GLFW_KEY_BACKSPACE: return core::InputKey::Backspace;
        case GLFW_KEY_TAB: return core::InputKey::Tab;
        case GLFW_KEY_ENTER: return core::InputKey::Enter;
        case GLFW_KEY_ESCAPE: return core::InputKey::Escape;
        case GLFW_KEY_SPACE: return core::InputKey::Space;
        case GLFW_KEY_INSERT: return core::InputKey::Insert;
        case GLFW_KEY_DELETE: return core::InputKey::Delete;
        case GLFW_KEY_HOME: return core::InputKey::Home;
        case GLFW_KEY_END: return core::InputKey::End;
        case GLFW_KEY_PAGE_UP: return core::InputKey::PageUp;
        case GLFW_KEY_PAGE_DOWN: return core::InputKey::PageDown;
        case GLFW_KEY_LEFT: return core::InputKey::Left;
        case GLFW_KEY_RIGHT: return core::InputKey::Right;
        case GLFW_KEY_UP: return core::InputKey::Up;
        case GLFW_KEY_DOWN: return core::InputKey::Down;
        case GLFW_KEY_PRINT_SCREEN: return core::InputKey::PrintScreen;
        case GLFW_KEY_SCROLL_LOCK: return core::InputKey::ScrollLock;
        case GLFW_KEY_PAUSE: return core::InputKey::Pause;
        case GLFW_KEY_CAPS_LOCK: return core::InputKey::CapsLock;
        case GLFW_KEY_NUM_LOCK: return core::InputKey::NumLock;
        case GLFW_KEY_LEFT_SHIFT: return core::InputKey::LeftShift;
        case GLFW_KEY_RIGHT_SHIFT: return core::InputKey::RightShift;
        case GLFW_KEY_LEFT_CONTROL: return core::InputKey::LeftControl;
        case GLFW_KEY_RIGHT_CONTROL: return core::InputKey::RightControl;
        case GLFW_KEY_LEFT_ALT: return core::InputKey::LeftAlt;
        case GLFW_KEY_RIGHT_ALT: return core::InputKey::RightAlt;
        case GLFW_KEY_LEFT_SUPER: return core::InputKey::LeftSuper;
        case GLFW_KEY_RIGHT_SUPER: return core::InputKey::RightSuper;
        case GLFW_KEY_MENU: return core::InputKey::Menu;
        case GLFW_KEY_APOSTROPHE: return core::InputKey::Apostrophe;
        case GLFW_KEY_COMMA: return core::InputKey::Comma;
        case GLFW_KEY_MINUS: return core::InputKey::Minus;
        case GLFW_KEY_PERIOD: return core::InputKey::Period;
        case GLFW_KEY_SLASH: return core::InputKey::Slash;
        case GLFW_KEY_SEMICOLON: return core::InputKey::Semicolon;
        case GLFW_KEY_EQUAL: return core::InputKey::Equal;
        case GLFW_KEY_LEFT_BRACKET: return core::InputKey::LeftBracket;
        case GLFW_KEY_BACKSLASH: return core::InputKey::Backslash;
        case GLFW_KEY_RIGHT_BRACKET: return core::InputKey::RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return core::InputKey::GraveAccent;
        case GLFW_KEY_KP_DECIMAL: return core::InputKey::NumpadDecimal;
        case GLFW_KEY_KP_DIVIDE: return core::InputKey::NumpadDivide;
        case GLFW_KEY_KP_MULTIPLY: return core::InputKey::NumpadMultiply;
        case GLFW_KEY_KP_SUBTRACT: return core::InputKey::NumpadSubtract;
        case GLFW_KEY_KP_ADD: return core::InputKey::NumpadAdd;
        case GLFW_KEY_KP_ENTER: return core::InputKey::NumpadEnter;
        case GLFW_KEY_KP_EQUAL: return core::InputKey::NumpadEqual;
        default: return core::InputKey::Unknown;
    }
}

/// modifiers 位约定与 GLFW_MOD_* 一致：SHIFT=1 / CONTROL=2 / ALT=4 / SUPER=8 /
/// CAPS_LOCK=16 / NUM_LOCK=32——可直接传 GLFW_MOD_* 常量或其按位组合。
inline core::KeyModifiers decodeKeyModifiers(int modifiers) {
    core::KeyModifiers out;
    out.shift = (modifiers & GLFW_MOD_SHIFT) != 0;
    out.control = (modifiers & GLFW_MOD_CONTROL) != 0;
    out.alt = (modifiers & GLFW_MOD_ALT) != 0;
    out.super = (modifiers & GLFW_MOD_SUPER) != 0;
    out.capsLock = (modifiers & GLFW_MOD_CAPS_LOCK) != 0;
    out.numLock = (modifiers & GLFW_MOD_NUM_LOCK) != 0;
    return out;
}

} // namespace detail

/// 完整点击：队列 press + release 后驱动一帧（按下/抬起同帧消费）。
/// button 默认左键；传 core::PointerButton::Right 即右键点击。
inline void click(core::window::Handle window,
                  core::dsl::Runtime& runtime,
                  double x,
                  double y,
                  core::PointerButton button = core::PointerButton::Left) {
    core::queuePointerButton(window, x, y, button,
                             core::PointerAction::Press, {});
    core::queuePointerButton(window, x, y, button,
                             core::PointerAction::Release, {});
    pollOnce(window, runtime);
}

/// 键盘注入：包装 core::queueKeyInput。keyCode 取 GLFW 键码（如 GLFW_KEY_ESCAPE /
/// GLFW_KEY_ENTER），内部映射为 core::InputKey。仅排队不驱动帧——调用方自行
/// pollOnce / composeModuleFrame 使事件送达（按需 Press 与 Release 隔帧成对）。
inline void queueKey(core::window::Handle window,
                     [[maybe_unused]] core::dsl::Runtime& runtime,
                     int keyCode,
                     core::KeyAction action,
                     int modifiers = 0) {
    core::KeyEvent event;
    event.key = detail::glfwKeyToInputKey(keyCode);
    event.action = action;
    event.modifiers = detail::decodeKeyModifiers(modifiers);
    core::queueKeyInput(window, event);
}

/// 滚轮注入：包装 core::queueScrollInput（dx/dy 为滚轮增量，如类 10 双栏同步的
/// -5.0/-5.0）。仅排队不驱动帧（同上）。
inline void queueScroll(core::window::Handle window,
                        [[maybe_unused]] core::dsl::Runtime& runtime,
                        double dx,
                        double dy) {
    core::queueScrollInput(window, dx, dy);
}

/// 文本注入：包装 core::queueTextInput（UTF-8 文本，如类 6(b) 长 profile 名输入）。
/// 仅排队不驱动帧（同上）。
inline void queueText(core::window::Handle window,
                      [[maybe_unused]] core::dsl::Runtime& runtime,
                      const std::string& text) {
    core::queueTextInput(window, text);
}

/// 完整左键拖拽序列：press(x0,y0) → steps 段均匀插值 move（每段 pollOnce 一帧，
/// 命中/悬停沿路径逐段推进）→ release(x1,y1)。类 11 占位条注入用。
inline void drag(core::window::Handle window,
                 core::dsl::Runtime& runtime,
                 double x0,
                 double y0,
                 double x1,
                 double y1,
                 int steps = 8) {
    const core::KeyModifiers noMods;
    const core::PointerButtons held(core::PointerButton::Left);
    core::queuePointerButton(window, x0, y0, core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    pollOnce(window, runtime);
    const int count = steps > 0 ? steps : 1;
    for (int step = 1; step <= count; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(count);
        core::queuePointerMotion(window,
                                 x0 + (x1 - x0) * t,
                                 y0 + (y1 - y0) * t,
                                 held,
                                 noMods);
        pollOnce(window, runtime);
    }
    core::queuePointerButton(window, x1, y1, core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    pollOnce(window, runtime);
}

namespace detail {

/// 深度遍历：记录 parentOf（id → 父 id，根为 ""）、id → 元素索引、叶子清单。
inline void walkElements(const core::dsl::Element& node,
                         const std::string& parentId,
                         std::unordered_map<std::string, std::string>& parentOf,
                         std::unordered_map<std::string, const core::dsl::Element*>& byId,
                         std::vector<const core::dsl::Element*>& leaves) {
    parentOf[node.id] = parentId;
    byId[node.id] = &node;
    if (node.children.empty()) {
        leaves.push_back(&node);
        return;
    }
    for (const auto& child : node.children) {
        walkElements(*child, node.id, parentOf, byId, leaves);
    }
}

inline bool framesIntersect(const core::dsl::Element& a, const core::dsl::Element& b) {
    const auto& r1 = a.frame;
    const auto& r2 = b.frame;
    return r1.x < r2.x + r2.width && r2.x < r1.x + r1.width &&
           r1.y < r2.y + r2.height && r2.y < r1.y + r1.height;
}

} // namespace detail

/// L2 弹层 z 碰撞扫描：popupId 弹层（含根链）之上不允许有帧相交的更高 z 叶子。
/// - 弹层不存在 → true（无验证对象）
/// - 弹层链上界 = max(p.subtreeMaxZIndex, p 到根链上每元素 zIndex)
/// - 遍历叶子 e：跳过 p 祖先/子孙（parentOf 判）与 opacity==0；帧相交且
///   e.subtreeMaxZIndex >= 上界 → cerr 双方 id+z 并返回 false
inline bool assertNoEqualZOverlap(core::dsl::Ui& ui, const std::string& popupId) {
    core::dsl::Element* popup = ui.find(popupId);
    if (popup == nullptr) {
        return true;
    }
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<const core::dsl::Element*> leaves;
    for (const auto& root : ui.roots()) {
        detail::walkElements(*root, "", parentOf, byId, leaves);
    }
    int upperBound = popup->subtreeMaxZIndex;
    for (std::string id = popup->id; !id.empty(); id = parentOf[id]) {
        const auto it = byId.find(id);
        if (it != byId.end() && it->second->zIndex > upperBound) {
            upperBound = it->second->zIndex;
        }
    }
    for (const core::dsl::Element* leaf : leaves) {
        if (leaf->opacity == 0.0f) {
            continue;
        }
        bool onChain = false;  // p 祖先/自身（含根链）
        for (std::string id = popup->id; !id.empty(); id = parentOf[id]) {
            if (id == leaf->id) {
                onChain = true;
                break;
            }
        }
        if (!onChain) {  // p 子孙/自身
            for (std::string id = leaf->id; !id.empty(); id = parentOf[id]) {
                if (id == popup->id) {
                    onChain = true;
                    break;
                }
            }
        }
        if (onChain || !detail::framesIntersect(*leaf, *popup)) {
            continue;
        }
        if (leaf->subtreeMaxZIndex >= upperBound) {
            std::cerr << "probe::assertNoEqualZOverlap: leaf '" << leaf->id
                      << "' z=" << leaf->zIndex << " (subtree " << leaf->subtreeMaxZIndex
                      << ") overlaps popup '" << popup->id << "' z=" << popup->zIndex
                      << " chainUpperBound=" << upperBound << '\n';
            return false;
        }
    }
    return true;
}

/// 绘制序断言：topId 必须整子树排在 bottomId 之后绘制（= 盖在其上）。
/// 判据 = 两者的最低公共祖先（LCA）处，top 侧子级 subtreeMaxZIndex 严格
/// 大于 bottom 侧子级 subtreeMaxZIndex —— 兄弟按子树上界稳定排序
///（core/dsl.h rebuildOrderedChildren），top 侧 > bottom 侧 ⇒ top 后绘；
/// 等值 ⇒ DOM 序保持，bottom 若在 DOM 序靠后就盖住 top（本断言红）。
/// 与 assertNoEqualZOverlap 互补：后者扫描叶子级 z 碰撞（叶子 subtreeMax
/// 常为 0，抓不住「容器级等值 z」），本断言直接判 LCA 处的两个比较对象，
/// 是「弹层不被下方兄弟盖住」的主判据（settings 桌宠卡双下拉类）。
/// 父链按**指针**建（挂 id 链会在无名包装栈处断掉——对话框内部容器等
/// 不设 id，id 链到那里即终结，LCA 假阴性 "no common ancestor"）。
inline bool assertDrawnAbove(core::dsl::Ui& ui,
                             const std::string& topId,
                             const std::string& bottomId) {
    const core::dsl::Element* top = ui.find(topId);
    const core::dsl::Element* bottom = ui.find(bottomId);
    if (top == nullptr || bottom == nullptr) {
        std::cerr << "probe::assertDrawnAbove: element missing: '"
                  << (top == nullptr ? topId : bottomId) << "'\n";
        return false;
    }
    std::unordered_map<const core::dsl::Element*, const core::dsl::Element*> parentOf;
    for (const auto& root : ui.roots()) {
        std::vector<const core::dsl::Element*> stack{root.get()};
        while (!stack.empty()) {
            const core::dsl::Element* node = stack.back();
            stack.pop_back();
            for (const auto& child : node->children) {
                parentOf[child.get()] = node;
                stack.push_back(child.get());
            }
        }
    }
    auto chainOf = [&](const core::dsl::Element* leaf) {
        std::vector<const core::dsl::Element*> chain;
        for (const core::dsl::Element* node = leaf; node != nullptr;) {
            chain.push_back(node);
            const auto it = parentOf.find(node);
            node = it == parentOf.end() ? nullptr : it->second;
        }
        return chain;
    };
    const std::vector<const core::dsl::Element*> topChain = chainOf(top);
    const std::vector<const core::dsl::Element*> bottomChain = chainOf(bottom);
    // LCA = bottom 链上第一个同时出现在 top 链中的节点（同根必存在）
    std::size_t lcaTopIdx = 0;
    std::size_t lcaBottomIdx = 0;
    bool found = false;
    for (std::size_t k = 0; k < bottomChain.size() && !found; ++k) {
        const auto it = std::find(topChain.begin(), topChain.end(), bottomChain[k]);
        if (it != topChain.end()) {
            lcaBottomIdx = k;
            lcaTopIdx = static_cast<std::size_t>(it - topChain.begin());
            found = true;
        }
    }
    if (!found) {
        std::cerr << "probe::assertDrawnAbove: no common ancestor between '"
                  << topId << "' and '" << bottomId << "'\n";
        return false;
    }
    if (lcaTopIdx == 0 || lcaBottomIdx == 0) {
        std::cerr << "probe::assertDrawnAbove: '" << topId << "' / '"
                  << bottomId << "' 是祖先关系（绘制序由兄弟排序决定的对象"
                     "应是 LCA 下两侧的直接子级），调用方选错比较对象\n";
        return false;
    }
    // LCA 的直接子级 = 各自链上 LCA 的前一个节点
    const core::dsl::Element* topChild = topChain[lcaTopIdx - 1];
    const core::dsl::Element* bottomChild = bottomChain[lcaBottomIdx - 1];
    if (topChild->subtreeMaxZIndex > bottomChild->subtreeMaxZIndex) {
        return true;
    }
    auto label = [](const core::dsl::Element* e) {
        return e->id.empty() ? std::string("<unnamed>") : e->id;
    };
    std::cerr << "probe::assertDrawnAbove: '" << topId << "'（LCA 侧子级 '"
              << label(topChild) << "' subtreeMaxZ="
              << topChild->subtreeMaxZIndex << "）未压过 '" << bottomId
              << "'（LCA 侧子级 '" << label(bottomChild) << "' subtreeMaxZ="
              << bottomChild->subtreeMaxZIndex << "）——等值/更低 z 时稳定"
                 "排序保持 DOM 序，后者后绘盖住前者\n";
    return false;
}

/// RAII：临时把元素 onScrollOffsetChanged 换成计数器包装（仍转发原回调），
/// 析构时恢复原回调；元素不存在时为空操作。注意元素在下次 compose 重建
/// 树时即失效，guard 必须在对应 composeModuleFrame 之前销毁。
class SwapScrollCallback {
public:
    SwapScrollCallback(core::dsl::Ui& ui,
                       const std::string& elementId,
                       int* counter)
        : element_(ui.find(elementId)), counter_(counter) {
        if (element_ == nullptr) {
            return;
        }
        original_ = std::move(element_->onScrollOffsetChanged);
        element_->onScrollOffsetChanged = [this](float offset) {
            if (counter_ != nullptr) {
                ++(*counter_);
            }
            if (original_) {
                original_(offset);
            }
        };
    }

    ~SwapScrollCallback() {
        if (element_ != nullptr) {
            element_->onScrollOffsetChanged = std::move(original_);
        }
    }

    SwapScrollCallback(const SwapScrollCallback&) = delete;
    SwapScrollCallback& operator=(const SwapScrollCallback&) = delete;

private:
    core::dsl::Element* element_ = nullptr;
    std::function<void(float)> original_;
    int* counter_ = nullptr;
};

} // namespace probe