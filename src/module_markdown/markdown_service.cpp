// src/module_markdown/markdown_service.cpp —— 笔记文件读写 + 索引 / FTS5 搜索（接口见 markdown_service.h）。
#include "markdown_service.h"

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/str_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace module::markdown {

namespace {

// 把用户输入词清洗成安全 FTS5 词元：只保留字母/数字/下划线/非 ASCII 字节，
// 双引号包成 phrase（消灭 AND 等关键字语义），后跟 * 做 prefix 匹配。
std::string sanitizeFtsToken(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (unsigned char ch : token) {
        // 保留 ASCII 字母/数字/下划线 + 所有非 ASCII 字节（UTF-8 多字节）
        const bool keep = std::isalnum(ch) != 0 || ch == '_' || ch >= 0x80;
        if (keep) out.push_back(static_cast<char>(ch));
    }
    return out;
}

// FTS 查询计划。match = 安全 MATCH 表达式（"" = 跳过 FTS）；
// anyLossy = 有词元失真 → 需 LIKE 兜底；allLossy = 全部失真 → 只走 LIKE。
struct FtsQueryPlan {
    std::string match;
    bool anyLossy = false;
    bool allLossy = false;
};

// 拆词 → 安全 FTS5 MATCH 表达式 + 失真标记；空查询 → match=""（调用方处理）。
FtsQueryPlan buildFtsQuery(const std::string& raw) {
    std::istringstream iss(raw);
    std::string token;
    std::string joined;
    size_t total = 0;
    size_t lossy = 0;
    while (iss >> token) {
        ++total;
        std::string clean = sanitizeFtsToken(token);
        if (clean != token) ++lossy;
        if (clean.empty()) continue;
        if (!joined.empty()) joined += ' ';
        joined += '"';
        joined += clean;
        joined += "\"*";
    }
    FtsQueryPlan plan;
    plan.match = joined;
    plan.anyLossy = lossy > 0;
    plan.allLossy = total > 0 && lossy == total;
    return plan;
}

// LIKE 模式转义 % _ \（配合 ESCAPE '\'），实现已下沉 core::str。
using core::str::escapeLikePattern;

// SqlRow → NoteMeta
NoteMeta rowToNoteImpl(const core::data::SqlRow& row) {
    NoteMeta n;
    auto idR = row.getInt64("id");
    if (idR.isOk()) n.id = idR.value();
    auto pathR = row.getString("path");
    if (pathR.isOk()) n.relativePath = pathR.value();
    auto titleR = row.getString("title");
    if (titleR.isOk()) n.title = titleR.value();
    auto sizeR = row.getInt64("size_bytes");
    if (sizeR.isOk()) n.sizeBytes = sizeR.value();
    auto modR = row.getInt64("modified_at");
    if (modR.isOk()) n.modifiedAt = modR.value();
    return n;
}

// 文件 mtime → unix 秒；失败返回 -1。C++17 file_clock epoch 是实现定义的
//（MinGW 非 1970），故用 file_clock / system_clock 同刻 now 的差值换算。
int64_t fileMtimeEpoch(const fs::path& absPath) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(absPath, ec);
    if (ec) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch() +
        (mtime - std::chrono::file_clock::now())).count();
}

} // namespace


MarkdownService::MarkdownService(core::data::SqliteDb& db,
                                 const fs::path& notesDir)
    : db_(db), notesDir_(notesDir),
      watcher_(std::make_unique<platform::FileWatcher>()) {
    // 启动原生文件监听（平台矩阵见 platform/file_watcher.h）；watcher 要先于首扫存在。
    std::error_code ec;
    fs::create_directories(notesDir_, ec);
    auto r = watcher_->start(notesDir_,
                             [this](std::vector<platform::FileWatchEvent> ev) {
                                 onWatchEvent(std::move(ev));
                             });
    if (r.isFail()) {
        LOG_WARN("Markdown 文件监听启动失败，退回 mtime/size 轮询: " +
                 r.errorMessage());
    }
}

MarkdownService::~MarkdownService() {
    // 先 join watcher 线程，保证回调不再触碰任何成员
    if (watcher_) watcher_->stop();
}

bool MarkdownService::watcherActive() const {
    return watcher_ && watcher_->active();
}

bool MarkdownService::hasWatchCredit() const {
    // 仅 load 不 exchange：peek 语义，credit 留给 pollChanged 消费
    return watcherFired_.load();
}

