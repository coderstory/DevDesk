// src/module_claude_proxy/claude_ui_logs.cpp
// 完整调用记录弹窗（过滤/分页/后台取数）；测试缝计数器定义在本文件。
#include "claude_ui_logs.h"

#include "claude_ui_logic.h"
#include "core/logging.h"

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/virtuallist.h>
#include <ui_kit/dialog_form.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <future>   // promise / shared_future（先就绪、后唤醒）
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace module::claude {

// 测试缝计数器（生产零读取点，文件末尾经 extern "C" 导出给无头用例）：
// 累计派发次数 + 最近一次实际执行查询的线程 id 哈希（worker 写入，0 = 还没跑过）。
std::atomic<int> g_callsQueryDispatches{0};
std::atomic<unsigned long long> g_callsQueryDispatchThread{0};

// 诊断计时缝：「打开弹窗 → 查询派发 → 结果上屏」各段 steady_clock 纳秒打点
//（0 = 未发生），无头用例读差值即得各段耗时（生产零读取点）。
std::atomic<long long> g_callsQueryDispatchNs{0};
std::atomic<long long> g_callsQueryWorkerStartNs{0};
std::atomic<long long> g_callsQueryWorkerDoneNs{0};
std::atomic<long long> g_callsQueryHarvestNs{0};
// 次序缝：唤醒那一刻真实探一次结果共享状态是否已就绪，把次序违规变成可判定断言。
std::atomic<bool> g_callsQueryWakeSawReady{false};

namespace {

// steady_clock 纳秒（唯一用途：上面四个计时缝）
long long steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 唯一跑在后台线程的调用记录 DB 入口（static：不碰成员）。当页行 + 总匹配数同
// 一次派发取；queryCalls 失败 → error 回传；countCalls 失败按本页行数兜底。
CallsPage runCallsQuery(ClaudeService* svc, ClaudeService::CallLogQuery q) {
    // 测试缝：本函数跑在哪个线程（派发计数在派发点记，见 refreshCallsCache）
    g_callsQueryDispatchThread.store(
        static_cast<unsigned long long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())),
        std::memory_order_relaxed);
    g_callsQueryWorkerStartNs.store(steadyNs(), std::memory_order_relaxed);
    CallsPage p;
    auto r = svc->queryCalls(q);
    if (r.isFail()) {
        p.error = r.errorMessage();
        return p;
    }
    p.rows = std::move(r).value();
    auto c = svc->countCalls(q);
    p.total = c.isOk() ? c.value() : static_cast<int>(p.rows.size());
    return p;
}

// 当前过滤条件 → 查询（渲染线程构造、值语义进后台；搜索/下拉/翻页共用）
ClaudeService::CallLogQuery callsQueryNow(const LogsState& st) {
    ClaudeService::CallLogQuery q;
    q.search = st.callsSearch_;
    q.statusClass =
        kCallsStatusClasses[std::clamp(st.callsStatusIdx_, 0, kCallsStatusCount - 1)];
    q.limit = kCallsPageSize;
    q.offset = st.callsPage_ * kCallsPageSize;
    return q;
}

// 置脏：下一次 compose（弹窗打开时）发一次读意图。debounce=true（搜索按键）时
// 顺延一个防抖窗口——窗口内继续按键只顺延，停手后派发一次；其余动作走 false。
void markCallsStale(LogsState& st, bool debounce) {
    st.callsPageDirty_ = true;
    st.callsEarliestQueryAt_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(debounce ? kCallsSearchDebounceMs : 0);
}

// 过滤条件变化：回第一页重查（搜索框按键 → 防抖；状态码下拉 → 立即）
void applyCallsFilter(LogsState& st, bool debounce) {
    st.callsPage_ = 0;
    st.callsScrollOffset_ = 0;
    markCallsStale(st, debounce);
}

// 落快照（只在渲染线程调）：成功替换行 + 总数；失败清空当页并提示
void applyCallsPage(LogsState& st, CallsPage p,
                    const std::function<void(const std::string&,
                                             const std::string&, bool)>& toast) {
    st.callsPageLoaded_ = true;   // 本轮已判定（失败也不再显示「加载中…」）
    if (!p.error.empty()) {
        st.allCalls_.clear();
        st.callsTotalMatches_ = 0;
        LOG_ERROR("调用记录加载失败: " + p.error);
        toast("加载失败", p.error, true);
        return;
    }
    st.allCalls_ = std::move(p.rows);
    st.callsTotalMatches_ = p.total;
    g_callsQueryHarvestNs.store(steadyNs(), std::memory_order_relaxed);
}

