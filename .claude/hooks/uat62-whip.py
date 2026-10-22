"""UAT6.2 自检鞭策 hook —— UserPromptSubmit 触发
每条用户消息计数 +1，每 3 轮复读用户目标 + 自检四问；非 3 轮只注入自检四问。
count 文件放 TEMP 目录避免 git 噪音。
"""
import sys
import os
import json

GOALS_FILE = r"D:\project\gui\DevDesk\.claude\UAT62-用户目标.md"
_tmp = os.environ.get("TEMP") or os.environ.get("TMP") or ""
COUNT_FILE = os.path.join(_tmp, "uat62-whip-count") if _tmp else ""

FOUR_QUESTIONS = (
    "【UAT6.2 自检四问】"
    "①是否遵守流程？（台账先行：清单落台账才派发；批完对账核销；状态机⬜→🔵→✅只认实测；子任务≤30分钟硬时限，超时立即报告部分结果）"
    "②用户的需求完成了吗？（对照目标文件）"
    "③还有遗漏吗？（对照台账未✅项）"
    "④成果审计了吗？（每波次双独立专家审计，未审不算完）"
)


def _read_count():
    try:
        with open(COUNT_FILE, "r", encoding="ascii") as fh:
            return int(fh.read().strip())
    except Exception:
        return 1


def _write_count(n):
    if not COUNT_FILE:
        return
    try:
        os.makedirs(os.path.dirname(COUNT_FILE), exist_ok=True)
        with open(COUNT_FILE, "w", encoding="ascii") as fh:
            fh.write(str(n))
    except Exception:
        pass


def _read_goals():
    try:
        with open(GOALS_FILE, "r", encoding="utf-8") as fh:
            return fh.read()
    except Exception:
        return f"目标文件缺失：{GOALS_FILE}"


def main():
    count = _read_count() + 1
    _write_count(count)

    if count % 3 == 0:
        injected = f"【用户目标·每3轮复读】\n{_read_goals()}\n{FOUR_QUESTIONS}"
    else:
        injected = FOUR_QUESTIONS

    output = {
        "hookSpecificOutput": {
            "hookEventName": "UserPromptSubmit",
            "additionalContext": injected,
        }
    }
    print(json.dumps(output, ensure_ascii=False))
    sys.exit(0)


if __name__ == "__main__":
    main()