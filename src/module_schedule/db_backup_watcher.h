// 数据库每小时整点备份（need455）：挂在调度引擎唯一 1Hz 泵上（ScheduleEngine 每
// tick 末回调），跨过整点小时桶时调 core::dbmaint::createBackup（既有在线备份
// 核心）一次，滚动保留 kKeepBackups 份。失败只记日志，不弹窗不打扰。
//
// 相位：首个 tick 只记相位不备份（不做启动触发）；UTC 小时桶（UTC+8 时区与本地
// 整点重合；半小时时区有 ≤30min 偏移，本需求不涉及）。
// 开关：进程级状态（设置「数据库自动备份」，默认开）——关闭时不产出但相位照常推进。
// 并发契约：只在 tick 线程（主线程）调用，无锁。
#pragma once

#include <cstdint>
#include <filesystem>

namespace module::schedule {

// 进程级开关（设置页第五桥 / main 启动注入）；仅主线程调用。
void setHourlyDbBackupEnabled(bool on);
bool hourlyDbBackupEnabled();

class DbBackupWatcher {
public:
    // 滚动保留份数：24 份整点 = 最近 24 小时每小时一个恢复点。
    static constexpr int kKeepBackups = 24;

    // dbFile 为空 = 配置异常：每次 fire 只记一条 WARN 并返回 false，不崩。
    explicit DbBackupWatcher(std::filesystem::path dbFile,
                             int keepBackups = kKeepBackups);

    // 引擎每 tick 调用；返回 true = 本 tick 真的产出了一份新备份。
    bool tick(int64_t nowUnix);

    // 测试缝：复位相位（不清全局开关）
    void resetForTest();

private:
    bool fire();

    std::filesystem::path dbFile_;
    int keepBackups_;
    int64_t hourBucket_ = 0;  // 0 = 尚未对相位（首个 tick 只记不备）
};

}  // namespace module::schedule