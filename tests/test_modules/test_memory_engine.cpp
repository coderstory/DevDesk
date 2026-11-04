// tests/test_modules/test_memory_engine.cpp
// 记忆引擎托管纯逻辑单测（S2 批次④）。全部注入假探针/假响应：不启真引擎、
// 不碰网络、不装 pip 包（真引擎验证归 live/UAT 层）。
// 契约红线（改动即契约变更）：
//   - Python 合格区间 3.10~3.13（3.14+ 探测步拦截，不放行到 pip；
//     bug157 实测 telemem 1.10.1 兼容 3.13 后放宽）
//   - TELEMEM_CONFIG 恒不含密钥；key 只进子进程环境变量
//   - 健康判据查嵌套 error（body.error / body.result.error），不能只看传输层
#include "doctest.h"

#include "test_support.h"

#include "module_memory/engine/llama_host.h"
#include "module_memory/engine/python_locator.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/engine/telemem_process.h"
#include "module_memory/engine/token_usage.h"
#include "platform/exec_capture.h"
#include "platform/process_host.h"
#include "core/module_interface.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <eui/dsl.h>

extern "C" ::core::Module* create_memory_module();

using namespace module::memory::engine;

namespace {

bool contains(const std::vector<std::string>& v, const std::string& item) {
    for (const std::string& s : v) {
        if (s == item) return true;
    }
    return false;
}

} // namespace

// ── Python 版本判定 ──

REGISTER_TEST(memory_python_version_parse,
              "memory: Python 版本串提取（取首个 3.X[Y.Z] 段；杂讯/无版本回空）", {
    DOCTEST_CHECK_EQ(parsePythonVersionOutput("3.12.4\n"), std::string("3.12.4"));
    DOCTEST_CHECK_EQ(parsePythonVersionOutput("Python 3.10.0 (tags/v3.10.0)\n"),
                     std::string("3.10.0"));
    DOCTEST_CHECK_EQ(parsePythonVersionOutput(""), std::string(""));
    DOCTEST_CHECK_EQ(parsePythonVersionOutput("not a version"),
                     std::string(""));
    return true;
})

REGISTER_TEST(memory_python_version_qualified,
              "memory: Python 合格区间 3.10~3.13（3.14+/3.9- 探测步拦截）", {
    DOCTEST_CHECK(pythonVersionQualified("3.10.0"));
    DOCTEST_CHECK(pythonVersionQualified("3.11.9"));
    DOCTEST_CHECK(pythonVersionQualified("3.12.4"));
    DOCTEST_CHECK(!pythonVersionQualified("3.9.7"));
    DOCTEST_CHECK(pythonVersionQualified("3.13.1"));  // bug157 放宽
    DOCTEST_CHECK(!pythonVersionQualified("3.14.0"));
    DOCTEST_CHECK(!pythonVersionQualified(""));
    DOCTEST_CHECK(!pythonVersionQualified("garbage"));
    return true;
})

REGISTER_TEST(memory_python_locator_rejects_unqualified,
              "memory: 探测器注入假探针——全候选 3.14 全拒（不起真进程）", {
    auto probe = [](const std::string&) { return std::string("3.14.0\n"); };
    DOCTEST_CHECK(!findQualifiedPython(probe).has_value());
    auto deadProbe = [](const std::string&) { return std::string(); };
    DOCTEST_CHECK(!findQualifiedPython(deadProbe).has_value());
    return true;
})

REGISTER_TEST(memory_python_locator_picks_qualified_candidate,
              "memory: 探测器按候选序选首个合格者（候选[0] 合格即中；uv 存在时必先于 PATH）", {
    // 机器无关口径：真实候选清单 + 假探针只让候选[0] 合格 → 返回者必须是候选[0]
    const std::vector<std::string> candidates = pythonCandidatePaths();
    DOCTEST_REQUIRE(!candidates.empty());
    const std::string qualified = candidates[0];
    auto probe = [&](const std::string& exe) {
        return exe == qualified ? std::string("3.12.4\n")
                                : std::string("3.14.0\n");
    };
    const auto found = findQualifiedPython(probe);
    DOCTEST_REQUIRE(found.has_value());
    DOCTEST_CHECK_EQ(found->exePath, qualified);
    DOCTEST_CHECK_EQ(found->versionText, std::string("3.12.4"));
    // uv 管的 3.12 若在本机存在，必须排在 PATH 兜底（python.exe）之前
    size_t uvIdx = candidates.size();
    size_t pathIdx = candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (uvIdx == candidates.size() &&
            candidates[i].find("cpython-3.12") != std::string::npos) {
            uvIdx = i;
        }
        if (pathIdx == candidates.size() && candidates[i] == "python.exe") {
            pathIdx = i;
        }
    }
    if (uvIdx != candidates.size() && pathIdx != candidates.size()) {
        DOCTEST_CHECK(uvIdx < pathIdx);
    }
    return true;
})

