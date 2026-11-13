// src/module_git/git_search_model.h
// 全局搜索匹配模型（need:14 F-08；技术方案 §5/§6.1）：纯子串不分词 +
// ASCII 大小写折叠 + 单文件 5 条命中上限 + 码点边界 preview 截断。
// 纯函数、零 UI / git 依赖：worker 线程调用，doctest 直打（先例 git_content_model）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace module::git {

struct SearchHit {
    std::string relPath;   // 相对 workdir POSIX 路径
    int line = 0;          // 1 起
    std::string preview;   // 截断预览（码点边界 + 省略号）
    size_t matchStart = 0; // preview 内 match 段字节偏移（高亮 runs 用）
    size_t matchLen = 0;   // match 段字节长
    bool moreInFile = false;  // 此文件还有更多命中（5 条上限截断标记）
};

struct SearchQuery {
    std::string needle;                          // 已 trim（提交前控制器裁）
    std::vector<std::string> skipUntrackedDirs;  // 尾 '/' 形态（statusByPath_ 快照，
                                                 // B0 核对：untracked 目录条目 kind=
                                                 // Untracked、路径带尾 '/'，libgit2
                                                 // status 不递归 untracked 的键形态）
};

// ASCII 大小写折叠：A-Z 逐字节映射 a-z，非 ASCII 原样（CJK 无大小写；拉丁扩展
// Ä/ä 不互匹配——方案 §5 边界声明）。字节 1:1 变换 → 折叠串上的字节偏移与原串一致。
std::string asciiFold(const std::string& s);

// 纯子串匹配：ASCII 折叠后逐行查找；每行取首个命中；单文件 5 条上限，第 6 个
// 命中行出现时第 5 条 moreInFile=true 收口。needle 为空 → 空结果。
std::vector<SearchHit> matchLines(const std::vector<std::string>& lines,
                                  const std::string& needle);

// 码点边界截断：以 [matchStart, matchStart+matchLen) 为中心，前后各 radius 个
// 码点成窗（窗界对齐 UTF-8 序列起点，不切半个多字节字符）；左/右截断补省略号
// 「…」（U+2026）。outMatchStart/outMatchLen（可空）= match 段在 preview 内的
// 字节偏移/长度。
std::string truncateAtCodepoint(const std::string& s, size_t matchStart,
                                size_t matchLen, size_t radius,
                                size_t* outMatchStart, size_t* outMatchLen);

} // namespace module::git
