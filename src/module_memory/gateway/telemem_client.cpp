// src/module_memory/gateway/telemem_client.cpp
#include "telemem_client.h"
#include <utility>

#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

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

std::string lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trimWhitespace(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// BUG-114-2：注入相似度下限。telemem search_memories 原生 threshold 参数
//（Optional[float] ∈ [0,1] 相似度下限，缺省 None = 不过滤）。取值 0.4 出处
//（2026-11-03 mac e2e 校准）：0.5 时探针弱相似短语查询「增量构建」「8421」
// 被过杀（top-5 全部 score<0.5 → S4/S7 掉档），0.3 负对照不拦（POC 实测
// 338 字注入）；0.4 下探针全绿 + 负对照拦截（证据 run3）。校准硬门：e2e 探针
// S6 冻结 15 查询 Recall@5 ≥85%，掉档按同法下调重校。
// bug255 重校实验（2026-11-10 mac POC，终报结论=阈值不可解，保持 0.4）：
// mem0 faiss score=1/(1+L2²)，llama-server 输出归一化向量 → 正交地板
// 1/(1+2)≈0.333，0.4 实际只拦 cos<0.25。实测分数带：负例（天气/股票）
// 0.4098-0.4518；弱短查询「增量构建」0.4380、「8421」0.4179；近逐字自然
// 提问 0.48-0.6+；同文短语（12 字节逐字前缀）≥0.55。两类验收场景分数带
// 重叠：0.4 下负对照泄漏（天气 0.4098>0.4），0.5 下近逐字 hook 提问被杀
// （实测 data2 真实提问 ~0.48<0.5）——无阈值同时满足「负对照零注入」与
// 「同文提问注入非空」，参数不可解，裁决项挂 need159（候选：换更强
// embedding/重排/hybrid 检索为引擎侧改造）。S6 冻结评测在 0.4/0.5 均
// 100%（校准硬门 ≥85% 不构成区分）。bge 检索指令前缀 POC 为负收益
// （正例 0.7525→0.7083，负例 0.2798→0.3257），不采用。
// hook 注入与 recall_eval 评测共用本 search 缝，阈值口径有意一致（评测即实测
// 注入行为）；若未来评测需独立参数，在此写明分歧理由后再拆。
constexpr double kSearchSimilarityThreshold = 0.4;

// 大小写不敏感取响应头（curl 原样保留服务器大小写；telemem/uvicorn 回小写）
std::string headerValue(const platform::http::Response& resp,
                        const std::string& name) {
    for (const auto& [k, v] : resp.headers) {
        if (lowered(k) == lowered(name)) return v;
    }
    return {};
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

// ── MCP 会话缝（2026-10-28 live 对账定案）────────────────────────────────
// telemem 1.10.1 streamable-http 严格按 MCP 规范校验：除 initialize 外一切
// POST 都要求 Mcp-Session-Id 头，缺失即 400 -32600 "Missing session ID"。
// 旧实现不带会话头 → search/add 全挂。此处 initialize 一次取回会话 id 缓存，
// 后续 tools/call 全部回传；引擎重启导致会话失效（404）时自动重取一次。

std::mutex g_sessionMu;
std::string g_sessionId;

void invalidateSessionId() {
    std::lock_guard<std::mutex> lk(g_sessionMu);
    g_sessionId.clear();
}

core::Result<std::string> ensureSessionId() {
    {
        std::lock_guard<std::mutex> lk(g_sessionMu);
        if (!g_sessionId.empty()) {
            return core::Result<std::string>::ok(g_sessionId);
        }
    }
    platform::json::JsonObject client;
    client.set("name", platform::json::makeString("devdesk-gateway"));
    client.set("version", platform::json::makeString("1.0"));
    platform::json::JsonObject params;
    params.set("protocolVersion", platform::json::makeString("2025-06-18"));
    params.set("capabilities", platform::json::JsonValue(platform::json::makeObject()));
    params.set("clientInfo", platform::json::JsonValue(client));
    platform::json::JsonObject rpc;
    rpc.set("jsonrpc", platform::json::makeString("2.0"));
    rpc.set("id", platform::json::makeInt(1));
    rpc.set("method", platform::json::makeString("initialize"));
    rpc.set("params", platform::json::JsonValue(params));
    const auto body = platform::json::stringify(platform::json::JsonValue(rpc));
    if (body.isFail()) {
        return core::Result<std::string>::fail(body.errorCode(),
                                               body.errorMessage());
    }
    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(kTelememPort) + "/mcp";
    req.body = body.value();
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json, text/event-stream";
    req.timeoutMs = 5000;
    const auto resp = platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    if (resp.value().statusCode != 200) {
        return core::Result<std::string>::fail(
            core::ErrorCode::HttpError,
            "initialize HTTP " + std::to_string(resp.value().statusCode));
    }
    const std::string sid = headerValue(resp.value(), "mcp-session-id");
    if (sid.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::HttpError, "initialize 响应缺 mcp-session-id");
    }
    std::lock_guard<std::mutex> lk(g_sessionMu);
    g_sessionId = sid;
    return core::Result<std::string>::ok(sid);
}

// tools/call 通用 POST（search/add/get/update/delete 共用；回环 + Accept 双类型 +
// 会话头）。会话失效（引擎重启后旧 id 被拒）→ 清缓存重取会话再试一次。
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
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto sid = ensureSessionId();
        if (sid.isFail()) {
            return core::Result<std::string>::fail(sid.errorCode(),
                                                   sid.errorMessage());
        }
        platform::http::Request req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(kTelememPort) + "/mcp";
        req.body = body.value();
        req.headers["Content-Type"] = "application/json";
        // MCP SDK 硬校验：Accept 双类型缺一 406（S2 live 实测教训）
        req.headers["Accept"] = "application/json, text/event-stream";
        req.headers["Mcp-Session-Id"] = sid.value();
        req.timeoutMs = timeoutMs;
        const auto resp = platform::http::send(req);
        if (resp.isFail()) {
            return core::Result<std::string>::fail(resp.errorCode(),
                                                   resp.errorMessage());
        }
        if (resp.value().statusCode == 404 && attempt == 0) {
            invalidateSessionId();  // 引擎重启/会话过期：重取一次
            continue;
        }
        if (resp.value().statusCode != 200) {
            return core::Result<std::string>::fail(
                core::ErrorCode::HttpError,
                "HTTP " + std::to_string(resp.value().statusCode));
        }
        return core::Result<std::string>::ok(resp.value().body);
    }
    return core::Result<std::string>::fail(core::ErrorCode::HttpError,
                                           "会话重取后仍失败");
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
            const auto inner = platform::json::parse(text.asString());
            if (inner.isOk() && inner.value().isArray()) {
                // mem0 风格 JSON 数组 → 逐条（带 id/score/日期）
                const JsonValue arr = inner.value();
                for (int k = 0; k < arr.size(); ++k) {
                    if (arr[k].isObject()) hits.push_back(hitFromObject(arr[k]));
                }
            } else if (inner.isOk() && inner.value().isObject() &&
                       inner.value().asObject().get("memories").isString()) {
                // telemem search_memories 真实形态（2026-10-28 live 定案）：
                // content[0].text = {"query":…,"memories":"<融合段落>"}——融合
                // 段落是空格连接的单串，无条目 id/日期；按单条取 memories 正文，
                // 绝不把 JSON 信封原文当记忆正文注入（旧实现踩此坑）。空段落=无命中。
                MemoryHit hit;
                hit.text = inner.value().asObject().get("memories").asString();
                if (!hit.text.empty()) hits.push_back(std::move(hit));
            } else {
                MemoryHit hit;
                hit.text = text.asString();
                if (!hit.text.empty()) hits.push_back(std::move(hit));
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
                // telemem _call 异常兜底（其 mcp/server.py:113-120）：工具内异常
                // 以普通 dict {"error","detail"} 回 HTTP 200（非 isError 信封），
                // 必须在此判负——否则云端 401/超时被静默成 ok("")，记账出队但
                // 零产物（bug239 真机实证形态；与 get_memory 的 not_found
                // fail-closed 同一判负先例）
                const JsonValue err = inner.value().asObject().get("error");
                if (!err.isNull()) {
                    std::string msg =
                        err.isString() ? err.asString() : std::string("unknown");
                    const JsonValue detail =
                        inner.value().asObject().get("detail");
                    if (detail.isString() && !detail.asString().empty()) {
                        msg += ": " + detail.asString();
                    }
                    return core::Result<std::string>::fail(
                        core::ErrorCode::HttpError, "引擎工具异常: " + msg);
                }
                const JsonValue idv = inner.value().asObject().get("id");
                if (idv.isString()) id = idv.asString();
            }
        }
    }
    return core::Result<std::string>::ok(std::move(id));
}

