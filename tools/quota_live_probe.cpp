// tools/quota_live_probe.cpp
// 一次性真机出网验收探针（NOT 产品代码，NOT 常规测试，两头都不许动它）。
//
// 存在的唯一理由：常规用例全走**注入的假 seam**（HttpSendFn 返回假 Response），
// 锁的是结构契约（canonical 串怎么拼、401/403 怎么分、百分比怎么反转），
// 证明不了真实服务端会不会接受。本探针走生产代码路径——`QuotaProvider::fetch()`
// + 真实 `platform::http::send`——对四家各发一次真请求。
//
// **绝不进默认门禁**：不进 tests/CMakeLists.txt 的 add_executable / add_test。
// 三个硬理由：① 真网络，CI/离线环境必红且与代码改动无关；② 需要用户本机
// ~/.cc-switch/cc-switch.db，别的机器上没有凭据可跑；③ 每次跑实打实消耗额度。
// 不要为了「让它能被跑到」把它挂进 ctest——那三条理由一条都没消失。
// 唯一入口：`bash scripts/run-quota-live-probe.sh [provider_id ...] [--dump <路径>]`
// （或 `cd build && ctest -L live`）。手敲编译行需要 yyjson 的 .o 与 openssl
// 路径，是平台特定的，脚本已经把它们兜住。
//
// 凭据红线（本文件是全项目最容易越界的地方）：
//   · 只**读** cc-switch 库（listCcSwitchProviders 内部即 SQLITE_OPEN_READONLY）。
//   · 凭据只在内存里从 cc-switch 过到 fetch() 一手，永不落盘、永不进日志。
//   · 打印一律走 redact()：凭据的任意 ≥8 字符子串替换成 ****；响应体只打前 N
//     字节，防上游把凭据回显进 body。本文件不含任何真实密钥。
//
// 退出码：0 = 每家都拿到可判定的结论（成功或**分类正确**的失败）；
//         1 = 有 provider 完全不可达 / 分类可疑 / dump 落盘失败，需要人看。
//         分类「是否正确」退出码不表达，由人判。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/logging.h"
#include "platform/json.h"
#include "quota/ccswitch_source.h"
#include "quota/quota_error_text.h"
#include "quota/quota_provider.h"
#include "quota/quota_registry.h"
#include "quota/quota_store.h"
#include "platform/http_client.h"

namespace q = module::claude::quota;

