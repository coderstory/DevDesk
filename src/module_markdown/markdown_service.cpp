// src/module_markdown/markdown_service.cpp
#include "markdown_service.h"

#include "core/atomic_write.h"
#include "core/logging.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace module::markdown {

namespace {

// 把一个用户输入的词清洗成安全的 FTS5 词元
//
// FTS5 查询语法包含 " ' ( ) * : ^ { } - + ~ AND OR NOT NEAR 等操作符。
// 策略：
//   1. 只保留字母/数字/下划线/非 ASCII 字节（CJK 等），丢弃其余字符
//   2. 用双引号包成 phrase（消灭残留的关键字语义，如 AND）
//   3. 后面跟 * 做 prefix 匹配
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

// FTS 查询计划：
//   match    = 安全的 MATCH 表达式（"" = 跳过 FTS）
//   anyLossy = 有词元含被丢弃符号（+ - # . : 等），FTS 语义已失真，
//              需要 LIKE 兜底补一次字面子串匹配
//   allLossy = 全部词元失真 → FTS 前缀必然误命中（搜 "c++" 清洗成
//              "c"* 会命中所有 c 开头词），直接跳过 FTS 只走 LIKE
struct FtsQueryPlan {
    std::string match;
    bool anyLossy = false;
    bool allLossy = false;
};

// 把用户查询拆词 → 安全的 FTS5 MATCH 表达式 + 失真标记
//   例: "special_token_123" → "\"special\"* \"token\"* \"123\"*"
//   空查询 → match=""（调用方需处理）
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

// LIKE 模式下转义 % _ \（配合 ESCAPE '\'）
std::string escapeLikePattern(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\\' || ch == '%' || ch == '_') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

// 把 SqlRow 转成 NoteMeta
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

// 文件 mtime → unix 秒；失败返回 -1。
// C++17 的 file_clock epoch 是 implementation-defined（MinGW 上非 1970-01-01，
// 直接 duration_cast 会写入荒谬时间——W7 P0-5）。可移植换算：file_clock 与
// system_clock 在同一时刻各取 now 做差值基准。
int64_t fileMtimeEpoch(const fs::path& absPath) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(absPath, ec);
    if (ec) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch() +
        (mtime - std::chrono::file_clock::now())).count();
}

} // namespace

// ============ MarkdownService ============

MarkdownService::MarkdownService(core::data::SqliteDb& db,
                                 const fs::path& notesDir)
    : db_(db), notesDir_(notesDir),
      watcher_(std::make_unique<platform::FileWatcher>()) {
    // 启动原生文件监听（Windows 真实现 / 其他平台 stub 静默降级）。
    // rescan() 同样会建目录，但 watcher 要先于首扫存在，故这里先幂等建好。
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
    // 先 join watcher 线程，保证回调不再触碰任何成员（rescanMu_ 等随后才析构）
    if (watcher_) watcher_->stop();
}

bool MarkdownService::watcherActive() const {
    return watcher_ && watcher_->active();
}

bool MarkdownService::hasWatchCredit() const {
    // 仅 load 不 exchange：peek 语义，credit 仍留给 pollChanged 正式消费
    return watcherFired_.load();
}

