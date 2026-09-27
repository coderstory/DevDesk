# 编译警告统一配置（2026-09-21 重构）
#
# -Werror 等警告选项只作用于 DevDesk 自己的 target（通过 devdesk_apply_warnings），
# 绝不 add_compile_options 全局下压——那会泄漏进 EUI-NEO 子目录的第三方代码，
# 曾在 MinGW GCC 下产生 517 个假错。
#
# 用法：每个 DevDesk target 定义后调用一次 devdesk_apply_warnings(<target>)

function(devdesk_apply_warnings target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "devdesk_apply_warnings: target ${target_name} does not exist")
    endif()

    # Apple Clang 的 CMAKE_CXX_COMPILER_ID 是 "AppleClang"（CMP0025 NEW，
    # CMake 4.x 恒为 NEW），不匹配下面的 "Clang" —— 漏掉它会让 macOS 上
    # -Wall -Wextra -Wpedantic -Werror 整段静默失效（编译器 id 判据从未命中）
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
       OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra)
        endif()
        # 顺序在后才能覆盖 -Wall/-Wextra 触发的该类警告
        target_compile_options(${target_name} PRIVATE -Wno-unused-parameter)
        # EUI-NEO 头文件（theme.h/markdown.h 的聚合初始化）会触发该警告，
        # 属第三方代码：降级为非致命，DevDesk 自己的新增告警仍可见
        target_compile_options(${target_name} PRIVATE -Wno-error=missing-field-initializers)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target_name} PRIVATE /W4)
    endif()
endfunction()
