// memory_install_probe.cpp — 记忆模块全链路端到端实测驱动（need:159，qa）。
// 三个子命令，全部走产品真实代码路径（不绕行、不 stub 引擎）：
//   install   —— TelememEngine::startAsync() 真安装链（Python 探测 → venv → pip 装
//                telemem → 写 telemem-config.yaml → 拉起 llama+telemem → 探活），
//                轮询 snapshot 打印状态时间线；--hold-file <p> = 落 Running 后持有
//                引擎直到该文件被删除（编排方用完引擎后 rm 之，析构 terminateTree 清场）。
//   collect   —— 真实 Claude Code 会话 jsonl 走 S3 链（真 Collector 采集切段 →
//                SanitizeGate 脱敏 → Curator 提案），再 PanelState::acceptProposal
//                全部接受入库（真引擎 add），逐条检索回验，最后 faiss/history 计数。
//                前置：引擎已在 8421 运行（install --hold 持有中）。
//   mcpserve  —— 真 memory_search 工具注册进 core::mcp::globalMcpTools() +
//                module::mcp::globalMcpService().start(port) 起 DevDesk MCP HTTP
//                服务，--hold-file 持有；外部 curl 走 initialize/tools/list/tools/call
//                验证「DevDesk 暴露的 MCP 工具能查到刚入库的记忆」。
// 环境隔离：DEVDESK_DATA_ROOT 必须已设（外部注入临时根），全部读写收敛该根。
// 进程纪律：install 拉起的子进程由 TelememEngine 析构 terminateTree 收敛，无孤儿。
#include <chrono>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/mcp_tool.h"
#include "module_mcp/mcp_service.h"
#include "module_memory/collector/collector.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/engine/telemem_engine.h"
#include "module_memory/gateway/telemem_client.h"
#include "module_memory/mcp/search_tool.h"
#include "module_memory/panel/panel_state.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/sanitize/sanitize_gate.h"

