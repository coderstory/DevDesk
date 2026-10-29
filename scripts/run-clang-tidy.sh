#!/usr/bin/env bash
# scripts/run-clang-tidy.sh —— clang-tidy 报告模式的一体化包装（本地与 CI 同源）
#
# 票据：05 号票「clang-tidy 接入 CI（报告模式）」；计划依据 docs/plans/开发计划-2026-10-25.md T5。
# CI 侧（.github/workflows/ci.yml 的 windows job）直接调本脚本 —— configure 参数、tidy 范围、
# 检查集与本地完全同源；06 号票清存量时以本脚本为准，清零后 CI 去掉 continue-on-error 转
# blocking（本脚本无需改动）。
#
# 做四件事：
#   1. 探测 clang-tidy：DEVDESK_CLANG_TIDY > PATH > 常见安装位置（GitHub windows-2025 镜像
#      预装在 /c/Program Files/LLVM/bin）。找不到 → 打印安装途径并 exit 2（尽力而为语义）。
#   2. 以与 scripts/build-test-run.sh 相同的 configure 参数（Ninja、EUI-NEO 同款探测优先级、
#      Windows 的 OPENSSL_ROOT_DIR=MinGW 根）+ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 配置出
#      独立目录 build-tidy/ 的 compile_commands.json，与 Debug/Release 构建目录互不污染。
#      两个刻意的差异（都是为 tidy 数据库质量服务，注释在下方 configure 处）：
#      -DDEVDESK_CCACHE=OFF（ccache 前缀会破坏 clang-tidy 的 driver mode 推断）与
#      -DDEVDESK_BUILD_PROBES=OFF（probe 可执行是测试专用产物，不进 tidy 范围）。
#   3. 从 compile_commands.json 圈定本仓库 src/ 与 tests/ 的 .cpp：EUI-NEO 是 add_subdirectory
#      进来的，其翻译单元同样会进数据库，但不属本票对象（框架问题归 EUI-NEO 仓库）。
#   4. 跑检查集 bugprone-*,cert-*,clang-analyzer-*（--use-color），末尾按检查名汇总命中数。
#
# 退出码：0 = clang-tidy 正常跑完（有无 warning 都是 0 —— clang-tidy 自身语义）；
#         1 = configure / 编译数据库 / 源清单环节失败（环境问题，真实红）；
#         2 = 未找到 clang-tidy（附安装途径）。
# CI 侧由 step 的 continue-on-error 豁免全部非零码（报告模式，不阻断 job）。
#
# 工具链现实（报告模式而非 blocking 的原因之一）：本仓与 CI Windows job 都是 MinGW GCC
# 工具链，而 runner 预装的 LLVM 是 MSVC 目标的发行版 —— clang-tidy 解析 GCC 风格命令行的
# 诊断质量受工具链差异影响，可能出现单 TU 解析失败/参数不识别；存量清零阶段（06 号票）
# 按实际输出调优（必要时补 --extra-arg 或换 MSVC 前置生成器），不在本票范围。
#
# 用法：bash scripts/run-clang-tidy.sh
# 环境变量：DEVDESK_CLANG_TIDY=<clang-tidy 路径> 显式指定；
#           DEVDESK_TIDY_BUILD_DIR=<目录>   覆盖 tidy 构建目录（默认 build-tidy）；
#           DEVDESK_BUILD_TYPE=Debug|Release（默认 Debug，与一体化脚本同变量名）；
#           EUI_NEO_DIR=<EUI-NEO 路径>      （默认按一体化脚本同款优先级探测）。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CHECKS="bugprone-*,cert-*,clang-analyzer-*"
TIDY_BUILD_DIR="${DEVDESK_TIDY_BUILD_DIR:-build-tidy}"
BUILD_TYPE="${DEVDESK_BUILD_TYPE:-Debug}"

# ---- 1) 探测 clang-tidy ----
TIDY_BIN="${DEVDESK_CLANG_TIDY:-}"
if [ -z "$TIDY_BIN" ] && command -v clang-tidy >/dev/null 2>&1; then
  TIDY_BIN="$(command -v clang-tidy)"
