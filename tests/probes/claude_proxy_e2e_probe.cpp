// claude_proxy_e2e_probe.cpp — Claude 切换器 mac 实弹 E2E（老板 2026-11-21 问「claude 能接上去执行」）。
//
// 链路：进程内真起 ClaudeService 本地代理（127.0.0.1 随机端口）→ 模拟 claude CLI 的
// /v1/messages 请求（model 由 CLI 侧 env 决定为上游真模型名；代理只转发不改写）→ 真上游
// → 断言非流式 + 流式（SSE）两发请求。
//
// 零波及：settings.json 全程不碰（服务直构默认不接管，接管路径未注入）；DB 用 --db 传入的
// 真实库快照**副本**（shell 侧 sqlite3 .backup 生成），真实库零接触；key 不打印、不进 argv/env。
//
// 运行：claude_proxy_e2e_probe --db <dbcopy.db> [--max-tokens N] [--skip-stream]
// 退出码：0 = 全链路通；1 = 链路失败；2 = 环境不满足（无激活 profile / 密钥不可用 / 用法错）；
//         77 = 实弹未准备（--db 快照不存在；ctest 77 跳过口径，probe::kSkipExitCode 同款）。

#include "core/database.h"
#include "core/result.h"
#include "module_claude_proxy/claude_service.h"
#include "platform/http_client.h"
#include "platform/json.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

int fail(int code, const std::string& layer, const std::string& detail) {
    std::cout << "FAIL [" << layer << "] " << detail << "\n";
    return code;
}

JsonObject parseObject(const std::string& text, const std::string& layer) {
    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        fail(1, layer, "响应 JSON 解析失败: " + parsed.errorMessage());
        return JsonObject();
    }
    if (!parsed.value().isObject()) {
        fail(1, layer, "响应不是 JSON 对象");
        return JsonObject();
    }
    return parsed.value().asObject();
}

// 非流式：POST /v1/messages（stream=false）→ 200 + content[0].text 非空
int runNonStream(int port, int maxTokens, const std::string& requestModel) {
    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/messages";
    req.headers["content-type"] = "application/json";
    req.headers["anthropic-version"] = "2023-06-01";
    req.headers["x-api-key"] = "PROXY_MANAGED";
    req.headers["authorization"] = "Bearer PROXY_MANAGED";
    req.body = "{\"model\":\"" + requestModel + "\",\"max_tokens\":" +
               std::to_string(maxTokens) +
               ",\"stream\":false,\"messages\":[{\"role\":\"user\","
               "\"content\":\"只回复两个字：成功\"}]}";
    req.timeoutMs = 60000;

    const auto t0 = std::chrono::steady_clock::now();
    auto r = platform::http::send(req);
    if (r.isFail()) {
        return fail(1, "非流式", "代理请求失败: " + r.errorMessage());
    }
    platform::http::Response resp = std::move(r).value();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (resp.statusCode != 200) {
        std::cout << "上游响应体: " << resp.body.substr(0, 500) << "\n";
        return fail(1, "非流式", "状态码 " + std::to_string(resp.statusCode) +
                                     "（期望 200）");
    }

    JsonObject root = parseObject(resp.body, "非流式");
    if (!root.has("content")) {
        return fail(1, "非流式", "响应缺 content 字段");
    }
    const JsonValue content = root.get("content");
    if (!content.isArray() || content.size() <= 0) {
        return fail(1, "非流式", "content 为空/非数组");
    }
    std::string text;
    for (int i = 0; i < content.size(); ++i) {
        const JsonValue block = content[i];
        if (block.isObject() && block.asObject().has("text")) {
            text += block.asObject().get("text").asString();
        }
    }
    if (text.empty()) {
        return fail(1, "非流式", "content 文本为空");
    }
    std::cout << "PASS [非流式] status=200 耗时=" << ms << "ms 上游模型="
              << (root.has("model") && root.get("model").isString()
                      ? root.get("model").asString()
                      : "<缺>")
              << " 回复=" << text.substr(0, 80) << "\n";
    return 0;
}