namespace {

namespace col = module::memory::collector;
namespace gateway = module::memory::gateway;
namespace mcpmod = module::memory::mcp;
namespace proposals = module::memory::proposals;
namespace sanitize = module::memory::sanitize;
namespace panelns = module::memory::panel;
namespace engine = module::memory::engine;

const char* stateName(module::memory::engine::TelememEngine::State s) {
    switch (s) {
        case module::memory::engine::TelememEngine::State::Idle: return "Idle";
        case module::memory::engine::TelememEngine::State::Preparing: return "Preparing";
        case module::memory::engine::TelememEngine::State::Running: return "Running";
        case module::memory::engine::TelememEngine::State::Stopped: return "Stopped";
        case module::memory::engine::TelememEngine::State::Failed: return "Failed";
    }
    return "?";
}

void printSnapshot(const engine::TelememEngine::Snapshot& s) {
    std::cout << "[probe] state=" << stateName(s.state)
              << " pythonFound=" << (s.pythonFound ? 1 : 0)
              << " pythonVersion=" << s.pythonVersion
              << " llamaMissing=" << (s.llamaMissing ? 1 : 0)
              << " embedState=" << static_cast<int>(s.embedState)
              << " embedReady=" << (s.embedReady ? 1 : 0)
              << "\n[probe] detail=" << s.detail << "\n";
    if (!s.embedDetail.empty()) std::cout << "[probe] embedDetail=" << s.embedDetail << "\n";
}

std::int64_t nowSec() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::uintmax_t dirBytesRecursive(const std::filesystem::path& dir) {
    std::error_code ec;
    std::uintmax_t total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

int g_fail = 0;
int g_ok = 0;

void check(bool cond, const std::string& what) {
    if (cond) { ++g_ok; std::cout << "[probe] PASS " << what << "\n"; }
    else      { ++g_fail; std::cout << "[probe] FAIL " << what << "\n"; }
}

int finish() {
    std::cout << "[probe] SUMMARY ok=" << g_ok << " fail=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}

// 持有直到 sentinel 文件被删除（编排方收口）；超时上限兜底防失控。
bool holdUntilRemoved(const std::filesystem::path& sentinel, int maxSeconds) {
    std::cout << "[probe] hold: 持有中，删除 " << sentinel.string() << " 收口\n";
    std::fflush(stdout);
    for (int i = 0; i < maxSeconds * 2; ++i) {
        std::error_code ec;
        if (!std::filesystem::exists(sentinel, ec)) {
            std::cout << "[probe] hold: 收口信号收到\n";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "[probe] hold: 达持有上限 " << maxSeconds << "s，自行退出\n";
    return false;
}

int cmdInstall(int argc, char* argv[]) {
    int timeoutSec = 900;
    std::filesystem::path holdFile;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--timeout-sec") == 0 && i + 1 < argc) {
            timeoutSec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hold-file") == 0 && i + 1 < argc) {
            holdFile = argv[++i];
        }
    }
    auto& eng = engine::TelememEngine::global();
    std::cout << "[probe] === install: 触发产品安装链 startAsync ===\n";
    printSnapshot(eng.snapshot());
    eng.startAsync();
    std::string lastLine;
    for (int i = 0; i < timeoutSec * 2; ++i) {
        const auto snap = eng.snapshot();
        const std::string line =
            std::string(stateName(snap.state)) + "|" + snap.detail;
        if (line != lastLine) {
            std::cout << "[probe] t+" << (i / 2) << "." << (i % 2 ? 5 : 0)
                      << "s " << line << "\n";
            std::fflush(stdout);
            lastLine = line;
        }
        if (snap.state == engine::TelememEngine::State::Running) {
            std::cout << "[probe] PASS 引擎 Running（安装链全绿）\n";
            printSnapshot(snap);
            if (!holdFile.empty()) {
                // 落一个 sentinel 表示就绪，编排方轮询到它再用引擎
                { std::ofstream o(holdFile); o << "running"; }
                holdUntilRemoved(holdFile, 3600);
            }
            return finish();
        }
        if (snap.state == engine::TelememEngine::State::Failed) {
            std::cout << "[probe] FAIL 安装链 Failed\n";
            printSnapshot(snap);
            return finish();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "[probe] FAIL " << timeoutSec << "s 未达 Running\n";
    printSnapshot(eng.snapshot());
    g_fail++;
    return finish();
}

int cmdCollect(int argc, char* argv[]) {
    std::filesystem::path projectsRoot;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--projects") == 0 && i + 1 < argc) {
            projectsRoot = argv[++i];
        }
    }
    if (projectsRoot.empty()) {
        std::cerr << "collect: 需要 --projects <dir>\n";
        return 2;
    }
    const char* dataRootEnv = std::getenv("DEVDESK_DATA_ROOT");
    if (dataRootEnv == nullptr || *dataRootEnv == '\0') {
        std::cerr << "collect: DEVDESK_DATA_ROOT 未设\n";
        return 2;
    }
    const std::filesystem::path memDir =
        std::filesystem::path(dataRootEnv) / "memory";

    // ── 采集 → 脱敏 → 提案（同 memory_e2e_probe S3 产品路径）──────────────
    std::cout << "[probe] === collect: 真实会话 S3 链 ===\n";
    col::Collector collector(memDir / "collector.db", projectsRoot);
    auto segs = collector.pollOnce();
    check(segs.isOk(), "pollOnce 成功");
    if (segs.isFail()) return finish();
    std::cout << "[probe] 采集段数=" << segs.value().size() << "\n";
    for (const auto& s : segs.value()) {
        std::cout << "[probe]   session=" << s.sessionId
                  << " events=" << s.events.size()
                  << " bytes=" << s.text.size() << "\n";
    }
    check(!segs.value().empty(), "真实会话切出至少 1 段");

    int blocked = 0;
    sanitize::SanitizeGate gate([](const std::string& line) {
        LOG_INFO("[memory][sanitize] " + line);
    });
    auto storeR = proposals::ProposalStore::open(memDir / "proposals.db");
    check(storeR.isOk(), "提案库打开");
    if (storeR.isFail()) return finish();
    auto store = std::move(storeR.value());

    for (const auto& seg : segs.value()) {
        auto gated = gate.process(seg.text);
        if (gated.isFail()) { ++blocked; continue; }
        if (!gated.value().hits.empty()) {
            ++blocked;
            std::cout << "[probe] 拦截段 session=" << seg.sessionId << " 类别=";
            for (const auto& h : gated.value().hits) std::cout << h.category << ",";
            std::cout << "\n";
            continue;
        }
        std::vector<proposals::ProposalEvidence> ev;
        for (const auto& e : seg.events) {
            ev.push_back(proposals::ProposalEvidence{
                e.sessionId, static_cast<std::int64_t>(e.eventSeq),
                seg.text, nowSec()});
        }
        auto card = proposals::Curator::proposeAdd(store, gated.value().text,
                                                   ev, 0.9, nowSec());
        check(card.isOk(), "提案生成 (" + seg.sessionId + ")");
    }
    check(collector.commit(segs.value()).isOk(), "水位/事件记账 commit");
    auto pending = store.listPending();
    check(pending.isOk(), "listPending 成功");
    const int pendingN = pending.isOk() ? static_cast<int>(pending.value().size()) : 0;
    std::cout << "[probe] pending=" << pendingN << " blocked=" << blocked << "\n";
    check(pendingN >= 1, "真实会话产出待审提案 ≥1");

    // ── 审核（接受全部）→ 入库 → 检索回验（同 S4 产品路径）────────────────
    std::cout << "[probe] === collect: S4 accept-all + 检索回验 ===\n";
    panelns::PanelState panel;
    panel.refresh(true);
    gateway::TelememHttpRetriever eng;
    const std::filesystem::path faissDir = memDir / "faiss_db";
    const std::uintmax_t faissBefore = dirBytesRecursive(faissDir);
    int accepted = 0;
    int recalled = 0;
    if (pending.isOk()) {
        for (const auto& p : pending.value()) {
            auto acc = panel.acceptProposal(p.id);
            if (!acc.isOk()) {
                check(false, "acceptProposal(" + p.id + ") 失败: " + acc.errorMessage());
                continue;
            }
            ++accepted;
            // 逐条检索回验：正文前 12 字节做探针短语（融合段应含已入库正文）。
            // bug255：必须按码点边界截取——字节 substr 劈开多字节字符产生非法
            // UTF-8，telemem JSON 解析失败回 400，回验恒败（2026-11-10 实测）。
            const std::string phrase = col::truncateUtf8(p.resultText, 12);
            auto r = eng.search(phrase, 5, 8000);
            int hits = 0;
            if (r.isOk()) {
                for (const auto& h : r.value()) {
                    if (h.text.find(phrase) != std::string::npos) ++hits;
                }
            } else {
                std::cout << "[probe] search 失败: " << r.errorMessage() << "\n";
            }
            std::cout << "[probe] 已入库 id=" << p.id << " bytes=" << p.resultText.size()
                      << " 检索回验命中=" << hits << "\n";
            if (hits >= 1) ++recalled;
        }
    }
    check(accepted >= 1, "接受提案 ≥1（真引擎 add）");
    check(recalled == accepted, "检索回验全召回 (" + std::to_string(recalled) +
                                    "/" + std::to_string(accepted) + ")");
    const std::uintmax_t faissAfter = dirBytesRecursive(faissDir);
    std::cout << "[probe] faiss_db " << faissBefore << "B → " << faissAfter << "B\n";
    check(faissAfter > faissBefore, "faiss 落盘增长");

    // history.db 条目计数（sqlite3 CLI 兜底；计数证据由外部 bash 再核一遍）
    std::cout << "[probe] faiss_db 文件清单：\n";
    {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(faissDir, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::cout << "[probe]   " << it->path().filename().string() << "\n";
        }
    }
    return finish();
}

int cmdMcpServe(int argc, char* argv[]) {
    int port = 18421;
    std::filesystem::path holdFile;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hold-file") == 0 && i + 1 < argc) {
            holdFile = argv[++i];
        }
    }
    std::cout << "[probe] === mcpserve: 注册真 memory_search + 起 DevDesk MCP HTTP ===\n";
    auto regR = mcpmod::setSearchEnabled(core::mcp::globalMcpTools(), true);
    if (!regR.isOk()) {
        std::cout << "[probe] FAIL setSearchEnabled: " << regR.errorMessage() << "\n";
        return 1;
    }
    const bool listed =
        core::mcp::globalMcpTools().find(mcpmod::kMemorySearchToolName).isOk();
    check(listed, "memory_search 已在 DevDesk MCP 工具注册表");
    auto startR = module::mcp::globalMcpService().start(port);
    if (startR.isFail()) {
        std::cout << "[probe] FAIL MCP 服务启动失败: " << startR.errorMessage() << "\n";
        return 1;
    }
    std::cout << "[probe] PASS MCP HTTP 服务监听 :" << startR.value() << "\n";
    std::cout << "[probe] 验证命令（外部 curl）：POST /mcp initialize → tools/list → "
                 "tools/call memory_search\n";
    std::fflush(stdout);
    if (!holdFile.empty()) holdUntilRemoved(holdFile, 1800);
    module::mcp::globalMcpService().stop();
    return finish();
}

}  // namespace

