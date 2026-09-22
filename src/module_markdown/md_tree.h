// src/module_markdown/md_tree.h
// 左栏二级目录树的纯数据模型（W8）：把笔记列表组成「顶层文件夹 → 二级文件」
// 的扁平行序列，UI 只负责逐行渲染。纯函数、无副作用、可单测。
//
// 分组规则（用户确认方案 + W9 修订）：
//   - 根组「根目录」常显且排在最前（W9：散文件目录也要能感知「这是树」；
//     空目录时保留该行，UI 层叠加空态提示）
//   - 顶层 = notes 目录的一级子目录（按字母序）
//   - 二级 = 该目录下的文件（保持 mtime 倒序，与 listAll 一致）
//   - 更深层文件（a/b/c.md）挂在一级目录 a 下、名字显示剩余子路径
//     （ponytail：二级树不递归展开，层级 >2 时显示 "b/c.md" 而非隐藏）
#pragma once

#include "markdown_service.h"

#include <set>
#include <string>
#include <vector>

namespace module::markdown {

struct MdTreeRow {
    bool isFolder = false;
    std::string name;     // 显示名（文件 = 文件名 / 剩余子路径；文件夹 = 目录名）
    std::string relPath;  // 文件 = POSIX 相对路径；文件夹 = 一级目录名（根目录组为 ""）
    int depth = 0;        // 0 = 顶层文件夹，1 = 文件
};

// collapsed = 已收起的一级目录名集合（内存态，不持久化）
// extraFolders = 磁盘枚举的一级目录（W9：含没有任何 .md 的空目录，
//   让「新建文件夹」后的空文件夹也能出现在树里）
std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed);

} // namespace module::markdown
