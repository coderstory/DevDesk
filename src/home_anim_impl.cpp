// src/home_anim_impl.cpp
// home_anim 配置缝实现（声明于 src/home_anim_bridge.h）。独立小 TU：探针链
// devdesk_home_anim 时不拖 app 层符号（settings_config.cpp.o 会传递依赖
// app::setClearColor 等——档位直读 app_config 行，不走 SettingsStore）。
#include "home_anim_bridge.h"

#include <chrono>
#include <cstdlib>

#include "core/app_config.h"  // listWithPrefix：直读 settings.homeAnimEnabled 行
#include "core/database.h"
#include "core/router.h"
#include "platform/reduced_motion.h"
#include "platform/window_visibility.h"

namespace home_anim {

static std::optional<bool> g_homeAnimEnabled;
static std::optional<bool> g_homeAnimReduceOverride;

int clamp01(int v) { return v < 0 ? 0 : v; }

bool enabled() {
    if (!g_homeAnimEnabled.has_value()) {
        // 直读 app_config 行（settings.homeAnimEnabled；值 = JSON bool 文本）。
        // 缺键/库未就绪/脏值 → 默认开（组合动效）。
        bool loaded = true;
        if (::core::data::SqliteDb* db = ::core::appDatabase()) {
            auto rows = ::core::appconfig::listWithPrefix(*db, "config.json:");
            if (rows.isOk()) {
                for (const auto& e : rows.value()) {
                    if (e.key == "config.json:homeAnimEnabled") {
                        loaded = (e.value == "true");
                        break;
                    }
                }
            }
        }
        g_homeAnimEnabled = loaded;
    }
    return *g_homeAnimEnabled;
}

void setEnabled(bool on) { g_homeAnimEnabled = on; }

bool reducedMotion() {
    if (g_homeAnimReduceOverride.has_value()) return *g_homeAnimReduceOverride;
    return ::platform::reduced_motion::systemReducedMotion();
}

void setReducedMotionOverride(std::optional<bool> on) { g_homeAnimReduceOverride = on; }

void resetForTests() {
    g_homeAnimEnabled.reset();
    g_homeAnimReduceOverride.reset();
}

double phaseSeconds() {
    // 独立壁钟（不耦合 app::fluidPhaseSeconds：探针链接面零 app 依赖）。
    using namespace std::chrono;
    static const auto epoch = steady_clock::now();
    return duration<double>(steady_clock::now() - epoch).count();
}

bool pumpWanted() {
    if (!enabled() || reducedMotion()) return false;
    ::core::Router* router = ::core::appRouter();
    if (router == nullptr) return false;
    if (router->currentPage() != ::core::PageId::Home) return false;
    return ::platform::window_visibility::mainWindowVisible();
}

}  // namespace home_anim