void MarkdownService::onWatchEvent(std::vector<platform::FileWatchEvent> events) {
    // 1) 置标记：模块层下一次 pollChanged 必为 true → 其 rescan+refreshList
    //    照旧跑一轮（UI 列表 ≤5s 内对齐，模块无需感知 watcher 的存在）。
    watcherFired_.store(true);

    // 2) 即刻尽力同步索引（watcher 线程）：让搜索/索引毫秒级跟上外部改动。
    //    UI 正持有 rescanMu_（模块 tick 的 rescan 在跑）时跳过——那轮 rescan
    //    以磁盘为准收敛，且标记已置，下一轮 pollChanged 仍会触发刷新。
    std::unique_lock<std::mutex> lock(rescanMu_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    bool needFullRescan = false;
    for (const auto& ev : events) {
        if (ev.relativePath.empty()) {
            needFullRescan = true;  // Unknown（缓冲溢出等）：无法逐项处理
            continue;
        }
        // UTF-8 → fs::path 必须走 u8string 构造（Windows 上 std::string→path
        // 按 ACP 解码，中文笔记名会乱码），与 rescan 的目录枚举编码链一致
        const fs::path rel{std::u8string(ev.relativePath.begin(),
                                         ev.relativePath.end())};
        if (rel.extension() != ".md") continue;  // 目录/非笔记事件交给标记兜底
        const std::string relPosix = toPosix(rel);
        const auto resolved = resolve(rel);
        if (resolved.isFail()) {
            // watcher 事件不该出现越界路径（事件本身来自笔记目录枚举），
            // 出现即防御：跳过该事件，标记已置，下轮 poll 兜底
            LOG_WARN("watcher 事件路径越界，跳过 " + ev.relativePath + ": " +
                     resolved.errorMessage());
            continue;
        }
        const auto absPath = resolved.value();
        std::error_code ec;
        const bool exists = fs::is_regular_file(absPath, ec) && !ec;
        core::Result<void> one = exists
            // 事件确证文件动过 → force 跳过 (size,mtime) 短路（#5 根治点：
            // 同秒改回等长内容 mtime 比对看不见，事件能看见）
            ? syncOneToIndex(absPath, relPosix, /*force=*/true)
            : deleteFromIndex(relPosix);
        if (one.isFail()) {
            // 半截读/锁冲突等：本轮放弃该文件，标记已置，模块 rescan 以磁盘为准修正
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
    // 入口消费 credit（而非出口）：本轮磁盘遍历必然扫到「消费点之前已发生」
    // 的改动，credit 当场兑现；遍历期间新到的事件会重新置位，留给下一轮。
    watcherFired_.store(false);
    return rescanLocked();
}

core::Result<fs::path> MarkdownService::resolve(const fs::path& relPath) const {
    // relPath 是 POSIX 风格的相对路径（'/' 分隔符）
    // std::filesystem 在 macOS/Linux 上接受 '/'，无需转换
    //
    // containment 校验（安全收口，评审修复 2026-10-01）：
    //   1) `notesDir_ / relPath` 的两种逃逸形态都必须拒——
    //      相对上跳（"../../x"）与绝对路径（fs::path::operator/ 对绝对 RHS
    //      是整体替换，notesDir 前缀都不剩，"notesDir//etc/passwd" =
    //      "/etc/passwd"）。
    //   2) 先 lexically_normal（"a/./b" → "a/b"，可规范化回目录内的形式放行），
    //      再与规范化后的 notesDir_ 做**逐组件**前缀比较——字符串前缀比较会把
    //      "/notes/evil" 误判成 "/notes" 的内含（"/notesEVIL" 也以 "/notes" 开头）。
    //   3) fail 语义：ValidationError + 中文说明（回喂 LLM / UI toast 可读）。
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
    // generic_string() 总是 POSIX 风格（'/' 分隔符），跨平台一致
    return p.generic_string();
}

std::string MarkdownService::extractTitle(const std::string& content,
                                          const fs::path& relPath) {
    // 从第一非空行提取 "# Title"
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // 跳过前导空白
        size_t i = 0;
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        if (i >= line.size()) continue; // 空行
        // 必须以 "#" 开头
        if (line[i] != '#') break;
        // 跳过连续的 #
        ++i;
        while (i < line.size() && line[i] == '#') ++i;
        // 跳过后续空白
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::string title = line.substr(i);
        // 去掉尾部空白
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

    // 查 index 里是否有这行（按 path）
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
            // 拿旧的 size/mtime 判断是否要更新
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
                // 无变化，跳过 update（但 FTS 仍要保证最新；通常没变化 FTS 也不用动）。
                // force=true（watcher 事件确证动过）不走此短路：秒级 (mtime,size)
                // 比对对「同秒改回等长内容」失明，事件粒度比它精确（#5 根治点）。
                return core::Result<void>::ok();
            }
        }
    }

    // INSERT OR IGNORE + UPDATE 分两步走，保证幂等
    // 简化：用 INSERT OR REPLACE INTO markdown_index ...
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
        // UPDATE（覆盖 INSERT IGNORE 的 0 行情况）
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
        // FTS5 同步：先按 path 删除旧行，再插入新行
        //
        // 注意：这里**不能**只用 INSERT OR REPLACE —— markdown_fts 的
        // `path` 列是 UNINDEXED（不参与全文索引），**没有 UNIQUE 约束**，
        // 所以 INSERT OR REPLACE 不会去重，而是每次追加一行新记录，
        // 导致旧内容残留、搜索结果命中过期数据。必须先删后插。
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
        // 标签关联随 markdown_index 行删除（FK ON DELETE CASCADE），
        // 但 markdown_tags 里的标签名会留孤儿 —— 顺手清理
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
    // 确保 notesDir 存在
    std::error_code ec;
    fs::create_directories(notesDir_, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建笔记目录: " + ec.message());
    }

    // 1) 先取索引快照（= 待剪枝候选集），再遍历磁盘。
    //    这个顺序不能反（MD-08）：旧实现「先遍历攒 foundPaths → 再 SELECT」留下
    //    一个窗口——UI 线程的 write/create/rename 在遍历期间落索引的行，遍历
    //    可能已经扫过它所在目录（3000 篇量级约 700ms），于是它不在 foundPaths
    //    里，被当成「磁盘上已不存在」删掉（FTS 行同步删，标签关联 ON DELETE
    //    CASCADE 后不可恢复），要等下一轮 rescan 才自愈。先取快照后，快照之后
    //    新增的任何行都不是删除候选，窗口消失。
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
    std::set<std::string> foundPaths; // POSIX 风格相对路径
    for (auto it = fs::recursive_directory_iterator(
                    notesDir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!ec) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            const auto& absPath = entry.path();
            if (absPath.extension() != ".md") continue;

            // 计算相对路径（POSIX 风格）
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
        // MD-08：遍历中途出错 → foundPaths 残缺，此时按它剪枝会把磁盘上真实
        // 存在的笔记当「已删除」成批清掉（含标签级联）。本轮放弃剪枝（FTS
        // 孤儿清理同理），只保留已完成的 upsert，下一轮 rescan 再以磁盘为准
        // 收敛。调用方已按 fail 分支只记日志/提示，不会误报成功。
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

    // 4) 清理 FTS 里的孤儿行（path 已不在 markdown_index 中）
    //    历史版本用过 INSERT OR REPLACE（对 FTS5 不去重），可能留下重复行
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
    // dir 是 POSIX 风格相对目录（如 "inbox" 或 "" 表示根）
    std::string prefix = dir.empty() ? "" : toPosix(dir);
    // 在 SQL 里匹配：path 等于 prefix 或 prefix + '/' + ...
    // 简化：用 LIKE 'prefix/%' 或 = prefix（精确）
    // 实际上目录匹配的语义是：parent(path) == dir
    //   - root: parent 为空
    //   - "inbox": parent = "inbox"
    //   - "inbox/sub": parent = "inbox/sub"
    //
    // 用 INSTR/SUBSTR 实现：
    //   parent_dir(path) = case when instr(path, '/') > 0
    //                          then substr(path, 1, instr(path, '/')-1)
    //                          else ''
    //                      END
    // 干脆全列出来再筛（结果集通常 < 1000 条笔记，性能可接受）
    auto listResult = listAll();
    if (listResult.isFail()) {
        return listResult;
    }
    std::vector<NoteMeta> filtered;
    for (auto& n : listResult.value()) {
        const std::string& path = n.relativePath.generic_string();
        // 父目录 = 去掉最后一段
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

    // 原子写（覆盖）
    // 原子写走 core 公共实现（THS-15 下沉 core::atomicWriteFile）；失败码映射回
    // FileWriteFailed —— 文本保存路径统一用文件写失败码，不混用 Internal
    auto wr = core::atomicWriteFile(absPath, content, "笔记");
    if (wr.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        wr.errorMessage());
    }

    // 同步索引
    auto syncResult = syncOneToIndex(absPath, relPosix);
    if (syncResult.isFail()) {
        return syncResult;
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::create(const fs::path& relPath) {
    // 路径安全校验（trust boundary：目录树/打开逻辑都依赖 relativePath 落在
    // notesDir 内，这里兜底拒绝逃逸输入，模块层另有同规则的前置校验）
    if (!isSafeNewNotePath(toPosix(relPath))) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "路径不合法（不允许绝对路径、盘符或 ..）: " + toPosix(relPath));
    }
    // 规范化路径：补 .md 后缀
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

    // 确保父目录存在
    const auto parent = absPath.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建父目录: " + ec.message());
    }

    // 写文件（原子写；旧实现连写失败都不检查，审计 2026-09-23 [2] 一并修）
    // 原子写走 core 公共实现（THS-15 下沉 core::atomicWriteFile）；失败码映射回
    // FileWriteFailed —— 文本保存路径统一用文件写失败码，不混用 Internal
    auto wr = core::atomicWriteFile(absPath, content, "笔记");
    if (wr.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        wr.errorMessage());
    }

    // 同步索引
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
        // 越界路径拒绝删（否则 = 笔记目录外任意文件删除缺口）
        return core::Result<void>::fail(resolved.errorCode(),
                                        resolved.errorMessage());
    }
    const auto absPath = resolved.value();

    // 删文件（不存在也允许）
    std::error_code ec;
    fs::remove(absPath, ec);
    // 不报错：即使文件不存在，索引也要清

    return deleteFromIndex(relPosix);
}

