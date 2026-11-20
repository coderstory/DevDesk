// src/module_wallpaper/platform/importer_macos.cpp
// 「从花見导入」的 macOS 实现（纯 C/C++，不引 ObjC——文件枚举与哈希都不需要）。
//
// 照搬参考实现已验证的语义，逐条对应：
//   ① 台账对账（need356 F-08）：台账里有、目标文件夹已找不到同名的 → 补墓碑 + 删台账行。
//      这是唯一覆盖「Finder 删 + 清空废纸篓」的路径（废纸篓回填看不见已清空的废纸篓）。
//   ② 废纸篓回填：把 ~/.Trash 顶层 mp4 幂等哈希进墓碑（用户删素材是先进废纸篓，
//      不回填的话清空废纸篓后素材又会被导回来）。读不了（TCC）静默跳过，不拦导入。
//   ③ 候选枚举：花見容器 Documents/Videos 顶层、只认 mp4、跳过隐藏与符号链接、字典序。
//   ④ 逐条判定：core/importer.h 的 decide()（顺序即语义）；「目标已存在」分支顺带解墓碑（F-09）。
//   ⑤ 拷贝：先 copy 到 `<同名>.tmp` 再 rename——`.tmp` 不过扫描白名单，半截拷贝永不会
//      被当成成品；rename 同卷原子。成功即登记导入台账（F-07）。
//
// 身份判据 = 文件名 + 字节大小（core/importer.h 的 makeSignature）：
// 视频动辄几十上百 MB，内容哈希要把每个文件整读一遍；导入是用户点一次等着的操作，
// 读盘量能省则省。花見下载的文件名自带唯一性，同名同大小但内容不同的概率可忽略。
#include "module_wallpaper/platform/importer_macos.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/paths.h"
#include "core/sqlite_db.h"

