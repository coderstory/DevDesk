"""LLM 客户端与 JSON 提取的离线测试（本地 http.server 桩）。"""

from __future__ import annotations

import json
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.ensure_import_path()
from memory_lab.llm import LlmClient, LlmError, extract_json  # noqa: E402


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length)
        state = self.server.state
        try:
            payload = json.loads(raw.decode("utf-8"))
        except ValueError:
            payload = None
        state["requests"].append(
            {"path": self.path, "headers": dict(self.headers), "payload": payload}
        )
        index = len(state["requests"]) - 1
        responses = state["responses"]
        status, body = responses[min(index, len(responses) - 1)]
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):  # 静默
        pass


class LlmClientTest(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.server.state = {"requests": [], "responses": []}
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_address[1]}/anthropic"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def _respond(self, *responses):
        self.server.state["responses"] = list(responses)

    def test_complete_ok_strips_model_suffix_and_extracts_text(self):
        self._respond(
            (
                200,
                {
                    "content": [
                        {"type": "thinking", "thinking": "不该返回"},
                        {"type": "text", "text": "你好"},
                        {"type": "text", "text": "，世界"},
                    ],
                    "usage": {
                        "input_tokens": 12,
                        "output_tokens": 5,
                        "cache_read_input_tokens": 3,
                    },
                },
            )
        )
        client = LlmClient(
            self.base_url, "secret-token", "qianfan/deepseek-v4.1-flash[1M]", retries=0
        )
        text, usage = client.complete(
            [
                {"role": "system", "content": "系统提示"},
                {"role": "user", "content": "打个招呼"},
            ],
            max_tokens=321,
            temperature=0.0,
        )
        self.assertEqual(text, "你好，世界")
        self.assertEqual(usage, {"input_tokens": 12, "output_tokens": 5})

        request = self.server.state["requests"][0]
        self.assertEqual(request["path"], "/anthropic/v1/messages")
        self.assertEqual(
            request["payload"]["model"], "qianfan/deepseek-v4.1-flash"
        )
        self.assertEqual(request["payload"]["system"], "系统提示")
        self.assertEqual(len(request["payload"]["messages"]), 1)
        self.assertEqual(request["payload"]["messages"][0]["role"], "user")
        self.assertEqual(request["payload"]["max_tokens"], 321)
        self.assertEqual(request["payload"]["temperature"], 0.0)
        self.assertEqual(
            request["headers"].get("Authorization"), "Bearer secret-token"
        )
        self.assertNotIn("secret-token", repr(client))

    @mock.patch("memory_lab.llm.time.sleep", return_value=None)
    def test_retry_after_429_then_success(self, sleep):
        self._respond(
            (429, {"error": "rate limited"}),
            (200, {"content": [{"type": "text", "text": "ok"}], "usage": {}}),
        )
        client = LlmClient(self.base_url, "t", "m", retries=2)
        text, usage = client.complete([{"role": "user", "content": "hi"}])
        self.assertEqual(text, "ok")
        self.assertEqual(usage, {"input_tokens": 0, "output_tokens": 0})
        self.assertEqual(len(self.server.state["requests"]), 2)
        sleep.assert_called_once_with(0.5)

    @mock.patch("memory_lab.llm.time.sleep", return_value=None)
    def test_5xx_exhausts_retries(self, sleep):
        self._respond((500, {"error": "boom"}))
        client = LlmClient(self.base_url, "t", "m", retries=2)
        with self.assertRaises(LlmError) as ctx:
            client.complete([{"role": "user", "content": "hi"}])
        self.assertEqual(ctx.exception.status, 500)
        self.assertEqual(len(self.server.state["requests"]), 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [0.5, 1.0])

    def test_4xx_raises_immediately_without_leaking_token(self):
        secret = "TOKEN-SHOULD-NOT-LEAK"
        self._respond((400, {"error": f"bad request for {secret}", "pad": "x" * 500}))
        client = LlmClient(self.base_url, secret, "m", retries=3)
        with self.assertRaises(LlmError) as ctx:
            client.complete([{"role": "user", "content": "hi"}])
        error = ctx.exception
        self.assertEqual(error.status, 400)
        self.assertEqual(len(self.server.state["requests"]), 1)
        self.assertNotIn(secret, str(error))
        self.assertNotIn(secret, error.body)
        self.assertLessEqual(len(error.body), 300)

    def test_network_error_raises_llm_error(self):
        client = LlmClient("http://127.0.0.1:1/anthropic", "t", "m", retries=0)
        with self.assertRaises(LlmError):
            client.complete([{"role": "user", "content": "hi"}])

    def test_base_url_ending_with_v1(self):
        self._respond(
            (200, {"content": [{"type": "text", "text": "ok"}], "usage": {}})
        )
        client = LlmClient(self.base_url + "/v1", "t", "m", retries=0)
        self.assertEqual(client.complete([{"role": "user", "content": "hi"}])[0], "ok")
        self.assertEqual(
            self.server.state["requests"][0]["path"], "/anthropic/v1/messages"
        )


class ExtractJsonTest(unittest.TestCase):
    def test_plain_object(self):
        self.assertEqual(extract_json('{"a": 1}'), {"a": 1})

    def test_fenced_json(self):
        text = '这是结果：\n```json\n{"observations": [{"title": "x"}]}\n```\n以上。'
        self.assertEqual(
            extract_json(text), {"observations": [{"title": "x"}]}
        )

    def test_dirty_text_with_nested_braces_and_quoted_brace(self):
        text = '好的，结果如下：{"a": {"b": "}"}, "c": [1, 2]} 希望有帮助'
        self.assertEqual(extract_json(text), {"a": {"b": "}"}, "c": [1, 2]})

    def test_multiple_objects_returns_first_parseable(self):
        text = '{坏掉的} 后面 {"ok": true}'
        self.assertEqual(extract_json(text), {"ok": True})

    def test_invalid_returns_none(self):
        self.assertIsNone(extract_json("完全不是 JSON"))
        self.assertIsNone(extract_json('{"a": }'))
        self.assertIsNone(extract_json("[1, 2, 3]"))
        self.assertIsNone(extract_json(""))
        self.assertIsNone(extract_json(None))


if __name__ == "__main__":
    unittest.main()