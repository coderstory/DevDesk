#!/usr/bin/env bash
# DevDesk 覆盖率工具（macOS AppleClang / Windows MinGW GCC 双路线）
# 分层口径：core（核心工具类 ≥90%）/ platform（≥70%）/ module_*（Service ≥80%）/ ui_kit 不强制；
#   EUI-NEO / 测试 / 探针 / app 层源码不插桩（CMakeLists.txt 的 DEVDESK_COVERAGE 段），
#   其行数不进分层表，末尾单列「非 DevDesk 层」小计仅供参考。
#
# 路线一（darwin / AppleClang）：llvm source-based coverage
#   （-fprofile-instr-generate -fcoverage-mapping）：
#   跑 devdesk_tests 落 .profraw → xcrun llvm-profdata merge → xcrun llvm-cov 汇总。
#
# 路线二（windows / MinGW GCC，2026-10-26 08 号票落地）：gcov
#   DEVDESK_COVERAGE=ON → --coverage 编译插桩，跑 devdesk_tests 落 .gcda（对象文件旁）→
#   gcov 逐对象出 .gcov 文本 → awk 按 (文件,行) 并集聚合、按 src/<层>/ 分层汇总。
#   纯 awk 无 Python 依赖（CI 的 msys2 环境没有 python）。
#   探测到 gcovr 时额外出 HTML（gcovr 只管人看的 HTML，分层 txt 数字统一由轻方案口径出，
#   两边不各说各话）；gcovr 跑挂只丢 HTML 不丢数字。gcov/gcovr 都没有 → 打印安装途径 exit 2。
#
# 用法: bash scripts/coverage.sh [选项]
#   (无选项)      全流程：configure（DEVDESK_COVERAGE=ON、Debug → 独立目录 build-coverage/）
#                 → 编 devdesk_tests + DevDesk（后者只为 POST_BUILD 资产）→ 跑全量单测
#                 （Failed 非 0 即中止，不出假基线）→ 汇总分层报告
#   --clean       删掉 build-coverage/ 全量重来（插桩配置变更 / 缓存漂移时用）
#   --report-only 不构建不跑测试，只用已有数据重新出汇总表
#                 （macOS：已有 coverage.profdata；Windows：已有 .gcov（缺则从 .gcda 现烤））
#   --show <路径后缀…> 逐行标注视图；【仅 macOS 路线支持】参数按路径后缀匹配且必须带
#                 扩展名（如 src/core/sqlite_db.cpp，可给多个）。漏掉扩展名 llvm-cov
#                 匹配不到，会静默打印整库——见到整树输出就是过滤词写错了
#   --help        本说明（= 本段注释）
#
# 环境变量：DEVDESK_JOBS 并发编译单元数（默认 3，同 build-test-run.sh 封顶）；
#           EUI_NEO_DIR 显式指定框架源码（否则按 仓库根 ./EUI-NEO submodule 定位）；
#           DEVDESK_COV_BUILD_DIR 覆盖构建目录（仅 Windows 路线认，默认 build-coverage/）。
# 产物（都在构建目录下）：
#   coverage-summary.txt  分层表（本次跑的快照，两路线同名字段）
#   coverage-report.txt   完整明细（macOS：llvm-cov report 全表含非 DevDesk 文件；
#                         Windows：awk 出的逐文件行覆盖表）
#   coverage-export.json  llvm-cov export 原始 JSON（仅 macOS；Windows 路线无对应物）
#   coverage.html         gcovr HTML 报告（仅 Windows 且装了 gcovr）
#   coverage.profdata     合并后的 profile 数据；同目录 profraw/ 是原始采样（仅 macOS）
# 注意：
#   * 构建目录固定 build-coverage/（Windows 可用 DEVDESK_COV_BUILD_DIR 换），与 build/ /
#     build-release/ 互不覆盖。分层数字只有默认全流程跑出来的才算数——--report-only
#     不构建不跑测试，不刷新数据。
#   * 与 scripts/build-test-run.sh 互不调用、互不改动（门禁零变化）。
#     表头的日期 / git rev / 测试统计三个字段由 COV_DATE / COV_GIT / COV_STATS 喂给汇总段。
set -e

cd "$(dirname "$0")/.."

# 平台判定：只借 DEVDESK_OS 一处分支，平台 if 不散落在别处。
DEVDESK_OS="linux"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows" ;;
  Darwin)               DEVDESK_OS="darwin" ;;
  *)                    DEVDESK_OS="linux" ;;
esac

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'

  exit 0
fi

