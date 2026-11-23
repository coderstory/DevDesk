// 主窗活体流质背景 + 20fps 限帧泵（自 app_sidebar.cpp 拆出；纯搬移，语义零变更）。
// 接口仍在 app_sidebar.h（测试缝不变），本 TU 只承载实现。
#include "app_sidebar.h"
#include "app_sidebar_internal.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/window_effect_state.h"  // bug274：晕层 alpha 按窗口效果档位
#include "platform/window_visibility.h"  // 主窗可见态（平台接口隔离，业务层零 Win32）
#include "settings_config.h"

#include <core/render/render_backend.h>  // need217：fluid-fps 日志聚合 RenderFrameStats
#include <eui/platform.h>  // core::platform::requestUiUpdate / setFramePacerActive
#include <eui/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace app {

namespace internal {

// 确定性 0~1 hash（splash.cpp 墨染同款 sin 散列取小数）：卫星的角度/距离/大小
// 全由索引 hash 决定——帧间稳定不闪烁，只随相位缓慢漂移（有机噪声感）
float fluidHash01(int n) {
    const float x = std::sin(static_cast<float>(n) * 127.1f) * 43758.5453f;
    return x - std::floor(x);
}

} // namespace internal

namespace {

// ── 动态背景（UAT6 主窗活体流质）：进程级状态（测试缝复位见 reset）──
// 配置运行时镜像：首帧惰性读 SettingsStore（fluidBackgroundEnabled）一次，
// 此后设置页经 setFluidBackgroundEnabled 写（持久化由设置侧 persist 落库）。
std::optional<bool> g_fluidEnabled;
// 窗可见态测试覆写（nullopt = 跟平台查询走）；生产路径恒 nullopt
std::optional<bool> g_fluidVisibleMode;
// 相位源（秒）：71ce7d4c 起走壁钟 elapsed（fluidPhaseSeconds() =
// steady_clock::now() - g_fluidEpoch），不再由帧泵 onFrame 累加。旧 g_fluidPhase
// 只写死状态已随 W3.5 审计 B（F4）移除。注意：壁钟模式下最小化/藏托盘期间相位
// 照走、休眠恢复会跳变——原「隐藏冻结 + dt 钳 0.1s」语义待 elapsed helper 收口。
static std::chrono::steady_clock::time_point g_fluidEpoch = std::chrono::steady_clock::now();

// ── 20fps 限帧泵（bug#194，老板定值 note326）─────────────────────────────
// 旧方案：常驻 1x1 onFrame 驱动元素让 runtime 置 animating_ → 主循环按刷新率
// 满帧出帧（实测 60fps，动态背景是唯一持续动画时帧循环被它独占吃满）。
// 新方案：守护线程 50ms 节拍调 core::platform::requestFrame()（线程安全，EUI
// async worker 同款先例）唤醒恰好一帧 compose——流质位置走壁钟相位，50ms 一帧
// 即推进；其余时间主循环回空闲等事件（SDL_WaitEventTimeout），零出帧零渲染。
// 交互/其它动画不受限：它们各自驱动满帧率，流质只是搭车多画几帧。
constexpr int kFluidPumpPeriodMs = 50;  // 20fps（老板定值）
struct FluidPump {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> active{false};  // atomic：compose 线程写、泵线程判读，跨线程语义不走侥幸
    std::atomic<bool> stop{false};
    std::thread t;
    FluidPump() : t([this] { run(); }) {}
    ~FluidPump() {
        stop.store(true);
        cv.notify_all();
        t.join();
    }
    void run() {
        std::unique_lock<std::mutex> lk(m);
        while (!stop.load(std::memory_order_relaxed)) {
            if (!active.load(std::memory_order_relaxed)) {
                cv.wait(lk, [this] { return active.load(std::memory_order_relaxed) || stop.load(); });
                continue;
            }
            if (cv.wait_for(lk, std::chrono::milliseconds(kFluidPumpPeriodMs)) ==
                std::cv_status::timeout) {
                if (active.load(std::memory_order_relaxed) && !stop.load()) {
                    // requestUiUpdate（claude 后台任务同款唤醒出口，生产验证过）：
                    // 置 ui+frame 双请求旗标并 postEmptyEvent——单 requestFrame 的
                    // 唤醒在本机 GLFW/Cocoa 上实测不生效（2026-11-07 bug#194）
                    ::core::platform::requestUiUpdate();
                }
            }
        }
    }
    void setActive(bool a) {
        if (active.exchange(a) == a) return;  // compose 每帧都调：值不变零 notify
        ::core::platform::setFramePacerActive(a);  // EUI 主循环空闲限时等待的开关（见 glfw_app_main）
        cv.notify_all();
    }
    bool isActive() const { return active.load(std::memory_order_relaxed); }
};
FluidPump& fluidPump() {
    static FluidPump p;
    return p;
}

// 浅色档三色轮替（同族不同明度）：流质晕专用（need172 §2.3「kLightHalo 同款三色
// 口径」，自 drawFluidBackground 上提）。v2（§10.3）起侧栏收缩按钮粒子不再共用此
// 表——第三色 #A7F3D0 与侧栏浅底近同亮度是粒子「观感弱」主因之一，改用
// app_sidebar.cpp 的专用表（kParticleLight）。
const eui::Color kLightHalo[3] = {eui::Color("#07C160"),
                                  eui::Color("#34D399"),
                                  eui::Color("#A7F3D0")};

// 可见态门控查询（compose 每帧顶层调用，铁律防线）：最小化（iconic）或藏托盘
//（hidden）→ false。原生查询收在 platform::window_visibility（Windows =
// IsIconic/IsWindowVisible；macOS = GLFW_ICONIFIED/GLFW_VISIBLE），业务层零平台
// 宏。两平台的零成本兜底都是 EUI 主循环跳帧（hiddenToTray/iconified 分支不跑
// 主窗 update/compose/render），本查询再钉一道：不可见帧即便 compose 被调用
// 也不画流质、不挂泵、相位不推进；平台判定不了时接口按可见降级（fail-open）。
bool fluidWindowVisible() {
    if (g_fluidVisibleMode.has_value()) return *g_fluidVisibleMode;
    return ::platform::window_visibility::mainWindowVisible();
}

} // namespace

