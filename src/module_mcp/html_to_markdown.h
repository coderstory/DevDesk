// HTML → Markdown 转换（spec §7）：自研容错扫描器，零第三方依赖。
// 契约：输入超 2 MiB 先按字节截断；非法/未闭合标签按文本处理、绝不抛异常；
// JS 渲染页面提取不到可见文本时返回空串（提示文案由工具层负责）。
#pragma once

#include <string>

namespace module::mcp {

std::string htmlToMarkdown(const std::string& html);

} // namespace module::mcp
