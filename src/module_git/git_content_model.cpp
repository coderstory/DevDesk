// src/module_git/git_content_model.cpp
#include "git_content_model.h"

#include "core/logging.h"
#include "platform/git/git_libgit2.h"  // pathToUtf8（日志锚打印 UTF-8 路径）

#include <algorithm>
#include <fstream>
#include <system_error>

namespace module::git {

namespace {

constexpr size_t kOversizeLimit = 1024 * 1024;  // 1 MiB（UI-A §5.5 占位判据）
constexpr size_t kBinaryScanBytes = 8192;       // 首 8KiB 含 NUL = 二进制（git 同口径）

// 严格 UTF-8 校验：拒绝代理区 / 超 U+10FFFF / 过长编码 / 悬挂 continuation
bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        unsigned codepoint = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = c & 0x07u;
        } else {
            return false;  // 0x80~0xBF 悬挂 continuation / 0xF8+ 非法首字节
        }
        if (i + static_cast<size_t>(extra) >= n) return false;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (cc & 0x3Fu);
        }
        if (extra == 1 && codepoint < 0x80) return false;          // 过长
        if (extra == 2 && codepoint < 0x800) return false;         // 过长
        if (extra == 3 && codepoint < 0x10000) return false;       // 过长
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;  // 代理区
        if (codepoint > 0x10FFFF) return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

} // namespace

FileContent FileContent::readFromDisk(const std::filesystem::path& absPath) {
    FileContent out;
    // 尺寸/读取全走 std::filesystem / ifstream 的 path 重载（Windows 窄化陷阱：
    // path.string() 转 ACP 会毁中文路径——F-10；doctest 中文路径用例锚此）
    std::error_code sizeEc;
    const auto size = std::filesystem::file_size(absPath, sizeEc);
    if (sizeEc) {
        out.state = ContentState::ReadError;
        out.errorMessage = "文件不存在或无权限读取";
        return out;
    }
    if (size == 0) {
        out.state = ContentState::Empty;
        return out;
    }
    if (size > kOversizeLimit) {
        // F-11③ 占位路径不读文件内容（GitAhead #161 反例）：尺寸闸短路，零内容
        // I/O；此日志即验收锚（◆日志）
        LOG_INFO("GitContent: 超限占位短路（未读内容）: " +
                 platform::git::pathToUtf8(absPath) + " (" +
                 std::to_string(size) + " bytes)");
        out.state = ContentState::Oversize;
        return out;
    }
    std::ifstream f(absPath, std::ios::binary);
    if (!f) {
        out.state = ContentState::ReadError;
        out.errorMessage = "文件不存在或无权限读取";
        return out;
    }
    std::string data(static_cast<size_t>(size), '\0');
    f.read(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (static_cast<size_t>(f.gcount()) != data.size()) {
        out.state = ContentState::ReadError;
        out.errorMessage = "读取不完整";
        return out;
    }

    const size_t scan = std::min(data.size(), kBinaryScanBytes);
    for (size_t i = 0; i < scan; ++i) {
        if (data[i] == '\0') {
            out.state = ContentState::Binary;
            return out;
        }
    }
    if (!isValidUtf8(data)) {
        out.state = ContentState::BadEncoding;
        return out;
    }

    out.state = ContentState::Ok;
    out.maxLineLen = 0;
    size_t start = 0;
    for (size_t i = 0; i <= data.size(); ++i) {
        if (i == data.size() || data[i] == '\n') {
            size_t end = i;
            if (end > start && data[end - 1] == '\r') --end;
            out.lines.emplace_back(data, start, end - start);
            // bug#21：maxLineLen 语义 = 码点数（按字节计会把 UTF-8 CJK 3 字节/字
            // 当 3 字符，横滚内容宽虚胖 ~3 倍）。Ok 态已过 isValidUtf8 严格校验，
            // 序列起点计数零额外校验成本。
            size_t codepoints = 0;
            for (size_t j = start; j < end; ++j) {
                if ((static_cast<unsigned char>(data[j]) & 0xC0) != 0x80) ++codepoints;
            }
            out.maxLineLen = std::max(out.maxLineLen, codepoints);
            start = i + 1;
        }
    }
    // 以 '\n' 结尾的文件：最后一次切分产生空尾行 → 丢弃（行号 1 起，IDEA 同口径）
    if (!out.lines.empty() && out.lines.back().empty() &&
        !data.empty() && data.back() == '\n') {
        out.lines.pop_back();
    }
    return out;
}

} // namespace module::git