# EUI-NEO 定位（两路线共用）：仓内 submodule，仓库根 ./EUI-NEO。
locate_eui_neo() {
  local EUI_DIR="${EUI_NEO_DIR:-}"
  if [ -z "$EUI_DIR" ]; then
    local REPO_ROOT
    REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -n "$REPO_ROOT" ] && [ -f "$REPO_ROOT/EUI-NEO/CMakeLists.txt" ]; then
      EUI_DIR="$REPO_ROOT/EUI-NEO"
    fi
  fi
  if [ -z "$EUI_DIR" ]; then
    echo "❌ 找不到 EUI-NEO（试过 \$EUI_NEO_DIR / 仓库根 ./EUI-NEO submodule；未检出先跑 git submodule update --init EUI-NEO）" >&2
    return 1
  fi
  echo "$EUI_DIR"
}

# 并行度封顶（多 worktree 并发时防互拖，同门禁脚本）
export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"

# ═══════════════════════════════════════════════════════════════════════════
# Windows 路线（MinGW GCC gcov）：独立函数，跑完即退，不影响下方 macOS 主流程。
# ═══════════════════════════════════════════════════════════════════════════
run_windows_gcov() {
  # 参数解析：与 macOS 路线对齐 --clean / --report-only；--show 无对应物（gcov 文本
  # 视图要按对象逐个跑，且文本格式对人不友好），给明确报错不静默吞。
  local COV_CLEAN=0 COV_REPORT_ONLY=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --clean)       COV_CLEAN=1 ;;
      --report-only) COV_REPORT_ONLY=1 ;;
      --show)        echo "❌ --show 仅 macOS 路线支持（gcov 文本视图请直接翻 .gcov 文件）" >&2; exit 1 ;;
      *)             echo "❌ Windows 路线不认识参数：$1（支持 --clean / --report-only / --help）" >&2; exit 1 ;;
    esac
    shift
  done

  # 工具探测：gcov（轻方案必需）与 gcovr（可选 HTML 增强）。都缺 → 安装途径 exit 2
  #（尽力而为语义，同 run-clang-tidy.sh 的探测风格）。
  local GCOV_BIN GCOVR_BIN
  GCOV_BIN="$(command -v gcov || true)"
  GCOVR_BIN="$(command -v gcovr || true)"
  if [ -z "$GCOV_BIN" ]; then
    echo "❌ 找不到 gcov / gcovr —— Windows 覆盖率需要 MinGW GCC 工具链" >&2
    echo "   安装（MSYS2）: pacman -S mingw-w64-x86_64-gcc   # 自带 gcov" >&2
    echo "   可选 HTML:    pip install gcovr                 # 有则额外出 coverage.html" >&2
    exit 2
  fi

  local BUILD_DIR BUILD_DIR_ABS TEST_BIN GCOV_OUT SUMMARY_TXT REPORT_TXT HTML_TXT
  BUILD_DIR="${DEVDESK_COV_BUILD_DIR:-build-coverage}"
  BUILD_DIR_ABS="$PWD/$BUILD_DIR"
  [ -d "$BUILD_DIR" ] && BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd)"
  TEST_BIN="$BUILD_DIR_ABS/tests/devdesk_tests.exe"
  GCOV_OUT="$BUILD_DIR_ABS/gcov-out"
  SUMMARY_TXT="$BUILD_DIR_ABS/coverage-summary.txt"
  REPORT_TXT="$BUILD_DIR_ABS/coverage-report.txt"
  HTML_TXT="$BUILD_DIR_ABS/coverage.html"

  # 构建目录一致性守卫：目录已 configure 过但 DEVDESK_COVERAGE 不是 ON（或构建类型不是
  # Debug）就硬失败，防拿普通构建目录当覆盖率产物、或反之（口径同 macOS 路线）。
  if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    if ! grep -q "^DEVDESK_COVERAGE:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
      echo "❌ $BUILD_DIR 已按 DEVDESK_COVERAGE=OFF（或旧版配置）configure，不能当覆盖率目录用" >&2
      echo "   用 --clean 重建，或删掉该目录" >&2
      exit 1
    fi
    local CACHED_TYPE
    CACHED_TYPE="$(grep -m1 '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- | tr -d '\r')"
    if [ "$CACHED_TYPE" != "Debug" ]; then
      echo "❌ $BUILD_DIR 已按 $CACHED_TYPE 配置，本脚本固定 Debug" >&2
      exit 1
    fi
  fi

  if [ "$COV_CLEAN" = "1" ]; then
    echo "── 全量重建（rm -rf ${BUILD_DIR}）──"
    rm -rf "$BUILD_DIR"
    BUILD_DIR_ABS="$PWD/$BUILD_DIR"
  fi

  local EUI_DIR
  EUI_DIR="$(locate_eui_neo)"
  echo "── EUI-NEO: $EUI_DIR ──"

  # ccache：未安装自动降级。EUI-NEO 不插桩、flag 未变，缓存照常命中。
  local CCACHE_ARGS=()
  if command -v ccache >/dev/null 2>&1; then
    CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi

  if [ "$COV_REPORT_ONLY" = "0" ] && [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "── configure（DEVDESK_COVERAGE=ON → ${BUILD_DIR}）──"
    # OpenSSL 参数口径同 build-test-run.sh 的 windows 分支：不写死 C:/msys64，取
    # MINGW_PREFIX（MSYS2 shell 一定导出），没有才退回默认安装位置。
    local MINGW_ROOT="${MINGW_PREFIX:-C:/msys64/mingw64}"
    local CONF_OUT
    CONF_OUT=$(cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DDEVDESK_COVERAGE=ON \
      -DEUI_NEO_DIR="$EUI_DIR" \
      "${CCACHE_ARGS[@]}" \
      -DOPENSSL_ROOT_DIR="$MINGW_ROOT" 2>&1) || {
      echo "$CONF_OUT" | tail -30
      echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
    echo "✅ configure 完成"
  fi

  if [ "$COV_REPORT_ONLY" = "0" ]; then
    # 构建 devdesk_tests + DevDesk 两个目标；probes 不编——不进覆盖率口径不白编。
    # 编 DevDesk 不是要跑它，是吃它的 POST_BUILD 资产拷贝（test_tray_icon 按「构建产物
    # assets」形态在 <build>/assets/ 下找文件，只编 devdesk_tests 时该目录是空的、用例必红）。
    echo "── 构建（devdesk_tests + DevDesk（POST_BUILD 资产），插桩）──"
    local BUILD_OUT
    BUILD_OUT=$(cmake --build "$BUILD_DIR" --target devdesk_tests DevDesk 2>&1) || {
      echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
      echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
    echo "$BUILD_OUT" | tail -2
    echo "✅ 构建成功"

    # 跑全量单测（Failed 非 0 即中止，不拿红基线出覆盖率）。
    echo "── 测试（采集 .gcda）──"
    # 只留本次数据：增量构建会留陈旧 .gcda，混进本次汇总就是假数字。
    find "$BUILD_DIR" -name '*.gcda' -delete 2>/dev/null || true
    # 测试进程依赖 MinGW DLL（libstdc++-6.dll 等，在 gcc/gcov 同目录）；tests/doctest.h
    # 的 TMPDIR 兜底是相对 cwd 的，必设 DEVDESK_TEST_TMPDIR（口径同 build-test-run.sh）。
    export PATH="$(dirname "$GCOV_BIN"):$PATH"
    export DEVDESK_TEST_TMPDIR="$BUILD_DIR_ABS/testtmp"
    mkdir -p "$DEVDESK_TEST_TMPDIR"
    local TEST_OUT
    TEST_OUT=$("$TEST_BIN" 2>&1) || true
    local FAIL_LINES
    FAIL_LINES="$(echo "$TEST_OUT" | grep "\[FAIL\]" | head -20)"
    if [ -n "$FAIL_LINES" ]; then echo "$FAIL_LINES"; fi
    COV_TEST_STATS="$(echo "$TEST_OUT" | grep -E "^(Total|Passed|Failed):" | tr '\n' ' ')"
    echo "$COV_TEST_STATS"
    echo ""
    if ! echo "$TEST_OUT" | grep -q "Failed: 0"; then
      echo "❌ 测试有失败——覆盖率基线必须建立在全绿之上，中止（完整输出跑: ${TEST_BIN}）" >&2
      exit 1
    fi
    echo "✅ 测试全绿"
  fi

  # gcov 轻方案：逐 .gcda 跑 gcov，.gcov 文本统一收集到 gcov-out/。
  # 每个 .gcda 独立子目录跑（cwd=GCOV_OUT/<seq>，gcov 把 .gcov 落在 cwd）：同一头文件
  # 被多个 TU include 时各 TU 都会产出同名 .gcov，混着跑会互相覆盖丢数据。
  # --report-only 且已有 .gcov 时跳过这一步（不构建不跑测试，也不重烤文本）。
  if [ "$COV_REPORT_ONLY" = "1" ] && find "$GCOV_OUT" -name '*.gcov' 2>/dev/null | grep -q .; then
    echo "── report-only：复用已有 .gcov（不重跑 gcov）──"
  else
    echo "── gcov：逐对象出 .gcov 文本 ──"
    rm -rf "$GCOV_OUT"
    local gcda seq=0
    # 双重约束同时满足：
    #   * cwd 必须是构建目录——gcno 里头文件路径是相对编译时工作目录记录的
    #     （如 ../src/core/result.h），cwd 不对则 gcov 找不到源文件、整段丢弃；
    #   * 输出必须逐对象隔离——同一头文件被多个 TU include 时各 .gcov 同名，
    #     直接落盘会互相覆盖丢数据。
    # 解法：gcov -t 把 .gcov 文本打到 stdout（GCC 12+），按 gcda 序号各自重定向成独立
    # 文件。gcda 参数用绝对路径（BUILD_DIR_ABS 是 MSYS 形式，转手给原生 gcov.exe 时
    # 会被 MSYS 转成 Windows 路径；相对路径原样穿透，必挂）。
    while IFS= read -r gcda; do
      seq=$((seq + 1))
      mkdir -p "$GCOV_OUT/$seq"
      (cd "$BUILD_DIR_ABS" && "$GCOV_BIN" -t "$gcda" > "$GCOV_OUT/$seq/dump.gcov") || true
    done < <(find "$BUILD_DIR_ABS" -name '*.gcda')
    echo "   （$seq 个对象的数据文件）"
  fi
  if ! find "$GCOV_OUT" -name '*.gcov' 2>/dev/null | grep -q .; then
    echo "❌ 没有任何 .gcov 数据——插桩没生效（$BUILD_DIR/CMakeCache.txt 应有 DEVDESK_COVERAGE:BOOL=ON，" >&2
    echo "   configure 输出应有「覆盖率插桩: ON（GNU gcov…）」）或测试进程没写出 .gcda" >&2
    exit 1
  fi

  # gcovr 可选增强：只管 HTML（人看），分层 txt 统一由下方 awk 轻方案出——分层口径
  # 只有一份实现，两边不各说各话。gcovr 挂了只丢 HTML，不丢数字。
  if [ "$COV_REPORT_ONLY" = "0" ] && [ -n "$GCOVR_BIN" ]; then
    echo "── gcovr: $GCOVR_BIN → coverage.html ──"
    if ! "$GCOVR_BIN" -r "$PWD" --object-directory "$BUILD_DIR_ABS" \
         --html "$HTML_TXT" >/dev/null 2>&1; then
      echo "⚠ gcovr HTML 生成失败（跳过，不影响 txt 数字）" >&2
    fi
  fi

  echo "── 汇总（awk 按 (文件,行) 并集 + 分层）──"
  local RUN_DATE GIT_REV
  RUN_DATE="$(date '+%Y-%m-%d %H:%M')"
  GIT_REV="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  COV_TEST_STATS="${COV_TEST_STATS:-}"
  # awk 程序落成文件再 -f：MSYS bash 把命令行参数里的 `\\` 转手给原生 awk.exe 时会
  # 丢一层反斜杠（Windows argv 重组），内嵌单引号大程序直接传参必坏；文件路线不经手。
  local AWK_PROG="$GCOV_OUT/summarize.awk"
  cat > "$AWK_PROG" <<'AWK'
    # ── 解析 .gcov 文本：每行 "<count|->:<行号>:<代码>" ──
    # 首字段：- = 非执行行；##### / ===== = 未执行；纯数字 = 执行次数。
    # 同一源文件会被多个 TU 的 .gcov 各写一份（inline 函数跨 TU 实例），按 (文件,行)
    # 并集聚合：行在任一 TU 视角下执行过即算覆盖——行覆盖的通行口径。
    {
      s = $0
      sub(/^[ \t]+/, "", s)
      c = index(s, ":"); if (c == 0) next
      f1 = substr(s, 1, c - 1); rest = substr(s, c + 1)
      c2 = index(rest, ":"); if (c2 == 0) next
      ln = substr(rest, 1, c2 - 1) + 0
      if (ln == 0) {
        # 元数据行（Source:/Graph:/Data:/Runs:）：行号字段前有空格填充，先去再匹配
        sub(/^[ \t]+/, "", rest)
        if (rest ~ /^0:Source:/) {
          src = substr(rest, 10)
          gsub(/\\/, "/", src)
          cur = src
          # 同一源文件出现在多个 .gcov（跨 TU inline）：文件清单去重登记。
          # 不能拿 ftot 判重——那是 END 里才初始化的聚合表，body 阶段恒空。
          if (!(cur in seen)) { seen[cur] = 1; seen_order[++nfiles] = cur }
        }
        next
      }
      if (cur == "") next
      if (f1 == "-") next   # 非执行行（纯声明/注释/括号）不进行覆盖统计
      key = cur SUBSEP ln
      if (!(key in ltot)) { ltot[key] = 1 }
      if (f1 ~ /^[0-9]+$/) { lcov[key] = 1 }
    }
    END {
      # (文件,行) 并集 → 按文件聚合计数
      for (k in ltot) {
        idx = index(k, SUBSEP)
        f = substr(k, 1, idx - 1)
        if (!(f in ftot)) { ftot[f] = 0; fcov[f] = 0 }
        ftot[f]++
        if (k in lcov) fcov[f]++
      }
      # 分层：路径里 src/<layer>/ 决定归属；匹配不到（EUI-NEO 头内联进插桩 TU 的代码、
      # 系统头）归「非 DevDesk 层」只给参考小计。纯 POSIX awk，不依赖 gawk 扩展。
      lt_n["core"] = 0; lt_n["platform"] = 0; lt_n["ui_kit"] = 0
      for (i = 1; i <= nfiles; i++) {
        f = seen_order[i]
        layer = "__other__"
        if (match(f, /src[\/](core|platform|ui_kit|module_[a-z0-9_]+)[\/]/)) {
          tail = substr(f, RSTART + 4)
          layer = tail
          sub(/[\/].*$/, "", layer)
        }
        if (!(layer in ltot_a)) { mod_order[++nmod] = layer }
        ltot_a[layer] += ftot[f]
        lcov_a[layer] += fcov[f]
        lfiles[layer]++
      }
      # 输出顺序：core → platform → ui_kit → module_*（字典序）→ 非DevDesk层。
      # awk 无 sort，层名就几个，插入排序足够。
      order[1] = "core"; order[2] = "platform"; order[3] = "ui_kit"
      no = 3
      for (i = 1; i <= nmod; i++) {
        l = mod_order[i]
        # 固定前三层已在 order[1..3]，重复插入会整表重号
        if (l == "__other__" || l == "core" || l == "platform" || l == "ui_kit") continue
        j = no
        while (j > 3 && order[j] > l) { order[j + 1] = order[j]; j-- }
        order[j + 1] = l; no++
      }
      if ("__other__" in ltot_a) order[++no] = "__other__"

      NEED["core"] = 90.0; NEED["platform"] = 70.0

      # ── 分层表（coverage-summary.txt）──
      head = "DevDesk 覆盖率（gcov，行覆盖；" COV_DATE "   git: " COV_GIT "）"
      print head > SUMMARY
      print "- 测试: " (COV_STATS != "" ? COV_STATS : "（report-only，未刷新数据）") > SUMMARY
      print "- 数据: " GCOV_OUT "/*.gcov ← devdesk_tests 全量单测" > SUMMARY
      print "        插桩口径: CMakeLists.txt 的 DEVDESK_COVERAGE 段（EUI-NEO/测试/探针/app 源码不插桩）" > SUMMARY
      print "" > SUMMARY
      printf "%-20s %5s %16s %7s  %-16s\n", "层", "文件", "行覆盖", "%", "目标(spec §12.3)" > SUMMARY
      sep = sprintf("%72s", ""); gsub(/ /, "-", sep)
      print sep > SUMMARY
      for (i = 1; i <= no; i++) {
        l = order[i]
        t = ltot_a[l]; c = lcov_a[l]
        p = (t ? sprintf("%.1f", 100.0 * c / t) : "-")
        if (l == "__other__") { label = "非DevDesk层(参考)"; target = "-"; mark = "" }
        else {
          if (l == "ui_kit") { label = l; target = "UI 不强制"; mark = "" }
          else if (l in NEED) {
            label = l
            target = (l == "core" ? "核心工具类 >=90%" : ">=70%")
            mark = (t && 100.0 * c / t >= NEED[l] ? "Y" : "N")
          }
          else {
            label = "module:" substr(l, 8)
            target = "Service >=80%"
            mark = (t && 100.0 * c / t >= 80.0 ? "Y" : "N")
          }
          if (l != "__other__") { own_t += t; own_c += c; own_n += lfiles[l] }
        }
        printf "%-20s %5d %16s %7s  %-16s %s\n", label, lfiles[l], c "/" t, p, target, mark > SUMMARY
      }
      print sep > SUMMARY
      op = (own_t ? sprintf("%.1f", 100.0 * own_c / own_t) : "-")
      printf "%-20s %5d %16s %7s\n", "DevDesk 合计", own_n, own_c "/" own_t, op > SUMMARY
      close(SUMMARY)

      # ── 逐文件明细（coverage-report.txt）──
      for (i = 1; i <= nfiles; i++) {
        f = seen_order[i]
        t = ftot[f]; c = fcov[f]
        p = (t ? sprintf("%.1f", 100.0 * c / t) : "-")
        printf "%7.1f%%  %6d/%-6d  %s\n", (t ? 100.0 * c / t : 0), c, t, f > REPORT
      }
      close(REPORT)
    }
AWK
  # 把所有 .gcov 内容按序拼进一个文件再喂 awk：awk 吃的是 .gcov 的行格式（路径列表
  # 行没有冒号会被整体跳过）；用临时文件做 stdin，也绕开几百个对象路径塞 argv 的
  # Windows 命令行长度上限。
  local ALL_TXT="$GCOV_OUT/all.gcov.txt"
  : > "$ALL_TXT"
  find "$GCOV_OUT" -name '*.gcov' -exec cat {} + >> "$ALL_TXT"
  [ -s "$ALL_TXT" ] || { echo "❌ 没有收集到任何 .gcov 内容" >&2; exit 1; }
  awk -f "$AWK_PROG" \
    -v SUMMARY="$SUMMARY_TXT" -v REPORT="$REPORT_TXT" -v GCOV_OUT="$GCOV_OUT" \
    -v COV_DATE="$RUN_DATE" -v COV_GIT="$GIT_REV" -v COV_STATS="$COV_TEST_STATS" < "$ALL_TXT"
  echo "✅ 汇总完成"
  echo ""
  echo "════ 覆盖率分层表 ════"
  cat "$SUMMARY_TXT"
  echo ""
  echo "（分层表: $SUMMARY_TXT ；逐文件明细: $REPORT_TXT$([ -f "$HTML_TXT" ] && echo " ；HTML: $HTML_TXT")）"
}

if [ "$DEVDESK_OS" = "windows" ]; then
  run_windows_gcov "$@"
  exit $?
fi

# 本脚本 darwin 路线之外的平台出声拒绝，不静默跑出错误口径。
if [ "$DEVDESK_OS" != "darwin" ]; then
  echo "❌ scripts/coverage.sh 支持 macOS（AppleClang）与 Windows（MinGW GCC），当前平台：${DEVDESK_OS}" >&2
  echo "   Linux 未验证过构建，暂不提供覆盖率路线" >&2
  exit 1
fi

# 工具链 PATH：cmake/ninja/ccache 在 /opt/homebrew/bin（幂等前置，与门禁脚本一致）
case ":$PATH:" in
  *:/opt/homebrew/bin:*) ;;
  *) if [ -d /opt/homebrew/bin ]; then export PATH="/opt/homebrew/bin:$PATH"; fi ;;