void MarkdownService::onWatchEvent(std::vector<platform::FileWatchEvent> events) {
    // 1) 置标记：原生 watcher 上报监听根下一切变化，若不按相关性过滤，伴生文件
    //（数据库/临时文件/.DS_Store）会逐帧触发 rescan 风暴。只对以下事件置 credit：
    // Unknown（空路径）/ .md 路径 / 目录 / 已不存在的非 .md 路径；存在的非 .md
    // 普通文件对索引与目录树零影响，不置。stub 平台无事件。
    bool indexRelevant = false;
    for (const auto& ev : events) {
        if (ev.relativePath.empty()) {  // Unknown：无法定位，必须当相关处理
            indexRelevant = true;
            break;
        }
        // UTF-8 → fs::path 必须走 u8string（Windows 上 std::string→path 按 ACP 解码会乱码）。
        const fs::path rel{std::u8string(ev.relativePath.begin(),
                                         ev.relativePath.end())};
        if (rel.extension() == ".md") {
            indexRelevant = true;
            break;
        }
        // resolve() 失败（越界路径等）→ 保守当相关（宁多一轮 rescan 不漏扫）。
        const auto resolved = resolve(rel);
        if (resolved.isFail()) {
            indexRelevant = true;
            break;
        }
        std::error_code ec;
        if (!fs::is_regular_file(resolved.value(), ec)) {
            // 目录（存在）或已不存在的路径（可能是被删的目录）→ 保守相关
            indexRelevant = true;
            break;
        }
    }
    if (indexRelevant) watcherFired_.store(true);

    // 2) 即刻尽力同步索引（watcher 线程）。UI 持有 rescanMu_ 时跳过
    //（那轮 rescan 以磁盘为准收敛，下轮 pollChanged 仍会刷新）。
    std::unique_lock<std::mutex> lock(rescanMu_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    bool needFullRescan = false;
    for (const auto& ev : events) {
        if (ev.relativePath.empty()) {
            needFullRescan = true;
            continue;
        }
        const fs::path rel{std::u8string(ev.relativePath.begin(),
                                         ev.relativePath.end())};
        if (rel.extension() != ".md") continue;  // 目录/非笔记事件交给标记兜底
        const std::string relPosix = toPosix(rel);
        const auto resolved = resolve(rel);
        if (resolved.isFail()) {
            // 防御：watcher 事件不该越界；标记已置，下轮 poll 兜底
            LOG_WARN("watcher 事件路径越界，跳过 " + ev.relativePath + ": " +
                     resolved.errorMessage());
            continue;
        }
        const auto absPath = resolved.value();
        std::error_code ec;
        const bool exists = fs::is_regular_file(absPath, ec) && !ec;
        core::Result<void> one = exists
            // force 跳过 (size,mtime) 短路：同秒等长改写 mtime 比对失明
            ? syncOneToIndex(absPath, relPosix, /*force=*/true)
            : deleteFromIndex(relPosix);
        if (one.isFail()) {
            // 本轮放弃，标记已置，下轮 rescan 修正
            LOG_WARN("watcher 事件同步失败 " + ev.relativePath + ": " +
                     one.errorMessage());
        }
    }
    if (needFullRescan) {
        auto full = rescanLocked();
        if (full.isFail()) {
            LOG_WARN("watcher 触发的全量重扫失败: " + full.errorMessage());
        }
    }
}

core::Result<void> MarkdownService::rescan() {
    std::lock_guard<std::mutex> lock(rescanMu_);
    // 入口消费 credit：本轮遍历必然扫到消费点之前的改动；遍历期间新事件重新置位。
    watcherFired_.store(false);
    return rescanLocked();
}

core::Result<fs::path> MarkdownService::resolve(const fs::path& relPath) const {
    // containment 校验：先 lexically_normal 再逐组件前缀比较（字符串前缀会把
    // "/notes/evil" 误判成 "/notes" 内含）；两种逃逸形态都必须拒。写路径同此守卫。
    const fs::path base = notesDir_.lexically_normal();
    const fs::path candidate = (notesDir_ / relPath).lexically_normal();
    auto baseIt = base.begin();
    auto candIt = candidate.begin();
    for (; baseIt != base.end(); ++baseIt, ++candIt) {
        if (candIt == candidate.end() || *candIt != *baseIt) {
            return core::Result<fs::path>::fail(
                core::ErrorCode::ValidationError,
                "路径越出笔记目录，已拒绝: " + toPosix(relPath));
        }
    }
    return core::Result<fs::path>::ok(candidate);
}

std::string MarkdownService::toPosix(const fs::path& p) {
    return p.generic_string();
}

std::string MarkdownService::extractTitle(const std::string& content,
                                          const fs::path& relPath) {
    // 从第一非空行提取 "# Title"
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        size_t i = 0;
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        if (i >= line.size()) continue; // 空行
        if (line[i] != '#') break;
        ++i;
        while (i < line.size() && line[i] == '#') ++i;
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::string title = line.substr(i);
        while (!title.empty() &&
               (title.back() == ' ' || title.back() == '\t' ||
                title.back() == '\r' || title.back() == '\n')) {
            title.pop_back();
        }
        if (!title.empty()) return title;
    }
    // 没找到：回退到文件名（去扩展名）
    std::string name = relPath.stem().string();
    if (!name.empty()) return name;
    return relPath.filename().string();
}

