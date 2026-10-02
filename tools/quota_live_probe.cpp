// tools/quota_live_probe.cpp
// ⚠⚠⚠ **一次性真机出网验收探针（NOT 产品代码，NOT 常规测试）** ⚠⚠⚠
//
// 存在的唯一理由：G0-G7 的全部用例都走**注入的假 seam**（HttpSendFn 返回假
// Response），它们锁的是**结构契约**（canonical 串怎么拼、401/403 怎么分、
// 百分比怎么反转），证明不了**真实服务端会接受**。本探针走生产代码路径
// ——`QuotaProvider::fetch()` + 真实 `platform::http::send`——对四家各发一次
// 真请求，回答「签名 / URL / header / 解析跟真实服务端对不对得上」。
//
// ── 凭据红线（本文件是全项目最容易越界的地方之一）────────────────────
//   · 只**读** cc-switch 库（listCcSwitchProviders 内部即 SQLITE_OPEN_READONLY）。
//   · 凭据只在内存里从 cc-switch 到 fetch() 过一手，**永不落盘、永不进日志**。
//   · 打印一律走 redactSecrets()：把凭据的任意 ≥8 字符子串替换成 ****，
//     且响应体只打前 N 字节（防上游把凭据回显进 body）。
//   · 本文件**不含**任何真实密钥（fixture 也是打码的）。
//
// ── 为什么它不在 ctest 里（三个理由，都是硬理由）────────────────────
//   1. 真网络：CI / 离线环境必红，且每次红都跟代码改动无关。
//   2. 真凭据：需要用户本机 `~/.cc-switch/cc-switch.db`，别的机器上没有。
//   3. 额度消耗：每次跑都实打实打四家供应商接口。
//   故**不进** tests/CMakeLists.txt 的 add_executable（不进 add_test），
//   手动按 README 段的命令编译运行；常规门禁（build-test-run.sh / ctest）
//   完全看不到它。想跑就显式跑，跑挂了不影响任何门禁数字。
//
// ── 怎么编译（**故意不写进 CMake**：见上面三个理由）─────────────────
//   cd <repo>/build
//   clang++ -std=c++20 -g -O0 \
//     -I../src -I../src/module_claude_proxy \
//     ../tools/quota_live_probe.cpp \
//     src/module_claude_proxy/libdevdesk_module_claude_proxy.a \
//     src/platform/libdevdesk_platform.a \
//     src/core/libdevdesk_core.a \
//     -lcrypto -lcurl -lsqlite3 \
//     -o quota_live_probe
//   ./quota_live_probe          # 跑四家
//
// 用法：`quota_live_probe [provider_id ...]`（不给参数 = 跑注册表里全部四家）。
// 退出码：0 = 每家都拿到可判定的结论（成功或**分类正确**的失败）；
//         1 = 有 provider 完全不可达 / 分类可疑，需要人看。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/logging.h"
#include "quota/ccswitch_source.h"
#include "quota/quota_error_text.h"
#include "quota/quota_provider.h"
#include "quota/quota_registry.h"
#include "quota/quota_store.h"
#include "platform/http_client.h"

namespace q = module::claude::quota;

namespace {

// ── 凭据脱敏（红线兜底）──────────────────────────────────────────
// 从 cc-switch 读到的**全部**敏感串登记进来；打印前逐个把 ≥8 字符的子串
// 替换成 ****。为什么要按子串而不是整串：响应体 / 错误文案里可能出现
// 「key 前 4 位」这类截断回显，整串匹配漏掉，子串匹配能盖住。
std::vector<std::string>& secretRegistry() {
    static std::vector<std::string> secrets;
    return secrets;
}

void registerSecret(const std::string& value) {
    if (value.size() < 8) return;  // 太短的不登记（打码会把正常文本也吃掉）
    secretRegistry().push_back(value);
}

std::string redact(std::string text) {
    for (const std::string& secret : secretRegistry()) {
        const std::size_t n = secret.size();
        std::size_t pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos) {
            text.replace(pos, n, "****");
            pos += 4;
        }
        // 前 8 字符（上游常回显 key 开头）
        if (n >= 12) {
            const std::string head = secret.substr(0, 8);
            std::size_t hp = 0;
            while ((hp = text.find(head, hp)) != std::string::npos) {
                text.replace(hp, head.size(), "****");
                hp += 4;
            }
        }
    }
    return text;
}

// 响应体只打前 N 字节：既够看清结构，又防上游把整个凭据回显进 body
// 后被写进报告 / 终端历史。
// 预览字节数：默认 400；G8_BODY_PREVIEW=<n> 可临时调大（**只影响本次调查的
// 终端输出**，与门禁无关）。上限 20000，防手滑把整个响应体倒进终端历史。
std::size_t bodyPreviewLimit() {
    const char* env = std::getenv("G8_BODY_PREVIEW");
    const long n = env != nullptr ? std::strtol(env, nullptr, 10) : 400;
    if (n <= 0) return 400;
    return static_cast<std::size_t>(n > 20000 ? 20000 : n);
}
constexpr std::size_t kBodyPreviewBytes = 400;  // 默认值（文案与默认截断用它）

