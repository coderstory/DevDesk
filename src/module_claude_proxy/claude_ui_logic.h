// src/module_claude_proxy/claude_ui_logic.h
// R4（P-B1）纯逻辑单元：claude_module.cpp 匿名命名空间里「不碰 Ui、不碰 DB」的
// 4 个自由函数 + 调用记录弹窗常量，提升到本头/实现后即可无头单测
//（spec 2026-09-27-god-class-refactor-design §3.2 / §5.2 第 4 条：
// 「能进 logic 头的就进」）。
//
// 零行为变化：函数体与常量取值逐字搬运（含注释），只改归属与链接性
//（匿名 ns 的 internal linkage → 本模块内可见）；调用点全部按名重定位，
// 不动任何取值。
#pragma once

#include "claude_service.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace module::claude {

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms);

// 按字节截断 UTF-8 串（审计 [11]：错误行尾附 error 摘要，过长会挤爆行宽）。
// 截点回退到字符边界，不把多字节字符切成半个（乱码方块）
std::string truncateUtf8(const std::string& s, size_t maxBytes);

// 模型映射摘要（§6.2 详情面板一行式）：「默认 <model> · 槽位已配 N/M」
// 与分母 M 都取自槽位表（审计 CPX-08：不再手写槽位清单与硬编码 /6）
std::string modelsSummary(const Profile& p);

// 「导入当前配置」名称建议：BASE_URL host（剥 scheme/路径/端口），
// 提取不到（无 BASE_URL）→「导入 <日期>」；用户可改
std::string suggestImportName(const std::string& baseUrl);

// ---- 调用记录弹窗常量（审计 [11]）----
//
// 调用记录弹窗状态码过滤（审计 [11]，spec §8.3 第 6 条「支持过滤、搜索」）：
// 下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
inline constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
inline constexpr const char* kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
inline constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数（审计 [11]；总设计只承诺「过滤、搜索」，条数由 UI 自定）
inline constexpr int kCallsPageSize = 20;
// 搜索框防抖窗口（审计 CPX-06）：每次按键顺延窗口，停手满一个窗口才派发一次
// 后台查询（窗口内多次按键合并成一次）；250ms = 常见搜索防抖档
inline constexpr int kCallsSearchDebounceMs = 250;

} // namespace module::claude
