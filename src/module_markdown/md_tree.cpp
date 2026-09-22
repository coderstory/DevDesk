// src/module_markdown/md_tree.cpp — buildTreeRows 实现（见 md_tree.h 设计说明）
#include "md_tree.h"

#include <algorithm>

namespace module::markdown {

std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& collapsed) {
    // 一级目录集合（std::set 自带字母序）
    std::set<std::string> folders;
    for (const auto& n : notes) {
        const std::string p = n.relativePath.generic_string();
        const auto pos = p.find('/');
        if (pos != std::string::npos) folders.insert(p.substr(0, pos));
    }

    std::vector<MdTreeRow> rows;
    rows.reserve(notes.size() + folders.size() + 1);

    auto addGroup = [&](const std::string& dir, const std::string& label) {
        MdTreeRow fr;
        fr.isFolder = true;
        fr.name = label;
        fr.relPath = dir;
        rows.push_back(fr);
        if (collapsed.count(dir) != 0) return;
        const std::string prefix = dir + "/";
        for (const auto& n : notes) {
            const std::string p = n.relativePath.generic_string();
            if (dir.empty()) {
                // 根目录组：只收没有 '/' 的散文件
                if (p.find('/') != std::string::npos) continue;
            } else if (p.rfind(prefix, 0) != 0) {
                continue;
            }
            MdTreeRow r;
            r.isFolder = false;
            r.name = dir.empty() ? n.relativePath.filename().string()
                                 : p.substr(prefix.size());
            r.relPath = p;
            r.depth = 1;
            rows.push_back(r);
        }
    };

    // W9：根组标题常显且置顶——笔记目录全是根目录散文件（零子文件夹）时
    // 树也不会退化成无结构的平铺列表，「这是树」始终可感知；空目录时
    // 树里也保留这一行，由 UI 层叠加空态提示
    addGroup("", "根目录");
    for (const auto& dir : folders) addGroup(dir, dir);
    return rows;
}

} // namespace module::markdown
