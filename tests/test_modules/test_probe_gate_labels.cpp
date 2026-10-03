// tests/test_modules/test_probe_gate_labels.cpp
// 「真机出网探针绝不进默认门禁」——**默认门禁里跑的**代码断言。
//
// ── 这条用例为什么存在（静态审计 2026-10-03 B2）────────────────────
// tests/CMakeLists.txt 与 scripts/run-quota-live-probe.sh 都曾写着
// 「这条由 tests/test_modules/test_probe_gate_labels.cpp 的单测逐条断言
//（改坏即全量红），不是靠注释保证」。**那个文件当时根本不存在。**
// 注释说有、实际没有，正是本项目反复翻车的那一类「注释断言冒充代码断言」。
// 后果比「注释错了」更具体：scripts/check-probe-gate-labels.sh 的 --self-test
// 是**手动**跑的，build-test-run.sh 只跑它的非 self-test 分支 → **没有任何东西
// 定期证明这道守卫还活着**。本文件把那句话变成真的。
//
// ── 为什么用 C++ 再说一遍（而不是只靠那个 shell 守卫）───────────────
// 两者的解析路径**完全独立**：那个守卫用 awk 状态机，本用例用 C++ 逐行扫。
// 一道守卫只有一个实现时，「守卫的解析逻辑被改坏」与「被守的东西被改坏」
// 会一起发生、互相掩盖，谁也发现不了。双实现下，其中一个被改坏时另一个
// 仍然是有效信号 —— 这是**有意的重复**，不是「改一处漏一处」。
//
// ── 仓库根定位：cwd 上溯，**不用** __FILE__ ──────────────────────────
// ccache 会把 __FILE__ 相对化，靠它上溯的守卫从别的 cwd 跑必误报
//（TIN-01 的实测事故，见 test_settings.cpp 的同款 findRepoRoot）。
// 定位失败时**大声报红**，不静默绿 —— 「找不到仓库根就当通过」是最坏的一种绿。

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path findRepoRoot() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec);
    if (ec) return {};
    for (int up = 0; up < 8; ++up) {
        if (std::filesystem::exists(dir / "src" / "core" / "proxy_port.h", ec) &&
            std::filesystem::exists(dir / "tests" / "CMakeLists.txt", ec)) {
            return dir;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

// 条目块 = 从 `add_test(NAME quota_live_probe` 起，到**下一个 `add_test(` 或
// 文件尾**。⚠ 这里的口径必须与 scripts/check-probe-gate-labels.sh 一致，且
// **不能用固定行数窗口**：静态审计 M3 亲手构造过「标签挪到第 20 行 → 旧守卫
// 报绿、断言 B/C 双双失效」的反证。窗口宽度本身就是要人同步的隐式不变式。
std::vector<std::string> quotaLiveProbeBlock(const std::vector<std::string>& lines) {
    std::vector<std::string> block;
    bool inBlock = false;
    for (const std::string& line : lines) {
        const bool isAddTest = line.find("add_test(") != std::string::npos;
        if (isAddTest) {
            if (inBlock) break;          // 上一个块到此为止
            if (line.find("add_test(NAME quota_live_probe") != std::string::npos) {
                inBlock = true;
            }
        }
        if (inBlock) block.push_back(line);
    }
    return block;
}

// 抽 `set_tests_properties(quota_live_probe ... LABELS "<值>" ...)` 里的标签串。
// 返回空串 = 该条目没有可解析的 LABELS。
std::string extractLabels(const std::vector<std::string>& block) {
    for (const std::string& line : block) {
        if (line.find("set_tests_properties(quota_live_probe") == std::string::npos) {
            continue;
        }
        const size_t labels = line.find("LABELS");
        if (labels == std::string::npos) continue;
        const size_t q1 = line.find('"', labels);
        if (q1 == std::string::npos) continue;
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        return line.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

// 标签串 → 规范化集合（分号分隔、去空格、排序、尾随逗号），便于比较
std::string normalizeLabels(const std::string& raw) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : raw) {
        if (c == ';') { parts.push_back(cur); cur.clear(); }
        else if (!std::isspace(static_cast<unsigned char>(c))) cur.push_back(c);
    }
    parts.push_back(cur);
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const std::string& p : parts) { out += p; out += ","; }
    return out;
}

}  // namespace

