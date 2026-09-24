// src/module_markdown/markdown_service.h
// Markdown 笔记本服务：文件系统 I/O + SQLite 索引 + FTS5 搜索
//
// 设计要点（参见 spec §8.1）：
//   - 文件存磁盘（core::paths::markdownDir()），UTF-8 编码
//   - 索引同步到 markdown_index 表（path / title / size / mtime）
//   - FTS5 同步到 markdown_fts 表（path UNINDEXED + title + content）
//   - 相对路径用 POSIX 分隔符（'/'），跨平台一致
//   - 所有失败走 Result<T>，不抛异常（devdesk 约定）
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"

#include <filesystem>
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

// 标签 + 引用它的笔记数（listAllTags 用）
struct TagCount {
    std::string name;
    int64_t noteCount = 0;
};

class MarkdownService {
public:
    MarkdownService(core::data::SqliteDb& db, const std::filesystem::path& notesDir);

    // 扫描 notesDir，同步到 markdown_index / markdown_fts：
    //   - 新文件 → 插入
    //   - 删除文件 → 从索引 + FTS 删除
    //   - 修改文件（mtime/size 变化）→ 更新索引 + FTS
    core::Result<void> rescan();

    // 列出所有笔记（按 modified_at DESC）
    core::Result<std::vector<NoteMeta>> listAll();

    // 列出某目录下的笔记（dir 为空表示根）
    //   目录匹配：relativePath 的父目录 == dir
    core::Result<std::vector<NoteMeta>> listInDirectory(
        const std::filesystem::path& dir);

    // 读取笔记全文（UTF-8）
    core::Result<std::string> read(const std::filesystem::path& relPath);

    // 写入笔记：写文件 → 更新索引 → 更新 FTS
    //   relPath 必须带 .md 后缀
    //   content 为 UTF-8 文本
    core::Result<void> write(const std::filesystem::path& relPath,
                       const std::string& content);

    // 新建笔记（写空模板 → 插入索引 + FTS）
    //   relPath 若无 .md 后缀自动补全
    core::Result<void> create(const std::filesystem::path& relPath);

    // 删除笔记（删文件 + 索引 + FTS）
    core::Result<void> remove(const std::filesystem::path& relPath);

    // 重命名/移动笔记（磁盘 rename + 索引 path 就地 UPDATE + FTS 先删后插）。
    // 关键：markdown_index.id 保持不变 → markdown_note_tags 外键关联（标签）
    // 随之保留，不能走 remove+create（那会级联删标签）。newRelPath 无 .md
    // 自动补全；目标已存在 → AlreadyExists；失败时磁盘 rename 回滚。
    // 返回最终生效的 POSIX 相对路径（补全后）。
    core::Result<std::string> rename(const std::filesystem::path& oldRelPath,
                                     const std::filesystem::path& newRelPath);

    // FTS5 全文搜索
    //   返回匹配的笔记元数据（按 FTS5 rank 排序）
    core::Result<std::vector<NoteMeta>> search(const std::string& query);

    // ---- 标签（spec §8.1；手动标签，数据全在 markdown_tags/markdown_note_tags）----

    // 某笔记的全部标签（按名字母序）
    core::Result<std::vector<std::string>> tagsForNote(
        const std::filesystem::path& relPath);

    // 全量替换某笔记的标签（事务：删旧关联 + upsert 标签 + 建新关联 +
    // 清理无笔记引用的孤儿标签）。笔记必须已在索引中，否则 fail。
    core::Result<void> setTags(const std::filesystem::path& relPath,
                               const std::vector<std::string>& tags);

    // 全部标签 + 各自笔记数（按名字母序；只含至少被一篇笔记引用的标签）
    core::Result<std::vector<TagCount>> listAllTags();

    // 含某标签的全部笔记（按 modified_at DESC）
    core::Result<std::vector<NoteMeta>> notesWithTag(const std::string& tag);

    // ---- W8：文件变化自动感知（spec §8.1 承诺的 5 秒轮询底座）----

    // 轻量磁盘轮询：只遍历目录比 mtime/size，不读内容、不写索引、不打日志。
    // 返回 true = 磁盘与索引有差异（新文件/删除/修改），需要调用方 rescan()。
    // mtime 秒级粒度的漏检窗口与升级路径：见实现处 pollChanged 注释 +
    // KNOWN_LIMITATIONS #5（亚秒级需 core schema + platform watcher）。
    core::Result<bool> pollChanged();

    // 磁盘文件的 mtime（unix 秒）。文件不存在 → fail。
    // 用于「当前打开的笔记被外部修改」检测（配合模块层缓存的 lastKnownMtime）。
    core::Result<int64_t> fileMtime(const std::filesystem::path& relPath) const;

    // 新建笔记路径安全校验：拒绝绝对路径、盘符（X:）、.. 逃逸、反斜杠。
    // create() 内部也会用同一规则兜底（trust boundary 在服务层）。
    static bool isSafeNewNotePath(const std::string& name);

    // ---- W9：新建文件夹入口 ----

    // 一级子目录清单（磁盘实时枚举，含空目录；目录树显示空文件夹行用）。
    // 返回目录名（不含路径），按字母序。
    core::Result<std::vector<std::string>> listTopFolders() const;

    // 新建文件夹（相对路径，安全规则同 isSafeNewNotePath；已存在不算错）
    core::Result<void> createFolder(const std::string& name);

    // 笔记目录（绝对路径）
    const std::filesystem::path& notesDir() const { return notesDir_; }

    // 工具函数：从内容提取标题（"# Title" 第一行）；空则用文件名
    static std::string extractTitle(const std::string& content,
                                    const std::filesystem::path& relPath);

    // 工具函数：确保 relPath 是 POSIX 风格（'/' 分隔）
    static std::string toPosix(const std::filesystem::path& p);

private:
    core::data::SqliteDb& db_;
    std::filesystem::path notesDir_;

    // relPath (POSIX) → 绝对路径
    std::filesystem::path resolve(const std::filesystem::path& relPath) const;

    // 把一个文件 upsert 到索引 + FTS（不做 rescan 的删除清理）
    core::Result<void> syncOneToIndex(const std::filesystem::path& absPath,
                                      const std::string& relPathPosix);

    // 从索引 + FTS 删除
    core::Result<void> deleteFromIndex(const std::string& relPathPosix);

    // 把 NoteMeta 从 SqlRow 还原
    static NoteMeta rowToNote(const core::data::SqlRow& row);
};

} // namespace module::markdown