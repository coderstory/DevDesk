// src/module_git/git_change_list_model.cpp
#include "git_change_list_model.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace module::git {

namespace {

// POSIX 相对路径的父目录（无 '/' → ""，即根层）；末段显示名。
std::string parentOf(const std::string& relPath) {
    const size_t slash = relPath.rfind('/');
    return slash == std::string::npos ? std::string() : relPath.substr(0, slash);
}

std::string lastSegment(const std::string& relPath) {
    const size_t slash = relPath.rfind('/');
    return slash == std::string::npos ? relPath : relPath.substr(slash + 1);
}

// untracked 目录条目路径尾 '/'（libgit2 不开 RECURSE_UNTRACKED_DIRS：目录整体
// 一条，git_repo.h:17）；裸 rfind 取末段会取到空串 = 行无名字（bug280 先例）。
// 剥尾归一（git_tree_model.cpp:140-152 先例）。
std::string stripTrailingSlash(const std::string& path) {
    std::string rel = path;
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    return rel;
}

int lazyKindRank(fs_walk::EntryKind kind) {
    return kind == fs_walk::EntryKind::File ? 1 : 0;
}

bool lazyEntryLess(const fs_walk::FsEntry& a, const fs_walk::FsEntry& b) {
    const int ra = lazyKindRank(a.kind);
    const int rb = lazyKindRank(b.kind);
    if (ra != rb) return ra < rb;
    return a.name < b.name;
}

// ---- 树派生内部节点（md_tree.cpp:34-99 同构：祖先目录集 + 逐级 emit）----
struct DirNode {
    std::string path;       // 链最深段目录路径（压缩节点=链尾）
    std::string display;    // 显示名（末段+'/'；压缩链 "a/b/c/"）
    bool untracked = false; // untracked 目录条目（U 徽标、lazy、childCount=0——R4）
    size_t childCount = 0;  // 子树变更条目数（untracked 行恒 0）
    std::vector<DirNode> subdirs;  // 名字节序
    std::vector<size_t> fileIdx;   // 直接文件 → 组内扁平下标（名字节序）
};

// 单链压缩（设计 §4.4：唯一子目录链合并显示，VSCode Compact Folders 惯例，
// clue#199）：无直接文件且唯一子目录（该子目录非 untracked——lazy 节点不参与
// 穿透合并）时把链并进本节点显示。
DirNode compressChain(DirNode node) {
    while (node.fileIdx.empty() && node.subdirs.size() == 1 &&
           !node.subdirs[0].untracked) {
        DirNode only = std::move(node.subdirs[0]);
        node.display += only.display;  // 子节点已自压缩，display 即相对段链
        node.path = std::move(only.path);
        node.subdirs = std::move(only.subdirs);
        node.fileIdx = std::move(only.fileIdx);
        node.childCount = only.childCount;
    }
    for (auto& sd : node.subdirs) sd = compressChain(std::move(sd));
    return node;
}

} // namespace

void GitChangeListModel::reset() {
    staged_.clear();
    unstaged_.clear();
    untrackedDirs_.clear();
    expandedTree_.clear();
    expandedFlat_.clear();
    collapsedTree_.clear();
    collapsedFlat_.clear();
    groupCollapsed_.clear();
    groupTouched_.clear();
    untrackedChildren_.clear();
    loadingDirs_.clear();
    dirErrors_.clear();
    rows_.clear();
}

