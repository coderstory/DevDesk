#!/usr/bin/env bash
# discipline-reminders.sh —— 开发纪律提醒（非阻塞，三模式合一）
# 用法: discipline-reminders.sh merge-tree|red-green|plan-first
# 行为：输出 additionalContext JSON 注入模型上下文；恒退出 0——提醒不阻塞，
#       强制判定交给独立审计层，hook 只负责在动作发生的当下把纪律递到眼前。
# 文案约束：单行、不含双引号与反斜杠（printf 手拼 JSON，不依赖 jq，理由同 strategy-guard.sh）。
set -u
mode="${1:-}"
case "$mode" in
  merge-tree)
    event="PostToolUse"
    ctx="【合并树门禁】分支绿 ≠ 合并绿：merge/pull/rebase 之后必须在合并结果树上跑一次完整门禁（构建+定向测试），不能只信两边分支各自绿。实证：f274f74 审计在 worktree 跑闸、合并进 dev 后从未构建，dev 当场无法 configure。"
    ;;
  red-green)
    event="PreToolUse"
    ctx="【先红后绿】本次 commit 若含行为变更，须先有测试先红后绿或变异验证证据；纯文档/配置/注释改动可豁免但 commit message 写明。恒真断言=假绿；答不出改动回退时哪条用例会红，就不该提交。"
    ;;
  plan-first)
    event="PreToolUse"
    ctx="【plan 先行】即将改 src/ 源码：非平凡改动须先有 spec/plan 依据（docs/superpowers/specs|plans/），实现前先读；trivial 一行修复可豁免但须自证。无依据动手 = 无审计锚点，六阶段流水线从阶段 1 起不了步。"
    ;;
  *)
    exit 0
    ;;
esac
printf '{"hookSpecificOutput":{"hookEventName":"%s","additionalContext":"%s"}}\n' "$event" "$ctx"
exit 0