// 弹窗打开时 compose 每帧调，只做两件零 IO 的事——①在途读已就绪 → 搬进快照
//（wait_for(0) 不阻塞）；②脏 + 无在途 + 过防抖窗口 → 发一次读意图。失败不自动重试。
void refreshCallsCache(LogsState& st,
                       const std::function<void(const std::string&,
                                                const std::string&, bool)>& toast) {
    if (st.callsFuture_.valid() &&
        st.callsFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        CallsPage page = st.callsFuture_.get();   // shared_future::get 不失效
        st.callsFuture_ = {};                     // 显式清空 = 让出「在途」判据
        applyCallsPage(st, std::move(page), toast);
    }
    if (!st.callsPageDirty_ || st.callsFuture_.valid()) {
        return;   // 空闲帧零开销 / 上一轮在途不叠加（脏标记留到下帧再判）
    }
    if (std::chrono::steady_clock::now() < st.callsEarliestQueryAt_) {
        return;   // 搜索防抖窗口内：下帧再判（停手后一帧内派发）
    }
    ClaudeService* svc = ClaudeService::global();
    if (!svc) {
        st.callsPageDirty_ = false;
        CallsPage p;
        p.error = "服务未就绪（ClaudeService 未注册）";
        applyCallsPage(st, std::move(p), toast);
        return;
    }
    st.callsPageDirty_ = false;
    // 测试缝：派发点计数（在渲染线程上做 → 同帧可读，断言不靠时序）
    g_callsQueryDispatches.fetch_add(1, std::memory_order_relaxed);
    g_callsQueryDispatchNs.store(steadyNs(), std::memory_order_relaxed);
    // 后台查询：**先就绪、后唤醒**，次序不能反——async 在 callable 返回后才发布值，先唤醒会让 UI 抢跑。
    // 唤醒用 core::platform::requestUiUpdate()（app::requestUpdate 只编进 app main TU）；失败也要唤醒。
    auto promise = std::make_shared<std::promise<CallsPage>>();
    std::shared_future<CallsPage> published = promise->get_future().share();
    st.callsFuture_ = published;
    st.callsWorker_ = std::async(
        std::launch::async,
        [svc, q = callsQueryNow(st), promise, published]() {
            // 次序缝复位：本轮若没有「唤醒前探到已就绪」的记录，用例判红
            g_callsQueryWakeSawReady.store(false, std::memory_order_relaxed);
            try {
                promise->set_value(runCallsQuery(svc, q));   // ① 结果落到共享状态
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            // ② 就绪之后才唤醒（唯一出口，内部先探一次真实就绪再发）
            wakeAfterPublish(published, g_callsQueryWakeSawReady);
            g_callsQueryWorkerDoneNs.store(steadyNs(), std::memory_order_relaxed);
        });
}

} // namespace

// 「查看全部 →」：开弹窗 + 置脏（下一帧后台查第一页）。首帧显示「加载中…」占位
void openCallsDialog(LogsState& st) {
    st.callsPage_ = 0;
    st.callsScrollOffset_ = 0;
    st.callsDialogOpen_ = true;
    markCallsStale(st, false);
}

