// src/module_settings/github_sync_engine.cpp
// GitHub Sync 编排引擎实现。方法体自 settings_module.cpp 逐字搬运
//（docs/superpowers/plans/2026-10-16-sync-engine-extraction-plan.md T1），
// 只允许三类机械改动：① this->成员 → deps_.；② 出参 / staged_ 写 → SyncRunResult；
// ③ ::core::paths::* / ::core::appDatabase() / nowSec() → deps_ 缝（带空回退）。
// 零逻辑编辑：同输入同输出，行为零变化由既有 sync 单测 + 真机矩阵兜底。
#include "github_sync_engine.h"

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
#include "core/sql_dump.h"
#include "github_sync_logic.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace module::settings {

namespace {

// epoch 秒（节流窗口与备份文件名共用；deps.nowSec 的生产回退实现）。
int64_t systemNowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 整文件读回（小文件：database.sql 量级）。失败 → false。
bool readFileToString(const std::filesystem::path& f, std::string& out) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 整文件写入（原子替换）。失败 → false。
// 必须原子：拉取写笔记文件的同一时刻，UI 线程可能因 kDbImported 正在重读该文件；
// 截断式直写会让读方拿到半截内容（并被当成 lastSavedText_ 记住）。
bool writeFileFromString(const std::filesystem::path& f,
                         const std::string& content) {
    return core::atomicWriteFile(f, content, "笔记").isOk();
}

// 409 确认框的覆盖清单 = database.sql + 本轮本地有改动的笔记（Put 集）。
// 任何冲突分支（sql PUT / 笔记 PUT / 删除传播）都要带上——清单空着弹
// 确认框等于让用户在不知道会覆盖什么的情况下二次确认。
std::vector<std::string> buildOverwriteList(
    const std::vector<std::string>& putNoteNames) {
    std::vector<std::string> overwrite{
        std::string(gsync::kRemoteSqlPath) + "（数据库）"};
    for (const auto& name : putNoteNames) {
        overwrite.push_back(gsync::remoteNotePath(name));
    }
    return overwrite;
}

// 子步骤结果并入总结果（ok/summary 由调用方定夺；其余可观测字段全量并入）。
// 各字段在单条编排链上至多一个子步骤会写（pull 不写 branchHint/overwriteFiles，
// push 不写 dbImported），「先到先得」与抽取前 staged_ 累计写语义一致。
void mergeSubResult(SyncRunResult& dst, SyncRunResult src) {
    dst.conflict = dst.conflict || src.conflict;
    dst.publicRepo = dst.publicRepo || src.publicRepo;
    dst.dbImported = dst.dbImported || src.dbImported;
    if (dst.branchHint.empty()) dst.branchHint = std::move(src.branchHint);
    if (dst.overwriteFiles.empty()) dst.overwriteFiles = std::move(src.overwriteFiles);
    if (dst.deleteSkipHint.empty()) dst.deleteSkipHint = std::move(src.deleteSkipHint);
}

// 拉取摘要的备份段：库备份（只有导入 database.sql 时才有）与笔记备份名**分别点名**，
// 两者都空则整段不出现（不留空括号）。为什么两个都要报：两类备份同放 backups/ 同一
// 目录，摘要里只报笔记名会让用户以为库没备份 —— 真出事时不知道该拿哪份文件回滚。
std::string backupClauseText(const std::string& dbName, const std::string& notesName) {
    if (dbName.empty() && notesName.empty()) return {};
    std::string s = "（本地备份：";
    if (!dbName.empty()) {
        s += "库 " + dbName;
        if (!notesName.empty()) s += "、";
    }
    if (!notesName.empty()) s += "笔记 " + notesName;
    return s + "）";
}

} // namespace

GithubSyncEngine::GithubSyncEngine(SyncEngineDeps deps) : deps_(std::move(deps)) {}

std::unique_ptr<platform::github::GithubApi> GithubSyncEngine::makeApi(
    const std::string& token, const std::string& owner,
    const std::string& repo) const {
    if (deps_.apiFactory) return deps_.apiFactory(token, owner, repo);
    return std::make_unique<platform::github::GithubApi>(token, owner, repo);
}

// 后台线程可见的阶段文案（UI 每帧消费；空槽 = 丢弃）。
void GithubSyncEngine::publish(const std::string& text) {
    if (deps_.progress) deps_.progress(text);
}

// 退出取消位（退出闸置位后，各阶段边界尽快收束）。
bool GithubSyncEngine::aborted() const {
    return deps_.aborted && deps_.aborted();
}

