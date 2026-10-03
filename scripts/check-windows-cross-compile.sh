#!/usr/bin/env bash
# scripts/check-windows-cross-compile.sh
# Windows 侧源码的**交叉编译**守卫（2026-10-03，欠账清偿批）
#
# ── 它在守什么 ─────────────────────────────────────────────────────
# src/ 下 14 个 `*_win.cpp` 只在 CMake 的 `if(WIN32)` 分支里参与构建，而开发
# 平台是 macOS。本地 macOS 构建**永远不编译它们**，于是「Windows 侧写错了」要等到
# 两条很慢的路径之一才暴露：push 后的 GitHub Actions（10~18 分钟一轮），或
# Windows 回归期。2026-10-02 新增的 credential_protection_win.cpp（DPAPI 密文入库）
# 就属于「写完到 10-03 一次都没被任何编译器看过」那一档——静态审计只能逐行核对
# 微软官方文档的签名，核对不等于编译。
#
# 本守卫把「Windows 源码能不能编过」这一条从 CI 拉回本地：装一条 MinGW 交叉链，
# 就能在提交前几秒内验完。**它验的是编译，不是运行时**（边界见下）。
#
# ── 验到了什么 / 没验什么（边界，逐字对应任务书要求）────────────────
#   ✅ 验到：Win32 API 名字拼错、签名/参数类型不对、缺 include、类型不匹配、
#      隐式窄化告警、以及本仓库的 -Wall -Wextra -Wpedantic -Werror 全档告警。
#      —— 这些过去要等 CI 才有反馈，现在本地几秒出结果。
#   ❌ **验不到：任何运行时行为。** DPAPI 的 CryptProtectData 产出的密文对不对、
#      CryptUnprotectData 能不能还原、跨用户/跨机器会怎样失败——交叉编译只产出
#      .o，不执行一行代码。**「交叉编译通过」≠「DPAPI 行为正确」**，后者只有
#      Windows 真机能验。本守卫通过 ≠ 该文件已验证。
#   ❌ 验不到：链接期符号缺失（只编到 -c，不链接）、跨模块 ABI 不一致。
#   ❌ 本批编不到：http_client_ca_win.cpp 需要 libcurl 头（llvm-mingw 不带，
#      Homebrew mingw-w64 装不上），见下方 SKIP 清单——**如实标出，不假装验过**。
#
# ── 为什么找不到工具链时退出 0 而不是红 ──────────────────────────────
# 这是**故意的**，且必须如此：CI 的 Windows runner 装的是 MSYS2 MinGW（原生
# x86_64，非交叉），macOS runner 没装任何 MinGW 交叉链；开发者的 Linux 机器
# 也未必装。一个「别人机器上必红」的守卫会被立刻关掉，等于没写。所以：探测不到
# 工具链 → 打印明确原因（含怎么装）→ 退出 0。**装了链还编不过才是红。**
#
# ── --self-test：自证非恒真 ────────────────────────────────────────
# 一个从不变红的守卫等于没写（仓库反复翻车的那类假绿）。本守卫的 --self-test：
#   - 好树：拿真工具链编真源码 → 必须绿（证明"绿"是真绿，不是没干活）
#   - 坏树 A：注入未定义符号 → 必须红（证明真在编译，不是恒真）
#   - 坏树 B：注入未使用变量 → 必须红（证明 -Werror 真的开着）
#   - 坏树 C：拼错 Win32 API 名 → 必须红（证明 Win32 符号解析真的在跑）
#   - 断言：好树必须编出**非空 .o**（Total>=1 的同款纪律——"没报错"与"编出了东西"
#     是两件事，空跑一遍的守卫会以 0 退出）
#
# 退出码：0 = 通过（含"工具链缺失，显式跳过"）；1 = 有源码编不过；2 = 环境错
#
# 用法：bash scripts/check-windows-cross-compile.sh [--root DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-windows-cross-compile.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

# ── 工具链探测（不硬编码任何版本号 / 路径）────────────────────────────
# 顺序：显式环境变量 → PATH → ~/.local/llvm-mingw-* （glob，不锁版本号）
find_toolchain() {
    if [ -n "${MINGW_CXX:-}" ] && command -v "$MINGW_CXX" >/dev/null 2>&1; then
        command -v "$MINGW_CXX"; return 0
    fi
    if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        command -v x86_64-w64-mingw32-g++; return 0
    fi
    local d
    for d in "$HOME"/.local/llvm-mingw-*/bin; do
        if [ -x "$d/x86_64-w64-mingw32-g++" ]; then
            echo "$d/x86_64-w64-mingw32-g++"; return 0
        fi
    done
    return 1
}