esac

BUILD_DIR="build-coverage"
BUILD_DIR_ABS="$PWD/$BUILD_DIR"
[ -d "$BUILD_DIR" ] && BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd)"
TEST_BIN="$BUILD_DIR_ABS/tests/devdesk_tests"
PROFRAW_DIR="$BUILD_DIR_ABS/profraw"
PROFDATA="$BUILD_DIR_ABS/coverage.profdata"
REPORT_TXT="$BUILD_DIR_ABS/coverage-report.txt"
SUMMARY_TXT="$BUILD_DIR_ABS/coverage-summary.txt"
EXPORT_JSON="$BUILD_DIR_ABS/coverage-export.json"

# llvm 工具走 xcrun：与 AppleClang 编译器同一 toolchain，杜绝 profraw 版本不匹配。
LLVM_PROFDATA="$(xcrun --find llvm-profdata)" || { echo "❌ xcrun 找不到 llvm-profdata（要装 Xcode CLT）" >&2; exit 1; }
LLVM_COV="$(xcrun --find llvm-cov)" || { echo "❌ xcrun 找不到 llvm-cov（要装 Xcode CLT）" >&2; exit 1; }

CLEAN=0; REPORT_ONLY=0; SHOW_MODE=0
for arg in "$@"; do
  case "$arg" in
    --clean)        CLEAN=1 ;;
    --report-only)  REPORT_ONLY=1 ;;
    --show)         SHOW_MODE=1 ;;
    *)              break ;;
  esac
