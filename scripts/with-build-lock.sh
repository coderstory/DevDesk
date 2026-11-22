#!/bin/sh
# 并发构建串行闸（2026-11-18）：子代理并发开发时共享同一 build/ 目录，
# ninja/cmake 并发同目录会互相踩踏。所有构建类命令一律经本闸：
#   bash scripts/with-build-lock.sh bash scripts/build-test-run.sh --dev --filter X
# 语义：mkdir 原子抢锁 → 抢到执行 → EXIT 释放；等待超时 30 分钟或锁龄 >45 分钟视为陈旧夺锁。
# 不做嵌套重入（同进程链内只包最外层一次）。
set -u
LOCK="${TMPDIR:-/tmp}/devdesk-build.lock.d"
waited=0
while ! mkdir "$LOCK" 2>/dev/null; do
  # 陈旧锁：持锁进程崩溃未释放（>45 分钟）；find 无命中时退出码仍 0，须判输出非空
  if [ -n "$(find "$LOCK" -maxdepth 0 -mmin +45 2>/dev/null)" ]; then
    echo "[build-lock] 陈旧锁 >45min，夺取" >&2
    rm -rf "$LOCK" 2>/dev/null
    continue
  fi
  sleep 3
  waited=$((waited + 3))
  if [ "$waited" -ge 1800 ]; then
    echo "[build-lock] 等待超时 30min，放弃" >&2
    exit 3
  fi
  if [ $((waited % 30)) -eq 0 ]; then
    echo "[build-lock] 等待中 ${waited}s（他人持锁构建）" >&2
  fi
done
echo "[build-lock] acquired (pid $$)" >&2
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM HUP
"$@"
rc=$?
exit $rc