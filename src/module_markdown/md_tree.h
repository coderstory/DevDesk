// src/module_markdown/md_tree.h
// 左栏目录树的纯数据模型（W8 建，W10 升两级，2026-09-29 放开层级）：把笔记列表 +
// 磁盘目录清单组成「目录行 → 其下文件行 → 其下子目录行」的扁平行序列，UI 只负责
// 逐行渲染。纯函数、无副作用、可单测。
//
// 分组规则（2026-09-29 起**任意层级**，不再限两级）：
//   - 根组「根目录」常显且排在最前（W9：散文件目录也要能感知「这是树」；
//     空目录时保留该行，UI 层叠加空态提示）
//   - 组内顺序：目录行 → 该目录的**直接**文件行（保持 mtime 倒序，与 listAll
//     一致）→ 各级子目录行（按字母序，各自递归展开）——与顶层「根组 → 顶层夹」
//     同序
//   - 目录层级不设上限：每一级目录都有自己的行（可点选、可收起），其直接文件
//     挂在它下面。旧的「三级+ 目录不进树、其文件挂到二级祖先行下显示残余子路径」
//     做法已删除——那套做法的代价是三级目录无法被选中（没有行位），而「选中目录」
//     正是新建笔记/文件夹的父目录来源（见 markdown_module 的 selectedDir_）
//
// 层级与缩进（UI 按 depth 算缩进；2026-09-30 起根组行是整棵树的**根**）：
//   根组行 = 0，顶层夹与其直接文件 = 1，再下一层 = 2 ……（目录行 depth =
//   相对路径的 '/' 个数 + 1，根组行除外；文件行 depth = 所属目录 depth + 1）。
//   旧口径「顶层夹 = 0 = 与根组行同级」已废——那会把根行选中时新建的顶层夹
//   渲染成根行的兄弟（用户读成「落在笔记根之外」）
#pragma once

#include "markdown_service.h"

#include <set>
#include <string>
#include <vector>

namespace module::markdown {

struct MdTreeRow {
    bool isFolder = false;
    std::string name;     // 显示名（文件 = 文件名（含 .md）；文件夹 = 目录名段）
    std::string relPath;  // 文件 = POSIX 相对路径；文件夹 = POSIX 相对目录路径
                          //（顶层夹 "工作" / 子夹 "工作/周会" / 根组 ""）
    int depth = 0;        // 展示层级：根组与顶层夹 = 0，其下文件与子夹 = 1，
                          // 更深逐级 +1（UI 按它算缩进）
};

// collapsed = 已收起的目录集合（relPath 为键，各层目录同表；内存态 +
//   视图状态文件持久化）；命中某个目录 → 该目录之下整棵子树不出行
// extraFolders = 磁盘枚举的**全层**目录（POSIX 相对路径，见
//   MarkdownService::listAllFolders；含没有任何 .md 的空目录，让「新建
//   文件夹」后的空文件夹也能出现在树里）
// notes = 笔记元数据（顺序即组内文件行顺序，调用方按 mtime 倒序传入）
std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed);

} // namespace module::markdown
