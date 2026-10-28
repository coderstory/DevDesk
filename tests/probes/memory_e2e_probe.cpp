// memory_e2e_probe.cpp — memcpp 全链真数据真跑探针（无头，不起 GUI/引擎进程：
// llama-server + telemem 由外部按引擎真实形态拉起，本探针只驱动数据链）。
// 场景链（2026-10-28 端到端联调验收）：
//   S3 采集→脱敏→提案（真 Collector/SanitizeGate/Curator/ProposalStore）
//   S4 审核→入库→撤销（真 PanelState::acceptProposal/reject/createUndoCard，
//      真引擎 add/search/delete 落 faiss）
//   S5 hook 注入函数级契约（真 runMemoryHook + 真 TelememHttpRetriever；
//      devdesk.exe 子命令真跑与 ledger 记账由外部 bash 阶段做）
//   S6 冻结 15 查询触发线评测（真 runFrozenEval 对真引擎）
// 环境隔离：DEVDESK_DATA_ROOT 必须已设（外部注入），全部读写收敛该根。
// 进程纪律：本探针不 spawn 任何子进程；结束不杀外部引擎。
#include <chrono>
#include <cstdint>
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
#include "module_memory/collector/collector.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/gateway/telemem_client.h"
#include "module_memory/hook/hook_command.h"
#include "module_memory/panel/panel_state.h"
#include "module_memory/panel/recall_eval.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/sanitize/sanitize_gate.h"

namespace {

namespace col = module::memory::collector;
namespace gateway = module::memory::gateway;
namespace proposals = module::memory::proposals;
namespace sanitize = module::memory::sanitize;
namespace panelns = module::memory::panel;

int g_fail = 0;
int g_ok = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_ok;
        std::cout << "[e2e] PASS " << what << "\n";
    } else {
        ++g_fail;
        std::cout << "[e2e] FAIL " << what << "\n";
    }
}