void GitChangeListModel::rebuild(const platform::git::StatusSnapshot& snapshot,
                                 ChangeViewMode mode) {
    viewMode_ = mode;
    staged_.clear();
    unstaged_.clear();
    untrackedDirs_.clear();
    for (const auto& entry : snapshot.entries) {
        const std::string rel = stripTrailingSlash(entry.path);
        if (rel.empty()) continue;  // 防御：根条目无行可落
        if (rel.size() != entry.path.size()) {
            // untracked 目录条目：目录整体一条（git_repo.h:17），恒归未暂存组
            //（D6-A：untracked 并入未暂存组无独立组）
            untrackedDirs_.insert(rel);
            continue;
        }
        auto& bucket = entry.staged ? staged_ : unstaged_;
        const auto it = bucket.find(rel);
        if (it == bucket.end()) {
            bucket.emplace(rel, ChangeEntry{entry.kind, entry.oldPath});
        } else if (entry.kind == platform::git::StatusKind::Renamed ||
                   (it->second.kind != platform::git::StatusKind::Renamed &&
                    entry.kind == platform::git::StatusKind::Deleted)) {
            // 组内同路径重复条目（防御）：保更「重」类别（口径同旧 applyStatus 合并）
            it->second.kind = entry.kind;
            if (!entry.oldPath.empty()) it->second.oldPath = entry.oldPath;
        }
    }
    // >200 组头默认折叠注入（POC②；用户手动切换过的组不重注入——折叠态记忆，
    // 设计 §6.4；计数回落 ≤200 不自动展开）
    const size_t totalEntries =
        staged_.size() + unstaged_.size() + untrackedDirs_.size();
    if (totalEntries > kGroupCollapseThreshold) {
        if (groupTouched_.count(ChangeListRow::Group::Staged) == 0) {
            groupCollapsed_.insert(ChangeListRow::Group::Staged);
        }
        if (groupTouched_.count(ChangeListRow::Group::Unstaged) == 0) {
            groupCollapsed_.insert(ChangeListRow::Group::Unstaged);
        }
    }
    derive();
}

void GitChangeListModel::setViewMode(ChangeViewMode mode) {
    if (viewMode_ == mode) return;
    viewMode_ = mode;
    derive();
}

void GitChangeListModel::setExpanded(const std::string& dirRel, bool expanded) {
    auto& bitmap = viewMode_ == ChangeViewMode::Tree ? expandedTree_ : expandedFlat_;
    auto& collapsed = viewMode_ == ChangeViewMode::Tree ? collapsedTree_ : collapsedFlat_;
    if (expanded) {
        bitmap.insert(dirRel);      // untracked lazy：显式展开
        collapsed.erase(dirRel);    // tracked 派生：撤显式折叠
    } else {
        bitmap.erase(dirRel);       // untracked lazy：收回
        collapsed.insert(dirRel);   // tracked 派生：显式折叠记忆（F-09）
    }
    derive();
}

bool GitChangeListModel::isExpanded(const std::string& dirRel) const {
    // 双默认态（设计 §7 N2）：lazy 目录=显式展开才开；tracked 派生目录=默认开，
    // 显式折叠才收
    if (isLazyTarget(dirRel)) {
        const auto& bitmap =
            viewMode_ == ChangeViewMode::Tree ? expandedTree_ : expandedFlat_;
        return bitmap.count(dirRel) > 0;
    }
    const auto& collapsed =
        viewMode_ == ChangeViewMode::Tree ? collapsedTree_ : collapsedFlat_;
    return collapsed.count(dirRel) == 0;
}

bool GitChangeListModel::hasFile(const std::string& relPath) const {
    if (staged_.count(relPath) > 0 || unstaged_.count(relPath) > 0) return true;
    for (const auto& [dir, kids] : untrackedChildren_) {
        for (const auto& r : kids) {
            if (r.kind == ChangeListRow::Kind::File && r.relPath == relPath) {
                return true;
            }
        }
    }
    return false;
}

void GitChangeListModel::setGroupCollapsed(ChangeListRow::Group group,
                                           bool collapsed) {
    groupTouched_.insert(group);
    if (collapsed) {
        groupCollapsed_.insert(group);
    } else {
        groupCollapsed_.erase(group);
    }
    derive();
}

bool GitChangeListModel::isGroupCollapsed(ChangeListRow::Group group) const {
    return groupCollapsed_.count(group) > 0;
}