int64_t GithubSyncEngine::now() const {
    return deps_.nowSec ? deps_.nowSec() : systemNowSec();
}

std::filesystem::path GithubSyncEngine::notesDir() const {
    return deps_.notesDir.empty() ? ::core::paths::markdownDir() : deps_.notesDir;
}

std::filesystem::path GithubSyncEngine::appDataDirPath() const {
    return deps_.appDataDir.empty() ? ::core::paths::appDataDir() : deps_.appDataDir;
}

std::filesystem::path GithubSyncEngine::sqlDumpPath() const {
    return deps_.sqlDumpFile.empty() ? ::core::paths::sqlDumpFile() : deps_.sqlDumpFile;
}

std::filesystem::path GithubSyncEngine::dbFilePath() const {
    return deps_.dbFile.empty() ? ::core::paths::sqliteDbFile() : deps_.dbFile;
}

// 动作分发（platform::github 的唯一调用点）。结果由调用方在后台线程结束后
// 搬进 UI 侧暂存（join 即 happens-before，无需原子）。
SyncRunResult GithubSyncEngine::run(SyncAction action,
                                    const std::string& repoUrl,
                                    const std::string& token,
                                    const std::string& branch) {
    SyncRunResult out;
    if (aborted()) return out;   // 退出中：不再碰 api/store/db
    const auto parts = platform::github::GithubApi::parseRepoUrl(repoUrl);
    std::string summary;
    bool ok = false;
    bool conflict = false;
    bool publicRepo = false;
    std::string branchHint;
    if (parts.first.empty() || parts.second.empty()) {
        summary = "仓库 URL 无法解析（需要 https://github.com/<owner>/<repo>）";
    } else {
        auto api = makeApi(token, parts.first, parts.second);
        switch (action) {
            case SyncAction::Test: {
                // 仓库可见性（public 黄字提醒，不硬拦）
                auto repo = api->getRepo();
                if (repo.isOk()) publicRepo = repo.value().isPrivate == false;
                auto report = api->testConnection(
                    [this](const platform::github::TestStep& step) {
                        publish(step.name + (step.ok ? " ✓" : " ✗"));
                    });
                ok = report.ok;
                summary = report.summary;
                break;
            }
            case SyncAction::Push: {
                auto r = push(*api, branch);
                ok = r.ok;
                summary = std::move(r.summary);
                mergeSubResult(out, std::move(r));
                break;
            }
            case SyncAction::Pull: {
                auto r = pull(*api, branch);
                ok = r.ok;
                summary = std::move(r.summary);
                mergeSubResult(out, std::move(r));
                break;
            }
            case SyncAction::PullThenPush: {
                auto pulled = pull(*api, branch);
                if (!pulled.ok) {
                    summary = "先拉取失败：" + pulled.summary;
                    mergeSubResult(out, std::move(pulled));   // 导入过库也保持标记
                    break;
                }
                mergeSubResult(out, std::move(pulled));
                auto pushed = push(*api, branch);
                ok = pushed.ok;
                summary = std::move(pushed.summary);
                mergeSubResult(out, std::move(pushed));
                if (ok) summary = "已拉取远端后再推送：" + summary;
                break;
            }
            case SyncAction::ValidateFirstSync:
            case SyncAction::FirstSync: {
                auto r = checkedFirstSync(*api, branch);
                ok = r.ok;
                summary = std::move(r.summary);
                publicRepo = r.publicRepo;
                mergeSubResult(out, std::move(r));
                break;
            }
            case SyncAction::None:
                break;
        }
        if (summary.empty()) summary = ok ? "完成" : "失败";
        out.logs = api->recentLog();
    }
    if (summary.empty()) summary = ok ? "完成" : "失败";
    out.ok = ok;
    out.summary = std::move(summary);
    out.conflict = out.conflict || conflict;
    out.publicRepo = out.publicRepo || publicRepo;
    if (out.branchHint.empty()) out.branchHint = std::move(branchHint);
    return out;
}

// 首次同步的唯一实现（启用开关与手动「立即同步」共用）：先跑测试连接三步
// （令牌 / 仓库可推 / 读写探针），过了才做首次方向判定。校验失败时 summary 即
// 失败摘要、返回 ok=false，调用方按各自的失败呈现处理（不在这里决定弹窗与否）。
SyncRunResult GithubSyncEngine::checkedFirstSync(
    platform::github::GithubApi& api, const std::string& branch) {
    SyncRunResult out;
    auto repo = api.getRepo();
    if (repo.isOk()) out.publicRepo = repo.value().isPrivate == false;
    auto report = api.testConnection(
        [this](const platform::github::TestStep& step) {
            publish(step.name + (step.ok ? " ✓" : " ✗"));
        });
    if (!report.ok) {
        out.summary = report.summary;
        return out;
    }
    auto first = firstSync(api, branch);
    if (!first.ok) {
        out.summary = first.summary;
        mergeSubResult(out, std::move(first));
        return out;
    }
    out.ok = true;
    out.summary = "校验通过；" + first.summary;
    mergeSubResult(out, std::move(first));
    return out;
}

