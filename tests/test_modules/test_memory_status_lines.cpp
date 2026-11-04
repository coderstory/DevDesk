// tests/test_modules/test_memory_status_lines.cpp
// 记忆页 bug203/bug217 修复的单测层：
//   bug203 — 引擎卡 python 行与 embedding 警示行独立呈现（原 pythonLine 三分支
//            pythonFound 优先，llamaMissing 文案在复合态永不可见）；
//   bug217 — 审核卡/已裁决卡文本按像素预算截断（EUI 文本溢出不裁剪，固定行盒
//            曾被 ~1975 CJK 长文纵向/横向溢出叠印）；
//   附：数据根覆盖的 MSYS 形态路径陷阱（真机「记忆审核 (0)」观察的最可能口径：
//       "/tmp/..." 在 Windows 落为当前盘相对路径 → dbDir 不存在 → 首跑静默零账面）
//       与提案库大表全量加载回归（listPending 无截断口径）。
// 全部纯函数/临时目录，零网络零子进程。
#include "doctest.h"

#include "test_support.h"

#include "module_memory/memory_module_internal.h"
#include "module_memory/proposals/proposal_store.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace engine = module::memory::engine;
namespace proposals = module::memory::proposals;

namespace {

engine::TelememEngine::Snapshot makeSnap(bool pythonFound, bool llamaMissing) {
    engine::TelememEngine::Snapshot s;
    s.state = engine::TelememEngine::State::Idle;
    s.pythonVersion = pythonFound ? "3.13.15" : "";
    s.pythonFound = pythonFound;
    s.llamaMissing = llamaMissing;
    return s;
}

}  // namespace

// ── bug203：状态行独立 ──

REGISTER_TEST(memory_status_lines_composite_unmasked,
              "memory bug203: pythonFound+llamaMissing 复合态两行同现（原三分支遮蔽）",
              {
    const auto snap = makeSnap(true, true);
    const auto py = module::memory::engineStatusPythonLine(snap);
    const auto embed = module::memory::engineStatusEmbedLine(snap);
    DOCTEST_CHECK_MESSAGE(py.find("已检测") != std::string::npos,
                          "python 行在复合态仍须呈现已检测文案");
    DOCTEST_CHECK_MESSAGE(py.find("Python 3.13.15") != std::string::npos,
                          "python 行携带探测版本");
    DOCTEST_CHECK_MESSAGE(embed.find("本机模型文件缺失") != std::string::npos,
                          "llamaMissing 醒目警示行必须在场（原实现被 pythonFound 优先分支遮蔽）");
    DOCTEST_CHECK_MESSAGE(embed.find("见首次安装向导第③步") != std::string::npos,
                          "警示行指路向导③步（need:202 F-06 逐字口径）");
    return true;
})

REGISTER_TEST(memory_status_lines_missing_python_fallback,
              "memory bug203: python 缺失态两行各自兜底、无缺失不出警示行", {
    const auto snap = makeSnap(false, false);
    const auto py = module::memory::engineStatusPythonLine(snap);
    const auto embed = module::memory::engineStatusEmbedLine(snap);
    DOCTEST_CHECK(py.find("未检测到 Python") != std::string::npos);
    DOCTEST_CHECK(embed.empty());
    const auto snap2 = makeSnap(false, true);
    DOCTEST_CHECK(module::memory::engineStatusPythonLine(snap2)
                      .find("未检测到 Python") != std::string::npos);
    DOCTEST_CHECK(module::memory::engineStatusEmbedLine(snap2)
                      .find("本机模型文件缺失") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_status_lines_embed_cleared,
              "memory bug203: llamaMissing 清除后警示行消失", {
    const auto snap = makeSnap(true, false);
    DOCTEST_CHECK(module::memory::engineStatusEmbedLine(snap).empty());
    return true;
})

// ── need:202 F-08：Running 完成态区分语义档就绪口径 ──

REGISTER_TEST(memory_status_lines_embed_ready_running,
              "memory need202 F-08: Running+就位区分随包/数据根；非 Running 不出就绪行", {
    using SS = module::memory::engine::TelememEngine;
    auto snap = makeSnap(true, false);
    snap.state = SS::State::Running;
    snap.embedReady = true;
    snap.embedSource = SS::EmbedSource::Pkg;
    DOCTEST_CHECK_EQ(module::memory::engineStatusEmbedLine(snap),
                     std::string("语义档就绪（随包）"));
    snap.embedSource = SS::EmbedSource::DataRoot;
    DOCTEST_CHECK_EQ(module::memory::engineStatusEmbedLine(snap),
                     std::string("语义档就绪（数据根）"));
    snap.state = SS::State::Idle;
    DOCTEST_CHECK(module::memory::engineStatusEmbedLine(snap).empty());
    snap.embedReady = false;
    snap.state = SS::State::Running;
    DOCTEST_CHECK(module::memory::engineStatusEmbedLine(snap).empty());
    return true;
})

// ── need:202 F-04：向导③步五态文案映射 ──

REGISTER_TEST(memory_wizard_embed_status_five_states,
              "memory need202 F-04: 向导③步五态文案逐字（设计 §2.2）", {
    using SS = module::memory::engine::TelememEngine;
    module::memory::TelememEngine::Snapshot s;
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("待开始"));  // Unknown
    s.embedState = SS::EmbedState::NeedDownload;
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("需下载（联网，首次约 26MB）"));
    s.embedState = SS::EmbedState::Downloading;
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("进行中…"));
    s.embedDetail = "准备 embedding 模型（首次需下载）…";
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     s.embedDetail);
    s.embedState = SS::EmbedState::Ready;
    s.embedSource = SS::EmbedSource::Pkg;
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("已就位（随包）"));
    s.embedSource = SS::EmbedSource::DataRoot;
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("已就位（数据根）"));
    s.embedState = SS::EmbedState::Failed;
    s.embedDetail = "下载中断: net";
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("失败：下载中断: net（可重试）"));
    s.embedDetail.clear();
    DOCTEST_CHECK_EQ(module::memory::wizardEmbedStatusText(s),
                     std::string("失败：本机包缺失（可重试）"));
    return true;
})