std::int64_t isoToEpochSec(const std::string& iso) {
    // "2026-10-27T12:34:56Z"（telemem 固定 UTC 输出，POC 定案）；容忍空格分隔
    // 与毫秒尾缀；空/非法/越界 → 0
    if (iso.size() < 19 || iso[4] != '-' || iso[7] != '-' ||
        (iso[10] != 'T' && iso[10] != ' ') || iso[13] != ':' ||
        iso[16] != ':') {
        return 0;
    }
    const auto num = [&iso](std::size_t at) -> int {
        const int hi = iso[at] - '0';
        const int lo = iso[at + 1] - '0';
        if (hi < 0 || hi > 9 || lo < 0 || lo > 9) return -1;
        return hi * 10 + lo;
    };
    const long long y = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 +
                        (iso[2] - '0') * 10 + (iso[3] - '0');
    const int mo = num(5), d = num(8), h = num(11), mi = num(14), s = num(17);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || s < 0 || s > 60) {
        return 0;
    }
    // 公历日期 → 自 1970-01-01 天数（Hinnant days_from_civil，平台无关，
    // 规避 timegm/_mkgmtime 平台差异）；引擎输出恒 UTC 无时区换算
    const long long yy = y - (mo <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        (153u * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2) / 5 +
        static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

core::Result<std::vector<MemoryListItem>> parseListResponse(
    const std::string& httpBody) {
    auto result = parseJsonRpcResult(httpBody);
    if (result.isFail()) {
        return core::Result<std::vector<MemoryListItem>>::fail(
            result.errorCode(), result.errorMessage());
    }
    std::vector<MemoryListItem> items;
    const JsonValue content = result.value().asObject().get("content");
    if (!content.isArray() || content.size() == 0) {
        return core::Result<std::vector<MemoryListItem>>::ok(std::move(items));
    }
    const auto inner =
        platform::json::parse(content[0].asObject().get("text").asString());
    if (inner.isFail() || !inner.value().isObject()) {
        return core::Result<std::vector<MemoryListItem>>::ok(std::move(items));
    }
    const JsonValue results = inner.value().asObject().get("results");
    if (!results.isArray()) {
        return core::Result<std::vector<MemoryListItem>>::ok(std::move(items));
    }
    for (int i = 0; i < results.size(); ++i) {
        if (!results[i].isObject()) continue;
        const JsonValue id = results[i].asObject().get("id");
        const JsonValue mem = results[i].asObject().get("memory");
        if (!id.isString() || !mem.isString()) continue;
        MemoryListItem item;
        item.id = id.asString();
        item.text = mem.asString();
        const JsonValue upd = results[i].asObject().get("updated_at");
        if (upd.isString()) item.updatedAtEpoch = isoToEpochSec(upd.asString());
        items.push_back(std::move(item));
    }
    return core::Result<std::vector<MemoryListItem>>::ok(std::move(items));
}

core::Result<std::int64_t> parseGetMemoryResponse(const std::string& httpBody) {
    auto result = parseJsonRpcResult(httpBody);
    if (result.isFail()) {
        return core::Result<std::int64_t>::fail(result.errorCode(),
                                                result.errorMessage());
    }
    const JsonValue content = result.value().asObject().get("content");
    if (!content.isArray() || content.size() == 0 || !content[0].isObject()) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::JsonParseError,
                                                "get_memory 响应缺 content");
    }
    const std::string text = content[0].asObject().get("text").asString();
    std::string updatedIso;
    const auto inner = platform::json::parse(text);
    if (inner.isOk() && inner.value().isObject()) {
        const JsonValue upd = inner.value().asObject().get("updated_at");
        if (upd.isString()) {
            updatedIso = upd.asString();
        } else {
            // 兼容 results 包裹形态（与 get_memories 同形时）
            const JsonValue results = inner.value().asObject().get("results");
            if (results.isArray() && results.size() > 0 &&
                results[0].isObject()) {
                const JsonValue u2 = results[0].asObject().get("updated_at");
                if (u2.isString()) updatedIso = u2.asString();
            }
        }
    }
    if (!updatedIso.empty()) {
        const std::int64_t epoch = isoToEpochSec(updatedIso);
        if (epoch <= 0) {
            return core::Result<std::int64_t>::fail(
                core::ErrorCode::JsonParseError,
                "updated_at 无法解析: " + updatedIso);
        }
        return core::Result<std::int64_t>::ok(epoch);
    }
    // mem0 delete 后 get_memory 回 not_found（POC 定案）→ 明确错误码，
    // 校验闭包据此分流（条目已删 ≠ 网络故障，方案卡 §二）
    if (text.find("not_found") != std::string::npos) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::NotFound,
                                                "条目不存在（not_found）");
    }
    return core::Result<std::int64_t>::fail(core::ErrorCode::JsonParseError,
                                            "get_memory 响应缺 updated_at");
}

