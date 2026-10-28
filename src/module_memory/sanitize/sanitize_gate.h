// src/module_memory/sanitize/sanitize_gate.h
// 脱敏闸（S3 ②）：云端外发前唯一清洗层，core.py 母本（tools/memory-lab/sanitize/core.py）
// 的 C++ 移植。契约（与母本逐条对齐，偏离即契约变更）：
// - 泄漏 0 优先于误杀：拿不准的形态一律脱敏（fail-closed）；
// - 六类检测（phone/email/id_card 含校验位/ipv4/url_param/secret）+ 优先级去重叠；
// - 检测在等长 shadow 文本上做（全角字符 1:1 映射半角，码点级等长），命中区间映射回原文；
// - preview 等长全遮蔽（'*' × 码点数），绝不携带原文片段（含首尾字符）；
// - fail-closed 是传播式：非法 UTF-8 视为检测器无法保证 → 返回 Error，调用方必须中断外发；
//   禁止在调用点把 Error 当"无命中"处理（否则静默变 fail-open，护栏测试钉住此契约）。
// 反重构警告：不要把 allowlist 当脱敏开关；allowlist 只豁免占位/回环值
//（127.0.0.1/0.0.0.0/255.255.255.255/13800000000），新增豁免项必须同步补进
// tools/memory-lab/sanitize/datasets/redteam.json 并说明理由。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::memory::sanitize {

struct SanitizeHit {
    std::string category;  // phone | email | id_card | ipv4 | url_param | secret
    std::size_t start = 0;  // 原文 UTF-8 字节偏移（含）
    std::size_t end = 0;    // 原文 UTF-8 字节偏移（不含）
    std::string detail;     // 检测器细类（如 bearer / kv_credential / id18:checksum_ok）
    std::string preview;    // 等长全遮蔽，绝不携带原文
};

struct SanitizeResult {
    std::string text;               // 命中区间替换为 [REDACTED:<category>] 后的文本
    std::vector<SanitizeHit> hits;  // 按位置升序、互不重叠

    std::vector<std::string> categories() const;  // 去重升序
};

// 纯检测（不修改文本）。输入非法 UTF-8 → Error（fail-closed）。
core::Result<std::vector<SanitizeHit>> scanText(const std::string& utf8Text);

// 外发前唯一入口。幂等：对输出再跑一遍不再变化；非法 UTF-8 → Error。
core::Result<SanitizeResult> sanitizeText(const std::string& utf8Text);

// 运行时开关的闸门：默认开；关 = 旁路直通 + 记一条审计日志行。
// 审计行只含长度/计数，绝不含文本内容（内容红线同 telemem_config 密钥纪律）。
class SanitizeGate {
public:
    // auditLine：旁路/过闸的审计回调；空 = 只静默旁路（测试用）。
    // 生产宿主接 core::log 文件通道；行内不得携带原文。
    using AuditFn = std::function<void(const std::string& auditLine)>;

    explicit SanitizeGate(AuditFn auditLine = {});

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // enabled：过闸（非法 UTF-8 → Error，调用方必须中断外发）。
    // disabled：旁路原样返回 + 审计行（hits 恒空，text == 输入）。
    core::Result<SanitizeResult> process(const std::string& utf8Text) const;

private:
    bool enabled_ = true;
    AuditFn auditLine_;
};

}  // namespace module::memory::sanitize
