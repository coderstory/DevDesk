"""观测压缩测试：分段边界、JSON 规范化、会话压缩入库参数。"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.install_foundation_stubs()

from memory_lab.compress import (  # noqa: E402
    build_segments,
    compress_segment,
    compress_session,
)
from memory_lab.embedding import HashEmbedder  # noqa: E402


def fake_payload(count=1, importance=4):
    return json.dumps(
        {
            "observations": [
                {
                    "title": f"标题{i}",
                    "subtitle": f"副标题{i}",
                    "narrative": f"叙事{i}",
                    "facts": [f"事实{i}a", f"事实{i}b"],
                    "concepts": ["测试", "压缩"],
                    "files": [f"src/file{i}.py"],
                    "importance": importance,
                }
                for i in range(count)
            ]
        },
        ensure_ascii=False,
    )


class FakeLlm:
    def __init__(self, text=None, usage=None):
        self.text = fake_payload() if text is None else text
        self.usage = usage or {"input_tokens": 100, "output_tokens": 50}
        self.calls = []

    def complete(self, messages, max_tokens=1500, temperature=None):
        self.calls.append(
            {"messages": messages, "max_tokens": max_tokens, "temperature": temperature}
        )
        return self.text, dict(self.usage)


class BuildSegmentsTest(unittest.TestCase):
    def setUp(self):
        self.conn = support.new_conn()

    def tearDown(self):
        self.conn.close()

    def test_split_by_event_count(self):
        for seq in range(5):
            support.insert_event(self.conn, "s1", seq, "user", "x" * 30)
        segments = build_segments(self.conn, "s1", max_chars=1000, max_events=2)
        self.assertEqual(
            [(seg.seq_start, seg.seq_end, seg.event_count) for seg in segments],
            [(0, 1, 2), (2, 3, 2), (4, 4, 1)],
        )
        self.assertTrue(all(seg.session_id == "s1" for seg in segments))
        self.assertIn("[0 user]", segments[0].text)

    def test_split_by_char_budget(self):
        for seq in range(5):
            support.insert_event(self.conn, "s2", seq, "user", "x" * 30)
        # 每行 "[0 user] " (9) + 30 = 39；两行 79 ≤ 100，三行 119 > 100
        segments = build_segments(self.conn, "s2", max_chars=100, max_events=40)
        self.assertEqual([seg.event_count for seg in segments], [2, 2, 1])
        self.assertTrue(all(len(seg.text) <= 100 for seg in segments))

    def test_blank_text_events_skipped_and_tool_name_labeled(self):
        support.insert_event(self.conn, "s3", 0, "user", "有效内容")
        support.insert_event(self.conn, "s3", 1, "tool_use", "   ")
        support.insert_event(
            self.conn, "s3", 2, "tool_use", '{"cmd": "ls"}', tool_name="Bash"
        )
        segments = build_segments(self.conn, "s3", max_chars=1000, max_events=10)
        self.assertEqual(len(segments), 1)
        self.assertEqual(segments[0].seq_start, 0)
        self.assertEqual(segments[0].seq_end, 2)
        self.assertEqual(segments[0].event_count, 2)
        self.assertIn("tool_use:Bash", segments[0].text)

    def test_non_contiguous_seq_preserved(self):
        support.insert_event(self.conn, "s4", 5, "user", "甲")
        support.insert_event(self.conn, "s4", 9, "user", "乙")
        segments = build_segments(self.conn, "s4")
        self.assertEqual((segments[0].seq_start, segments[0].seq_end), (5, 9))

    def test_unknown_session_returns_empty(self):
        self.assertEqual(build_segments(self.conn, "nope"), [])

    def test_long_single_event_is_clipped_to_budget(self):
        support.insert_event(self.conn, "s6", 0, "user", "y" * 500)
        segments = build_segments(self.conn, "s6", max_chars=100, max_events=10)
        self.assertEqual(len(segments), 1)
        self.assertLessEqual(len(segments[0].text), 100)


class CompressSegmentTest(unittest.TestCase):
    def _segment(self):
        from memory_lab.models import Segment

        return Segment(
            session_id="s1", seq_start=0, seq_end=3, text="事件正文", event_count=4
        )

    def test_parses_and_normalizes(self):
        llm = FakeLlm()
        observations = compress_segment(llm, self._segment(), project="proj")
        self.assertEqual(len(observations), 1)
        self.assertEqual(observations[0]["title"], "标题0")
        self.assertEqual(observations[0]["facts"], ["事实0a", "事实0b"])
        call = llm.calls[0]
        self.assertEqual(call["max_tokens"], 1500)
        self.assertEqual(call["temperature"], 0.0)
        system = call["messages"][0]["content"]
        user = call["messages"][1]["content"]
        self.assertIn("只记录事实", system)
        self.assertIn("proj", user)
        self.assertIn("事件正文", user)

    def test_fenced_and_dirty_json(self):
        llm = FakeLlm(
            '说明：\n```json\n' + fake_payload(2) + "\n```\n结束",
        )
        observations = compress_segment(llm, self._segment())
        self.assertEqual(len(observations), 2)

    def test_limits_and_clamps(self):
        payload = {
            "observations": [
                {
                    "title": "一二三四五六七八九十一二三四五六七八九十超",
                    "subtitle": "副" * 60,
                    "narrative": "叙" * 200,
                    "facts": ["", "  保留", None],
                    "concepts": [],
                    "files": ["a.py"],
                    "importance": 9,
                },
                {"subtitle": "缺标题应丢弃"},
                "不是字典",
                {
                    "title": "第三条",
                    "importance": "bad",
                },
                {"title": "第四条"},
            ]
        }
        llm = FakeLlm(json.dumps(payload, ensure_ascii=False))
        observations = compress_segment(llm, self._segment())
        self.assertEqual(len(observations), 3)  # 最多 3 条
        first = observations[0]
        self.assertEqual(len(first["title"]), 20)
        self.assertEqual(len(first["subtitle"]), 40)
        self.assertEqual(len(first["narrative"]), 120)
        self.assertEqual(first["facts"], ["保留"])
        self.assertEqual(first["importance"], 5)
        self.assertEqual(observations[1]["importance"], 3)

    def test_invalid_json_returns_empty(self):
        self.assertEqual(compress_segment(FakeLlm("抱歉，我不能。"), self._segment()), [])
        self.assertEqual(
            compress_segment(FakeLlm('{"observations": []}'), self._segment()), []
        )
        self.assertEqual(
            compress_segment(FakeLlm('{"observations": "x"}'), self._segment()), []
        )


class CompressSessionTest(unittest.TestCase):
    def setUp(self):
        self.conn = support.new_conn()

    def tearDown(self):
        self.conn.close()

    def _seed_user_session(self, session_id="s1", count=2, project="D--project-gui"):
        support.insert_session(self.conn, session_id, project)
        for seq in range(count):
            support.insert_event(self.conn, session_id, seq, "user", f"用户消息{seq}")

    def test_compress_session_writes_observations(self):
        self._seed_user_session("s1", count=2, project="proj-x")
        llm = FakeLlm(fake_payload(1), usage={"input_tokens": 120, "output_tokens": 30})
        result = compress_session(None, self.conn, "s1", llm)
        self.assertEqual(
            result,
            {
                "observations": 1,
                "usage": {"input_tokens": 120, "output_tokens": 30},
                "calls": 1,
            },
        )
        row = self.conn.execute("SELECT * FROM observations").fetchone()
        self.assertEqual(row["session_id"], "s1")
        self.assertEqual(row["project"], "proj-x")
        self.assertEqual(row["kind"], "observation")
        self.assertEqual(row["title"], "标题0")
        self.assertEqual(row["facts"], "事实0a\n事实0b")
        self.assertEqual(row["concepts"], "测试, 压缩")
        self.assertEqual(row["files"], "src/file0.py")
        self.assertEqual(row["importance"], 4)
        self.assertEqual(row["status"], "active")
        self.assertEqual(row["source_event_range"], "s1:0-1")
        self.assertTrue(row["created_at"])

    def test_multiple_segments_accumulate_usage(self):
        self._seed_user_session("s1", count=3)
        llm = FakeLlm(fake_payload(1), usage={"input_tokens": 10, "output_tokens": 2})
        result = compress_session(None, self.conn, "s1", llm, budget=None)
        expected_calls = len(build_segments(self.conn, "s1"))
        self.assertEqual(result["calls"], expected_calls)
        self.assertEqual(result["observations"], expected_calls)
        self.assertEqual(
            result["usage"],
            {"input_tokens": 10 * expected_calls, "output_tokens": 2 * expected_calls},
        )

    def test_skip_pure_thinking_and_tool_result_segments(self):
        support.insert_session(self.conn, "s2")
        support.insert_event(self.conn, "s2", 0, "thinking", "我在思考")
        support.insert_event(
            self.conn, "s2", 1, "tool_result", "命令输出", role="user"
        )
        llm = FakeLlm()
        result = compress_session(None, self.conn, "s2", llm)
        self.assertEqual(llm.calls, [])
        self.assertEqual(result, {
            "observations": 0,
            "usage": {"input_tokens": 0, "output_tokens": 0},
            "calls": 0,
        })

    def test_user_text_makes_segment_informative(self):
        support.insert_session(self.conn, "s3")
        support.insert_event(self.conn, "s3", 0, "thinking", "思考")
        support.insert_event(self.conn, "s3", 1, "user", "请修复 bug")
        llm = FakeLlm()
        result = compress_session(None, self.conn, "s3", llm)
        self.assertEqual(result["calls"], 1)
        self.assertEqual(result["observations"], 1)

    def test_budget_limits_calls(self):
        self._seed_user_session("s1", count=6)
        llm = FakeLlm()
        result = compress_session(None, self.conn, "s1", llm, budget=1)
        self.assertEqual(result["calls"], 1)
        self.assertEqual(result["observations"], 1)
        self.assertEqual(len(llm.calls), 1)
        zero = compress_session(None, self.conn, "s1", FakeLlm(), budget=0)
        self.assertEqual(zero["calls"], 0)

    def test_embedder_writes_vectors(self):
        self._seed_user_session("s1", count=1)
        llm = FakeLlm(fake_payload(2), usage={"input_tokens": 1, "output_tokens": 1})
        result = compress_session(None, self.conn, "s1", llm, embedder=HashEmbedder())
        self.assertEqual(result["observations"], 2)
        rows = self.conn.execute(
            "SELECT owner_id, model, dim FROM embeddings ORDER BY owner_id"
        ).fetchall()
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(row["model"] == "hash" for row in rows))
        self.assertTrue(all(row["dim"] == 512 for row in rows))
        obs_ids = {
            row["id"] for row in self.conn.execute("SELECT id FROM observations")
        }
        self.assertEqual({row["owner_id"] for row in rows}, obs_ids)

    def test_unknown_session_no_project_and_no_calls(self):
        llm = FakeLlm()
        result = compress_session(None, self.conn, "missing", llm)
        self.assertEqual(result["calls"], 0)
        self.assertEqual(llm.calls, [])


if __name__ == "__main__":
    unittest.main()