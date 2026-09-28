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
# 前置条件：
#   - installer/macos/icon-1024.png 存在（1024×1024 源 PNG，需在 Mac 上准备；
#     本脚本不负责生成源图，源图缺失时报错退出、不假装成功）
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
SRC_PNG="${OUT_DIR}/icon-1024.png"

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
  echo "[ERROR] 源图不存在: ${SRC_PNG}" >&2
  echo "        请先在 Mac 上准备 1024×1024 的 PNG 放到上述路径后再跑本脚本。" >&2
  exit 1
fi

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