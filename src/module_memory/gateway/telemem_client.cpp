// src/module_memory/gateway/telemem_client.cpp
#include "telemem_client.h"
#include <utility>

#include <cstring>

#include "module_memory/engine/telemem_config.h"
#include "platform/http_client.h"
#include "platform/json.h"

namespace module::memory::gateway {

using engine::kTelememPort;

namespace {

using platform::json::JsonValue;

// MCP 流式 HTTP 的 200 响应体是 SSE 信封；剥出首个 data: 行载荷（与引擎健康探活
// 同一契约，telemem_engine.cpp 同款实现——这里复制而非共享：engine/ 目录冻结）
std::string stripSseEnvelope(const std::string& body) {
    if (body.rfind("event:", 0) != 0) return body;
    static constexpr const char* kTag = "data: ";
    const size_t at = body.find(kTag);
    if (at == std::string::npos) return body;
    const size_t begin = at + std::strlen(kTag);
    const size_t end = body.find('\n', begin);
    return body.substr(begin,
                       end == std::string::npos ? std::string::npos
                                                : end - begin);
}

core::Result<JsonValue> parseJsonRpcResult(const std::string& httpBody) {
    const auto parsed = platform::json::parse(stripSseEnvelope(httpBody));
    if (parsed.isFail()) {
        return core::Result<JsonValue>::fail(core::ErrorCode::JsonParseError,
                                             "响应非 JSON");
    }
    const JsonValue root = parsed.value();
    if (!root.isObject()) {
        return core::Result<JsonValue>::fail(core::ErrorCode::JsonParseError,
                                             "响应非 JSON 对象");
    }
    // 工具级结构化错误长在 HTTP 200 body 里（body.error / result.error），不能只看传输层
    const JsonValue error = root.asObject().get("error");
    if (!error.isNull()) {
        return core::Result<JsonValue>::fail(
            core::ErrorCode::HttpError,
            "引擎工具错误: " + (error.isString() ? error.asString()
                                                 : std::string()));
    }
    const JsonValue result = root.asObject().get("result");
    if (result.isNull()) {
        return core::Result<JsonValue>::fail(core::ErrorCode::JsonParseError,
                                             "响应缺 result");
    }
    if (result.isObject()) {
        const JsonValue inner = result.asObject().get("error");
        if (!inner.isNull()) {
            return core::Result<JsonValue>::fail(
                core::ErrorCode::HttpError,
                "引擎工具错误(嵌套): " + (inner.isString()
                                              ? inner.asString()
                                              : std::string()));
        }
        const JsonValue isErr = result.asObject().get("isError");
        if (isErr.isBool() && isErr.asBool()) {
            return core::Result<JsonValue>::fail(core::ErrorCode::HttpError,
                                                 "引擎工具级 isError");
        }
    }
    return core::Result<JsonValue>::ok(result);
}

std::string ymFromIso(const std::string& iso) {
    // "2026-10-27T..." / "2026-10-27 ..." → "YYYY-MM-DD"；其余原样返回（空保持空）
    if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(0, 10);
    }
    return iso;
}

// 单条记忆 → MemoryHit；兼容字段名：memory/text/content、id/memory_id、
// score、created_at/createdAt/timestamp
MemoryHit hitFromObject(const JsonValue& obj) {
    MemoryHit hit;
    const JsonValue mem = obj.asObject().get("memory");
    if (mem.isString()) {
        hit.text = mem.asString();
    } else {
        const JsonValue t = obj.asObject().get("text");
        if (t.isString()) hit.text = t.asString();
    }
    const JsonValue id = obj.asObject().get("id");
    if (id.isString()) {
        hit.memoryId = id.asString();
    } else if (id.isNumber()) {
        hit.memoryId = std::to_string(id.asInt());
    }
    for (const char* d : {"created_at", "createdAt", "timestamp"}) {
        const JsonValue date = obj.asObject().get(d);
        if (date.isString()) {
            hit.dateText = ymFromIso(date.asString());
            break;
        }
    }
    const JsonValue score = obj.asObject().get("score");
    if (score.isNumber()) hit.score = score.asDouble();
    return hit;
}

}  // namespace