void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     LogsState& st,
                     const std::function<void(const std::string&,
                                              const std::string&, bool)>& toast) {
    // 只在弹窗打开时维持快照（关着不查库）；本帧只做零 IO 的两件事（见 refreshCallsCache）
    if (st.callsDialogOpen_) refreshCallsCache(st, toast);

    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::ExtraWide);
    const float h = 560.0f;
    const int rowH = 22;

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.calls";
    frame.width = ui_kit::DialogWidth::ExtraWide;
    frame.height = h;
    frame.open = st.callsDialogOpen_;
    frame.onOpenChange = [sp = &st](bool v) { sp->callsDialogOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "claude.dialog.calls.title", w, "调用记录",
                                    20.0f, t.colors.textPrimary, {.y = 20.0f});

            // 工具行：搜索框 + 状态码过滤 + 匹配总数
            components::input(ui, "claude.dialog.calls.search")
                .position(28.0f, 58.0f)
                .size(260.0f, 32.0f)
                .value(st.callsSearch_)
                .placeholder("搜索路径 / 方法…")
                .fontSize(12.0f)
                .theme(themeTokens)
                .onChange([sp = &st](const std::string& v) {
                    if (v == sp->callsSearch_) return;   // 无变化不重查（IME 组字等）
                    sp->callsSearch_ = v;
                    // 逐键只置脏 + 顺延防抖窗口，停手后派发一次后台查询
                    applyCallsFilter(*sp, true);
                })
                .onEnter([sp = &st] {
                    // EUI-NEO input 把 Escape 映射到 onEnter；显式动作不防抖，立即重查
                    if (!sp->callsSearch_.empty()) {
                        sp->callsSearch_.clear();
                        applyCallsFilter(*sp, false);
                    }
                })
                .build();

            // dropdown 无 position 接口 → 包定位 stack（仿 settings 下拉）
            ui.stack("claude.dialog.calls.status.ddwrap")
                .position(298.0f, 58.0f)
                .size(150.0f, 32.0f)
                .content([&] {
                    components::dropdown(ui, "claude.dialog.calls.status.dd")
                        .size(150.0f, 32.0f)
                        .theme(themeTokens)
                        .items(std::vector<std::string>(
                            kCallsStatusLabels,
                            kCallsStatusLabels + kCallsStatusCount))
                        .selected(st.callsStatusIdx_)
                        .open(st.callsStatusDdOpen_)
                        .onOpenChange([sp = &st](bool v) { sp->callsStatusDdOpen_ = v; })
                        .onChange([sp = &st](int idx) {
                            sp->callsStatusIdx_ =
                                std::clamp(idx, 0, kCallsStatusCount - 1);
                            applyCallsFilter(*sp, false);   // 下拉是明确动作：立即重查
                        })
                        .build();
                })
                .build();

            ui.text("claude.dialog.calls.count")
                .position(460.0f, 58.0f)
                .size(w - 460.0f - 28.0f, 32.0f)
                // 快照未装载（弹窗首帧，后台取数在路上）时不显示「0 条匹配」
                .text(st.callsPageLoaded_
                          ? (std::to_string(st.callsTotalMatches_) + " 条匹配")
                          : std::string())
                .fontSize(12.0f)
                .lineHeight(32.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textTertiary)
                .build();

            // 列表区：当页行 + scrollView（440 > 视口时出滚动条）
            if (!st.callsPageLoaded_) {
                // 弹窗首帧：读意图已发出、查询在途 → 显示加载态（不闪假空态）
                ui.text("claude.dialog.calls.loading")
                    .position(28.0f, 112.0f)
                    .size(w - 56.0f, 20.0f)
                    .text("加载中…")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
            } else if (st.allCalls_.empty()) {
                ui.text("claude.dialog.calls.empty")
                    .position(28.0f, 112.0f)
                    .size(w - 56.0f, 20.0f)
                    .text("(无匹配的调用记录)")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                // 当页行 + virtualList：行高 22 恒定，仅可见行进 slot 窗口重建；
                // 行 id 用绝对行号（测试缝按它找行）。overscan 一个视口高。
                const std::time_t now = std::time(nullptr);
                const float scrollY = 100.0f;
                const float scrollBottom = h - 60.0f - 12.0f;  // 底部按钮上方
                components::virtualList(ui, "claude.dialog.calls.scroll")
                    .position(28.0f, scrollY)
                    .size(w - 56.0f, scrollBottom - scrollY)
                    .itemCount(static_cast<int64_t>(st.allCalls_.size()))
                    .rowHeight(static_cast<float>(rowH))
                    .step(static_cast<float>(rowH))
                    .overscanViewports(1.0f)
                    .theme(themeTokens)
                    .offset(st.callsScrollOffset_)
                    .onChange([sp = &st](float v) { sp->callsScrollOffset_ = v; })
                    .row([&](eui::Ui& inner, const std::string& rowId,
                             int64_t index, float contentWidth,
                             float rowHeight) {
                        (void)rowId;
                        (void)rowHeight;
                        const auto idx = static_cast<size_t>(index);
                        if (idx >= st.allCalls_.size()) return;
                        const ClaudeService::CallLogEntry& c =
                            st.allCalls_[idx];
                        const std::string absId =
                            "claude.dialog.calls.row." + std::to_string(idx);
                        // 行包 stack：position 只在 stack 内生效
                        inner.stack(absId + ".wrap")
                            .size(contentWidth, static_cast<float>(rowH))
                            .content([&] {
                                std::string line =
                                    formatCallTime(c.startedAt, now) + "  " +
                                    c.method + " " + c.path + "  " +
                                    (c.statusCode > 0
                                         ? std::to_string(c.statusCode)
                                         : std::string("-")) +
                                    "  " + formatLatency(c.latencyMs);
                                // 无状态码行（超时/网络错误）行尾附错误摘要，
                                // 截 48 字节防挤爆行宽
                                if (c.statusCode <= 0 && !c.error.empty()) {
                                    line += "  " + truncateUtf8(c.error, 48);
                                }
                                const eui::Color rowColor =
                                    (c.statusCode >= 200 && c.statusCode < 300)
                                        ? t.colors.textSecondary
                                        : t.colors.error;
                                inner.text(absId)
                                    .position(0.0f, 2.0f)
                                    .size(contentWidth, 18.0f)
                                    .text(line)
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(rowColor)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            }

            // 底部：翻页 + 页码 + 关闭
            const int pages = std::max(
                1, (st.callsTotalMatches_ + kCallsPageSize - 1) / kCallsPageSize);
            const bool hasNext =
                (st.callsPage_ + 1) * kCallsPageSize < st.callsTotalMatches_;
            //（UAT5 米黄清理：底部翻页/关闭整组 surfaceElevated → WeChat Secondary 白底灰边）
            components::button(ui, "claude.dialog.calls.prev")
                .position(28.0f, h - 60.0f)
                .size(88.0f, 40.0f)
                .text("← 上一页")
                .fontSize(14.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .disabled(st.callsPage_ == 0)
                .onClick([sp = &st] {
                    if (sp->callsPage_ == 0) return;
                    --sp->callsPage_;
                    sp->callsScrollOffset_ = 0;
                    markCallsStale(*sp, false);   // 翻页：下一帧后台重查（不防抖）
                })
                .build();
            components::button(ui, "claude.dialog.calls.next")
                .position(128.0f, h - 60.0f)
                .size(88.0f, 40.0f)
                .text("下一页 →")
                .fontSize(14.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .disabled(!hasNext)
                .onClick([sp = &st] {
                    if ((sp->callsPage_ + 1) * kCallsPageSize >= sp->callsTotalMatches_) return;
                    ++sp->callsPage_;
                    sp->callsScrollOffset_ = 0;
                    markCallsStale(*sp, false);   // 翻页：下一帧后台重查（不防抖）
                })
                .build();
            ui.text("claude.dialog.calls.page")
                .position(228.0f, h - 60.0f)
                .size(332.0f, 40.0f)
                .text("第 " + std::to_string(st.callsPage_ + 1) + " / " +
                      std::to_string(pages) + " 页")
                .fontSize(12.0f)
                // 页码文本必须 verticalAlign=Center，否则 ink 从行框顶沿起排
                .lineHeight(40.0f)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
            components::button(ui, "claude.dialog.calls.close")
                .position(w - 28.0f - 120.0f, h - 60.0f)
                .size(120.0f, 40.0f)
                .text("关闭")
                .fontSize(14.0f)
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .border(1.0f, t.colors.borderDefault)
                .onClick([sp = &st] { sp->callsDialogOpen_ = false; })
                .build();
    });
}

} // namespace module::claude

// 测试缝导出（生产零调用点，extern "C" 免名字修饰）：暴露给无头用例做计数断言。
extern "C" int claude_calls_query_dispatches_for_test() {
    return module::claude::g_callsQueryDispatches.load();
}

extern "C" unsigned long long claude_calls_query_last_thread_for_test() {
    return module::claude::g_callsQueryDispatchThread.load();
}

// 诊断计时缝导出：四个计时点的纳秒时刻（0 = 未发生），用例读差值得各段耗时。
extern "C" long long claude_calls_query_dispatch_ns_for_test() {
    return module::claude::g_callsQueryDispatchNs.load();
}

extern "C" long long claude_calls_query_worker_start_ns_for_test() {
    return module::claude::g_callsQueryWorkerStartNs.load();
}

extern "C" long long claude_calls_query_worker_done_ns_for_test() {
    return module::claude::g_callsQueryWorkerDoneNs.load();
}

extern "C" long long claude_calls_query_harvest_ns_for_test() {
    return module::claude::g_callsQueryHarvestNs.load();
}

// 次序缝：最近一次唤醒发出时，结果共享状态是否**已经就绪**（true = 先就绪后唤醒）
extern "C" bool claude_calls_query_wake_saw_ready_for_test() {
    return module::claude::g_callsQueryWakeSawReady.load();
}