// ---- 动态背景（UAT6 主窗活体流质）----

bool fluidBackgroundEnabled() {
    if (!g_fluidEnabled.has_value()) {
        // 首帧惰性读库一次并缓存（此后每帧零 DB 查询）；库未就绪（无头测试等）
        // 按默认值，不制造每帧 ERROR 日志
        ::core::data::SqliteDb* db = ::core::appDatabase();
        if (db != nullptr) {
            const module::settings::SettingsStore store(db);
            g_fluidEnabled = store.load().fluidBackgroundEnabled;
        } else {
            g_fluidEnabled =
                module::settings::SettingsConfig{}.fluidBackgroundEnabled;
        }
    }
    return *g_fluidEnabled;
}

void setFluidBackgroundEnabled(bool enabled) { g_fluidEnabled = enabled; }

float fluidPhaseSeconds() {
    using namespace std::chrono;
    return duration<float>(steady_clock::now() - g_fluidEpoch).count();
}

void setFluidWindowVisibleMode(std::optional<bool> mode) {
    g_fluidVisibleMode = std::move(mode);
}

void resetFluidBackgroundForTests() {
    g_fluidEnabled.reset();
    g_fluidVisibleMode.reset();
    g_fluidEpoch = std::chrono::steady_clock::now();
    fluidPump().setActive(false);  // 泵随门控回厂（bug#194）：用例间不残留激活态
}

// 测试缝：20fps 泵当前门控态（drawFluidBackground 每帧按「开关开 && 窗可见」写入，
// bug#194 门控语义断言用；旧 onFrame 驱动元素已退役，树内无泵可断言）
bool fluidPumpActiveForTests() { return fluidPump().isActive(); }