// 首次方向：远端已有 database.sql → 先拉取覆盖（含备份）再推送；
// 远端没有 → 首次上传（push 内含分支自动创建）。
SyncRunResult GithubSyncEngine::firstSync(platform::github::GithubApi& api,
                                          const std::string& branch) {
    SyncRunResult out;
    if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
    publish("① 探测远端数据…");
    auto probe = api.getFile(gsync::kRemoteSqlPath, branch);
    const bool exists = probe.isOk() && probe.value().exists;
    // getFile 对 404 返回 ok(exists=false) 而非 NotFound 错误（「文件不在」
    // 是调用方要据以决策的状态，不是错误）——判 missing 只能看 exists 位，
    // 按错误码判是死代码：空仓首次启用会恒走「远端状态未知」失败分支。
    const bool missing = probe.isOk() && !probe.value().exists;
    switch (gsync::firstSyncChoice(exists, missing)) {
        case gsync::FirstSyncChoice::PullThenPush: {
            auto pulled = pull(api, branch);
            if (!pulled.ok) {
                out.summary = "首次拉取失败：" + pulled.summary;
                mergeSubResult(out, std::move(pulled));   // 导入过库也保持标记
                return out;
            }
            mergeSubResult(out, std::move(pulled));
            auto pushed = push(api, branch);
            if (!pushed.ok) {
                out.summary = pushed.summary;
                mergeSubResult(out, std::move(pushed));
                return out;
            }
            out.ok = true;
            out.summary = "已用远端数据覆盖本地并推送：" + pushed.summary;
            mergeSubResult(out, std::move(pushed));
            return out;
        }
        case gsync::FirstSyncChoice::Push: {
            auto pushed = push(api, branch);
            if (!pushed.ok) {
                out.summary = pushed.summary;
                mergeSubResult(out, std::move(pushed));
                return out;
            }
            out.ok = true;
            out.summary = "首次上传完成：" + pushed.summary;
            mergeSubResult(out, std::move(pushed));
            return out;
        }
        case gsync::FirstSyncChoice::Fail:
            out.summary = "探测远端失败：" + (probe.isFail()
                                                  ? probe.errorMessage()
                                                  : std::string("远端状态未知"));
            return out;
    }
    out.summary = "首次同步方向未知";
    return out;
}

