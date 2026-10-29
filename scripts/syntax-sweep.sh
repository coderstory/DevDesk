#!/usr/bin/env bash
# scripts/syntax-sweep.sh
# 全量语法扫描：对 build 树里每个编译单元按其**原始编译旗标**跑
# clang -fsyntax-only（-ferror-limit=0），报告全部 warning / error，不产任何产物。
#
# 用途：
#   - 大改 / 跨机同步后的快速全量反馈（ninja 一处失败即停，本脚本一次扫全树，
#     含 add_subdirectory 并入的 EUI-NEO）；
#   - 提交前警告卫生检查（-Werror 下警告=错误；非 -Werror TU 的警告也照报，
#     vendored 3rd/ 除外，见下）。
#
# 前提：build/ 已由 scripts/build-test-run.sh configure 过（本脚本只读 build.ninja，
# 不 configure、不构建——那是 build-test-run.sh 的职责，禁止在此手敲 cmake）。
#
# 退出码：0 = 无诊断；1 = 有诊断（/3rd/ vendored 豁免，不计入）；2 = 环境错（不得静默绿）
#
# 用法：bash scripts/syntax-sweep.sh [--jobs N] [--strict] [--build-dir DIR]
#   --jobs N        并行度（默认 4；与构建并行跑时建议压低，IO 抢占实测互拖）
#   --strict        vendored 3rd/ 的诊断也计入退出码（默认：照报但不拦）
#   --build-dir DIR 构建目录（默认 build；Release 对应 build-release）

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS=4
STRICT=0
BUILD_DIR="build"

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --strict) STRICT=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

NINJA_FILE="${REPO_ROOT}/${BUILD_DIR}/build.ninja"
if [ ! -f "$NINJA_FILE" ]; then
    echo "❌ ${NINJA_FILE} 不存在——先跑 bash scripts/build-test-run.sh 完成 configure" >&2
    exit 2
fi

command -v ninja >/dev/null 2>&1 || { echo "❌ ninja 不在 PATH" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "❌ python3 不在 PATH" >&2; exit 2; }

echo "── 语法扫描（-fsyntax-only，不产产物）：${BUILD_DIR}/ 全树 ──"

# 单次进程内完成：ninja -t commands 抓全部编译边 → 逐条改写为 -fsyntax-only → 并行跑
# （改写规则：截取 -MD 之前的旗标段 + 源文件路径；-c 与 -fsyntax-only 互斥必须去掉；
#   ccache 对语法扫描无收益且无缓存，剥掉）
python3 - "$REPO_ROOT" "$BUILD_DIR" "$JOBS" "$STRICT" <<'PYEOF'
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

repo, build_dir, jobs, strict = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] == "1"
workdir = f"{repo}/{build_dir}"

raw = subprocess.run(["ninja", "-C", workdir, "-t", "commands"],
                     capture_output=True, text=True, check=True).stdout
cmds = [l.strip() for l in raw.splitlines() if " -c " in l]
if not cmds:
    print("❌ build.ninja 里没有编译边——树是干净的 configure 产物吗？", file=sys.stderr)
    sys.exit(2)

def check(cmd):
    i = cmd.find(" -MD ")
    if i < 0:
        return None
    prefix = cmd[:i].replace("ccache ", "", 1)
    m = re.search(r"-c (\S+)$", cmd)
    if not m:
        return None
    src = m.group(1)
    try:
        r = subprocess.run(f"{prefix} -ferror-limit=0 -fsyntax-only {src}", shell=True,
                           capture_output=True, text=True, timeout=180, cwd=workdir)
    except subprocess.TimeoutExpired:
        return ("error", src, "TIMEOUT (>180s)")
    diag = re.compile(r"\.(cpp|h|hpp|mm|c|cc):\d+:\d+: (warning|error):")
    hits = [l for l in (r.stderr or "").splitlines() if diag.search(l)]
    if not hits:
        return None
    vendored = "/3rd/" in src
    return ("vendored" if vendored else "error", src, "\n".join(hits))

problems = []
with ThreadPoolExecutor(max_workers=jobs) as ex:
    for res in ex.map(check, cmds):
        if res:
            problems.append(res)

own = [p for p in problems if p[0] != "vendored"]
vendored = [p for p in problems if p[0] == "vendored"]

for kind, src, hits in own:
    tag = "VENDORED-STRICT" if kind == "error" else kind
    print(f"=== [{tag}] {src}")
    print(hits)
if vendored and not strict:
    print(f"── {len(vendored)} 个 vendored 3rd/ TU 有诊断（默认豁免退出码，--strict 可拦）：")
    for _, src, _ in vendored:
        print(f"    {src}")
elif vendored and strict:
    for _, src, hits in vendored:
        print(f"=== [VENDORED-STRICT] {src}")
        print(hits)

print(f"── 扫描完成：{len(cmds)} 个编译单元，自有诊断 {len(own)}，vendored {len(vendored)} ──")
sys.exit(1 if (len(own) > 0 or (strict and len(vendored) > 0)) else 0)
PYEOF
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "✅ 语法扫描全绿"
else
    echo "❌ 语法扫描有诊断（上方逐条）"
fi
exit "$rc"
