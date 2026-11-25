#include "module_schedule/db_backup_watcher.h"

#include "core/db_backup.h"
#include "core/logging.h"

#include <utility>

namespace module::schedule {
namespace {
// 进程级开关（设置「数据库自动备份」，默认开）：单线程进程状态——只管主线程
// tick 读与设置桥写，无锁（与 fullscreenPopup 同口径）。
bool g_hourlyBackupEnabled = true;
}  // namespace

void setHourlyDbBackupEnabled(bool on) { g_hourlyBackupEnabled = on; }
bool hourlyDbBackupEnabled() { return g_hourlyBackupEnabled; }

DbBackupWatcher::DbBackupWatcher(std::filesystem::path dbFile, int keepBackups)
    : dbFile_(std::move(dbFile)),
      keepBackups_(keepBackups < 1 ? 1 : keepBackups) {}

void DbBackupWatcher::resetForTest() { hourBucket_ = 0; }

bool DbBackupWatcher::tick(int64_t nowUnix) {
    const int64_t bucket = nowUnix / 3600;  // UTC 小时桶（UTC+8 与本地整点重合）
    if (hourBucket_ == 0 || bucket < hourBucket_) {
        // 首个 tick 只对相位（不做启动触发）；时钟回拨同样只重对相位不触发
        hourBucket_ = bucket;
        return false;
    }
    if (bucket == hourBucket_) return false;
    // 跨过整点：睡眠/唤醒跨多桶只补一份（不连发），开关关闭也推进相位——
    // 重新打开不补已过去的整点
    hourBucket_ = bucket;
    if (!g_hourlyBackupEnabled) return false;
    return fire();
}

bool DbBackupWatcher::fire() {
    if (dbFile_.empty()) {
        LOG_WARN("小时备份跳过：数据库文件路径未知（备份源为空）");
        return false;
    }
    auto backup = core::dbmaint::createBackup(dbFile_, keepBackups_);
    if (backup.isFail()) {
        // 失败只记日志：下个整点自然重试，绝不弹窗打扰
        LOG_WARN("小时备份失败（等下个整点重试）: " + backup.errorMessage());
        return false;
    }
    LOG_INFO("小时备份完成: " + backup.value().string());
    return true;
}

}  // namespace module::schedule