CXX="$(find_toolchain || true)"

# 编译标志：与 cmake/CompilerWarnings.cmake 的 DevDesk 档一致（Clang/GNU 分支）。
# ⚠ 只取**可移植**子集：CompilerWarnings.cmake 里那五条 -Wno-error=（array-bounds /
# restrict / stringop-overflow / alloc-size-larger-than / conversion-null）是
# **GCC 专属**名，clang 见到会报 -Wunknown-warning-option，在 -Werror 下直接把
# 构建打死（实测）。llvm-mingw 的 g++ 驱动是 clang，所以这里绝不能带上它们。
# -Wno-error=missing-field-initializers 两个前端都认，保留。
CXXFLAGS=(
    -std=c++20
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
    -Wno-error=missing-field-initializers
)

# include 路径：只给 -I src 与各**目录自身**的 include 路径。
# ⚠ 绝不给 -I src/core：src/core/time.h 会经 -I 优先级劫持系统 <time.h>/<ctime>
#   （libc++ 内部链条实测编译爆炸）。core/CMakeLists.txt 里那条注释记的就是这件事。
INCS=(-I "${ROOT}/src"
      -I "${ROOT}/src/platform"
      -I "${ROOT}/src/module_settings")

# ── 受编清单 ────────────────────────────────────────────────────────
# 「源文件」+「额外 include 目录」（platform 下的文件要引 platform/ 里的兄弟头）。
# 每条都用 CMake 的 if(WIN32) 分支实际选中的源，**不是**按文件名 glob 出来的——
# glob 会把将来新增的、但 CMake 还没登记的文件也算进来，报一个与本守卫无关的红。
TARGETS=(
    "src/core/atomic_write_win.cpp"
    "src/core/file_perms_win.cpp"
    "src/core/paths_win.cpp"
    "src/core/time_win.cpp"
    "src/core/window_layout_win.cpp"
    "src/module_settings/window_effect_hint_win.cpp"
    "src/platform/credential_protection_win.cpp"
    "src/platform/file_open_win.cpp"
    "src/platform/file_watcher_win.cpp"
    "src/platform/keychain_win.cpp"
    "src/platform/system_theme_win.cpp"
    "src/platform/terminal_launch_win.cpp"
    "src/platform/tray_icon_win.cpp"
)

# 编不到 + 原因（**如实列出，不假装验过**）。要收口需先给工具链装 mingw-w64 的
# libcurl 开发头（MSYS2: pacman -S mingw-w64-x86_64-curl）。
SKIPPED=(
    "src/platform/http_client_ca_win.cpp|需 libcurl 头（<curl/curl.h> + CURLSSLOPT_NATIVE_CA），llvm-mingw 不自带"
)

# compile_one <file> <outdir> → 退出码即成败；日志落在 <outdir>/<base>.log
compile_one() {
    local file="$1" outdir="$2" base
    base="$(basename "$file" .cpp)"
    "$CXX" "${CXXFLAGS[@]}" -c "${ROOT}/${file}" "${INCS[@]}" \
        -o "${outdir}/${base}.o" >"${outdir}/${base}.log" 2>&1
}