fi
if [ -z "$TIDY_BIN" ]; then
  # 顺序：GitHub windows 镜像的预装位置在前，MinGW 自带包在后
  for _cand in "/c/Program Files/LLVM/bin/clang-tidy.exe" \
               "/c/Program Files/LLVM/bin/clang-tidy" \
               /mingw64/bin/clang-tidy.exe; do
    if [ -e "$_cand" ]; then TIDY_BIN="$_cand"; break; fi
  done
  unset _cand
fi
if [ -z "$TIDY_BIN" ]; then
  echo "❌ 未找到 clang-tidy。安装任选其一：" >&2
  echo "   winget install LLVM.LLVM      # GitHub windows runner 已预装于 C:\Program Files\LLVM\bin" >&2
  echo "   choco install llvm" >&2
  echo "   https://github.com/llvm/llvm-project/releases（win64 安装包）" >&2
  echo "   装在非默认路径时用 DEVDESK_CLANG_TIDY=<路径> 指定后重跑" >&2
  exit 2
fi
if [ ! -e "$TIDY_BIN" ]; then
  echo "❌ DEVDESK_CLANG_TIDY 指向的路径不存在: $TIDY_BIN" >&2
  exit 2
fi
echo "── clang-tidy: $TIDY_BIN ──"
"$TIDY_BIN" --version || true   # 版本留痕（票据要求：CI 日志可见工具版本）

# ---- 2) configure 出 compile_commands.json（与 build-test-run.sh 同参数）----
# 平台判定与 OPENSSL_ROOT_DIR 取法照搬一体化脚本：Windows 的 OpenSSL 不在 CMake 默认搜索
# 路径，必须显式给 MinGW 根；macOS 给 Homebrew openssl@3；Linux 不需要。
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows" ;;
  Darwin)               DEVDESK_OS="darwin" ;;
  *)                    DEVDESK_OS="linux" ;;
esac
OPENSSL_ARGS=()
if [ "$DEVDESK_OS" = "windows" ]; then
  if [ -z "${MINGW_PREFIX:-}" ]; then
    if command -v g++ >/dev/null 2>&1; then
      MINGW_PREFIX="$(cygpath -m "$(dirname "$(dirname "$(command -v g++)")")")"
    else
      for _cand in /d/software/msys64/mingw64 /c/msys64/mingw64; do
        if [ -d "$_cand" ]; then MINGW_PREFIX="$(cygpath -m "$_cand")"; break; fi
      done
      unset _cand
    fi
  fi
  OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR="${MINGW_PREFIX:-C:/msys64/mingw64}")
elif [ "$DEVDESK_OS" = "darwin" ]; then
  OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3)
fi

# EUI-NEO 定位：与 build-test-run.sh 同优先级（环境变量 > 仓库根 ./EUI-NEO submodule）
EUI_DIR="${EUI_NEO_DIR:-}"
if [ -z "$EUI_DIR" ]; then
  REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
  if [ -n "$REPO_ROOT" ] && [ -f "$REPO_ROOT/EUI-NEO/CMakeLists.txt" ]; then
    EUI_DIR="$REPO_ROOT/EUI-NEO"
  fi
fi
if [ -z "$EUI_DIR" ]; then
  echo "❌ 找不到 EUI-NEO（试过 \$EUI_NEO_DIR / 仓库根 ./EUI-NEO submodule；未检出先跑 git submodule update --init EUI-NEO）" >&2
  exit 1
fi

# 两个与正常构建的刻意差异（见文件头）：
#   DEVDESK_CCACHE=OFF —— 根 CMakeLists 会自动探测 ccache 挂 launcher，compile_commands.json
#   的命令行会变成 "ccache g++ ..."，clang-tidy 按命令首 token 推断 driver mode，ccache 在首
#   位 = 推断失效；tidy 库不真正编译，缓存无意义，必须关。
#   DEVDESK_BUILD_PROBES=OFF —— probe 可执行是测试专用产物，不进 tidy 范围（与 CI 的
#   DEVDESK_BUILD_PROBES=0 取向一致；probe 的 cpp 也不会出现在下面的范围清单里）。
echo "── configure: $TIDY_BUILD_DIR/（$BUILD_TYPE，EUI-NEO: $EUI_DIR）──"
CONF_OUT="$(cmake -B "$TIDY_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DEUI_NEO_DIR="$EUI_DIR" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DDEVDESK_CCACHE=OFF \
  -DDEVDESK_BUILD_PROBES=OFF \
  "${OPENSSL_ARGS[@]}" 2>&1)" || {
  echo "$CONF_OUT" | tail -30
  echo "❌ configure 失败（上方为错误摘要）"; exit 1
}