// need256 E2E：假 key 走真实钥匙串通道 → 引擎 Running（启动透传生效）→
// add_memory(infer=true) 真实出机 → 云端 401 fail-closed 文案可见（bug254
// 口径：真因透传）。key 值只进内存/钥匙串，绝不打印（密钥红线，出参过脱敏）。
int cmdKeyE2e(int argc, char* argv[]) {
    int timeoutSec = 900;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--timeout-sec") == 0 && i + 1 < argc) {
            timeoutSec = std::atoi(argv[++i]);
        }
    }
    const std::string kFakeKey = "sk-e2e-fake-need256";
    if (!engine::saveLlmKeyToKeychain(kFakeKey)) {
        std::cout << "[probe] FAIL 假 key 存钥匙串失败\n";
        g_fail++;
        return finish();
    }
    std::cout << "[probe] 假 key 已存钥匙串（devdesk.memory/llm-key，值不打印）\n";
    struct KeyGuard {  // 出口保证：测毕必清（真钥匙串不残留测试凭据）
        ~KeyGuard() { engine::clearLlmKeyFromKeychain(); }
    } guard;
    auto& eng = engine::TelememEngine::global();
    eng.startAsync();
    std::string lastLine;
    bool running = false;
    for (int i = 0; i < timeoutSec * 2; ++i) {
        const auto snap = eng.snapshot();
        const std::string line =
            std::string(stateName(snap.state)) + "|" + snap.detail;
        if (line != lastLine) {
            std::cout << "[probe] t+" << (i / 2) << "." << (i % 2 ? 5 : 0)
                      << "s " << line << "\n";
            std::fflush(stdout);
            lastLine = line;
        }
        if (snap.state == engine::TelememEngine::State::Running) {
            running = true;
            std::cout << "[probe] PASS 引擎 Running（钥匙串 key 启动透传生效）\n";
            break;
        }
        if (snap.state == engine::TelememEngine::State::Failed) {
            std::cout << "[probe] FAIL 引擎 Failed（key 通道未透传？）\n";
            printSnapshot(snap);
            g_fail++;
            return finish();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!running) {
        std::cout << "[probe] FAIL " << timeoutSec << "s 未达 Running\n";
        g_fail++;
        return finish();
    }
    // 提炼步真实出机（与面板 runInferStep 同一执行器 addInferred）：假 key
    // 必失败且文案含 401 真因（fail-closed）
    gateway::TelememHttpRetriever inferEngine;
    const auto r = inferEngine.addInferred(
        "need256 e2e 烟测：假 key 提炼调用，预期 401 fail-closed");
    if (r.isOk()) {
        std::cout << "[probe] FAIL 假 key 提炼意外成功（fail-closed 失效）\n";
        g_fail++;
        return finish();
    }
    const std::string msg =
        engine::redactSecretsForLog(r.errorMessage(), kFakeKey);
    std::cout << "[probe] 提炼调用已真实发出，fail-closed 文案: " << msg << "\n";
    std::fflush(stdout);
    const bool looks401 = msg.find("401") != std::string::npos ||
                          msg.find("authentication") != std::string::npos ||
                          msg.find("Unauthorized") != std::string::npos;
    if (!looks401) {
        std::cout << "[probe] FAIL 错误文案未透传 401 真因\n";
        g_fail++;
        return finish();
    }
    std::cout << "[probe] PASS need256 key 通道 E2E（配置→透传→提炼→401 fail-closed）\n";
    return finish();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // 裸调用（ctest 守卫链 add_test 无参注册）退 77 = SKIP_RETURN_CODE 跳过：
        // 本探针归 QA 显式子命令驱动（install 需网络 pip / mcpserve 起真 HTTP
        // 服务，都不能进守卫链自动跑）；argless 退 2 会令守卫链恒红（池199）。
        std::cout << "memory_install_probe: 裸调用跳过（ctest 77）；"
                     "用法: memory_install_probe <install|collect|mcpserve> "
                     "[参数]\n";
        return 77;
    }
    const std::string cmd = argv[1];
    if (cmd == "install") return cmdInstall(argc, argv);
    if (cmd == "collect") return cmdCollect(argc, argv);
    if (cmd == "mcpserve") return cmdMcpServe(argc, argv);
    if (cmd == "key-e2e") return cmdKeyE2e(argc, argv);
    std::cerr << "未知子命令: " << cmd << "\n";
    return 2;
}
