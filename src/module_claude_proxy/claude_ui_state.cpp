// src/module_claude_proxy/claude_ui_state.cpp
// R4（P-B2 续）：UI 数据缓存的后台刷新（审计 perf [13] + H2 / CPX-06）——
// 逐字自 claude_module.cpp 搬出（含注释），只把 this 成员换成状态引用
// （ProfileListState&）与服务指针显式入参。
//
// 与 claude_ui_state.h 同属「状态单元」：状态 struct 是纯值类型（无方法），
// 其（后台）刷新以自由函数的形态住在同一单元的 .cpp 里。
#include "claude_ui_state.h"

#include "core/logging.h"

#include <chrono>
#include <future>
#include <utility>

namespace module::claude {

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
    }
    if (wantRecent) {
        // 版本号取**查询前**快照（同旧实现）：查询期间落库的调用会让下一帧
        // 版本再对不上，宁可多查一次不漏显
        r.verBefore = svc->callLogVersion();
        auto rR = svc->recentCalls(3);
        if (rR.isOk()) {
            r.recent = std::move(rR).value();
            r.recentOk = true;
        } else {
            r.callsError = rR.errorMessage();
        }
    }
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
    applyDataRefresh(st, st.refreshFuture_.get());
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
    st.refreshFuture_ = std::async(std::launch::async, runDataRefresh, svc,
                                   wantProfiles, wantProfiles || verChanged);
}

} // namespace module::claude