DB="$ROOT/$TIDY_BUILD_DIR/compile_commands.json"
if [ ! -s "$DB" ]; then
  echo "❌ $DB 不存在或为空（configure 未产出编译数据库）" >&2
  exit 1
fi
echo "── compile_commands.json: $(wc -l < "$DB" | tr -d ' ') 行 ──"

# ---- 3) 圈定 tidy 范围：本仓库 src/、tests/ 的 .cpp 且出现在数据库里 ----
# 数据库里的路径是 Windows 混合盘符形态（D:/...），与 pwd 的 POSIX 形态（/d/...）不同，
# 须换算成同形态后做字面前缀匹配；平台守卫文件（*_mac.cpp 等）在 Windows 数据库里没有
# 编译条目，天然被过滤。
ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
TIDY_FILES=()
while IFS= read -r f; do
  TIDY_FILES+=("$f")
done < <(sed -n 's/^[[:space:]]*"file":[[:space:]]*"\(.*\)",\{0,1\}$/\1/p' "$DB" \
  | awk -v root="$ROOT_WIN/" 'index($0, root) == 1' \
  | grep -E '(src|tests)/.*\.cpp$' \
  | sort -u)
if [ "${#TIDY_FILES[@]}" -eq 0 ]; then
  echo "❌ 未能从 compile_commands.json 提取本仓库 src/tests 的 .cpp（前缀: $ROOT_WIN/）" >&2
  exit 1
fi
echo "── tidy 范围: ${#TIDY_FILES[@]} 个 .cpp（本仓库 src/ + tests/；EUI-NEO/probe 不在列）──"

# ---- 4) 跑 clang-tidy（检查集同 CI；--use-color 诊断着色）----
# header-filter 限定本仓库 src/tests 头（路径含 /(src|tests)/ 才报），框架/系统头里的
# 连带诊断不刷屏。注意 clang-tidy 对有编译错误的 TU 也会给非零退出码 —— 报告模式下由
# CI 的 continue-on-error 豁免，本地看汇总与明细即可。
TIDY_LOG="$(mktemp -t devdesk-tidy.XXXXXX)"
TIDY_RC=0
"$TIDY_BIN" -p "$TIDY_BUILD_DIR" --use-color --checks="$CHECKS" \
  --header-filter='/(src|tests)/' "${TIDY_FILES[@]}" 2>&1 | tee "$TIDY_LOG"
TIDY_RC="${PIPESTATUS[0]}"

echo "── 检查命中汇总（按检查名计数，Top 20）──"
grep -oE '\[(bugprone|cert|clang-analyzer)-[A-Za-z0-9._-]+\]' "$TIDY_LOG" \
  | sort | uniq -c | sort -rn | head -20 || true
TOTAL_HITS="$(grep -cE '\[(bugprone|cert|clang-analyzer)-[A-Za-z0-9._-]+\]' "$TIDY_LOG" || true)"
echo "── 命中合计: ${TOTAL_HITS:-0} 条（完整明细见上方输出）──"
rm -f "$TIDY_LOG"

if [ "$TIDY_RC" -ne 0 ]; then
  echo "⚠ clang-tidy 退出码 $TIDY_RC（含编译错误/解析失败的 TU，明细见上方）"
  echo "   报告模式：CI 以 continue-on-error 豁免，不阻断 job —— 存量清零后转 blocking（06 号票）"
  exit "$TIDY_RC"
fi
echo "✅ clang-tidy 报告完成（报告模式：不阻断；存量清零与转 blocking 见 06 号票）"
exit 0
