// tests/test_modules/test_memory_engine.cpp
// 记忆引擎托管纯逻辑单测（S2 批次④）。全部注入假探针/假响应：不启真引擎、
// 不碰网络、不装 pip 包（真引擎验证归 live/UAT 层）。
// 契约红线（改动即契约变更）：
//   - Python 合格区间 3.10~3.12（3.13+ 探测步拦截，不放行到 pip）
//   - TELEMEM_CONFIG 恒不含密钥；key 只进子进程环境变量
//   - 健康判据查嵌套 error（body.error / body.result.error），不能只看传输层
#include "doctest.h"

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
              "memory: Python 合格区间 3.10~3.12（3.13+/3.9- 探测步拦截）", {
    DOCTEST_CHECK(pythonVersionQualified("3.10.0"));
    DOCTEST_CHECK(pythonVersionQualified("3.11.9"));
    DOCTEST_CHECK(pythonVersionQualified("3.12.4"));
    DOCTEST_CHECK(!pythonVersionQualified("3.9.7"));
    DOCTEST_CHECK(!pythonVersionQualified("3.13.1"));
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
    DOCTEST_CHECK(yaml.find(std::string("api_key_env: ") + kLlmKeyEnvName) !=
                  std::string::npos);
    // embedding 只绑回环 + 模型名常量
    DOCTEST_CHECK(yaml.find("http://127.0.0.1:11435/v1") != std::string::npos);
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
    // pip 首跑安装
    const auto pipArgs = buildPipInstallArgs(venv);
    DOCTEST_CHECK(contains(pipArgs, "-m"));
    DOCTEST_CHECK(contains(pipArgs, "pip"));
    DOCTEST_CHECK(contains(pipArgs, "telemem"));
    // 服务拉起：只绑回环
    const auto serverArgs = buildTelememServerArgs(venv, kTelememPort);
    DOCTEST_CHECK(contains(serverArgs, "--host"));
    DOCTEST_CHECK(contains(serverArgs, "127.0.0.1"));
    DOCTEST_CHECK(contains(serverArgs, "--port"));
    DOCTEST_CHECK(contains(serverArgs, "8421"));
    DOCTEST_CHECK(contains(serverArgs, "-m"));
    DOCTEST_CHECK(contains(serverArgs, "telemem"));
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
    DOCTEST_CHECK(envNoKey.size() == 1);
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
    for (const char* id : {"memory.eng.card", "memory.eng.state",
                           "memory.eng.start.bg", "memory.eng.stop.bg",
                           "memory.usage.bar", "memory.feed.card"}) {
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