void GitChangeListModel::markUntrackedLoading(const std::string& dirRel) {
    loadingDirs_.insert(dirRel);
    derive();
}

void GitChangeListModel::markUntrackedFailed(const std::string& dirRel,
                                             const std::string& message) {
    loadingDirs_.erase(dirRel);
    dirErrors_[dirRel] = message;
    derive();
}

void GitChangeListModel::requestRetry(const std::string& dirRel) {
    // 清错误 → 回到待加载；pendingUntrackedDirs() 下次 compose 周期重新投递
    dirErrors_.erase(dirRel);
    derive();
}

void GitChangeListModel::applyUntrackedChildren(
    const std::string& dirRel, const fs_walk::WalkResult& result) {
    loadingDirs_.erase(dirRel);
    if (!result.ok) {
        dirErrors_[dirRel] = result.message.empty() ? std::string("无法读取目录")
                                                    : result.message;
        derive();
        return;
    }
    // 防御：dirRel 已不是可展开 lazy 目录（刷新后条目消失等过期回包）→ 丢弃
    if (!isLazyTarget(dirRel)) {
        derive();
        return;
    }
    dirErrors_.erase(dirRel);
    std::vector<fs_walk::FsEntry> entries = result.entries;
    std::sort(entries.begin(), entries.end(), lazyEntryLess);  // 防御重排（walk 已排好）
    size_t fileTotal = 0;
    for (const auto& e : entries) {
        if (e.kind == fs_walk::EntryKind::File) ++fileTotal;
    }
    std::vector<ChangeListRow> kids;
    kids.reserve(entries.size() + 1);
    size_t fileTaken = 0;
    for (const auto& e : entries) {
        ChangeListRow row;
        row.group = ChangeListRow::Group::Unstaged;
        row.name = e.name;
        row.relPath = e.relPath;
        row.status = platform::git::StatusKind::Untracked;
        if (e.kind == fs_walk::EntryKind::File) {
            if (fileTaken >= kMaxUntrackedChildren) continue;  // >500 截断（设计 §6.2）
            ++fileTaken;
            row.kind = ChangeListRow::Kind::File;
        } else {
            // Directory/Subrepo 都续 lazy（展开=对该 dirRel 再投 walkChildren；
            // Subrepo 内枚举正常工作，只读边界不破）
            row.kind = ChangeListRow::Kind::Dir;
            row.dirRel = e.relPath;
            row.name = e.name + "/";
            row.childCount = 0;  // R4：lazy 前不可得
        }
        kids.push_back(std::move(row));
    }
    if (fileTotal > kMaxUntrackedChildren) {
        ChangeListRow tail;
        tail.kind = ChangeListRow::Kind::Truncate;
        tail.group = ChangeListRow::Group::Unstaged;
        tail.dirRel = dirRel;
        tail.remainCount = fileTotal - kMaxUntrackedChildren;
        kids.push_back(std::move(tail));
    }
    untrackedChildren_[dirRel] = std::move(kids);
    derive();
}

std::vector<std::string> GitChangeListModel::pendingUntrackedDirs() const {
    std::set<std::string> lazyAll(untrackedDirs_);
    for (const auto& [dirRel, kids] : untrackedChildren_) {
        collectLazyDirs(kids, lazyAll);
    }
    std::vector<std::string> out;
    for (const auto& dirRel : lazyAll) {
        if (isExpanded(dirRel) && untrackedChildren_.count(dirRel) == 0 &&
            loadingDirs_.count(dirRel) == 0 && dirErrors_.count(dirRel) == 0) {
            out.push_back(dirRel);
        }
    }
    // 浅路径在前 → 首屏根层先回（旧 GitTreeModel::pendingDirs 同款）
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() < b.size();
              });
    return out;
}

void GitChangeListModel::derive() {
    rows_.clear();
    static const std::set<std::string> kNoLazyDirs;  // staged 组无 lazy 目录
    emitGroup(ChangeListRow::Group::Staged, staged_, kNoLazyDirs);
    emitGroup(ChangeListRow::Group::Unstaged, unstaged_, untrackedDirs_);
}

