#!/usr/bin/env python3
"""zcode 供应商自动切换执行器（需-16，技术面 docs/superpowers/research/2026-10-30-provider-auto-switch-技术面.md）。

「探测即判断」：两次 cron 点（23:05 / 9:05）跑同一逻辑——
  1. 最小真实 zcode 回合探测当前档位实际可用性（成功=可用；报文含 quota/402/余额类=不可用）；
  2. 可用→不切（白嫖到底）；不可用→切回另一档；网络类失败重试 1 次防抖，仍失败只告警不切。
切换 = `~/.zcode/v2/provider_config.json` 的 `config.defaultModelSelection.providerId` 一个字段，
原子写（tmp+rename）。全程记 provider_switch.log，异常桌面通知不静默。

子命令：
  check              探测+按需切换（cron 点调用，无参数形态）
  status             只读当前档位
  --dry-run          只打印决策不写回
  --probe-only       只探测不决策（验收/人工核查用）

档位：免费 = account:bigmodel-start-plan；付费 = credentials.json 的
account-provider 身份动态解析（解析不到回落常量，报警提示跑 zcode login bigmodel）。

授权来源：老板娘依老板「电脑随你们操作」在案授权代批（跨软件配置写入已批全自动版）。
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path

HOME = Path.home()
CONFIG = HOME / ".zcode" / "v2" / "provider_config.json"
CREDENTIALS = HOME / ".zcode" / "v2" / "credentials.json"
LOG = HOME / ".zcode" / "v2" / "provider_switch.log"

FREE_PROVIDER = "account:bigmodel-start-plan"
FALLBACK_PAID = "account-provider:account:bigmodel-individual-coding-plan"

# CLI 调用形态与 bridge 派单同款（zcode.cjs 直跑，env 对缺一报「无法定位内置配置」）
ZCODE_CJS = "/Applications/ZCode.app/Contents/Resources/glm/zcode.cjs"
ZCODE_ENV = {
    **os.environ,
    "ZCODE_BUILTIN_PROVIDER_CONFIG_FILE":
        "/Applications/ZCode.app/Contents/Resources/config/provider/zcode-builtin.json",
    "ZCODE_PERSONAL_PROVIDER_CONFIG_FILE": str(CONFIG),
    "NO_COLOR": "1",
}
UNAVAILABLE_MARKS = ("quota", "402", "余额", "不足", "insufficient", "exceeded", "limit")


def _log(msg: str) -> None:
    line = f"{datetime.now():%Y-%m-%d %H:%M:%S} {msg}"
    print(line)
    try:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def _alert(msg: str) -> None:
    _log(f"ALERT {msg}")
    try:  # macOS 桌面通知；失败只留日志（告警本身不许静默，故再打 stderr）
        subprocess.run(["osascript", "-e",
                        f'display notification "{msg}" with title "zcode 供应商切换"'],
                       timeout=10, check=False)
    except Exception as error:  # noqa: BLE001
        print(f"桌面通知失败：{error}", file=sys.stderr)


def _paid_provider() -> str:
    try:
        cred = json.loads(CREDENTIALS.read_text(encoding="utf-8"))
        if isinstance(cred, dict):
            # 付费档身份键形如「<providerId>:identity」（实测键名），providerId 取键前缀；
            # 排除免费档，多档时取最后一个非免费档
            for key in cred:
                if key.endswith(":identity") and "start-plan" not in key:
                    return key[: -len(":identity")]
    except (OSError, ValueError):
        pass
    _log(f"WARN 付费档身份解析失败，回落常量 {FALLBACK_PAID}（若持续出现请跑 zcode login bigmodel）")
    return FALLBACK_PAID


def _probe() -> tuple[bool, str]:
    """最小真实回合探测当前档位。返回 (可用, 判定依据一句话)。"""
    if os.environ.get("ZCODE_PROBE_FORCE_FAIL") == "1":
        return False, "注入失败（ZCODE_PROBE_FORCE_FAIL=1，验收用）"
    for attempt in (1, 2):  # 失败重试 1 次防网络抖动误切
        try:
            run = subprocess.run(
                ["node", ZCODE_CJS, "-p", "回复一个字：好", "--mode", "plan"],
                env=ZCODE_ENV, capture_output=True, text=True, timeout=180,
            )
            out = (run.stdout + run.stderr).lower()
            if run.returncode == 0:
                return True, f"探测回合成功（第 {attempt} 次）"
            if any(mark in out for mark in UNAVAILABLE_MARKS):
                return False, f"额度/余额类失败：{out[:200]}"
            reason = f"非额度类失败：{out[:200]}"
            _log(f"WARN 探测失败（第 {attempt} 次）{reason}")
        except subprocess.TimeoutExpired:
            reason = "探测回合超时（180s）"
            _log(f"WARN 探测失败（第 {attempt} 次）{reason}")
    return False, reason + "（重试后仍失败，按不可用处理）"


def _atomic_write_provider_id(provider_id: str) -> None:
    cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    cfg["config"]["defaultModelSelection"]["providerId"] = provider_id
    fd, tmp = tempfile.mkstemp(dir=str(CONFIG.parent), suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    os.replace(tmp, CONFIG)  # 同目录 rename，原子生效


def check(dry_run: bool, window: str) -> int:
    """两个 cron 点同一「探测即判断」，方向按窗口定：
    --window free（23:05 免费窗入口）：先落免费档再探测，可用=白嫖；不可用切回付费。
    --window paid（9:05 窗口出口 / 默认）：探测当前档，可用=白嫖到底不切；不可用切回付费。"""
    if not CONFIG.exists():
        _alert(f"配置文件不存在：{CONFIG}")
        return 1
    current_id = json.loads(CONFIG.read_text(encoding="utf-8"))[
        "config"]["defaultModelSelection"]["providerId"]
    paid = _paid_provider()

    if window == "free":
        if current_id != FREE_PROVIDER:
            if dry_run:
                print(f"[dry-run] 将切到免费档 {FREE_PROVIDER} 并探测")
                return 0
            _atomic_write_provider_id(FREE_PROVIDER)
            current_id = FREE_PROVIDER
        ok, reason = _probe()
        if ok:
            _log(f"免费档可用，白嫖中（{reason}）")
            return 0
        _alert(f"免费档不可用（{reason}），切回付费档")
        if dry_run:
            return 0
        _atomic_write_provider_id(paid)
        ok2, reason2 = _probe()
        _log("付费档复探测" + ("可用" if ok2 else f"仍不可用（{reason2}），请人工 zcode login bigmodel"))
        return 0 if ok2 else 1

    # paid 窗口（默认）：白嫖到底——当前可用就不切
    ok, reason = _probe()
    _log(f"当前={current_id} 探测={'可用' if ok else '不可用'}（{reason}）→ "
         + ("维持不切" if ok else "切回付费档"))
    if ok:
        return 0
    if dry_run:
        print(f"[dry-run] 将切回付费档 {paid}")
        return 0
    try:
        _atomic_write_provider_id(paid)
    except Exception as error:  # noqa: BLE001
        _alert(f"写回失败，仍停在 {current_id}：{error}")
        return 1
    ok2, reason2 = _probe()
    if ok2:
        _log(f"✅ 已切回付费档，复探测可用")
    else:
        _alert(f"已切回付费档但复探测仍不可用（{reason2}），请人工 zcode login bigmodel")
    return 0


def main() -> int:
    args = sys.argv[1:]
    window = "paid" if "--window paid" in " ".join(args) or "--window" not in args else \
        (args[args.index("--window") + 1] if "--window" in args else "paid")
    if "--window free" in " ".join(args):
        window = "free"
    if "--probe-only" in args:
        ok, reason = _probe()
        _log(f"探测（仅探测不决策）：{'可用' if ok else '不可用'}（{reason}）")
        return 0 if ok else 1
    if "status" in args:
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))["config"]["defaultModelSelection"]
        print(json.dumps(cfg, ensure_ascii=False))
        return 0
    return check(dry_run="--dry-run" in args, window=window)


if __name__ == "__main__":
    sys.exit(main())
