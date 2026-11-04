#!/bin/bash
# install-win-debug.sh —— Windows 装机口径（2026-11-03 老板令）：Debug 产物 strip 后覆盖安装。
#
# 流程：杀 DevDesk.exe → objcopy --only-keep-debug 分离调试符号 → 拷贝 exe 到装机目录
#       → strip --strip-debug → --add-gnu-debuglink 回链 → 拷 assets + THIRD-PARTY-NOTICES.md
#       → sha256 清单 → 重启 app。
#
# 调试符号落点（崩溃回溯用）：${DEVDESK_DATA_ROOT:-%USERPROFILE%\DevDesk}\debug-symbols\DevDesk.exe.debug
#   （随 app 数据根约定；gdb 里手动 `symbol-file <该文件>` 加载——Windows PE 无 build-id 自动回链）。
#
# 用法：
#   bash scripts/install-win-debug.sh [--target DIR] [--no-launch]
#   默认 target=%LOCALAPPDATA%\Programs\DevDesk；源 = <仓库>/build/DevDesk.exe（Debug 内循环产物）。
#   演练：--target <临时目录> --no-launch；真装机由收口节点主会话统一执行。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="$(cygpath -u "$LOCALAPPDATA")/Programs/DevDesk"
LAUNCH=1
while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET="$(cygpath -u "$2")"; shift 2 ;;
        --no-launch) LAUNCH=0; shift ;;
        *) echo "[错误] 未知参数 $1（支持 --target DIR / --no-launch）"; exit 2 ;;
    esac
done

if ! command -v strip >/dev/null 2>&1 || ! command -v objcopy >/dev/null 2>&1; then
    # MSYS2 工具链（strip/objcopy 所在），环境线索 env#2
    export PATH="/d/software/msys64/mingw64/bin:$PATH"
fi
command -v strip >/dev/null 2>&1 || { echo "[错误] 找不到 strip（需 MSYS2 MinGW64 binutils）"; exit 2; }
command -v objcopy >/dev/null 2>&1 || { echo "[错误] 找不到 objcopy（需 MSYS2 MinGW64 binutils）"; exit 2; }

EXE_SRC="$REPO_ROOT/build/DevDesk.exe"
[ -f "$EXE_SRC" ] || { echo "[错误] 找不到构建产物 ${EXE_SRC}，先跑 bash scripts/build-test-run.sh --dev"; exit 1; }

DATA_ROOT="${DEVDESK_DATA_ROOT:-$(cygpath -u "$USERPROFILE")/DevDesk}"
SYMBOLS_DIR="$DATA_ROOT/debug-symbols"
EXE_DST="$TARGET/DevDesk.exe"

size_mb() { awk -v b="$1" 'BEGIN { printf "%.1fMB", b / 1048576 }'; }

echo "[1/6] 停止运行中的 DevDesk…"
taskkill //F //IM DevDesk.exe >/dev/null 2>&1 || echo "  无运行实例"
sleep 1

echo "[2/6] 分离调试符号 → ${SYMBOLS_DIR}"
mkdir -p "$SYMBOLS_DIR"
rm -f "$SYMBOLS_DIR/DevDesk.exe.debug" "$SYMBOLS_DIR/DevDesk.exe.debug.sha256"
objcopy --only-keep-debug "$EXE_SRC" "$SYMBOLS_DIR/DevDesk.exe.debug"

echo "[3/6] 拷贝 exe 并 strip 调试段…"
mkdir -p "$TARGET"
cp -f "$EXE_SRC" "$EXE_DST"
# strip 偶发 Invalid argument（AV 对新写大 PE 的瞬时锁），重试兜底；体积断言防「假 strip」出货
STRIP_OK=0
for i in 1 2 3; do
    if strip --strip-debug "$EXE_DST" 2>/dev/null; then
        STRIP_OK=1
        break
    fi
    sleep 2
done
if [ "${STRIP_OK}" -ne 1 ]; then
    echo "[错误] strip 三次失败（AV 瞬时锁未解除），中止装机——未 strip 的 exe 不会留在装机目录"
    rm -f "$EXE_DST"
    exit 1
fi
EXE_SRC_SIZE=$(stat -c%s "$EXE_SRC")
EXE_DST_SIZE=$(stat -c%s "$EXE_DST")
if [ "${EXE_DST_SIZE}" -ge "${EXE_SRC_SIZE}" ]; then
    echo "[错误] strip 后体积未缩小（${EXE_DST_SIZE} >= ${EXE_SRC_SIZE}），strip 实际未生效，中止"
    rm -f "$EXE_DST"
    exit 1
fi
# debuglink 偶发 Invalid argument（AV 对新写大 PE 的瞬时锁），重试兜底
DEBUGLINK_OK=0
for i in 1 2 3; do
    if objcopy --add-gnu-debuglink="$SYMBOLS_DIR/DevDesk.exe.debug" "$EXE_DST" 2>/dev/null; then
        DEBUGLINK_OK=1
        break
    fi
    sleep 2
done
[ "${DEBUGLINK_OK}" -eq 1 ] || echo "  [警告] --add-gnu-debuglink 三次失败（不影响运行；gdb 手动 symbol-file 加载符号）"

echo "[4/6] 拷贝 assets 与第三方声明…"
rm -rf "$TARGET/assets"
cp -R "$REPO_ROOT/build/assets" "$TARGET/assets"
NOTICES_SRC=""
for cand in "$REPO_ROOT/build/THIRD-PARTY-NOTICES.md" "$REPO_ROOT/THIRD-PARTY-NOTICES.md"; do
    [ -f "$cand" ] && { NOTICES_SRC="$cand"; break; }
done
if [ -n "$NOTICES_SRC" ]; then
    cp -f "$NOTICES_SRC" "$TARGET/THIRD-PARTY-NOTICES.md"
else
    echo "  [警告] THIRD-PARTY-NOTICES.md 未找到，跳过"
fi

echo "[5/6] 生成 sha256 清单…"
( cd "$TARGET" && find . -type f ! -name install-manifest-sha256.txt -print0 \
    | sort -z | xargs -0 sha256sum > install-manifest-sha256.txt )
sha256sum "$SYMBOLS_DIR/DevDesk.exe.debug" > "$SYMBOLS_DIR/DevDesk.exe.debug.sha256"

echo "[6/6] 完成。"
echo "  exe：$(size_mb "$(stat -c%s "$EXE_SRC")") → $(size_mb "$(stat -c%s "$EXE_DST")")"
echo "  符号：$(size_mb "$(stat -c%s "$SYMBOLS_DIR/DevDesk.exe.debug")") @ ${SYMBOLS_DIR}"
echo "  清单：${TARGET}/install-manifest-sha256.txt"

if [ "$LAUNCH" -eq 1 ]; then
    echo "[重启] 启动装机副本…"
    ( cd "$TARGET" && cmd //c start "" "$(cygpath -w "$EXE_DST")" )
fi
