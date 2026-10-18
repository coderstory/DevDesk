// 热榜响应解析（纯函数，无 IO / 无网络 / 无时钟）。
//
// 数据源：
//   微博 side/hotSearch：https://weibo.com/ajax/side/hotSearch（需 Referer: https://weibo.com/）
//   微博 statuses/hot_band：https://weibo.com/ajax/statuses/hot_band
//   百度 PC：https://top.baidu.com/api/board?platform=pc&tab=realtime
//   今日头条：https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc
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

// 备用热榜源标识；parseHotList 按此分发。
enum class HotSource {
    Weibo,      // 微博 side/hotSearch
    WeiboBand,  // 微博 statuses/hot_band
    Baidu,      // 百度 PC realtime
    Toutiao,    // 今日头条 hot-board
};

struct HotItem {
    std::string title;            // 已 trim；保留展示用 `#`
    std::optional<int64_t> hot;   // 热度；缺失 / 非数值 / 负数 / 非整数 = nullopt
    std::string url;              // 微博：搜索链接；百度/头条：原站链接（非 http(s) 时为空）
    int rank = 0;                 // 1-based；按各源排序规则重排后连续
};

// 解析官方热榜响应。成功返回全部有效项（广告、缺 realpos、空标题项被跳过），
// 按 realpos 升序并重排 rank=1..N；空 realtime 数组返回空 vector 成功。
// 顶层不是合法 JSON / ok != 1 / data 或 data.realtime 结构错误 → Result::fail
//（带字段路径的中文错误），不会用「空 vector 成功」掩盖坏响应。
core::Result<std::vector<HotItem>> parseWeiboHot(std::string_view json);

// 解析微博 hot_band 响应（data.band_list）。过滤规则、标题/链接构造与
// parseWeiboHot 相同；忽略独立的 data.hotgov。空 band_list → 成功空 vector。
core::Result<std::vector<HotItem>> parseWeiboBand(std::string_view json);

// 解析百度 PC 实时热榜。在 data.cards 中按 component=="hotList" 选卡（不写死下标），
// 忽略 data.topContent；按 index 升序重排 rank=1..N（缺 index 时保持数组相对顺序）。
// success != true / 找不到 hotList 卡片 / content 不是数组 → 失败；空 content → 成功空 vector。
core::Result<std::vector<HotItem>> parseBaiduHot(std::string_view json);

// 解析今日头条热榜。忽略 fixed_top_data，按 data 数组顺序赋 rank=1..N；
// status != "success" / data 不是数组 → 失败；空数组 → 成功空 vector。
core::Result<std::vector<HotItem>> parseToutiaoHot(std::string_view json);

// 按 source 分发；未知/不支持的热榜源 → 失败。
core::Result<std::vector<HotItem>> parseHotList(HotSource source, std::string_view json);

// 取前 n 项（不重排）：n=0 → 空；n >= size → 全部；否则前 n 项。
std::vector<HotItem> takeTopN(std::vector<HotItem> items, std::size_t n);

} // namespace module::agent
