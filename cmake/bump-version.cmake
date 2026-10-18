# cmake/bump-version.cmake
# 构建版本号自增脚本：VERSION_FILE（x.y.z，每段 0-9）每次调用 +1：
#   z 满 10 进 y，y 满 10 进 x，x 满 10 饱和 9.9.9（不退回 9.0.0）。
#   文件不存在时用 VERSION_SEED 播种（写初值、不自增），下个构建起 +1。
#   独立可测：不依赖任何外部变量，只读写 VERSION_FILE 指定的文件。
# 调用：
#   cmake -D VERSION_FILE=<path> -D VERSION_SEED=<x.y.z> -P cmake/bump-version.cmake
# 被顶层 add_custom_target(bump_version ALL) 于每次 cmake --build 时执行，
# 计数文件是 build 目录本地的 VERSION.build。
# 反重构警告：调用方绝不能把 VERSION_FILE 指回源树 VERSION——源树 VERSION 注册在
# CMAKE_CONFIGURE_DEPENDS，构建期写入会让下一个构建必触发 cmake regen，且
# project(VERSION) 变化改动 DEVDESK_VERSION define、把 121 个 TU 拖进全量重编
#（ninja -d explain 实证：output build.ninja older than most recent input .../VERSION）。
if(NOT DEFINED VERSION_FILE)
    message(FATAL_ERROR "bump-version.cmake: 缺少 -D VERSION_FILE=<path> 参数")
endif()

# 首个构建：文件不存在 → 用种子写初值，不自增（+1 从第二个构建开始）
if(NOT EXISTS "${VERSION_FILE}")
    if(NOT DEFINED VERSION_SEED)
        message(FATAL_ERROR "bump-version.cmake: VERSION_FILE 不存在且缺 -D VERSION_SEED=<x.y.z>")
    endif()
    file(WRITE "${VERSION_FILE}" "${VERSION_SEED}")
    return()
endif()

file(READ "${VERSION_FILE}" VER)
string(STRIP "${VER}" VER)
string(REPLACE "." ";" V "${VER}")
list(GET V 0 X)
list(GET V 1 Y)
list(GET V 2 Z)

math(EXPR Z "${Z} + 1")
if(Z GREATER 9)
    set(Z 0)
    math(EXPR Y "${Y} + 1")
endif()
if(Y GREATER 9)
    set(Y 0)
    math(EXPR X "${X} + 1")
endif()
if(X GREATER 9)
    set(X 9)
    set(Y 9)
    set(Z 9)  # 饱和 9.9.9，不退回 9.0.0
endif()

set(NEW "${X}.${Y}.${Z}")
file(WRITE "${VERSION_FILE}" "${NEW}")
message(STATUS "Build version: ${VER} -> ${NEW}")