core::Result<std::vector<MemoryHit>> parseSearchResponse(
    const std::string& httpBody) {
    auto result = parseJsonRpcResult(httpBody);
    if (result.isFail()) {
        return core::Result<std::vector<MemoryHit>>::fail(result.errorCode(),
                                                          result.errorMessage());
    }
    std::vector<MemoryHit> hits;
    const JsonValue content = result.value().asObject().get("content");
    if (content.isArray()) {
        for (int i = 0; i < content.size(); ++i) {
            const JsonValue item = content[i];
            if (!item.isObject()) continue;
            const JsonValue text = item.asObject().get("text");
            if (!text.isString()) continue;
            // text 元素本身是 mem0 风格 JSON 数组 → 逐条；否则按融合段落单条
            const auto inner = platform::json::parse(text.asString());
            if (inner.isOk() && inner.value().isArray()) {
                const JsonValue arr = inner.value();
                for (int k = 0; k < arr.size(); ++k) {
                    if (arr[k].isObject()) hits.push_back(hitFromObject(arr[k]));
                }
            } else {
                MemoryHit hit;
                hit.text = text.asString();
                hits.push_back(std::move(hit));
            }
        }
    }
    return core::Result<std::vector<MemoryHit>>::ok(std::move(hits));
}

core::Result<std::string> parseAddResponse(const std::string& httpBody) {
    auto result = parseJsonRpcResult(httpBody);
    if (result.isFail()) {
        return core::Result<std::string>::fail(result.errorCode(),
                                               result.errorMessage());
    }
    // 引擎侧新条目 id 尽力提取（telemem add 返回 stored 状态，无 id → 空串）
    std::string id;
    const JsonValue content = result.value().asObject().get("content");
    if (content.isArray() && content.size() > 0) {
        const JsonValue first = content[0];
        if (first.isObject()) {
            const auto inner = platform::json::parse(
                first.asObject().get("text").asString());
            if (inner.isOk() && inner.value().isObject()) {
                const JsonValue idv = inner.value().asObject().get("id");
                if (idv.isString()) id = idv.asString();
            }
        }
    }
    return core::Result<std::string>::ok(std::move(id));
}

core::Result<std::vector<MemoryHit>> TelememHttpRetriever::search(
    const std::string& query, int limit, int timeoutMs) {
    platform::json::JsonObject args;
    args.set("query", platform::json::makeString(query));
    args.set("limit", platform::json::makeInt(limit));
    platform::json::JsonObject params;
    params.set("name", platform::json::makeString("search_memories"));
    params.set("arguments", platform::json::JsonValue(args));
    platform::json::JsonObject rpc;
    rpc.set("jsonrpc", platform::json::makeString("2.0"));
    rpc.set("id", platform::json::makeInt(1));
    rpc.set("method", platform::json::makeString("tools/call"));
    rpc.set("params", platform::json::JsonValue(params));
    const auto body = platform::json::stringify(
        platform::json::JsonValue(rpc));
    if (body.isFail()) {
        return core::Result<std::vector<MemoryHit>>::fail(
            body.errorCode(), body.errorMessage());
    }

    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(engine::kTelememPort) + "/mcp";
    req.body = body.value();
    req.headers["Content-Type"] = "application/json";
    // MCP SDK 硬校验：Accept 双类型缺一 406（S2 live 实测教训）
    req.headers["Accept"] = "application/json, text/event-stream";
    req.timeoutMs = timeoutMs;
    const auto resp = platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<std::vector<MemoryHit>>::fail(
            resp.errorCode(), resp.errorMessage());
    }
    if (resp.value().statusCode != 200) {
        return core::Result<std::vector<MemoryHit>>::fail(
            core::ErrorCode::HttpError,
            "HTTP " + std::to_string(resp.value().statusCode));
    }
    return parseSearchResponse(resp.value().body);
}