// 推送：dump 全库 + 笔记 → PUT / DELETE（单文件粒度 = 单次 commit）。
SyncRunResult GithubSyncEngine::push(platform::github::GithubApi& api,
                                     const std::string& branch) {
    SyncRunResult out;
    auto* db = deps_.db;
    if (db == nullptr) {
        out.summary = "数据库未就绪";
        return out;
    }
    // ① 全库 dump
    if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
    publish("① 导出数据库…");
    const auto dumpFile = sqlDumpPath();
    auto exported = ::core::sql_dump::exportToFile(
        *db, dumpFile, ::core::sql_dump::defaultSkipTables());
    if (exported.isFail()) {
        out.summary = "导出失败：" + exported.errorMessage();
        return out;
    }
    std::string dump;
    if (!readFileToString(dumpFile, dump)) {
        out.summary = "读取导出文件失败";
        return out;
    }
    // 体积上限：PUT 单请求 100MB（base64 ×1.37）→ 原始 ≈73MB，
    // 超限明确报错而不是让远端拒收 413 后报「HTTP 413」。
    if (gsync::dumpTooLargeForRemote(dump.size())) {
        out.summary = "数据库导出过大，暂不支持同步（原始 " +
                      std::to_string(dump.size() / (1000 * 1000)) +
                      " MB，上限约 73 MB；请清理调用日志或待办数据后重试）";
        LOG_ERROR("Settings: GitHub Sync " + out.summary);
        return out;
    }

    // ② 分支：listTree 404 = 分支不存在 → 以仓库默认分支为基点自动创建。
    //   判据必须是 listTree 的错误码，不能拿 getFile 探：getFile 对 404 返回
    //   ok(exists=false) 而非 NotFound 错误，「文件不在」与「分支不在」分不开
    //   （按 getFile 判的 branchMissing 是死代码，分支自动创建从不触发）。
    publish("② 校验分支…");
    if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
    auto repo = api.getRepo();
    if (repo.isFail()) {
        out.summary = "读取仓库信息失败：" + repo.errorMessage();
        return out;
    }
    auto tree = api.listTree(branch);
    if (tree.isFail() && tree.errorCode() == core::ErrorCode::NotFound) {
        if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
        auto created = api.createRef(branch, repo.value().defaultBranch);
        if (created.isFail()) {
            out.summary = "创建分支 " + branch + " 失败：" +
                          created.errorMessage();
            return out;
        }
        out.branchHint = gsync::branchCreatedHint(branch);
        tree = api.listTree(branch);
    }
    if (tree.isFail()) {
        out.summary = "读取远端笔记树失败：" + tree.errorMessage();
        return out;
    }
    const std::string notesDirPrefix = gsync::kRemoteNotesDir;
    std::map<std::string, std::string> remoteNoteShas;   // 文件名 → 远端 sha
    for (const auto& entry : tree.value()) {
        if (entry.path.rfind(notesDirPrefix, 0) != 0 ||
            entry.path.size() <= notesDirPrefix.size() ||
            entry.path.compare(entry.path.size() - 3, 3, ".md") != 0) {
            continue;
        }
        remoteNoteShas.emplace(entry.path.substr(notesDirPrefix.size()),
                               entry.sha);
    }

    // ④ 全量判态（纯本地 + 已取到的树；零写入）：每个名字过 classifyNote →
    // pushActionForState。有任一 Conflict 就先把清单列进确认框并中止——
    // 不推任何笔记、不动远端（database.sql 也留到冲突判定之后才 PUT）。
    std::map<std::string, gsync::LocalNote> local;
    for (auto& [name, content] : collectLocalNotes()) {
        local.emplace(name, gsync::makeLocalNote(std::move(content)));
    }
    auto synced = deps_.store->syncedNoteRecords();
    if (synced.isFail()) {
        out.summary = "读取同步登记失败：" + synced.errorMessage();
        return out;
    }
    struct NotePlan {
        std::string name;
        gsync::NoteSyncState state;
        gsync::PushAction action;
    };
    std::vector<NotePlan> plans;
    std::vector<std::string> putNames;   // 覆盖清单用：本地有改动的笔记
    {
        std::set<std::string> names;
        for (const auto& [name, note] : local) {
            (void)note;
            names.insert(name);
        }
        for (const auto& [name, rec] : synced.value()) {
            (void)rec;
            names.insert(name);
        }
        for (const auto& name : names) {
            const auto rec = synced.value().find(name);
            const auto loc = local.find(name);
            const bool hasRecord = rec != synced.value().end();
            const bool localExists = loc != local.end();
            const bool localHashMatches =
                hasRecord && localExists &&
                !rec->second.contentHash.empty() &&
                rec->second.contentHash == loc->second.hash;
            const bool remoteExists = remoteNoteShas.count(name) != 0;
            const bool remoteShaMatches =
                hasRecord && remoteExists &&
                remoteNoteShas[name] == rec->second.sha;
            const auto state = gsync::classifyNote(
                hasRecord, localExists, localHashMatches, remoteShaMatches);
            const auto action = gsync::pushActionForState(
                state, remoteExists, remoteShaMatches);
            if (action == gsync::PushAction::Put) putNames.push_back(name);
            plans.push_back(NotePlan{name, state, action});
        }
    }
    for (const auto& plan : plans) {
        if (plan.action != gsync::PushAction::Conflict) continue;
        out.conflict = true;
        out.summary = plan.state == gsync::NoteSyncState::LocalDeleted
                          ? ("远端笔记 notes/" + plan.name +
                             " 已被其他设备改动，本地删除未传播（409 冲突）")
                          : ("笔记 notes/" + plan.name +
                             " 本地与远端都有改动（409 冲突）");
        // 弹窗「拉取远端后再推」的拉取会把远端内容下载回本地并更新登记
        // （Diverged 与 LocalDeleted+远端变 都在 pull 的 Download 集里），
        // 重判 InSync 后本轮推送即可通过——不会原地打转。
        out.overwriteFiles = buildOverwriteList(putNames);
        return out;
    }

    // ⑤ database.sql（sha 作冲突检测：远端已变 → 409）。sha 从②的树里取
    //（与笔记判态同源同一棵树；getFile 单独探一发是多余的请求）。
    publish("④ 上传 database.sql…");
    if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
    std::string sha;
    for (const auto& entry : tree.value()) {
        if (entry.path == gsync::kRemoteSqlPath) {
            sha = entry.sha;
            break;
        }
    }
    auto put = api.putFile(gsync::kRemoteSqlPath, dump,
                           "DevDesk sync: database.sql", sha, branch);
    if (put.isFail()) {
        if (put.errorCode() == core::ErrorCode::Conflict) {
            out.conflict = true;
            out.summary = "远端 database.sql 已被改动（409 冲突）";
            out.overwriteFiles = buildOverwriteList(putNames);
        } else {
            out.summary = "上传 database.sql 失败：" + put.errorMessage();
        }
        return out;
    }
    // 登记 PUT 响应的新 sha：下次拉取据此识别「远端未变」。
    if (!put.value().empty()) {
        if (auto rec = deps_.store->recordSyncedSqlSha(put.value());
            rec.isFail()) {
            LOG_WARN("Settings: database.sql 登记失败: " +
                     rec.errorMessage());
        }
    }

    // ⑥ 执行（PUT / DELETE），单文件粒度 = 单次远端 commit
    publish("⑤ 同步笔记…");
    int putCount = 0;
    int delCount = 0;
    int deleteSkipCount = 0;   // 安全栏跳过数（进 UI 提示）
    for (const auto& plan : plans) {
        // 退出取消位——每个 PUT/DELETE（含 store 登记写）前查一次
        if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
        const std::string path = gsync::remoteNotePath(plan.name);
        if (plan.action == gsync::PushAction::Put) {
            // shaIfMatch：LocalModified 用登记 sha（远端动过 → 409，真并发
            // 检测）；New 或登记缺 sha 用树里的现值（无 → 无 sha 创建）。
            std::string shaIfMatch;
            const auto rec = synced.value().find(plan.name);
            if (rec != synced.value().end() && !rec->second.sha.empty()) {
                shaIfMatch = rec->second.sha;
            } else if (remoteNoteShas.count(plan.name) != 0) {
                shaIfMatch = remoteNoteShas[plan.name];
            }
            auto r = api.putFile(path, local.at(plan.name).content,
                                 "DevDesk sync: " + path, shaIfMatch, branch);
            if (r.isFail()) {
                if (r.errorCode() == core::ErrorCode::Conflict) {
                    // 判态与 PUT 之间远端又变了（竞态）→ 仍走冲突路径
                    out.conflict = true;
                    out.summary = "远端笔记 " + path + " 已被改动（409 冲突）";
                    out.overwriteFiles = buildOverwriteList(putNames);
                } else {
                    out.summary = "上传笔记 " + path + " 失败：" +
                                  r.errorMessage();
                }
                return out;
            }
            // 登记 PUT 响应的新 sha + 本地内容 hash（hash 是下轮 InSync
            // 短路的依据）；新 sha 拿不到（响应解析失败）→ 登记空 sha。
            if (auto rec = deps_.store->recordSyncedNote(
                    plan.name, r.value(), local.at(plan.name).hash);
                rec.isFail()) {
                LOG_WARN("Settings: 笔记推送登记失败 " + path + ": " +
                         rec.errorMessage());
            }
            ++putCount;
            continue;
        }
        if (plan.action == gsync::PushAction::Delete) {
            // 安全栏：登记有而本地读不到 ≠ 本地已删。文件还在磁盘上
            //（本轮读内容失败等瞬态原因）→ 不删远端、不清登记，下轮再判。
            std::error_code existsEc;
            const bool stillThere =
                std::filesystem::exists(notesDir() / plan.name, existsEc) &&
                !existsEc;
            if (stillThere) {
                LOG_WARN("Settings: 本地文件仍在但未能读取，跳过删除远端 " +
                         path);
                ++deleteSkipCount;   // 跳过不能只进日志
                continue;
            }
            // 远端已无此文件（LocalDeleted 判态里 remoteExists=false）→
            // 无需 DELETE，只清登记。DELETETE 的 sha 用树里的现值（判态时
            // 已确认 == 登记 sha，即未被别的设备改过——C6）。
            if (remoteNoteShas.count(plan.name) != 0) {
                auto r = api.deleteFile(path,
                                        "DevDesk sync: delete " + path,
                                        remoteNoteShas[plan.name], branch);
                if (r.isFail()) {
                    out.summary = "删除远端笔记 " + path + " 失败：" +
                                  r.errorMessage();
                    return out;
                }
            }
            if (auto f = deps_.store->forgetSyncedNote(plan.name);
                f.isFail()) {
                LOG_WARN("Settings: 笔记删除登记清理失败 " + plan.name +
                         ": " + f.errorMessage());
            }
            ++delCount;
        }
        // Skip：InSync（未变）/ RemoteModified（远端领先，交由 pull）
    }
    out.summary = "已推送 database.sql 与 " + std::to_string(putCount) +
                  " 篇笔记" + (delCount > 0 ? "（删除 " + std::to_string(delCount) +
                                              " 篇）" : "");
    if (deleteSkipCount > 0) {
        // 安全栏跳过不能只进日志（静默失败面）→ 卡片黄字
        out.deleteSkipHint =
            "有 " + std::to_string(deleteSkipCount) +
            " 篇本地仍在但读取失败的笔记，已跳过远端删除（下轮重试）";
    }
    out.ok = true;
    return out;
}

