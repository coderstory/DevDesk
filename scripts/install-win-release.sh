#!/bin/bash
# install-win-release.sh —— Windows Release 装机口径：Release 产物原样覆盖安装。
# 与 install-win-debug.sh 的差异（Release 发布形态，语义对齐 package-windows.sh）：
#   - 源 = <仓库>/build-release/DevDesk.exe（bash scripts/build-test-run.sh --release 产物）
#   - exe 原样出货：不 strip、不分离调试符号（package-windows.sh 全程无 strip；
#     Release 无 -g 调试段，strip 无收益且破坏「所装即所建」语义）
#   - mpv.exe 必须随装（安装版壁纸后端唯一解析位 = <安装目录>/assets/mpv/mpv.exe，
#     缺失=壁纸必坏；源缺失即失败，对齐 package-windows.sh 的硬断言口径）
#   - embedding 模型随装断言（need:241 / debt226）
#
# 流程：杀 DevDesk.exe → 拷 exe（sha256 校验）→ assets rm+全量拷 →
#       unpack-memory-assets → mpv 随装 → conpty redist → THIRD-PARTY-NOTICES.md →
#       sha256 清单 → 重启 app。
#
# 用法：
#   bash scripts/install-win-release.sh [--target DIR] [--no-launch]
#   默认 target=%LOCALAPPDATA%\Programs\DevDesk；先跑 bash scripts/build-test-run.sh --release。
#   演练：--target <临时目录> --no-launch；真装机由主会话统一执行。
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

EXE_SRC="$REPO_ROOT/build-release/DevDesk.exe"
[ -f "$EXE_SRC" ] || { echo "[错误] 找不到构建产物 ${EXE_SRC}，先跑 bash scripts/build-test-run.sh --release"; exit 1; }

# mpv 源（安装版唯一解析位=<安装目录>/assets/mpv/mpv.exe；对齐 package-windows.sh）
MPV_SRC="${MPV_BIN:-D:/software/mpv/mpv.exe}"
MPV_SRC_RAW="$MPV_SRC"
if command -v cygpath >/dev/null 2>&1; then
  # 容忍 Windows 形态（D:\... / D:/...）：统一转 POSIX；POSIX 路径原样透传
  MPV_SRC="$(cygpath -u "$MPV_SRC" 2>/dev/null || printf '%s' "$MPV_SRC")"
fi

# embedding 模型名与 package-windows.sh 同步
EMBED_MODEL_NAME="bge-small-zh-v1.5-q8_0.gguf"

EXE_DST="$TARGET/DevDesk.exe"
size_mb() { awk -v b="$1" 'BEGIN { printf "%.1fMB", b / 1048576 }'; }

echo "[1/7] 停止运行中的 DevDesk…"
taskkill //F //IM DevDesk.exe >/dev/null 2>&1 || echo "  无运行实例"
sleep 1

echo "[2/7] 拷贝 Release exe（原样，不 strip）…"
mkdir -p "$TARGET"
cp -f "$EXE_SRC" "$EXE_DST"
# 拷贝完整性：源/副本 sha256 必须一致（截断/半写当场识破）
SRC_H=$(sha256sum "$EXE_SRC" | cut -d' ' -f1)
DST_H=$(sha256sum "$EXE_DST" | cut -d' ' -f1)
[ "$SRC_H" = "$DST_H" ] || { echo "[错误] exe 哈希不符（拷贝不完整），中止"; exit 1; }
echo "  哈希一致：${DST_H:0:16}…（$(size_mb "$(stat -c%s "$EXE_DST")")）"

echo "[3/7] 刷新 assets（rm + 全量拷）…"
rm -rf "$TARGET/assets"
cp -R "$REPO_ROOT/build-release/assets" "$TARGET/assets"
# embedding 随包资产补解包（need:202 交付链）：已解包则 stamp 幂等跳过
bash "$REPO_ROOT/scripts/unpack-memory-assets.sh" "$TARGET" || true

