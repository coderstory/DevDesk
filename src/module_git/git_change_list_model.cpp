// src/module_git/git_change_list_model.cpp
#include "git_change_list_model.h"

#include <algorithm>
#include <cctype>
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

// ASCII 大小写折叠（原 git_search_model 同款：A-Z 逐字节映射 a-z，其余原样；
// 字节 1:1 变换 → 折叠串上的字节偏移与原串一致）。
std::string asciiFold(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
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
    bool lazy = false;      // untracked 嵌套 lazy 派生目录锚（懒枚举边界，不参与穿透合并）
    size_t childCount = 0;  // 子树文件行数（纯结构信息——文件夹原则令）
    std::vector<DirNode> subdirs;  // 名字节序
    std::vector<size_t> fileIdx;   // 直接文件 → 组内扁平下标（名字节序）
};

// 单链压缩（设计 §4.4：唯一子目录链合并显示，VSCode Compact Folders 惯例，
// clue#199）：无直接文件且唯一子目录（该子目录非 lazy 锚——懒枚举边界不穿透）时
// 把链并进本节点显示。
DirNode compressChain(DirNode node) {
    while (node.fileIdx.empty() && node.subdirs.size() == 1 && !node.subdirs[0].lazy) {
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
    filterQuery_.clear();
    filterFolded_.clear();
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
            //（D6-A）；裁决④：不再产 Dir 行，控制器自动枚举首层后文件行并入派生
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

void GitChangeListModel::setFilter(const std::string& rawQuery) {
    filterQuery_ = rawQuery;
    filterFolded_ = asciiFold(rawQuery);
    derive();
}

bool GitChangeListModel::matchesFilter(const std::string& relPath) const {
    if (filterFolded_.empty()) return true;
    return asciiFold(relPath).find(filterFolded_) != std::string::npos;
}

std::map<std::string, GitChangeListModel::ChangeEntry>
GitChangeListModel::mergedUnstagedFiles() const {
    // 未暂存有效文件集（裁决④）：tracked 条目 + 顶层 untracked 目录已枚举的
    // 直接文件行（嵌套内容仍走 lazy，不在此并入——展开才可见）。tracked 优先。
    std::map<std::string, ChangeEntry> merged = unstaged_;
    for (const auto& dirRel : untrackedDirs_) {
        const auto it = untrackedChildren_.find(dirRel);
        if (it == untrackedChildren_.end()) continue;
        for (const auto& kid : it->second) {
            if (kid.kind != ChangeListRow::Kind::File) continue;
            merged.emplace(kid.relPath,
                           ChangeEntry{platform::git::StatusKind::Untracked, {}});
        }
    }
    return merged;
}

std::set<std::string> GitChangeListModel::nestedLazyDirs() const {
    // 嵌套 lazy 派生目录 = 各顶层缓存内 Dir 行递归收集（裁决④：顶层条目不再是
    // lazy 容器，只有枚举出的子目录才续 lazy）
    std::set<std::string> out;
    for (const auto& dirRel : untrackedDirs_) {
        const auto it = untrackedChildren_.find(dirRel);
        if (it != untrackedChildren_.end()) collectLazyDirs(it->second, out);
    }
    return out;
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
    // 双默认态（设计 §7 N2）：lazy 派生目录=显式展开才开；tracked 派生目录=默认开，
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

std::optional<platform::git::StatusKind> GitChangeListModel::fileStatus(
    const std::string& relPath) const {
    const auto inBucket = [&relPath](const std::map<std::string, ChangeEntry>& b) {
        const auto it = b.find(relPath);
        return it == b.end() ? std::nullopt : std::optional(it->second.kind);
    };
    if (auto s = inBucket(staged_)) return s;
    if (auto s = inBucket(unstaged_)) return s;
    for (const auto& [dir, kids] : untrackedChildren_) {
        for (const auto& r : kids) {
            if (r.kind == ChangeListRow::Kind::File && r.relPath == relPath) {
                return r.status;
            }
        }
    }
    return std::nullopt;
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
    // 防御：dirRel 已非有效枚举目标（刷新后条目消失等过期回包）→ 丢弃
    if (untrackedDirs_.count(dirRel) == 0 && !isLazyTarget(dirRel)) {
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
        if (e.kind == fs_walk::EntryKind::File) {
            if (fileTaken >= kMaxUntrackedChildren) continue;  // >500 截断（设计 §6.2）
            ++fileTaken;
            row.kind = ChangeListRow::Kind::File;
            row.status = platform::git::StatusKind::Untracked;  // U 徽标只落文件行
        } else {
            // Directory/Subrepo 都续 lazy（展开=对该 dirRel 再投 walkChildren；
            // Subrepo 内枚举正常工作，只读边界不破）。原则令：派生目录行不带
            // 徽标、不计数（lazy 枚举前不可得）
            row.kind = ChangeListRow::Kind::Dir;
            row.lazy = true;
            row.dirRel = e.relPath;
            row.name = e.name + "/";
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
    std::vector<std::string> out;
    const auto pending = [&](const std::string& dirRel, bool requireExpanded) {
        if (requireExpanded && !isExpanded(dirRel)) return;
        if (untrackedChildren_.count(dirRel) > 0) return;  // 已缓存
        if (loadingDirs_.count(dirRel) > 0) return;        // 在途
        if (dirErrors_.count(dirRel) > 0) return;          // 错误待重试
        out.push_back(dirRel);
    };
    // ①顶层 status 条目：自动枚举（裁决④，无视展开）
    for (const auto& dirRel : untrackedDirs_) pending(dirRel, false);
    // ②嵌套 lazy 派生目录：展开才枚举（点击动线，D3-A）
    for (const auto& dirRel : nestedLazyDirs()) pending(dirRel, true);
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
    emitGroup(ChangeListRow::Group::Unstaged, mergedUnstagedFiles(), nestedLazyDirs());
}

void GitChangeListModel::emitGroup(
    ChangeListRow::Group group,
    const std::map<std::string, ChangeEntry>& files,
    const std::set<std::string>& lazyDirs) {
    // 过滤态预裁剪（need:299 §4.9）：命中文件集 +（自身路径命中或子缓存含命中的）
    // lazy 目录集——树=命中链祖先目录自然派生（自动临时展开）；平铺=行级匹配
    std::map<std::string, ChangeEntry> matched;
    std::set<std::string> effectiveLazy = lazyDirs;
    const bool filtering = !filterFolded_.empty();
    if (filtering) {
        for (const auto& [path, entry] : files) {
            if (matchesFilter(path)) matched.emplace(path, entry);
        }
        for (auto it = effectiveLazy.begin(); it != effectiveLazy.end();) {
            if (matchesFilter(*it) || cacheHasMatchedFile(*it)) {
                ++it;
            } else {
                it = effectiveLazy.erase(it);
            }
        }
    }
    const std::map<std::string, ChangeEntry>& effective =
        filtering ? matched : files;
    // 组内计数（原则令：只算文件行——文件夹行不计）：
    //   常态 = 桶内文件 + 已展开 lazy 嵌套目录的可见文件；
    //   过滤态 = 命中文件行数（设计 §4.9；lazy 未枚举内容不可知不计）
    size_t count = effective.size();
    if (filtering) {
        for (const auto& dirRel : effectiveLazy) count += countMatchedLazyFiles(dirRel);
    } else {
        for (const auto& dirRel : effectiveLazy) count += countLazyFiles(dirRel);
    }
    // 空组整组隐藏（§4.3）。过滤态例外（§4.9 无匹配判定=可见行集口径）：lazy
    // 目录自身路径命中时行集非空（有可点行不报无匹配）——组头计数仍=文件行数
    //（原则令），组随可见行集保留；仅目录行可见时计数显 0
    if (count == 0 && effectiveLazy.empty()) return;
    ChangeListRow header;
    header.kind = ChangeListRow::Kind::GroupHeader;
    header.group = group;
    header.name = std::string(group == ChangeListRow::Group::Staged ? "已暂存"
                                                                    : "未暂存") +
                  " (" + std::to_string(count) + ")";
    header.childCount = count;
    rows_.push_back(header);
    if (groupCollapsed_.count(group) > 0) return;  // 折叠：子树不出行（§4.3）

    if (viewMode_ == ChangeViewMode::Flat) {
        emitGroupFlat(group, effective, effectiveLazy);
    } else {
        emitGroupTree(group, effective, effectiveLazy);
    }
    if (group == ChangeListRow::Group::Unstaged) emitTopLevelStateRows(group);
}

void GitChangeListModel::emitTopLevelStateRows(ChangeListRow::Group group) {
    // 顶层 untracked 条目的枚举失败/截断尾行（裁决④：顶层无 Dir 行可挂——
    // 组尾 depth 0 承载；过滤态按目录路径参与匹配）
    for (const auto& dirRel : untrackedDirs_) {
        if (!matchesFilter(dirRel)) continue;
        const auto errIt = dirErrors_.find(dirRel);
        if (errIt != dirErrors_.end()) {
            ChangeListRow row;
            row.kind = ChangeListRow::Kind::Error;
            row.group = group;
            row.dirRel = dirRel;
            row.errorText = errIt->second;
            rows_.push_back(row);
            continue;
        }
        const auto it = untrackedChildren_.find(dirRel);
        if (it == untrackedChildren_.end()) continue;
        for (const auto& kid : it->second) {
            if (kid.kind != ChangeListRow::Kind::Truncate) continue;
            ChangeListRow row = kid;
            row.depth = 0;
            rows_.push_back(row);
        }
    }
}

void GitChangeListModel::emitGroupTree(
    ChangeListRow::Group group,
    const std::map<std::string, ChangeEntry>& files,
    const std::set<std::string>& lazyDirs) {
    // 祖先目录集（仅文件路径派生；无变更的中间目录不入树）。过滤态=调用方已
    // 裁剪为命中文件（祖先链=命中链，自动临时展开）
    std::set<std::string> allDirs;
    for (const auto& [path, entry] : files) {
        size_t slash = path.find('/');
        while (slash != std::string::npos) {
            allDirs.insert(path.substr(0, slash));
            slash = path.find('/', slash + 1);
        }
    }
    // 直接子目录（字节序）：derived 全层 ∪ 挂靠的嵌套 lazy 锚
    std::map<std::string, std::vector<std::string>> subMap;
    for (const auto& d : allDirs) {
        const std::string parent = parentOf(d);
        if (parent != d) subMap[parent].push_back(d);  // 防御：自指不递归
    }
    std::map<std::string, std::string> lazyDisplay;  // lazy 锚 → 相对锚点显示名
    for (const auto& d : lazyDirs) {
        if (allDirs.count(d) > 0) continue;  // 碰撞防御：derived 节点优先
        // 整链一节点挂最近派生祖先（无则挂根；无变更中间目录不入树——按
        // parentOf 直挂会落进未派生幻影父目录成孤儿行，永不渲染）
        std::string anchor = parentOf(d);
        while (!anchor.empty() && allDirs.count(anchor) == 0) anchor = parentOf(anchor);
        subMap[anchor].push_back(d);
        // lazy 锚显示名=相对锚点的剩余段链（锚点已由 derived 节点表达）
        lazyDisplay.emplace(
            d, (anchor.empty() ? d : d.substr(anchor.size() + 1)) + "/");
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
            node.lazy = lazyDirs.count(child) > 0;
            if (node.lazy) {
                // 嵌套 lazy 锚：相对锚点显示剩余段链 + 叶化（子层 lazy，计数枚举前不可得）
                node.display = lazyDisplay.at(child);
                node.childCount = 0;
            } else {
                node.display = lastSegment(child) + "/";
                node.subdirs = buildSubdirs(child);
                const auto fileIt = fileMap.find(child);
                if (fileIt != fileMap.end()) node.fileIdx = fileIt->second;
                size_t subtree = node.fileIdx.size();
                for (const auto& sd : node.subdirs) {
                    subtree += sd.lazy ? 0 : sd.childCount;
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

    // 展开一个目录：目录行 → 直接文件行 → 各子目录行（递归）；lazy 锚展开出
    // 子行（emitLazyRows 三态）。过滤态：derived 目录恒展开（命中文件必有后代），
    // lazy 锚展开=子缓存含命中文件
    std::function<void(const DirNode&, int)> emitDir =
        [&](const DirNode& node, int depth) {
            ChangeListRow row;
            row.kind = ChangeListRow::Kind::Dir;
            row.group = group;
            row.depth = depth;
            row.name = node.display;
            row.relPath = node.path;
            row.dirRel = node.path;
            row.lazy = node.lazy;
            row.childCount = node.childCount;
            rows_.push_back(row);
            const bool open = filterFolded_.empty()
                                  ? isExpanded(node.path)
                                  : (node.lazy ? cacheHasMatchedFile(node.path) : true);
            if (!open) return;
            if (node.lazy) {
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
    // 嵌套 lazy 锚目录行混排其中（裁决④：顶层 untracked 已并入文件行，无目录条目）
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
        row.lazy = true;
        rows_.push_back(row);
        // 过滤态：lazy 锚展开=子缓存含命中文件；常态=位图
        const bool open = filterFolded_.empty() ? isExpanded(dirRel)
                                                : cacheHasMatchedFile(dirRel);
        if (open) emitLazyRows(dirRel, 1, group);
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
        if (cached.kind == ChangeListRow::Kind::File) {
            if (!matchesFilter(cached.relPath)) continue;  // 过滤态行级裁剪
            ChangeListRow row = cached;
            row.depth = depth;
            rows_.push_back(row);
            continue;
        }
        if (cached.kind == ChangeListRow::Kind::Dir) {
            const bool selfMatch = matchesFilter(cached.dirRel);
            const bool descMatch = cacheHasMatchedFile(cached.dirRel);
            if (!selfMatch && !descMatch) continue;  // 过滤态：自身路径或子缓存命中才留
            ChangeListRow row = cached;
            row.depth = depth;
            rows_.push_back(row);
            const bool open = filterFolded_.empty() ? isExpanded(cached.dirRel)
                                                    : descMatch;
            if (open) emitLazyRows(cached.dirRel, depth + 1, group);
            continue;
        }
        // Truncate 尾行：父目录可见即随行（过滤态不按路径裁剪——是状态行不是变更项）
        ChangeListRow row = cached;
        row.depth = depth;
        rows_.push_back(row);
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

size_t GitChangeListModel::countMatchedLazyFiles(const std::string& dirRel) const {
    const auto it = untrackedChildren_.find(dirRel);
    if (it == untrackedChildren_.end()) return 0;
    size_t n = 0;
    for (const auto& r : it->second) {
        if (r.kind == ChangeListRow::Kind::File) {
            if (matchesFilter(r.relPath)) ++n;
        } else if (r.kind == ChangeListRow::Kind::Dir) {
            n += countMatchedLazyFiles(r.dirRel);
        }
    }
    return n;
}

bool GitChangeListModel::cacheHasMatchedFile(const std::string& dirRel) const {
    return countMatchedLazyFiles(dirRel) > 0;
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
    return nestedLazyDirs().count(dirRel) > 0;
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
