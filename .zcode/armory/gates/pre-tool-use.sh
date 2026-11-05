#!/bin/sh
# ZCode PreToolUse 硬门禁（配置见 .zcode/config.json，规则见 AGENTS.md §十）
# 用法: pre-tool-use.sh <bash-gate|path-gate>
# 语义: 读 stdin 工具调用 JSON 原文做模式匹配；命中禁止模式 → stderr 输出原因并退出 2（阻断）；否则退出 0
# 设计: 不解析 JSON（工具类型已由 config.json 的 matcher 保证），POSIX sh，macOS/MSYS2 双平台可用
input=$(cat)
mode="${1:-}"

block() {
  echo "[门禁] $1" >&2
  exit 2
}

case "$mode" in
  bash-gate)
    # 走正规构建脚本的一律放行（含查看/引用该脚本内容的场景）
    case "$input" in
      *build-test-run.sh*) exit 0 ;;
    esac
    case "$input" in
      *cmake\ -B*|*cmake\ --build*)
        block "禁止手敲 cmake（cmake -B / cmake --build）。一律: bash scripts/build-test-run.sh（AGENTS.md §五）" ;;
    esac
    ;;
  path-gate)
    # 产物目录（2026-11-06 池134 补）：AGENTS.md §十一 声明「写产物目录 ❌」，
    # 原实现只拦归档目录、漏了构建目录——Edit/Write 直写 build/ 实测放行。
    # 不误伤构建：本 gate 只挂 Edit|Write matcher，Bash 走 bash-gate，
    # 构建脚本经 Bash 写 build/ 不受此约束。
    case "$input" in
      */build/*|*/build-release/*)
        block "构建产物目录不可直接写入（AGENTS.md §十一）。构建一律: bash scripts/build-test-run.sh" ;;
    esac
    case "$input" in
      */analysis/*|*/pr-bodies/*|*/splash-delivery/*|*/devdesk-agent-m1-sdd-archive/*|*/eui-neo-zpatch-copy/*|*eui-neo-dropdown-popup-z.patch*|*pr-push-eui-neo.sh*)
        block "产物/归档目录与文件，修改前先征得用户同意（AGENTS.md §七；2026-10-29 起原件已备份至仓库外，勿在仓库内重建）" ;;
    esac
    ;;
esac
exit 0
