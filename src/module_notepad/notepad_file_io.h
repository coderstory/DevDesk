// notepad_file_io.h — 记事本读/写/mtime 拦截/保存状态机单点（need:273 批 1）。
// 读走 core::readTextFile（fs::path 宽路径，Windows 中文路径免疫）；写走
// core::atomicWriteFile（临时文件+原子替换）。不复用 module_markdown
// readFileUtf8（模块隔离，check-module-isolation 硬守卫）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "core/result.h"
#include "notepad_encoding.h"

namespace module::notepad {

// 大文件阈值（技术方案卡 §八；常量收敛便于 QA 调参）。1MB 关高亮+关 wrap
//（可编辑，F-16 一级）；10MB 只读（二级）。POC 依据：
// docs/research/2026-11-16-word-wrap-poc.md §1（shaping ≈0.6ms/KB）。
inline constexpr size_t kNotepadLargeFileBytes = 1024 * 1024;
inline constexpr size_t kNotepadHugeFileBytes = 10 * 1024 * 1024;

// 载入结果：rawBytes=原始字节（干净直通保证）；text=剥 BOM+LF 归一后的编辑初值。
struct NotepadLoadedFile {
    std::string rawBytes;
    std::string text;
    std::filesystem::file_time_type mtime{};
    bool hasMtime = false;
    NotepadEncodingProbe enc{};
    NotepadEol eol = NotepadEol::Lf;
    bool mixedEol = false;
    bool hasBom = false;
    size_t byteCount = 0;
};

// 读文件 + 全流程探测（编码/行尾/大小档）。不存在 → fail(FileNotFound)；
// 打开/读取失败 → fail(FileReadFailed)。无异常纪律（Result 化包装）。
core::Result<NotepadLoadedFile> notepadLoadFile(const std::string& path);

// 编辑缓冲按编码档（BOM 回写）+ 行尾档（全文回写）重编码（脏缓冲保存路径）。
std::string notepadEncodeBuffer(const std::string& textLf, bool hasBom,
                                NotepadEol eol);

// 原子写盘（编码后的最终字节）。父目录存在性由调用方保证（保存目标=已打开文件）。
core::Result<void> notepadWriteFile(const std::string& path,
                                    const std::string& content);

}  // namespace module::notepad
