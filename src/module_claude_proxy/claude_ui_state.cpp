// src/module_claude_proxy/claude_ui_state.cpp
// UI 数据缓存的后台刷新：状态 struct 是纯值类型，刷新以自由函数形态住在 .cpp。
#include "claude_ui_state.h"

#include "claude_ui_logic.h"   // wakeAfterPublish（结果就绪 → 唤醒主循环的唯一出口）
#include "core/logging.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace module::claude {

// 次序缝：唤醒发出时结果是否已就绪（true = 先就绪后唤醒）。
std::atomic<bool> g_dataRefreshWakeSawReady{false};

DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles) {
    DataRefresh r;
    r.wantProfiles = wantProfiles;
    if (wantProfiles) {
        auto listR = svc->listProfiles();
        if (listR.isOk()) {
            r.profiles = std::move(listR).value();
        } else {
            r.profilesError = listR.errorMessage();
        }
        // 激活查询失败静默按无激活
        auto aR = svc->getActiveProfile();
        if (aR.isOk()) {
            r.active = std::move(aR).value();
            r.hasActive = true;
        }
        // 共享配置片段与 profiles 同组取（脏标记同刷，省一次轮询）
        auto sR = svc->listSharedConfigs();
        if (sR.isOk()) {
            r.sharedConfigs = std::move(sR).value();
        } else {
            r.sharedConfigsError = sR.errorMessage();
        }
    }
    // 唤醒不在这里：此刻结果尚未发布，先唤醒就是「先唤醒、后就绪」（UI 抢跑）。
    // 唤醒收在 refreshDataCache 派发处的唯一出口 wakeAfterPublish()。
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
        // 片段快照随 profiles 一起落（失败保旧快照不清空 + 记日志，错误行下次重试自愈）
        if (r.sharedConfigsError.empty()) {
            st.cachedSharedConfigs_ = std::move(r.sharedConfigs);
            st.cachedSharedConfigsError_.clear();
        } else {
            st.cachedSharedConfigsError_ = r.sharedConfigsError;
            LOG_ERROR("共享配置片段加载失败: " + st.cachedSharedConfigsError_);
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
        // 冷启动兜底：快照从未装载（服务晚就绪 / 调用方未走 onStart）时本帧同步
        // 装载一次（首帧必须有快照不得空帧）；失败保持本路径逐帧重试
        applyDataRefresh(st, runDataRefresh(svc, true));
        return;
    }
    if (st.refreshFuture_.valid()) {
        return;   // 上一轮在途：不叠加（脏条件下帧仍成立，意图不丢）
    }
    const bool wantProfiles = st.dataDirty_;
    if (!wantProfiles) {
        return;   // 空闲帧零开销
    }
    if (!refreshDue(st)) {
        return;   // 节流窗口内：下帧再判（可见最多晚一个节流 tick + 1 帧）
    }
    st.lastRefreshPost_ = std::chrono::steady_clock::now();
    // 后台刷新：**先就绪、后唤醒**（与调用记录查询同一形态）。次序不能反：
    // async 在 callable 返回后才发布值，任务内先唤醒会让 UI 抢跑（结果没人上屏）。
    auto promise = std::make_shared<std::promise<DataRefresh>>();
    std::shared_future<DataRefresh> published = promise->get_future().share();
    st.refreshFuture_ = published;
    st.refreshWorker_ = std::async(
        std::launch::async,
        [svc, wantProfiles, promise, published]() {
            // 次序缝复位：本轮没探到「已就绪才唤醒」即判红
            g_dataRefreshWakeSawReady.store(false, std::memory_order_relaxed);
            try {
                promise->set_value(
                    runDataRefresh(svc, wantProfiles));  // ① 就绪
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            // ② 就绪之后才唤醒（唯一出口，内部先探真实就绪再发）
            wakeAfterPublish(published, g_dataRefreshWakeSawReady);
        });
}

} // namespace module::claude

// 次序缝导出（生产零调用点）
extern "C" bool claude_data_refresh_wake_saw_ready_for_test() {
    return module::claude::g_dataRefreshWakeSawReady.load();
}
