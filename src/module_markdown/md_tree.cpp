// src/module_markdown/md_tree.cpp — buildTreeRows 实现（见 md_tree.h 设计说明）
#include "md_tree.h"

#include <algorithm>

namespace module::markdown {

std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed) {
    // 目录集合 = 笔记路径推导（前两级）∪ 磁盘枚举（全层，含空目录）
    std::set<std::string> dirs = extraFolders;
    for (const auto& n : notes) {
        const std::string p = n.relativePath.generic_string();
        const size_t first = p.find('/');
        if (first == std::string::npos) continue;
        dirs.insert(p.substr(0, first));  // 一级夹
        const size_t second = p.find('/', first + 1);
        if (second != std::string::npos) dirs.insert(p.substr(0, second));  // 二级夹
        // 三级及更深目录不再进树：其文件挂到最近的二级祖先行下（残余子路径）
    }

    // 一级夹 / 二级夹（"父/子"）分桶；均按字母序（std::set）
    std::set<std::string> topFolders;
    std::set<std::string> subFolders;
    for (const auto& d : dirs) {
        const size_t first = d.find('/');
        if (first == std::string::npos) {
            topFolders.insert(d);
        } else if (d.find('/', first + 1) == std::string::npos) {
            subFolders.insert(d);
        }
    }

    std::vector<MdTreeRow> rows;
    rows.reserve(notes.size() + dirs.size() + 1);

    // 文件夹行；返回是否展开（收起则其下的文件行与子夹行都不出）
    const auto addFolderRow = [&](const std::string& dir, const std::string& label,
                                  int depth) {
        MdTreeRow fr;
        fr.isFolder = true;
        fr.name = label;
        fr.relPath = dir;
        fr.depth = depth;
        rows.push_back(fr);
        return collapsed.count(dir) == 0;
    };

    // 文件行：dir 下的笔记（recursive=false → 只收直接子文件，深层文件归子夹；
    // recursive=true → 收整棵子树的文件，残余子路径作显示名，内容不丢）
    const auto addFileRows = [&](const std::string& dir, int depth, bool recursive) {
        const std::string prefix = dir.empty() ? std::string() : dir + "/";
        for (const auto& n : notes) {
            const std::string p = n.relativePath.generic_string();
            if (dir.empty()) {
                // 根目录组：只收没有 '/' 的散文件
                if (p.find('/') != std::string::npos) continue;
            } else if (p.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string rest = p.substr(prefix.size());
            if (!recursive && rest.find('/') != std::string::npos) continue;
            MdTreeRow r;
            r.isFolder = false;
            r.name = rest;
            r.relPath = p;
            r.depth = depth;
            rows.push_back(r);
        }
    };

    // W9：根组标题常显且置顶——笔记目录全是根目录散文件（零子文件夹）时
    // 树也不会退化成无结构的平铺列表，「这是树」始终可感知；空目录时
    // 树里也保留这一行，由 UI 层叠加空态提示
    if (addFolderRow("", "根目录", 0)) addFileRows("", 1, false);
    for (const auto& top : topFolders) {
        if (!addFolderRow(top, top, 0)) continue;  // 收起：整棵一级夹子树隐藏
        addFileRows(top, 1, false);
        for (const auto& sub : subFolders) {
            if (sub.rfind(top + "/", 0) != 0) continue;  // 只要本夹下的二级夹
            // 已收起的一级夹在上面就 continue 了，所以这里不会看到其子夹
            const std::string label = sub.substr(top.size() + 1);
            if (addFolderRow(sub, label, 1)) addFileRows(sub, 2, true);
        }
    }
    return rows;
}

} // namespace module::markdown