namespace module::wallpaper::importer {
namespace {

namespace fs = std::filesystem;

// 取「文件名 + 字节大小」身份签名。返回 false = 取不到大小（读失败，
// 调用方按 FailStat 计数，绝不静默放行）。
bool signatureOf(const fs::path& path, std::string* signature) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    if (signature != nullptr) {
        *signature = makeSignature(path.filename().string(),
                                   static_cast<long long>(size));
    }
    return true;
}

// 两张表同库同表空间（墓碑 + 导入台账）：一次 open 建齐，缺一张即视为坏库。
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS deleted_video_signature ("
    "signature TEXT PRIMARY KEY, deleted_at REAL NOT NULL);"
    "CREATE TABLE IF NOT EXISTS imported_signature ("
    "signature TEXT PRIMARY KEY, imported_at REAL NOT NULL);";

// 打开 + 建表。出参 corrupt=true 表示「库内容确实是坏的」（可安全删库重建）。
// 其它失败（BUSY/权限/IO）一律 corrupt=false——**删库是最后手段，只能在确认坏库时做**。
std::unique_ptr<core::data::SqliteDb> openAndEnsureSchema(const fs::path& path,
                                                          bool* corrupt) {
    if (corrupt != nullptr) *corrupt = false;
    auto opened = core::data::SqliteDb::open(path);
    if (opened.isFail()) {
        // open 失败：CANTOPEN/BUSY/PERM 都不是坏库；坏库在 open 阶段通常仍能成功
        // （sqlite 只读文件头，非库文件也当空库打开）。这里只把 CanNotOpen 且
        // 文件存在的情形当可疑，但保守起见不删——交给建表那一步判定。
        return nullptr;
    }
    auto db = std::make_unique<core::data::SqliteDb>(std::move(opened.value()));
    auto schema = db->exec(kSchemaSql);
    if (schema.isFail()) {
        // 只有「内容不是数据库 / 库已损坏」才允许删库重建。SQLITE_BUSY 走
        // DatabaseOpenFailed、权限走 FilePermissionDenied、磁盘满走 FileWriteFailed
        // ——这些都是暂时/环境问题，删库会把健康的墓碑与台账一起扔进垃圾桶
        // （删除线程与导入线程并发写同库，BUSY 本就是高发场景）。
        const core::ErrorCode code = schema.errorCode();
        if (corrupt != nullptr) {
            *corrupt = code == core::ErrorCode::DatabaseCorrupted ||
                       code == core::ErrorCode::Internal;  // SQLITE_ERROR = 非库文件/表结构坏
        }
        return nullptr;
    }
    return db;
}

// 打开并确保两张表存在（墓碑 + 导入台账同库同表空间）。
// 坏库降级：**仅确认损坏时**删文件重建一次（墓碑/台账历史可弃，永不拦导入或删除）。
//
// **建表失败也算坏库（但只限真损坏）**：文件内容是乱码时 sqlite3_open_v2 仍会成功
// （它只读文件头，非库文件也能以「空库」打开），真正暴露损坏的是建表/查询。只在
// open 失败时删库会让乱码库永远返回 nullptr——每次导入静默丢掉全部墓碑与台账
// （实测 bug，probe 原始断言就撞在这上面）。
// WAL 伴生文件一并删：残留 -wal 会把旧内容重放进新建的空库。
//
// 并发：删除钩子（服务线程）与导入线程各开短连接写同一库文件；SqliteDb::open 内部
// 已设 sqlite3_busy_timeout(5000)（core/sqlite_db.cpp），BUSY 会先等 5s 而不是静默丢写。
// 这里不另设 PRAGMA busy_timeout——重复设置只会覆盖同一连接上更可靠的上游取值。
std::unique_ptr<core::data::SqliteDb> openOrCreate(const std::string& databasePath) {
    std::error_code ec;
    const fs::path path(databasePath);
    fs::create_directories(path.parent_path(), ec);

    bool corrupt = false;
    if (auto db = openAndEnsureSchema(path, &corrupt)) return db;
    if (!corrupt) return nullptr;  // BUSY / 权限 / IO：当空台账，绝不删库

    // 确认坏库：删主文件与 WAL 伴生件后重来一次（仍失败 = 目录不可写等真故障）。
    fs::remove(path, ec);
    fs::remove(fs::path(path.string() + "-wal"), ec);
    fs::remove(fs::path(path.string() + "-shm"), ec);
    return openAndEnsureSchema(path, nullptr);
}

// SQLite 墓碑台账。连接每次操作现开现关——调用点（导入路径）分钟级一次，
// 现开现关的开销可忽略；持句柄反而要管生命周期与锁。
class SqliteLedger final : public TombstoneLedger {
public:
    explicit SqliteLedger(std::string databasePath) : databasePath_(std::move(databasePath)) {}

    bool contains(const std::string& signature) override {
        if (signature.empty()) return false;
        auto db = openOrCreate(databasePath_);
        if (db == nullptr) return false;  // 读静默：坏库当没删过
        auto statement = db->prepare("SELECT 1 FROM deleted_video_signature WHERE signature = ?1");
        if (statement.isFail()) return false;
        core::data::Statement& handle = statement.value();
        if (handle.bind(1, signature).isFail()) return false;
        auto stepped = handle.step();
        return stepped.isOk() && stepped.value();
    }

    bool insert(const std::string& signature, double deletedAtEpoch) override {
        if (signature.empty()) return false;
        auto db = openOrCreate(databasePath_);
        if (db == nullptr) return false;
        // INSERT OR IGNORE：重复插入是 no-op 不是错误。
        auto statement = db->prepare(
            "INSERT OR IGNORE INTO deleted_video_signature (signature, deleted_at) "
            "VALUES (?1, ?2)");
        if (statement.isFail()) return false;
        core::data::Statement& handle = statement.value();
        if (handle.bind(1, signature).isFail()) return false;
        if (handle.bind(2, deletedAtEpoch).isFail()) return false;
        return handle.step().isOk();
    }

private:
    std::string databasePath_;
};

// SQLite 导入台账（need356 F-07/F-08）。与墓碑同库同 open 路径：
// 坏库降级、建目录、busy_timeout 全部复用，不再有第二份。
class SqliteImportLedger final : public ImportLedger {
public:
    explicit SqliteImportLedger(std::string databasePath)
        : databasePath_(std::move(databasePath)) {}

    bool insert(const std::string& signature, double importedAtEpoch) override {
        if (signature.empty()) return false;
        auto db = openOrCreate(databasePath_);
        if (db == nullptr) return false;
        auto statement = db->prepare(
            "INSERT OR IGNORE INTO imported_signature (signature, imported_at) "
            "VALUES (?1, ?2)");
        if (statement.isFail()) return false;
        core::data::Statement& handle = statement.value();
        if (handle.bind(1, signature).isFail()) return false;
        if (handle.bind(2, importedAtEpoch).isFail()) return false;
        return handle.step().isOk();
    }

    bool remove(const std::string& signature) override {
        if (signature.empty()) return false;
        auto db = openOrCreate(databasePath_);
        if (db == nullptr) return false;
        auto statement =
            db->prepare("DELETE FROM imported_signature WHERE signature = ?1");
        if (statement.isFail()) return false;
        core::data::Statement& handle = statement.value();
        if (handle.bind(1, signature).isFail()) return false;
        return handle.step().isOk();
    }

    std::vector<std::string> all() override {
        std::vector<std::string> out;
        auto db = openOrCreate(databasePath_);
        if (db == nullptr) return out;  // 读静默：坏库当空台账
        auto statement = db->prepare("SELECT signature FROM imported_signature");
        if (statement.isFail()) return out;
        auto rows = statement.value().fetchAll();
        if (rows.isFail()) return out;
        for (const core::data::SqlRow& row : rows.value()) {
            auto signature = row.getString("signature");
            if (signature.isFail() || signature.value().empty()) continue;
            out.push_back(signature.value());
        }
        return out;
    }

private:
    std::string databasePath_;
};

// 顶层枚举：只认 RegularFile、跳过符号链接与隐藏文件，字典序。
// noAccess 出参（可空）：目录存在但枚举被拒（TCC）置真——与「不存在」区分开，
// 不再同一降级（bug200 症状①）。
std::vector<fs::path> listTopLevelMp4(const fs::path& directory, bool* readable,
                                      bool* noAccess) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (readable != nullptr) *readable = false;
    if (noAccess != nullptr) *noAccess = false;
    fs::directory_iterator it(directory, ec);
    if (ec) {
        // 枚举失败的归因：目录还存在 = 有东西读不了（TCC）；目录都没了 = 未安装。
        std::error_code existsEc;
        if (noAccess != nullptr && fs::exists(directory, existsEc)) {
            *noAccess = true;
        }
        return out;
    }
    if (readable != nullptr) *readable = true;

    for (const fs::directory_entry& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        if (entry.is_symlink(entryEc) && !entryEc) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.') continue;
        std::string extension = entry.path().extension().string();
        if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
        for (char& ch : extension) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (extension != "mp4") continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() < b.filename().string();
              });
    return out;
}

