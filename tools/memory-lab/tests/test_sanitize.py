"""脱敏层测试：六类形态、变体、allowlist、重叠合并、命中清单契约。"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.ensure_import_path()

from sanitize import Hit, SanitizeResult, sanitize, scan  # noqa: E402
from sanitize import core as sanitize_core  # noqa: E402


class PhoneTest(unittest.TestCase):
    def test_plain(self):
        r = sanitize("手机号 13812345678 收不到验证码")
        self.assertEqual(r.text, "手机号 [REDACTED:phone] 收不到验证码")
        self.assertEqual([(h.category, h.detail) for h in r.hits], [("phone", "cn_mobile")])

    def test_separated_variants(self):
        for text in ("138-1234-5678", "138 1234 5678"):
            with self.subTest(text=text):
                self.assertEqual(sanitize(text).categories, ["phone"])

    def test_country_code_variants(self):
        for text in ("+8613812345678", "86-13812345678", "+86 138 1234 5678"):
            with self.subTest(text=text):
                self.assertEqual(sanitize(text).categories, ["phone"])

    def test_fullwidth_digits(self):
        r = sanitize("１３８１２３４５６７８")
        self.assertEqual(r.text, "[REDACTED:phone]")
        self.assertEqual(r.categories, ["phone"])

    def test_allowlist_fake_number(self):
        r = sanitize("示例假号码 13800000000 不脱敏")
        self.assertEqual(r.hits, [])
        self.assertEqual(r.text, "示例假号码 13800000000 不脱敏")

    def test_timestamp_shapes_not_phone(self):
        for text in ("时间戳 1729012345678", "unix 秒 1729012345", "订单号 20261027123456"):
            with self.subTest(text=text):
                self.assertEqual(sanitize(text).hits, [])


class EmailTest(unittest.TestCase):
    def test_plain(self):
        r = sanitize("发到 alice.smith@example.com 即可")
        self.assertEqual(r.text, "发到 [REDACTED:email] 即可")

    def test_plus_tag_and_multi_level_domain(self):
        self.assertEqual(sanitize("a: user+tag@gmail.com").categories, ["email"])
        self.assertEqual(sanitize("<admin@company.com.cn>").categories, ["email"])

    def test_fullwidth_at(self):
        r = sanitize("全角 test＠qq.com")
        self.assertEqual(r.text, "全角 [REDACTED:email]")


class IdCardTest(unittest.TestCase):
    def test_valid_checksum(self):
        r = sanitize("身份证 110101199003078910")
        self.assertEqual(r.hits[0].detail, "id18:checksum_ok")
        self.assertEqual(r.categories, ["id_card"])

    def test_bad_checksum_still_redacted(self):
        # 校验位 awareness：非法校验位照样命中（泄漏 0 优先），只差 detail 标注
        r = sanitize("校验位错的 11010119900307891X 也要脱敏")
        self.assertEqual(r.hits[0].detail, "id18:checksum_bad")
        self.assertNotIn("11010119900307891X", r.text)

    def test_1880_birth_year_not_missed(self):
        r = sanitize("证件 440524188001010110 归档")
        self.assertNotIn("440524188001010110", r.text)

    def test_spaced_groups(self):
        self.assertNotIn("110101 19900307 8910", sanitize("分段 110101 19900307 8910").text)

    def test_legacy_15_digit(self):
        r = sanitize("旧 15 位 110101900307891")
        self.assertEqual(r.hits[0].detail, "id15")

    def test_fullwidth(self):
        self.assertEqual(sanitize("１１０１０１１９９００３０７８９１０").categories, ["id_card"])

    def test_order_number_not_id(self):
        self.assertEqual(sanitize("订单号 20261027123456").hits, [])


class Ipv4Test(unittest.TestCase):
    def test_plain_and_with_port(self):
        self.assertEqual(sanitize("服务器 192.168.1.100 挂了").categories, ["ipv4"])
        self.assertNotIn("10.0.0.42", sanitize("连接 10.0.0.42:5432 超时").text)

    def test_in_url(self):
        self.assertEqual(sanitize("后台 http://192.168.1.1/admin").categories, ["ipv4"])

    def test_version_numbers_not_ip(self):
        for text in ("v1.2.3", "依赖 1.2.3", "内核 2.4.108", "五段 1.2.3.4.5"):
            with self.subTest(text=text):
                self.assertEqual(sanitize(text).hits, [])

    def test_loopback_allowlist(self):
        for text in ("127.0.0.1", "0.0.0.0", "255.255.255.255"):
            with self.subTest(text=text):
                self.assertEqual(sanitize(f"地址 {text}").hits, [])


class UrlParamTest(unittest.TestCase):
    def test_token_value_removed_normal_kept(self):
        r = sanitize("https://api.example.com/v1/users?token=abc123def456&limit=10")
        self.assertNotIn("abc123def456", r.text)
        self.assertIn("limit=10", r.text)

    def test_case_insensitive_param_name(self):
        self.assertNotIn("Upper123", sanitize("https://api.p.com/v1?TOKEN=Upper123").text)

    def test_benign_url_untouched(self):
        text = "文档 https://example.com/docs?a=1&b=2"
        r = sanitize(text)
        self.assertEqual(r.hits, [])
        self.assertEqual(r.text, text)


class SecretTest(unittest.TestCase):
    def test_bearer_jwt_header(self):
        r = sanitize(
            "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0."
            "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJVadQssw5c"
        )
        self.assertEqual(r.categories, ["secret"])
        self.assertNotIn("eyJ", r.text)

    def test_bearer_plain_word_not_matched(self):
        self.assertEqual(sanitize("用 Bearer authentication 鉴权").hits, [])

    def test_vendor_keys(self):
        cases = {
            "aws": "AKIAIOSFODNN7EXAMPLE",
            "github": "ghp_16C7e42F292c6912E7710c838347Ae178B4a",
            "openai": "sk-abc123XYZdef456GHI789jkl",
            "slack": "xoxb-123456789012-234567890123-abcdefghij",
        }
        for name, key in cases.items():
            with self.subTest(name=name):
                self.assertNotIn(key, sanitize(f"key {key} 泄漏").text)

    def test_private_key_block_and_bare_begin(self):
        block = "-----BEGIN RSA PRIVATE KEY-----\nMIIEpA\n-----END RSA PRIVATE KEY-----"
        self.assertEqual(sanitize(f"头 {block} 尾").text, "头 [REDACTED:secret] 尾")
        r = sanitize("-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAA")
        self.assertTrue(r.text.endswith("[REDACTED:secret]"))

    def test_kv_credentials(self):
        for text, secret in (
            ("api_key = skS3cr3tValue99", "skS3cr3tValue99"),
            ("API_KEY: AbCd1234EfGh5678", "AbCd1234EfGh5678"),
            ('passwd: "abc123xyz"', "abc123xyz"),
            ("db.password=P@ssw0rd9", "P@ssw0rd9"),
            ("set x-api-key: 0123456789abcdef", "0123456789abcdef"),
            ("Authorization: Basic dXNlcm5hbWU6cGFzc3dvcmQ=", "dXNlcm5hbWU6cGFzc3dvcmQ="),
        ):
            with self.subTest(text=text):
                self.assertNotIn(secret, sanitize(text).text)

    def test_word_boundary_protects_common_params(self):
        self.assertEqual(sanitize("max_tokens=4096 与 timeout=30").hits, [])

    def test_fail_closed_descriptive_value(self):
        # fail-closed 契约：password: 后跟普通单词也切掉，宁可误杀
        r = sanitize("password: required")
        self.assertNotEqual(r.hits, [])


class ContractTest(unittest.TestCase):
    def test_hit_has_no_raw_secret(self):
        text = "手机 13812345678"
        for hit in scan(text):
            self.assertIsInstance(hit, Hit)
            self.assertNotIn("13812345678", hit.preview)
            self.assertEqual(text[hit.start : hit.end], "13812345678")  # 区间可回溯原文

    def test_preview_fully_masked(self):
        # 契约：preview 全遮蔽、零原文片段（历史实现保留首尾各 2 字符，7 字符密钥泄 4 字符）
        for text in ("手机 13812345678", "password: abcd1234", "token=abc123def456"):
            hits = scan(text)
            self.assertTrue(hits, text)  # 前置：样本必须真命中，防空转假绿
            for hit in hits:
                self.assertEqual(hit.preview, "*" * (hit.end - hit.start), text)

    def test_no_leak_in_output(self):
        secrets = ["13812345678", "AKIAIOSFODNN7EXAMPLE", "a@b.cn", "110101199003078910"]
        text = " ".join(secrets)
        out = sanitize(text).text
        for secret in secrets:
            self.assertNotIn(secret, out)

    def test_overlap_merged_single_hit(self):
        r = sanitize("access_key_id=AKIA1234567890ABCDEF")
        self.assertEqual(len(r.hits), 1)
        self.assertEqual(r.categories, ["secret"])
        self.assertEqual(r.text, "[REDACTED:secret]")

    def test_result_type_and_categories(self):
        r = sanitize("13812345678 和 a@b.cn")
        self.assertIsInstance(r, SanitizeResult)
        self.assertEqual(r.categories, ["email", "phone"])

    def test_idempotent(self):
        once = sanitize("token=abc123def456 & 138-1234-5678").text
        self.assertEqual(sanitize(once).text, once)


class FailClosedGuardTest(unittest.TestCase):
    """fail-closed 护栏：检测器异常/畸形结果必须沿 sanitize() 上抛（传播式 fail-closed）。

    反重构警告：禁止在 sanitize 调用点包兜底 except 吞异常——那会把 fail-closed
    静默变成 fail-open，本组测试钉住"异常必须炸出来"这一契约。
    """

    def test_detector_exception_propagates(self):
        with mock.patch.object(
            sanitize_core, "_iter_candidates", side_effect=RuntimeError("detector boom")
        ):
            with self.assertRaises(RuntimeError):
                sanitize("手机号 13812345678")

    def test_malformed_detector_output_propagates(self):
        # 畸形候选必须抛错而不是静默放行原文：元组过短在 sort key 索引处炸（IndexError），
        # 元组过长在 scan 解包处炸（ValueError）——两个崩溃点都钉住。
        with mock.patch.object(sanitize_core, "_iter_candidates", return_value=[(3, "phone")]):
            with self.assertRaises(IndexError):
                sanitize("手机号 13812345678")
        with mock.patch.object(
            sanitize_core, "_iter_candidates", return_value=[(3, 0, 5, "phone", "x", "extra")]
        ):
            with self.assertRaises(ValueError):
                sanitize("手机号 13812345678")


if __name__ == "__main__":
    unittest.main()