// ── TELEMEM_CONFIG 生成 ──

REGISTER_TEST(memory_config_generation_contract,
              "memory: TELEMEM_CONFIG 内容契约（anthropic/智谱端点/回环 embedding）", {
    const std::string yaml = generateTelememConfigYaml("/tmp/memdata");
    DOCTEST_CHECK(yaml.find("provider: anthropic") != std::string::npos);
    DOCTEST_CHECK(yaml.find(kLlmBaseUrl) != std::string::npos);
    DOCTEST_CHECK(yaml.find(std::string("model: ") + kLlmModel) !=
                  std::string::npos);
    // live 对账定案：字段名必须是 mem0/telemem 真实类接受的——
    // AnthropicConfig 收 anthropic_base_url、BaseEmbedderConfig 收 openai_base_url，
    // 写 base_url/api_key_env 都是 TypeError（引擎启动即崩）
    DOCTEST_CHECK(yaml.find(std::string("anthropic_base_url: ") + kLlmBaseUrl) !=
                  std::string::npos);
    DOCTEST_CHECK(yaml.find("api_key_env") == std::string::npos);
    DOCTEST_CHECK(yaml.find("\n    base_url: ") ==
                  std::string::npos);  // 行首裸 base_url 禁现（anthropic/openai_base_url 合法）
    // embedding 只绑回环 + 模型名常量；维度必须显式 512（embedder 与 faiss 各一份，
    // mem0 无联动，缺省 1536 与向量维度不符——首次 add 即炸）
    DOCTEST_CHECK(yaml.find("http://127.0.0.1:11435/v1") != std::string::npos);
    DOCTEST_CHECK(yaml.find("embedding_dims: 512") != std::string::npos);
    DOCTEST_CHECK(yaml.find("embedding_model_dims: 512") != std::string::npos);
    DOCTEST_CHECK(yaml.find(std::string("model: ") + kEmbedModel) !=
                  std::string::npos);
    // 数据收敛进 dataDir
    DOCTEST_CHECK(yaml.find("/tmp/memdata/faiss_db") != std::string::npos);
    DOCTEST_CHECK(yaml.find("/tmp/memdata/history.db") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_config_never_contains_key,
              "memory: 密钥红线——生成的配置恒不含 key（api_key 恒空，key 走环境）", {
    const char* kFakeKey = "sk-fake-abcdef0123456789";
    const std::string yaml =
        generateTelememConfigYaml(memoryDataDir());  // 与生产同源入参
    DOCTEST_CHECK(yaml.find(kFakeKey) == std::string::npos);
    DOCTEST_CHECK(yaml.find("api_key: \"\"") != std::string::npos);
    // 生成文本喂进日志脱敏后也不可能出现密钥（防御性口径自证）
    DOCTEST_CHECK(redactSecretsForLog(yaml, kFakeKey).find(kFakeKey) ==
                  std::string::npos);
    return true;
})

REGISTER_TEST(memory_key_extraction_from_settings,
              "memory: settings.json 取 key（PROXY_MANAGED 占位非凭据；坏 JSON 回空）", {
    DOCTEST_CHECK_EQ(extractLlmKeyFromSettingsJson(
                         R"({"env":{"ANTHROPIC_AUTH_TOKEN":"zk-real-1"}})"),
                     std::string("zk-real-1"));
    // AUTH_TOKEN 是托管占位 → 落到 API_KEY
    DOCTEST_CHECK_EQ(extractLlmKeyFromSettingsJson(
                         R"({"env":{"ANTHROPIC_AUTH_TOKEN":"PROXY_MANAGED",)"
                         R"("ANTHROPIC_API_KEY":"zk-second"}})"),
                     std::string("zk-second"));
    // 两个键都是占位/缺失 → 空串
    DOCTEST_CHECK_EQ(extractLlmKeyFromSettingsJson(
                         R"({"env":{"ANTHROPIC_AUTH_TOKEN":"PROXY_MANAGED"}})"),
                     std::string());
    DOCTEST_CHECK_EQ(extractLlmKeyFromSettingsJson(R"({"other":1})"),
                     std::string());
    DOCTEST_CHECK_EQ(extractLlmKeyFromSettingsJson("{not json"),
                     std::string());
    return true;
})

REGISTER_TEST(memory_redact_secrets_for_log,
              "memory: 日志脱敏——密钥原文替换 <redacted>，空密钥原样", {
    const std::string secret = "zk-secret-42";
    const std::string out =
        redactSecretsForLog("key=" + secret + "; url=/v1", secret);
    DOCTEST_CHECK(out.find(secret) == std::string::npos);
    DOCTEST_CHECK(out.find("<redacted>") != std::string::npos);
    DOCTEST_CHECK_EQ(redactSecretsForLog("no key here", ""), std::string("no key here"));
    return true;
})

// ── 进程参数构造（不起真引擎）──

REGISTER_TEST(memory_process_args_construction,
              "memory: telemem/llama 进程参数构造（回环 + 端口 + venv 解释器）", {
    const std::filesystem::path venv = "/memdata/venv";
    // venv 创建
    const auto venvArgs = buildVenvArgs("C:/Python312/python.exe", venv);
    DOCTEST_REQUIRE(venvArgs.size() == 4);
    DOCTEST_CHECK(venvArgs[0] == "C:/Python312/python.exe");
    DOCTEST_CHECK(venvArgs[1] == "-m");
    DOCTEST_CHECK(venvArgs[2] == "venv");
    // pip 首跑安装（IMP-114-3：钉版）
    const auto pipArgs = buildPipInstallArgs(venv);
    DOCTEST_CHECK(contains(pipArgs, "-m"));
    DOCTEST_CHECK(contains(pipArgs, "pip"));
    DOCTEST_CHECK(contains(pipArgs, "telemem==1.10.1"));
    // telemem 元数据不带 anthropic（live 实测 ImportError），必须显式同装
    DOCTEST_CHECK(contains(pipArgs, "anthropic"));
    // 服务拉起：只绑回环
    const auto serverArgs = buildTelememServerArgs(venv, kTelememPort);
    DOCTEST_CHECK(contains(serverArgs, "--host"));
    DOCTEST_CHECK(contains(serverArgs, "127.0.0.1"));
    DOCTEST_CHECK(contains(serverArgs, "--port"));
    DOCTEST_CHECK(contains(serverArgs, "8421"));
    DOCTEST_CHECK(contains(serverArgs, "-m"));
    // 真实入口：`python -m telemem` 无 __main__，只有 -m telemem.mcp --transport
    DOCTEST_CHECK(contains(serverArgs, "telemem.mcp"));
    DOCTEST_CHECK(contains(serverArgs, "--transport"));
    DOCTEST_CHECK(contains(serverArgs, "streamable-http"));
    // 解释器来自 venv 目录（平台布局差异只允许二选一）
    const std::string exe = venvPythonExe(venv).string();
    DOCTEST_CHECK(exe.find("venv") != std::string::npos);
    DOCTEST_CHECK(exe.find("python") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_process_env_key_channel,
              "memory: 拉起环境——TELEMEM_CONFIG 注入；key 只经环境变量、空 key 不注入", {
    const auto env = buildTelememEnv("/memdata/telemem-config.yaml", "zk-k");
    bool hasConfig = false;
    bool hasKey = false;
    for (const std::string& e : env) {
        if (e.rfind("TELEMEM_CONFIG=", 0) == 0) hasConfig = true;
        if (e.rfind(std::string(kLlmKeyEnvName) + "=", 0) == 0) hasKey = true;
    }
    DOCTEST_CHECK(hasConfig);
    DOCTEST_CHECK(hasKey);
    // 空 key：环境里不得出现空串占位（telemem 会把空串当真 key 用）
    const auto envNoKey = buildTelememEnv("/memdata/c.yaml", "");
    for (const std::string& e : envNoKey) {
        DOCTEST_CHECK(e.rfind(std::string(kLlmKeyEnvName) + "=", 0) != 0);
    }
    // G-1 回填偏差②：恒注入 NO_PROXY/no_proxy（云端 host 旁路本机代理），
    // 空 key 时环境 = TELEMEM_CONFIG + 双代理键共 3 行
    DOCTEST_CHECK(envNoKey.size() == 3);
    bool hasNoProxy = false;
    for (const std::string& e : envNoKey) {
        if (e.rfind("NO_PROXY=", 0) == 0 || e.rfind("no_proxy=", 0) == 0) {
            DOCTEST_CHECK(e.find("open.bigmodel.cn") != std::string::npos);
            hasNoProxy = true;
        }
    }
    DOCTEST_CHECK(hasNoProxy);
    return true;
})

// IMP-114-3：镜像源透传（纯函数版，不做 getenv；旗标 = PIP_INDEX_URL 官方同义通道）
REGISTER_TEST(memory_pip_mirror_flags,
              "memory: pip 参数——钉版 telemem==1.10.1；PIP_INDEX_URL/PIP_EXTRA_INDEX_URL 透传为旗标", {
    const std::filesystem::path venv = "/memdata/venv";
    // 无镜像：不加旗标
    const auto plain = buildPipInstallArgs(venv, nullptr, nullptr);
    DOCTEST_CHECK(contains(plain, "telemem==1.10.1"));
    DOCTEST_CHECK(contains(plain, "anthropic"));
    DOCTEST_CHECK(!contains(plain, "--index-url"));
    DOCTEST_CHECK(!contains(plain, "--extra-index-url"));
    // 有镜像：等价透传
    const auto mirrored = buildPipInstallArgs(venv, "https://mirrors.example/simple",
                                              "https://extra.example/simple");
    DOCTEST_CHECK(contains(mirrored, "--index-url"));
    DOCTEST_CHECK(contains(mirrored, "https://mirrors.example/simple"));
    DOCTEST_CHECK(contains(mirrored, "--extra-index-url"));
    DOCTEST_CHECK(contains(mirrored, "https://extra.example/simple"));
    // 空串 = 未设置：不加旗标
    const auto emptyVal = buildPipInstallArgs(venv, "", "");
    DOCTEST_CHECK(!contains(emptyVal, "--index-url"));
    DOCTEST_CHECK(!contains(emptyVal, "--extra-index-url"));
    return true;
})

REGISTER_TEST(memory_llama_args,
              "memory: llama-server 参数构造（回环 + 端口 + 模型 + 仅嵌入档）", {
    const auto args = buildLlamaServerArgs("/tools/llama-server.exe",
                                           "/memdata/models/m.gguf", 11435);
    DOCTEST_CHECK(contains(args, "--host"));
    DOCTEST_CHECK(contains(args, "127.0.0.1"));
    DOCTEST_CHECK(contains(args, "--port"));
    DOCTEST_CHECK(contains(args, "11435"));
    DOCTEST_CHECK(contains(args, "-m"));
    DOCTEST_CHECK(contains(args, "/memdata/models/m.gguf"));
    DOCTEST_CHECK(contains(args, "--embeddings"));
    return true;
})

// ── 健康判据（错误嵌 result 体契约）──

REGISTER_TEST(memory_health_nested_error_contract,
              "memory: 健康判据——200 且嵌套 error（result.error）必判不健康", {
    // 引擎活着且正常
    const HealthVerdict ok =
        judgeTelememHealth(200, false, R"({"result":{"content":[]}})");
    DOCTEST_CHECK(ok.healthy);
    // 契约主例：is_error 旗标缺位/为假，但错误嵌在 result 体里 → 不健康
    const HealthVerdict nested = judgeTelememHealth(
        200, false,
        R"({"result":{"isError":false,"error":{"code":-32000,"message":"config invalid"}}})");
    DOCTEST_CHECK(!nested.healthy);
    // 顶层 error（工具级结构化报错形态）
    const HealthVerdict top =
        judgeTelememHealth(200, false, R"({"error":"invalid_arguments"})");
    DOCTEST_CHECK(!top.healthy);
    return true;
})

REGISTER_TEST(memory_health_transport_and_body_guard,
              "memory: 健康判据——网络错/非 200/非 JSON 一律不健康", {
    DOCTEST_CHECK(!judgeTelememHealth(0, true, "").healthy);
    DOCTEST_CHECK(!judgeTelememHealth(503, false, "service unavailable").healthy);
    DOCTEST_CHECK(!judgeTelememHealth(200, false, "<html>hi</html>").healthy);
    DOCTEST_CHECK(!judgeTelememHealth(404, false, R"({"result":{}})").healthy);
    return true;
})

// ── 消耗计量表（结构契约：软/硬两档 + 仪表占比钳制）──

REGISTER_TEST(memory_token_usage_meter,
              "memory: token 计量表——软/硬档判定与占比钳制（硬档满刻度，异常配置不产生负宽）", {
    TokenUsageMeter m;  // 默认：零消耗
    DOCTEST_CHECK(m.phase() == TokenUsageMeter::Phase::Normal);
    DOCTEST_CHECK_EQ(m.monthRatio(), 0.0);

    m.monthTokens = m.softBudgetTokens;  // 恰达软档 → Soft（黄条）
    DOCTEST_CHECK(m.phase() == TokenUsageMeter::Phase::Soft);
    m.monthTokens = m.hardBudgetTokens;  // 恰达硬档 → Hard（暂停提炼）
    DOCTEST_CHECK(m.phase() == TokenUsageMeter::Phase::Hard);
    DOCTEST_CHECK_EQ(m.monthRatio(), 1.0);
    m.monthTokens = m.hardBudgetTokens * 3;  // 超限钳满，不超 1
    DOCTEST_CHECK_EQ(m.monthRatio(), 1.0);

    m.hardBudgetTokens = 0;  // 异常配置：占比恒 0（UI 不产生负宽/NaN）
    m.monthTokens = 100;
    DOCTEST_CHECK_EQ(m.monthRatio(), 0.0);
    return true;
})

// ── 三视图 UI 骨架（无头 compose 断言：默认主控台在场上，锚点齐全）──

REGISTER_TEST(memory_ui_skeleton_console,
              "memory: 三视图骨架——默认主控台 compose 出引擎卡/启停钮/计量条，Tab 三枚在场", {
    std::unique_ptr<::core::Module> mod(create_memory_module());
    DOCTEST_REQUIRE(mod != nullptr);
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("memory_console");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("memory.title") != nullptr);
    for (const char* id : {"memory.tabs.0.text", "memory.tabs.1.text",
                           "memory.tabs.2.text"}) {
        DOCTEST_CHECK_MESSAGE(ui.find(id) != nullptr,
                              std::string("缺 Tab 锚点: ") + id);
    }
    // U1 起引擎卡收进 eng.root 栈、启停钮合并为单 toggle、健康四格接真账面
    for (const char* id : {"memory.eng.card", "memory.eng.state",
                           "memory.eng.wizard.bg", "memory.eng.toggle.bg",
                           "memory.health.card0", "memory.health.h2.value",
                           "memory.usage.bar", "memory.recent.card",
                           "memory.feed.card"}) {
        DOCTEST_CHECK_MESSAGE(ui.find(id) != nullptr,
                              std::string("缺主控台锚点: ") + id);
    }
    return true;
})

// ── process_host（真实拉起极短命进程，非引擎；非 Windows 自动跳过）──

REGISTER_TEST(memory_process_host_spawn_and_reap,
              "memory: process_host 拉起短命进程可探活消亡；树杀幂等", {
    // Windows 用 cmd.exe 判别平台；非 Windows（macOS/Linux）本批未实机验证，跳过
    if (!platform::executableOnPath("cmd.exe")) return true;
    using platform::process_host::isAlive;
    using platform::process_host::spawnDetached;
    using platform::process_host::terminateTree;
    const auto r = spawnDetached({"cmd.exe", "/c", "exit 0"}, {}, {});
    DOCTEST_REQUIRE(r.isOk());
    const std::int64_t pid = r.value();
    DOCTEST_CHECK(pid > 0);
    // 有界等进程自然退出（≤5s，50ms 步进）
    for (int i = 0; i < 100 && isAlive(pid); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    DOCTEST_CHECK(!isAlive(pid));
    // 幂等：杀已退出进程与非法 pid 都必须 ok，不抛不挂
    DOCTEST_CHECK(terminateTree(pid).isOk());
    DOCTEST_CHECK(terminateTree(-1).isOk());
    DOCTEST_CHECK(!spawnDetached({}, {}, {}).isOk());  // 空 argv 必拒
    return true;
})

// ── need:202 F-01/F-02/F-03：llama/模型定位链 assets 序 ──

namespace {

// 写入 + 存在重试：新落盘的 .exe 名文件会被 AV/索引器短暂持锁，exists(p, ec)
// 在该窗口内带 ec 返回 false——种子不稳定会让定位链用例红绿抖动（2026-11-04 实测）
void seedFileUntilVisible(const std::filesystem::path& p,
                          const std::string& content) {
    for (int i = 0; i < 50; ++i) {
        { std::ofstream out(p, std::ios::binary); out << content; }
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void seedLlamaDir(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "llama" / "bin");
    seedFileUntilVisible(root / "llama" / "bin" / llamaServerBinaryName(), "x");
}

void seedModelDir(const std::filesystem::path& root, std::uintmax_t bytes) {
    std::filesystem::create_directories(root / "models");
    seedFileUntilVisible(root / "models" / "bge-small-zh-v1.5-q8_0.gguf",
                         std::string(static_cast<std::size_t>(bytes), 'm'));
}

} // namespace

REGISTER_TEST(memory_llama_locate_env_first,
              "memory need202 F-01: llama 定位 env 序恒首；空值/不存在静默跳过", {
    const auto assets = test_support::makeTempDir("devdesk_mem_locate_", "pkg");
    const auto data = test_support::makeTempDir("devdesk_mem_locate_", "data");
    seedLlamaDir(assets);
    seedLlamaDir(data);
    {
        test_support::ScopedEnv noEnv("DEVDESK_LLAMA_DIR", "");
        const auto hit = locateLlamaServerBetween(assets, data);
        DOCTEST_CHECK_MESSAGE(std::string(hit.source) == "pkg",
                              "env 空值须落到包内序，实测 source=" +
                                  std::string(hit.source));
    }
    {
        test_support::ScopedEnv env("DEVDESK_LLAMA_DIR", data.string());
        // 与 locateLlamaServerBetween env 序逐字同款的前置复刻（区分「环境/路径层」
        // 与「函数内部分支」：若这里 exists=1 而 source≠env，即函数内 getenv/路径
        // 解析被污染，需要查链接而非查种子）
        const char* rawChk = std::getenv("DEVDESK_LLAMA_DIR");
        const bool inlineHit =
            rawChk != nullptr && *rawChk != '\0' &&
            std::filesystem::exists(
                std::filesystem::path(rawChk) / "llama" / "bin" /
                    llamaServerBinaryName());
        DOCTEST_CHECK_MESSAGE(inlineHit, "env 序前置复刻须命中（env 通路本身可用）");
        const auto hit = locateLlamaServerBetween(assets, data);
        DOCTEST_CHECK_MESSAGE(std::string(hit.source) == "env",
                              "env 指向 data 根须命中 env 序，实测 source=" +
                                  std::string(hit.source));
        DOCTEST_CHECK(hit.exePath ==
                      data / "llama" / "bin" / llamaServerBinaryName());
    }
    {
        test_support::ScopedEnv bad("DEVDESK_LLAMA_DIR",
                                    (data / "nope").string());
        const auto hit = locateLlamaServerBetween(assets, data);
        DOCTEST_CHECK_MESSAGE(std::string(hit.source) == "pkg",
                              "env 指向不存在目录须静默跳过该序");
    }
    return true;
})

REGISTER_TEST(memory_llama_locate_pkg_over_dataroot,
              "memory need202 F-01: llama 定位包内序优先数据根序（旧装机兼容）", {
    const auto assets = test_support::makeTempDir("devdesk_mem_locate_", "pkg2");
    const auto data = test_support::makeTempDir("devdesk_mem_locate_", "data2");
    seedLlamaDir(data);
    {
        test_support::ScopedEnv noEnv("DEVDESK_LLAMA_DIR", "");
        const auto hit = locateLlamaServerBetween({}, data);  // assets 空=无包
        DOCTEST_CHECK_EQ(std::string(hit.source), std::string("dataroot"));
    }
    seedLlamaDir(assets);
    {
        test_support::ScopedEnv noEnv("DEVDESK_LLAMA_DIR", "");
        const auto hit = locateLlamaServerBetween(assets, data);
        DOCTEST_CHECK_EQ(std::string(hit.source), std::string("pkg"));
    }
    return true;
})

REGISTER_TEST(memory_llama_locate_miss_falls_to_path_or_empty,
              "memory need202 F-01/F-03: 四处全空→exePath 空（PATH 有则如实记 path）", {
    const auto assets = test_support::makeTempDir("devdesk_mem_locate_", "empty");
    const auto data = test_support::makeTempDir("devdesk_mem_locate_", "empty2");
    test_support::ScopedEnv noEnv("DEVDESK_LLAMA_DIR", "");
    const auto miss = locateLlamaServerBetween(assets, data);
    DOCTEST_CHECK_MESSAGE(miss.exePath.empty() ||
                              std::string(miss.source) == "path",
                          "全空时只允许 PATH 命中或彻底未命中");
    return true;
})

REGISTER_TEST(memory_embed_model_pkg_first_zero_byte_skip,
              "memory need202 F-02: 模型定位包内优先数据根；0 字节视同缺失下沉", {
    const auto assets = test_support::makeTempDir("devdesk_mem_model_", "pkg");
    const auto data = test_support::makeTempDir("devdesk_mem_model_", "data");
    {
        // 只有数据根有模型
        seedModelDir(data, 10);
        const auto hit = locateEmbedModelBetween({}, data);
        DOCTEST_CHECK_EQ(std::string(hit.source), std::string("dataroot"));
    }
    {
        // 包内 0 字节 → 下沉数据根
        seedModelDir(assets, 0);
        const auto hit = locateEmbedModelBetween(assets, data);
        DOCTEST_CHECK_MESSAGE(std::string(hit.source) == "dataroot",
                              "0 字节包内模型须视同缺失下沉数据根");
    }
    {
        // 包内非空 → pkg 优先，数据根文件不动
        seedModelDir(assets, 10);
        const auto hit = locateEmbedModelBetween(assets, data);
        DOCTEST_CHECK_EQ(std::string(hit.source), std::string("pkg"));
    }
    {
        const auto miss = locateEmbedModelBetween(
            test_support::makeTempDir("devdesk_mem_model_", "e"),
            test_support::makeTempDir("devdesk_mem_model_", "e2"));
        DOCTEST_CHECK(miss.modelPath.empty());
        DOCTEST_CHECK_EQ(std::string(miss.source), std::string(""));
    }
    return true;
})

REGISTER_TEST(memory_llama_locate_diagnosis_four_seqs,
              "memory need202 F-03: 诊断文案含四序逐项与重装/env 指路", {
    const auto data = test_support::makeTempDir("devdesk_mem_diag_", "data");
    test_support::ScopedEnv noEnv("DEVDESK_LLAMA_DIR", "");
    const auto diag = llamaLocateDiagnosis(data);
    DOCTEST_CHECK_MESSAGE(diag.find("DEVDESK_LLAMA_DIR") != std::string::npos,
                          "诊断须含 env 序检查结果");
    DOCTEST_CHECK_MESSAGE(diag.find("包内 assets/llama/bin") != std::string::npos,
                          "诊断须含包内序检查结果");
    DOCTEST_CHECK_MESSAGE(diag.find("数据根 memory/llama/bin") != std::string::npos,
                          "诊断须含数据根序检查结果");
    DOCTEST_CHECK_MESSAGE(diag.find("PATH") != std::string::npos,
                          "诊断须含 PATH 序检查结果");
    DOCTEST_CHECK_MESSAGE(diag.find("重装") != std::string::npos,
                          "诊断须给重装指路");
    return true;
})