// 拷贝：tmp + rename。失败清理 tmp 并计失败（自愈，下轮重导）。
bool copyViaTmp(const fs::path& source, const fs::path& target) {
    const fs::path tmp = target.string() + ".tmp";
    std::error_code ec;
    // 预置 tmp 是上次崩溃残留：先清掉，避免 copy 直接失败。
    fs::remove(tmp, ec);
    fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code renameEc;
    fs::rename(tmp, target, renameEc);
    if (renameEc) {
        fs::remove(tmp, ec);
        // 目标已被并发导入抢先放好：成品没丢，按已存在计。
        if (fs::exists(target, ec)) return true;
        return false;
    }
    return true;
}

bool hasVideoTrack(const fs::path& path) {
    // 本文件是 .cpp：不能 include AVFoundation。用平台探针（image_probe_macos.cpp 同族的
    // C API 思路）——这里只做「容器可打开且有时长」的粗判，真正的视频轨判定由
    // 播放器在装载时给出。判据保守：打不开就当没有视频轨（不入库）。
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return !ec && size > 0;
}

} // namespace

std::string defaultHanamiSourceDirectory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return std::string();
    return (fs::path(home) / "Library" / "Containers" / "whbalzac.Huajian" / "Data" /
            "Documents" / "Videos")
        .string();
}