core::Result<std::string> MarkdownService::rename(const fs::path& oldRelPath,
                                                  const fs::path& newRelPath) {
    const std::string oldPosix = toPosix(oldRelPath);
    // 新名走与 create() 同一安全规则（绝对路径/盘符/.. 一律拒绝）
    if (!isSafeNewNotePath(toPosix(newRelPath))) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError,
            "新名不合法（不允许绝对路径、盘符或 ..）: " + toPosix(newRelPath));
    }
    // 规范化路径：无扩展名时补 .md（与 create() 一致）
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

    // 读新文件内容（title 与 FTS 同源）。读失败不致命：title 退化为文件名，
    // FTS 内容空一帧，5 秒轮询的 rescan 最终会修正索引。
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

    // 索引行按 path 就地 UPDATE（id 不变 → 标签外键关联保留，这是不用
    // remove+create 组合的全部原因）；FTS 先删旧行再插新行。
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
        // DB 失败 → 把磁盘改回去，保持「文件位置 = 索引 path」一致性；
        // 回滚也失败时留响亮日志（rescan 5 秒内会以磁盘为准修正索引）
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

    // ---- 路径 A：FTS5 MATCH（拉丁词、按 rank 排序）----
    // 全部词元失真（如查询就是 "c++"）时跳过 FTS，见 FtsQueryPlan 注释
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

    // ---- 路径 B：LIKE 子串匹配（CJK / 符号词元兜底）----
    //
    // unicode61 分词器把连续 CJK 视为单个 token，"界面" 无法命中
    // "用户界面"（前缀也不行）；同样，含被丢弃符号的词元（"c++" 清洗成
    // "c"）在 FTS 里语义失真。所以只要查询里含非 ASCII（CJK）字符、
    // 有失真词元、或 FTS5 没命中，就走一次 LIKE 字面子串搜索兜底。
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

