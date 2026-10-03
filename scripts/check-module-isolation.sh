#!/usr/bin/env bash
# scripts/check-module-isolation.sh
# 模块隔离静态守卫（波 1 TIN-01）——settings 侧「不依赖兄弟模块」的契约检查。
#
# 背景：这条守卫原先编在 tests/test_modules/test_settings.cpp 里
#（REGISTER_TEST settings_module_isolation_guard），靠 __FILE__ 上溯找仓库根。
# ccache 把 __FILE__ 改写成相对路径后，cwd = 仓库根 时该候选退化成裸相对路径
# 并误命中 → 返回空 root 报「找不到仓库根」→ 全量必红（机制见波 1 spec §L10）。
# 脚本版不依赖 __FILE__、不依赖 $PWD（全程绝对路径、脚本内不 cd）。
#
# 断言（与删除前的 C++ 守卫逐条等价）：
#   A. src/module_settings/settings_module.cpp
#      1) 不得出现 module_pet/、PetConfigStore、petConfigFile、namespace module::pet
#      2) 必含 #include "module_pet_bridge.h" 恰好 1 次（桥声明唯一来源）
#      3) src/module_pet_bridge.h 导出的**每一个**桥函数
#         （名单自动抽取，见下 A3）都必须在 settings_module.cpp 有 ≥ 1 次真调用
#         （`::module::pet::X` 调用形）——解耦不是删功能：桥函数都要真被调用
#      4) 头里两道抽取口径（声明式 / static_assert 哨兵式）集合必须一致，
#         否则报「抽取不可靠」——防止某道口径单独漂移又互相掩盖
#   B. src/module_settings/CMakeLists.txt 的非注释行
#      5) 不得出现 devdesk_module_pet / devdesk_module_claude_proxy
#        （解释性注释里允许提及，故按行过滤首非空白字符为 '#' 的行）
#   C. module_agent 隔离（agent M1 Task 11）：agent 消费 todo/markdown 工具
#      走 core::ToolRegistry 按名调用（todo_tools.h 头注的设计契约），所以
#      5) src/module_agent/*.{h,cpp} 不得 include 兄弟模块头
#         （module_home|markdown|todo|claude_proxy|settings|pet)/ 均零命中；
#         按「include 路径形态」匹配——注释里不带斜杠的兄弟模块名不算违规
#      6) 反向：todo/markdown/claude/**settings** 的工具集与模块文件（todo_tools /
#         markdown_tools / claude_tools / app_tools / todo_module /
#         markdown_module / claude_module / settings_module）不得引用
#         module_agent/（工具注册方不回依赖消费方；
#         各 tools 头注释里「不依赖 module_agent」不带斜杠，不误报）
#         settings 族为 ARCH-02/ARCH-04 补入（M2 静态审计）：app.read_settings
#         是 M2 新增的第 3 个工具落点，名单此前只补了 claude ——第三处落点
#         从此不再只靠人记
#   D. module_home 隔离（2026-10-03 静态审计 B1）：首页余量卡经
#      src/quota_home_bridge.h 这个中立头读 claude 侧数据。**链接层面**
#      devdesk_module_home 依赖 devdesk_module_claude_proxy（nm -u 实测两个
#      未定义符号，见 module_home/CMakeLists.txt 的显式声明），而静态库链接
#      **顺序敏感**。本段把三件「注释里说过」的事变成会红的断言：
#      7) src/module_home/*.{h,cpp} 不得 include 兄弟模块头
#         （按 include 路径形态 = 带斜杠后缀；注释里的裸模块名不误报）
#      8) src/module_home/*.{h,cpp} 不得定义/打开 `namespace module::claude`
#         （它只能**消费**中立头里的类型与自由函数，不能往那个命名空间里加东西）
#      9) **链接顺序**：顶层 CMakeLists.txt 的链接表里 devdesk_module_home
#         必须排在 devdesk_module_claude_proxy 之前 —— 顺序颠倒 = 两个桥
#         符号 undefined，且**不会有任何注释提醒你**（注释都在被它打破的文件里）
#
# 退出码：0 = 通过；1 = 违规（逐条打印 文件:行:命中行）；2 = 环境错（不得静默绿）
#
# 用法：bash scripts/check-module-isolation.sh [--root DIR] [--self-test]
#   --root DIR  指定被检查的仓库根（默认自动定位：本脚本所属 git 仓库的根，
#               worktree 里即该 worktree 根），反证测试用它指向仓库外拷贝树。
#   --self-test 造坏输入证明本守卫不是恒绿，并**用真实仓库树做底**（见下）。
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-module-isolation.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# SELF 是**脚本所在目录**（沿用既有约定：git -C "$SELF" / dirname "$SELF" 都按目录用）。
# --self-test 要拿它去**执行**本脚本，所以另存一份脚本自身的绝对路径，别把目录当命令跑。
SELF_FILE="$SELF/$(basename "${BASH_SOURCE[0]}")"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root)
            [ $# -ge 2 ] || { usage; exit 2; }
            ROOT="$2"
            shift 2
            ;;
        --self-test)
            SELF_TEST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            usage
            exit 2
            ;;
    esac
