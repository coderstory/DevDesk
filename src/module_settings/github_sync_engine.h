// src/module_settings/github_sync_engine.h
// GitHub Sync 编排引擎（自 settings_module.cpp 抽出，见
// docs/superpowers/plans/2026-10-16-sync-engine-extraction-plan.md）：
// push / pull / firstsync / 动作分发 / 备份 / 覆盖清单装配全部在此，
// SettingsModule 只留 UI 接线 + 后台线程 + 每帧收割 + 弹窗渲染。
//
// 可单测性：所有外依赖走构造注入（对齐 agent_module.h 的 Deps 风格）——
// 假发送器经 apiFactory 建带 setSender 的 GithubApi（spec §4「发送器与 sleep
// 均可注入」）；时钟 / 进度 / 取消走 std::function 缝；路径与库显式注入，
// 空值回落生产默认（core::paths / core::appDatabase）。引擎零 UI 类型依赖、
// 零平台宏、不持线程：run() 是同步阻塞调用，线程仍由调用方拥有。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "github_sync_config.h"   // GithubSyncStore（登记 / 导入 / 配置）
#include "platform/github_api.h"  // GithubApi / ApiLogEntry

namespace module::settings {

// 同步动作（原 settings_module.cpp 匿名 namespace 的 SyncAction，移到这里共用；
// 弹窗「重试」按原动作重跑；PullThenPush = 409 冲突弹窗的主动作；
// FirstSync / ValidateFirstSync = 首次同步：先跑测试连接三步校验，过了才做首次
// 方向判定，见 GithubSyncEngine::checkedFirstSync。两者跑同一条路，差别只在失败
// 呈现——ValidateFirstSync 由启用开关触发、失败不弹窗（用户刚拨开关，弹窗等于
// 打断）；FirstSync 由手动「立即同步」触发、失败弹窗。校验强度必须一致：首次
// 同步要么拉远端覆盖本地库、要么上传全量，配置未经验证就动数据是不可接受的）。
enum class SyncAction { None, Push, Pull, Test, PullThenPush, FirstSync, ValidateFirstSync };

// 一次动作的结果：引擎产出、UI 侧消费；零 UI 类型依赖（无 eui / 无卡片结构）。
// 字段一一对应原 staged_* 暂存面（搬运语义不变：join 后由线程 lambda 搬进
// SettingsModule 的 staged_*，harvest 照旧搬到 UI 侧成员）。
struct SyncRunResult {
    bool ok = false;
    std::string summary;
    bool conflict = false;                     // 409 → UI 走冲突弹窗变体
    bool publicRepo = false;                   // 测试连接发现 public 仓 → 黄字
    std::string branchHint;                    // 分支被自动创建提示
    std::vector<std::string> overwriteFiles;   // 409 覆盖清单（原 stashOverwriteListForConflict）
    std::string deleteSkipHint;                // 删除传播安全栏跳过提示（原 staged_）
    bool dbImported = false;                   // 本轮导入过库 → UI 重载配置缓存
    std::vector<platform::github::ApiLogEntry> logs;  // 弹窗日志源（原 api.recentLog()）
};

// 引擎依赖（构造注入）。生产传真实实现；测试传 fake —— 对齐 agent_module.h 的
// Deps 风格。路径 / 工厂的空值回落语义：空 = 用生产默认，不为本票加任何生产分支。
struct SyncEngineDeps {
    core::data::SqliteDb* db = nullptr;  // appDatabase；nullptr → 运行期报「数据库未就绪」
    GithubSyncStore* store = nullptr;    // 登记 / 导入 / 配置缓存重载

    // 时钟缝（空 = 生产真实系统时间；测试注入假时钟驱动节流/备份名）。
    std::function<int64_t()> nowSec;
    // 进度槽（引擎各阶段调；UI 侧在锁内落 pending）。空 = 丢弃。
    std::function<void(const std::string&)> progress;
    // 取消位（退出闸置位后各阶段边界收束）。空 = 永不中止。
    std::function<bool()> aborted;

    // 路径缝（空 = 生产 core::paths；测试注入临时目录）。
    std::filesystem::path notesDir;      // 空 → core::paths::markdownDir()
    std::filesystem::path appDataDir;    // 空 → core::paths::appDataDir()
    std::filesystem::path sqlDumpFile;   // 空 → core::paths::sqlDumpFile()
    std::filesystem::path dbFile;        // 空 → core::paths::sqliteDbFile()（备份拷贝源）

    // api 工厂（测试注入带假发送器的 GithubApi；生产按 token/owner/repo 建真 api）。
    // 空 → 生产默认实现。不需要给 GithubApi 造虚接口：假 api 走 setSender 注入缝。
    std::function<std::unique_ptr<platform::github::GithubApi>(
        const std::string& token, const std::string& owner,
        const std::string& repo)> apiFactory;
};

class GithubSyncEngine {
public:
    explicit GithubSyncEngine(SyncEngineDeps deps);

    // 执行一个动作（同步阻塞；调用方在后台线程跑）。repoUrl 解析失败 → 结果
    // ok=false、summary 带解析失败文案（与抽取前一致）。
    SyncRunResult run(SyncAction action, const std::string& repoUrl,
                      const std::string& token, const std::string& branch);

    // 分步编排（run 按 action 分发到这些；单测可直接调，绕过动作分发）。
    // push 内含：dump → 分支判定/自动创建 → 全量判态 → 409 中止 → PUT sql →
    // PUT/DELETE 笔记（含删除安全栏）。
    SyncRunResult push(platform::github::GithubApi& api, const std::string& branch);
    // pull 内含：全树列举 → sql 按登记 sha 判变 → 覆盖前双备份 → 导入 →
    // 笔记按登记 sha 下载（远端已删 → 本地保留 + 清登记）。
    SyncRunResult pull(platform::github::GithubApi& api, const std::string& branch);
    // 首次方向：远端有 sql → 先拉取覆盖（含备份）再推送；远端没有 → 首次上传
    //（push 内含分支自动创建）。
    SyncRunResult firstSync(platform::github::GithubApi& api, const std::string& branch);

private:
    // 首次同步的唯一实现（启用开关与手动「立即同步」共用）：先跑测试连接三步
    //（令牌 / 仓库可推 / 读写探针），过了才做首次方向判定。
    SyncRunResult checkedFirstSync(platform::github::GithubApi& api,
                                   const std::string& branch);

    SyncEngineDeps deps_;
    std::unique_ptr<platform::github::GithubApi> makeApi(
        const std::string& token, const std::string& owner, const std::string& repo) const;
    void publish(const std::string& text);
    bool aborted() const;
    int64_t now() const;
    std::filesystem::path notesDir() const;    // 空回退 core::paths
    std::filesystem::path appDataDirPath() const;
    std::filesystem::path sqlDumpPath() const;
    std::filesystem::path dbFilePath() const;
    // 内部：备份（库 / 笔记目录）、本地笔记快照（M5/M6/M7）
    core::Result<void> backupLocalDb(core::data::SqliteDb& db, std::string& outName);
    core::Result<void> backupNotesDir(std::string& outName);
    std::map<std::string, std::string> collectLocalNotes() const;
};

} // namespace module::settings
