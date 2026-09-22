// src/module_markdown/md_tree.cpp — buildTreeRows 实现（见 md_tree.h 设计说明）
#include "md_tree.h"

#include <algorithm>

namespace module::markdown {

std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& collapsed) {
    // 一级目录集合（std::set 自带字母序）
    std::set<std::string> folders;
    bool hasRootFiles = false;
    for (const auto& n : notes) {
        const std::string p = n.relativePath.generic_string();
        const auto pos = p.find('/');
        if (pos == std::string::npos) hasRootFiles = true;
        else folders.insert(p.substr(0, pos));
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

    for (const auto& dir : folders) addGroup(dir, dir);
    if (hasRootFiles) addGroup("", "根目录");
    return rows;
}

} // namespace module::markdown
