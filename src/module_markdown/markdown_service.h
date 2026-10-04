// Markdown 笔记本服务：文件系统 I/O + SQLite 索引 + FTS5 搜索。
// 文件存盘（notesDir，UTF-8）；索引在 markdown_index，FTS5 在 markdown_fts。
// 相对路径用 POSIX 分隔符（'/'）；所有失败走 Result<T>，不抛异常。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"
#include "platform/file_watcher.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace module::markdown {

// 笔记元数据（来自 markdown_index 表）
struct NoteMeta {
    int64_t id = 0;
    std::filesystem::path relativePath; // 总是 POSIX 风格（'/' 分隔）
    std::string title;
    int64_t sizeBytes = 0;
    int64_t modifiedAt = 0; // unix epoch seconds
};

// 标签 + 引用它的笔记数
struct TagCount {
    std::string name;
    int64_t noteCount = 0;
};

class MarkdownService {
public:
    MarkdownService(core::data::SqliteDb& db, const std::filesystem::path& notesDir);
// 析构先 stop watcher（join 后台线程），保证回调不再触碰已销毁成员
    ~MarkdownService();

    MarkdownService(const MarkdownService&) = delete;
    MarkdownService& operator=(const MarkdownService&) = delete;

// 扫描 notesDir 同步到索引 / FTS（新文件插入、删除清理、mtime/size 变化更新）
    core::Result<void> rescan();

// 列出所有笔记（按 modified_at DESC）
    core::Result<std::vector<NoteMeta>> listAll();

// 列出某目录下的笔记（dir 为空 = 根；匹配 relativePath 父目录 == dir）
    core::Result<std::vector<NoteMeta>> listInDirectory(
        const std::filesystem::path& dir);

// 读取笔记全文（UTF-8）
    core::Result<std::string> read(const std::filesystem::path& relPath);

// 写入笔记：写文件 → 更新索引 → 更新 FTS（relPath 须带 .md，content UTF-8）
    core::Result<void> write(const std::filesystem::path& relPath,
                       const std::string& content);

// 新建笔记（写空模板 → 插入索引 + FTS；relPath 无 .md 自动补全）
    core::Result<void> create(const std::filesystem::path& relPath);

// 删除笔记（删文件 + 索引 + FTS）
    core::Result<void> remove(const std::filesystem::path& relPath);

// 重命名/移动笔记（磁盘 rename + 索引 path 就地 UPDATE + FTS 先删后插）。
// markdown_index.id 保持不变 → 标签外键关联保留，不能走 remove+create（会级联删标签）。
// 目标已存在 → AlreadyExists；失败回滚 rename；返回最终 POSIX 相对路径。
    core::Result<std::string> rename(const std::filesystem::path& oldRelPath,
                                     const std::filesystem::path& newRelPath);

// FTS5 全文搜索（按 rank 排序）
    core::Result<std::vector<NoteMeta>> search(const std::string& query);

// ---- 标签（数据全在 markdown_tags / markdown_note_tags）----

// 某笔记的全部标签（按名字母序）
    core::Result<std::vector<std::string>> tagsForNote(
        const std::filesystem::path& relPath);

// 全量替换某笔记的标签（事务：删旧关联 + upsert + 建新关联 + 清孤儿标签）。
// 笔记必须已在索引中，否则 fail。
    core::Result<void> setTags(const std::filesystem::path& relPath,
                               const std::vector<std::string>& tags);

// 全部标签 + 各自笔记数（按名字母序；只含被引用的标签）
    core::Result<std::vector<TagCount>> listAllTags();

// 含某标签的全部笔记（按 modified_at DESC）
    core::Result<std::vector<NoteMeta>> notesWithTag(const std::string& tag);

// ---- 文件变化自动感知：watcher 事件 + 磁盘轮询两条腿 ----
// watcher 事件即刻同步索引并按相关性置 credit；pollChanged 先消费 credit，
// 否则退回磁盘 mtime/size 比对（stub 平台 / watcher 掉线兜底）。true → rescan()。

// 轻量磁盘轮询：只比 mtime/size，不读内容/不写索引。true = 有差异，需 rescan()。
    core::Result<bool> pollChanged();

// 原生文件监听是否在工作（stub / 启动失败 = false，pollChanged 退化为比对）。
    bool watcherActive() const;

// watcher 是否有未消费的事件 credit（pollChanged 消费）。模块层每帧 peek：
// true → 即时 rescan（毫秒级响应），stub 平台恒 false 退回轮询。
// credit 只在「索引/目录树相关」事件置位：.md、Unknown（空路径）、目录、
// 已不存在的非 .md 路径；存在的非 .md 普通文件不置。
    bool hasWatchCredit() const;

// 磁盘文件的 mtime（unix 秒）。文件不存在 → fail。供外部修改检测用。
    core::Result<int64_t> fileMtime(const std::filesystem::path& relPath) const;

// 路径安全校验：拒绝绝对路径、盘符、.. 逃逸、反斜杠（create 内部同规则兜底）。
    static bool isSafeNewNotePath(const std::string& name);


// 一级子目录清单（磁盘实时枚举，含空目录；返回目录名，按字母序）。
    core::Result<std::vector<std::string>> listTopFolders() const;

// 全层子目录清单（磁盘递归枚举，含空目录），供目录树用。返回 POSIX 相对路径
//（无尾部 '/'，按字母序）。枚举不截断（更深层目录下的文件仍要能显示）。
    core::Result<std::vector<std::string>> listAllFolders() const;

// 新建文件夹（相对路径；安全规则同 isSafeNewNotePath，已存在不算错）
    core::Result<void> createFolder(const std::string& name);