echo "[4/7] mpv 随装（安装版壁纸后端唯一解析位）…"
if [ -f "$MPV_SRC" ]; then
    mkdir -p "$TARGET/assets/mpv"
    cp -f "$MPV_SRC" "$TARGET/assets/mpv/mpv.exe"
    echo "  mpv: $MPV_SRC_RAW → assets/mpv/mpv.exe"
elif [ -f "$TARGET/assets/mpv/mpv.exe" ]; then
    echo "  [警告] mpv 源缺席（$MPV_SRC_RAW），沿用已拷入的 build-release/assets 副本"
else
    echo "[错误] 找不到 mpv.exe（$MPV_SRC_RAW），且产物目录无副本——安装版壁纸必坏，中止"
    echo "   获取：https://github.com/shinchiro/mpv-winbuild-cmake/releases 后 MPV_BIN=<路径> 重跑"
    exit 1
fi

echo "[5/7] embedding 模型随装断言（need:241 / debt226）…"
EMBED_MODEL_DST="$TARGET/assets/models/$EMBED_MODEL_NAME"
[ -f "$EMBED_MODEL_DST" ] \
  || { echo "[错误] embedding 模型缺失（$EMBED_MODEL_DST）——记忆模块检索必坏，中止"; exit 1; }
echo "  模型就位：assets/models/${EMBED_MODEL_NAME}（$(size_mb "$(stat -c%s "$EMBED_MODEL_DST")")）"

# ConPTY v2 redist（debt246）：成对资产拷 exe 旁 conpty\（运行时三级查找第二级）。
# 缺席 = 告警不阻断——运行时回落 inbox 是正式降级路径；半目录必须拦（wtmux 实证坑）。
echo "[6/7] conpty redist + 第三方声明…"
if [ -f "$REPO_ROOT/third_party/conpty/conpty.dll" ] && [ -f "$REPO_ROOT/third_party/conpty/OpenConsole.exe" ]; then
    rm -rf "$TARGET/conpty"
    mkdir -p "$TARGET/conpty"
    cp -f "$REPO_ROOT/third_party/conpty/conpty.dll" "$TARGET/conpty/conpty.dll"
    cp -f "$REPO_ROOT/third_party/conpty/OpenConsole.exe" "$TARGET/conpty/OpenConsole.exe"
    echo "  conpty redist：conpty.dll + OpenConsole.exe → $TARGET/conpty"
elif [ -e "$REPO_ROOT/third_party/conpty/conpty.dll" ] || [ -e "$REPO_ROOT/third_party/conpty/OpenConsole.exe" ]; then
    echo "[错误] third_party/conpty/ 只有半个目录（conpty.dll 与 OpenConsole.exe 必须成对），中止"
    exit 1
else
    echo "  [警告] third_party/conpty/ 资产缺席（装机将回落 inbox ConPTY）"
fi
NOTICES_SRC=""
for cand in "$REPO_ROOT/build-release/THIRD-PARTY-NOTICES.md" "$REPO_ROOT/THIRD-PARTY-NOTICES.md"; do
    [ -f "$cand" ] && { NOTICES_SRC="$cand"; break; }
done
if [ -n "$NOTICES_SRC" ]; then
    cp -f "$NOTICES_SRC" "$TARGET/THIRD-PARTY-NOTICES.md"
else
    echo "  [警告] THIRD-PARTY-NOTICES.md 未找到，跳过"
fi

echo "[7/7] 生成 sha256 清单…"
( cd "$TARGET" && find . -type f ! -name install-manifest-sha256.txt -print0 \
    | sort -z | xargs -0 sha256sum > install-manifest-sha256.txt )
echo "完成。"
echo "  exe：$(size_mb "$(stat -c%s "$EXE_SRC")") → ${TARGET}/DevDesk.exe"
echo "  清单：${TARGET}/install-manifest-sha256.txt"

if [ "$LAUNCH" -eq 1 ]; then
    echo "[重启] 启动装机副本…"
    ( cd "$TARGET" && cmd //c start "" "$(cygpath -w "$EXE_DST")" )
fi
