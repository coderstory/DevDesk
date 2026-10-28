// Markdown 笔记只读能力提供方（跨模块注入）：module_markdown 侧注册（生命周期随模块
// onStart/onStop），module_todo 经 ServiceLocator 消费。module_todo 不得 include
// module_markdown（模块隔离铁律），只能依赖本 core 接口。
// 路径契约：relPath = 笔记索引 relativePath（POSIX '/' 分隔），空串 = 未关联。
#pragma once

#include "result.h"

#include <string>
#include <vector>

namespace core {

struct NoteRef {
    std::string relPath;  // 相对路径（POSIX 分隔）
    std::string title;    // 标题（索引标题；空则消费方回落 relPath 显示）
};

class IMarkdownNotesSource {
public:
    virtual ~IMarkdownNotesSource() = default;

    // 全部笔记清单。失败 = 提供方不可用（消费方必须走降级路径）
    virtual Result<std::vector<NoteRef>> listNotes() = 0;

    // 笔记当前是否存在（磁盘层判定）。false = 不存在或不可判定（灰态口径：
    // 不可判定按不存在显示，且不解除关联）
    virtual bool noteExists(const std::string& relPath) = 0;
};

} // namespace core