core::Result<std::vector<MemoryHit>> TelememHttpRetriever::search(
    const std::string& query, int limit, int timeoutMs) {
    platform::json::JsonObject args;
    args.set("query", platform::json::makeString(query));
    args.set("limit", platform::json::makeInt(limit));
    args.set("threshold",
             platform::json::makeDouble(kSearchSimilarityThreshold));
    const auto resp = callTool("search_memories", std::move(args), timeoutMs);
    if (resp.isFail()) {
        return core::Result<std::vector<MemoryHit>>::fail(resp.errorCode(),
                                                          resp.errorMessage());
    }
    return parseSearchResponse(resp.value());
}

// get_memories 清单（解析共用 parseListResponse，池31 提出）；正文匹配取引擎侧
// 条目 id（撤销链回填用）。
core::Result<std::string> findMemoryIdByText(const std::string& text) {
    platform::json::JsonObject args;
    args.set("limit", platform::json::makeInt(200));
    const auto resp = callTool("get_memories", std::move(args), 10000);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    auto items = parseListResponse(resp.value());
    if (items.isFail()) {
        return core::Result<std::string>::fail(items.errorCode(),
                                               items.errorMessage());
    }
    for (const MemoryListItem& item : items.value()) {
        if (memoryTextMatches(item.text, text)) {
            return core::Result<std::string>::ok(item.id);
        }
    }
    return core::Result<std::string>::ok(std::string());
}

