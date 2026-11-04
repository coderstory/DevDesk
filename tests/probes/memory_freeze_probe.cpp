// memory_freeze_probe.cpp — bug190（P0）红绿探针：Memory 页审核视图在大量
// pending 提案 × 长 CJK 正文下的单次 compose 耗时与卡片流上限。
//
// 背景（evidence/2026-11-04-memory-freeze/stack-evidence.md）：真实库 1630 条
// pending（一次采集轮风暴批量落库），审核视图无上限逐卡渲染 ≈ 每卡 17 元素，
// 单帧 compose 陷入 171s+（CJK 回退字形逐个 FT_Load_Glyph + autofit hinting
// 且 advance 无缓存），UI 消息泵饿死。本探针复现该机制：
//   组 R（红判据）：种子 120 条 × ~600 字 CJK pending 提案 → 审核视图
//     ①渲染卡数必须被上限截断（无上限 = 红）
//     ②热帧 compose 耗时必须 < 250ms（15 帧预算；无缓存时秒级起步 = 红）
//     ③「再显示 50 条」渐进加载可用（仅在截断生效时执行，避免红态慢帧拖探针）
//   组 G（advance 一致性）：CJK 串测量宽度打印为黄金值（修前/修后必须逐位
//     一致——advance 缓存按同表达式复算，不改任何数值）；缓存命中路径与逐字
//     累加求和必须相等。
// 数据隔离：DEVDESK_DATA_ROOT 进程内指临时目录（memory_probe 同款），不碰真实根。
// 运行：memory_freeze_probe.exe（无参数；截图不走本探针，计时即证据）。
#include "probes/probe_harness.h"
#include <cstdio>
#include <ctime>
#include <chrono>
#include <system_error>
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/render/text.h"
#include "core/theme.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/memory_module.h"
#include "module_memory/proposals/proposal_store.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

const std::string NL = "\n";  // DBG 行尾（避免字面换行进串）

void setEnvVar(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

// bug190 真实库分布：kind 全 add、result_text 均长 ~2000 CJK 字符（含「**明确
// 不做**：…」这类范围声明正文）。种子正文按真实均长 1/3 缩放（600 字），条数
// 120——机制同源（逐字形无缓存度量 × 无上限卡流），红态量级控制在分钟内。
std::string makeProposalBody(int salt) {
    static const char* seg = "确认本次范围边界：明确不做的部分包括查无结果的负缓存"
                             "（可选优化未拍板）、缓存读写逻辑重构、同步 Job 处置，"
                             "这些都不在本次范围内；先把主流程冻结问题修完，再谈优化。";
    std::string body = "**明确不做**：范围声明（条目 #" +
                       std::to_string(salt) + "）——";
    for (int i = 0; i < 8; ++i) {
        body += seg;
    }
    return body;  // ≈ 8×72 CJK 字 ≈ 580 字/条
}

struct Asserts {
    core::dsl::Ui* ui = nullptr;
    bool passed = true;

    const core::dsl::Element* elem(const std::string& id) {
        return ui != nullptr ? ui->find(id) : nullptr;
    }
    bool exists(const std::string& id) {
        if (elem(id) == nullptr) {
            std::cerr << "memory_freeze_probe: FAIL——结构缺失 '" << id << "'\n";
            passed = false;
            return false;
        }
        return true;
    }
    void expect(bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "memory_freeze_probe: FAIL——" << what << "\n";
            passed = false;
        }
    }
};

}  // namespace