REGISTER_TEST(probe_gate_labels_guard_is_live,
              "probe_gate_labels: 真机探针挂 live 标签、不挂 probe、门禁不调它（A/B/C/D）", {
    const std::filesystem::path root = findRepoRoot();
    DOCTEST_REQUIRE_MESSAGE(!root.empty(),
                             "护栏无法定位仓库根（cwd=" +
                                 std::filesystem::current_path().string() +
                                 "）：请从仓库根或 build/ 下运行 devdesk_tests");

    // ── 断言 A：恰好一条 add_test(NAME quota_live_probe ──────────────
    const std::vector<std::string> cmakeLines =
        readLines(root / "tests" / "CMakeLists.txt");
    DOCTEST_REQUIRE_MESSAGE(!cmakeLines.empty(),
                             "读不到 tests/CMakeLists.txt（护栏无法取证）");
    int addTestCount = 0;
    for (const std::string& l : cmakeLines) {
        if (l.find("add_test(NAME quota_live_probe") != std::string::npos) ++addTestCount;
    }
    DOCTEST_CHECK_MESSAGE(addTestCount == 1,
                          "期望恰好 1 条 add_test(NAME quota_live_probe)，实到 " +
                              std::to_string(addTestCount) +
                              " 条（0 = 入口被删；≥2 = 有人复制了一份）");

    const std::vector<std::string> block = quotaLiveProbeBlock(cmakeLines);
    // ⚠ **非恒真守卫的第一道闸**：块抽取本身必须真的抽到了东西。
    // 「过滤器匹配 0 条时 Total: 0 不等于没红」是本项目踩过的坑 ——
    // 块抽取规则一改（加了缩进、改了拼写），后面几条断言会全部静默通过。
    DOCTEST_REQUIRE_MESSAGE(!block.empty(),
                             "条目块抽取失败：块必须非空（抽取规则已与文件失配）");
    bool blockHasAddTest = false;
    for (const std::string& l : block) {
        if (l.find("add_test(NAME quota_live_probe") != std::string::npos) {
            blockHasAddTest = true;
        }
    }
    DOCTEST_REQUIRE_MESSAGE(blockHasAddTest,
                             "抽到的块里没有 add_test 本行 —— 块抽取已错位");

    // ── 断言 B：标签集合恰好 {live} ──────────────────────────────────
    const std::string raw = extractLabels(block);
    DOCTEST_REQUIRE_MESSAGE(!raw.empty(),
                             "quota_live_probe 条目没有可解析的 LABELS —— "
                             "块抽取没覆盖到 set_tests_properties，或它被删/被改了形态");
    const std::string norm = normalizeLabels(raw);
    DOCTEST_CHECK_MESSAGE(norm == "live,",
                          "标签集合期望恰好 {live}，实到 {" + norm + "}（raw=\"" +
                              raw + "\"）");

    // ── 断言 C：不含 probe（否则进默认门禁 = 真打四家供应商 = CI 必红）──
    // ⚠ 判据写「**整串里找不到 probe**」，不是「找得到 probe 就红」的反面写法。
    // 上一版误写成 `norm.find("probe") == 0`（想表达「不以 probe 开头」），
    // 那条判据对**正确**的 {live} 也为假 → 恒红。用例一上线即红，当场拆穿。
    // 恒红守卫比没有守卫更糟：它会让人习惯性地忽略这条红，最后连真红也一起忽略。
    DOCTEST_CHECK_MESSAGE(norm.find("probe") == std::string::npos,
                          "标签含 probe：门禁跑的是 ctest -L probe，含它即进默认"
                          "门禁 = 真打网络 = CI 必红（raw=\"" + raw + "\"）");

    // ── 断言 D：门禁脚本不得调用真机探针 ─────────────────────────────
    const std::vector<std::string> gateLines =
        readLines(root / "scripts" / "build-test-run.sh");
    DOCTEST_REQUIRE_MESSAGE(!gateLines.empty(),
                             "读不到 scripts/build-test-run.sh（护栏无法取证）");
    for (const std::string& l : gateLines) {
        DOCTEST_CHECK_MESSAGE(l.find("run-quota-live-probe.sh") == std::string::npos,
                              "门禁脚本里出现了 run-quota-live-probe.sh —— "
                              "build-test-run.sh 是常规门禁入口，不得调用真网络探针");
    }
    return true;
})