// BUG-114-1：撤销链回填文本比对——两侧 trim 后再比。实证（2026-11-03 mac e2e，
// 字符级）：采集合段文本尾带 '\n'（session_reader extractMessageText 每块固定
// 补换行），telemem 存储侧剥尾空白入库 → 库内 275 字 vs 入参 274 字，精确比对
// 永差 1 尾字符 → target_ids 恒空 → proposal_store createUndoCard 拒绝。
bool memoryTextMatches(const std::string& stored, const std::string& collected) {
    return trimWhitespace(stored) == trimWhitespace(collected);
}

core::Result<std::string> TelememHttpRetriever::addDirect(
    const std::string& text) {
    // 入库非 hook 同步路径，放宽到 10s
    const auto resp =
        callTool("add_memory", buildAddMemoryArgs(text, false), 10000);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    const auto parsed = parseAddResponse(resp.value());
    if (parsed.isFail()) {
        return core::Result<std::string>::fail(parsed.errorCode(),
                                               parsed.errorMessage());
    }
    if (!parsed.value().empty()) {
        return parsed;  // 引擎侧直接回了 id（未来版本形态）
    }
    // telemem 1.10.1 add 只回 {"status":"stored"}（live 定案，无 id）；
    // 撤销链需要引擎条目 id → get_memories 按正文精确匹配回填（匹配不到返回
    // 空串：id 缺失时提案 targetIds 为空，如实进入契约边界记录）。
    return findMemoryIdByText(text);
}

