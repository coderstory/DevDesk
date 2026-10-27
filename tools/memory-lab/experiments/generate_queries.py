"""一次性生成冻结查询集（运行一次后 queries.json 即为评测基线，评测时不再调用 LLM）。

用法（只允许在需要重建查询集时执行，例如数据集整体替换）：
    D:/software/venvs/memory-lab/Scripts/python.exe -I generate_queries.py

输入：observations.json（由 freeze_datasets.py 生成）
输出：raw/query_generation.json（原始响应）+ queries.json（冻结查询，含 gold_ids）

注意：本脚本调用云端网关，生成结果必须提交入库；eval_retrieval.py 只读 queries.json。
"""

from __future__ import annotations

import json
import os
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODEL = "qianfan/deepseek-v4.1-flash"
TARGET_IDS = list(range(15))  # 与历史评测一致：前 15 条为出题目标，其余 12 条作干扰项

PROMPT = """下面每条是从开发者历史会话中抽取的项目记忆。请为每条写 1 个真实的开发者提问（中文，15-30 字）。
要求：不知道这条记忆的人也会这样问，但该记忆正是答案；禁止照抄标题里的词；不要提"记忆/条目"。
输出严格 JSON 数组：[{"i":0,"q":"..."}]，不要解释。"""


def _settings_env() -> dict:
    path = Path.home() / ".claude" / "settings.json"
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["env"]


def main() -> None:
    observations = json.loads((HERE / "observations.json").read_text(encoding="utf-8"))
    sample = observations[:15]
    payload = [
        {"i": i, "title": o["title"], "narrative": o.get("narrative", "")}
        for i, o in enumerate(sample)
    ]
    env = _settings_env()
    body = {
        "model": MODEL,
        "max_tokens": 4000,
        "thinking": {"type": "disabled"},
        "messages": [{"role": "user", "content": PROMPT + "\n\n" + json.dumps(payload, ensure_ascii=False)}],
    }
    request = urllib.request.Request(
        env["ANTHROPIC_BASE_URL"] + "/v1/messages",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "content-type": "application/json",
            "authorization": "Bearer " + env["ANTHROPIC_AUTH_TOKEN"],
            "anthropic-version": "2023-06-01",
        },
    )
    with urllib.request.urlopen(request, timeout=180) as response:
        data = json.loads(response.read().decode("utf-8"))
    text = "".join(block.get("text", "") for block in data.get("content", []) if block.get("type") == "text")
    raw = {"request_model": MODEL, "prompt": PROMPT, "input": payload, "response": data}
    (HERE / "raw").mkdir(exist_ok=True)
    (HERE / "raw" / "query_generation.json").write_text(
        json.dumps(raw, ensure_ascii=False, indent=1), encoding="utf-8"
    )

    parsed = json.loads(text.strip().removeprefix("```json").removesuffix("```").strip())
    queries = []
    for index, entry in enumerate(parsed):
        target = int(entry["i"])
        queries.append(
            {
                "id": f"q{index + 1:02d}",
                "query": entry["q"].strip(),
                "target_id": target,
                "source": "llm_generated_from_target",
            }
        )
    (HERE / "raw" / "queries_llm_only.json").write_text(
        json.dumps(queries, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    print(json.dumps(queries, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()