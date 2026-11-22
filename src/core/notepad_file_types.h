// 记事本可接管扩展名清单——单一真源（core 层零依赖）。need:273 批 1。
// 消费方三类：app_assembly（OS 漏斗 / 托盘关联启动 / 第二实例唤醒判据）、
// module_notepad（语言映射兜底 / 窗内打开对话框过滤器）、module_settings
// （F-18 逐扩展注册，后续批次）。改清单只改这一处，所有判据同步生效。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace core::notepad {

// 关联清单：文档 / 代码 / 配置三档全选（老板 2026-11-16 一轮裁决）。
// 顺序即设置页展示顺序；按枚举全量收齐，与裁决逐项对账。
inline const std::vector<std::string_view>& notepadExtensions() {
    static const std::vector<std::string_view> kExtensions = {
        ".md",     ".markdown", ".json", ".txt", ".yaml", ".yml", ".toml",
        ".log",    ".ini",      ".js",   ".mjs", ".cjs",  ".jsx", ".ts",
        ".tsx",    ".css",      ".html", ".htm", ".xml",  ".sql", ".py",
        ".cpp",    ".cc",       ".cxx",  ".h",   ".hpp",
    };
    return kExtensions;
}

// ASCII 折叠小写：只折 'A'-'Z'，不依赖 locale（tolower 受 locale 影响，
// app_assembly dispatchPageForFile 既有口径）。OS 关联送达扩展名大小写不定
//（README.MD / NOTES.Markdown），判定必须与 OS 行为对齐。
inline char notepadAsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 后缀匹配公共实现（大小写不敏感；APFS/NTFS 默认不敏感卷上 OS 送达同 casing，
// 残余的异 casing 送达同样命中）。空后缀不命中。
inline bool matchesAnyExtension(
    const std::string& path, const std::vector<std::string_view>& extensions) {
    for (const std::string_view ext : extensions) {
        const size_t n = ext.size();
        if (path.size() < n) continue;
        const size_t offset = path.size() - n;
        bool hit = true;
        for (size_t i = 0; i < n; ++i) {
            if (notepadAsciiLower(path[offset + i]) !=
                notepadAsciiLower(static_cast<char>(ext[i]))) {
                hit = false;
                break;
            }
        }
        if (hit) return true;
    }
    return false;
}

// 路径是否为记事本可接管的扩展名。空后缀不命中。
inline bool isNotepadExtension(const std::string& path) {
    return matchesAnyExtension(path, notepadExtensions());
}

// 笔记模块（markdown）承接扩展名（need:380 wave5-d C-3）：OS 双击 .md 送达
// 改走主窗笔记页（Router "open:" 负载），不再进独立记事本窗。两元素仍保留在
// notepadExtensions() 关联注册清单内——OS 关联登记面不变，双击才仍进 DevDesk，
// 只是进程内分发目标改向。消费方 = app_assembly 漏斗（分发 / 唤醒 / 托盘裁决）。
inline const std::vector<std::string_view>& markdownExtensions() {
    static const std::vector<std::string_view> kMdExtensions = {".md", ".markdown"};
    return kMdExtensions;
}

inline bool isMarkdownExtension(const std::string& path) {
    return matchesAnyExtension(path, markdownExtensions());
}

}  // namespace core::notepad