core::Result<std::string> TelememHttpRetriever::addInferred(
    const std::string& text) {
    // 提炼语义（infer=true）：实测云端往返 4.45s 为快失败形态，成功含 LLM
    // 生成 → 30s。不做 findMemoryIdByText：提炼后正文被云端改写，按原文精确
    // 匹配拿不到产物 id（需-3 spec §3）；产物 id 走清单差分（需-3 路②，
    // infer_batch tagInferredProducts 消费）。
    const auto resp =
        callTool("add_memory", buildAddMemoryArgs(text, true), 30000);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                               resp.errorMessage());
    }
    return parseAddResponse(resp.value());
}

core::Result<void> TelememHttpRetriever::updateMemoryText(
    const std::string& memoryId, const std::string& text) {
    // update_memory(memory_id, text)：成功回 {"status":"updated",...}；
    // 判错契约与 add/delete 共用（parseAddResponse 内 parseJsonRpcResult）
    const auto resp =
        callTool("update_memory", buildUpdateMemoryArgs(memoryId, text), 10000);
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

core::Result<void> TelememHttpRetriever::setMemoryForgotten(
    const std::string& memoryId, bool forgotten) {
    // live 对账定案（2026-10-28）：
    //  - telemem 1.10.1 的 update_memory 形参 required = [memory_id, text]，
    //    只发 metadata 是工具级校验错（isError=true，live 复现），旧映射必挂；
    //  - 即便补齐 text，update 打上的 forgotten 标记也不会被 search_memories
    //    过滤（检索无 metadata 过滤缝，live 实证：标记后 search 仍返回原文）——
    //    「移出自动想起」在引擎侧唯一真实生效的通道是 delete_memory；
    //  - delete 不可逆：恢复语义（forgotten=false）由提案执行层用原文重录入
    //    实现，本接口对 false 如实返回错误。
    if (!forgotten) {
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "telemem 1.10.1 无恢复缝（delete 不可逆），恢复由提案层重录入");
    }
    platform::json::JsonObject args;
    args.set("memory_id", platform::json::makeString(memoryId));
    const auto resp = callTool("delete_memory", std::move(args), 10000);
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

core::Result<std::vector<MemoryListItem>> TelememHttpRetriever::listAll(
    int limit, int timeoutMs) {
    platform::json::JsonObject args;
    args.set("limit", platform::json::makeInt(limit));
    const auto resp = callTool("get_memories", std::move(args), timeoutMs);
    if (resp.isFail()) {
        return core::Result<std::vector<MemoryListItem>>::fail(
            resp.errorCode(), resp.errorMessage());
    }
    return parseListResponse(resp.value());
}

core::Result<std::int64_t> TelememHttpRetriever::getMemoryVersion(
    const std::string& memoryId) {
    platform::json::JsonObject args;
    args.set("memory_id", platform::json::makeString(memoryId));
    const auto resp = callTool("get_memory", std::move(args), 10000);
    if (resp.isFail()) {
        return core::Result<std::int64_t>::fail(resp.errorCode(),
                                                resp.errorMessage());
    }
    return parseGetMemoryResponse(resp.value());
}

}  // namespace module::memory::gateway
