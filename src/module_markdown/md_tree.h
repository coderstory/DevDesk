// 左栏目录树的纯数据模型：把笔记列表 + 磁盘目录清单组成「目录行 → 其下文件行 →
// 其下子目录行」的扁平行序列，UI 逐行渲染。纯函数、无副作用、可单测。
// 分组规则：根组「根目录」常显且最前；组内顺序 = 目录行 → 直接文件行（mtime 倒序）
// → 各级子目录行（字母序，各自递归展开）。目录层级不设上限，各级都有自己的行
//（可点选/收起），其直接文件挂在它下面。
// 缩进（UI 按 depth 算）：根组行 = 0，顶层夹与其直接文件 = 1，再下一层 = 2 ……
//（目录行 depth = 相对路径 '/' 个数 + 1，根组行除外；文件行 = 所属目录 depth + 1）。
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
    int depth = 0;        // 展示层级：根组与顶层夹 = 0，其下文件与子夹 = 1，
                          // 更深逐级 +1（UI 按它算缩进）
};

// collapsed = 已收起的目录（relPath 为键，各层同表；命中的目录其子树不出行）。
// extraFolders = 磁盘枚举的全层目录（含空目录）。
// notes = 笔记元数据（顺序即组内文件行顺序）。
std::vector<MdTreeRow> buildTreeRows(const std::vector<NoteMeta>& notes,
                                     const std::set<std::string>& extraFolders,
                                     const std::set<std::string>& collapsed);

} // namespace module::markdown