std::string bodyPreview(const std::string& body) {
    std::string head = body.substr(0, bodyPreviewLimit());
    const bool truncated = body.size() > kBodyPreviewBytes;
    return redact(head) + (truncated ? "…[截断，总长 " + std::to_string(body.size()) +
                                           " 字节]" : "");
}

const char* errorName(q::QuotaError e) {
    // quota_store.h 的 quotaErrorName 是**落库列文本**（小写），
    // 这里另起一份给日志看，避免把「日志用的名字」与「列里的名字」混成一个。
    switch (e) {
        case q::QuotaError::None: return "None";
        case q::QuotaError::Auth: return "Auth";
        case q::QuotaError::Entitlement: return "Entitlement";
        case q::QuotaError::Shape: return "Shape";
        case q::QuotaError::Transient: return "Transient";
        case q::QuotaError::NotConfigured: return "NotConfigured";
    }
    return "?";
}

// ── 记录一次 HTTP 往返（探针自己观察，不改生产代码）────────────────
struct HttpTrace {
    std::string method;
    std::string url;
    int statusCode = 0;
    bool networkError = false;
    std::string body;
    long elapsedMs = 0;
    std::string curlError;
};

// **包装**真实 send：先调生产的 platform::http::send（真网络），再把观测到的
// 状态码 / body 记下来。不改 provider 代码 —— provider 只看到同一个 HttpSendFn。
q::HttpSendFn tracingSend(std::vector<HttpTrace>* traces) {
    return [traces](const platform::http::Request& req) {
        HttpTrace trace;
        trace.method = req.method;
        trace.url = req.url;
        const core::Result<platform::http::Response> sent = platform::http::send(req);
        if (sent.isOk()) {
            trace.statusCode = sent.value().statusCode;
            trace.networkError = sent.value().networkError;
            trace.body = sent.value().body;
            trace.elapsedMs = sent.value().elapsedMs;
            if (sent.value().networkError) trace.curlError = sent.value().errorMessage;
        } else {
            trace.curlError = sent.errorMessage();
        }
        traces->push_back(trace);
        return sent;
    };
}

void printHttpTraces(const std::vector<HttpTrace>& traces) {
    for (const HttpTrace& t : traces) {
        std::cout << "    HTTP " << t.method << " " << t.url << "\n";
        if (!t.curlError.empty()) {
            std::cout << "      transport-error: " << redact(t.curlError) << "\n";
        }
        std::cout << "      status=" << t.statusCode
                  << " networkError=" << (t.networkError ? "true" : "false")
                  << " elapsedMs=" << t.elapsedMs << "\n";
        std::cout << "      body[0.." << kBodyPreviewBytes << "]= " << bodyPreview(t.body)
                  << "\n";
    }
}

void printResult(const q::QuotaResult& r) {
    std::cout << "    QuotaError = " << errorName(r.error) << "\n";
    std::cout << "    kind       = "
              << (r.kind == q::QuotaKind::Balance ? "Balance" : "Tiered") << "\n";
    if (r.error != q::QuotaError::None) {
        std::cout << "    message    = " << redact(r.errorMessage) << "\n";
        std::cout << "    用户文案   = "
                  << redact(q::quotaErrorHeadline(r.error, r.errorMessage)) << "\n";
        return;
    }
    if (r.balanceRemaining != 0.0 || !r.currency.empty()) {
        std::cout << "    balance    = " << r.balanceRemaining << " " << r.currency << "\n";
    }
    if (!r.planName.empty()) std::cout << "    planName   = " << redact(r.planName) << "\n";
    for (const q::QuotaWindow& w : r.windows) {
        std::cout << "    window     = " << w.key << " used=" << w.usedPercent << "%"
                  << " resetsAt=" << (w.resetsAt.empty() ? "(无)" : w.resetsAt) << "\n";
    }
}

// cc-switch 的哪一条对应哪个 provider id：靠 usage_script.codingPlanProvider
// （cc-switch 侧的判定，与我们的注册表 id 同名约定），缺则回落按 base_url 探测。
// **只匹配，不猜**：匹配不上的 provider 报「无法对应」，让上层看到缺口，
// 而不是随便挑一条把别家的凭据发出去。
struct Target {
    std::string providerId;
    const q::CcSwitchProviderInfo* info = nullptr;
};

