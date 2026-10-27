cmake_minimum_required(VERSION 3.16)
project(memcpp-probe C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# llama.cpp 以子目录并入探针构建树，全部编静态库（BUILD_SHARED_LIBS=OFF）。
# 探针是独立于 DevDesk 主构建（scripts/build-test-run.sh）的一次性验证工程，
# 只取 llama 静态库，不编示例/服务/测试。
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)

add_subdirectory(llama.cpp)

add_executable(memcpp-probe probe_main.cpp)
target_link_libraries(memcpp-probe PRIVATE llama psapi shell32)
# 契约：单 exe 零 MinGW 运行时依赖。-static 使 libgcc/libstdc++/winpthread 全静态；
# msvcrt/kernel32 等 Windows 系统 DLL 不在此列（与 DevDesk 主程序同一验收口径）。
target_link_options(memcpp-probe PRIVATE -static)
