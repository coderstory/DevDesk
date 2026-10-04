// src/platform/utf8.h
// UTF-8 良构校验与清洗。为什么需要：platform::json 两端对非法 UTF-8 不对称——stringify 带
// YYJSON_WRITE_ALLOW_INVALID_UNICODE（非法字节原样输出），parse 用默认 flag（严格拒绝）。工具回喂文本
// 混进非法字节（md.read 读 GBK/Latin-1 笔记是最常见来源）时，tool_result 先 stringify 成功、再 parse 自检
// 失败 → LOG_FATAL + std::abort ⇒ **整个应用死亡**（M2 起发生在无人值守的 cron run 里）。
// 两道边界共用：工具边界（md.read）isValid → 非法即 isError 回喂；回喂入口 sanitize → 非法字节换 U+FFFD。
// 判定口径 = RFC 3629：拒绝 overlong、代理区（U+D800..U+DFFF）、> U+10FFFF、截断与孤立续字节——与 yyjson 读侧一致。
#pragma once

#include <string>
#include <string_view>

namespace platform::utf8 {

// 整串是否良构 UTF-8（空串 = true）
bool isValid(std::string_view text);

// 非法字节替换为 U+FFFD（EF BF BD）；良构输入原样返回（快速路径）。
std::string sanitize(std::string_view text);

} // namespace platform::utf8
