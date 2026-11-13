// src/module_git/git_content_model.h
// 右栏内容视图数据面（need:10 批次三；技术方案 §1 git_content_model + UI 方案 A §5.5）：
// worktree 读文件 + 四态判据（空 / 二进制 / 超大 >1 MiB / 编码不支持）+ 读取失败。
// 纯函数、零 UI / git 依赖：worker 线程调用，doctest 直打临时文件。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace module::git {

enum class ContentState { Ok, Empty, Binary, Oversize, BadEncoding, ReadError };

struct FileContent {
    ContentState state = ContentState::Ok;
    std::vector<std::string> lines;  // state==Ok 有效：'\n' 切分、行尾 '\r' 剥除；
                                     // 文件以换行结尾不产生末尾空行（行号 = 1 起）
    std::string errorMessage;        // state==ReadError 时的人类可读原因
    size_t maxLineLen = 0;           // state==Ok 有效：最长行码点数（F-07⑥ 横滚内容宽；
                                     // bug#21：字节→码点，UTF-8 CJK 不再虚胖 3 倍）

    // 四态判据（F-11）：空(0 字节)/二进制(首 8KiB 含 NUL)/超大(>1 MiB)/非 UTF-8；
    // 打不开（不存在/无权限）→ ReadError。占位态不解读文件体（GitAhead #161 反例
    // 对策：超大/二进制不进行内容扫描）。
    static FileContent readFromDisk(const std::filesystem::path& absPath);
};

} // namespace module::git