core::Result<void> MarkdownService::syncOneToIndex(
    const fs::path& absPath,
    const std::string& relPathPosix,
    bool force) {

    std::error_code ec;
    auto fileSize = fs::file_size(absPath, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
            "无法获取文件大小 " + absPath.string() + ": " + ec.message());
    }
    auto mtimeSec = fileMtimeEpoch(absPath);
    if (mtimeSec < 0) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
            "无法获取 mtime " + absPath.string());
    }

    // 读内容
    std::ifstream ifs(absPath);
    if (!ifs) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
            "无法打开文件: " + absPath.string());
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    const std::string content = oss.str();

    const std::string title = extractTitle(content, relPathPosix);

    auto selPrep = db_.prepare(
        "SELECT id, size_bytes, modified_at FROM markdown_index WHERE path = ?");
    if (selPrep.isFail()) {
        return core::Result<void>::fail(selPrep.errorCode(),
            "prepare select index: " + selPrep.errorMessage());
    }
    {
        auto& stmt = selPrep.value();
        auto b = stmt.bind(1, relPathPosix);
        if (b.isFail()) {
            return core::Result<void>::fail(b.errorCode(),
                "bind path: " + b.errorMessage());
        }
        auto stepResult = stmt.step();
        if (stepResult.isFail()) {
            return core::Result<void>::fail(stepResult.errorCode(),
                "step select index: " + stepResult.errorMessage());
        }
        const bool exists = stepResult.value();

        if (exists) {
            auto rowResult = stmt.currentRow();
            if (rowResult.isFail()) {
                return core::Result<void>::fail(rowResult.errorCode(),
                    "currentRow: " + rowResult.errorMessage());
            }
            const auto& row = rowResult.value();
            int64_t oldSize = 0, oldMtime = 0;
            auto sR = row.getInt64("size_bytes");
            if (sR.isOk()) oldSize = sR.value();
            auto mR = row.getInt64("modified_at");
            if (mR.isOk()) oldMtime = mR.value();

            if (!force &&
                oldSize == static_cast<int64_t>(fileSize) &&
                oldMtime == static_cast<int64_t>(mtimeSec)) {
                // force=true 不走此短路：秒级比对对「同秒改回等长内容」失明
                return core::Result<void>::ok();
            }
        }
    }

    // INSERT OR IGNORE + UPDATE 两步保证幂等
    auto upsert = db_.transaction([&]() -> core::Result<void> {
        {
            auto prep = db_.prepare(
                "INSERT OR IGNORE INTO markdown_index "
                "(path, title, size_bytes, modified_at, created_at) "
                "VALUES (?, ?, ?, ?, ?)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare insert: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, relPathPosix);
            stmt.bind(2, title);
            stmt.bind(3, static_cast<int64_t>(fileSize));
            stmt.bind(4, static_cast<int64_t>(mtimeSec));
            stmt.bind(5, static_cast<int64_t>(mtimeSec));
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step insert: " + stepResult.errorMessage());
            }
        }
        // UPDATE（覆盖 INSERT IGNORE 未命中行的情况）
        {
            auto prep = db_.prepare(
                "UPDATE markdown_index SET title = ?, size_bytes = ?, "
                "modified_at = ? WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare update: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, title);
            stmt.bind(2, static_cast<int64_t>(fileSize));
            stmt.bind(3, static_cast<int64_t>(mtimeSec));
            stmt.bind(4, relPathPosix);
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step update: " + stepResult.errorMessage());
            }
        }
        // FTS5 须先删后插：path 列 UNINDEXED 无 UNIQUE，INSERT OR REPLACE
        // 会追加而非去重 → 旧内容残留、搜索命中过期数据。
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_fts WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare fts delete: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, relPathPosix);
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step fts delete: " + stepResult.errorMessage());
            }
        }
        {
            auto prep = db_.prepare(
                "INSERT INTO markdown_fts (path, title, content) "
                "VALUES (?, ?, ?)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare fts: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, relPathPosix);
            stmt.bind(2, title);
            stmt.bind(3, content);
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step fts: " + stepResult.errorMessage());
            }
        }
        return core::Result<void>::ok();
    });
    if (upsert.isFail()) {
        return core::Result<void>::fail(upsert.errorCode(),
            "index upsert: " + upsert.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::deleteFromIndex(
    const std::string& relPathPosix) {
    auto tx = db_.transaction([&]() -> core::Result<void> {
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_index WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare delete index: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, relPathPosix);
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step delete index: " + stepResult.errorMessage());
            }
        }
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_fts WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare delete fts: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, relPathPosix);
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step delete fts: " + stepResult.errorMessage());
            }
        }
        // 标签关联随 index 行删除（FK CASCADE），但 markdown_tags 标签名会留孤儿 —— 顺手清理
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_tags WHERE id NOT IN "
                "(SELECT DISTINCT tag_id FROM markdown_note_tags)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare delete orphan tags: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(),
                    "step delete orphan tags: " + stepResult.errorMessage());
            }
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) {
        return core::Result<void>::fail(tx.errorCode(),
            "delete from index: " + tx.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::rescanLocked() {
    std::error_code ec;
    fs::create_directories(notesDir_, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建笔记目录: " + ec.message());
    }

    // 1) 先取索引快照，再遍历磁盘 —— 顺序不能反：反了会把遍历期间写入的行
    // 误判为「磁盘已不存在」而误删（级联删标签）。
    auto prep = db_.prepare("SELECT path FROM markdown_index");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(),
            "prepare select paths: " + prep.errorMessage());
    }
    std::vector<std::string> indexedPaths;
    {
        auto& stmt = prep.value();
        auto rowsResult = stmt.fetchAll();
        if (rowsResult.isFail()) {
            return core::Result<void>::fail(rowsResult.errorCode(),
                "fetch paths: " + rowsResult.errorMessage());
        }
        indexedPaths.reserve(rowsResult.value().size());
        for (const auto& row : rowsResult.value()) {
            auto p = row.getString("path");
            if (p.isOk()) indexedPaths.push_back(p.value());
        }
    }

    // 2) 递归遍历磁盘，把所有 .md 文件 upsert 到索引
    std::set<std::string> foundPaths;
    for (auto it = fs::recursive_directory_iterator(
                    notesDir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!ec) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            const auto& absPath = entry.path();
            if (absPath.extension() != ".md") continue;

            auto relGeneric = fs::relative(absPath, notesDir_, ec);
            if (ec) continue;
            const std::string relPosix = relGeneric.generic_string();

            auto syncResult = syncOneToIndex(absPath, relPosix);
            if (syncResult.isFail()) {
                LOG_WARN("索引失败 " + absPath.string() + ": " +
                         syncResult.errorMessage());
                continue;
            }
            foundPaths.insert(relPosix);
        }
    }
    if (ec) {
        // 遍历中途出错 → foundPaths 残缺，按它剪枝会成批误删真实笔记。
        // 本轮放弃剪枝，下一轮以磁盘为准收敛。
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
            "遍历笔记目录时出错，本轮跳过索引剪枝: " + ec.message());
    }

    // 3) 删除「快照里有、磁盘上已不存在」的条目
    for (const auto& p : indexedPaths) {
        if (foundPaths.find(p) != foundPaths.end()) continue;
        auto delResult = deleteFromIndex(p);
        if (delResult.isFail()) {
            LOG_WARN("删除索引失败 " + p + ": " + delResult.errorMessage());
        }
    }

    // 4) 清理 FTS 孤儿行
    if (auto cleanup = db_.exec(
            "DELETE FROM markdown_fts WHERE path NOT IN "
            "(SELECT path FROM markdown_index)");
        cleanup.isFail()) {
        LOG_WARN("清理 FTS 孤儿行失败: " + cleanup.errorMessage());
    }

    LOG_INFO("MarkdownService rescan 完成，共索引 " +
             std::to_string(foundPaths.size()) + " 个文件");
    return core::Result<void>::ok();
}