void GitChangeListModel::emitGroup(
    ChangeListRow::Group group,
    const std::map<std::string, ChangeEntry>& files,
    const std::set<std::string>& lazyDirs) {
    // 组内计数（设计 §4.3）：文件条目 + untracked 目录条目（各计 1）+ 已展开
    // lazy 子文件行（展开后子文件计入未暂存计数）
    size_t count = files.size() + lazyDirs.size();
    for (const auto& dirRel : lazyDirs) count += countLazyFiles(dirRel);
    if (count == 0) return;  // 空组整组隐藏（设计 §4.3；两组皆空=整页空态由视图承担）
    ChangeListRow header;
    header.kind = ChangeListRow::Kind::GroupHeader;
    header.group = group;
    header.name = std::string(group == ChangeListRow::Group::Staged ? "已暂存"
                                                                    : "未暂存") +
                  " (" + std::to_string(count) + ")";  // 计数=组内文件行数（§4.3，视图直绘）
    header.childCount = count;
    rows_.push_back(header);
    if (groupCollapsed_.count(group) > 0) return;  // 折叠：子树不出行（§4.3）

    if (viewMode_ == ChangeViewMode::Flat) {
        emitGroupFlat(group, files, lazyDirs);
    } else {
        emitGroupTree(group, files, lazyDirs);
    }
}

void GitChangeListModel::emitGroupTree(
    ChangeListRow::Group group,
    const std::map<std::string, ChangeEntry>& files,
    const std::set<std::string>& lazyDirs) {
    // 祖先目录集（仅文件路径派生；untracked 目录不派生祖先——无变更的中间目录不入树）
    std::set<std::string> allDirs;
    for (const auto& [path, entry] : files) {
        size_t slash = path.find('/');
        while (slash != std::string::npos) {
            allDirs.insert(path.substr(0, slash));
            slash = path.find('/', slash + 1);
        }
    }
    // 直接子目录（字节序）：derived 全层 ∪ 挂靠的 untracked 条目
    std::map<std::string, std::vector<std::string>> subMap;
    for (const auto& d : allDirs) {
        const std::string parent = parentOf(d);
        if (parent != d) subMap[parent].push_back(d);  // 防御：自指不递归
    }
    for (const auto& d : lazyDirs) {
        if (allDirs.count(d) > 0) continue;  // 碰撞防御：derived 节点优先（libgit2 语义下不相容）
        // 整链一节点挂最近派生祖先（无则挂根；设计：无变更中间目录不入树——
        // 按 parentOf 直挂会落进未派生幻影父目录成孤儿行，永不渲染）
        std::string anchor = parentOf(d);
        while (!anchor.empty() && allDirs.count(anchor) == 0) anchor = parentOf(anchor);
        subMap[anchor].push_back(d);
    }
    for (auto& [parent, kids] : subMap) {
        std::sort(kids.begin(), kids.end());  // 两来源合并后重排保字节序
    }
    // 直接文件 → 扁平表 + 目录下标桶（map 迭代 = 路径字节序，桶内同序）
    std::vector<std::pair<const std::string*, const ChangeEntry*>> fileFlat;
    fileFlat.reserve(files.size());
    std::map<std::string, std::vector<size_t>> fileMap;
    for (const auto& [path, entry] : files) {
        fileMap[parentOf(path)].push_back(fileFlat.size());
        fileFlat.push_back({&path, &entry});
    }

    std::function<std::vector<DirNode>(const std::string&)> buildSubdirs =
        [&](const std::string& dir) -> std::vector<DirNode> {
        std::vector<DirNode> out;
        const auto subIt = subMap.find(dir);
        if (subIt == subMap.end()) return out;
        for (const auto& child : subIt->second) {
            DirNode node;
            node.path = child;
            node.untracked = lazyDirs.count(child) > 0;
            if (node.untracked) {
                // untracked 条目：完整链展示 + 叶化（子层 lazy，R4 计数不可得）
                node.display = child + "/";
                node.childCount = 0;
            } else {
                node.display = lastSegment(child) + "/";
                node.subdirs = buildSubdirs(child);
                const auto fileIt = fileMap.find(child);
                if (fileIt != fileMap.end()) node.fileIdx = fileIt->second;
                size_t subtree = node.fileIdx.size();
                for (const auto& sd : node.subdirs) {
                    subtree += sd.untracked ? 1 : sd.childCount;
                }
                node.childCount = subtree;
            }
            out.push_back(compressChain(std::move(node)));
        }
        return out;
    };

    const std::vector<DirNode> rootDirs = buildSubdirs(std::string());
    std::vector<size_t> rootFiles;
    const auto rootFileIt = fileMap.find(std::string());
    if (rootFileIt != fileMap.end()) rootFiles = rootFileIt->second;

    // 展开一个目录：目录行 → 直接文件行 → 各子目录行（递归）；untracked 目录行
    // 展开出 lazy 子行（emitLazyRows 三态）
    std::function<void(const DirNode&, int)> emitDir =
        [&](const DirNode& node, int depth) {
            ChangeListRow row;
            row.kind = ChangeListRow::Kind::Dir;
            row.group = group;
            row.depth = depth;
            row.name = node.display;
            row.relPath = node.path;
            row.dirRel = node.path;
            if (node.untracked) {
                row.status = platform::git::StatusKind::Untracked;
                row.childCount = 0;  // R4：子树计数 lazy 前不可得，不显
            } else {
                row.childCount = node.childCount;
            }
            rows_.push_back(row);
            if (!isExpanded(node.path)) return;
            if (node.untracked) {
                emitLazyRows(node.path, depth + 1, group);
                return;
            }
            for (const auto& sd : node.subdirs) emitDir(sd, depth + 1);
            for (size_t idx : node.fileIdx) {
                emitFileRow(*fileFlat[idx].first, *fileFlat[idx].second,
                            depth + 1, group);
            }
        };
    for (const auto& node : rootDirs) emitDir(node, 0);
    for (size_t idx : rootFiles) {
        emitFileRow(*fileFlat[idx].first, *fileFlat[idx].second, 0, group);
    }
}

