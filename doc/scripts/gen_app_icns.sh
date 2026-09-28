#!/usr/bin/env bash
# =============================================================================
# gen_app_icns.sh —— 在 macOS 上从源 PNG 生成 DevDesk 的 .icns 图标
#
# 【重要】本脚本只能在 macOS 上运行（iconutil / sips 是 macOS 专有工具，
#   macOS 系统自带；Windows / Linux 上没有，脚本会给出清晰报错并退出）。
#   当前开发机是 Windows 11、仓库零 .icns（installer 设计 spec §4.4 第 1 项
#   "图标只能在 Mac 上 iconutil 生成"）；本脚本在 Windows 侧先行入库，
#   仅作为"把源图拷到 Mac 后一键生成"的载体，实际生成动作必须在 Mac 上做。
#
# 前置条件（源图三选一，按下列顺序取第一个存在的）：
#   1) 环境变量 DEVDESK_ICON_SRC=<某个 PNG 路径>（显式指定）
#   2) installer/macos/icon-1024.png（标准源图：1024×1024，被 installer/macos/.gitignore
#      忽略，需手工准备 —— 画质最好的一条路）
#   3) <构建目录>/assets/icon.png（构建时 POST_BUILD 从 EUI-NEO assets 拷来的图标，
#      零手工步骤、构过就能跑；当前是 360×351，大档靠 sips 放大、清晰度略降，故只作兜底）
#   构建目录 = DEVDESK_BUILD_DIR 或 <仓库根>/build（与 platform/macos/build-app.sh 同款开关）。
#   —— 只跑打包、不关心 icns 入库时不必用本脚本：build-app.sh 已内联等价的 sips+iconutil 步骤。
#   - 已安装 Xcode Command Line Tools（自带 iconutil / sips）
#
# 产物（均入库交付，spec §4.4 第 1 项）：
#   - installer/macos/DevDesk.icns      —— App 图标（.app bundle 的 CFBundleIconFile 用）
#   - installer/macos/.VolumeIcon.icns  —— DMG 卷图标（打包脚本拷到 DMG 根目录并命名
#                                           为 .VolumeIcon.icns 后，Finder 显示为卷图标）
#
# 用法：
#   bash doc/scripts/gen_app_icns.sh          # 从仓库根调用（脚本按自身位置定位路径，
#                                             从任意目录调用均可）
#
# 尺寸说明（Apple iconset 标准，共 10 张）：
#   16 / 32 / 128 / 256 / 512 各一张，及各自 @2x == 32 / 64 / 256 / 512 / 1024；
#   即 iconset 文件名为 icon_{16,16@2x,32,32@2x,128,128@2x,256,256@2x,512,512@2x}.png
# =============================================================================
set -euo pipefail

# 脚本所在目录向上两级 = 仓库根（无论从哪个 cwd 调用都成立）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/installer/macos"
BUILD_DIR="${DEVDESK_BUILD_DIR:-${REPO_ROOT}/build}"

# 源图解析：DEVDESK_ICON_SRC > installer/macos/icon-1024.png > <构建目录>/assets/icon.png
# （三级兜底的理由见文件头「前置条件」；缺全部才报错退出，不假装成功）
SRC_PNG="${DEVDESK_ICON_SRC:-${OUT_DIR}/icon-1024.png}"
if [ ! -f "${SRC_PNG}" ]; then
  SRC_PNG="${BUILD_DIR}/assets/icon.png"
fi

# ---------- 平台 / 工具前置检查（不满足就给清晰报错，不假装成功） ----------
case "$(uname -s)" in
  Darwin) : ;;
  *) echo "[ERROR] 本脚本只能在 macOS 上运行（iconutil / sips 是 macOS 专有工具）；当前系统: $(uname -s)" >&2; exit 1 ;;
esac

for tool in iconutil sips; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "[ERROR] 缺少 macOS 自带工具: ${tool}（正常 macOS 都带，请确认已装 Xcode Command Line Tools）" >&2
    exit 1
  fi
done

if [ ! -f "${SRC_PNG}" ]; then
  echo "[ERROR] 找不到源图（三处都不存在）：" >&2
  echo "        1) DEVDESK_ICON_SRC=<png 路径>" >&2
  echo "        2) ${OUT_DIR}/icon-1024.png" >&2
  echo "        3) ${BUILD_DIR}/assets/icon.png（需先构建过一次 DevDesk）" >&2
  exit 1
fi
echo "[INFO] 源图: ${SRC_PNG}"

# 源图解析不到尺寸（sips 失败）或任一边小于 1024 时给出警告（不阻断）：
# 小于 1024 意味着 512@2x（1024 像素）条目需要放大，清晰度会下降。
SRC_W="$(sips -g pixelWidth  "${SRC_PNG}" 2>/dev/null | awk '/pixelWidth/{print $2}')"
SRC_H="$(sips -g pixelHeight "${SRC_PNG}" 2>/dev/null | awk '/pixelHeight/{print $2}')"
if [ -n "${SRC_W}" ] && [ -n "${SRC_H}" ] && { [ "${SRC_W}" -lt 1024 ] || [ "${SRC_H}" -lt 1024 ]; }; then
  echo "[WARN] 源图尺寸为 ${SRC_W}×${SRC_H}，小于标准 1024×1024；小尺寸条目会放大、质量可能下降" >&2
fi

# ---------- 用 sips 生成标准 .iconset 的 10 张图（全部从 1024 源图向下缩放） ----------
TMP_ROOT="$(mktemp -d /tmp/DevDesk-icns.XXXXXX)"
ICONSET_DIR="${TMP_ROOT}/DevDesk.iconset"
mkdir -p "${ICONSET_DIR}"
trap 'rm -rf "${TMP_ROOT}"' EXIT

# 每行: <iconset 文件名> <目标像素边长>（sips -z 参数顺序为「高 宽」，两值相等无差别）
SIZES="
icon_16x16.png 16
icon_16x16@2x.png 32
icon_32x32.png 32
icon_32x32@2x.png 64
icon_128x128.png 128
icon_128x128@2x.png 256
icon_256x256.png 256
icon_256x256@2x.png 512
icon_512x512.png 512
icon_512x512@2x.png 1024
"
while read -r name px; do
  [ -z "${name}" ] && continue
  sips -z "${px}" "${px}" "${SRC_PNG}" --out "${ICONSET_DIR}/${name}" >/dev/null
done <<< "${SIZES}"

# ---------- iconutil 打包为 .icns（App 图标与 DMG 卷图标两份产物） ----------
mkdir -p "${OUT_DIR}"

iconutil -c icns -o "${OUT_DIR}/DevDesk.icns" "${ICONSET_DIR}"
iconutil -c icns -o "${OUT_DIR}/.VolumeIcon.icns" "${ICONSET_DIR}"

# ---------- 产物校验：存在且非空才算成功 ----------
for f in "${OUT_DIR}/DevDesk.icns" "${OUT_DIR}/.VolumeIcon.icns"; do
  if [ ! -s "${f}" ]; then
    echo "[ERROR] 产物缺失或为空: ${f}" >&2
    exit 1
  fi
done

echo "[OK] 已生成（$(wc -c < "${OUT_DIR}/DevDesk.icns") bytes）："
echo "    ${OUT_DIR}/DevDesk.icns"
echo "    ${OUT_DIR}/.VolumeIcon.icns"
echo "Mac 侧打包时将 .VolumeIcon.icns 放入 DMG 根目录即可显示卷图标。"
echo "记得显式入库：git add installer/macos/DevDesk.icns installer/macos/.VolumeIcon.icns"