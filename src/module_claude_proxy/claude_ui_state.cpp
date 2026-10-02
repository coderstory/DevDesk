// src/module_claude_proxy/claude_ui_state.cpp
// R4（P-B2 续）：UI 数据缓存的后台刷新（审计 perf [13] + H2 / CPX-06）——
// 逐字自 claude_module.cpp 搬出（含注释），只把 this 成员换成状态引用
// （ProfileListState&）与服务指针显式入参。
//
// 与 claude_ui_state.h 同属「状态单元」：状态 struct 是纯值类型（无方法），
// 其（后台）刷新以自由函数的形态住在同一单元的 .cpp 里。
#include "claude_ui_state.h"

#include "claude_ui_logic.h"   // wakeAfterPublish（结果就绪 → 唤醒主循环的唯一出口）
#include "core/logging.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace module::claude {

// 次序缝（批次 D 评审收尾 2026-10-02，口径同 claude_ui_logs.cpp 的
// g_callsQueryWakeSawReady）：页面级刷新发出唤醒时，结果共享状态是否**已就绪**
// （true = 先就绪、后唤醒）。文件末尾经 extern "C" 导出给无头用例。
std::atomic<bool> g_dataRefreshWakeSawReady{false};

DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles,
                           bool wantRecent) {
    DataRefresh r;
    r.wantProfiles = wantProfiles;
    r.wantRecent = wantRecent;
    if (wantProfiles) {
        auto listR = svc->listProfiles();
        if (listR.isOk()) {
            r.profiles = std::move(listR).value();
        } else {
            r.profilesError = listR.errorMessage();
        }
        // 激活查询失败静默按无激活（旧语义不变）
        auto aR = svc->getActiveProfile();
        if (aR.isOk()) {
            r.active = std::move(aR).value();
            r.hasActive = true;
        }
        // 批 2：共享配置片段与 profiles 同组取（脏标记同刷，省一次轮询）
        auto sR = svc->listSharedConfigs();
        if (sR.isOk()) {
            r.sharedConfigs = std::move(sR).value();
        } else {
            r.sharedConfigsError = sR.errorMessage();
        }
    }
    if (wantRecent) {
        // 版本号取**查询前**快照（同旧实现）：查询期间落库的调用会让下一帧
        // 版本再对不上，宁可多查一次不漏显
        r.verBefore = svc->callLogVersion();
        // 数据侧上限见 ProfileListState::kRecentCallsSnapshot 的注释：
        // 它不是显示上限（显示按内容框可用高推），只是「取多少条进快照」
        auto rR = svc->recentCalls(ProfileListState::kRecentCallsSnapshot);
        if (rR.isOk()) {
            r.recent = std::move(rR).value();
            r.recentOk = true;
        } else {
            r.callsError = rR.errorMessage();
        }
    }
    // 唤醒**不在这里**（批次 D 评审收尾 2026-10-02）：本函数由 worker lambda 调用，
    // 此刻结果还只在局部变量 r 里、尚未 set_value 发布 → 在这里唤醒就是「先唤醒、
    // 后就绪」（UI 抢跑消费唤醒位 → 结果没人上屏）。唤醒收在 refreshDataCache 派发
    // 处的唯一出口 wakeAfterPublish()：先 promise.set_value 发布，再唤醒。
    // 另：本函数在**后台线程**跑（compose 的 refreshDataCache 经 std::async 派发；
    // proxy worker 落库只推版本号、不产生 UI 事件），没有那一下唤醒，快照要等用户
    // 下一次动鼠标才被 compose 收割——这条路径连弹窗那种过渡动画的帧都没有。
    return r;
}

