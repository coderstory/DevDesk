// 整文件文本读取（core 层，同步、无异常）
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace core {

// 读整个文件为二进制安全字符串。不存在/打不开 → 空串；readFailed 非空时区分
// 「读取失败」与「空文件」（IO 中断也置 true）。需要错误详情的调用方自行用
// Result 封装，不复用本函数
inline std::string readTextFile(const std::filesystem::path& p,
                                bool* readFailed = nullptr) {
    if (readFailed) *readFailed = false;
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (readFailed) *readFailed = true;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (readFailed && f.bad()) *readFailed = true;
    return ss.str();
}

}  // namespace core