done

if [ "$SHOW_MODE" = "1" ]; then
  while [ "${1:-}" = "--show" ]; do shift; done
  if [ ! -f "$PROFDATA" ] || [ ! -x "$TEST_BIN" ]; then
    echo "❌ 缺 $PROFDATA 或 $TEST_BIN —— 先跑一次全流程（bash scripts/coverage.sh）" >&2
    exit 1
  fi
  exec "$LLVM_COV" show "$TEST_BIN" -instr-profile="$PROFDATA" "$@"
fi

# 汇总生成（全流程末段与 --report-only 共用）：全表（llvm-cov report，含非 DevDesk 文件）
# 落 $REPORT_TXT 留档；分层表用 export JSON 分组计算——report 终端表会截断长路径，不适合
# 脚本解析，只作人读的完整明细。
generate_report() {
  if [ ! -f "$PROFDATA" ] || [ ! -x "$TEST_BIN" ]; then
    echo "❌ 缺 $PROFDATA 或 $TEST_BIN —— 先跑一次全流程（bash scripts/coverage.sh）" >&2
    exit 1
  fi
  "$LLVM_COV" report "$TEST_BIN" -instr-profile="$PROFDATA" > "$REPORT_TXT"
  "$LLVM_COV" export -format=text -summary-only "$TEST_BIN" -instr-profile="$PROFDATA" > "$EXPORT_JSON"
  RUN_DATE="$(date '+%Y-%m-%d %H:%M')"
  GIT_REV="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  COV_DATE="$RUN_DATE" COV_GIT="$GIT_REV" COV_STATS="${COV_TEST_STATS:-}" \
    python3 - "$EXPORT_JSON" "$SUMMARY_TXT" <<'PY'
import json, os, re, sys

export_path, summary_path = sys.argv[1], sys.argv[2]
with open(export_path) as fh:
    data = json.load(fh)
files = data["data"][0]["files"]

# 分层：路径里的 src/<layer>/ 前缀决定归属（core/platform/ui_kit/module_*）；
# 匹配不到的（EUI-NEO 头、系统头等被内联进插桩 TU 的代码）归「非 DevDesk 层」，
# 只给参考小计，不进分层口径。正则对绝对/相对、正反斜杠路径都成立。
layer_re = re.compile(r"(^|[\\/])src/(core|platform|ui_kit|module_[a-z0-9_]+)([\\/]|$)")
buckets = {}
for f in files:
    m = layer_re.search(f["filename"])
    layer = m.group(2) if m else "__other__"
    b = buckets.setdefault(layer, [0, 0, 0])   # [总行, 覆盖行, 文件数]
    s = f["summary"]["lines"]
    b[0] += s["count"]
    b[1] += s["covered"]
    b[2] += 1

# spec §12.3 目标：核心工具类 ≥90% / Platform ≥70% / Service(各模块) ≥80% / UI 不强制
LABEL = {"core": "core", "platform": "platform", "ui_kit": "ui_kit"}
NEED = {"core": 90.0, "platform": 70.0}
order = [l for l in ("core", "platform", "ui_kit") if l in buckets]
order += sorted(l for l in buckets if l.startswith("module_"))
if "__other__" in buckets:
    order.append("__other__")

def pct(c, t):
    return "%.1f" % (100.0 * c / t) if t else "-"

own = [0, 0, 0]
out = []
out.append("DevDesk 覆盖率（llvm-cov，行覆盖）")
out.append("- 日期: %s   git: %s" % (os.environ.get("COV_DATE", ""), os.environ.get("COV_GIT", "")))
out.append("- 测试: %s" % (os.environ.get("COV_STATS") or "（report-only，未刷新数据）"))
out.append("- 数据: build-coverage/coverage.profdata ← devdesk_tests 全量单测")
out.append("        插桩口径: CMakeLists.txt 的 DEVDESK_COVERAGE 段（EUI-NEO/测试/探针/app 源码不插桩）")
out.append("")
out.append("%-20s %5s %16s %7s  %-16s" % ("层", "文件", "行覆盖", "%", "目标(spec §12.3)"))
out.append("-" * 72)
for layer in order:
    t, c, n = buckets[layer]
    if layer == "__other__":
        label, target, mark = "非DevDesk层(参考)", "-", ""
    else:
        label = layer if layer in LABEL else "module:" + layer[len("module_"):]
        if layer == "ui_kit":
            target, mark = "UI 不强制", ""
        elif layer in NEED:
            target = "核心工具类 >=90%" if layer == "core" else ">=70%"
            mark = "Y" if t and 100.0 * c / t >= NEED[layer] else "N"
        else:
            target, mark = "Service >=80%", "Y" if t and 100.0 * c / t >= 80.0 else "N"
        own[0] += t; own[1] += c; own[2] += n
    out.append("%-20s %5d %16s %7s  %-16s %s" % (label, n, "%d/%d" % (c, t), pct(c, t), target, mark))
out.append("-" * 72)
out.append("%-20s %5d %16s %7s" % ("DevDesk 合计", own[2], "%d/%d" % (own[1], own[0]), pct(own[1], own[0])))
text = "\n".join(out)
print(text)
with open(summary_path, "w") as fh:
    fh.write(text + "\n")
PY
}