// ── 动态背景（UAT6 主窗活体流质）：纯 DSL 层组合 ──
// UAT6.1：7 层半径缩至原 60% 的小晕 + 每层 2 颗卫星；相位/周期错开分布，
// 观感从「几大团」变「多小晕流动」。帧泵与门控同旧版。z 序最底，正文可读
// 性零影响。B5 起 14 层、角速度 0.070~0.166 rad/s（提速 ×2），整圈 38~90s。
void drawFluidBackground(eui::Ui& ui, const eui::Screen& screen) {
    // 门控即泵联动（bug#194）：可见且开启才画元素并维持 20Hz 泵激活；否则零元素
    // 且泵挂起——主循环回空闲等事件，零出帧零渲染（铁律语义不变）。
    const bool pumpActive = fluidBackgroundEnabled() && fluidWindowVisible();
    // 门控翻转边沿日志（#218 occlusion 证据面）：挂起/恢复各留一条痕，锁屏/遮挡
    // 态的「泵已停」可直接从日志判读；值不变零输出，不刷屏。首评必留痕
    //（optional：覆盖「首帧即遮挡」场景，static bool 初值会吞掉第一次挂起）。
    static std::optional<bool> s_lastPumpGateLogged;
    if (!s_lastPumpGateLogged.has_value() || *s_lastPumpGateLogged != pumpActive) {
        s_lastPumpGateLogged = pumpActive;
        LOG_INFO(pumpActive ? "流质帧泵恢复（开关开 + 窗可见）"
                            : "流质帧泵挂起（开关关或窗不可见/最小化/遮挡）");
    }
    fluidPump().setActive(pumpActive);
    if (!pumpActive) return;
    const auto fpsT0 = std::chrono::steady_clock::now();  // bug#194 单帧耗时口径

    const auto& t = ::core::theme::current();

    // bug274：半透/磨砂档窗体按档透出桌面（clearColor 单层定标），晕层在该两档
    // 降透明（×1.5，峰值 0.19→0.285）保持「活体流质」可辨；off 档维持原基线。
    const float haloBoost =
        ::core::window_effect_state::current() == "off" ? 1.0f : 1.5f;

    constexpr int kHalos = 14;           // B5（2026-11-02）：7→14 小晕，铺满窗口不挤成一坨
    constexpr float kMaxAlpha = 0.19f;   // 单层峰值（2026-11-01 重构 0.11→0.19：0.11 透不出卡面）
    constexpr int kSatsPerHalo = 3;      // 卫星/层（2→3 更细碎，液体感更强）
    // 层参数表（锚点 = 窗口比例；速度 rad/s；相位 rad 防共漂；半径 = 短边比例）
    // B5 提速 ×2：速度翻倍后漂移周期 ~75s→~38s，振幅同步下调（0.16/0.13→0.13/0.10）
    // 防「在绕圈」观感（方案 §10.6 拍板③，一次改完不分步）。
    constexpr float kAnchorX[kHalos] = {
        0.50f, 0.50f, 0.24f, 0.76f, 0.12f, 0.88f, 0.36f,   // 原 7
        0.62f, 0.38f, 0.68f, 0.32f, 0.05f, 0.95f, 0.44f};  // 新 7 补空白区
    constexpr float kAnchorY[kHalos] = {
        0.46f, 0.56f, 0.30f, 0.72f, 0.80f, 0.18f, 0.88f,   // 原 7
        0.38f, 0.66f, 0.88f, 0.24f, 0.52f, 0.42f, 0.08f};  // 新 7
    constexpr float kSpeedX[kHalos] = {
        0.084f, 0.072f, 0.142f, 0.116f, 0.102f, 0.134f, 0.088f,
        0.126f, 0.098f, 0.158f, 0.110f, 0.146f, 0.092f, 0.138f};
    constexpr float kSpeedY[kHalos] = {
        0.070f, 0.098f, 0.128f, 0.166f, 0.076f, 0.110f, 0.144f,
        0.120f, 0.084f, 0.152f, 0.096f, 0.136f, 0.104f, 0.078f};
    constexpr float kPhase[kHalos] = {
        0.0f, 1.7f, 3.1f, 4.9f, 2.3f, 5.8f, 4.0f,
        0.9f, 2.6f, 3.7f, 5.2f, 1.4f, 6.1f, 4.5f};
    // 半径扩到 14 个固定值（不引 hash：帧间稳定不闪）。极值 0.372/0.124 → 极值比
    // 恒 3.0（探针按此断言）。最大圆放 j=3 而非 j=0：alpha 随 j 递减，j=0 最浓，
    // 最大圆放最前会与最浓 alpha 叠加显得突兀（方案 §10.6 建议）。
    constexpr float kRadiusFrac[kHalos] = {
        0.240f, 0.124f, 0.264f, 0.372f, 0.216f, 0.204f, 0.228f,  // 原 7（最大移到 j=3）
        0.190f, 0.330f, 0.280f, 0.150f, 0.350f, 0.210f, 0.300f}; // 新 7

    const float phase = fluidPhaseSeconds();
    const float minDim = std::min(screen.width, screen.height);
    // need217 实测修正：retained 兄弟 run 扫描只发生在「同一父容器的连续子元素」上
    //（renderElementChildren），本函数顶层直建的 56 rect 是 56 个独立 root——
    // renderDirect 逐 root 调 renderElement 不经过 run 扫描，opt-in 永不生效（实测
    // hits/miss/rebuild 全零）。包一层全窗非交互 stack：子元素绝对坐标与 z 序不变，
    // run 在其子序列上成形。
    ui.stack("app.fluid.root")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .content([&] {
    for (int j = 0; j < kHalos; ++j) {
        // 漂移中心 = 锚点 + 异频异相振幅（层间不共振，慢到只有「液面在流」而无轨迹感）
        const float cx = screen.width * kAnchorX[j] +
                         screen.width * 0.13f *
                             std::sin(kSpeedX[j] * phase + kPhase[j]);
        const float cy = screen.height * kAnchorY[j] +
                         screen.height * 0.10f *
                             std::cos(kSpeedY[j] * phase + kPhase[j]);
        const float r = minDim * kRadiusFrac[j];
        // 内浓外淡：14 层时原式 (1 - 0.13j) 在 j=13 算出负 alpha（圆不可见/渲染异常），
        // 改每档 0.05 + 下限 0.30（方案 §10.6）；bug274 起乘 haloBoost（档位提亮）。
        const float alpha = kMaxAlpha * haloBoost *
                            std::max(0.30f, 1.0f - 0.05f * static_cast<float>(j));
        // 浅色档三色轮替：同族不同明度 → 液面深浅流动，而非一整块绿
        //（kLightHalo 已上提至匿名 namespace）；
        // 暗色档沿用双色（浅绿压深底会发灰光）。
        const eui::Color& color =
            t.isDark ? ((j % 2 == 0) ? t.colors.primary : t.colors.primaryHover)
                     : kLightHalo[j % 3];
        ui.rect("app.fluid.halo." + std::to_string(j))
            .position(cx - r, cy - r)
            .size(r * 2.0f, r * 2.0f)
            .color(color)
            .radius(r)  // 半径 = 半宽 → 正圆（splash 墨染同款手法）
            .opacity(alpha)
            // bug#263：不进 retained 低清/陈旧复用（need217 F-3 两个 opt-in 退役）。
            // 流质是常动内容——签名帧帧漂移：reuseFrames>0 时陈旧窗把可见刷新钉死在
            // compose/reuseFrames（10 帧窗 × 16fps pump ≈1.6Hz =「一秒一帧」）；而
            // reuseFrames==0 时稳帧阈值=2、签名永不重复 → 层建不起来，scale 形同虚设，
            // 等效直绘（= bug#194 验收基线：每帧推进、CPU 0.2-0.7%）。retained API
            // 保留给静态/缓变内容，流质 rect 直绘逐帧出画。
            .build();
        // 确定性 hash 卫星：层心外慢轨道漂移的墨点，角度/距离/大小全由索引
        // hash 定死，帧间稳定不闪烁（漂移只来自相位项）
        for (int b = 0; b < kSatsPerHalo; ++b) {
            const float ang = internal::fluidHash01(j * 31 + b * 7 + 2) * 6.2831853f +
                              phase * (0.020f + 0.011f * internal::fluidHash01(j * 13 + b * 5 + 3));
            const float dist =
                r * (0.62f + 0.30f * internal::fluidHash01(j * 17 + b * 3 + 4));
            const float br = r * (0.055f + 0.040f * internal::fluidHash01(j * 19 + b * 11 + 5));
            ui.rect("app.fluid.sat." + std::to_string(j) + "." +
                    std::to_string(b))
                .position(cx + std::cos(ang) * dist - br,
                          cy + std::sin(ang) * dist - br)
                .size(br * 2.0f, br * 2.0f)
                .color(color)
                .radius(br)
                .opacity(alpha * 0.45f)  // bug#263：与晕层同口径，不进 retained opt-in（见上）
                .build();
        }
    }
        });
    // 帧泵元素已退役（bug#194）：原常驻 1x1 onFrame 驱动（app.fluid.anim.driver，
    // md.tree.anim.driver 同款先例）令 runtime 置 animating_ 满刷新率持续出帧，
    // 是动态背景持续吃 GPU 的主因；出帧改由文件头 FluidPump 20Hz 自持泵线程驱动，
    // 树内不再有帧泵元素。相位不受影响：仍走壁钟 fluidPhaseSeconds()（71ce7d4c，
    // g_fluidPhase 写死状态已随 W3.5 审计 B（F4）清空）。
    // bug#194 帧率自证（DEVDESK_FLUID_FPS_LOG=1 才产出，生产零成本）：每 5s 记
    // 「流质可见帧」实际出帧率与单帧 compose 建元素耗时——限帧前后对比数据源。
    static const bool kFpsLog = std::getenv("DEVDESK_FLUID_FPS_LOG") != nullptr;
    if (kFpsLog) {
        static int frames = 0;
        static long long buildUsTotal = 0;
        // need217 F-4：聚合上帧 RenderFrameStats（compose 时刻读到的是上一已完帧）；
        // renderDirectCpuNs 摊到每帧——低清接入前后帧耗对比数据源。
        static long long rectTotal = 0, hitsTotal = 0, missTotal = 0;
        static long long rebuildTotal = 0, layerDrawTotal = 0, directNsTotal = 0;
        static auto windowStart = std::chrono::steady_clock::now();
        ++frames;
        buildUsTotal += std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - fpsT0).count();
        const auto& st = ::core::render::lastRenderFrameStats();
        rectTotal += st.rectDraws;
        hitsTotal += st.retainedLayerHits;
        missTotal += st.retainedLayerMisses;
        rebuildTotal += st.retainedLayerRebuilds;
        layerDrawTotal += st.retainedLayerDraws;
        directNsTotal += static_cast<long long>(st.renderDirectCpuNs);
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - windowStart).count();
        if (elapsed >= 5.0) {
            LOG_INFO("[fluid-fps] rate=" + std::to_string(frames / elapsed) +
                     "fps avg_build=" +
                     std::to_string(static_cast<double>(buildUsTotal) / frames) +
                     "us rect_total=" + std::to_string(rectTotal) +
                     " hits_total=" + std::to_string(hitsTotal) +
                     " miss_total=" + std::to_string(missTotal) +
                     " rebuild_total=" + std::to_string(rebuildTotal) +
                     " layerDraw_total=" + std::to_string(layerDrawTotal) +
                     " direct_cpu=" +
                     std::to_string(static_cast<double>(directNsTotal) / frames / 1000.0) +
                     "us_per_frame frames=" + std::to_string(frames) + " window=" +
                     std::to_string(elapsed) + "s");
            frames = 0;
            buildUsTotal = 0;
            rectTotal = hitsTotal = missTotal = 0;
            rebuildTotal = layerDrawTotal = directNsTotal = 0;
            windowStart = now;
        }
    }
}

} // namespace app