// 流式：POST /v1/messages（stream=true）→ 200 + text/event-stream + 收到 message_stop
// （claude CLI 实际走的就是流式，这是「接上去执行」最贴近真实会话的形态）
int runStream(int port, int maxTokens, const std::string& requestModel) {
    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/messages";
    req.headers["content-type"] = "application/json";
    req.headers["anthropic-version"] = "2023-06-01";
    req.headers["authorization"] = "Bearer PROXY_MANAGED";
    req.body = "{\"model\":\"" + requestModel + "\",\"max_tokens\":" +
               std::to_string(maxTokens) +
               ",\"stream\":true,\"messages\":[{\"role\":\"user\","
               "\"content\":\"从 1 数到 3，只回数字\"}]}";
    req.timeoutMs = 60000;

    auto r = platform::http::sendStreaming(req);
    if (r.isFail()) {
        return fail(1, "流式", "sendStreaming 启动失败: " + r.errorMessage());
    }
    std::unique_ptr<platform::http::StreamingResponse> stream =
        std::move(r.value());
    if (!stream->waitForStatus(15000)) {
        return fail(1, "流式", "15s 内未拿到上游响应头");
    }
    if (stream->statusCode() != 200) {
        return fail(1, "流式", "状态码 " +
                                   std::to_string(stream->statusCode()) +
                                   "（期望 200）content-type=" +
                                   stream->contentType());
    }
    // contentType() 由 header 回调填；上游未显式给时代理会回落 text/event-stream，
    // 但客户端侧可能在首 chunk 前就读不到——不以此做硬判据，改由 SSE 事件断言兜底。

    std::string acc;
    int chunks = 0;
    while (true) {
        std::string chunk;
        std::string err;
        const auto cr = stream->readChunk(chunk, err);
        if (cr == platform::http::ChunkResult::Error) {
            return fail(1, "流式", "读 chunk 失败: " + err);
        }
        if (cr == platform::http::ChunkResult::End) {
            break;
        }
        acc += chunk;
        ++chunks;
        if (acc.find("message_stop") != std::string::npos) {
            break;
        }
        if (acc.size() > 256 * 1024) {
            return fail(1, "流式", "收流超 256KB 未见到 message_stop，中止");
        }
    }
    const bool sawStart = acc.find("message_start") != std::string::npos;
    const bool sawDelta = acc.find("content_block_delta") != std::string::npos;
    const bool sawStop = acc.find("message_stop") != std::string::npos;
    if (!sawStart || !sawDelta || !sawStop) {
        return fail(1, "流式",
                    "SSE 事件缺失（start=" + std::to_string(sawStart) +
                        " delta=" + std::to_string(sawDelta) +
                        " stop=" + std::to_string(sawStop) + "）chunks=" +
                        std::to_string(chunks));
    }
    std::cout << "PASS [流式] status=200 SSE 事件齐全（start/delta/stop）chunks="
              << chunks << " 字节=" << acc.size() << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string dbPath;
    int maxTokens = 32;
    bool skipStream = false;
    // 可选：不用库里既有激活 profile，改在副本库内建一个临时 profile。
    // key 只从环境变量读（不进 argv / 不落日志），副本库在 /tmp、用完即删。
    std::string baseUrlOverride;
    std::string modelOverride;
    std::string authOverride;
    std::string keyEnvName;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) {
            dbPath = argv[++i];
        } else if (a == "--max-tokens" && i + 1 < argc) {
            maxTokens = std::atoi(argv[++i]);
        } else if (a == "--skip-stream") {
            skipStream = true;
        } else if (a == "--base-url" && i + 1 < argc) {
            baseUrlOverride = argv[++i];
        } else if (a == "--model" && i + 1 < argc) {
            modelOverride = argv[++i];
        } else if (a == "--auth" && i + 1 < argc) {
            authOverride = argv[++i];
        } else if (a == "--key-env" && i + 1 < argc) {
            keyEnvName = argv[++i];
        }
    }
    if (dbPath.empty() || maxTokens <= 0) {
        std::cout << "用法: claude_proxy_e2e_probe --db <dbcopy.db> "
                     "[--max-tokens N] [--skip-stream]\n"
                     "       [--base-url URL --model M --auth both|bearer|api_key "
                     "--key-env ENVNAME]\n";
        return 2;
    }
    if (!keyEnvName.empty() && baseUrlOverride.empty()) {
        return fail(2, "用法", "--key-env 需同时给 --base-url");
    }

    // 实弹环境门：--db 快照副本不存在 = 实弹未准备（ctest 沙箱常态），按 probe
    // 跳过口径 77 退出（tests/CMakeLists.txt 已把 77 登记为 Skipped，实弹禁
    // 重跑口径）；快照存在但打不开才是真环境失败（2）。
    if (!std::filesystem::exists(dbPath)) {
        std::cout << "SKIP [环境] DB 快照副本不存在（实弹未准备）: " << dbPath
                  << "\n";
        return 77;
    }

    // 1. 打开真实库快照副本（迁移过的 schema，直接 open）
    auto opened = core::Database::open(dbPath);
    if (opened.isFail()) {
        return fail(2, "环境", "打不开 DB 副本 " + dbPath + ": " +
                                   opened.errorMessage());
    }
    core::Database db = std::move(opened.value());
    module::claude::ClaudeService svc(db.db());

    // 2. 取激活 profile。--base-url 给了则在副本库内建/更新一个临时 profile
    if (!baseUrlOverride.empty()) {
        const char* raw = keyEnvName.empty() ? nullptr : std::getenv(keyEnvName.c_str());
        if (raw == nullptr || *raw == '\0') {
            return fail(2, "环境", "环境变量 " + keyEnvName + " 为空/未设置");
        }
        const std::string model =
            modelOverride.empty() ? "claude-sonnet-5" : modelOverride;
        const std::string auth = authOverride.empty() ? "both" : authOverride;
        auto created = svc.createProfile("e2e-temp", baseUrlOverride, raw, model,
                                         /*activateIfNoneActive=*/true, auth);
        if (created.isFail()) {
            return fail(2, "环境", "建临时 profile 失败: " + created.errorMessage());
        }
        auto activated = svc.setActiveProfile(created.value().id);
        if (activated.isFail()) {
            return fail(2, "环境", "激活临时 profile 失败: " + activated.errorMessage());
        }
        // 6 槽位模型映射：模型名改写发生在 claude CLI 侧（settings.json 的
        // ANTHROPIC_DEFAULT_*_MODEL 由 buildProxyEnv 从这些槽位生成），代理只转发
        // 不改写 model 字段。故此探针直接发上游真模型名（= requestModel）。
        module::claude::Profile p = created.value();
        p.modelsConfigJson =
            "{\"default\":{\"model\":\"" + model + "\",\"name\":\"\"},"
            "\"sonnet\":{\"model\":\"" + model + "\",\"name\":\"" + model + "\"},"
            "\"opus\":{\"model\":\"" + model + "\",\"name\":\"" + model + "\"},"
            "\"haiku\":{\"model\":\"" + model + "\",\"name\":\"" + model + "\"},"
            "\"fable\":{\"model\":\"" + model + "\",\"name\":\"" + model + "\"},"
            "\"subagent\":{\"model\":\"" + model + "\",\"name\":\"\"}}";
        p.model = model;
        auto updated = svc.updateProfile(p);
        if (updated.isFail()) {
            return fail(2, "环境", "写 models_config 失败: " + updated.errorMessage());
        }
        std::cout << "临时 profile 已建（key 来自环境变量 " << keyEnvName
                  << "，副本库内，不外泄）\n";
    }

    // 2. 取激活 profile + 原生链解密 key（mac = 0600 明文；Windows DPAPI 密文在此明确报错）
    auto active = svc.getActiveProfile();
    if (active.isFail()) {
        return fail(2, "环境", "副本库无激活 profile: " + active.errorMessage());
    }
    const module::claude::Profile profile = active.value();
    auto key = svc.loadApiKey(profile.id);
    if (key.isFail()) {
        return fail(2, "环境",
                    "key 不可用（本机解不了 = 双机同步密文，需本机重录）: " +
                        key.errorMessage());
    }
    std::cout << "profile: id=" << profile.id << " name=" << profile.name
              << " base=" << profile.baseUrl << " auth=" << profile.authField
              << " model=" << profile.model << " keyLen=" << key.value().size()
              << "\n";

    // 3. 起本地代理（port=0 → OS 分配；接管路径未注入 → settings.json 零波及）
    auto started = svc.startProxy("127.0.0.1", 0);
    if (started.isFail()) {
        return fail(1, "代理启动", started.errorMessage());
    }
    const int port = started.value();
    std::cout << "proxy: 127.0.0.1:" << port << "\n";

    // CLI 侧经 env 拿到的上游真模型名（此处直接用它发请求，模拟 CLI 的最终形态）
    const std::string requestModel =
        modelOverride.empty() ? profile.model : modelOverride;
    if (requestModel.empty()) {
        return fail(2, "环境", "无法确定请求模型名（--model 未给且 profile 无 model）");
    }

    int rc = runNonStream(port, maxTokens, requestModel);
    if (rc == 0 && !skipStream) {
        rc = runStream(port, maxTokens, requestModel);
    }

    // 4. 代理事件槽：最后一条终态应为 200（0 = 上游不可达口径）
    if (rc == 0) {
        const auto ev = svc.takeProxyEvent();
        std::cout << "proxy 事件槽终态: "
                  << (ev.has_value() ? std::to_string(*ev) : "<空>") << "\n";
    }

    svc.stopProxy();
    if (rc == 0) {
        std::cout << "E2E 全链路 PASS（claude 形态请求 → 本地代理 → ark 上游）\n";
    }
    return rc;
}