core::Result<std::vector<NoteMeta>> MarkdownService::listAll() {
    auto prep = db_.prepare(
        "SELECT id, path, title, size_bytes, modified_at "
        "FROM markdown_index ORDER BY modified_at DESC");
    if (prep.isFail()) {
        return core::Result<std::vector<NoteMeta>>::fail(
            prep.errorCode(), "prepare listAll: " + prep.errorMessage());
    }
    auto& stmt = prep.value();
    auto rowsResult = stmt.fetchAll();
    if (rowsResult.isFail()) {
        return core::Result<std::vector<NoteMeta>>::fail(
            rowsResult.errorCode(),
            "fetch listAll: " + rowsResult.errorMessage());
    }
    std::vector<NoteMeta> out;
    out.reserve(rowsResult.value().size());
    for (const auto& row : rowsResult.value()) {
        out.push_back(rowToNoteImpl(row));
    }
    return core::Result<std::vector<NoteMeta>>::ok(std::move(out));
}

core::Result<std::vector<NoteMeta>> MarkdownService::listInDirectory(
    const fs::path& dir) {
    std::string prefix = dir.empty() ? "" : toPosix(dir);
    auto listResult = listAll();
    if (listResult.isFail()) {
        return listResult;
    }
    std::vector<NoteMeta> filtered;
    for (auto& n : listResult.value()) {
        const std::string& path = n.relativePath.generic_string();
        const auto pos = path.find_last_of('/');
        std::string parent;
        if (pos == std::string::npos) parent = "";
        else parent = path.substr(0, pos);

        if (parent == prefix) {
            filtered.push_back(std::move(n));
        }
    }
    return core::Result<std::vector<NoteMeta>>::ok(std::move(filtered));
}

