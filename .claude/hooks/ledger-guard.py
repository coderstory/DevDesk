"""台账守卫 —— PostToolUse Edit|Write 触发
守卫 UAT62 台账文件：编号行不可删、状态列值域受控 {⬜, 🔵, ✅, ⏸}。
非 UAT62 台账文件零开销快速路径。
"""
import sys
import os
import json
import re
import subprocess

LINE_PATTERN = re.compile(r"^\|\s*([A-H]\d+)\s*\|")
VALID_STATUSES = {"⬜", "🔵", "✅", "⏸"}


def _extract_file_path(data):
    """从 hook stdin JSON 中递归提取 file_path（兼容各种嵌套层级）"""
    if isinstance(data, dict):
        fp = data.get("file_path")
        if fp:
            return fp
        for v in data.values():
            r = _extract_file_path(v)
            if r:
                return r
    elif isinstance(data, list):
        for item in data:
            r = _extract_file_path(item)
            if r:
                return r
    return None


def _git_root(file_path):
    d = os.path.dirname(os.path.abspath(file_path))
    try:
        r = subprocess.run(
            ["git", "-C", d, "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, timeout=10,
        )
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    return None


def _baseline_lines(git_root, rel_path):
    try:
        r = subprocess.run(
            ["git", "-C", git_root, "show", f"HEAD:{rel_path}"],
            capture_output=True, text=True, timeout=10,
        )
        if r.returncode == 0:
            return r.stdout.splitlines()
    except Exception:
        pass
    return None


def _numbered_ids(lines):
    ids = set()
    for ln in lines:
        m = LINE_PATTERN.match(ln)
        if m:
            ids.add(m.group(1))
    return ids


def _check_statuses(lines):
    errors = []
    for i, ln in enumerate(lines, 1):
        m = LINE_PATTERN.match(ln)
        if not m:
            continue
        row_id = m.group(1)
        found = False
        for cell in ln.split("|")[1:]:  # 跳过行首空串, 余下各格逐个扫
            stripped = cell.strip()
            if not stripped:
                continue
            if any(stripped.startswith(marker) for marker in VALID_STATUSES):
                found = True
                break
        if not found:
            errors.append(f"行{i}({row_id}): 无有效状态标记")
    return errors


def main():
    raw = sys.stdin.read()
    if not raw.strip():
        sys.exit(0)

    try:
        data = json.loads(raw)
    except Exception:
        sys.exit(0)

    file_path = _extract_file_path(data)
    if not file_path:
        sys.exit(0)

    file_path = file_path.replace("\\", "/")

    # 快速路径：非 UAT62 台账文件零开销
    if "docs/superpowers/plans/" not in file_path or "UAT62" not in file_path:
        sys.exit(0)

    git_root = _git_root(file_path)
    if not git_root:
        sys.exit(0)

    try:
        abs_fp = os.path.realpath(os.path.abspath(file_path))
        abs_root = os.path.realpath(git_root)
        rel_path = os.path.relpath(abs_fp, abs_root).replace("\\", "/")
    except Exception:
        sys.exit(0)

    baseline = _baseline_lines(git_root, rel_path)
    if baseline is None:
        # 文件未进 git（新增），不用守卫
        sys.exit(0)

    # 读当前文件
    try:
        with open(file_path, "r", encoding="utf-8") as fh:
            current = [ln.rstrip("\n\r") for ln in fh]
    except Exception:
        sys.exit(0)

    base_ids = _numbered_ids(baseline)
    curr_ids = _numbered_ids(current)

    all_errs = []

    missing = base_ids - curr_ids
    if missing:
        ml = "、".join(sorted(missing))
        all_errs.append(
            f"[台账守卫] 编号行被删：{ml} —— "
            "台账行只许增/改状态，禁删；确需删走主会话决策"
        )

    stat_errs = _check_statuses(current)
    for e in stat_errs:
        all_errs.append(f"[台账守卫] {e}")

    if all_errs:
        for e in all_errs:
            print(e, file=sys.stderr)
        sys.exit(2)

    sys.exit(0)


if __name__ == "__main__":
    main()