if [ "$REPORT_ONLY" = "1" ]; then
  echo "── 汇总（report-only，数据 = 已有 ${PROFDATA}）──"
  generate_report
  exit 0
fi

if [ "$CLEAN" = "1" ]; then
  echo "── 全量重建（rm -rf ${BUILD_DIR}）──"
  rm -rf "$BUILD_DIR"
fi

EUI_DIR="$(locate_eui_neo)"
echo "── EUI-NEO: $EUI_DIR ──"

# ccache：未安装自动降级。EUI-NEO 不插桩、flag 未变，缓存照常命中。
CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# configure：只在目录未初始化或 --clean 后，构建类型固定 Debug。一致性守卫——目录已
# configure 过但 DEVDESK_COVERAGE 不是 ON（或构建类型不是 Debug）就硬失败，防拿普通构建
# 目录当覆盖率产物、或反之。
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  if ! grep -q "^DEVDESK_COVERAGE:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "❌ $BUILD_DIR 已按 DEVDESK_COVERAGE=OFF（或旧版配置）configure，不能当覆盖率目录用" >&2
    echo "   用 --clean 重建，或删掉该目录" >&2
    exit 1
  fi
  CACHED_TYPE="$(grep -m1 '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- | tr -d '\r')"
  if [ "$CACHED_TYPE" != "Debug" ]; then
    echo "❌ $BUILD_DIR 已按 $CACHED_TYPE 配置，本脚本固定 Debug" >&2
    exit 1
  fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "── configure（DEVDESK_COVERAGE=ON → ${BUILD_DIR}）──"
  CONF_OUT=$(cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DDEVDESK_COVERAGE=ON \
    -DEUI_NEO_DIR="$EUI_DIR" \
    "${CCACHE_ARGS[@]}" \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 2>&1) || {
    echo "$CONF_OUT" | tail -30
    echo "❌ configure 失败（上方为错误摘要）"; exit 1; }
  echo "✅ configure 完成"