namespace {

// 从 cc-switch 读到的**全部**敏感串登记进来；打印前逐个把 ≥8 字符的子串
// 替换成 ****。按子串而非整串匹配，是因为响应体 / 错误文案里常出现「key 前 4 位」
// 这类截断回显——整串匹配漏掉，子串匹配盖得住。
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

// 预览字节数：默认 400；G8_BODY_PREVIEW=<n> 可临时调大（只影响本次调查的
// 终端输出，与门禁无关）。上限 20000，防手滑把整个响应体倒进终端历史。
std::size_t bodyPreviewLimit() {
    const char* env = std::getenv("G8_BODY_PREVIEW");
    const long n = env != nullptr ? std::strtol(env, nullptr, 10) : 400;
    if (n <= 0) return 400;
    return static_cast<std::size_t>(n > 20000 ? 20000 : n);
}
// 截断长度取自 bodyPreviewLimit()，而「已截断」的判定取自本常量——两者各管一段，
// 不要合并成一个：调大 G8_BODY_PREVIEW 时该标记不再出现是预期行为。
constexpr std::size_t kBodyPreviewBytes = 400;

std::string bodyPreview(const std::string& body) {
    std::string head = body.substr(0, bodyPreviewLimit());
    const bool truncated = body.size() > kBodyPreviewBytes;
    return redact(head) + (truncated ? "…[截断，总长 " + std::to_string(body.size()) +
                                           " 字节]" : "");
}

const char* errorName(q::QuotaError e) {
    // 刻意不复用 quota_store.h 的 quotaErrorName：那份是**落库列文本**（小写），
    // 这里给日志看。合成一份会把「日志用的名字」与「列里的名字」混成一个。
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

// 包装**真实** send：先调生产的 platform::http::send（真网络），再把观测到的
// 状态码 / body 记下来。不改 provider 代码 —— provider 只看到同一个 HttpSendFn。
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

// --dump：把本次真机查回来的结果落成 JSON，让首页余量卡被**真数据**点验过
// （本探针只验 provider 层，UI 呈现层不在此验证）。落盘的 JSON 是下游
// home_probes 的 `--quota-fixture <同一份 JSON>` 输入，那边走真 QuotaStore /
// 真桥 / 真绘制。
//
// 凭据红线在落盘口同样成立：dump 的字段**只有**
//   providerId / displayName / ccswitchName / kind / error / errorMessage /
//   currency / planName / balanceRemaining / windows[key,usedPercent,resetsAt]
// —— apiKey / accessKeyId / secretAccessKey / base_url **一概不进**。
// errorMessage / planName 另走 redact()（上游可能把凭据回显进错误文案）。
//
// 枚举落的是**列文本**（quotaErrorName / quotaKindName，quota_store.h 的单点
// 映射），不是自造字符串：下游拿同一套函数反解，两边不可能各写一份映射。
//
// 返回 false = 没能落成（文件打不开 / JSON 不合法 / 往返自检不过），main 据此
// 把退出码抬成 1 —— 落图链路不许静默半通。
struct DumpRow {
    std::string providerId;
    std::string displayName;
    std::string ccswitchName;   // cc-switch 里的条目名 = 下游该建的 profile 名
    q::QuotaResult result;
};

// 转义交给平台自己的序列化器：手搓一份转义表迟早漏字符（NUL / 代理对 / DEL），
// 而漏掉的表现是「JSON 静默解析错」——那正是本工具最不能出的错。
std::string jsonQuote(const std::string& s) {
    const core::Result<std::string> r =
        platform::json::stringify(platform::json::makeString(s));
    if (r.isFail()) return "\"\"";
    return r.value();
}

bool writeDump(const std::string& path, const std::vector<DumpRow>& rows) {
    // 数组 / 对象容器手拼（platform::json 没有「往数组里塞对象」的公开 API），
    // 但**所有标量都走 jsonQuote / stringify**，转义仍是平台那份。
    std::string text = "{\"schema\":\"devdesk-quota-live-dump/1\"";
    text += ",\"fetchedAt\":" +
            std::to_string(static_cast<long long>(std::time(nullptr)));
    text += ",\"providers\":[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const DumpRow& r = rows[i];
        if (i > 0) text += ",";
        text += "{\"providerId\":" + jsonQuote(r.providerId);
        text += ",\"displayName\":" + jsonQuote(r.displayName);
        text += ",\"ccswitchName\":" + jsonQuote(r.ccswitchName);
        text += ",\"kind\":" + jsonQuote(q::quotaKindName(r.result.kind));
        text += ",\"error\":" + jsonQuote(q::quotaErrorName(r.result.error));
        text += ",\"errorMessage\":" + jsonQuote(redact(r.result.errorMessage));
        text += ",\"currency\":" + jsonQuote(r.result.currency);
        text += ",\"planName\":" + jsonQuote(redact(r.result.planName));
        text += ",\"balanceRemaining\":" + std::to_string(r.result.balanceRemaining);
        text += ",\"windows\":[";
        for (std::size_t j = 0; j < r.result.windows.size(); ++j) {
            const q::QuotaWindow& w = r.result.windows[j];
            if (j > 0) text += ",";
            text += "{\"key\":" + jsonQuote(w.key);
            text += ",\"usedPercent\":" + std::to_string(w.usedPercent);
            text += ",\"resetsAt\":" + jsonQuote(w.resetsAt) + "}";
        }
        text += "]}";
    }
    text += "]}";

    // 往返自检：写完立刻用平台的 parse 把同一段文本解回来，元素数必须与写出一致。
    // 去掉它，一个坏 JSON 会被下游（home_probes）当成「0 条真数据」静默渲染成空卡片。
    const core::Result<platform::json::JsonValue> parsed =
        platform::json::parse(text);
    if (parsed.isFail()) {
        std::cerr << "quota_live_probe: dump JSON 自检解析失败: "
                  << parsed.errorMessage() << "\n";
        return false;
    }
    const platform::json::JsonValue arr =
        parsed.value().asObject().get("providers");
    if (!arr.isArray() || static_cast<std::size_t>(arr.size()) != rows.size()) {
        std::cerr << "quota_live_probe: dump JSON 往返元素数不符（写出 "
                  << rows.size() << " 读回 "
                  << (arr.isArray() ? arr.size() : -1) << "）\n";
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "quota_live_probe: dump 文件打不开: " << path << "\n";
        return false;
    }
    out << text;
    out.close();
    if (!out) {
        std::cerr << "quota_live_probe: dump 写入失败: " << path << "\n";
        return false;
    }
    std::cout << "\n== dump ==\n  path      = " << path
              << "\n  providers = " << arr.size() << "\n";
    return true;
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
    std::string dumpPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--all") continue;
        if (arg == "--dump" && i + 1 < argc) {
            dumpPath = argv[++i];
            continue;
        }
        wanted.push_back(arg);
    }
    if (wanted.empty()) {
        // 注册表里的全部 provider（表序 = 行序，不含哨兵）。
        // 不能写 `for (auto& e : kQuotaProviders)`：头里那行是
        // `extern const QuotaProviderEntry kQuotaProviders[]`——**边界未知**的数组，
        // 范围 for 会因「不完整类型」编译失败。长度只有定义所在的 quota_registry.cpp
        // 拿得到，即 quotaProviderTableLength()。
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
    std::vector<DumpRow> dumpRows;
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
        // matchesBaseUrl 与注册表 id 的一致性：这条不一致时 UI 侧自动选中会挑错 provider
        std::cout << "    matchesBaseUrl(cfg.baseUrl) = "
                  << (provider->matchesBaseUrl(t.info->baseUrl) ? "true" : "false")
                  << "\n";

        // ProviderConfig 装配必须与 ProfileCredentialSource::resolveAndFetch 同形
        // （profile 密钥 + cc-switch AK/SK 覆盖）——两边漂移了本探针就验不到生产路径。
        // 这里手搭是因为探针不建库 / 不走 ProfileRepository。
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
        if (!dumpPath.empty()) {
            DumpRow dr;
            dr.providerId = t.providerId;
            dr.displayName = provider->displayName();
            dr.ccswitchName = t.info->name;
            dr.result = result;
            dumpRows.push_back(std::move(dr));
        }
    }

    // dump 失败也算失败（退出码 1）：本探针的交付物是「真数据落图」这条链，
    // 链子上任何一环半通都必须显式红，而不是留一张看着挺像的图。
    const bool dumpOk = dumpPath.empty() || writeDump(dumpPath, dumpRows);

    if (!problems.empty()) {
        std::cout << "\n== 对应失败 ==\n";
        for (const std::string& p : problems) std::cout << "  " << p << "\n";
    }
    std::cout << "\n== 汇总 ==\n";
    std::cout << "  请求成功发出: " << targets.size() << " / 请求 " << wanted.size() << "\n";
    std::cout << "  完全不可达: " << unreachable << "\n";
    if (!dumpPath.empty()) {
        std::cout << "  dump: " << (dumpOk ? "已落盘 " + dumpPath : "❌ 失败")
                  << "\n";
    }
    std::cout << "  （退出码只反映「能不能拿到可判定结论」，分类是否**正确**由人判）\n";
    return unreachable == 0 && problems.empty() && dumpOk ? 0 : 1;
}
