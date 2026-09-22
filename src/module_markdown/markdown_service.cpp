// src/module_markdown/markdown_service.cpp
#include "markdown_service.h"

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
    : db_(db), notesDir_(notesDir) {}

fs::path MarkdownService::resolve(const fs::path& relPath) const {
    // relPath 是 POSIX 风格的相对路径（'/' 分隔符）
    // std::filesystem 在 macOS/Linux 上接受 '/'，无需转换
    return notesDir_ / relPath;
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
    const std::string& relPathPosix) {

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

            if (oldSize == static_cast<int64_t>(fileSize) &&
                oldMtime == static_cast<int64_t>(mtimeSec)) {
                // 无变化，跳过 update（但 FTS 仍要保证最新；通常没变化 FTS 也不用动）
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
        return core::Result<void>::ok();
    });
    if (tx.isFail()) {
        return core::Result<void>::fail(tx.errorCode(),
            "delete from index: " + tx.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> MarkdownService::rescan() {
    // 确保 notesDir 存在
    std::error_code ec;
    fs::create_directories(notesDir_, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建笔记目录: " + ec.message());
    }

    // 1) 递归遍历磁盘，把所有 .md 文件 upsert 到索引
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
        LOG_WARN("遍历笔记目录时出错: " + ec.message());
    }

    // 2) 删除索引中存在但磁盘上已不存在的条目
    auto prep = db_.prepare("SELECT path FROM markdown_index");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(),
            "prepare select paths: " + prep.errorMessage());
    }
    std::vector<std::string> toRemove;
    {
        auto& stmt = prep.value();
        auto rowsResult = stmt.fetchAll();
        if (rowsResult.isFail()) {
            return core::Result<void>::fail(rowsResult.errorCode(),
                "fetch paths: " + rowsResult.errorMessage());
        }
        for (const auto& row : rowsResult.value()) {
            auto p = row.getString("path");
            if (p.isOk() && foundPaths.find(p.value()) == foundPaths.end()) {
                toRemove.push_back(p.value());
            }
        }
    }
    for (const auto& p : toRemove) {
        auto delResult = deleteFromIndex(p);
        if (delResult.isFail()) {
            LOG_WARN("删除索引失败 " + p + ": " + delResult.errorMessage());
        }
    }

    // 3) 清理 FTS 里的孤儿行（path 已不在 markdown_index 中）
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
    const auto absPath = resolve(relPath);
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
    const auto absPath = resolve(relPath);

    // 确保父目录存在
    const auto parent = absPath.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建父目录 " + parent.string() + ": " + ec.message());
    }

    // 写文件（覆盖）
    {
        std::ofstream ofs(absPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                "无法打开文件写入: " + absPath.string());
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                "写入失败: " + absPath.string());
        }
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
    const auto absPath = resolve(norm);

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

    // 写文件
    {
        std::ofstream ofs(absPath, std::ios::binary);
        if (!ofs) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                "无法创建文件: " + absPath.string());
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
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
    const auto absPath = resolve(relPath);

    // 删文件（不存在也允许）
    std::error_code ec;
    fs::remove(absPath, ec);
    // 不报错：即使文件不存在，索引也要清

    return deleteFromIndex(relPosix);
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

NoteMeta MarkdownService::rowToNote(const core::data::SqlRow& row) {
    return rowToNoteImpl(row);
}

// ---- W8：文件变化自动感知 ----

core::Result<bool> MarkdownService::pollChanged() {
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
    const auto absPath = resolve(relPath);
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