// 拉取（全树列举 + 登记 sha 状态比对）：树列举 → database.sql 按登记 sha 判变 →
// 笔记按登记比对下载。远端已删的本地笔记保留（保守语义）；
// 远端库未变（树 sha == 登记）→ 跳过导入，本地未推改动不被旧数据覆盖。
SyncRunResult GithubSyncEngine::pull(platform::github::GithubApi& api,
                                     const std::string& branch) {
    SyncRunResult out;
    publish("① 列举远端文件树…");
    auto tree = api.listTree(branch);
    if (tree.isFail()) {
        out.summary = "列举远端文件树失败：" + tree.errorMessage();
        return out;
    }
    const std::string notesDirPrefix = gsync::kRemoteNotesDir;
    std::string remoteSqlSha;
    std::map<std::string, std::string> remoteNotes;   // 文件名 → 远端 sha
    for (const auto& entry : tree.value()) {
        if (entry.path == gsync::kRemoteSqlPath) {
            remoteSqlSha = entry.sha;
            continue;
        }
        if (entry.path.rfind(notesDirPrefix, 0) != 0 ||
            entry.path.size() <= notesDirPrefix.size() ||
            entry.path.compare(entry.path.size() - 3, 3, ".md") != 0) {
            continue;
        }
        remoteNotes.emplace(entry.path.substr(notesDirPrefix.size()),
                            entry.sha);
    }

    auto* db = deps_.db;
    if (db == nullptr) {
        out.summary = "数据库未就绪";
        return out;
    }
    if (deps_.store == nullptr) {
        out.summary = "同步存储未就绪";
        return out;
    }
    bool sqlImported = false;
    std::string notesBackupNameOut;   // 摘要用：库导入发生时的笔记备份目录名
    std::string dbBackupNameOut;      // 摘要用：覆盖本地库前落盘的库备份文件名

    // ② 笔记动作先算（纯本地，零网络）：拉取覆盖面 = 导入 database.sql 或
    // 下载笔记任一发生 → 先整目录备份笔记，再动库。
    auto synced = deps_.store->syncedNoteRecords();
    if (synced.isFail()) {
        out.summary = "读取笔记登记失败：" + synced.errorMessage();
        return out;
    }
    std::map<std::string, gsync::LocalNote> localNotes;
    for (auto& [name, content] : collectLocalNotes()) {
        localNotes.emplace(name, gsync::makeLocalNote(std::move(content)));
    }
    const auto actions = gsync::pullNoteActions(
        remoteNotes, localNotes, synced.value());
    int downloadCount = 0;
    for (const auto& action : actions) {
        if (action.kind == gsync::PullAction::Kind::Download) {
            ++downloadCount;
        }
    }
    auto sqlRec = deps_.store->syncedSqlSha();
    if (sqlRec.isFail()) {
        out.summary = "读取 database.sql 登记失败：" + sqlRec.errorMessage();
        return out;
    }
    const bool sqlImportPlanned =
        !remoteSqlSha.empty() && sqlRec.value() != remoteSqlSha;

    if (sqlImportPlanned || downloadCount > 0) {
        if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
        // 拉取覆盖前整目录备份笔记（与库备份同目录、独立计数、各保 5 份）。
        publish("② 备份本地数据…");
        std::string notesBackupName;
        auto nb = backupNotesDir(notesBackupName);
        if (nb.isFail()) {
            out.summary = "笔记备份失败，已中止拉取：" + nb.errorMessage();
            return out;
        }
        notesBackupNameOut = notesBackupName;
        if (sqlImportPlanned) {
            // 覆盖本地 DB 前必须先备份（备份失败即中止，绝不在无退路时覆盖）。
            // 出参名进摘要：用户要能从摘要直达 backups/ 里那份可回滚的文件。
            auto backup = backupLocalDb(*db, dbBackupNameOut);
            if (backup.isFail()) {
                out.summary = "备份失败，已中止拉取：" + backup.errorMessage();
                return out;
            }
            publish("③ 下载并导入数据库…");
            auto remote = api.getFile(gsync::kRemoteSqlPath, branch);
            if (remote.isFail()) {
                out.summary = "下载 database.sql 失败：" + remote.errorMessage();
                out.logs = api.recentLog();
                return out;
            }
            if (!remote.value().exists || remote.value().content.empty()) {
                out.summary = "远端 database.sql 不存在或为空";
                out.logs = api.recentLog();
                return out;
            }
            // 经 store 导入——begin/end 事件包住（订阅方暂停/恢复后台
            // 写者），成功发 imported（全局重载广播）+ PRAGMA 复位。
            auto imported =
                deps_.store->importDatabase(remote.value().content);
            if (imported.isFail()) {
                // 报错要点到**回滚点**：导入期失败是单事务整体回滚（库没动，见
                // sql_dump::importFromString），但 integrity_check 失败发生在导入
                // 已提交之后（库已换新）—— 两种情形此刻的退路都是 ② 步刚落盘的这份
                // 库备份，摘要实名给出（与成功摘要同口径：不让用户翻日志或翻
                // backups/ 目录猜文件名）。不报笔记备份：笔记同步（④ 步）还没跑，
                // 它此刻不是回滚点，报出来是噪音。
                out.summary = "导入失败：" + imported.errorMessage() +
                              backupClauseText(dbBackupNameOut, std::string());
                return out;
            }
            // 登记树 sha：下次拉取据此识别「远端未变」。
            if (auto rec = deps_.store->recordSyncedSqlSha(remoteSqlSha);
                rec.isFail()) {
                LOG_WARN("Settings: database.sql 登记失败: " +
                         rec.errorMessage());   // 只影响下轮跳过判定，不算失败
            }
            sqlImported = true;
            // app_config 表被 DROP 重建，本模块缓存指向旧行 → 通知 harvest 重载。
            out.dbImported = true;
        }
    }

    // ③ 笔记：按登记 sha 比对，只下载远端变过的
    publish("④ 同步笔记…");
    int dlCount = 0;
    int keepCount = 0;
    for (const auto& action : actions) {
        // 退出取消位——每次下载写盘 / 登记写库前查一次
        if (aborted()) { out.summary = "同步已中止（退出中）"; return out; }
        if (action.kind == gsync::PullAction::Kind::KeepLocal) {
            LOG_INFO("Settings: 远端已删除笔记 " + action.fileName +
                     "，本地保留（pull 不替远端传播删除）");
            // 清登记——远端被别设备删掉、本地未改的笔记若留着旧登记，
            // 下轮 push 按 InSync 短路永不重推（远端永远缺这篇），本地一改
            // 又带失效 sha PUT → 409/422 死循环。清掉后下轮 push 判 New，
            // shaIfMatch 取远端树现值（不逐篇 GET；远端无此名 →
            // 无 sha PUT 重建，local wins）。
            if (auto f = deps_.store->forgetSyncedNote(action.fileName);
                f.isFail()) {
                LOG_WARN("Settings: 笔记基线清理失败 " + action.fileName +
                         ": " + f.errorMessage());
            }
            ++keepCount;
            continue;
        }
        auto f = api.getFile(gsync::remoteNotePath(action.fileName), branch);
        if (f.isFail() || !f.value().exists) {
            out.summary = "下载笔记 " + action.fileName + " 失败：" +
                          (f.isFail() ? f.errorMessage()
                                      : std::string("远端已不存在"));
            return out;
        }
        // 嵌套笔记（远端可含子目录）：落盘前按需建父目录。失败即整拉取中止是有意
        // 为之（宁可停也不留半个状态），但**不该因为缺一个目录就中止** —— 建目录
        // 是这一次写入的必要步骤，不是用户的先决条件。
        const auto target = notesDir() / action.fileName;
        std::error_code mkEc;
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path(), mkEc);
        }
        if (mkEc || !writeFileFromString(target, f.value().content)) {
            out.summary = "写入笔记文件失败：" + action.fileName;
            return out;
        }
        // 登记树 sha + 下载内容的 hash：下次 push 据此 InSync 短路、
        // 下次 pull 据此跳过未变文件。
        if (auto rec = deps_.store->recordSyncedNote(
                action.fileName, remoteNotes[action.fileName],
                gsync::contentHash(f.value().content));
            rec.isFail()) {
            LOG_WARN("Settings: 笔记拉取登记失败 " + action.fileName + ": " +
                     rec.errorMessage());
        }
        ++dlCount;
    }
    out.summary = std::string(sqlImported ? "已导入 database.sql"
                                          : "database.sql 未变，未导入") +
                  "；下载 " + std::to_string(dlCount) + " 篇笔记" +
                  (keepCount > 0
                       ? "（远端已删 " + std::to_string(keepCount) +
                             " 篇，本地保留）"
                       : "") +
                  backupClauseText(dbBackupNameOut, notesBackupNameOut);
    out.ok = true;
    out.logs = api.recentLog();
    return out;
}

