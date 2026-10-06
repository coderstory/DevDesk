// tests/test_modules/test_probe_gate_labels.cpp
// 断言 tests/CMakeLists.txt 里真机出网探针 quota_live_probe 的标签与门禁接线（标签 / 门禁脚本）。
// 跨文件前提：
//   - scripts/check-probe-gate-labels.sh 与本文件是**有意的重复实现**，解析路径彼此独立
//     （那边 awk 状态机、这边 C++ 逐行扫）。单实现时「守卫的解析被改坏」与「被守的东西被改坏」
//     同时发生、互相掩盖。合并成一处会同时废掉这道双保险，不许合并。
//   - 默认门禁 scripts/build-test-run.sh 不跑那个 shell 守卫的 --self-test 分支，
//     本文件是唯一会定期执行到的那一半，因此不能删。
//   - 仓库根靠 cwd 上溯，不用 __FILE__：ccache 会把 __FILE__ 相对化，靠它定位从别的 cwd 跑必误报。
//     与 test_settings.cpp 的 findRepoRoot 同款重复实现，同样是刻意的，不要跨文件合并。

#include "doctest.h"

#include <algorithm>
#include <cctype>
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

// 条目块 = 从 `add_test(NAME quota_live_probe` 起，到下一个 `add_test(` 或文件尾。
// 口径必须与 scripts/check-probe-gate-labels.sh 一致。不能用固定行数窗口：标签一旦被挪出
// 窗口，守卫照报绿而下面两条断言双双失效——窗口宽度是要人同步的隐式不变式。
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
// 返回空串 = 该条目没有可解析的 LABELS（调用方据此 REQUIRE 失败，而不是当作「无标签」放过）。
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

    // 断言 A：恰好一条 add_test(NAME quota_live_probe
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
    // 非恒真守卫的第一道闸：块抽取本身必须真的抽到了东西。「过滤器匹配 0 条时 Total: 0
    // 不等于没红」同类坑——块抽取规则一改（加了缩进、改了拼写），后面几条断言全部静默通过。
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

    // 断言 B：标签集合恰好 {live}
    const std::string raw = extractLabels(block);
    DOCTEST_REQUIRE_MESSAGE(!raw.empty(),
                             "quota_live_probe 条目没有可解析的 LABELS —— "
                             "块抽取没覆盖到 set_tests_properties，或它被删/被改了形态");
    const std::string norm = normalizeLabels(raw);
    DOCTEST_CHECK_MESSAGE(norm == "live,",
                          "标签集合期望恰好 {live}，实到 {" + norm + "}（raw=\"" +
                              raw + "\"）");

    // 断言 C：不含 probe（否则进默认门禁 = 真打四家供应商 = CI 必红）
    // 判据是「整串里找不到 probe」，不可简化成 norm.find("probe") == 0——那表达的是「不以 probe
    // 开头」，对正确的 {live} 也为假，变成恒红守卫。恒红守卫比没有守卫更糟：人会习惯性地
    // 忽略这条红，最后连真红也一起忽略。
    DOCTEST_CHECK_MESSAGE(norm.find("probe") == std::string::npos,
                          "标签含 probe：门禁跑的是 ctest -L probe，含它即进默认"
                          "门禁 = 真打网络 = CI 必红（raw=\"" + raw + "\"）");

    // 断言 D：门禁脚本不得调用真机探针
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