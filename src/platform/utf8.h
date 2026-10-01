// src/platform/utf8.h
// UTF-8 良构校验与清洗（M2 静态审计 F1，high）。
//
// 为什么需要：platform::json 的两端对非法 UTF-8 不对称——
//   · stringify 带 YYJSON_WRITE_ALLOW_INVALID_UNICODE（json.cpp）→ 非法字节**原样输出**；
//   · parse 用默认 flag（json.cpp）→ yyjson 读侧**严格拒绝**非法 UTF-8。
// 工具回喂文本里混进非法字节（md.read 读 GBK/Latin-1 笔记是最常见来源；拼进
// 错误消息的文件路径同理）时，AgentLoop 的 tool_result 块先 stringify 成功、
// 再 parse 自检失败 → LOG_FATAL + std::abort ⇒ **整个应用死亡**，且 M2 起会
// 发生在无人值守的 cron run 里。
//
// 本头提供两个纯函数，供两道边界共用：
//   · 工具边界（md.read）：isValid → 非法即 isError 回喂，不把乱码喂给模型；
//   · 回喂入口（AgentLoop::toolResultText）：sanitize → 非法字节换 U+FFFD，
//     保证「交给 JSON 拼装层的文本恒良构」，abort 路径从工具输出不可达。
//
// 判定口径 = RFC 3629：拒绝 overlong（C0/C1 前导、E0 9F 等）、拒绝代理区
//（ED A0 80..ED BF BF，即 UTF-8 编码的 U+D800..U+DFFF）、拒绝 > U+10FFFF
//（F4 90 起）、拒绝截断序列与孤立续字节——与 yyjson 读侧的严格口径一致。
#pragma once

#include <string>
#include <string_view>

namespace platform::utf8 {

// 整串是否良构 UTF-8（空串 = true）
bool isValid(std::string_view text);

// 非法字节替换为 U+FFFD（EF BF BD）；良构输入原样返回（先整体校验的快速路径，
// 不分配新内容时零拷贝语义不保证，但内容逐字相同）
std::string sanitize(std::string_view text);

} // namespace platform::utf8