// 备份本地库到 backups/devdesk-<ts>.db（保留最近 5 份）。
// 先 wal_checkpoint 再整文件拷贝：WAL 里未回写的页不 checkpoint 会丢。
core::Result<void> GithubSyncEngine::backupLocalDb(core::data::SqliteDb& db,
                                                   std::string& outName) {
    const auto dbFile = dbFilePath();
    const auto dir = appDataDirPath() / gsync::kBackupDirName;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "创建备份目录失败：" + ec.message());
    }
    auto cp = db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    if (cp.isFail()) {
        LOG_WARN("Settings: wal_checkpoint 失败（继续备份）: " +
                 cp.errorMessage());
    }
    const std::string name = gsync::backupFileName(now());
    const auto target = dir / name;
    std::filesystem::copy_file(dbFile, target,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "拷贝数据库失败：" + ec.message());
    }
    gsync::pruneBackups(dir, gsync::kBackupPrefix, gsync::kBackupKeep);
    outName = name;
    return core::Result<void>::ok();
}

// 拉取覆盖前整目录备份笔记到 backups/notes-<ts>/（与库备份同目录、
// 前缀隔离、独立计数，各保 5 份）。**递归**拷贝 .md 并保留子目录结构（子目录笔记
// 也在回滚点里 —— pull 的删除传播按登记名删，嵌套笔记一旦被登记就会真删）；
// 笔记目录不存在 → 无可备份 = ok。
core::Result<void> GithubSyncEngine::backupNotesDir(std::string& outName) {
    const auto srcDir = notesDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(srcDir, ec)) {
        return core::Result<void>::ok();
    }
    const auto dir = appDataDirPath() / gsync::kBackupDirName;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "创建备份目录失败：" + ec.message());
    }
    const std::string name = gsync::notesBackupDirName(now());
    const auto target = dir / name;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        "创建笔记备份目录失败：" +
                                            ec.message());
    }
    for (std::filesystem::recursive_directory_iterator it(
             srcDir, std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const auto rel = std::filesystem::relative(it->path(), srcDir, fec);
        if (fec || rel.empty()) continue;
        const std::string fname = rel.generic_string();
        if (fname.size() < 3 ||
            fname.compare(fname.size() - 3, 3, ".md") != 0) {
            continue;
        }
        const auto dst = target / rel;
        std::filesystem::create_directories(dst.parent_path(), ec);
        if (ec) {
            return core::Result<void>::fail(
                core::ErrorCode::FileWriteFailed,
                "创建笔记备份子目录失败：" + dst.parent_path().string() +
                    ": " + ec.message());
        }
        std::filesystem::copy_file(
            it->path(), dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return core::Result<void>::fail(
                core::ErrorCode::FileWriteFailed,
                "拷贝笔记备份失败：" + fname + ": " + ec.message());
        }
    }
    gsync::pruneBackups(dir, gsync::kNotesBackupPrefix, gsync::kBackupKeep,
                        "");
    outName = name;
    return core::Result<void>::ok();
}

// 本地笔记快照（**相对**笔记目录的路径名 → 内容），递归收集。
// 为什么相对名：远端名 = 树路径去掉 `notes/` 前缀（见 push 的 remoteNoteShas 构造），
// 嵌套笔记在远端就是 `notes/sub/a.md` → 名字必须是 `sub/a.md`。`filename()` 单层收集
// 会让子目录笔记对同步完全隐身（用户以为「同步成功」却从未上传），且 Windows 上
// `generic_string()` 保证分隔符是 '/'、反斜杠不会漂进远端路径。
std::map<std::string, std::string> GithubSyncEngine::collectLocalNotes() const {
    std::map<std::string, std::string> out;
    std::error_code ec;
    const auto dir = notesDir();
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (std::filesystem::recursive_directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const auto rel = std::filesystem::relative(it->path(), dir, fec);
        if (fec || rel.empty()) continue;
        const std::string name = rel.generic_string();
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".md") != 0) {
            continue;
        }
        std::string content;
        if (readFileToString(it->path(), content)) {
            out.emplace(name, std::move(content));
        }
    }
    return out;
}

} // namespace module::settings