core::Result<std::string> MarkdownService::read(const fs::path& relPath) {
    const std::string relPosix = toPosix(relPath);
    const auto resolved = resolve(relPath);
    if (resolved.isFail()) {
        return core::Result<std::string>::fail(resolved.errorCode(),
                                               resolved.errorMessage());
    }
    const auto absPath = resolved.value();
    std::ifstream ifs(absPath);
    if (!ifs) {
        return core::Result<std::string>::fail(
            core::ErrorCode::FileNotFound,
            "无法读取 " + absPath.string());
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return core::Result<std::string>::ok(oss.str());
}

core::Result<void> MarkdownService::write(const fs::path& relPath,
                                          const std::string& content) {
    const std::string relPosix = toPosix(relPath);
    const auto resolved = resolve(relPath);
    if (resolved.isFail()) {
        return core::Result<void>::fail(resolved.errorCode(),
                                        resolved.errorMessage());
    }
    const auto absPath = resolved.value();

    // 确保父目录存在
    const auto parent = absPath.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建父目录 " + parent.string() + ": " + ec.message());
    }

    // 原子写；失败码统一映射为 FileWriteFailed
    auto wr = core::atomicWriteFile(absPath, content, "笔记");
    if (wr.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        wr.errorMessage());
    }

    auto syncResult = syncOneToIndex(absPath, relPosix);
    if (syncResult.isFail()) {
        return syncResult;
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::create(const fs::path& relPath) {
    // trust boundary 在服务层
    if (!isSafeNewNotePath(toPosix(relPath))) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "路径不合法（不允许绝对路径、盘符或 ..）: " + toPosix(relPath));
    }
    // 无扩展名时补 .md
    fs::path norm = relPath;
    if (norm.extension() != ".md") {
        norm += ".md";
    }
    const std::string relPosix = toPosix(norm);
    const auto resolved = resolve(norm);
    if (resolved.isFail()) {
        return core::Result<void>::fail(resolved.errorCode(),
                                        resolved.errorMessage());
    }
    const auto absPath = resolved.value();

    if (fs::exists(absPath)) {
        return core::Result<void>::fail(core::ErrorCode::AlreadyExists,
            "文件已存在: " + absPath.string());
    }

    // 默认模板
    const std::string title = norm.stem().string();
    const std::string content = "# " + title + "\n\n";

    const auto parent = absPath.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建父目录: " + ec.message());
    }

    // 原子写；失败码统一映射为 FileWriteFailed
    auto wr = core::atomicWriteFile(absPath, content, "笔记");
    if (wr.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        wr.errorMessage());
    }

    auto syncResult = syncOneToIndex(absPath, relPosix);
    if (syncResult.isFail()) {
        // 回滚：删文件
        fs::remove(absPath, ec);
        return syncResult;
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::remove(const fs::path& relPath) {
    const std::string relPosix = toPosix(relPath);
    const auto resolved = resolve(relPath);
    if (resolved.isFail()) {
        // 越界路径拒绝删（否则是目录外任意文件删除缺口）
        return core::Result<void>::fail(resolved.errorCode(),
                                        resolved.errorMessage());
    }
    const auto absPath = resolved.value();

    // 不存在也允许删（索引照清）
    std::error_code ec;
    fs::remove(absPath, ec);

    return deleteFromIndex(relPosix);
}

core::Result<std::string> MarkdownService::rename(const fs::path& oldRelPath,
                                                  const fs::path& newRelPath) {
    const std::string oldPosix = toPosix(oldRelPath);
    if (!isSafeNewNotePath(toPosix(newRelPath))) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError,
            "新名不合法（不允许绝对路径、盘符或 ..）: " + toPosix(newRelPath));
    }
    // 无扩展名时补 .md
    fs::path norm = newRelPath;
    if (norm.extension() != ".md") {
        norm += ".md";
    }
    const std::string newPosix = toPosix(norm);
    if (newPosix == oldPosix) {
        return core::Result<std::string>::ok(oldPosix);  // 原地改名 = 无操作
    }

    std::error_code ec;
    const auto resolvedOld = resolve(oldRelPath);
    if (resolvedOld.isFail()) {
        return core::Result<std::string>::fail(resolvedOld.errorCode(),
                                               resolvedOld.errorMessage());
    }
    const auto resolvedNew = resolve(norm);
    if (resolvedNew.isFail()) {
        return core::Result<std::string>::fail(resolvedNew.errorCode(),
                                               resolvedNew.errorMessage());
    }
    const auto absOld = resolvedOld.value();
    const auto absNew = resolvedNew.value();
    if (!fs::exists(absOld, ec) || ec) {
        return core::Result<std::string>::fail(core::ErrorCode::FileNotFound,
            "原文件不存在: " + absOld.string());
    }
    if (fs::exists(absNew, ec)) {
        return core::Result<std::string>::fail(core::ErrorCode::AlreadyExists,
            "目标文件已存在: " + absNew.string());
    }
    fs::create_directories(absNew.parent_path(), ec);
    if (ec) {
        return core::Result<std::string>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建目标父目录 " + absNew.parent_path().string() + ": " + ec.message());
    }
    fs::rename(absOld, absNew, ec);
    if (ec) {
        return core::Result<std::string>::fail(core::ErrorCode::FileWriteFailed,
            "重命名失败 " + oldPosix + " → " + newPosix + ": " + ec.message());
    }

    // 读新文件内容（title 与 FTS 同源）；读失败不致命，下轮 rescan 修正。
    std::string content;
    {
        std::ifstream ifs(absNew);
        if (ifs) {
            std::ostringstream oss;
            oss << ifs.rdbuf();
            content = oss.str();
        }
    }
    auto fileSize = fs::file_size(absNew, ec);
    if (ec) fileSize = 0;
    const int64_t mtime = fileMtimeEpoch(absNew);
    const std::string title = extractTitle(content, norm);

    auto tx = db_.transaction([&]() -> core::Result<void> {
        {
            auto prep = db_.prepare(
                "UPDATE markdown_index SET path = ?, title = ?, "
                "size_bytes = ?, modified_at = ? WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare rename index: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, newPosix);
            stmt.bind(2, title);
            stmt.bind(3, static_cast<int64_t>(fileSize));
            stmt.bind(4, mtime);
            stmt.bind(5, oldPosix);
            auto step = stmt.step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step rename index: " + step.errorMessage());
            }
        }
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_fts WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare rename fts delete: " + prep.errorMessage());
            }
            prep.value().bind(1, oldPosix);
            auto step = prep.value().step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step rename fts delete: " + step.errorMessage());
            }
        }
        {
            auto prep = db_.prepare(
                "INSERT INTO markdown_fts (path, title, content) VALUES (?, ?, ?)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare rename fts insert: " + prep.errorMessage());
            }
            prep.value().bind(1, newPosix);
            prep.value().bind(2, title);
            prep.value().bind(3, content);
            auto step = prep.value().step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step rename fts insert: " + step.errorMessage());
            }
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) {
        // DB 失败 → 磁盘改回去（保持文件位置 = 索引 path 一致）
        std::error_code rbEc;
        fs::rename(absNew, absOld, rbEc);
        if (rbEc) {
            LOG_ERROR("重命名回滚失败（文件留在 " + newPosix + "，索引未变）: " +
                      rbEc.message());
        }
        return core::Result<std::string>::fail(tx.errorCode(),
            "rename 更新索引: " + tx.errorMessage());
    }
    LOG_INFO("Markdown 重命名笔记: " + oldPosix + " → " + newPosix);
    return core::Result<std::string>::ok(newPosix);
}

