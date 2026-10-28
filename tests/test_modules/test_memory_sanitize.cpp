// tests/test_modules/test_memory_sanitize.cpp
// 脱敏闸单测（S3-A）。红线：红队夹具（tools/memory-lab/sanitize/datasets/redteam.json）
// 泄漏 0、误杀 0——夹具任何一条被静默跳过都算护栏失效（样本数下限钉死）。
#include "doctest.h"

#include "core/fs_text.h"
#include "module_memory/sanitize/sanitize_gate.h"
#include "platform/json.h"

#include <filesystem>
#include <string>
#include <vector>

using module::memory::sanitize::SanitizeGate;
using module::memory::sanitize::sanitizeText;
using module::memory::sanitize::scanText;

namespace {

platform::json::JsonValue loadRedteamDataset() {
    // DEVDESK_SOURCE_DIR 由 tests/CMakeLists.txt 注入（与 DEVDESK_VERSION 同通道）
    static const std::string kPath =
        std::string(DEVDESK_SOURCE_DIR) +
        "/tools/memory-lab/sanitize/datasets/redteam.json";
    const auto parsed =
        platform::json::parse(core::readTextFile(std::filesystem::path(kPath)));
    return parsed.isOk() ? parsed.value() : platform::json::makeNull();
}

}  // namespace

REGISTER_TEST(memory_sanitize_redteam_zero_leak_zero_misfire,
              "memory: 红队夹具 110 条——泄漏 0 / 误杀 0 / 幂等（样本数下限钉死）", {
    const auto dataset = loadRedteamDataset();
    DOCTEST_REQUIRE(dataset.isArray());
    const int total = dataset.size();
    DOCTEST_REQUIRE_MESSAGE(total >= 100,
                            "红队样本数异常（夹具没被加载或被裁剪）: " +
                                std::to_string(total));
    int positives = 0;
    int negatives = 0;
    for (int i = 0; i < total; ++i) {
        const auto entry = dataset[i];
        const std::string text = entry.asObject().get("text").asString();
        const auto r = sanitizeText(text);
        DOCTEST_REQUIRE_MESSAGE(
            r.isOk(), std::string("夹具含非法 UTF-8? id=") +
                          entry.asObject().get("id").asString());
        const auto& out = r.value();
        if (entry.asObject().get("kind").asString() == "positive") {
            ++positives;
            const auto secrets = entry.asObject().get("secrets");
            for (int s = 0; s < secrets.size(); ++s) {
                const std::string secret = secrets[s].asString();
                DOCTEST_CHECK_MESSAGE(out.text.find(secret) == std::string::npos,
                                      std::string("泄漏: ") +
                                          entry.asObject().get("id").asString());
            }
            const auto expect = entry.asObject().get("expect_categories");
            bool catHit = false;
            for (const std::string& c : out.categories()) {
                for (int e = 0; e < expect.size(); ++e) {
                    if (expect[e].asString() == c) catHit = true;
                }
            }
            DOCTEST_CHECK_MESSAGE(catHit,
                                  std::string("期望类别未命中: ") +
                                      entry.asObject().get("id").asString());
            // 幂等：对输出再跑一遍不再变化
            const auto again = sanitizeText(out.text);
            DOCTEST_REQUIRE(again.isOk());
            DOCTEST_CHECK_EQ(again.value().text, out.text);
        } else {
            ++negatives;
            DOCTEST_CHECK_MESSAGE(out.hits.empty(),
                                  std::string("误杀: ") +
                                      entry.asObject().get("id").asString() + " 命中 " +
                                      (out.hits.empty()
                                           ? std::string()
                                           : out.hits[0].category));
            DOCTEST_CHECK_EQ(out.text, text);
        }
    }
    DOCTEST_CHECK(positives > 0);
    DOCTEST_CHECK(negatives > 0);
    return true;
})

REGISTER_TEST(memory_sanitize_preview_full_mask,
              "memory: preview 等长全遮蔽——全 '*' 且不含任何原文片段", {
    const auto r = sanitizeText("电话 13812345678 邮箱 a@b.com");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE_EQ(r.value().hits.size(), static_cast<std::size_t>(2));
    for (const auto& h : r.value().hits) {
        DOCTEST_CHECK_EQ(h.preview.size(), h.end - h.start);
        DOCTEST_CHECK(h.preview.find_first_not_of('*') == std::string::npos);
    }
    DOCTEST_CHECK_EQ(r.value().hits[0].category, std::string("phone"));
    DOCTEST_CHECK_EQ(r.value().hits[1].category, std::string("email"));
    // 输出只替换命中区间，其余字节不动
    DOCTEST_CHECK(r.value().text.find("电话 ") == 0);
    DOCTEST_CHECK(r.value().text.find("[REDACTED:phone]") != std::string::npos);
    DOCTEST_CHECK(r.value().text.find("[REDACTED:email]") != std::string::npos);
    return true;
})