bool buildTargets(const q::CcSwitchProviderList& list,
                  const std::vector<std::string>& wanted, std::vector<Target>* out,
                  std::vector<std::string>* problems) {
    for (const std::string& id : wanted) {
        Target t;
        t.providerId = id;
        for (const q::CcSwitchProviderInfo& info : list.providers) {
            bool hit = false;
            if (info.codingPlanProvider == id) {
                hit = true;
            } else {
                const q::QuotaProvider* p = q::findProvider(id);
                if (p != nullptr && p->matchesBaseUrl(info.baseUrl)) hit = true;
            }
            if (hit) {
                t.info = &info;
                break;
            }
        }
        if (t.info == nullptr) {
            problems->push_back(id + "：cc-switch 里找不到对应条目（不会猜着发）");
            continue;
        }
        // 凭据登记进脱敏表（**只登记，不打印**）
        registerSecret(t.info->apiKey);
        registerSecret(t.info->accessKeyId);
        registerSecret(t.info->secretAccessKey);
        out->push_back(t);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    core::log::setLevel(core::log::Level::Warn);  // 不打 Debug：cc-switch 读取路径会打 provider id

    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--all") continue;
        wanted.push_back(arg);
    }
    if (wanted.empty()) {
        // 注册表里的全部 provider（表序 = 行序，不含哨兵）。
        // ⚠ 不能写 `for (auto& e : kQuotaProviders)`：头里那行是
        // `extern const QuotaProviderEntry kQuotaProviders[]`——**边界未知**的数组，
        // 范围 for 会因「不完整类型」编译失败（quota_registry.h:41-52 记着同一条）。
        // 长度只有定义所在的 quota_registry.cpp 拿得到，即 quotaProviderTableLength()。
        const std::size_t tableLength = q::quotaProviderTableLength();
        for (std::size_t i = 0; i < tableLength; ++i) {
            if (q::kQuotaProviders[i].id == nullptr) break;  // 哨兵
            wanted.emplace_back(q::kQuotaProviders[i].id);
        }
    }

    const q::CcSwitchProviderList list = q::listCcSwitchProviders();
    std::cout << "== cc-switch 只读源 ==\n";
    std::cout << "  dbPath   = " << q::ccSwitchDbPath() << "\n";
    std::cout << "  status   = " << static_cast<int>(list.status)
              << "  providers=" << list.providers.size() << "\n";
    if (list.status != q::CcSwitchSourceStatus::Ok) {
        std::cout << "  reason   = " << redact(list.unavailableReason) << "\n";
        std::cout << "\n❌ cc-switch 来源不可用，无法出网验收。\n";
        return 1;
    }

    std::vector<Target> targets;
    std::vector<std::string> problems;
    buildTargets(list, wanted, &targets, &problems);

    std::cout << "\n== 真机出网（真实 platform::http::send）==\n";
    int unreachable = 0;
    for (const Target& t : targets) {
        std::cout << "\n-- " << t.providerId << "  (cc-switch: " << t.info->name
                  << ", base_url=" << t.info->baseUrl << ")\n";

        const q::QuotaProvider* provider = q::findProvider(t.providerId);
        if (provider == nullptr) {
            std::cout << "    ❌ 注册表里没有该 provider\n";
            ++unreachable;
            continue;
        }
        std::cout << "    displayName = " << provider->displayName() << "\n";
        // matchesBaseUrl 与注册表 id 的一致性（G6 自动选中靠它）
        std::cout << "    matchesBaseUrl(cfg.baseUrl) = "
                  << (provider->matchesBaseUrl(t.info->baseUrl) ? "true" : "false")
                  << "\n";

        // ProviderConfig 装配：与 ProfileCredentialSource::resolveAndFetch
        // **同形**（profile 密钥 + cc-switch AK/SK 覆盖）。这里手搭是因为
        // 探针不建库 / 不走 ProfileRepository（那是真机 UI 的事）。
        q::ProviderConfig cfg;
        cfg.providerId = t.providerId;
        cfg.baseUrl = t.info->baseUrl;
        cfg.apiKey = t.info->apiKey;
        if (!t.info->accessKeyId.empty()) cfg.fields["accessKeyId"] = t.info->accessKeyId;
        if (!t.info->secretAccessKey.empty()) {
            cfg.fields["secretAccessKey"] = t.info->secretAccessKey;
        }

        std::vector<HttpTrace> traces;
        const q::HttpSendFn http = tracingSend(&traces);
        const q::QuotaResult result = provider->fetch(cfg, http);

        printHttpTraces(traces);
        printResult(result);
        if (traces.empty()) {
            std::cout << "    ⚠ 一次请求都没发出去（provider 在发请求前就返回了）\n";
        }
    }

    if (!problems.empty()) {
        std::cout << "\n== 对应失败 ==\n";
        for (const std::string& p : problems) std::cout << "  " << p << "\n";
    }
    std::cout << "\n== 汇总 ==\n";
    std::cout << "  请求成功发出: " << targets.size() << " / 请求 " << wanted.size() << "\n";
    std::cout << "  完全不可达: " << unreachable << "\n";
    std::cout << "  （退出码只反映「能不能拿到可判定结论」，分类是否**正确**由人判）\n";
    return unreachable == 0 && problems.empty() ? 0 : 1;
}