bool hanamiImportAvailable() {
    return true;
}

SourceDirectoryStatus probeHanamiSourceDirectory(const std::string& directory) {
    SourceDirectoryStatus status;
    bool noAccess = false;
    listTopLevelMp4(fs::path(directory), &status.readable, &noAccess);
    status.noAccess = noAccess;
    return status;
}

void openFullDiskAccessSettings() {
    // 用户点「打开系统设置」才走到这里（禁自动打开）。旧串 macOS 13+ 可能失效：
    // open 退出码非 0 时回退新串。经 std::system/posix 兜底，无 GUI 依赖。
    if (std::system(
            "open \"x-apple.systempreferences:com.apple.preference.security?"
            "Privacy_AllFiles\"") == 0) {
        return;
    }
    std::system(
        "open \"x-apple.systempreferences:com.apple.settings.privacy?"
        "Privacy_AllFiles\"");
}

std::string defaultTombstoneDatabasePath() {
    return (core::paths::appDataDir() / "wallpaper" / "deleted-videos.sqlite").string();
}

bool recordDeletedSignature(const std::string& signature) {
    if (signature.empty()) return false;
    return SqliteLedger(defaultTombstoneDatabasePath())
        .insert(signature, static_cast<double>(std::time(nullptr)));
}

bool removeTombstoneSignature(const std::string& signature) {
    if (signature.empty()) return false;
    auto db = openOrCreate(defaultTombstoneDatabasePath());
    if (db == nullptr) return false;
    auto statement =
        db->prepare("DELETE FROM deleted_video_signature WHERE signature = ?1");
    if (statement.isFail()) return false;
    core::data::Statement& handle = statement.value();
    if (handle.bind(1, signature).isFail()) return false;
    return handle.step().isOk();
}

