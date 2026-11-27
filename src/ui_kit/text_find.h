// notepad_find.h — 查找/替换纯函数引擎（need263 批 2，tech 卡 §四）。
// 字节域：偏移与 InputState 的 cursor/selectionStart/selectionEnd（全文档
// UTF-8 字节偏移）同域，零换算。无 UI 依赖，无头可测；不抛异常。
// 大小写折叠 = 仅 ASCII（禁 locale）；词字符 = [A-Za-z0-9_ ∪ >=0x80 字节]
//（CJK 字节视作词字符，整字符参与全词边界判定）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <eui/types.h>  // eui::TextRun（= core::TextRun 值类型桥接例外）

namespace ui_kit::text_find {

// 环绕跳转的「无命中」哨兵（hits 空表 / 无满足者时返回）。
inline constexpr size_t kFindNoHit = static_cast<size_t>(-1);

struct FindOptions {
    bool caseSensitive = false;
    bool wholeWord = false;
};

// 命中区间：字节闭开 [start, end)。同一 findAll 结果内互不重叠、按 start 升序。
struct FindHit {
    size_t start = 0;
    size_t end = 0;
};

// 全量命中（非重叠贪心扫描；空 query → 空）。
std::vector<FindHit> findAll(const std::string& text, const std::string& query,
                             const FindOptions& opts);

// 环绕跳转（N-3 固定）：返回命中下标。
//   nextFrom = 首个 start > fromByte 的命中，无则回绕 hits[0]；
//   prevFrom = 最后一个 start < fromByte 的命中，无则回绕 hits.back()。
// 返回下标而非 FindHit 值：窗口层 session.cur 需要下标跟踪当前命中，
// 且 SIZE_MAX 天然表达「无命中」（值返回无法无歧义表达）。
size_t nextFrom(const std::vector<FindHit>& hits, size_t fromByte);
size_t prevFrom(const std::vector<FindHit>& hits, size_t fromByte);

// 单个替换结果。cursor/sel 按 F-07 落点锁（见 replaceOneAt）。
struct ReplaceOneResult {
    std::string text;
    size_t cursor = 0;
    size_t selStart = 0;
    size_t selEnd = 0;
};

// 单个替换 + F-07 落点锁（PRD 锁定项，doctest 全边界断言）：
// 光标 = 替换文本之后（start >= 替换末字节）下一个命中的 start（含环绕回
// hits[0]）；无下一命中（含剩余命中集为空）→ 光标/选区 = 替换文本末尾
//（零宽选区）。选区 = 该下一命中区间。
ReplaceOneResult replaceOneAt(const std::string& text, const FindHit& hit,
                              const std::string& repl,
                              const std::string& query, const FindOptions& opts);

struct ReplaceAllResult {
    std::string text;
    size_t count = 0;
};

// 全部替换：一次拼接原文本全部命中（替换文本内若含 query 不重扫——防自举
// 死循环，官方编辑器同口径）；count = 命中数（0 → 原文返回）。
ReplaceAllResult replaceAll(const std::string& text, const std::string& query,
                            const std::string& repl, const FindOptions& opts);

// 命中层 ∪ 语法层合并（tech 卡 §四）：在命中边界切分语法段、命中段覆盖为
// 单一合成色（styled-runs 预合成由调用方给色 + need377 bold 位）。
//   base        语法层 runs（全文档字节坐标，mdKeywordHighlight 平移后产物）
//   hits        命中层（全文档字节坐标）
//   currentHit  当前命中（nullptr = 无当前命中，全部用 hitColor）
//   lineText / lineStartByte  本行显示文本与行首字节（限定合并域
//               [lineStartByte, lineStartByte+lineText.size())，跨行命中裁剪）
//   hitBold     命中段是否加粗（need377：TextRun.bold faux-bold 渲染位）
// 返回 runs：全文档坐标、行内裁剪、按 start 升序、相邻同样式合并、互不重叠。
std::vector<eui::TextRun> mergeRuns(const std::vector<eui::TextRun>& base,
                                    const std::vector<FindHit>& hits,
                                    const FindHit* currentHit,
                                    const std::string& lineText,
                                    size_t lineStartByte, eui::Color hitColor,
                                    eui::Color currentHitColor,
                                    bool hitBold = false);

}  // namespace ui_kit::text_find
