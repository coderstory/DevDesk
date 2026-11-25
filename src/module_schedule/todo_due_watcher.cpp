#include "module_schedule/todo_due_watcher.h"

#include "core/logging.h"
#include "core/overdue.h"

#include <string>
#include <utility>
#include <vector>

namespace module::schedule {
namespace {

struct DueTask {
    int64_t id = 0;
    std::string title;
    int64_t dueAt = 0;
};

// 空标题兜底：body 里出现「」无法辨认是哪个任务
const std::string& displayTitle(const DueTask& task) {
    static const std::string kFallback = "未命名任务";
    return task.title.empty() ? kFallback : task.title;
}

}  // namespace

TodoDueWatcher::TodoDueWatcher(core::data::SqliteDb& db, ChannelRegistry& channels)
    : db_(db), channels_(channels) {}

void TodoDueWatcher::resetForTest() {
    lastScanAt_ = 0;
    notified_.clear();
}

void TodoDueWatcher::tick(int64_t nowUnix) {
    // 时钟回拨：以新时钟重开扫描窗口（账不丢，避免回拨后长时间不扫）
    if (lastScanAt_ != 0 && nowUnix < lastScanAt_) lastScanAt_ = nowUnix;
    if (lastScanAt_ != 0 && nowUnix - lastScanAt_ < kScanIntervalSec) return;
    lastScanAt_ = nowUnix;
    scan(nowUnix);
}

void TodoDueWatcher::scan(int64_t nowUnix) {
    const std::string sql =
        std::string("SELECT id, title, due_at FROM todo_tasks WHERE ") +
        core::overdue::kDueNowWhereSql + " ORDER BY due_at ASC, id ASC";
    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        LOG_WARN("todo.due: 到期任务查询准备失败 " + prep.errorMessage());
        return;
    }
    auto& stmt = prep.value();
    if (auto bound = stmt.bind(1, nowUnix); bound.isFail()) {
        LOG_WARN("todo.due: 到期时间绑定失败 " + bound.errorMessage());
        return;
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        LOG_WARN("todo.due: 到期任务读取失败 " + rows.errorMessage());
        return;
    }

    // 账重建：本轮仍在到期集里的任务保留原账（完成/改期/删除自动出账）
    std::map<int64_t, int64_t> nextLedger;
    std::vector<DueTask> fresh;
    for (const auto& row : rows.value()) {
        const int64_t id = row.getInt64("id").valueOr(0);
        const int64_t dueAt = row.getInt64("due_at").valueOr(0);
        if (id <= 0 || dueAt <= 0) continue;
        auto known = notified_.find(id);
        if (known != notified_.end() && known->second == dueAt) {
            nextLedger[id] = dueAt;
            continue;
        }
        fresh.push_back(DueTask{id, row.getString("title").valueOr(""), dueAt});
    }
    if (fresh.empty()) {
        notified_ = std::move(nextLedger);
        return;
    }

    // toast 渠道单槽覆盖（同轮分条只剩最后一条）：必须聚合成一条。超上限的任务
    // 不入账 → 下轮扫描重试，不能静默丢。
    const std::size_t count = std::min<std::size_t>(
        fresh.size(), static_cast<std::size_t>(kMaxTasksPerNotice));
    fresh.resize(count);

    Delivery delivery;
    delivery.level = 0;
    delivery.deeplink = "todo";
    if (count == 1) {
        delivery.title = "待办已到期";
        delivery.body = "「" + displayTitle(fresh[0]) + "」已到期";
        delivery.dedupeKey = "todo:due:" + std::to_string(fresh[0].id) + ":" +
                             std::to_string(fresh[0].dueAt);
    } else {
        delivery.title = "有 " + std::to_string(count) + " 个待办已到期";
        const std::size_t listed = std::min<std::size_t>(
            count, static_cast<std::size_t>(kMaxTitlesInBody));
        std::string body;
        for (std::size_t i = 0; i < listed; ++i) {
            if (i > 0) body += "、";
            body += "「" + displayTitle(fresh[i]) + "」";
        }
        delivery.body = body + (count > listed
                                    ? "等 " + std::to_string(count) + " 个任务已到期"
                                    : "已到期");
        delivery.dedupeKey = "todo:due:batch:" + std::to_string(nowUnix);
    }

    auto report = channels_.deliver({kChannelToast}, delivery);
    if (report.isFail()) {
        LOG_WARN("todo.due: 渠道投递失败 " + report.errorMessage());
    } else if (!report.value().allOk()) {
        LOG_WARN("todo.due: toast 渠道未接收（渠道未注册或内部失败），本次提醒丢失");
    }
    // 投递后无论成败都记账：渠道不可用时每 60s 重试只会刷日志，到期提醒属
    // 尽力而为（与调度引擎的失败重试链路不同域）。
    for (const auto& task : fresh) nextLedger[task.id] = task.dueAt;
    notified_ = std::move(nextLedger);
}

}  // namespace module::schedule