void GitChangeListModel::emitGroupFlat(
    ChangeListRow::Group group,
    const std::map<std::string, ChangeEntry>& files,
    const std::set<std::string>& lazyDirs) {
    // 目录段聚合排序（设计 §4.7：同目录相邻）= 全路径字节序（同前缀自然相邻）；
    // untracked 目录条目混排其中（U 徽标，无独立节）
    std::vector<std::pair<const std::string*, const ChangeEntry*>> flat;
    flat.reserve(files.size());
    for (const auto& [path, entry] : files) flat.push_back({&path, &entry});
    const std::vector<std::string> lazySorted(lazyDirs.begin(), lazyDirs.end());
    size_t i = 0, j = 0;
    while (i < flat.size() || j < lazySorted.size()) {
        const bool takeFile =
            j >= lazySorted.size() ||
            (i < flat.size() && *flat[i].first < lazySorted[j]);
        if (takeFile) {
            emitFileRow(*flat[i].first, *flat[i].second, 0, group);
            ++i;
            continue;
        }
        const std::string& dirRel = lazySorted[j];
        ChangeListRow row;
        row.kind = ChangeListRow::Kind::Dir;
        row.group = group;
        row.depth = 0;
        row.name = dirRel + "/";
        row.relPath = dirRel;
        row.dirRel = dirRel;
        row.status = platform::git::StatusKind::Untracked;
        row.childCount = 0;
        rows_.push_back(row);
        if (isExpanded(dirRel)) emitLazyRows(dirRel, 1, group);
        ++j;
    }
}

