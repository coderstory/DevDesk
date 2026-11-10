// src/platform/single_instance.h
// 单实例互斥 + 二次运行唤起首个实例窗口 +（Windows）文件路径跨实例送达
// 头文件平台无关；实现按平台拆文件（与 keychain / tray_icon 同一模式）
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace platform::single_instance {

// ack 等待封顶（对齐 PRD F-01「5 秒内开窗」验收口径；测试经第二形参注入短缝，不等真 5s）
inline constexpr unsigned kAckTimeoutMs = 5000;

// ── F-01 路径载荷协议（平台无关；编解码公开供测试直测）──
// 布局（u32 一律小端）：[魔数][ack 名长][ack 名字节][路径数]{[路径字节长][UTF-8 字节]}
// 上界：整包 ≤ kMaxPayloadBytes（恰一命名 section 尺寸）；路径数 ≤ kMaxPayloadPaths。
// 超界 / 魔数错 / 任一字段越界 → 编码返回空串、解码 false——接收方整包丢弃、永不部分
// 消费（悬垂 section / 半包 / 魔数错按本判据拒收）。传输层只搬运不校验 UTF-8，非法路径
// 由上层打开流程兜底呈现。
struct PathPayload {
    std::string ackName;             // 专名 ack 事件名（发送方 pid+tick 生成，载荷头携带；接收方按它 SetEvent）
    std::vector<std::string> paths;  // UTF-8 文件路径，按送达序
};
inline constexpr uint32_t kPathPayloadMagic = 0x44444D44u;  // 小端内存呈现 "DDMD"
inline constexpr size_t kMaxPayloadBytes = 32 * 1024;
inline constexpr uint32_t kMaxPayloadPaths = 256;

namespace si_detail {

inline void putU32Le(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

// 前置条件：pos ≤ in.size()（只在成功后推进，恒成立）
inline bool getU32Le(const std::string& in, size_t& pos, uint32_t& v) {
    if (in.size() - pos < 4) return false;
    v = static_cast<uint32_t>(static_cast<unsigned char>(in[pos])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[pos + 1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[pos + 2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[pos + 3])) << 24);
    pos += 4;
    return true;
}

} // namespace si_detail

// 超界（整包 > 32KB 或路径数 > 256）→ 返回空串（发送方视为不可投递，直接失败退出）
inline std::string encodePathPayload(const PathPayload& payload) {
    if (payload.paths.size() > kMaxPayloadPaths) return {};
    uint64_t total = 4 + 4 + payload.ackName.size() + 4;
    for (const auto& p : payload.paths) total += 4 + p.size();
    if (total > kMaxPayloadBytes) return {};

    std::string out;
    out.reserve(static_cast<size_t>(total));
    si_detail::putU32Le(out, kPathPayloadMagic);
    si_detail::putU32Le(out, static_cast<uint32_t>(payload.ackName.size()));
    out.append(payload.ackName);
    si_detail::putU32Le(out, static_cast<uint32_t>(payload.paths.size()));
    for (const auto& p : payload.paths) {
        si_detail::putU32Le(out, static_cast<uint32_t>(p.size()));
        out.append(p);
    }
    return out;
}

// 任何越界 / 魔数错 / 零长字段 → false 且 out 置空。尾部剩余字节忽略（section 复用
// 残留，不判非法）。零长路径 / 零路径数 / 空 ack 名判非法 = 契约：发送方只产非空
// 路径与专名 ack，且 section 未覆写区恒零填充——截断半包的合法头接全零尾恰解出
// 「零长路径」，放行即误消费悬垂包
inline bool decodePathPayload(const std::string& bytes, PathPayload& out) {
    out = PathPayload{};
    if (bytes.size() > kMaxPayloadBytes) return false;
    size_t pos = 0;
    uint32_t magic = 0;
    if (!si_detail::getU32Le(bytes, pos, magic) || magic != kPathPayloadMagic)
        return false;
    uint32_t ackLen = 0;
    if (!si_detail::getU32Le(bytes, pos, ackLen) || ackLen == 0 ||
        ackLen > static_cast<uint32_t>(bytes.size() - pos))
        return false;
    out.ackName.assign(bytes, pos, ackLen);
    pos += ackLen;
    uint32_t pathCount = 0;
    if (!si_detail::getU32Le(bytes, pos, pathCount) || pathCount == 0 ||
        pathCount > kMaxPayloadPaths)
        return false;
    for (uint32_t i = 0; i < pathCount; ++i) {
        uint32_t len = 0;
        if (!si_detail::getU32Le(bytes, pos, len) || len == 0 ||
            len > static_cast<uint32_t>(bytes.size() - pos))
            return false;
        out.paths.emplace_back(bytes, pos, len);
        pos += len;
    }
    return true;
}

// 第一实例拿锁返回 true，第二实例发现锁已存在返回 false。
// Windows：命名互斥量 Local\DevDesk.SingleInstance.Mutex；首实例同时启动监听线程。
// macOS / POSIX：恒 true（.app bundle 由系统保证单实例；其它 POSIX 桌面环境同理）。
bool acquireInstanceLock();

// 路径接收回调：首实例监听线程读映射并整包校验通过后触发。
// 线程契约：回调运行在监听线程——注册方须线程安全，且只做入队/投递回主线程，
// 勿在回调内直接触碰 UI。生产在启动装配期注册一次后不更换。
using PathsHandler = std::function<void(const std::vector<std::string>& paths)>;
void setOpenPathsHandler(PathsHandler handler);

// 第二实例：带路径唤醒首实例（Windows：整段事务持发送互斥 → 写命名 section →
// SetEvent 唤醒 → 等载荷头携带的专名 ack，封顶 ackTimeoutMs；映射句柄持到 ack/超时
// 才关，保证首实例读取期映射对象存活）。返回 true = 首实例已确认收到；false = 未送达
//（ack 超时 / 通路不可用），调用方照常退出——单实例不变式优先，路径丢弃 = 改造前
// 退化行为。macOS / POSIX：no-op 恒 false（路径走 Apple Event / argv，不经此通道）。
bool notifyExistingInstance(const std::vector<std::string>& paths,
                            unsigned ackTimeoutMs = kAckTimeoutMs);

// 监听侧同步处理一次：读命名 section → 整包校验 → SetEvent(载荷内专名 ack) → 路径回调。
// 无合法载荷（纯唤起 / 悬垂脏包）返回 false、不误 ack、不入队。生产由 Windows 监听
// 线程在唤醒循环里调用；公开出来供进程内全链路单测同步驱动（免真线程时序等待）。
bool consumeSectionSignalOnce();

// ── 测试缝（仅 Windows 实现生效，其余平台 no-op；生产路径从不调用）──
// 覆写命名对象根名（默认 "DevDesk" → Local\<root>.SingleInstance.*），隔离开发机真实
// 实例通道。须先于 acquireInstanceLock 调用（监听线程按启动时的根名建事件）。
void setSingleInstanceRootForTest(const std::string& root);
// 原样写命名 section（可写任意非法字节，造悬垂 / 半包 / 坏魔数），供校验丢弃路径单测。
bool writeSectionForTest(const std::string& bytes);

} // namespace platform::single_instance
