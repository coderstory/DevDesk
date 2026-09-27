#!/usr/bin/env bash
# ccache 覆盖用环境变量（**平时不需要**）。
#
# 配置已落 ccache 持久配置，手敲 cmake 只要带两个 launcher 参数就吃到跨 worktree 缓存：
#     cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#       -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ...
#   （配置本体：%LOCALAPPDATA%\ccache\ccache.conf + D:/software/ccache-devdesk/ccache.conf；
#     ccache -p 可看每个值的来源，本机实测 = (…\ccache.conf) base_dir = D:\project\gui / hash_dir = false）
#
# 本文件只在要**临时覆盖**默认值时才 source：换缓存盘、换基准目录、临时关缓存等。
#     source scripts/ccache-env.sh && bash scripts/build-test-run.sh
# 环境变量优先级高于配置文件，所以 source 之后以这里的值为准（未设才用默认）。
#
# 为什么基准目录/工作目录这两项是跨 worktree 命中的关键：
#   CCACHE_BASEDIR   把 D:/project/gui/<worktree>/... 的绝对路径改写成「相对当前目录」再哈希。
#                    每个 worktree 绝对路径都不同，不改写 → 同一份源码哈希不同 → 永远 miss。
#   CCACHE_NOHASHDIR 命令哈希不含工作目录；-g 的调试信息里也含绝对路径，靠 BASEDIR 一起改写。
#   CCACHE_DIR       缓存放持久盘。不能放 build/ 里——worktree 用完即删，缓存会被一起删掉。
export CCACHE_DIR="${CCACHE_DIR:-D:/software/ccache-devdesk}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}"
# BASEDIR 必须是「worktree / DevDesk」与「EUI-NEO」的公共父目录（本机 = D:/project/gui），
# 且推荐 Windows 风格路径（ccache 是原生 exe，不认 /d/... 这种 MSYS 路径）。
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-D:/project/gui}"
export CCACHE_NOHASHDIR="${CCACHE_NOHASHDIR:-true}"