// ── bug217：像素预算截断 ──

REGISTER_TEST(memory_truncate_within_budget_untouched,
              "memory bug217: 预算内文本原样保留、空串安全", {
    DOCTEST_CHECK_EQ(module::memory::truncateByAdvance("短文本", 500.0f, 12.5f),
                     std::string("短文本"));
    DOCTEST_CHECK(module::memory::truncateByAdvance("", 100.0f, 12.5f).empty());
    return true;
})

REGISTER_TEST(memory_truncate_utf8_boundary_ellipsis,
              "memory bug217: 超预算按 UTF-8 码点边界截断并以省略号收尾", {
    std::string longText;
    for (int i = 0; i < 2000; ++i) longText += "记";
    const auto out =
        module::memory::truncateByAdvance(longText, 300.0f, 12.5f);
    DOCTEST_CHECK(out.size() < longText.size());
    DOCTEST_REQUIRE(out.size() >= 3);
    DOCTEST_CHECK(out.compare(out.size() - 3, 3, "\xE2\x80\xA6") == 0);
    const std::string body = out.substr(0, out.size() - 3);
    DOCTEST_CHECK_MESSAGE(body.size() % 3 == 0,
                          "截断边界不得切碎 UTF-8 码点");
    DOCTEST_CHECK(static_cast<float>(body.size() / 3) * 12.5f + 12.5f <=
                  300.0f + 0.01f);
    return true;
})

REGISTER_TEST(memory_truncate_ascii_halfwidth,
              "memory bug217: ASCII 半宽口径（预算 110px 减预留 10px 字号 10 = 20 个 ASCII）",
              {
    const std::string s20(20, 'a');
    DOCTEST_CHECK_EQ(module::memory::truncateByAdvance(s20, 110.0f, 10.0f), s20);
    const auto out21 =
        module::memory::truncateByAdvance(std::string(21, 'a'), 110.0f, 10.0f);
    DOCTEST_CHECK_EQ(out21.size(), static_cast<std::size_t>(20) + 3);
    return true;
})

// ── 附：数据根 MSYS 形态陷阱（真机「记忆审核 (0)」口径） ──

#ifdef _WIN32
REGISTER_TEST(memory_data_root_msys_form_trap,
              "memory: MSYS 形态数据根在 Windows 是盘相对路径 → dbDir 落空（审核 (0) 口径）",
              {
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           "/tmp/devdesk-mem-root-trap");
    const char* env = std::getenv("DEVDESK_DATA_ROOT");
    const std::filesystem::path root(env != nullptr ? env : "");
    DOCTEST_CHECK_FALSE(root.is_absolute());
    DOCTEST_CHECK_FALSE(std::filesystem::exists(root));
    return true;
})
#endif

// ── 附：提案库大表全量加载回归（listPending 无截断，1629 真库口径） ──

REGISTER_TEST(memory_list_pending_full_load_2000,
              "memory bug217 前置: listPending 全量加载 2000 条 pending 不截断", {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("mem-listpending-test-" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::filesystem::create_directories(tmp);
    auto opened = proposals::ProposalStore::open(tmp / "proposals.db");
    DOCTEST_REQUIRE(opened.isOk());
    auto& store = opened.value();
    const std::int64_t now = 1793766937;  // 对齐真库批量落库秒
    constexpr int kCount = 2000;
    int created = 0;
    for (int i = 0; i < kCount; ++i) {
        auto r =
            store.create("add", {}, "提案正文 " + std::to_string(i), "", "", {},
                         0.5, "collect", "assistant_derived", now);
        if (r.isOk()) ++created;
    }
    DOCTEST_CHECK_EQ(created, kCount);
    auto pend = store.listPending();
    DOCTEST_REQUIRE(pend.isOk());
    DOCTEST_CHECK_MESSAGE(
        static_cast<int>(pend.value().size()) == kCount,
        "listPending 必须全量返回（无隐式 LIMIT，1629 真库口径）");
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    return true;
})
