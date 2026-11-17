#pragma once
// driver_dump.h — id dump（债-18 编排件）：--dump-ids CLI 模式（启动→compose→打印→
// 退出）+ 场景步骤 {"type":"dump","path":...}（中途落盘）共用这里的收集与格式化。
// 格式：id<TAB>x,y w×h<TAB>flags（flags = interactive/clip/disabled/opacity0/zIndex）。
// 滚动态（bug310）：滚动偏移不落在布局 frame 上——EUI 以 render transform
//（scrollTransformForElement）平移内容/滑块，dump 的 x,y 永远不动。故对带滚动
// 状态的元素追加数值标志，让「滚轮是否真的生效」在 dump 里可读：
//   scroll=<offset>/<max>      滚动根自身（scrollStateId）
//   scrollSrc=<offset>/<max>   内容层（scrollContentSourceId 指向的滚动根偏移）
//   scrollBar=<offset>/<max>   滑块（scrollThumbSourceId 指向的滚动根偏移）
#include "probes/probe_harness.h"

#include <fstream>
#include <string>
#include <vector>

namespace ui_drive {

struct DumpRow {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    std::string flags;
};

// 输出格式化纯函数（无头单测钉格式）
inline std::string formatDumpRow(const DumpRow& row) {
    std::string xy = std::to_string(static_cast<int>(row.x)) + "," +
                     std::to_string(static_cast<int>(row.y));
    std::string wh = std::to_string(static_cast<int>(row.w)) + "x" +
                     std::to_string(static_cast<int>(row.h));
    return row.id + "\t" + xy + " " + wh + "\t" + row.flags;
}

inline std::vector<DumpRow> collectDumpRows(core::dsl::Ui& ui) {
    std::vector<DumpRow> rows;
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<const core::dsl::Element*> leaves;
    for (const auto& root : ui.roots()) {
        probe::detail::walkElements(*root, "", parentOf, byId, leaves);
    }
    rows.reserve(byId.size());
    for (const auto& [id, element] : byId) {
        DumpRow row;
        row.id = id;
        row.x = element->frame.x;
        row.y = element->frame.y;
        row.w = element->frame.width;
        row.h = element->frame.height;
        if (element->interactive) row.flags += "interactive ";
        if (element->clip) row.flags += "clip ";
        if (element->disabled) row.flags += "disabled ";
        if (element->opacity == 0.0f) row.flags += "opacity0 ";
        if (element->zIndex != 0) row.flags += "zIndex=" + std::to_string(element->zIndex) + " ";
        const auto scrollFlag = [&](const char* name, const std::string& sourceId) {
            if (sourceId.empty()) return;
            const auto src = byId.find(sourceId);
            if (src == byId.end()) return;
            row.flags += std::string(name) + "=" +
                         std::to_string(static_cast<int>(src->second->scrollOffset)) + "/" +
                         std::to_string(static_cast<int>(src->second->scrollMaxOffset)) + " ";
        };
        scrollFlag("scroll", element->scrollStateId);
        scrollFlag("scrollSrc", element->scrollContentSourceId);
        scrollFlag("scrollBar", element->scrollThumbSourceId);
        while (!row.flags.empty() && row.flags.back() == ' ') row.flags.pop_back();
        rows.push_back(std::move(row));
    }
    // id 排序 = dump 稳定可 diff（树序跨 compose 重建不稳定）
    std::sort(rows.begin(), rows.end(), [](const DumpRow& a, const DumpRow& b) {
        return a.id < b.id;
    });
    return rows;
}

// 落盘（失败 stderr 诊断 + false，不静默——对齐 probe 截图惯例）
inline bool writeDumpFile(const std::vector<DumpRow>& rows, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "ui_driver: dump 落盘失败（打不开）: " << path << "\n";
        return false;
    }
    for (const DumpRow& row : rows) {
        out << formatDumpRow(row) << "\n";
    }
    return out.good();
}

} // namespace ui_drive