void GitChangeListModel::emitFileRow(const std::string& path,
                                     const ChangeEntry& entry, int depth,
                                     ChangeListRow::Group group) {
    ChangeListRow row;
    row.kind = ChangeListRow::Kind::File;
    row.group = group;
    row.depth = depth;
    row.name = lastSegment(path);
    row.relPath = path;
    row.status = entry.kind;
    row.oldPath = entry.oldPath;
    rows_.push_back(row);
}

void GitChangeListModel::emitLazyRows(const std::string& dirRel, int depth,
                                      ChangeListRow::Group group) {
    if (loadingDirs_.count(dirRel) > 0) {
        // 加载行（>1s 才转圈由视图按时间戳升级，模型恒出占位行——validateSpinnerDue
        // 先例 git_page_controller.h:110）
        ChangeListRow row;
        row.kind = ChangeListRow::Kind::Loading;
        row.group = group;
        row.depth = depth;
        row.name = "正在读取…";
        row.dirRel = dirRel;
        rows_.push_back(row);
        return;
    }
    const auto errIt = dirErrors_.find(dirRel);
    if (errIt != dirErrors_.end()) {
        // 行内错误行（枚举失败显式报错，禁静默空——bug#290 同族根治口径），
        // 目录行保持展开可重试（设计 §6.2）
        ChangeListRow row;
        row.kind = ChangeListRow::Kind::Error;
        row.group = group;
        row.depth = depth;
        row.dirRel = dirRel;
        row.errorText = errIt->second;
        rows_.push_back(row);
        return;
    }
    const auto it = untrackedChildren_.find(dirRel);
    if (it == untrackedChildren_.end()) return;  // 已展开未加载（pending 投递前瞬间）
    for (const auto& cached : it->second) {
        ChangeListRow row = cached;
        row.depth = depth;
        rows_.push_back(row);
        if (row.kind == ChangeListRow::Kind::Dir && isExpanded(row.dirRel)) {
            emitLazyRows(row.dirRel, depth + 1, group);
        }
    }
}

size_t GitChangeListModel::countLazyFiles(const std::string& dirRel) const {
    if (!isExpanded(dirRel)) return 0;
    const auto it = untrackedChildren_.find(dirRel);
    if (it == untrackedChildren_.end()) return 0;
    size_t n = 0;
    for (const auto& r : it->second) {
        if (r.kind == ChangeListRow::Kind::File) {
            ++n;
        } else if (r.kind == ChangeListRow::Kind::Dir) {
            n += countLazyFiles(r.dirRel);  // 嵌套展开目录的子文件一并计入
        }
    }
    return n;
}

void GitChangeListModel::collectLazyDirs(const std::vector<ChangeListRow>& rows,
                                         std::set<std::string>& out) const {
    for (const auto& r : rows) {
        if (r.kind != ChangeListRow::Kind::Dir || r.dirRel.empty()) continue;
        out.insert(r.dirRel);
        const auto it = untrackedChildren_.find(r.dirRel);
        if (it != untrackedChildren_.end()) collectLazyDirs(it->second, out);
    }
}

bool GitChangeListModel::isLazyTarget(const std::string& dirRel) const {
    std::set<std::string> lazyAll(untrackedDirs_);
    for (const auto& [root, kids] : untrackedChildren_) {
        collectLazyDirs(kids, lazyAll);
    }
    return lazyAll.count(dirRel) > 0;
}

char statusBadgeLetter(platform::git::StatusKind kind) {
    switch (kind) {
        case platform::git::StatusKind::New: return 'A';
        case platform::git::StatusKind::Modified: return 'M';
        case platform::git::StatusKind::Deleted: return 'D';
        case platform::git::StatusKind::Renamed: return 'R';
        case platform::git::StatusKind::Untracked: return 'U';
        case platform::git::StatusKind::Ignored: return 'I';
    }
    return '?';
}

} // namespace module::git
