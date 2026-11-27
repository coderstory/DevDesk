// notepad_sql_format.h — SQL 格式化纯函数引擎（need:380 最小范围：关键字
// 规范化 + 缩进）。自研轻量（禁引第三方依赖，简报已证事实）；与
// ui_kit/text_find 同款纪律：纯函数、无 UI 依赖，供 compose 提交链与直测消费。
#pragma once

#include <string>
#include <string_view>

namespace module::notepad {

// SQL 文本格式化：关键字大写规范化 + 子句断行 + 4 空格缩进（括深度）。
//   - 注释（-- 行注释 / /* */ 块注释）与字符串字面量（'…' 含 '' 转义、
//     "…"、`…`）原样保留、不参与重排（仅整体搬运）；
//   - 大小写不敏感识别关键字（含常见类型/函数词），识别命中 → 输出大写；
//   - 主子句（SELECT/FROM/WHERE/GROUP/ORDER/HAVING/LIMIT/OFFSET/UNION/
//     INSERT/UPDATE/DELETE/SET/VALUES/WITH/JOIN 族/ON）顶层断行；
//     顶层 AND/OR 断行 + 一档缩进；
//   - 顶层 SELECT 列表逗号断行 + 一档缩进，其余逗号保持行内；
//   - 词间单空格，标点紧贴（括号内外无冗余空格），行注释强制收行。
// 空串 / 无 token → 原样返回。纯函数：同输入同输出，无副作用。
std::string formatSqlText(std::string_view sql);

// SQL 关键字命中（大小写不敏感；格式化与潜在复用方共用单一表源）。
bool isSqlKeyword(std::string_view word);

}  // namespace module::notepad
