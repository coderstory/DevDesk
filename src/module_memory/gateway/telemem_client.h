// src/module_memory/gateway/telemem_client.h
// 检索接口缝（spec §2 ④）：调用方只依赖 IMemoryRetriever，不 import/直连 telemem；
// A 阶段实现 = 转发 telemem MCP（127.0.0.1:8421 JSON-RPC），升级 C 换实现时调用方零改动。
// 密钥红线：本通道不带任何凭据（引擎侧已注入）。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"
#include "platform/json.h"

namespace module::memory::gateway {

struct MemoryHit {
    std::string text;      // 记忆正文（单行化前）
    std::string memoryId;  // 引擎侧条目 id（来源标注；缺失为空）
    std::string dateText;  // "YYYY-MM-DD"（缺失为空）
    double score = 0.0;    // 相关度（排序输入；缺失 0）
};

class IMemoryRetriever {
public:
    virtual ~IMemoryRetriever() = default;

    // 检索：调用方给硬超时（hook 通道 ≤800ms）；失败 = Error，不抛异常
    virtual core::Result<std::vector<MemoryHit>> search(const std::string& query,
                                                        int limit,
                                                        int timeoutMs) = 0;
    // 直存语义入库（infer=false：不调云端提炼）；返回引擎侧新条目 id（缺失空串）
    virtual core::Result<std::string> addDirect(const std::string& text) = 0;

    // 提炼语义入库（infer=true：telemem 调云端 LLM 把原文段提炼为结论再落库，
    // 需-3 spec §2）。接口默认 NotImplemented：既有假实现/直存调用方零波及，
    // 提炼通道由 TelememHttpRetriever 与测试假实现按需实现。硬档暂停闸与
    // 每小时节流在 pipeline::infer_batch 消费侧统一设闸，本通道不做成本判定。
    virtual core::Result<std::string> addInferred(const std::string& text) {
        (void)text;
        return core::Result<std::string>::fail(core::ErrorCode::NotImplemented,
                                               "本实现不支持提炼入库");
    }

    // 移出「自动想起」（卡片代理层执行面）：live 对账定案（2026-10-28）——
    // telemem 1.10.1 无 forgotten 过滤缝（update 打标记后 search 照回），唯一
    // 真实生效通道 = delete_memory；故 true → delete_memory，false（恢复）引擎
    // 侧无缝 → 如实报 NotImplemented，恢复由提案执行层按原卡证据重录入
    virtual core::Result<void> setMemoryForgotten(const std::string& memoryId,
                                                  bool forgotten) = 0;
};

// MCP-HTTP 实现（生产装配）。telemem 只绑回环（kTelememPort）。
// 会话契约（2026-10-28 live 定案）：telemem 1.10.1 streamable-http 除 initialize
// 外一律要求 Mcp-Session-Id 头（缺失 400 -32600）——本实现 initialize 一次缓存
// 会话 id，tools/call 全部回传；引擎重启致 404 时自动重取一次。
class TelememHttpRetriever : public IMemoryRetriever {
public:
    core::Result<std::vector<MemoryHit>> search(const std::string& query,
                                                int limit,
                                                int timeoutMs) override;
    core::Result<std::string> addDirect(const std::string& text) override;
    core::Result<std::string> addInferred(const std::string& text) override;
    core::Result<void> setMemoryForgotten(const std::string& memoryId,
                                          bool forgotten) override;
};

// 响应解析纯函数（单测钉三层）：SSE 信封剥离 → JSON-RPC envelope（含嵌套
// result.error / body.error 判错，同健康判据契约）→ content[] 逐条化：
//   - text 元素本身是 mem0 风格 JSON 数组（含 memory/score/createdAt 字段）→ 逐条；
//   - text 为 {"query":…,"memories":"<融合段落>"} 信封（telemem search_memories
//     实测唯一形态，live 定案 2026-10-28）→ 取 memories 正文按单条，空段落=无命中；
//     绝不把信封 JSON 原文当记忆正文（旧实现缺陷）。
core::Result<std::vector<MemoryHit>> parseSearchResponse(
    const std::string& httpBody);

// add_memory 响应解析：工具级错误（isError / error 字段）→ Error；成功 → 新条目 id
//（telemem 返回 {"status":"stored",...} 无 id → 空串，调用方按空处理）
core::Result<std::string> parseAddResponse(const std::string& httpBody);

// add_memory 入参组装（纯函数，单测钉 infer 契约）：infer=false 直存 / true 提炼
inline platform::json::JsonObject buildAddMemoryArgs(const std::string& text,
                                                     bool infer) {
    platform::json::JsonObject args;
    args.set("text", platform::json::makeString(text));
    args.set("infer", platform::json::makeBool(infer));
    return args;
}

}  // namespace module::memory::gateway
