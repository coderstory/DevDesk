// src/module_markdown/md_tree.h
// 左栏目录树的纯数据模型（W8 建，W10 升两级）：把笔记列表 + 磁盘目录清单
// 组成「顶层夹 → 子夹 → 文件」的扁平行序列，UI 只负责逐行渲染。纯函数、
// 无副作用、可单测。
//
// 分组规则（W10 两级目录）：
//   - 根组「根目录」常显且排在最前（W9：散文件目录也要能感知「这是树」；
//     空目录时保留该行，UI 层叠加空态提示）
//   - 顶层 = notes 目录的一级子目录（按字母序）
//   - 子夹 = 二级子目录（"父/子"；含没有任何 .md 的**空子夹**，W10 新增）
//   - 每组内：文件行在前（保持 mtime 倒序，与 listAll 一致），子夹行在后
//     （按字母序）——与顶层「根目录组 → 顶层夹」的排列同序
//   - 更深层（三级+）目录不再出文件夹行：其下的文件挂到最近的二级祖先行，
//     名字显示残余子路径（a/b/c/x.md → 子夹 a/b 下的 "c/x.md"，内容不丢）
#pragma once

#include "markdown_service.h"

#include <set>
#include <string>
#include <vector>

namespace module::markdown {

struct MdTreeRow {
    bool isFolder = false;
    std::string name;     // 显示名（文件 = 文件名或残余子路径；文件夹 = 目录名）
    std::string relPath;  // 文件 = POSIX 相对路径；文件夹 = POSIX 相对目录路径
                          //（顶层夹 "工作" / 子夹 "工作/周会" / 根组 ""）
    int depth = 0;        // 展示层级：根组与顶层夹 = 0，其下文件与子夹 = 1，
                          // 子夹下的文件 = 2（UI 按它算缩进）
};

// collapsed = 已收起的目录集合（relPath 为键，顶层夹/子夹同表；内存态 +
//   视图状态文件持久化）
// extraFolders = 磁盘枚举的**全层**目录（POSIX 相对路径，见
//   MarkdownService::listAllFolders；含没有任何 .md 的空目录，让「新建
//   文件夹」后的空文件夹也能出现在树里）
std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed);

} // namespace module::markdown
