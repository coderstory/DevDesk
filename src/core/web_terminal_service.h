// 网页终端跨模块窄接口（need#490 W1）：module_settings 的设置卡读取/写入网页终端状态，
// 但 module_settings 不能 include module_terminal 的头，故经 ServiceLocator 取本接口。
// 本头只含标准库类型——EUI 绘制留在模块内，二维码以模块矩阵跨层（不把 eui::Ui 塞进 core）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

enum class WebBindState {
    Stopped,
    Listening,
    Error,
};

// 只读快照：实现侧在 UI 主线程采样，消费方按值取走后再画（不得持锁渲染）
struct WebAccessSnapshot {
    bool running = false;
    int port = 0;                        // 实际监听端口；未运行 = 0
    int configuredPort = 0;              // 持久化配置端口（未运行时的设置页显示源）
    std::vector<std::string> addresses;  // 可扫码的本机 IPv4（含回环，不含 0.0.0.0）
    WebBindState bindState = WebBindState::Stopped;
    std::int64_t bindTime = 0;           // unix 秒；0 = 未绑定
    std::int64_t lastLogin = 0;          // unix 秒；0 = 从未登录
    int onlineCount = 0;                 // 当前在线网页会话数
    std::vector<std::uint8_t> qrMatrix;  // 行优先 0/1 模块矩阵，qrSize × qrSize
    int qrSize = 0;
    int tokenRemainingSec = 0;           // 绑定令牌剩余秒数；0 = 无有效令牌
    std::string error;                   // bindState == Error 时的人话原因
};

// 未设置的字段 = 保持原值（补丁语义，唯一写入入口 writeSettings 消费）
struct TerminalSettingsPatch {
    std::optional<std::string> fontFamily;
    std::optional<int> fontSize;
    std::optional<std::string> colorMode;
    std::optional<std::string> palette;
    std::optional<std::string> shell;
    std::optional<std::string> startupDir;
    std::optional<int> webPort;  // 网页服务监听端口（1024-65535）
};

class IWebTerminalService {
public:
    virtual ~IWebTerminalService() = default;

    virtual WebAccessSnapshot snapshot() const = 0;
    virtual core::Result<void> start() = 0;
    virtual core::Result<void> stop() = 0;
    // 旧令牌与 TOTP 密钥一并作废后重新生成（unclaimed）
    virtual core::Result<void> refreshToken() = 0;
    // 撤销验证器绑定：清空票据表，所有网页登录立即失效
    virtual core::Result<void> revokeBinding() = 0;
    // 设置单一写入入口：校验 → 写内存 → 落库（失败回滚）→ 广播；网页与桌面面板共用
    virtual core::Result<void> writeSettings(const TerminalSettingsPatch& patch) = 0;
};

} // namespace core
