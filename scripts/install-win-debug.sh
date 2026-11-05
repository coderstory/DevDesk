#!/bin/bash
# install-win-debug.sh —— Windows 装机口径（2026-11-03 老板令）：Debug 产物 strip 后覆盖安装。
#
# 流程：杀 DevDesk.exe → 【快路径 debt193+debt253】build/DevDesk.stripped.exe 存在且清单两行
#       哈希（源 exe + 副本自身）sha256sum -c 全验通过（构建期已 strip+debuglink、符号已落
#       数据根）→ 直接拷贝，秒级；否则回退现场链路（~5min）：
#       objcopy --only-keep-debug 分离调试符号 → 拷贝 exe → strip --strip-debug →
#       --add-gnu-debuglink 回链 → 拷 assets + THIRD-PARTY-NOTICES.md → sha256 清单 → 重启 app。
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

# 快路径判据（debt193 + debt253）：构建脚本（build-test-run.sh 构建成功段）已产出 strip
# 副本并记录两行 sha256 清单（第一行源 exe、第二行副本自身）。判据 = 清单含副本行
#（老版单行清单只验源不验副本 → 视为无效）+ sha256sum -c 两行全验（源哈希一致 +
# 副本字节级完整：截断/改写必不符，旧「仅体积<源」判据挡不住）+ 符号文件在 → 直接
# 拷贝（秒级）；任一不满足 → 回退现场 strip 全链路（下方 fallback，逻辑原样保留）。
# 哈希判据与 mtime 无关（cp/touch 可造任意 mtime，字节比对才能回答「副本是不是这份
# exe 产出且未被截断」）。
STRIP_COPY="$REPO_ROOT/build/DevDesk.stripped.exe"
STRIP_HASH="$REPO_ROOT/build/DevDesk.stripped.sha256"
FAST_COPY=0
if [ -f "$STRIP_COPY" ] && [ -f "$STRIP_HASH" ] && [ -f "$SYMBOLS_DIR/DevDesk.exe.debug" ] \
   && grep -qF "DevDesk.stripped.exe" "$STRIP_HASH" \
   && (cd "$REPO_ROOT/build" && sha256sum -c DevDesk.stripped.sha256 >/dev/null 2>&1); then
    FAST_COPY=1
fi

echo "[1/6] 停止运行中的 DevDesk…"
taskkill //F //IM DevDesk.exe >/dev/null 2>&1 || echo "  无运行实例"
sleep 1

if [ "$FAST_COPY" -eq 1 ]; then
    echo "[2/6] 命中构建期 strip 副本（源+副本清单哈希全验）→ 直接拷贝（debt193 装机提速，秒级）"
    mkdir -p "$TARGET"
    cp -f "$STRIP_COPY" "$EXE_DST"
    # 与 fallback 同一条安全断言：装机体积必须小于源 exe，防「假 strip」出货
    EXE_SRC_SIZE=$(stat -c%s "$EXE_SRC")
    EXE_DST_SIZE=$(stat -c%s "$EXE_DST")
    if [ "${EXE_DST_SIZE}" -ge "${EXE_SRC_SIZE}" ]; then
        echo "[错误] strip 副本体积未缩小（${EXE_DST_SIZE} >= ${EXE_SRC_SIZE}），疑似假 strip，中止"
        echo "  （可 rm build/DevDesk.stripped.exe build/DevDesk.stripped.sha256 后重跑，走现场 strip fallback）"
        rm -f "$EXE_DST"
        exit 1
    fi
    echo "[3/6] 跳过现场 objcopy/strip（副本已在构建期完成 strip + debuglink）"
else
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
fi  # FAST_COPY 快/慢路径汇合（fast=拷副本；slow=现场 objcopy+strip+debuglink）

echo "[4/6] 拷贝 assets 与第三方声明…"
rm -rf "$TARGET/assets"
cp -R "$REPO_ROOT/build/assets" "$TARGET/assets"
# embedding 随包资产补解包（need:202 交付链）：build/assets 已解包则 stamp 跳过；
# 未解包则直接从仓内 7z 解到安装目录（告警不阻断——HF 下载/数据根兜底仍在）
bash "$REPO_ROOT/scripts/unpack-memory-assets.sh" "$TARGET" || true
NOTICES_SRC=""
for cand in "$REPO_ROOT/build/THIRD-PARTY-NOTICES.md" "$REPO_ROOT/THIRD-PARTY-NOTICES.md"; do
    [ -f "$cand" ] && { NOTICES_SRC="$cand"; break; }
done
if [ -n "$NOTICES_SRC" ]; then
    cp -f "$NOTICES_SRC" "$TARGET/THIRD-PARTY-NOTICES.md"
else
    echo "  [警告] THIRD-PARTY-NOTICES.md 未找到，跳过"
fi

# ConPTY v2 redist（debt246）：third_party/conpty/ 成对资产随装（exe 旁 conpty\，
# 运行时三级查找第二级）。缺资产 = 告警不阻断——运行时回落 inbox 是正式降级路径。
# [5/6] 的全目录 sha256 清单自动覆盖本目录，无需单独登记。
if [ -f "$REPO_ROOT/third_party/conpty/conpty.dll" ] && [ -f "$REPO_ROOT/third_party/conpty/OpenConsole.exe" ]; then
    rm -rf "$TARGET/conpty"
    mkdir -p "$TARGET/conpty"
    cp -f "$REPO_ROOT/third_party/conpty/conpty.dll" "$TARGET/conpty/conpty.dll"
    cp -f "$REPO_ROOT/third_party/conpty/OpenConsole.exe" "$TARGET/conpty/OpenConsole.exe"
    echo "  conpty redist：conpty.dll + OpenConsole.exe → $TARGET/conpty"
else
    echo "  [警告] third_party/conpty/ 资产缺席（放包契约见 third_party/conpty/README.md），装机将回落 inbox ConPTY"
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
