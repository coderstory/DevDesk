#!/usr/bin/env bash
# ccache 覆盖用环境变量（**平时不需要**）：配置已落 ccache 持久配置，手敲 cmake 带
# -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache 就吃到跨 worktree 缓存。
#
# 本文件只在要临时覆盖默认值时才 source：
#     source scripts/ccache-env.sh && bash scripts/build-test-run.sh
# 环境变量优先级高于配置文件，所以 source 之后以这里的值为准（未设才用下面的平台默认）。
#
# 配置本体按平台各一份，**位置不同**——写错地方等于没写（ccache -p 可看每个值实际来自哪一份）：
#   Windows: %LOCALAPPDATA%\ccache\ccache.conf（默认缓存目录下的那份）
#            + D:/software/ccache-devdesk/ccache.conf（CCACHE_DIR 被显式指定时读的是这一份）
#   macOS:   ~/Library/Preferences/ccache/ccache.conf —— **不是** XDG 的 ~/.config/ccache/ccache.conf，
#            实测写在 XDG 路径下 ccache 完全无视（--show-config 仍显示默认值），放这里才生效。
#
# 跨 worktree 命中的关键在这三项：
#   CCACHE_BASEDIR   把 <基准目录>/<worktree>/... 的绝对路径改写成「相对当前目录」再哈希。每个
#                    worktree 的绝对路径都不同，不改写则同一份源码哈希不同，永远 miss。
#   CCACHE_NOHASHDIR 命令哈希不含工作目录；-g 的调试信息里也含绝对路径，靠 BASEDIR 一起改写。
#   CCACHE_DIR       缓存放持久盘。不能放 build/ 里——worktree 用完即删，缓存会被一起删掉。

# 三个默认值按平台取：Windows 的数值是实测基线，不要改。
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    _ccache_dir_default="D:/software/ccache-devdesk"
    _ccache_basedir_default="D:/project/gui"
    _ccache_maxsize_default="20G"
    ;;
  Darwin)
    # 本机 $HOME=/Users/coderstory：展开即 /Users/coderstory/Library/Caches/ccache-devdesk 与
    # /Users/coderstory/dev/gui，与本机持久配置里的 cache_dir / base_dir 一致
    _ccache_dir_default="$HOME/Library/Caches/ccache-devdesk"
    _ccache_basedir_default="$HOME/dev/gui"
    _ccache_maxsize_default="5G"
    ;;
  *)
    # Linux：仓库位置不固定，取平台中立的 $HOME 方案（XDG 缓存目录 + 与 macOS 同构的布局）；
    # 与本机实际布局不符时显式覆盖 CCACHE_DIR / CCACHE_BASEDIR
    _ccache_dir_default="${XDG_CACHE_HOME:-$HOME/.cache}/ccache-devdesk"
    _ccache_basedir_default="$HOME/dev/gui"
    _ccache_maxsize_default="5G"
    ;;
esac

# 三条 ${VAR:-默认} = 外部值非空才覆盖，设成空串也回落默认。不要简化成 ${VAR-默认}
# ——空串会被当有效值一路传下去。
export CCACHE_DIR="${CCACHE_DIR:-$_ccache_dir_default}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-$_ccache_maxsize_default}"
# BASEDIR 必须是 DevDesk 与 EUI-NEO 的公共父目录（Windows 本机 = D:/project/gui，macOS 本机 =
# /Users/coderstory/dev/gui）；Windows 上还要用 Windows 风格路径——ccache 是原生 exe，不认 /d/... 这种 MSYS 路径。
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$_ccache_basedir_default}"
export CCACHE_NOHASHDIR="${CCACHE_NOHASHDIR:-true}"

unset _ccache_dir_default _ccache_basedir_default _ccache_maxsize_default
