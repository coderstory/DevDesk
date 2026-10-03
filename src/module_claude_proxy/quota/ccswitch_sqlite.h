// src/module_claude_proxy/quota/ccswitch_sqlite.h
// cc-switch 只读库读取的**共用脚手架**（RAII / 探列 / 文本列 / 只读打开）。
//
// ── 为什么有这个头 ──────────────────────────────────────────────────────
// cc-switch 的库被两条独立路径读：
//   ① quota/ccswitch_source.cpp —— 余量查询时现取凭据（baseUrl + apiKey）
//   ② ccswitch_import.cpp     —— 把供应商导入成 DevDesk 的 Profile
// 两者的**骨架**逐字相同（sqlite RAII 关闭 / pragma 探列 / column_text 读串 /
// 错误串拼装 / 只读打开 + busy timeout），2026-10-03 轻量化审计实测：6 个函数
// 在两文件里各存在一份完全相同的副本（`ccswitch_import.cpp` 文件头当时还写着
// 「直接复用，**不重写一份**」——注释与实现相反）。本头把这份骨架收成单点。
//
// 行为与语义**逐字未变**：只读 flags、2000ms busy timeout、错误文案全都按原样。
// 两处文案差异（「配置库」/「数据库」）由 `openReadOnly` 的 `noun` 参数保留——
// 测试直接断言过这两串中文（test_ccswitch_import.cpp:393 断言「数据库」、
// test_claude_ui.cpp:8234 断言「配置库」），合并时不许改口径。
//
// ── 只读硬约束 ──────────────────────────────────────────────────────────
// 见 quota/ccswitch_source.h 文件头（一律 SQLITE_OPEN_READONLY，绝不含
// READWRITE / CREATE；不建表、不写 schema、不 VACUUM、不动 -wal/-shm）。
#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ccswitch_source.h"  // kCcSwitchOpenFlags / ccSwitchDbPath（同目录相对）

namespace module::claude::quota::ccswitch_sqlite {

// cc-switch 应用正在写自己的库；只读连接撞上写锁时短暂等一下，别一上来就报失败。
inline constexpr int kBusyTimeoutMs = 2000;

// ---- sqlite RAII（项目铁律：不抛异常，全部走确定性关闭）----
struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbHandle = std::unique_ptr<sqlite3, SqliteCloser>;

struct SqliteStmtCloser {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt != nullptr) sqlite3_finalize(stmt);
    }
};
using StmtHandle = std::unique_ptr<sqlite3_stmt, SqliteStmtCloser>;

inline std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(text));
}

// providers 表的列名清单。表不存在时查询失败 → 空清单（调用方按 Schema 处理）。
// 这是「schema 演进防护」的前半段：不写死 SELECT *，而是先看清楚有哪些列。
inline std::vector<std::string> providerColumns(sqlite3* db) {
    std::vector<std::string> columns;
    const char* sql = "SELECT name FROM pragma_table_info('providers')";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) return columns;
    const StmtHandle stmt(raw);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        columns.push_back(columnText(stmt.get(), 0));
    }
    return columns;
}

inline bool hasColumn(const std::vector<std::string>& columns, const char* name) {
    return std::find(columns.begin(), columns.end(), name) != columns.end();
}

// 缺列时用 NULL 占位（而不是整条 SELECT 失败）：cc-switch 哪天给 providers 换 /
// 拆列，我们仍然能把认得的字段读出来，而不是一颗雷「来源坏了」。
inline std::string columnOrNull(const std::vector<std::string>& columns,
                                const char* name) {
    return hasColumn(columns, name) ? std::string(name) : std::string("NULL");
}

inline std::string sqliteError(sqlite3* db, int code) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    std::string text = message != nullptr ? message : sqlite3_errstr(code);
    if (text.empty()) text = "sqlite 错误 " + std::to_string(code);
    return text;
}

// ---- 只读打开（两条读路径共用的前半段）------------------------------------
enum class OpenStatus {
    Ok,
    NotFound,    // 主目录取不到 / 库文件不存在（`reason` 已填好面向用户的中文）
    Unreadable,  // 存在但打不开：非 SQLite 文件、权限不足、打开即失败
};

// 打开 cc-switch 库的只读连接。成功时 `db` 持有效句柄，`reason` 不动。
//
// `path` 传 ccSwitchDbPath() 的结果（本函数不自己去取，便于调用方决定时机与
// 日志）；`noun` 只进错误文案（"配置库" / "数据库"），两侧口径各自保留。
inline OpenStatus openReadOnly(DbHandle& db, std::string* reason,
                               const std::string& path, const char* noun) {
    if (path.empty()) {
        *reason = "找不到用户主目录，无法定位 cc-switch 配置库";
        return OpenStatus::NotFound;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        *reason = std::string("未找到 cc-switch ") + noun + "（" + path + "）";
        return OpenStatus::NotFound;
    }

    // 只读打开：不含 READWRITE / CREATE，绝不把库建出来、绝不动 cc-switch 的文件
    sqlite3* raw = nullptr;
    const int openCode =
        sqlite3_open_v2(path.c_str(), &raw, kCcSwitchOpenFlags, nullptr);
    db.reset(raw);
    if (openCode != SQLITE_OK) {
        // 取错误串时句柄可能已是 nullptr（sqlite3_open_v2 失败时也可能留一个非空
        // 的坏句柄），sqliteError 对两者都安全；DbHandle 保证它一定被关掉。
        *reason = std::string("打不开 cc-switch ") + noun +
                  "（只读）：" + sqliteError(db.get(), openCode);
        return OpenStatus::Unreadable;
    }
    sqlite3_busy_timeout(db.get(), kBusyTimeoutMs);
    return OpenStatus::Ok;
}

// ---- 结果体装配 ----------------------------------------------------------
// 两个 Result 结构（CcSwitchProviderList / CcSwitchImportRows）都有
// `status` 与 `unavailableReason` 两个同义字段，只是枚举类型不同 → 走模板。
//
// `ListT` 必须由调用方显式给出：它只出现在返回类型里，无法从实参推导。
// 每个读侧因此各留一个一行 wrapper（`unavailable`），调用点写法保持原样。
template <typename ListT, typename StatusT>
inline ListT makeUnavailable(StatusT status, std::string reason) {
    ListT out;
    out.status = status;
    out.unavailableReason = std::move(reason);
    return out;
}

}  // namespace module::claude::quota::ccswitch_sqlite
