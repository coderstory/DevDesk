// src/module_memory/gateway/telemem_client.h
// 检索接口缝（spec §2 ④）：调用方只依赖 IMemoryRetriever，不 import/直连 telemem；
// A 阶段实现 = 转发 telemem MCP（127.0.0.1:8421 JSON-RPC），升级 C 换实现时调用方零改动。
// 密钥红线：本通道不带任何凭据（引擎侧已注入）。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"

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
};

// MCP-HTTP 实现（生产装配）。telemem 只绑回环（kTelememPort）。
class TelememHttpRetriever : public IMemoryRetriever {
public:
    core::Result<std::vector<MemoryHit>> search(const std::string& query,
                                                int limit,
                                                int timeoutMs) override;
    core::Result<std::string> addDirect(const std::string& text) override;
};

// 响应解析纯函数（单测钉三层）：SSE 信封剥离 → JSON-RPC envelope（含嵌套
// result.error / body.error 判错，同健康判据契约）→ content[] 逐条化：
//   - text 元素本身是 mem0 风格 JSON 数组（含 memory/score/createdAt 字段）→ 逐条；
//   - 否则按"融合段落"处理（telemem search_memories 默认形态，推测项 6）→ 单条。
core::Result<std::vector<MemoryHit>> parseSearchResponse(
    const std::string& httpBody);

// add_memory 响应解析：工具级错误（isError / error 字段）→ Error；成功 → 新条目 id
//（telemem 返回 {"status":"stored",...} 无 id → 空串，调用方按空处理）
core::Result<std::string> parseAddResponse(const std::string& httpBody);

}  // namespace module::memory::gateway