done

# ── --self-test：自证非恒真（2026-10-03 门禁加固补上）────────────────────
# 为什么必须有：上一批给 D 段（module_home 侧）加了三条断言，但**这三条一次都
# 没被机器跑过**——只有人手点。本守卫又是门禁链里跑得最频繁的一道，于是
# 「判据哪天退化成恒真」这件事没有任何东西会发现。本块就是那个东西。
#
# 形态：**拿真实仓库树当底 + 定点变异**。不手搓最小假树——手搓的假树会与真实
# 代码形状漂移（守卫将来加一条文件级断言，假树缺文件 → 好树假红；恒红守卫比没有
# 守卫更糟），而且「假树上的判据」推不出「这份代码上的判据」。
#
# 每条坏树都先 `diff -r` 断言**树真的变了**，再断言它红了。顺序反了就白忙：
# 「跑完是绿的，但什么都没改」是本项目反复栽的坑（本会话实施者与控制器各踩三次以上）。
# 另外坏树要求的退出码是**恰好 1**（违规），不是「任何非零」——2 是环境错，
# 把它当成红会把「守卫自己跑不起来」伪装成「守卫抓到了违规」。
if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT

    [ -d "$ROOT/src" ] || { echo "❌ 自检失败：$ROOT/src 不存在" >&2; exit 2; }
    [ -f "$ROOT/CMakeLists.txt" ] || { echo "❌ 自检失败：$ROOT/CMakeLists.txt 不存在" >&2; exit 2; }

    mkdir -p "$TMP/good"
    cp -R "$ROOT/src" "$TMP/good/src"
    cp "$ROOT/CMakeLists.txt" "$TMP/good/CMakeLists.txt"

    new_tree() { rm -rf "$TMP/$1"; cp -R "$TMP/good" "$TMP/$1"; }

    # 跨平台原地改文件。**不要用 `sed -i ''`**：那是 BSD 专有写法，GNU sed
    # （Windows CI 的 MSYS2 就是它）会把 '' 当成脚本、把真正的表达式当成文件名，
    # 结果是**什么都没改**还报一行错——自测据此会得出「假绿守卫」的错误结论。
    # 本守卫要进 Windows CI 的门禁链，构造动作必须两平台同形。
    sed_i() {
        local f="$1" t; shift
        t="$(mktemp)"
        sed "$@" "$f" > "$t" && cat "$t" > "$f"
        rm -f "$t"
    }

    run_tree() { bash "$SELF_FILE" --root "$TMP/$1" >/dev/null 2>&1; }

    assert_red() {
        local rc
        run_tree "$1"; rc=$?
        if diff -r "$TMP/good" "$TMP/$1" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$1]：与好树逐字节相同 —— 变异没生效，这条自测是假证据（$2）"
            fail=1; return
        fi
        if [ "$rc" -eq 1 ]; then
            echo "  ✅ 坏树 [$1]：红 —— $2"
        else
            echo "  ❌ 坏树 [$1]：退出码 $rc（期望恰好 1 = 违规）—— $2"
            fail=1
        fi
    }
    assert_green() {
        local rc
        run_tree "$1"; rc=$?
        if diff -r "$TMP/good" "$TMP/$1" >/dev/null 2>&1; then
            echo "  ❌ 树 [$1]：与好树逐字节相同 —— 这条证据不成立（什么都没改）"
            fail=1; return
        fi
        if [ "$rc" -eq 0 ]; then
            echo "  ✅ 树 [$1]：绿 —— $2"
        else
            echo "  ❌ 树 [$1]：退出码 $rc（期望 0）—— $2"
            fail=1
        fi
    }

    fail=0

    # 0) 好树基线：真实仓库树必须绿。它不绿，后面所有「坏树红了」都没有意义。
    if run_tree good; then
        echo "  ✅ 好树（真实仓库树逐字节拷贝）：绿"
    else
        echo "  ❌ 好树（真实仓库树）就红了 —— 本守卫对当前代码不成立，先修守卫"
        bash "$SELF_FILE" --root "$TMP/good" 2>&1 | head -12
        fail=1
    fi

    # 1) D7：module_home 里 include 兄弟模块头（上一轮审计员点的那个洞）
    new_tree home_include_sibling
    for f in "$TMP/home_include_sibling"/src/module_home/*.cpp \
             "$TMP/home_include_sibling"/src/module_home/*.h; do
        [ -f "$f" ] || continue
        printf '%s\n' '#include "module_claude_proxy/quota/quota_store.h"' >> "$f"
    done
    assert_red home_include_sibling "module_home include 兄弟模块头（D7）"

    # 2) D8：module_home 里打开 module::claude 命名空间
    new_tree home_namespace_claude
    for f in "$TMP/home_namespace_claude"/src/module_home/*.cpp \
             "$TMP/home_namespace_claude"/src/module_home/*.h; do
        [ -f "$f" ] || continue
        printf '%s\n' 'namespace module::claude { struct SelfTestProbe {}; }' >> "$f"
    done
    assert_red home_namespace_claude "module_home 往 module::claude 里加东西（D8）"

    # 3) D9：顶层链接表把 claude_proxy 排到 home 之前 —— 上一轮审计员说的那个炸法
    new_tree link_order
    awk '
        { L[NR] = $0 }
        END {
            h = 0; p = 0
            for (i = 1; i <= NR; i++) {
                if (!h && L[i] ~ /devdesk_module_home[ \t]*$/) { h = i; hl = L[i] }
                if (!p && L[i] ~ /devdesk_module_claude_proxy[ \t]*$/) { p = i; pl = L[i] }
            }
            if (!h || !p) exit 3
            for (i = 1; i <= NR; i++) {
                if (i == h) print pl
                else if (i == p) print hl
                else print L[i]
            }
        }
    ' "$TMP/link_order/CMakeLists.txt" > "$TMP/cm.tmp" 2>/dev/null \
        && cat "$TMP/cm.tmp" > "$TMP/link_order/CMakeLists.txt"
    assert_red link_order "顶层链接顺序颠倒 home < claude_proxy（D9）"

    # 4) D9 的另一半：链接表里根本没有 home 这一项 → 顺序不变式无法断言
    new_tree link_missing_home
    sed_i "$TMP/link_missing_home/CMakeLists.txt" '/devdesk_module_home[ \t]*$/d'
    assert_red link_missing_home "链接表里缺 devdesk_module_home，守卫看不见自己要守的东西（D9 抽取失配）"

    # 5) A1：settings 侧反向依赖 pet
    new_tree settings_pet_include
    printf '%s\n' '#include "module_pet/pet_window.h"' >> "$TMP/settings_pet_include/src/module_settings/settings_module.cpp"
    assert_red settings_pet_include "settings_module.cpp include module_pet/（A1）"

    # 6) A2：桥声明的 include 不再唯一
    new_tree bridge_include_dup
    printf '%s\n' '#include "module_pet_bridge.h"' >> "$TMP/bridge_include_dup/src/module_settings/settings_module.cpp"
    assert_red bridge_include_dup "桥头被 include 两次，声明不再唯一（A2 要求恰好 1 次）"

    # 7) A3：桥函数被「解耦」掉了——头里还导出着，调用点却没了
    new_tree bridge_call_missing
    sed_i "$TMP/bridge_call_missing/src/module_settings/settings_module.cpp" '/::module::pet::/d'
    assert_red bridge_call_missing "桥函数无真调用点（解耦退化成删功能，A3）"

    # 8) B：settings 自己的 CMakeLists 又 link 了兄弟模块库
    new_tree settings_cmake_link
    printf '%s\n' 'target_link_libraries(devdesk_settings PRIVATE devdesk_module_pet)' \
        >> "$TMP/settings_cmake_link/src/module_settings/CMakeLists.txt"
    assert_red settings_cmake_link "settings CMakeLists 非注释行 link 兄弟模块库（B）"

    # 9) C5：module_agent include 兄弟模块头
    new_tree agent_include_sibling
    for f in "$TMP/agent_include_sibling"/src/module_agent/*.cpp \
             "$TMP/agent_include_sibling"/src/module_agent/*.h; do
        [ -f "$f" ] || continue
        printf '%s\n' '#include "module_todo/todo_tools.h"' >> "$f"
    done
    assert_red agent_include_sibling "module_agent include 兄弟模块头（C5）"

    # 10) C6：工具方回头依赖 module_agent（注册方不回依赖消费方）
    new_tree tool_ref_agent
    printf '%s\n' '#include "module_agent/agent_tools.h"' >> "$TMP/tool_ref_agent/src/module_todo/todo_tools.h"
    assert_red tool_ref_agent "todo_tools 回依赖 module_agent（C6）"

    # 11) 反向证据：一棵「写法不同但合规」的树必须绿。
    #     没有这条，守卫可能退化成刷屏器——那种守卫迟早被人关掉，比没有守卫更糟。
    #     这里刻意做三件「像整理、但合规」的事：① module_home 里加注释，注释里
    #     **故意提到兄弟模块裸名**（判据按 include 路径形态 = 带斜杠后缀，
    #     裸名不误报）；② 顶层链接表里把**与本不变式无关**的两个库换序；
    #     ③ 改写 settings_module.cpp 的 include 顺序。
    new_tree benign_style
    for f in "$TMP/benign_style"/src/module_home/*.cpp; do
        [ -f "$f" ] || continue
        {
            printf '\n'
            printf '%s\n' '// 读法说明：本卡经 src/quota_home_bridge.h 这个中立头读 claude 侧数据。'
            printf '%s\n' '// 注释里提到 module_claude_proxy / module_pet 这类裸名不算违规——'
            printf '%s\n' '// 判据按 include 路径形态（带斜杠后缀）匹配，不是按子串。'
        } >> "$f"
    done
    printf '%s\n' '// 说明：本模块不依赖 module_pet，所有桥调用都经 module_pet_bridge.h。' \
        >> "$TMP/benign_style/src/module_settings/settings_module.cpp"
    # 与 home/claude_proxy 相对顺序无关的换序：markdown ↔ todo
    awk '
        { L[NR] = $0 }
        END {
            h = 0; p = 0
            for (i = 1; i <= NR; i++) {
                if (!h && L[i] ~ /devdesk_module_markdown[ \t]*$/) { h = i; hl = L[i] }
                if (!p && L[i] ~ /devdesk_module_todo[ \t]*$/) { p = i; pl = L[i] }
            }
            if (!h || !p) exit 3
            for (i = 1; i <= NR; i++) {
                if (i == h) print pl
                else if (i == p) print hl
                else print L[i]
            }
        }
    ' "$TMP/benign_style/CMakeLists.txt" > "$TMP/cm.tmp" 2>/dev/null \
        && cat "$TMP/cm.tmp" > "$TMP/benign_style/CMakeLists.txt"
    assert_green benign_style "写法不同但合规：注释含裸模块名 + 无关库换序 + 追加说明"

    if [ "$fail" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
        exit 1
    fi
    echo "✅ --self-test 通过：好树绿 / 10 棵坏树各红 / 反向树绿 —— 非恒真"
    exit 0
fi

SRC="$ROOT/src/module_settings/settings_module.cpp"
CMK="$ROOT/src/module_settings/CMakeLists.txt"
BRIDGE="$ROOT/src/module_pet_bridge.h"

# ---- 环境检查：缺东西一律 exit 2，绝不静默绿 ----
[ -d "$ROOT" ] || { echo "环境错误：仓库根不存在: $ROOT" >&2; exit 2; }
[ -f "$SRC" ] && [ -r "$SRC" ] || { echo "环境错误：不可读 $SRC" >&2; exit 2; }
[ -f "$CMK" ] && [ -r "$CMK" ] || { echo "环境错误：不可读 $CMK" >&2; exit 2; }
[ -f "$BRIDGE" ] && [ -r "$BRIDGE" ] || { echo "环境错误：不可读 $BRIDGE" >&2; exit 2; }

VIOLATIONS=0

# 绝对路径 → 仓库相对路径（打印用）；不在 ROOT 下则原样输出
rel() {
    case "$1" in
        "$ROOT"/*) printf '%s' "${1#"$ROOT"/}" ;;
        *)         printf '%s' "$1" ;;
    esac
}

report() {
    printf '%s\n' "$1"
    VIOLATIONS=$((VIOLATIONS + 1))
}

# 命中即违规：grep -nF 输出形如「行号:命中行」，拼成 文件:行:命中行
check_absent_fixed() {
    local file="$1" pat="$2" hits line
    hits="$(grep -nF -- "$pat" "$file" || true)"
    [ -n "$hits" ] || return 0
    while IFS= read -r line; do
        report "$(rel "$file"):$line"
    done <<< "$hits"
}

# 期望恰好一次
check_exactly_once_fixed() {
    local file="$1" pat="$2" n
    n="$(grep -oF -- "$pat" "$file" | wc -l | tr -d ' ')"
    if [ "$n" != "1" ]; then
        report "$(rel "$file"):0:缺少/重复期望内容 '$pat'（要求恰好 1 次，实测 ${n} 次）"
    fi
}

# 期望至少一次
check_at_least_once_fixed() {
    local file="$1" pat="$2"
    if ! grep -qF -- "$pat" "$file"; then
        report "$(rel "$file"):0:缺少期望调用 '$pat'（要求 ≥ 1 次，实测 0 次）"
    fi
}

# ---- A1. settings_module.cpp：pet 头零 include、盘 store 零触碰、无内联声明块 ----
check_absent_fixed "$SRC" 'module_pet/'
check_absent_fixed "$SRC" 'PetConfigStore'
check_absent_fixed "$SRC" 'petConfigFile'
check_absent_fixed "$SRC" 'namespace module::pet'

# ---- A2. 正向：桥声明唯一来源 = src 根中立头，恰好 include 一次 ----
check_exactly_once_fixed "$SRC" '#include "module_pet_bridge.h"'

# ---- A3. 正向：桥函数都真在调用点被用到（`::module::pet::X` 调用形）----
# 桥名单**不再手工维护**：从 src/module_pet_bridge.h 自动抽取，两道口径交叉校验：
#   ① 声明式——namespace module::pet 块内行尾为 `);` 的函数声明，取 `(` 前标识符
#   ② 哨兵式——static_assert 段里 &module::pet::NAME 的 NAME
# 两口径必须给出同一集合：只在一侧出现的名字判违规（抽取口径漂移/声明被改成跨行
# 都归此类，宁红勿静默绿）。随后每个名字都必须在 settings_module.cpp 有 ≥ 1 次
# 真调用（解耦不是删功能：closePetWindow / showPetWindow 等同样要求真被调用）。
# 为什么要自动抽取（merge-review R2）：旧版是一份手维护的固定清单，与头文件的
# 导出双份并存——以后新增桥函数而忘改本脚本，守卫会**静默漏检**（不报错、不漏
# 到人眼前），等于守卫失准。2026-09-30 起名单只有一个来源 = 头文件本身。

# ① 声明式抽取：输出「行号:名字」
BRIDGE_DECL="$(awk '
    /^namespace module::pet[[:space:]]*\{/ { inns = 1; next }
    inns && /^}[[:space:]]*\/\// { inns = 0; next }
    inns {
        line = $0
        sub(/^[ \t]+/, "", line)
        if (line == "" || line ~ /^\/\//) next
        if (line !~ /\);[ \t]*$/) next          # 只认函数声明行尾
        pre = line
        sub(/\(.*$/, "", pre)                    # 砍掉参数表 → 剩「返回类型 + 名字」
        cnt = split(pre, part, /[^A-Za-z0-9_]/)
        name = part[cnt]
        if (name != "") printf "%d:%s\n", FNR, name
    }
' "$BRIDGE" || true)"

# ② 哨兵式抽取：static_assert 段的 &module::pet::NAME
BRIDGE_ASSERT="$(grep -nEo '&module::pet::[A-Za-z_][A-Za-z0-9_]*' "$BRIDGE" \
    | sed -E 's#^([0-9]+):&module::pet::#\1:#' || true)"

DECL_COUNT="$(printf '%s' "$BRIDGE_DECL" | grep -c ':' || true)"
ASSERT_COUNT="$(printf '%s' "$BRIDGE_ASSERT" | grep -c ':' || true)"

# 抽取彻底失效（头被清空/改名/解析规则整体失配）→ 环境错，绝不静默绿
if [ "${DECL_COUNT:-0}" -eq 0 ] || [ "${ASSERT_COUNT:-0}" -eq 0 ]; then
    echo "环境错误：无法从 $BRIDGE 抽取桥函数名单（声明式 ${DECL_COUNT:-0} 项 /" \
         "哨兵式 ${ASSERT_COUNT:-0} 项）——抽取规则已与头文件失配，不放行" >&2
    exit 2
fi

# 交叉校验：只在一侧出现的名字 = 抽取口径漂移 → 违规（红）
DECL_NAMES="$(printf '%s' "$BRIDGE_DECL" | cut -d: -f2 | sort -u)"
ASSERT_NAMES="$(printf '%s' "$BRIDGE_ASSERT" | cut -d: -f2 | sort -u)"
DRIFT="$(printf '%s\n' "$DECL_NAMES" "$ASSERT_NAMES" | sort | uniq -c \
    | awk '$1 == 1 { print $2 }')"
if [ -n "$DRIFT" ]; then
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        ln="$(printf '%s\n' "$BRIDGE_DECL" | awk -F: -v n="$name" '$2 == n { print $1; exit }')"
        [ -n "$ln" ] || ln="$(printf '%s\n' "$BRIDGE_ASSERT" | awk -F: -v n="$name" '$2 == n { print $1; exit }')"
        report "$(rel "$BRIDGE"):${ln:-0}:${name}（桥名单抽取口径漂移：声明式与 " \
                 "static_assert 哨兵式集合不一致，该桥无法被可靠断言）"
    done <<< "$DRIFT"
fi

# 逐桥真调用点断言（名单来自头文件，非手维护）。
# 未被调用时打印 头文件:声明行:桥名 —— 让人一眼看到「哪个新桥被漏检」。
while IFS= read -r name; do
    [ -n "$name" ] || continue
    if ! grep -qF -- "::module::pet::${name}" "$SRC"; then
        ln="$(printf '%s\n' "$BRIDGE_DECL" | awk -F: -v n="$name" '$2 == n { print $1; exit }')"
        report "$(rel "$BRIDGE"):${ln:-0}:${name}（头文件导出但 settings_module.cpp " \
                 "无 '::module::pet::${name}' 调用点：解耦不是删功能，桥必须真被调用）"
    fi
done <<< "$DECL_NAMES"

# ---- B. settings CMakeLists.txt：非注释行不得再 link 兄弟模块库 ----
CMK_HITS="$(awk -v f="$(rel "$CMK")" '
    /^[ \t]*#/ { next }                       # 注释行允许提及目标名
    /devdesk_module_pet|devdesk_module_claude_proxy/ {
        printf "%s:%d:%s\n", f, FNR, $0
    }
' "$CMK" || true)"
if [ -n "$CMK_HITS" ]; then
    while IFS= read -r line; do
        report "$line"
    done <<< "$CMK_HITS"
fi

# ---- C. module_agent 隔离（agent M1）：兄弟模块头零 include + 工具方不回依赖 ----
AGENT_DIR="$ROOT/src/module_agent"
[ -d "$AGENT_DIR" ] || { echo "环境错误：目录不存在: $AGENT_DIR" >&2; exit 2; }
# 5) agent 源文件不得 include 兄弟模块头（include 路径形态 = 带斜杠后缀）
for agent_file in "$AGENT_DIR"/*.h "$AGENT_DIR"/*.cpp; do
    [ -f "$agent_file" ] || { echo "环境错误：module_agent 源文件缺失: $agent_file" >&2; exit 2; }
    for sibling in module_home module_markdown module_todo \
                   module_claude_proxy module_settings module_pet; do
        check_absent_fixed "$agent_file" "${sibling}/"
    done
done
# 6) 反向：工具集/模块注册文件不得引用 module_agent 头（同款带斜杠形态；
#    tools 头注释「不依赖 module_agent」不带斜杠，不误报）
for tool_file in "$ROOT/src/module_todo/todo_tools.h" \
                 "$ROOT/src/module_todo/todo_tools.cpp" \
                 "$ROOT/src/module_todo/todo_module.cpp" \
                 "$ROOT/src/module_markdown/markdown_tools.h" \
                 "$ROOT/src/module_markdown/markdown_tools.cpp" \
                 "$ROOT/src/module_markdown/markdown_module.cpp" \
                 "$ROOT/src/module_claude_proxy/claude_tools.h" \
                 "$ROOT/src/module_claude_proxy/claude_tools.cpp" \
                 "$ROOT/src/module_claude_proxy/claude_module.cpp" \
                 "$ROOT/src/module_settings/app_tools.h" \
                 "$ROOT/src/module_settings/app_tools.cpp" \
                 "$ROOT/src/module_settings/settings_module.cpp"; do
    [ -f "$tool_file" ] && [ -r "$tool_file" ] || {
        echo "环境错误：不可读 $tool_file" >&2; exit 2; }
    check_absent_fixed "$tool_file" 'module_agent/'
done

# ---- D. module_home 隔离（静态审计 B1）----
# 为什么必须有：首页余量卡经 src/quota_home_bridge.h 中立头读 claude 侧数据，
#   链接层面 devdesk_module_home 确实有两个未定义符号落在
#   devdesk_module_claude_proxy 里（nm -u 实测）。姿态与 pet 桥/settings 同款：
#   include 层中立、链接层由最终目标兜住。**代价是这个库不自洽**，于是：
#     ① 谁把兄弟模块头 include 进 module_home/ → 依赖形状悄悄变了，无人报警
#     ② 谁把链接表整理成字母序 → 两个符号 undefined
#   两条都是「只有出事才看得见」的类型，所以写成断言。
HOME_DIR="$ROOT/src/module_home"
[ -d "$HOME_DIR" ] || { echo "环境错误：目录不存在: $HOME_DIR" >&2; exit 2; }
home_files=0
for home_file in "$HOME_DIR"/*.h "$HOME_DIR"/*.cpp; do
    [ -f "$home_file" ] || { echo "环境错误：module_home 源文件缺失: $home_file" >&2; exit 2; }
    home_files=$((home_files + 1))
    # D7：兄弟模块头零 include（带斜杠的 include 路径形态；注释里的裸名不误报）
    for sibling in module_markdown module_todo module_claude_proxy \
                   module_settings module_pet module_agent; do
        check_absent_fixed "$home_file" "${sibling}/"
    done
    # D8：不得打开/定义 module::claude 命名空间。home 只能**消费**中立头里的
    # 类型与自由函数（`module::claude::quota::X` 调用形是允许的，那是消费）。
    check_absent_fixed "$home_file" 'namespace module::claude'
done
[ "$home_files" -gt 0 ] || { echo "环境错误：module_home 下零个源文件" >&2; exit 2; }

# D9：链接顺序。静态库链接顺序敏感 —— devdesk_module_home 排在
# devdesk_module_claude_proxy **之后**时，quota_home_bridge 的两个桥符号
#（listQuotaHomeRows / requestQuotaImmediateRefresh）undefined，构建期红。
# 这一条以前只有 module_home/CMakeLists.txt 里的一段注释，而**注释挡不住
# 有人把顶层链接表整理成字母序**（home < claude_proxy 本来就不是字母序）。
TOP_CMAKE="$ROOT/CMakeLists.txt"
[ -f "$TOP_CMAKE" ] && [ -r "$TOP_CMAKE" ] \
    || { echo "环境错误：不可读 $TOP_CMAKE" >&2; exit 2; }
read -r _home_line _proxy_line <<< "$(awk '
    /devdesk_module_home[ \t]*$/ && !ln_h { ln_h = FNR }
    /devdesk_module_claude_proxy[ \t]*$/ && !ln_p { ln_p = FNR }
    END { printf "%s %s", (ln_h ? ln_h : 0), (ln_p ? ln_p : 0) }
' "$TOP_CMAKE")"
if [ "$_home_line" -eq 0 ] || [ "$_proxy_line" -eq 0 ]; then
    # 两个库任一不在顶层链接表里 → 抽取失配。宁红勿静默绿：这正是「守卫
    # 看不见自己要守的东西」的那类假绿。
    report "$(rel "$TOP_CMAKE"):0:链接表里找不到 devdesk_module_home($_home_line)/devdesk_module_claude_proxy($_proxy_line) —— 链接顺序不变式无法断言（守卫失准）"
elif [ "$_home_line" -ge "$_proxy_line" ]; then
    report "$(rel "$TOP_CMAKE"):${_proxy_line}:devdesk_module_home（第 ${_home_line} 行）必须排在 devdesk_module_claude_proxy（第 ${_proxy_line} 行）**之前** —— 静态库链接顺序敏感，颠倒即 listQuotaHomeRows / requestQuotaImmediateRefresh undefined"
fi

# ---- 结论 ----
if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ 模块隔离守卫失败：$VIOLATIONS 处违规（上方 文件:行:命中行）" >&2
    exit 1
fi

echo "✅ 模块隔离守卫通过（settings_module.cpp 无 pet 依赖 / 头文件导出的 ${DECL_COUNT} 个桥调用齐全；CMakeLists 未 link 兄弟模块；module_agent 零兄弟 include 且工具方不回依赖；module_home ${home_files} 个源文件零兄弟 include、零 namespace module::claude 定义、链接顺序 home<claude_proxy）"
exit 0