if [ "$SELF_TEST" -eq 1 ]; then
    if [ -z "$CXX" ]; then
        echo "❌ --self-test 需要真实的 MinGW 交叉链（当前探测不到）" >&2
        echo "   本守卫的'红'必须由**真编译**产生；没有工具链时 --self-test 无法自证，" >&2
        echo "   退出非 0 而不是假装通过。" >&2
        exit 2
    fi
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    echo "自测：工具链 = ${CXX}"
    "$CXX" --version | head -1 | sed 's/^/       /'

    SRC_REL="src/platform/credential_protection_win.cpp"
    ok=0

    # ── 好树：真源码必须编过，且必须产出非空 .o ──
    if compile_one "$SRC_REL" "$TMP"; then
        if [ -s "${TMP}/credential_protection_win.o" ]; then
            echo "  ✅ 好树：绿（且产出非空 .o —— 真编了，不是没干活）"
        else
            echo "  ❌ 好树：退出码 0 但没产出 .o —— 假绿，守卫无效"
            ok=1
        fi
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫本身坏了（恒红守卫比没有守卫更糟）"
        sed 's/^/       /' "${TMP}/credential_protection_win.log" | head -5
        ok=1
    fi

    # ── 坏树：每个都必须红，且必须真的改动了文件 ──
    # 「等价变异不构成证据」：sed 没匹配上时产物与原文件逐字相同，编过是废话。
    # 所以每个坏树先 diff 断言**确实被改过**，再要求编译红。
    declare -a BAD_NAMES=(undef_symbol unused_var typo_winapi)
    declare -a BAD_SEDS=(
        's|    return core::Result<std::string>::ok(std::move(plain));|    return notARealSymbol_xyz;|'
        's|const unsigned char c = static_cast<unsigned char>(ch);|const unsigned char c = static_cast<unsigned char>(ch); int unused_probe_xyz = 0;|'
        's|CryptUnprotectData(|CryptUnprotectDataTypoXYZ(|'
    )

    for i in "${!BAD_NAMES[@]}"; do
        name="${BAD_NAMES[$i]}"
        mkdir -p "$TMP/$name/src/platform" "$TMP/$name/src/core"
        sed "${BAD_SEDS[$i]}" "${ROOT}/${SRC_REL}" > "$TMP/$name/${SRC_REL}"
        # 头文件靠 -I ROOT 才能找到，这里用真实 ROOT 的头，源码用变异体
        if diff -q "${ROOT}/${SRC_REL}" "$TMP/$name/${SRC_REL}" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$name]：sed 没匹配上（变异是空操作）—— 这条自测无效"
            ok=1
            continue
        fi
        if "$CXX" "${CXXFLAGS[@]}" -c "$TMP/$name/${SRC_REL}" \
               -I "${ROOT}/src" -I "${ROOT}/src/platform" \
               -o "$TMP/$name/out.o" >"$TMP/$name/log" 2>&1; then
            echo "  ❌ 坏树 [$name]：本该红却绿了 —— **假绿守卫**，断言无效"
            ok=1
        else
            echo "  ✅ 坏树 [$name]：红（已确认变异真的改了源码）"
        fi
    done

    exit "$ok"
fi

# ── 正常路径 ────────────────────────────────────────────────────────
if [ -z "$CXX" ]; then
    echo "Windows 交叉编译守卫：**跳过**（本机没装 MinGW 交叉链）"
    echo "  探测顺序：\$MINGW_CXX → PATH 的 x86_64-w64-mingw32-g++ → ~/.local/llvm-mingw-*/bin"
    echo "  装法（macOS）：解包 llvm-mingw 预编译包到 ~/.local/llvm-mingw-<版本>/，"
    echo "                或 pacman -S mingw-w64-x86_64-gcc（MSYS2）。"
    echo "  说明：CI 的 macOS runner 同样没有这条链，所以**跳过是正确行为而非失败**。"
    echo "        注意这只验得了编译——DPAPI 等运行时行为仍需 Windows 真机。"
    exit 0
fi

echo "Windows 交叉编译守卫：${CXX}"
"$CXX" --version | head -1 | sed 's/^/  /'

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

total=0; okc=0; failc=0; failed_list=""
for f in "${TARGETS[@]}"; do
    [ -f "${ROOT}/${f}" ] || { echo "❌ 清单里的文件不存在：${f}" >&2; exit 2; }
    total=$((total + 1))
    if compile_one "$f" "$OUT"; then
        okc=$((okc + 1))
        printf '  ✅ %s\n' "$f"
    else
        failc=$((failc + 1))
        failed_list="${failed_list} ${f}"
        printf '  ❌ %s\n' "$f"
        grep -E 'error:' "$OUT/$(basename "$f" .cpp).log" 2>/dev/null | head -5 | sed 's/^/       /'
    fi
done

# Total>=1 纪律：清单全空时"0 失败"是假绿，直接判环境错。
if [ "$total" -lt 1 ]; then
    echo "❌ 受编清单为空（total=0）—— 守卫没有实际检查任何东西，拒绝报绿" >&2
    exit 2
fi

echo "  ── 编译 ${okc}/${total} 通过"
if [ "${#SKIPPED[@]}" -gt 0 ]; then
    echo "  ── 编不到（如实列出，未验证）："
    for s in "${SKIPPED[@]}"; do
        echo "     · ${s%%|*} —— ${s##*|}"
    done
fi

if [ "$failc" -ne 0 ]; then
    echo "❌ ${failc} 个 Windows 源文件交叉编译失败：${failed_list}"
    echo "   注意：这**只**说明编不过。DPAPI 密文正确性、DPAPI 解密能否还原等运行时"
    echo "   行为，交叉编译一概验不到，仍需 Windows 真机。"
    exit 1
fi

echo "✅ Windows 侧源码交叉编译全通过（${total} 个文件）"
echo "   边界：验的是**编译**（API 存在 / 签名对 / 类型匹配 / -Werror 全档告警），"
echo "         不是运行时。DPAPI 行为仍需 Windows 真机验证。"
exit 0