// 微博热榜响应解析（纯函数，无 IO / 无网络 / 无时钟）。
//
// 数据源：https://weibo.com/ajax/side/hotSearch（需 Referer: https://weibo.com/）。
// 本文件只负责把响应 JSON 变成 HotItem 列表；HTTP、源链降级、缓存/限频与 hot.list
// 工具注册由后续任务在工具层接入。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace module::agent {

struct HotItem {
    std::string title;            // 已 trim；保留展示用 `#`
    std::optional<int64_t> hot;   // `num` 缺失 / 非 number / 不可转换 = nullopt
    std::string url;              // https://s.weibo.com/weibo?q=<percent-encoded>
    int rank = 0;                 // 1-based；按 realpos 重排后连续
};

// 解析官方热榜响应。成功返回全部有效项（广告、缺 realpos、空标题项被跳过），
// 按 realpos 升序并重排 rank=1..N；空 realtime 数组返回空 vector 成功。
// 顶层不是合法 JSON / ok != 1 / data 或 data.realtime 结构错误 → Result::fail
//（带字段路径的中文错误），不会用「空 vector 成功」掩盖坏响应。
core::Result<std::vector<HotItem>> parseWeiboHot(std::string_view json);

// 取前 n 项（不重排）：n=0 → 空；n >= size → 全部；否则前 n 项。
std::vector<HotItem> takeTopN(std::vector<HotItem> items, std::size_t n);

} // namespace module::agent