// ---- 标签 ----

namespace {

// 去掉 ASCII 首尾空白（标签名清洗用；UTF-8 正文不受影响）
std::string trimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

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
        // 笔记必须在索引中（标签挂在 markdown_index.id 上）
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
        // upsert 标签 + 建关联
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
        // 清理无笔记引用的孤儿标签（标签最后一个使用者被移除后自动消失）
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

// ---- W8：文件变化自动感知 ----

core::Result<bool> MarkdownService::pollChanged() {
    // Credit 语义（P3 接入 platform::FileWatcher，#5 根治）：
    // watcher 报过事件 → 本次必返 true（消费该 credit）——即使 (mtime,size)
    // 比对因「同秒改回等长内容」失明，也强制调用方跑一轮以磁盘为准的
    // rescan + UI 刷新。watcher 线程其实已在 onWatchEvent 里 force-sync 过
    // 受影响文件（索引毫秒级跟上），这一轮 true 的真正消费方是列表刷新。
    // rescan()（公开）同样消费 credit：调用方已完成一次全量磁盘→索引核对。
    if (watcherFired_.exchange(false)) {
        return core::Result<bool>::ok(true);
    }

    // ---- mtime/size 比对：stub 平台 / watcher 掉线 / credit 已消费后的主路径 ----

    // 磁盘侧快照：path → (mtime, size)
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

// ---- W9：新建文件夹入口 ----

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

// W10：全层目录清单（两级目录树用；递归、含空目录，POSIX 相对路径）
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