core::Result<std::vector<NoteMeta>> MarkdownService::search(
    const std::string& query) {
    if (query.empty()) {
        return listAll();
    }

    std::vector<NoteMeta> results;
    std::set<std::string> seen;

    auto merge = [&results, &seen](const std::vector<NoteMeta>& incoming) {
        for (const auto& n : incoming) {
            const std::string key = n.relativePath.generic_string();
            if (seen.insert(key).second) {
                results.push_back(n);
            }
        }
    };

    // ---- 路径 A：FTS5 MATCH（拉丁词，按 rank 排序）----
    const FtsQueryPlan plan = buildFtsQuery(query);
    if (!plan.match.empty() && !plan.allLossy) {
        auto prep = db_.prepare(
            "SELECT i.id, i.path, i.title, i.size_bytes, i.modified_at "
            "FROM markdown_fts f "
            "JOIN markdown_index i ON i.path = f.path "
            "WHERE markdown_fts MATCH ? "
            "ORDER BY rank");
        if (prep.isOk()) {
            auto& stmt = prep.value();
            if (stmt.bind(1, plan.match).isOk()) {
                auto rowsResult = stmt.fetchAll();
                if (rowsResult.isOk()) {
                    std::vector<NoteMeta> hits;
                    hits.reserve(rowsResult.value().size());
                    for (const auto& row : rowsResult.value()) {
                        hits.push_back(rowToNoteImpl(row));
                    }
                    merge(hits);
                } else {
                    LOG_WARN("FTS5 搜索失败: " + rowsResult.errorMessage());
                }
            }
        } else {
            LOG_WARN("FTS5 prepare 失败: " + prep.errorMessage());
        }
    }

    // ---- 路径 B：LIKE 字面子串兜底（CJK / 失真词元）----
    // unicode61 把连续 CJK 视为单个 token；失真词元 FTS 语义亦错，故走 LIKE。
    const bool hasNonAscii = std::any_of(
        query.begin(), query.end(),
        [](char c) { return static_cast<unsigned char>(c) >= 0x80; });

    if (hasNonAscii || plan.anyLossy || results.empty()) {
        const std::string pattern = "%" + escapeLikePattern(query) + "%";
        auto prep = db_.prepare(
            "SELECT i.id, i.path, i.title, i.size_bytes, i.modified_at "
            "FROM markdown_fts f "
            "JOIN markdown_index i ON i.path = f.path "
            "WHERE f.title LIKE ? ESCAPE '\\' "
            "   OR f.content LIKE ? ESCAPE '\\' "
            "ORDER BY i.modified_at DESC");
        if (prep.isFail()) {
            return core::Result<std::vector<NoteMeta>>::fail(
                prep.errorCode(), "prepare search like: " + prep.errorMessage());
        }
        auto& stmt = prep.value();
        if (stmt.bind(1, pattern).isFail() || stmt.bind(2, pattern).isFail()) {
            return core::Result<std::vector<NoteMeta>>::fail(
                core::ErrorCode::Internal, "bind search like pattern 失败");
        }
        auto rowsResult = stmt.fetchAll();
        if (rowsResult.isFail()) {
            return core::Result<std::vector<NoteMeta>>::fail(
                rowsResult.errorCode(),
                "fetch search like: " + rowsResult.errorMessage());
        }
        std::vector<NoteMeta> hits;
        hits.reserve(rowsResult.value().size());
        for (const auto& row : rowsResult.value()) {
            hits.push_back(rowToNoteImpl(row));
        }
        merge(hits);
    }

    return core::Result<std::vector<NoteMeta>>::ok(std::move(results));
}


namespace {

// 去掉 ASCII 首尾空白（定义已下沉 core::str）。
using core::str::trimAscii;

} // namespace

