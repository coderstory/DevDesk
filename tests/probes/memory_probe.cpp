// memory_probe.cpp — 记忆页三视图真数据探针（组 A 主控台四格 / 组 B 审核卡流 /
// 组 C 配置卡 + 三视图截图）。
//
// 运行：memory_probe.exe [--out <目录>] [--size 1200x1000]
//   --size 口径 = 逻辑像素（probe_harness 约定：探针窗口逻辑 = 物理，帧驱动
//   恒传刻度 1；与 about/todo_drag 探针同款，不按 contentScale 换算）。
// 数据隔离：进程内先把 DEVDESK_DATA_ROOT 指到临时目录（先于一切 memoryDataDir()
// 消费），演示数据（3 条提案 + 账面行 + 注入账 + 评测结果）全部落该独立根，
// 不碰真实数据根。截图 = 进程内 glReadPixels 帧捕获（home_probes 同款），
// 三视图各一张落 --out 目录（默认 ./.verify-screenshots/）。
#include "probes/probe_harness.h"
#include <ctime>
#include <system_error>
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/engine/telemem_engine.h"
#include "module_memory/memory_module.h"
#include "module_memory/panel/recall_eval.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::string NL = "\n";  // DBG 行尾（避免字面换行进串）

// 置环境变量（进程内生效；getenv 立即可见）
void setEnvVar(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

// 断言收集器：所有组共用，任何一条失败即整体 FAIL（stderr 逐条诊断）
struct Asserts {
    core::dsl::Ui* ui = nullptr;
    bool passed = true;

    const core::dsl::Element* elem(const std::string& id) {
        return ui != nullptr ? ui->find(id) : nullptr;
    }
    bool exists(const std::string& id) {
        if (elem(id) == nullptr) {
            std::cerr << "memory_probe: FAIL——结构缺失 '" << id << "'\n";
            passed = false;
            return false;
        }
        return true;
    }
    bool textEq(const std::string& id, const std::string& expect) {
        const auto* el = elem(id);
        if (el == nullptr) {
            std::cerr << "memory_probe: FAIL——结构缺失 '" << id << "'\n";
            passed = false;
            return false;
        }
        if (el->text != expect) {
            std::cerr << "memory_probe: FAIL——'" << id << "' 文本「" << el->text
                      << "」≠ 期望「" << expect << "」\n";
            passed = false;
            return false;
        }
        return true;
    }
    bool textHas(const std::string& id, const std::string& needle) {
        const auto* el = elem(id);
        if (el == nullptr) {
            std::cerr << "memory_probe: FAIL——结构缺失 '" << id << "'\n";
            passed = false;
            return false;
        }
        if (el->text.find(needle) == std::string::npos) {
            std::cerr << "memory_probe: FAIL——'" << id << "' 文本「" << el->text
                      << "」不含「" << needle << "」\n";
            passed = false;
            return false;
        }
        return true;
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);

    // ── 独立数据根（必须先于一切 memoryDataDir 消费）──
    const auto tmpRoot =
        std::filesystem::temp_directory_path() /
        ("memcpp-probe-root-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::filesystem::create_directories(tmpRoot);
    setEnvVar("DEVDESK_DATA_ROOT", tmpRoot.string());
    const std::filesystem::path dataDir =
        module::memory::engine::memoryDataDir();

    // ── 演示数据：3 条提案 + 账面行 + 注入账 + 评测结果（全部落独立根）──
    {
        std::filesystem::create_directories(dataDir);
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        auto prop = module::memory::proposals::ProposalStore::open(
            dataDir / "proposals.db");
        if (prop.isFail()) {
            std::cerr << "memory_probe: 提案库打开失败: "
                      << prop.errorMessage() << "\n";
            return 3;
        }
        auto& store = prop.value();
        using module::memory::proposals::ProposalEvidence;
        const ProposalEvidence ev{"demo-session-abc", 7, "演示会话原文引用",
                                  now - 3600};
        auto addP = module::memory::proposals::Curator::proposeAdd(
            store, "用户偏好：周报用要点式，每条一行", {ev}, 0.9, now);
        // targetIds = 引擎条目 id（telemem live 定案 = UUID 串，F3）；整型字面量
        // 会在 vector<string> 上变成 size_type 构造（编译期即拒）
        auto forgetP = store.create(
            "forget", {"b7e23ec2-0042-4c55-8f0a-1b2c3d4e5f60"}, "", "", "", {ev},
            0.85, "decay", "assistant_derived", now, 3);
        auto invalP = store.create(
            "invalidate", {"b7e23ec2-0043-4c55-8f0a-1b2c3d4e5f60"}, "", "", "",
            {ev}, 0.6, "conflict", "assistant_derived", now, 5);
        if (addP.isFail() || forgetP.isFail() || invalP.isFail()) {
            std::cerr << "memory_probe: 演示提案创建失败\n";
            return 3;
        }
        auto ledger = module::memory::proposals::UsageLedger::open(
            dataDir / "usage_ledger.db");
        if (ledger.isFail()) {
            std::cerr << "memory_probe: 账本打开失败\n";
            return 3;
        }
        (void)ledger.value().record("llm_input", 1200, now);
        (void)ledger.value().record("llm_output", 340, now);
        (void)ledger.value().record("embed", 800, now);
        (void)ledger.value().recordInjection(3, 214, now);
        auto evalStore = module::memory::panel::RecallEvalStore::open(
            dataDir / "recall_eval.db");
        if (evalStore.isOk()) {
            module::memory::panel::RecallResult seeded;
            seeded.ts = now;
            seeded.recallPct = 71;  // < 85 默认升级线 → 驱动黄标分支
            seeded.hits = 11;
            seeded.queries = 15;
            (void)evalStore.value().save(seeded);
        }
    }  // 种子连接在此全部关闭，避免与面板连接并发

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("memory_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::theme::setLight();  // 对照实验：浅色档下 Primary 按钮文字是否可见
    std::unique_ptr<::core::Module> mod(create_memory_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "memory";
    // --size 即逻辑尺寸（逻辑 = 物理，probe_harness pollOnce 刻意口径）；不按
    // contentScale 换算——除法会把 2x Retina 下的 1200x1000 压成 600x500 布局。
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    std::cout << "memory_probe: 逻辑窗口 " << args.windowW << "x" << args.windowH
              << "（--size 口径 = 逻辑像素，逻辑 = 物理）" << NL;
    Asserts A;

    // 帧驱动刻度 1 与上方口径配对（probe_harness pollOnce 同款）
    auto composeFrame = [&]() {
        runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
        runtime.compose(pageId, w, h, [&](eui::Ui& u, const eui::Screen& screen) {
            A.ui = &u;
            mod->compose(u, screen);
        });
    };
    auto switchView = [&](int view) {
        auto* mem = dynamic_cast<module::memory::MemoryModule*>(mod.get());
        if (mem == nullptr) {
            std::cerr << "memory_probe: FAIL——模块类型转换失败\n";
            A.passed = false;
            return;
        }
        mem->setActiveViewForTest(view);
    };
    // 截图输出目录（默认 .verify-screenshots/；不存在自动建）
    std::filesystem::path outDir = ".verify-screenshots";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    std::error_code outEc;
    std::filesystem::create_directories(outDir, outEc);
    auto shoot = [&](const std::string& name, bool* okFlag) {
        probe::FrameCapture capture(window);
        if (!capture.capturePng(runtime, (outDir / name).string())) {
            *okFlag = false;
        }
    };

    // ── 组 A：主控台四格真数值（种子在场）──
    composeFrame();
    composeFrame();
    {
        for (const char* id :
             {"memory.console.scroll", "memory.view.canvas", "memory.eng.card",
              "memory.health.card0", "memory.health.card3"}) {
            const auto* el = A.ui != nullptr ? A.ui->find(id) : nullptr;
            if (el == nullptr) {
                std::cout << "DBG " << id << " : MISSING" << NL;
            } else {
                std::cout << "DBG " << id << " : (" << el->frame.x << ","
                          << el->frame.y << " " << el->frame.width << "x"
                          << el->frame.height << ")" << NL;
            }
        }
    }
    {
        const auto& t = core::theme::current();
        std::cout << "DBG onPrimary=(" << t.colors.onPrimary.r << ","
                  << t.colors.onPrimary.g << "," << t.colors.onPrimary.b
                  << ") primary=(" << t.colors.primary.r << ","
                  << t.colors.primary.g << "," << t.colors.primary.b << ")"
                  << NL;
        const auto* btn = A.elem("memory.eng.toggle.text");
        if (btn != nullptr) {
            std::cout << "DBG toggle.text='" << btn->text << "' color=("
                      << btn->color.r << "," << btn->color.g << ","
                      << btn->color.b << "," << btn->color.a << ") fontSize="
                      << btn->fontSize << " at(" << btn->frame.x << ","
                      << btn->frame.y << " " << btn->frame.width << "x"
                      << btn->frame.height << ")" << NL;
        } else {
            std::cout << "DBG toggle.text=MISSING" << NL;
        }
    }
    A.textEq("memory.health.h2.value", "3/5 条");       // 注入预算环文本
    A.textEq("memory.health.h2.chars", "214/500 字");
    A.textEq("memory.health.h1.value", "71");           // 触发线评测读数
    A.textHas("memory.health.h1.chip", "低于升级线");    // 71 < 85 黄标
    A.textEq("memory.health.h0.chiptext", "强制开启 · 已锁定");
    A.exists("memory.health.h3.value");                 // 存储概览格
    A.textHas("memory.usage.nums", "今日");
    A.textHas("memory.tabs.0.text", "主控台");  // wtab 文本节点契约 <id>.text
    for (int i = 0; i < 8; ++i) composeFrame();  // 过渡动画收敛后落图
    shoot("memory-console.png", &A.passed);

    // ── 组 D：TAB 点击命中链（bug160：滚动根曾从页顶 (0,0) 铺满，scrollState
    // interactive 整面盖住 TAB 行吞点击）。隔帧成对口径（schedule_ui_probe
    // 同款）：队列逐帧消费，Press/Release 隔帧；同时断言 TAB 中心命中目标 =
    // TAB hit 面而非滚动根。──
    auto clickTab = [&](int idx, const char* hitId) {
        const auto* tab = A.elem("memory.tabs." + std::to_string(idx));
        if (tab == nullptr) {
            std::cerr << "memory_probe: FAIL——'memory.tabs." << idx
                      << "' 缺失\n";
            A.passed = false;
            return;
        }
        const float cx = tab->frame.x + tab->frame.width * 0.5f;
        const float cyT = tab->frame.y + tab->frame.height * 0.5f;
        const std::string hitAtTab =
            runtime.hitTestInteractiveAt(static_cast<double>(cx),
                                         static_cast<double>(cyT), 1.0f);
        if (hitAtTab != hitId) {
            std::cerr << "memory_probe: FAIL——TAB." << idx << " 中心命中 '"
                      << hitAtTab << "' ≠ " << hitId << "（命中链被盖）\n";
            A.passed = false;
        }
        core::queuePointerMotion(window, cx, cyT, core::PointerButtons{},
                                 core::KeyModifiers{});
        core::queuePointerButton(window, cx, cyT, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cyT, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
        for (int i = 0; i < 6; ++i) composeFrame();
    };
    {
        clickTab(1, "memory.tabs.1.hit");
        A.exists("memory.review.scroll");  // 视图已切到审核
        if (A.elem("memory.console.scroll") != nullptr) {
            std::cerr << "memory_probe: FAIL——点 TAB.1 后主控台滚动根仍在\n";
            A.passed = false;
        }
        A.textHas("memory.tabs.1.text", "(3)");
        clickTab(2, "memory.tabs.2.hit");
        A.exists("memory.config.scroll");  // 视图已切到配置
        A.exists("memory.cfg.mask.switch.track");
        clickTab(0, "memory.tabs.0.hit");
        A.exists("memory.console.scroll");  // 切回主控台
    }

    // ── 组 B：审核页卡片流（3 张 pending 卡 + tab 徽章 + 逐卡操作钮）──
    {
        auto prop = module::memory::proposals::ProposalStore::open(
            dataDir / "proposals.db");
        auto pend = prop.isOk()
                        ? prop.value().listPending()
                        : core::Result<std::vector<
                              module::memory::proposals::Proposal>>::
                              fail(core::ErrorCode::Internal, "seed gone");
        if (pend.isFail() || pend.value().size() != 3) {
            std::cerr << "memory_probe: FAIL——种子提案应为 3 条，实测 "
                      << (pend.isOk() ? pend.value().size() : 0) << "\n";
            A.passed = false;
        } else {
            switchView(1);
            composeFrame();
            composeFrame();
            A.textHas("memory.tabs.1.text", "(3)");
            for (const auto& p : pend.value()) {
                A.exists("memory.rv.card." + p.id);
                A.exists("memory.rv.accept." + p.id);
                A.exists("memory.rv.reject." + p.id);
                A.exists("memory.rv.edit." + p.id);
            }
            A.exists("memory.rv.batchsw.track");
        }
    }
    for (int i = 0; i < 8; ++i) composeFrame();
    shoot("memory-review.png", &A.passed);

    // ── 组 E：引擎卡状态行独立性（bug203）+ 启动按钮命中链 ──
    // bug203：pythonFound 优先分支曾遮蔽 llamaMissing 文案——注入「python 已检
    // 测 + embedding 缺失」复合态，断言 python 行与 embedding 警示行同时在场。
    // 修复前：memory.eng.embed 元素不存在 → 本组红。
    {
        namespace engine = module::memory::engine;
        engine::TelememEngine::Snapshot fake;
        fake.state = engine::TelememEngine::State::Idle;
        fake.pythonVersion = "3.13.15";
        fake.pythonFound = true;
        fake.llamaMissing = true;
        engine::TelememEngine::global().setSnapshotForTest(fake);
        switchView(0);
        composeFrame();
        composeFrame();
        A.textHas("memory.eng.python", "已检测");          // python 行独立呈现
        A.textHas("memory.eng.embed", "embedding 服务缺失");  // 醒目警示行在场
        // 启动按钮命中链：按钮中心命中 = 按钮本体（bug201 复验口径；非滚动根/卡底）
        const auto* bg = A.elem("memory.eng.toggle.bg");
        if (bg == nullptr) {
            std::cerr << "memory_probe: FAIL——'memory.eng.toggle.bg' 缺失\n";
            A.passed = false;
        } else {
            const float cx = bg->frame.x + bg->frame.width * 0.5f;
            const float cyB = bg->frame.y + bg->frame.height * 0.5f;
            const std::string hit = runtime.hitTestInteractiveAt(
                static_cast<double>(cx), static_cast<double>(cyB), 1.0f);
            if (hit != "memory.eng.toggle.bg") {
                std::cerr << "memory_probe: FAIL——启动钮中心命中 '" << hit
                          << "' ≠ memory.eng.toggle.bg（命中链被盖）\n";
                A.passed = false;
            }
        }
        for (int i = 0; i < 8; ++i) composeFrame();
        shoot("memory-engine-card.png", &A.passed);
    }

    // ── 组 F：审核卡长正文/长证据链截断（bug217：~1975 CJK 长文曾溢出行盒叠印）──
    // 修复前：body/evtoggle 文本全量进元素 → 长度断言红 + 截图可见溢出叠印。
    {
        std::string longText;
        for (int i = 0; i < 2000; ++i) longText += "记";
        auto prop2 = module::memory::proposals::ProposalStore::open(
            dataDir / "proposals.db");
        std::string longId;
        if (prop2.isFail()) {
            std::cerr << "memory_probe: FAIL——长文种子库打开失败\n";
            A.passed = false;
        } else {
            using module::memory::proposals::ProposalEvidence;
            const ProposalEvidence longEv{"demo-session-long", 7, longText,
                                          static_cast<std::int64_t>(std::time(nullptr)) - 60};
            auto longP = module::memory::proposals::Curator::proposeAdd(
                prop2.value(), longText, {longEv}, 0.9,
                static_cast<std::int64_t>(std::time(nullptr)));
            if (longP.isFail()) {
                std::cerr << "memory_probe: FAIL——长文提案创建失败\n";
                A.passed = false;
            } else {
                longId = longP.value().id;
            }
        }
        if (!longId.empty()) {
            // 面板刷新有 1s 墙钟节流（PanelState::refresh）——先跨过再断言，
            // 否则拿到的是种子期旧快照（伪红）
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            switchView(1);
            composeFrame();
            composeFrame();
            A.textHas("memory.tabs.1.text", "(4)");
            const auto* bodyEl = A.elem("memory.rv.body." + longId);
            const auto* evEl = A.elem("memory.rv.evtoggle." + longId);
            if (bodyEl == nullptr || evEl == nullptr) {
                std::cerr << "memory_probe: FAIL——长文卡元素缺失\n";
                A.passed = false;
            } else {
                // 预算 ≈ 3 行 × 卡宽；截断后正文/证据链均 <1500 字节（未截断
                // 时 UTF-8 下 ≈6000+ 字节，断言区分度充足）
                if (bodyEl->text.size() >= 1500 || evEl->text.size() >= 1500) {
                    std::cerr << "memory_probe: FAIL——长文未截断（body "
                              << bodyEl->text.size() << "B / evtoggle "
                              << evEl->text.size() << "B，预算内应 <1500B）\n";
                    A.passed = false;
                }
                const std::string kEllipsis = "\xE2\x80\xA6";  // U+2026 …
                if (bodyEl->text.size() < 3 ||
                    bodyEl->text.compare(bodyEl->text.size() - 3, 3,
                                         kEllipsis) != 0) {
                    std::cerr << "memory_probe: FAIL——截断正文未以省略号收尾\n";
                    A.passed = false;
                }
            }
            for (int i = 0; i < 8; ++i) composeFrame();
            shoot("memory-review-longtext.png", &A.passed);
        }
    }

    // ── 组 C：配置页关键控件（脱敏开关 / 导入钮 / 清除钮 / 阈值滑条 / 数据目录）──
    {
        switchView(2);
        composeFrame();
        composeFrame();
        A.exists("memory.cfg.mask.switch.track");
        A.exists("memory.cfg.llm.import");
        A.exists("memory.cfg.data.clear");
        A.exists("memory.cfg.trig.s1");
        A.textHas("memory.cfg.data.path", dataDir.generic_string());
    }
    for (int i = 0; i < 8; ++i) composeFrame();
    shoot("memory-config.png", &A.passed);

    // ── 组 G：向导四步（need:202 F-04/F-05）——③ embedding 本机包四态 + ④ 顺延 ──
    // 打开向导（点 memory.eng.wizard，命中链口径同组 E），③/④ 行文案随注入的
    // Snapshot.embed* 字段走（向导只读字段不探文件系统，设计 §4 实现约束）。
    {
        namespace engine = module::memory::engine;
        switchView(0);
        composeFrame();
        const auto* wb = A.elem("memory.eng.wizard.bg");
        if (wb == nullptr) {
            std::cerr << "memory_probe: FAIL——'memory.eng.wizard.bg' 缺失\n";
            A.passed = false;
        } else {
            const float cx = wb->frame.x + wb->frame.width * 0.5f;
            const float cyW = wb->frame.y + wb->frame.height * 0.5f;
            core::queuePointerButton(window, cx, cyW, core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            core::queuePointerButton(window, cx, cyW, core::PointerButton::Left,
                                     core::PointerAction::Release, {});
            for (int i = 0; i < 6; ++i) composeFrame();
        }
        if (A.elem("memory.dialog.wizard.s3.name") == nullptr ||
            A.elem("memory.dialog.wizard.s4.name") == nullptr) {
            std::cerr << "memory_probe: FAIL——向导未现四步结构（s3/s4 缺）\n";
            A.passed = false;
        } else {
            A.textHas("memory.dialog.wizard.s3.name", "③ embedding 本机包");
            A.textHas("memory.dialog.wizard.s4.name", "④ 引擎拉起与健康检查");
            A.textHas("memory.dialog.wizard.intro", "embedding 本机包");
            // 态 1：idle 引导（模型缺、llama 在场）→ 需下载灰 + ④ 待开始灰
            engine::TelememEngine::Snapshot fake;
            fake.state = engine::TelememEngine::State::Idle;
            fake.embedState = engine::TelememEngine::EmbedState::NeedDownload;
            engine::TelememEngine::global().setSnapshotForTest(fake);
            composeFrame();
            composeFrame();
            A.textEq("memory.dialog.wizard.s3.status",
                     "需下载（联网，首次约 26MB）");
            A.textEq("memory.dialog.wizard.s4.status", "待开始");
            shoot("memory-wizard-need-download.png", &A.passed);
            // 态 2：Running + 随包就位 → ③绿（随包）+ ④运行绿 + 完成块
            fake = engine::TelememEngine::Snapshot{};
            fake.state = engine::TelememEngine::State::Running;
            fake.embedReady = true;
            fake.embedSource = engine::TelememEngine::EmbedSource::Pkg;
            fake.embedState = engine::TelememEngine::EmbedState::Ready;
            engine::TelememEngine::global().setSnapshotForTest(fake);
            composeFrame();
            composeFrame();
            A.textEq("memory.dialog.wizard.s3.status", "已就位（随包）");
            A.textEq("memory.dialog.wizard.s4.status", "运行中（127.0.0.1:8421）");
            A.textHas("memory.dialog.wizard.done", "安装完成");
            shoot("memory-wizard-ready.png", &A.passed);
            // 态 3：下载失败 → ③红（可重试）+ 失败块带「开始安装 / 重试」逐字
            fake = engine::TelememEngine::Snapshot{};
            fake.state = engine::TelememEngine::State::Failed;
            fake.embedState = engine::TelememEngine::EmbedState::Failed;
            fake.embedDetail = "下载中断: net";
            engine::TelememEngine::global().setSnapshotForTest(fake);
            composeFrame();
            composeFrame();
            A.textHas("memory.dialog.wizard.s3.status", "失败：下载中断: net");
            A.textHas("memory.dialog.wizard.s3.status", "（可重试）");
            A.textHas("memory.dialog.wizard.fail", "开始安装 / 重试");
            shoot("memory-wizard-failed.png", &A.passed);
            // 弹窗高度裁定（设计 §9：320→348）；元素缺失不判红（frame 实现可换 id）
            if (const auto* dlg = A.elem("memory.dialog.wizard")) {
                if (dlg->frame.height != 348.0f) {
                    std::cerr << "memory_probe: FAIL——向导高度 "
                              << dlg->frame.height << " ≠ 348\n";
                    A.passed = false;
                }
            }
        }
    }

    std::cout << "memory_probe: " << (A.passed ? "PASS" : "FAIL")
              << " · 数据根 " << tmpRoot.generic_string() << " · 截图目录 "
              << outDir.generic_string() << "\n";
    mod->onStop();
    probe::terminateWindowBackend();
    return A.passed ? 0 : 1;
}
