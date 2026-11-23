// 自研二维码编码器（Model 2 / byte 模式 / 纠错 L）。只服务本机会话绑定 URL：
//   http://<ip>:<port>/#b=<token>，典型 60-80 字节，自动选版本（V4/V5）。
// 边界：支持 V1-V10（L 级）；V4/V5 有 Vision 解码验证与逐位 golden 对照，
//       V6+ 走多块交织 + 版本信息，按 ISO/IEC 18004 表驱动，仅做结构断言。
// 不依赖 EUI / 平台层：纯标准库，便于无头单测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace module::terminal::web {

enum class QrEcLevel { L };

struct QrMatrix {
    int size = 0;                  // 边长（模块数）= 17 + 4*version
    int version = 0;
    int mask = -1;
    std::vector<uint8_t> modules;  // size*size 行优先；1=深色

    bool at(int row, int col) const {
        return modules[static_cast<std::size_t>(row) * static_cast<std::size_t>(size) +
                       static_cast<std::size_t>(col)] != 0;
    }
};

struct QrEncodeOptions {
    int version = 0;  // 0=按容量自动选
    int mask = -1;    // -1=按 ISO 罚分自动选
};

// 成功 true；容量溢出 / 参数非法（版本越界、掩码越界、非 L 级）false，不越界不截断。
bool encode(const std::string& payload, QrEcLevel level, QrMatrix& out,
            const QrEncodeOptions& options = {});

}  // namespace module::terminal::web
