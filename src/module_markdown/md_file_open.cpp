// src/module_markdown/md_file_open.cpp
#include "md_file_open.h"

#include "core/logging.h"

#include <system_error>

namespace fs = std::filesystem;

namespace module::markdown {

core::Result<std::string> captureFileIntoNotesDir(const fs::path& notesDir,
                                                  const std::string& absPath) {
    std::error_code ec;
    const fs::path src = fs::path(absPath);
    if (absPath.empty() || !fs::exists(src, ec) || ec ||
        !fs::is_regular_file(src, ec)) {
        return core::Result<std::string>::fail(core::ErrorCode::FileNotFound,
            "文件不存在或不是普通文件: " + absPath);
    }

    fs::create_directories(notesDir, ec);  // 已存在 = 无操作不报错
    if (ec) {
        return core::Result<std::string>::fail(core::ErrorCode::FileWriteFailed,
            "无法创建笔记目录: " + ec.message());
    }

    // 两侧规范化消除 ./.. 与链接写法差异（weakly_canonical 不要求文件存在）。注意 relative
    // 大小写敏感：个别段大小写不同会判到目录外，走「复制捕获」兜底，行为安全。
    fs::path srcC = src;
    fs::path baseC = notesDir;
    {
        std::error_code ec1;
        const fs::path t = fs::weakly_canonical(src, ec1);
        if (!ec1) srcC = t;
    }
    {
        std::error_code ec2;
        const fs::path t = fs::weakly_canonical(notesDir, ec2);
        if (!ec2) baseC = t;
    }

    // 1) 已在笔记目录内 → 相对化（目录外路径 relative 生成 ../.. 形式，用 ".." 前缀识别逃逸）
    const fs::path rel = fs::relative(srcC, baseC, ec);
    if (!ec && !rel.empty()) {
        const std::string relPosix = rel.generic_string();
        if (relPosix.rfind("..", 0) != 0) {
            return core::Result<std::string>::ok(relPosix);
        }
    }

    // 2) 目录外文件 → 复制进笔记目录根，强制 .md（rescan/pollChanged/树只认 .md，否则永不
    //    进索引）；名字 = 原文件名主干 + .md，撞名追加「 2」「 3」。
    std::string stem = srcC.stem().string();
    if (stem.empty()) stem = "导入文件";
    fs::path dst = notesDir / fs::path(stem + ".md");
    int dedupe = 2;
    while (true) {
        std::error_code ec3;
        if (!fs::exists(dst, ec3)) break;
        if (dedupe > 999) {
            return core::Result<std::string>::fail(core::ErrorCode::AlreadyExists,
                "笔记目录同名文件过多: " + stem);
        }
        dst = notesDir / fs::path(stem + " " + std::to_string(dedupe++) + ".md");
    }
    std::error_code cec;
    fs::copy_file(srcC, dst, fs::copy_options::none, cec);
    if (cec) {
        return core::Result<std::string>::fail(core::ErrorCode::FileReadFailed,
            "复制进笔记目录失败 " + absPath + ": " + cec.message());
    }
    const std::string newRel = dst.filename().generic_string();
    LOG_INFO("外部文件已捕获进笔记目录: " + absPath + " → " + newRel);
    return core::Result<std::string>::ok(newRel);
}

} // namespace module::markdown
