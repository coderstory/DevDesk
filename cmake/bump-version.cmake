# cmake/bump-version.cmake
# 构建版本号自增脚本（2026-09-28 about 页设计 D-01/D-03）：
#   VERSION 文件（x.y.z，每段 0-9）在每次构建时 +1：
#     z 满 10 进 y，y 满 10 进 x，x 满 10 饱和 9.9.9（不退回 9.0.0）。
#   独立可测：不依赖任何外部变量，只读 VERSION_FILE 指定路径的文件并写回。
# 调用：
#   cmake -D VERSION_FILE=<path> -P cmake/bump-version.cmake
# 被顶层 add_custom_target(bump_version ALL) 于每次 cmake --build 时执行；
# VERSION 经 CMAKE_CONFIGURE_DEPENDS 注册，bump 后的新值在下一个 configure
# 拾取（本次构建显示当前版本、下个构建得到 +1 后的版本）。
if(NOT DEFINED VERSION_FILE)
    message(FATAL_ERROR "bump-version.cmake: 缺少 -D VERSION_FILE=<path> 参数")
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