int main() {
    // ── 独立数据根（先于一切 memoryDataDir 消费）──
    const auto tmpRoot =
        std::filesystem::temp_directory_path() /
        ("memory-freeze-probe-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmpRoot);
    setEnvVar("DEVDESK_DATA_ROOT", tmpRoot.string());
    const std::filesystem::path dataDir =
        module::memory::engine::memoryDataDir();

    // ── 种子：120 条 pending add 提案 × ~600 字 CJK 正文 ──
    constexpr int kSeedPending = 120;
    {
        std::filesystem::create_directories(dataDir);
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        auto prop = module::memory::proposals::ProposalStore::open(
            dataDir / "proposals.db");
        if (prop.isFail()) {
            std::cerr << "memory_freeze_probe: 提案库打开失败: "
                      << prop.errorMessage() << "\n";
            return 3;
        }
        auto& store = prop.value();
        using module::memory::proposals::ProposalEvidence;
        for (int i = 0; i < kSeedPending; ++i) {
            const ProposalEvidence ev{"freeze-probe-session", 7,
                                      "会话原文引用（证据链摘录，CJK 若干）",
                                      now - 3600};
            char idbuf[48];
            std::snprintf(idbuf, sizeof(idbuf), "seed-target-%06d", i);
            auto r = store.create("add", {idbuf}, makeProposalBody(i), "text",
                                  "freeze-probe-project", {ev}, 0.9, "collect",
                                  "assistant_derived", now, 30);
            if (r.isFail()) {
                std::cerr << "memory_freeze_probe: 种子提案 " << i
                          << " 创建失败: " << r.errorMessage() << "\n";
                return 3;
            }
        }
    }  // 种子连接在此关闭，避免与面板连接并发

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("memory_freeze_probe", 1200, 900);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::theme::setLight();
    std::unique_ptr<::core::Module> mod(create_memory_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "memory";
    const float w = 1200.0f;
    const float h = 900.0f;
    Asserts A;

    // ── 组 G：advance 黄金宽度（修前/修后必须逐位一致）──
    struct Golden {
        const char* text;
        float fontSize;
    };
    const Golden goldens[] = {
        {"明确不做：C 查无的负缓存（可选优化，未拍板）、缓存读写逻辑、同步 Job "
         "处置——都不在本次范围",
         12.75f},  // gdb 冻结现场卡住的那条文本
        {"记忆审核 · telemem 记忆引擎 v1.10.1（今天 12:38）", 12.5f},
        {"ABCdef123 ～ ！?", 13.5f},
    };
    std::cout << "GOLDEN-BEGIN" << NL;
    for (const auto& g : goldens) {
        const float wCold = core::TextPrimitive::measureTextWidth(
            g.text, "", g.fontSize, 400);
        const float wWarm = core::TextPrimitive::measureTextWidth(
            g.text, "", g.fontSize, 400);  // 第二次 = TextSizeCache 命中路径
        std::cout << "GOLDEN width=" << std::fixed << wWarm << " cold="
                  << wCold << " size=" << g.fontSize << NL;
        A.expect(wCold == wWarm,
                 "同串两次测量宽度不一致（缓存命中路径漂移）");
        // 逐字累加求和 = 缓存命中（逐字形）路径一致性
        const std::string s = g.text;
        float sum = 0.0f;
        for (size_t b = 0; b < s.size();) {
            size_t step = 1;
            const unsigned char c = static_cast<unsigned char>(s[b]);
            if (c >= 0xF0) step = 4;
            else if (c >= 0xE0) step = 3;
            else if (c >= 0xC0) step = 2;
            sum += core::TextPrimitive::measureTextWidth(
                s.substr(b, step), "", g.fontSize, 400);
            b += step;
        }
        const float whole = core::TextPrimitive::measureTextWidth(
            s, "", g.fontSize, 400);
        std::cout << "GOLDEN whole=" << std::fixed << whole
                  << " perCharSum=" << sum << NL;
        A.expect(whole == sum,
                 "整串测量 ≠ 逐字累加（shaping 高级排版 joint 语义除外，"
                 "CJK 文本应相等）");
    }
    std::cout << "GOLDEN-END" << NL;

    // ── 组 G2：advance 缓存微基准（稳定红绿判据，不受冷帧磁盘噪声影响）──
    // S1/S2 同词汇表不同文本：pre-fix 两个串都要逐字形 FT_Load_Glyph+autofit
    // （各 ≈ 600×29µs ≈ 17ms）；post-fix S2 的字形 advance 全部命中缓存
    // （仅少量新 salt 数字加载）→ 毫秒以下。5ms 阈值两侧均有 ≥3x 裕度。
    {
        const std::string s1 = makeProposalBody(1);
        const std::string s2 = makeProposalBody(999);  // 同词表，仅数字盐不同
        const auto t1 = std::chrono::steady_clock::now();
        (void)core::TextPrimitive::measureTextWidth(s1, "", 12.5f, 400);
        const auto t2 = std::chrono::steady_clock::now();
        (void)core::TextPrimitive::measureTextWidth(s2, "", 12.5f, 400);
        const auto t3 = std::chrono::steady_clock::now();
        const double s1Ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        const double s2Ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::cout << "MICRO s1Ms=" << std::fixed << s1Ms << " s2Ms=" << s2Ms
                  << " (同词表第二串，advance 缓存敏感)" << NL;
        A.expect(s2Ms < 5.0,
                 "同词表第二串 shape " + std::to_string(s2Ms) +
                     "ms ≥ 5ms（advance 无缓存，逐字形 autofit 重算）");
    }

    // 切审核视图（种子 120 条全 match：filterType/Src/State 默认 0 = 全部/待审）
    auto* mem = dynamic_cast<module::memory::MemoryModule*>(mod.get());
    if (mem == nullptr) {
        std::cerr << "memory_freeze_probe: FAIL——模块类型转换失败\n";
        probe::terminateWindowBackend();
        return 4;
    }
    mem->setActiveViewForTest(1);

    auto composeFrame = [&]() {
        runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
        runtime.compose(pageId, w, h, [&](eui::Ui& u, const eui::Screen& screen) {
            A.ui = &u;
            mod->compose(u, screen);
        });
    };
    // 计时只包 compose（= gdb 冻结的那段：buildLayoutNode + layout + 文本度量）
    auto timedComposeMs = [&]() {
        const auto t0 = std::chrono::steady_clock::now();
        runtime.compose(pageId, w, h, [&](eui::Ui& u, const eui::Screen& screen) {
            A.ui = &u;
            mod->compose(u, screen);
        });
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    composeFrame();  // 预热主题/字体栈（不计入判定）

    // ── 组 R①：审核视图冷帧（首帧 = 全量 shaping）──
    const double coldMs = timedComposeMs();
    std::cout << "COMPOSE coldMs=" << std::fixed << coldMs << NL;

    // ── 组 R②：热帧（TextSizeCache 已热；真实场景每帧重 compose，LRU 1024
    // 装不下 120 卡 × 十余文本键 → 持续驱逐，热帧 ≈ 冷帧）──
    const double warmMs = timedComposeMs();
    std::cout << "COMPOSE warmMs=" << std::fixed << warmMs << NL;

    // 渲染卡数（上限截断判据）
    int rendered = 0;
    {
        auto prop = module::memory::proposals::ProposalStore::open(
            dataDir / "proposals.db");
        auto pend = prop.value().listPending();
        if (pend.isOk()) {
            for (const auto& p : pend.value()) {
                if (A.elem("memory.rv.card." + p.id) != nullptr) ++rendered;
            }
        }
    }
    std::cout << "CARDS rendered=" << rendered << " seeded=" << kSeedPending
              << " elementsPerCard=17 accounting=" << rendered * 17 << NL;

    // 红判据①：卡流必须被上限截断（150 内 = 默认上限 50 可见；无上限实现 =
    // rendered == 120 = 红）
    A.expect(rendered < kSeedPending,
             "审核视图未截断：rendered=" + std::to_string(rendered) +
                 " == 种子全量（bug190 卡流无上限）");
    // 红判据②：热帧必须 < 250ms（≈15 帧预算；真实规模下 TextSizeCache 超容量
    // 驱逐 = 每帧全量重 shape）；冷帧含首次字体加载/AV 噪声，放宽到 800ms
    //（红态实测 2311~3210ms，裕度 ~3x）
    A.expect(warmMs < 250.0,
             "热帧 compose " + std::to_string(warmMs) +
                 "ms ≥ 250ms（帧预算×15，bug190 冻结机制在）");
    A.expect(coldMs < 800.0,
             "冷帧 compose " + std::to_string(coldMs) +
                 "ms ≥ 800ms（红态实测 2311ms+）");

    // ── 组 R③：渐进加载「再显示 50 条」（仅截断生效时执行；红态跳过防慢帧
    // 拖爆探针时长）。按钮内容位深在 50 卡 × 244px ≈ 12k px——用模块滚动偏移
    // 测试缝直驱滚到底（scrollView builder 内部 clamp(offset, 0, maxOffset)
    // 吸底，尾行必然入视口），再点击验证渐进加载。──
    if (rendered < kSeedPending) {
        A.exists("memory.rv.more");
        A.exists("memory.rv.more.note");
        mem->setReviewScrollForTest(1000000.0f);  // 吸底（builder 内部钳制）
        composeFrame();  // 帧1：新 offset 进元素（程序性跳变，update+compose）
        composeFrame();  // 帧2：update 同步 instance → 平移生效
        {
            const auto* se = A.elem("memory.review.scroll");
            std::cout << "SCROLLDBG scrollEl="
                      << (se != nullptr ? "found" : "null")
                      << " offset=" << (se != nullptr ? se->scrollOffset : -1.f)
                      << " maxOffset="
                      << (se != nullptr ? se->scrollMaxOffset : -1.f)
                      << " stateId="
                      << (se != nullptr && !se->scrollStateId.empty() ? "set"
                                                                      : "empty")
                      << NL;
        }
        const auto* more = A.elem("memory.rv.more.bg");
        if (more == nullptr) {
            std::cerr << "memory_freeze_probe: FAIL——滚到底后 more.bg 缺失\n";
            A.passed = false;
        } else {
            // 滚动平移是渲染期 transform（scrollTransformForElement 读
            // instance offset），元素 frame.y 恒为未平移布局位；屏幕位 =
            // frame.y − 实际滚动 offset（吸底 = scrollMaxOffset，帧2 已同步）。
            const auto* scrollEl = A.elem("memory.review.scroll");
            const float scrollApplied =
                scrollEl != nullptr
                    ? std::min(scrollEl->scrollMaxOffset,
                               std::max(0.0f, scrollEl->scrollOffset))
                    : 0.0f;
            std::cout << "SCROLL moreLayoutY=" << std::fixed
                      << more->frame.y << " appliedOffset=" << scrollApplied
                      << NL;
            const float cx = more->frame.x + more->frame.width * 0.5f;
            const float cy =
                more->frame.y - scrollApplied + more->frame.height * 0.5f;
            const std::string hitAtMore =
                runtime.hitTestInteractiveAt(static_cast<double>(cx),
                                             static_cast<double>(cy), 1.0f);
            std::cout << "HIT more.bg.center -> " << hitAtMore << NL;
            if (cy < 0.0f || cy > h ||
                hitAtMore.find("memory.rv.more") == std::string::npos) {
                std::cerr << "memory_freeze_probe: FAIL——渐进加载按钮屏幕位 ("
                          << cx << "," << cy << ") 命中 '" << hitAtMore
                          << "'（不在视口/点击链被盖）\n";
                A.passed = false;
            } else {
                const double scrolledMs = timedComposeMs();
                std::cout << "COMPOSE scrolledMs=" << std::fixed << scrolledMs
                          << NL;
                core::queuePointerMotion(window, cx, cy, core::PointerButtons{},
                                         core::KeyModifiers{});
                core::queuePointerButton(window, cx, cy,
                                         core::PointerButton::Left,
                                         core::PointerAction::Press, {});
                core::queuePointerButton(window, cx, cy,
                                         core::PointerButton::Left,
                                         core::PointerAction::Release, {});
                composeFrame();  // update 消费点击
                const double afterMoreMs = timedComposeMs();
                std::cout << "COMPOSE afterMoreMs=" << std::fixed
                          << afterMoreMs << NL;
                int rendered2 = 0;
                auto prop = module::memory::proposals::ProposalStore::open(
                    dataDir / "proposals.db");
                auto pend = prop.value().listPending();
                if (pend.isOk()) {
                    for (const auto& p : pend.value()) {
                        if (A.elem("memory.rv.card." + p.id) != nullptr)
                            ++rendered2;
                    }
                }
                std::cout << "CARDS afterMore=" << rendered2 << NL;
                A.expect(rendered2 > rendered,
                         "点「再显示 50 条」后卡数未增长：" +
                             std::to_string(rendered) + " → " +
                             std::to_string(rendered2));
                A.expect(afterMoreMs < 250.0,
                         "加载后热帧 " + std::to_string(afterMoreMs) +
                             "ms ≥ 250ms");
            }
        }
    }

    std::cout << "memory_freeze_probe: " << (A.passed ? "PASS" : "FAIL")
              << " · 数据根 " << tmpRoot.generic_string() << "\n";
    mod->onStop();
    probe::terminateWindowBackend();
    return A.passed ? 0 : 1;
}
