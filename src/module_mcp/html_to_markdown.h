// HTML → Markdown 转换（spec §7）：自研容错扫描器，零第三方依赖。
// 契约：输入超 2 MiB 先按字节截断；非法/未闭合标签按文本处理、绝不抛异常；
// JS 渲染页面提取不到可见文本时返回空串（提示文案由工具层负责）。
// 正文主干提取（spec §7 同节）：extractMainContent 默认 true，剔除语义样板
// （header/footer/nav 等）与高链接密度短块；false 时规则全关断，输出与引入
// 该参数前的基线逐字节一致（差分保底，专项测试锁死，不得回归）。
#pragma once

#include <string>

namespace module::mcp {

std::string htmlToMarkdown(const std::string& html,
                           bool extractMainContent = true);

} // namespace module::mcp