REGISTER_TEST(memory_sanitize_fail_closed_invalid_utf8,
              "memory: fail-closed——非法 UTF-8 返回 Error（绝不当无命中放行）", {
    const std::string bad = "ok\xe4\xb8";  // 截断的多字节序列
    DOCTEST_CHECK(scanText(bad).isFail());
    DOCTEST_CHECK(sanitizeText(bad).isFail());
    const std::string overlong = "\xc0\xaf";  // 过长编码
    DOCTEST_CHECK(scanText(overlong).isFail());
    const std::string surrogate = "\xed\xa0\x80";  // 代理区
    DOCTEST_CHECK(scanText(surrogate).isFail());
    return true;
})

REGISTER_TEST(memory_sanitize_gate_default_on_and_bypass_audits,
              "memory: 闸默认开；关闭=旁路直通+审计行（行内无内容）", {
    std::string audit;
    SanitizeGate gate([&audit](const std::string& line) { audit = line; });
    DOCTEST_CHECK(gate.enabled());  // 默认开

    const std::string text = "手机 13812345678";
    auto gated = gate.process(text);
    DOCTEST_REQUIRE(gated.isOk());
    DOCTEST_CHECK(gated.value().hits.size() == 1);
    DOCTEST_CHECK(gated.value().text.find("13812345678") == std::string::npos);

    gate.setEnabled(false);
    auto bypass = gate.process(text);
    DOCTEST_REQUIRE(bypass.isOk());
    DOCTEST_CHECK(bypass.value().hits.empty());
    DOCTEST_CHECK_EQ(bypass.value().text, text);  // 原样旁路
    DOCTEST_CHECK(audit.find("bypass") != std::string::npos);
    DOCTEST_CHECK(audit.find("13812345678") == std::string::npos);  // 审计无内容
    return true;
})

REGISTER_TEST(memory_sanitize_detector_spot_checks,
              "memory: 六类检测抽检——kv/Bearer/私钥/JWT/校验位/allowlist", {
    // kv 键值（复合键 access_key_id 优先于前缀 access_key）
    auto r = sanitizeText("access_key_id=AKIAIOSFODNN7EXAMPLE");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().hits.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.value().hits[0].category, std::string("secret"));
    // Bearer（大小写精确）+ 普通语句不误切
    r = sanitizeText("Header Bearer abc123def456ghi789");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().hits[0].detail, std::string("bearer"));
    r = sanitizeText("Bearer authentication is required");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().hits.empty());
    // 私钥块（含无 END 的裸 BEGIN 脱到文本尾）
    r = sanitizeText("-----BEGIN RSA PRIVATE KEY-----\nMIIEpA\n-----END RSA PRIVATE KEY-----\ntail");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().hits[0].detail, std::string("private_key"));
    r = sanitizeText("-----BEGIN PRIVATE KEY-----\nMIIEpA-no-end");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().hits[0].detail, std::string("private_key_no_end"));
    // hits 区间指向原文；原文 41 字节、脱敏后仅剩占位符
    DOCTEST_CHECK_EQ(r.value().hits[0].end, static_cast<std::size_t>(41));
    DOCTEST_CHECK_EQ(r.value().text, std::string("[REDACTED:secret]"));
    // 18 位身份证校验位细节（合法 ok / 非法 bad 都命中）
    auto sr = scanText("110101199003078910");
    DOCTEST_REQUIRE(sr.isOk());
    DOCTEST_CHECK_EQ(sr.value()[0].detail, std::string("id18:checksum_ok"));
    sr = scanText("110101199003078919");
    DOCTEST_REQUIRE(sr.isOk());
    DOCTEST_CHECK_EQ(sr.value()[0].detail, std::string("id18:checksum_bad"));
    // allowlist：回环地址与文档假号码不脱敏
    r = sanitizeText("bind 127.0.0.1 or call 13800000000");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().hits.empty());
    // 全角变体经 shadow 1:1 归一后命中
    r = sanitizeText("全角 １３８１２３４５６７８");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().hits.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.value().hits[0].category, std::string("phone"));
    return true;
})