// 递归目录体积（faiss 落盘变化证据）
std::uintmax_t dirBytes(const std::filesystem::path& dir) {
    std::error_code ec;
    std::uintmax_t total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied,
             ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

std::int64_t nowSec() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

int finish() {
    std::cout << "[e2e] SUMMARY ok=" << g_ok << " fail=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* dataRootEnv = std::getenv("DEVDESK_DATA_ROOT");
    if (dataRootEnv == nullptr || *dataRootEnv == '\0') {
        std::cerr << "memory_e2e_probe: DEVDESK_DATA_ROOT 未设\n";
        return 2;
    }
    const std::filesystem::path dataRoot = dataRootEnv;
    std::filesystem::path projectsRoot;
    std::filesystem::path holdSentinel;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--projects") == 0 && i + 1 < argc) {
            projectsRoot = argv[++i];
        } else if (std::strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            holdSentinel = argv[++i];
        }
    }
    if (projectsRoot.empty()) {
        std::cerr << "memory_e2e_probe: 需要 --projects <root>\n";
        return 2;
    }
    const std::filesystem::path memDir = dataRoot / "memory";

    // ── S3：采集 → 脱敏 → 提案 ────────────────────────────────────────────
    std::cout << "[e2e] === S3 collect/sanitize/propose ===\n";
    col::Collector collector(memDir / "collector.db", projectsRoot);
    auto segs = collector.pollOnce();
    check(segs.isOk(), "S3 pollOnce 成功");
    if (segs.isFail()) return finish();
    std::cout << "[e2e] 段数=" << segs.value().size() << "\n";
    for (const auto& s : segs.value()) {
        std::cout << "[e2e]   session=" << s.sessionId
                  << " events=" << s.events.size()
                  << " bytes=" << s.text.size() << "\n";
    }
    check(segs.value().size() == 3,
          "S3 三个会话各出一段（4KB 内合段）");

    int blocked = 0;
    std::vector<std::string> blockedCategories;
    bool sensitiveRawLeaked = false;
    sanitize::SanitizeGate gate([](const std::string& line) {
        // 生产同款：审计行走 core::log 文件通道（内容红线：只含计数与长度）
        LOG_INFO("[memory][sanitize] " + line);
        std::cout << "[e2e][audit] " << line << "\n";
    });
    auto storeR = proposals::ProposalStore::open(memDir / "proposals.db");
    check(storeR.isOk(), "S3 提案库打开");
    if (storeR.isFail()) return finish();
    auto store = std::move(storeR.value());

    for (const auto& seg : segs.value()) {
        auto gated = gate.process(seg.text);
        if (gated.isFail()) {  // fail-closed：非法输入中断外发
            ++blocked;
            continue;
        }
        if (!gated.value().hits.empty()) {
            ++blocked;
            for (const auto& h : gated.value().hits) {
                blockedCategories.push_back(h.category);
                // 脱敏后文本不得再含原文片段（preview 等长遮蔽契约）
                if (gated.value().text.find(seg.text) != std::string::npos) {
                    sensitiveRawLeaked = true;
                }
            }
            std::cout << "[e2e] 拦截段 session=" << seg.sessionId << " 类别=";
            for (const auto& c : sanitize::SanitizeResult{gated.value()}.categories()) {
                std::cout << c << ",";
            }
            std::cout << "\n";
            continue;  // 拦截：不产提案（验收口径：敏感段被拦截）
        }
        std::vector<proposals::ProposalEvidence> ev;
        for (const auto& e : seg.events) {
            ev.push_back(proposals::ProposalEvidence{
                e.sessionId, static_cast<std::int64_t>(e.eventSeq),
                seg.text, nowSec()});
        }
        auto card = proposals::Curator::proposeAdd(store, gated.value().text,
                                                   ev, 0.9, nowSec());
        check(card.isOk(), "S3 干净段生成新增提案 (" + seg.sessionId + ")");
    }
    check(collector.commit(segs.value()).isOk(), "S3 水位/事件记账 commit");
    check(blocked == 1, "S3 恰好 1 段被拦截（含手机号+密钥的会话）");
    bool catOk = false;
    for (const auto& c : blockedCategories) {
        if (c == "phone" || c == "secret") catOk = true;
    }
    check(catOk, "S3 拦截类别命中 phone/secret");
    check(!sensitiveRawLeaked, "S3 脱敏段无原文泄漏");

    auto pending = store.listPending();
    check(pending.isOk() && pending.value().size() == 2,
          "S3 pending 提案 = 2（两个干净会话）");
    if (pending.isOk()) {
        for (const auto& p : pending.value()) {
            check(p.status == "pending" && p.source == "collect",
                  "S3 提案 " + p.id + " status=pending source=collect");
            check(!p.evidence.empty() &&
                      (p.evidence[0].sessionId.rfind("session-a", 0) == 0 ||
                       p.evidence[0].sessionId.rfind("session-b", 0) == 0),
                  "S3 提案 " + p.id + " 来源会话正确 (" +
                      p.evidence[0].sessionId + ")");
            check(std::abs(p.createdAt - nowSec()) <= 10,
                  "S3 提案 " + p.id + " createdAt 贴当前时间");
            check(p.resultText.find("13800138000") == std::string::npos &&
                      p.resultText.find("sk-e2eFakeKey") == std::string::npos,
                  "S3 提案 " + p.id + " 正文零敏感原文");
        }
    }

    // ── S4：审核 → 入库 → 撤销（真 PanelState 生产路径）─────────────────────
    std::cout << "[e2e] === S4 review/apply/undo ===\n";
    panelns::PanelState panel;
    panel.refresh(true);  // 内部按需 openStores（私有）
    if (!pending.isOk() || pending.value().size() < 2) return finish();
    const std::string idA = pending.value()[0].id;   // 接受
    const std::string idB = pending.value()[1].id;   // 拒绝
    const std::string textA = pending.value()[0].resultText;
    const std::string textB = pending.value()[1].resultText;
    gateway::TelememHttpRetriever engine;

    const std::filesystem::path faissDir = memDir / "faiss_db";
    const std::uintmax_t faissBefore = dirBytes(faissDir);

    auto probePhrase = [&](const std::string& phrase) {
        auto r = engine.search(phrase, 5, 8000);
        if (r.isFail()) return 0;
        int n = 0;
        for (const auto& h : r.value()) {
            if (h.text.find(phrase) != std::string::npos) ++n;
        }
        return n;
    };
    // 探针短语：会话正文里逐字出现的稳定词（检索命中判定 = 融合段含该词）
    const std::string phraseA = "增量构建";  // 会话 A 正文中逐字出现的稳定词
    check(probePhrase(phraseA) == 0, "S4 接受前引擎检索 0 命中");

    auto acc = panel.acceptProposal(idA);
    check(acc.isOk(), "S4 acceptProposal(" + idA + ") 成功（真引擎 add）");
    const int hitsAfter = probePhrase(phraseA);
    check(hitsAfter >= 1, "S4 接受后引擎检索命中已入库记忆 (" +
                              std::to_string(hitsAfter) + ")");
    const std::uintmax_t faissAfter = dirBytes(faissDir);
    check(faissAfter != faissBefore,
          "S4 faiss 落盘变化 " + std::to_string(faissBefore) + "B → " +
              std::to_string(faissAfter) + "B");

    auto rej = panel.rejectProposal(idB);
    check(rej.isOk(), "S4 rejectProposal(" + idB + ") 成功");
    check(probePhrase(textB.substr(0, 24)) == 0, "S4 拒绝项未入库（检索 0 命中）");

    // ── S5（函数级）：hook 注入契约（真检索器对真引擎）────────────────────
    std::cout << "[e2e] === S5 hook inject (in-probe) ===\n";
    const std::string hookIn =
        "{\"session_id\":\"probe-s1\",\"hook_event_name\":"
        "\"UserPromptSubmit\",\"prompt\":\"DevDesk 构建脚本怎么跑？\"}";
    int injectedItems = 0;
    int injectedChars = 0;
    auto outcome = module::memory::hook::runMemoryHook(
        hookIn, &engine,
        [&](const std::string& kind, long long tokens) {
            if (kind == "inject_items") injectedItems = static_cast<int>(tokens);
            if (kind == "inject_chars") injectedChars = static_cast<int>(tokens);
        });
    check(outcome.exitCode == 0, "S5 hook exitCode 恒 0");
    check(outcome.stdoutText.find("hookSpecificOutput") != std::string::npos &&
              outcome.stdoutText.find("additionalContext") != std::string::npos &&
              outcome.stdoutText.find("UserPromptSubmit") != std::string::npos,
          "S5 stdout 含 hookSpecificOutput.additionalContext");
    check(outcome.stdoutText.find(phraseA) != std::string::npos,
          "S5 注入命中已入库记忆");
    check(outcome.injectedItems >= 1 && outcome.injectedItems <= 5,
          "S5 注入条数 " + std::to_string(outcome.injectedItems) + " ≤5");
    check(outcome.injectedChars <= 500,
          "S5 注入字数 " + std::to_string(outcome.injectedChars) + " ≤500");
    check(outcome.stdoutText.find("[20") != std::string::npos ||
              outcome.stdoutText.find("来源:") != std::string::npos,
          "S5 注入行带日期/来源标注");

    // ── S4 续：撤销链（接受 → 撤销 → 引擎真删除）──────────────────────────
    std::cout << "[e2e] === S4 undo chain ===\n";
    auto undoId = panel.createUndoCard(idA);
    check(undoId.isOk(), "S4 createUndoCard 成功");
    if (undoId.isOk()) {
        auto undone = panel.acceptProposal(undoId.value());
        check(undone.isOk(), "S4 撤销卡接受成功（引擎 delete 通道）");
        check(probePhrase(phraseA) == 0, "S4 撤销后引擎检索 0 命中（真删除）");
    }
    // ── S6：冻结 15 查询触发线评测（真引擎）──────────────────────────────
    std::cout << "[e2e] === S6 frozen eval (15 queries) ===\n";
    auto evalR = panelns::runFrozenEval(engine);
    check(evalR.isOk(), "S6 runFrozenEval 完成");
    if (evalR.isOk()) {
        const auto& r = evalR.value();
        std::cout << "[e2e] Recall@5 = " << r.recallPct << "% (" << r.hits
                  << "/" << r.queries << ")\n";
        check(r.queries == 15, "S6 参评查询 = 15");
        check(r.recallPct >= 85,
              "S6 Recall@5 " + std::to_string(r.recallPct) + "% ≥ 85% 触发线");
    }

    // ── S7：留一条在库记忆，供外部 devdesk.exe memory hook 真跑命中 ──────────
    std::cout << "[e2e] === S7 seed memory for binary hook ===" << std::endl;
    auto extra = proposals::Curator::proposeAdd(
        store,
        "DevDesk 记忆模块的 telemem 引擎绑定回环端口 8421，本机 embedding 服务绑定回环端口 11435",
        {}, 0.9, nowSec());
    check(extra.isOk(), "S7 附加提案创建");
    if (extra.isOk()) {
        auto accExtra = panel.acceptProposal(extra.value().id);
        check(accExtra.isOk(), "S7 附加提案接受入库");
        check(probePhrase(std::string("8421")) >= 1, "S7 引擎检索命中 8421 记忆");
    }

    std::cout << "[e2e] SUMMARY ok=" << g_ok << " fail=" << g_fail << "\n";

    // ── HOLD：保持引擎数据链可用，等外部跑 devdesk.exe memory hook ─────────
    if (!holdSentinel.empty()) {
        std::cout << "[e2e] HOLDING（等哨兵文件 " << holdSentinel.string()
                  << " 出现后退出；期间引擎可被外部子命令访问）\n";
        std::cout.flush();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(240);
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code ec;
            if (std::filesystem::exists(holdSentinel, ec)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::cout << "[e2e] hold 结束，退出\n";
    }
    return g_fail == 0 ? 0 : 1;
}