core::Result<std::vector<std::string>> MarkdownService::tagsForNote(
    const fs::path& relPath) {
    auto prep = db_.prepare(
        "SELECT t.name FROM markdown_note_tags nt "
        "JOIN markdown_tags t ON t.id = nt.tag_id "
        "JOIN markdown_index i ON i.id = nt.note_id "
        "WHERE i.path = ? ORDER BY t.name");
    if (prep.isFail()) {
        return core::Result<std::vector<std::string>>::fail(
            prep.errorCode(), "prepare tagsForNote: " + prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (stmt.bind(1, toPosix(relPath)).isFail()) {
        return core::Result<std::vector<std::string>>::fail(
            core::ErrorCode::Internal, "bind tagsForNote 失败");
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<std::string>>::fail(
            rows.errorCode(), "fetch tagsForNote: " + rows.errorMessage());
    }
    std::vector<std::string> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        if (auto n = row.getString("name"); n.isOk()) out.push_back(n.value());
    }
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

core::Result<void> MarkdownService::setTags(
    const fs::path& relPath, const std::vector<std::string>& tags) {
    const std::string relPosix = toPosix(relPath);
    // 清洗：去首尾空白、跳过空串、去重（保持首次出现序）
    std::vector<std::string> clean;
    std::set<std::string> seen;
    for (const auto& raw : tags) {
        std::string s = trimAscii(raw);
        if (s.empty()) continue;
        if (seen.insert(s).second) clean.push_back(std::move(s));
    }

    auto tx = db_.transaction([&]() -> core::Result<void> {
        int64_t noteId = -1;
        {
            auto prep = db_.prepare(
                "SELECT id FROM markdown_index WHERE path = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare setTags select note: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            if (stmt.bind(1, relPosix).isFail()) {
                return core::Result<void>::fail(core::ErrorCode::Internal,
                    "bind setTags select note 失败");
            }
            auto step = stmt.step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step setTags select note: " + step.errorMessage());
            }
            if (!step.value()) {
                return core::Result<void>::fail(core::ErrorCode::NotFound,
                    "笔记未索引，无法设置标签: " + relPosix);
            }
            auto row = stmt.currentRow();
            if (row.isFail() || row.value().getInt64("id").isFail()) {
                return core::Result<void>::fail(core::ErrorCode::Internal,
                    "读取笔记 id 失败: " + relPosix);
            }
            noteId = row.value().getInt64("id").value();
        }
        // 全量替换：删旧关联
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_note_tags WHERE note_id = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare setTags delete: " + prep.errorMessage());
            }
            auto& stmt = prep.value();
            if (stmt.bind(1, noteId).isFail()) {
                return core::Result<void>::fail(core::ErrorCode::Internal,
                    "bind setTags delete 失败");
            }
            auto step = stmt.step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step setTags delete: " + step.errorMessage());
            }
        }
        for (const auto& name : clean) {
            {
                auto prep = db_.prepare(
                    "INSERT OR IGNORE INTO markdown_tags (name) VALUES (?)");
                if (prep.isFail()) {
                    return core::Result<void>::fail(prep.errorCode(),
                        "prepare setTags insert tag: " + prep.errorMessage());
                }
                prep.value().bind(1, name);
                auto step = prep.value().step();
                if (step.isFail()) {
                    return core::Result<void>::fail(step.errorCode(),
                        "step setTags insert tag: " + step.errorMessage());
                }
            }
            int64_t tagId = -1;
            {
                auto prep = db_.prepare(
                    "SELECT id FROM markdown_tags WHERE name = ?");
                if (prep.isFail()) {
                    return core::Result<void>::fail(prep.errorCode(),
                        "prepare setTags select tag: " + prep.errorMessage());
                }
                prep.value().bind(1, name);
                auto step = prep.value().step();
                if (step.isFail() || !step.value()) {
                    return core::Result<void>::fail(core::ErrorCode::Internal,
                        "读取标签 id 失败: " + name);
                }
                auto row = prep.value().currentRow();
                if (row.isFail() || row.value().getInt64("id").isFail()) {
                    return core::Result<void>::fail(core::ErrorCode::Internal,
                        "读取标签 id 失败: " + name);
                }
                tagId = row.value().getInt64("id").value();
            }
            {
                auto prep = db_.prepare(
                    "INSERT OR IGNORE INTO markdown_note_tags "
                    "(note_id, tag_id) VALUES (?, ?)");
                if (prep.isFail()) {
                    return core::Result<void>::fail(prep.errorCode(),
                        "prepare setTags link: " + prep.errorMessage());
                }
                prep.value().bind(1, noteId);
                prep.value().bind(2, tagId);
                auto step = prep.value().step();
                if (step.isFail()) {
                    return core::Result<void>::fail(step.errorCode(),
                        "step setTags link: " + step.errorMessage());
                }
            }
        }
        // 清理无笔记引用的孤儿标签
        {
            auto prep = db_.prepare(
                "DELETE FROM markdown_tags WHERE id NOT IN "
                "(SELECT DISTINCT tag_id FROM markdown_note_tags)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                    "prepare setTags prune: " + prep.errorMessage());
            }
            auto step = prep.value().step();
            if (step.isFail()) {
                return core::Result<void>::fail(step.errorCode(),
                    "step setTags prune: " + step.errorMessage());
            }
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) {
        return core::Result<void>::fail(tx.errorCode(),
            "setTags: " + tx.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<TagCount>> MarkdownService::listAllTags() {
    auto prep = db_.prepare(
        "SELECT t.name AS name, COUNT(nt.note_id) AS n "
        "FROM markdown_tags t "
        "JOIN markdown_note_tags nt ON nt.tag_id = t.id "
        "GROUP BY t.id ORDER BY t.name");
    if (prep.isFail()) {
        return core::Result<std::vector<TagCount>>::fail(
            prep.errorCode(), "prepare listAllTags: " + prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<TagCount>>::fail(
            rows.errorCode(), "fetch listAllTags: " + rows.errorMessage());
    }
    std::vector<TagCount> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        TagCount tc;
        if (auto n = row.getString("name"); n.isOk()) tc.name = n.value();
        if (auto c = row.getInt64("n"); c.isOk()) tc.noteCount = c.value();
        out.push_back(std::move(tc));
    }
    return core::Result<std::vector<TagCount>>::ok(std::move(out));
}

core::Result<std::vector<NoteMeta>> MarkdownService::notesWithTag(
    const std::string& tag) {
    auto prep = db_.prepare(
        "SELECT i.id, i.path, i.title, i.size_bytes, i.modified_at "
        "FROM markdown_note_tags nt "
        "JOIN markdown_tags t ON t.id = nt.tag_id "
        "JOIN markdown_index i ON i.id = nt.note_id "
        "WHERE t.name = ? ORDER BY i.modified_at DESC");
    if (prep.isFail()) {
        return core::Result<std::vector<NoteMeta>>::fail(
            prep.errorCode(), "prepare notesWithTag: " + prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (stmt.bind(1, tag).isFail()) {
        return core::Result<std::vector<NoteMeta>>::fail(
            core::ErrorCode::Internal, "bind notesWithTag 失败");
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<NoteMeta>>::fail(
            rows.errorCode(), "fetch notesWithTag: " + rows.errorMessage());
    }
    std::vector<NoteMeta> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        out.push_back(rowToNoteImpl(row));
    }
    return core::Result<std::vector<NoteMeta>>::ok(std::move(out));
}

NoteMeta MarkdownService::rowToNote(const core::data::SqlRow& row) {
    return rowToNoteImpl(row);
}


core::Result<bool> MarkdownService::pollChanged() {
    // Credit 语义：watcher 报过事件 → 本次必返 true（消费该 credit），即使
    // (mtime,size) 比对失明也强制调用方跑一轮 rescan + UI 刷新。
    if (watcherFired_.exchange(false)) {
        return core::Result<bool>::ok(true);
    }

    // ---- mtime/size 比对：stub 平台 / watcher 掉线的兜底主路径 ----
    std::map<std::string, std::pair<int64_t, int64_t>> disk;
    std::error_code ec;
    if (fs::exists(notesDir_, ec)) {
        for (auto it = fs::recursive_directory_iterator(
                        notesDir_, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".md") continue;
            auto rel = fs::relative(entry.path(), notesDir_, ec);
            if (ec) continue;
            const int64_t mtime = fileMtimeEpoch(entry.path());
            const int64_t size = static_cast<int64_t>(entry.file_size(ec));
            if (mtime < 0 || ec) continue;
            disk[rel.generic_string()] = {mtime, size};
        }
    }

    // 索引侧快照
    auto prep = db_.prepare("SELECT path, size_bytes, modified_at FROM markdown_index");
    if (prep.isFail()) {
        return core::Result<bool>::fail(prep.errorCode(),
            "prepare pollChanged: " + prep.errorMessage());
    }
    std::map<std::string, std::pair<int64_t, int64_t>> indexed;
    {
        auto& stmt = prep.value();
        auto rows = stmt.fetchAll();
        if (rows.isFail()) {
            return core::Result<bool>::fail(rows.errorCode(),
                "fetch pollChanged: " + rows.errorMessage());
        }
        for (const auto& row : rows.value()) {
            auto p = row.getString("path");
            if (p.isFail()) continue;
            int64_t size = 0, mtime = 0;
            if (auto v = row.getInt64("size_bytes"); v.isOk()) size = v.value();
            if (auto v = row.getInt64("modified_at"); v.isOk()) mtime = v.value();
            indexed[p.value()] = {mtime, size};
        }
    }
    return core::Result<bool>::ok(disk != indexed);
}

core::Result<int64_t> MarkdownService::fileMtime(const fs::path& relPath) const {
    const auto resolved = resolve(relPath);
    if (resolved.isFail()) {
        return core::Result<int64_t>::fail(resolved.errorCode(),
                                           resolved.errorMessage());
    }
    const auto absPath = resolved.value();
    const int64_t mtime = fileMtimeEpoch(absPath);
    if (mtime < 0) {
        return core::Result<int64_t>::fail(core::ErrorCode::FileNotFound,
            "文件不存在或无法读取 mtime: " + absPath.string());
    }
    return core::Result<int64_t>::ok(mtime);
}

bool MarkdownService::isSafeNewNotePath(const std::string& name) {
    if (name.empty()) return false;
    if (name.front() == '/' || name.front() == '\\') return false;  // 绝对路径
    if (name.size() >= 2 && name[1] == ':') return false;           // 盘符 X:
    if (name.find("..") != std::string::npos) return false;         // 父目录逃逸
    if (name.find('\\') != std::string::npos) return false;         // 统一 POSIX '/'
    return true;
}


core::Result<std::vector<std::string>> MarkdownService::listTopFolders() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(notesDir_, ec) || ec) {
        return core::Result<std::vector<std::string>>::ok(std::move(out));
    }
    for (auto it = fs::directory_iterator(
                    notesDir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code dec;
        if (it->is_directory(dec) && !dec) {
            out.push_back(it->path().filename().generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

// 全层目录清单（递归、含空目录，POSIX 相对路径）
core::Result<std::vector<std::string>> MarkdownService::listAllFolders() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(notesDir_, ec) || ec) {
        return core::Result<std::vector<std::string>>::ok(std::move(out));
    }
    for (auto it = fs::recursive_directory_iterator(
                    notesDir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code dec;
        if (!it->is_directory(dec) || dec) continue;
        // 相对路径统一 POSIX 形态（与索引里的笔记路径同构，可直接比对前缀）
        const auto rel = fs::relative(it->path(), notesDir_, dec);
        if (dec || rel.empty()) continue;
        out.push_back(rel.generic_string());
    }
    std::sort(out.begin(), out.end());
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

core::Result<void> MarkdownService::createFolder(const std::string& name) {
    if (!isSafeNewNotePath(name)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "文件夹名不合法（不允许绝对路径、盘符或 ..）: " + name);
    }
    const auto absPath = notesDir_ / fs::path(name);
    std::error_code ec;
    fs::create_directories(absPath, ec);  // 已存在不算错（幂等）
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建文件夹 " + absPath.string() + ": " + ec.message());
    }
    return core::Result<void>::ok();
}

} // namespace module::markdown