fi

# 构建 devdesk_tests + DevDesk 两个目标；probes 不编——不进覆盖率口径不白编。**编 DevDesk
# 不是要跑它**，是吃它的 POST_BUILD 资产拷贝（assets/pets、托盘图标、splash shader →
# <build>/assets/）：test_tray_icon 的第 3 条断言按「构建产物 assets」形态在
# <build>/assets/ 下找 defaultIconPath() 指向的文件，只编 devdesk_tests 时该目录是空的、
# 用例必红。
echo "── 构建（devdesk_tests + DevDesk（POST_BUILD 资产），插桩）──"
BUILD_OUT=$(cmake --build "$BUILD_DIR" --target devdesk_tests DevDesk 2>&1) || {
  echo "$BUILD_OUT" | grep -iE "error|FAILED" | head -30
  echo "❌ 构建失败（上方为错误摘要）"; exit 1; }
echo "$BUILD_OUT" | tail -2
echo "✅ 构建成功"

# 跑全量单测（Failed 非 0 即中止，不拿红基线出覆盖率）。
echo "── 测试（采集 .profraw）──"
mkdir -p "$PROFRAW_DIR"
rm -f "$PROFRAW_DIR"/*.profraw   # 只留本次数据，防陈旧 profraw 混进 merge
# LLVM_PROFILE_FILE 显式指定落点（%p = PID）：绕开「default.profraw 写到哪了」整类坑
export LLVM_PROFILE_FILE="$PROFRAW_DIR/devdesk_tests-%p.profraw"
TEST_OUT=$("$TEST_BIN" 2>&1) || true
FAIL_LINES=$(echo "$TEST_OUT" | grep "\[FAIL\]" | head -20)
if [ -n "$FAIL_LINES" ]; then echo "$FAIL_LINES"; fi
COV_TEST_STATS=$(echo "$TEST_OUT" | grep -E "^(Total|Passed|Failed):" | tr '\n' ' ')
echo "$COV_TEST_STATS"
echo ""
if ! echo "$TEST_OUT" | grep -q "Failed: 0"; then
  echo "❌ 测试有失败——覆盖率基线必须建立在全绿之上，中止（完整输出跑: ${TEST_BIN}）" >&2
  exit 1
fi
echo "✅ 测试全绿"
PROFRAWS=("$PROFRAW_DIR"/*.profraw)
if [ ! -f "${PROFRAWS[0]}" ]; then
  echo "❌ 测试跑完了但没有 .profraw 落盘（$PROFRAW_DIR 为空）——插桩没生效或运行时没写 profile" >&2
  echo "   排查: ① configure 输出应有「覆盖率插桩: ON」 ② $BUILD_DIR/CMakeCache.txt 有 DEVDESK_COVERAGE:BOOL=ON" >&2
  exit 1
fi

echo "── llvm-profdata merge ──"
"$LLVM_PROFDATA" merge -sparse "${PROFRAWS[@]}" -o "$PROFDATA"
echo "✅ $PROFDATA"

echo "── 汇总 ──"
generate_report
echo "（分层表: $SUMMARY_TXT ；全表明细: $REPORT_TXT ；原始 JSON: ${EXPORT_JSON}）"