namespace {

// tools/call 通用 POST（search/add/update 三工具共用；回环 + Accept 双类型）
core::Result<std::string> callTool(const std::string& tool,
                                   platform::json::JsonObject args,
                                   long timeoutMs) {
    platform::json::JsonObject params;
    params.set("name", platform::json::makeString(tool));
    params.set("arguments", platform::json::JsonValue(args));
    platform::json::JsonObject rpc;
    rpc.set("jsonrpc", platform::json::makeString("2.0"));
    rpc.set("id", platform::json::makeInt(1));
    rpc.set("method", platform::json::makeString("tools/call"));
    rpc.set("params", platform::json::JsonValue(params));
    const auto body = platform::json::stringify(platform::json::JsonValue(rpc));
    if (body.isFail()) {
        return core::Result<std::string>::fail(body.errorCode(),
                                               body.errorMessage());
    }
    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(engine::kTelememPort) + "/mcp";
    req.body = body.value();
    req.headers["Content-Type"] = "application/json";
    // MCP SDK 硬校验：Accept 双类型缺一 406（S2 live 实测教训）
    req.headers["Accept"] = "application/json, text/event-stream";
    req.timeoutMs = timeoutMs;
    const auto resp = platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    if (resp.value().statusCode != 200) {
        return core::Result<std::string>::fail(
            core::ErrorCode::HttpError,
            "HTTP " + std::to_string(resp.value().statusCode));
    }
    return core::Result<std::string>::ok(resp.value().body);
}

}  // namespace

core::Result<void> TelememHttpRetriever::setMemoryForgotten(
    const std::string& memoryId, bool forgotten) {
    platform::json::JsonObject meta;
    meta.set("forgotten", platform::json::makeBool(forgotten));
    platform::json::JsonObject args;
    args.set("memory_id", platform::json::makeString(memoryId));
    args.set("metadata", platform::json::JsonValue(meta));
    const auto resp = callTool("update_memory", std::move(args), 10000);
    if (resp.isFail()) {
        return core::Result<void>::fail(resp.errorCode(), resp.errorMessage());
    }
    const auto parsed = parseAddResponse(resp.value());
    if (parsed.isFail()) {
        return core::Result<void>::fail(parsed.errorCode(),
                                        parsed.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::string> TelememHttpRetriever::addDirect(
    const std::string& text) {
    platform::json::JsonObject args;
    args.set("text", platform::json::makeString(text));
    args.set("infer", platform::json::makeBool(false));  // 直存语义，不调云端提炼
    platform::json::JsonObject params;
    params.set("name", platform::json::makeString("add_memory"));
    params.set("arguments", platform::json::JsonValue(args));
    platform::json::JsonObject rpc;
    rpc.set("jsonrpc", platform::json::makeString("2.0"));
    rpc.set("id", platform::json::makeInt(1));
    rpc.set("method", platform::json::makeString("tools/call"));
    rpc.set("params", platform::json::JsonValue(params));
    const auto body = platform::json::stringify(platform::json::JsonValue(rpc));
    if (body.isFail()) {
        return core::Result<std::string>::fail(body.errorCode(),
                                               body.errorMessage());
    }

    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(engine::kTelememPort) + "/mcp";
    req.body = body.value();
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json, text/event-stream";
    req.timeoutMs = 10000;  // 入库非 hook 同步路径，放宽到 10s
    const auto resp = platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    if (resp.value().statusCode != 200) {
        return core::Result<std::string>::fail(
            core::ErrorCode::HttpError,
            "HTTP " + std::to_string(resp.value().statusCode));
    }
    return parseAddResponse(resp.value().body);
}

}  // namespace module::memory::gateway