void applyDataRefresh(ProfileListState& st, DataRefresh r) {
    if (r.wantProfiles) {
        if (r.profilesError.empty()) {
            st.cachedProfiles_ = std::move(r.profiles);
            st.cachedProfilesError_.clear();
            st.dataDirty_ = false;   // list 查询成功才清脏（失败保持脏下帧重试）
            st.dataLoaded_ = true;   // 快照已装载：此后刷新全走后台
        } else {
            st.cachedProfiles_.clear();
            st.cachedProfilesError_ = r.profilesError;
            LOG_ERROR("Profile 列表加载失败: " + st.cachedProfilesError_);
        }
        st.cachedActive_ = std::move(r.active);
        st.cachedHasActive_ = r.hasActive;
        // 批 2：片段快照随 profiles 一起落（失败保旧快照不清空 + 记日志，
        // 错误行下次写操作重试后自愈；区块按错误态显示）
        if (r.sharedConfigsError.empty()) {
            st.cachedSharedConfigs_ = std::move(r.sharedConfigs);
            st.cachedSharedConfigsError_.clear();
        } else {
            st.cachedSharedConfigsError_ = r.sharedConfigsError;
            LOG_ERROR("共享配置片段加载失败: " + st.cachedSharedConfigsError_);
        }
    }
    if (r.wantRecent) {
        if (r.recentOk) {
            st.cachedRecent_ = std::move(r.recent);
            st.cachedCallsError_.clear();
            st.seenCallLogVersion_ = r.verBefore;
        } else {
            st.cachedRecent_.clear();
            st.cachedCallsError_ = r.callsError;
            LOG_ERROR("调用记录加载失败: " + st.cachedCallsError_);
        }
    }
}

void harvestDataRefresh(ProfileListState& st) {
    if (!st.refreshFuture_.valid()) return;
    if (st.refreshFuture_.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
        return;   // 还在跑
    }
    DataRefresh r = st.refreshFuture_.get();   // shared_future::get 不失效
    st.refreshFuture_ = {};                    // 显式清空 = 让出「在途」判据
    applyDataRefresh(st, std::move(r));
}

bool refreshDue(const ProfileListState& st) {
    return std::chrono::steady_clock::now() - st.lastRefreshPost_ >=
           std::chrono::milliseconds(ProfileListState::kRefreshIntervalMs);
}

void refreshDataCache(ProfileListState& st, ClaudeService* svc) {
    harvestDataRefresh(st);
    if (!st.dataLoaded_) {
        // 冷启动兜底：快照从未装载过（服务晚就绪 / 调用方未走 onStart）时
        // 本帧同步装载一次。首帧必须有快照（硬约束：不得空帧），异步首刷
        // 必然先渲染一帧空列表 → 这一帧的同步查询是刻意的、只发生一次；
        // 装载成功即转后台（失败保持本路径逐帧重试 = 旧「每帧查」同退化）
        applyDataRefresh(st, runDataRefresh(svc, true, true));
        return;
    }
    if (st.refreshFuture_.valid()) {
        return;   // 上一轮在途：不叠加（脏/版本条件下帧仍成立，意图不丢）
    }
    const uint64_t ver = svc->callLogVersion();
    const bool wantProfiles = st.dataDirty_;
    const bool verChanged = (ver != st.seenCallLogVersion_);
    if (!wantProfiles && !verChanged) {
        return;   // 空闲帧零开销（与旧实现一致：版本没动就不查）
    }
    if (!refreshDue(st)) {
        return;   // 节流窗口内：下帧再判（可见最多晚一个节流 tick + 1 帧）
    }
    st.lastRefreshPost_ = std::chrono::steady_clock::now();
    // 后台刷新：**先就绪、后唤醒**（批次 D 评审收尾 2026-10-02，与 claude_ui_logs.cpp
    // 的调用记录查询同一形态）。次序不能反：std::async 在 callable 返回之后才发布
    // 值，任务内先唤醒会让 UI 抢跑（compose 收割看到「未就绪」→ 唤醒位已被消费 →
    // 结果没人上屏）。框架惯例见 EUI-NEO core/async::finishTask / image_source.cpp:949。
    auto promise = std::make_shared<std::promise<DataRefresh>>();
    std::shared_future<DataRefresh> published = promise->get_future().share();
    st.refreshFuture_ = published;
    st.refreshWorker_ = std::async(
        std::launch::async,
        [svc, wantProfiles, wantRecent = wantProfiles || verChanged, promise,
         published]() {
            // 次序缝复位（同 claude_ui_logs.cpp）：本轮没探到「已就绪才唤醒」即判红
            g_dataRefreshWakeSawReady.store(false, std::memory_order_relaxed);
            try {
                promise->set_value(
                    runDataRefresh(svc, wantProfiles, wantRecent));  // ① 就绪
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            // ② 就绪之后才唤醒（唯一出口，内部先探真实就绪再发）
            wakeAfterPublish(published, g_dataRefreshWakeSawReady);
        });
}

} // namespace module::claude

// ============ 批次 D 次序缝（生产零调用点，照 CPX-06 测试缝先例）============
extern "C" bool claude_data_refresh_wake_saw_ready_for_test() {
    return module::claude::g_dataRefreshWakeSawReady.load();
}