ImportReport runHanamiImport(const std::string& destinationFolder,
                             const std::function<void(int, int)>& onProgress,
                             const std::atomic<bool>* cancel) {
    ImportReport report;
    const fs::path destination(destinationFolder);
    if (destinationFolder.empty()) {
        report.sourceMissing = true;
        return report;
    }
    std::error_code ec;

    SqliteLedger ledger(defaultTombstoneDatabasePath());
    SqliteImportLedger importLedger(defaultTombstoneDatabasePath());

    // ① 台账对账（need356 F-08）：台账记着「导入成功过」，但目标文件夹已找不到同名文件
    // → 用户删了它（可能是 Finder 删 + 清空废纸篓，废纸篓回填看不见）→ 补墓碑并删台账行。
    // 必须早于废纸篓回填与逐候选判定，本轮摘要才是对账后的口径。
    // 读不到台账 = 空台账，跳过整步（坏库不得拦导入）。
    {
        if (cancel != nullptr && cancel->load()) return report;
        for (const std::string& signature : importLedger.all()) {
            const std::string name = signatureFileName(signature);
            if (name.empty()) continue;
            std::error_code existsEc;
            const bool targetExists = fs::exists(destination / name, existsEc);
            // 探存在性失败（EACCES / IO）时不能当「不存在」：那会给仍然存在的素材
            // 补墓碑，而墓碑只有「目标已存在」分支能解，等于把用户存量素材永久锁死
            // 且无任何提示。未知即保守跳过本轮该条（下轮再判）。
            if (existsEc) continue;
            if (shouldTombstoneOnReconcile(targetExists)) {
                // 两步写各自 best effort：补墓碑失败会下一轮再补（台账行留着），
                // 删台账行失败只意味着下一轮重复补一次墓碑（INSERT OR IGNORE 幂等）。
                if (ledger.insert(signature, static_cast<double>(std::time(nullptr)))) {
                    const bool removed = importLedger.remove(signature);
                    (void)removed;  // 失败不拦导入：重复补墓碑是幂等 no-op
                }
            }
        }
    }

    // ② 废纸篓回填：顶层 mp4 幂等进墓碑。读不了静默跳过，不拦导入。
    {
        if (cancel != nullptr && cancel->load()) return report;
        const char* home = std::getenv("HOME");
        if (home != nullptr && *home != '\0') {
            bool readable = false;
            const std::vector<fs::path> trashed =
                listTopLevelMp4(fs::path(home) / ".Trash", &readable, nullptr);
            if (readable) {
                for (const fs::path& item : trashed) {
                    std::string signature;
                    if (!signatureOf(item, &signature)) continue;
                    if (ledger.insert(signature, static_cast<double>(std::time(nullptr)))) {
                        ++report.tombstoneSeededFromTrash;
                    }
                }
            }
        }
    }

    // ③ 候选枚举。区分「不存在」（未安装）与「存在但被 TCC 拒」（无权限）。
    bool sourceReadable = false;
    bool sourceNoAccess = false;
    const std::vector<fs::path> candidates = listTopLevelMp4(
        fs::path(defaultHanamiSourceDirectory()), &sourceReadable, &sourceNoAccess);
    if (!sourceReadable) {
        if (sourceNoAccess) {
            report.sourceNoAccess = true;
        } else {
            report.sourceMissing = true;
        }
        return report;
    }

    // ④⑤ 逐条判定 + 拷贝 + 成功登记台账。
    const int total = static_cast<int>(candidates.size());
    int done = 0;
    for (const fs::path& candidate : candidates) {
        // 取消在候选之间生效：正在拷贝的那一个会拷完（半截由 .tmp 机制挡住）。
        if (cancel != nullptr && cancel->load()) return report;
        const fs::path target = destination / candidate.filename();

        CandidateFacts facts;
        // ec 重载：stat 出非 ENOENT 错误（如 EACCES）按「不存在」走后续判定
        // （取大小大概率同样失败 → FailStat），绝不走抛异常路径击穿导入线程。
        facts.targetExists = fs::exists(target, ec);
        // 目标已存在就跳过，连候选的大小都不用取（省一次 stat）。
        if (!facts.targetExists) {
            facts.signatureOk = signatureOf(candidate, &facts.signature);
            if (facts.signatureOk) {
                facts.inTombstone = ledger.contains(facts.signature);
                // 墓碑命中就注定跳过，不必再探视频轨（省一次探测）。
                facts.hasVideoTrack = facts.inTombstone || hasVideoTrack(candidate);
            }
        } else {
            // need356 F-09：目标已存在 = 文件「回到了」文件夹（从废纸篓拖回 / 重新放入）。
            // 解墓碑必须用**目标文件**的签名：墓碑里记的是目标目录里那个「文件名+大小」，
            // 候选源文件的签名与它不是一回事。stat 失败静默跳过——解墓碑是 best effort，
            // 不阻断导入、不计入任何报告分项。
            std::string targetSignature;
            if (signatureOf(target, &targetSignature)) {
                const bool removed = removeTombstoneSignature(targetSignature);
                (void)removed;
            }
        }

        const CandidateVerdict verdict = decide(facts);
        recordVerdict(verdict, report);
        if (verdict == CandidateVerdict::Import) {
            if (copyViaTmp(candidate, target)) {
                ++report.imported;
                // F-07：拷贝成功即刻登记台账（逐条即时，不攒到收尾——中途取消也要保住
                // 已导入的那些，否则下一轮它们会被当「目标已存在」跳过而台账永远缺行）。
                const bool recorded = importLedger.insert(
                    facts.signature, static_cast<double>(std::time(nullptr)));
                (void)recorded;  // 台账写失败不阻断导入、不改摘要数字
            } else {
                ++report.failedCopy;
            }
        }
        ++done;
        if (onProgress) onProgress(done, total);
    }
    return report;
}

} // namespace module::wallpaper::importer
