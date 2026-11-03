// tests/test_modules/test_wallpaper_importer.cpp
// 「从花見导入」的纯逻辑：身份签名（文件名+字节大小）与去重判定矩阵。
//
// 真文件系统动作（枚举/拷贝/墓碑 SQLite/废纸篓回填）归 probe 与真机验证；
// 这里只钉「签名拼得对」与「判定顺序对」——两者错了会静默导错/漏导。
// REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
#include "doctest.h"

#include "module_wallpaper/core/importer.h"

#include <string>

namespace {

namespace wp = module::wallpaper;

} // namespace

// ── 身份签名：文件名 + \x1f + 字节大小 ──

REGISTER_TEST(wallpaper_importer_signature_basic,
              "wallpaper importer: 签名 = 文件名 + \\x1f 分隔符 + 字节大小", {
    // 字面量拆开写："\x1f123" 会把 123 整个吃进十六进制转义（超出 char 范围）。
    DOCTEST_CHECK_EQ(wp::importer::makeSignature("a.mp4", 123),
                     std::string("a.mp4\x1f"
                                 "123"));
    DOCTEST_CHECK_EQ(wp::importer::makeSignature("", 0),
                     std::string("\x1f"
                                 "0"));
    return true;
})

REGISTER_TEST(wallpaper_importer_signature_separator_prevents_collision,
              "wallpaper importer: 分隔符消除拼接歧义——(a1,2) 与 (a,12) 签名不同", {
    // 没有分隔符时两者都会拼成 "a12"：同名同大小误判会让一个素材被删、
    // 另一个被冤枉跳过。\\x1f 在合法文件系统名里不会出现，歧义不复存在。
    const std::string first = wp::importer::makeSignature("a1", 2);
    const std::string second = wp::importer::makeSignature("a", 12);
    DOCTEST_CHECK_NE(first, second);
    DOCTEST_CHECK_NE(first, std::string("a12"));
    DOCTEST_CHECK_NE(second, std::string("a12"));
    return true;
})

REGISTER_TEST(wallpaper_importer_signature_distinguishes_name_and_size,
              "wallpaper importer: 同名不同大小 / 同大小不同名 → 签名不同", {
    DOCTEST_CHECK_NE(wp::importer::makeSignature("v.mp4", 100),
                     wp::importer::makeSignature("v.mp4", 101));
    DOCTEST_CHECK_NE(wp::importer::makeSignature("v.mp4", 100),
                     wp::importer::makeSignature("w.mp4", 100));
    return true;
})

// ── 去重判定矩阵 ──

REGISTER_TEST(wallpaper_importer_decide_import,
              "wallpaper importer: 无冲突候选判 Import", {
    wp::importer::CandidateFacts facts;
    facts.signatureOk = true;
    facts.hasVideoTrack = true;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::Import);
    return true;
})

REGISTER_TEST(wallpaper_importer_decide_existing_wins_first,
              "wallpaper importer: 目标已存在最优先——签名没算出/墓碑命中也报 SkipExisting", {
    // 顺序即语义：exists-check 必须在签名之前——存在就跳过，连 stat 都省一次；
    // 且目标==来源时绝不写 .tmp。
    wp::importer::CandidateFacts facts;
    facts.targetExists = true;
    facts.signatureOk = false;
    facts.inTombstone = true;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::SkipExisting);
    return true;
})

REGISTER_TEST(wallpaper_importer_decide_stat_failure,
              "wallpaper importer: 大小取不到判 FailStat（绝不静默放行）", {
    wp::importer::CandidateFacts facts;
    facts.targetExists = false;
    facts.signatureOk = false;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::FailStat);
    return true;
})

REGISTER_TEST(wallpaper_importer_decide_tombstone_before_probe,
              "wallpaper importer: 墓碑命中先于视频轨探针——命中即可跳过、省一次探测", {
    // 平台层在墓碑命中时不再探视频轨（hasVideoTrack 留 false），
    // 判定必须仍然落在 SkipTombstone 而不是 RejectNoVideoTrack。
    wp::importer::CandidateFacts facts;
    facts.signatureOk = true;
    facts.inTombstone = true;
    facts.hasVideoTrack = false;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::SkipTombstone);
    return true;
})

REGISTER_TEST(wallpaper_importer_decide_no_video_track,
              "wallpaper importer: 探针判定无视频轨判 RejectNoVideoTrack", {
    wp::importer::CandidateFacts facts;
    facts.signatureOk = true;
    facts.hasVideoTrack = false;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::RejectNoVideoTrack);
    return true;
})

// ── 身份判据的既定取舍（用户决策，钉住防止将来无意识改回去）──
// 内容相同但文件名不同的文件（dup-of-a.mp4）不再去重——识别「同一个素材」
// 靠文件名+大小，不做整文件内容哈希（视频几十上百 MB，导入不该整读一遍）。

REGISTER_TEST(wallpaper_importer_same_content_other_name_is_import,
              "wallpaper importer: 内容相同但名字不同 → 仍判 Import（名字+大小判据的既定取舍）", {
    wp::importer::CandidateFacts facts;
    facts.signatureOk = true;  // dup-of-a.mp4：名字不同 → 签名不同
    facts.hasVideoTrack = true;
    DOCTEST_CHECK(wp::importer::decide(facts) ==
                  wp::importer::CandidateVerdict::Import);
    return true;
})

// ── 报告计数：每个判定落到各自分项，不串味 ──
// 分项合并会让「到底被哪一道吃掉的」无法定位（用户问「为什么没导进来」时答不出）。

REGISTER_TEST(wallpaper_importer_record_verdict_per_field,
              "wallpaper importer: 四种跳过判定各记各的分项（不合并计数）", {
    wp::importer::ImportReport report;
    wp::importer::recordVerdict(wp::importer::CandidateVerdict::SkipExisting, report);
    wp::importer::recordVerdict(wp::importer::CandidateVerdict::SkipTombstone, report);
    wp::importer::recordVerdict(wp::importer::CandidateVerdict::FailStat, report);
    wp::importer::recordVerdict(wp::importer::CandidateVerdict::RejectNoVideoTrack,
                                report);
    DOCTEST_CHECK_EQ(report.skippedExisting, 1);
    DOCTEST_CHECK_EQ(report.skippedTombstone, 1);
    DOCTEST_CHECK_EQ(report.failedCopy, 1);        // 取不到大小计入 failedCopy
    DOCTEST_CHECK_EQ(report.rejectedByProbe, 1);
    DOCTEST_CHECK_EQ(report.imported, 0);          // Import 本身不计数（由拷贝结果决定）
    return true;
})

REGISTER_TEST(wallpaper_importer_record_import_not_counted,
              "wallpaper importer: Import 判定本身不计 imported（由真实拷贝成功决定）", {
    wp::importer::ImportReport report;
    wp::importer::recordVerdict(wp::importer::CandidateVerdict::Import, report);
    DOCTEST_CHECK_EQ(report.imported, 0);
    DOCTEST_CHECK_EQ(report.failedCopy, 0);
    return true;
})
