// src/module_markdown/md_tree.cpp — buildTreeRows 实现（见 md_tree.h 设计说明）
#include "md_tree.h"

#include <functional>
#include <map>

namespace module::markdown {

namespace {

// 相对路径的父目录（POSIX；无 '/' → ""，即根组）
std::string parentOf(const std::string& relPath) {
    const size_t slash = relPath.rfind('/');
    return slash == std::string::npos ? std::string() : relPath.substr(0, slash);
}

// 展示层级 = 相对路径里的 '/' 个数（"工作" → 0、"工作/周会" → 1、再深逐级 +1）
int folderDepth(const std::string& dir) {
    int depth = 0;
    for (char c : dir) {
        if (c == '/') ++depth;
    }
    return depth;
}

// 目录行的显示名 = 路径最后一段（"工作/周会" → "周会"）
std::string lastSegment(const std::string& dir) {
    const size_t slash = dir.rfind('/');
    return slash == std::string::npos ? dir : dir.substr(slash + 1);
}

} // namespace

std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed) {
    // 目录集合 = 磁盘枚举（全层，含空目录）∪ 笔记路径的**全部**祖先目录
    std::set<std::string> dirs;
    // 目录 → 直接子文件（保持 notes 传入顺序 = mtime 倒序）；一次分桶，
    // 递归时不重复扫全表
    std::map<std::string, std::vector<const NoteMeta*>> filesOf;
    for (const auto& n : notes) {
        const std::string p = n.relativePath.generic_string();
        size_t slash = p.find('/');
        while (slash != std::string::npos) {
            dirs.insert(p.substr(0, slash));  // 逐级祖先目录都进树
            slash = p.find('/', slash + 1);
        }
        filesOf[parentOf(p)].push_back(&n);
    }
    for (const auto& d : extraFolders) {
        if (!d.empty()) dirs.insert(d);
    }

    // 目录 → 直接子目录；dirs 是 std::set（字节序）→ 桶内顺序即字母序，
    // 子目录行序稳定（与旧的「顶层夹 / 子夹按 std::set 顺序」一致）
    std::map<std::string, std::vector<std::string>> childrenOf;
    for (const auto& d : dirs) {
        const std::string parent = parentOf(d);
        if (parent == d) continue;  // 防御：自指不递归（正常数据不会出现）
        childrenOf[parent].push_back(d);
    }

    std::vector<MdTreeRow> rows;
    rows.reserve(notes.size() + dirs.size() + 1);

    // 展开一个目录：目录行 → 直接文件行 → 各级子目录行（逐个递归）。
    // 收起（collapsed 命中）→ 该目录之下整棵子树不出行。
    // isRootGroup 只影响显示名：根组的 relPath 是 ""（建不出同名目录，故用
    // 常量「根目录」当标题；顶层夹显示自己的名字）
    std::function<void(const std::string&, bool)> emit = [&](const std::string& dir,
                                                            bool isRootGroup) {
        MdTreeRow folder;
        folder.isFolder = true;
        folder.name = isRootGroup ? std::string("根目录") : lastSegment(dir);
        folder.relPath = dir;
        folder.depth = folderDepth(dir);
        rows.push_back(folder);
        if (collapsed.count(dir) != 0) return;  // 收起：整棵子树隐藏

        const int childDepth = folder.depth + 1;
        if (auto it = filesOf.find(dir); it != filesOf.end()) {
            for (const NoteMeta* n : it->second) {
                MdTreeRow r;
                r.isFolder = false;
                r.name = n->relativePath.filename().string();  // 含 .md（完整文件名）
                r.relPath = n->relativePath.generic_string();
                r.depth = childDepth;
                rows.push_back(r);
            }
        }
        if (auto it = childrenOf.find(dir); it != childrenOf.end()) {
            for (const auto& child : it->second) emit(child, false);
        }
    };

    // 根组标题常显且置顶——笔记目录全是根目录散文件（零子文件夹）时树也不会
    // 退化成无结构的平铺列表，「这是树」始终可感知；空目录时树里也保留这一行，
    // 由 UI 层叠加空态提示
    emit("", true);
    return rows;
}

} // namespace module::markdown
