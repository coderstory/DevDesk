#!/usr/bin/env bash
# **真机出网验收探针的唯一显式入口**：把「tools/quota_live_probe.cpp 到底怎么跑」变成一条命令。
#
# 该探针不进 CMake（不进 add_executable / add_test）：真网络 + 需要用户本机
# ~/.cc-switch/cc-switch.db + 每次实打四家供应商接口，进了 CI 必红，别处没凭据可跑。
# 这些理由一个都没消失，所以**它仍然不进默认门禁**。
#
#     bash scripts/run-quota-live-probe.sh                       # 跑四家
#     bash scripts/run-quota-live-probe.sh --dump /tmp/q.json    # 跑四家 + 落 JSON
#     bash scripts/run-quota-live-probe.sh opencode_go volcano   # 只跑指定几家
#     (cd build && ctest -L live)                                # ctest 入口
#
# 「绝不进默认门禁」是**代码保证**，不是注释保证：
#   ① 本脚本**不被** scripts/build-test-run.sh 调用（那里只跑 ctest -L probe）。
#   ② ctest 条目的 LABELS 是 **live**，不含 **probe** → 任何 `ctest -L probe` 都看不到它。
#      改 label 时这两处任一走漏，默认门禁就会开始每次真出网。
#   ①② 由**两个独立实现**同时守住（改坏一个另一个仍报警）：
#      · scripts/check-probe-gate-labels.sh —— awk 状态机，build-test-run.sh 串跑
#      · tests/test_modules/test_probe_gate_labels.cpp —— 默认门禁里跑的单测
#    两者解析路径刻意不同（awk vs C++ 逐行扫）：一个的解析逻辑被改坏时，另一个仍是有效
#    信号。动其中一方之前先确认另一方还在。
#
# 编译前先过两道静态守卫（check-quota-write-paths / check-probe-gate-labels），失败即中止：
# 探针链接的就是 devdesk_module_claude_proxy.a，完全有能力绕过 applyFetchResult 直写
# profile_quota_result，而 build-test-run.sh 那条路上只有 ctest -L probe——真机这条路上原本
# 没有任何补偿性检查，不该带着一个已知违规去打真网络。
#
# 凭据红线：只读 ~/.cc-switch/cc-switch.db（探针内部即 SQLITE_OPEN_READONLY），不写用户数据
# 根、不碰 Keychain、不启代理。所有 argv 原样透传给探针。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 构建目录：位置参数（若它是目录）> DEVDESK_BUILD_ROOT > <repo>/build。
# **不**手敲 cmake configure——CLAUDE.md 明令构建走 build-test-run.sh；本脚本只在既有构建
# 目录之上「编译一个不进 CMake 的工具」，不碰 configure 缓存。
BUILD_DIR="${DEVDESK_BUILD_ROOT:-${REPO_ROOT}/build}"
if [ "$#" -gt 0 ] && [ -d "$1" ]; then
    BUILD_DIR="$1"
    shift
fi

OUT="${BUILD_DIR}/quota_live_probe"

if [ ! -d "${BUILD_DIR}" ]; then
    echo "❌ 构建目录不存在：${BUILD_DIR}" >&2
    echo "   先跑一次：bash scripts/build-test-run.sh --no-run" >&2
    exit 2
fi

echo "── 编译前静态守卫 ──"
for guard in check-quota-write-paths check-probe-gate-labels; do
    if ! bash "${REPO_ROOT}/scripts/${guard}.sh"; then
        echo "❌ ${guard}.sh 失败 —— 已中止，不带着已知违规去跑真机探针" >&2
        exit 1
    fi
done

if [ -n "${CXX:-}" ]; then
    CC_BIN="${CXX}"
elif command -v c++ >/dev/null 2>&1; then
    CC_BIN="c++"
elif command -v g++ >/dev/null 2>&1; then
    CC_BIN="g++"
elif command -v clang++ >/dev/null 2>&1; then
    CC_BIN="clang++"
else
    echo "❌ 找不到 C++ 编译器（\$CXX / c++ / g++ / clang++）" >&2
    exit 2
fi
CCACHE_FLAGS=()
if command -v ccache >/dev/null 2>&1; then
    CCACHE_FLAGS=(ccache)
fi

LIB_ARGS=()
for lib in \
    "src/module_claude_proxy/libdevdesk_module_claude_proxy.a" \
    "src/platform/libdevdesk_platform.a" \
    "src/core/libdevdesk_core.a" \
    "src/ui_kit/libdevdesk_ui_kit.a"
do
    if [ ! -f "${BUILD_DIR}/${lib}" ]; then
        echo "❌ 缺静态库：${BUILD_DIR}/${lib}" >&2
        echo "   先跑一次：bash scripts/build-test-run.sh --no-run" >&2
        exit 2
    fi
    LIB_ARGS+=("${BUILD_DIR}/${lib}")
done

# yyjson：platform::json 的实现对象（EUI-NEO vendored，编进 EUI-NEO）。漏掉它 macOS 上
# 直接 link 失败（yyjson 符号未定义）——手敲的编译命令十有八九就漏在这一项。
YYJSON_OBJ="$(find "${BUILD_DIR}" -name 'yyjson.c.o' -print -quit 2>/dev/null || true)"
if [ -z "${YYJSON_OBJ}" ]; then
    echo "❌ 找不到 yyjson 编译产物（yyjson.c.o）——EUI-NEO 的 vendored 依赖" >&2
    echo "   在 ${BUILD_DIR} 下没编出来；先跑一次完整构建。" >&2
    exit 2
fi

EXTRA_FLAGS=()
for d in "${OPENSSL_ROOT_DIR:-}" /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
    if [ -n "${d}" ] && [ -d "${d}/lib" ]; then
        EXTRA_FLAGS+=("-L${d}/lib")
    fi
done

echo "== 编译真机探针 =="
echo "  build  = ${BUILD_DIR}"
echo "  cxx    = ${CC_BIN}"
echo "  yyjson = ${YYJSON_OBJ}"
"${CCACHE_FLAGS[@]}" "${CC_BIN}" -std=c++20 -g -O0 -Wall -Wextra \
    -I"${REPO_ROOT}/src" \
    -I"${REPO_ROOT}/src/module_claude_proxy" \
    "${REPO_ROOT}/tools/quota_live_probe.cpp" \
    "${LIB_ARGS[@]}" \
    "${YYJSON_OBJ}" \
    "${EXTRA_FLAGS[@]+"${EXTRA_FLAGS[@]}"}" \
    -lcrypto -lcurl -lsqlite3 \
    -o "${OUT}"

echo "== 运行（真网络；退出码 0 = 每家都拿到可判定的结论）=="
exec "${OUT}" "$@"