    // 笔记目录（绝对路径）
    const std::filesystem::path& notesDir() const { return notesDir_; }

// 从内容提取标题（"# Title" 第一行）；空则用文件名
    static std::string extractTitle(const std::string& content,
                                    const std::filesystem::path& relPath);

// 确保 relPath 是 POSIX 风格（'/' 分隔）
    static std::string toPosix(const std::filesystem::path& p);

private:
    core::data::SqliteDb& db_;
    std::filesystem::path notesDir_;

// 原生文件监听：ctor 启动 watch notesDir_（失败仅记日志，降级纯轮询）；
// 停止统一在析构函数体内完成。
    std::unique_ptr<platform::FileWatcher> watcher_;
// watcher 有事件但模块层尚未消费（pollChanged 读取后清除）；仅「索引/目录树
// 相关」批置位（见 hasWatchCredit 契约）。
    std::atomic<bool> watcherFired_{false};
// 串行化 rescan：watcher 线程与 UI 线程（模块 5s tick）可能并发进入
    std::mutex rescanMu_;

// FileWatcher 回调（后台线程）：置标记 + 尽力即刻同步
    void onWatchEvent(std::vector<platform::FileWatchEvent> events);

// rescan 实体（调用方须已持有 rescanMu_）
    core::Result<void> rescanLocked();

// relPath (POSIX) → 笔记目录内绝对路径。先 lexically_normal 再逐组件确认仍在
// notesDir_ 之内（LLM 可控路径会进这里）；逃逸 → fail（ValidationError）。
    core::Result<std::filesystem::path> resolve(
        const std::filesystem::path& relPath) const;

// 把文件 upsert 到索引 + FTS（不做删除清理）。force=true 跳过 (size,mtime)
// 短路：watcher 事件已确证文件动过，秒级粒度不可信。
    core::Result<void> syncOneToIndex(const std::filesystem::path& absPath,
                                      const std::string& relPathPosix,
                                      bool force = false);

// 从索引 + FTS 删除
    core::Result<void> deleteFromIndex(const std::string& relPathPosix);

// 把 NoteMeta 从 SqlRow 还原
    static NoteMeta rowToNote(const core::data::SqlRow& row);
};

} // namespace module::markdown