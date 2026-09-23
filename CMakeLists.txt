cmake_minimum_required(VERSION 3.14)
project(DevDesk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# 大气档编译选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# EUI-NEO 作为子项目（dev 时使用绝对路径，生产用 submodule）
# 注意：必须在 include(Dependencies.cmake) 之前定义——ZLIB 查找提示要引用自带 zlib 头
set(EUI_NEO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../EUI-NEO" CACHE PATH "EUI-NEO source dir")
if(NOT EXISTS "${EUI_NEO_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "EUI-NEO not found at ${EUI_NEO_DIR}")
endif()

# ---- EUI-NEO 版本诊断（审计 [11]）：打印 git rev + 软校验最低依赖 rev ----
# EUI-NEO 以同级源码目录集成（非 submodule），DevDesk 依赖其本地 main 上
# 尚未 push 到 origin 的提交（push 与否是用户决策门控），故此处**不做硬钉扎**
# （他人克隆只有 v0.6.0，硬 fail 会造成误阻断）；EUI-NEO push 后可将本诊断
# 升级为硬检查。最低依赖 rev 与 CLAUDE.md「当前依赖 rev」保持一致，消费
# EUI-NEO 新 API 后两处同步更新。
set(DEVDESK_EUI_NEO_MIN_REV "d5e302c")
execute_process(
    COMMAND git -C "${EUI_NEO_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE EUI_NEO_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _devdesk_eui_rev_rc
)
if(_devdesk_eui_rev_rc EQUAL 0 AND NOT EUI_NEO_GIT_REV STREQUAL "")
    message(STATUS "EUI-NEO git rev: ${EUI_NEO_GIT_REV} (minimum dependency rev: ${DEVDESK_EUI_NEO_MIN_REV})")
    execute_process(
        COMMAND git -C "${EUI_NEO_DIR}" merge-base --is-ancestor ${DEVDESK_EUI_NEO_MIN_REV} HEAD
        ERROR_QUIET
        RESULT_VARIABLE _devdesk_eui_anc_rc
    )
    if(NOT _devdesk_eui_anc_rc EQUAL 0)
        message(WARNING
            "EUI-NEO 当前检出（rev ${EUI_NEO_GIT_REV}）不包含 CLAUDE.md 记录的最低依赖 rev ${DEVDESK_EUI_NEO_MIN_REV}——"
            "trayMenu/windowEffect/lineStyles/requestFocus 等新 API 可能缺失导致编译失败。请将 EUI-NEO 切到本地 main "
            "并更新至最新（EUI-NEO push 后将启用硬版本检查）。")
    endif()
else()
    message(WARNING
        "无法读取 EUI-NEO（${EUI_NEO_DIR}）的 git rev——可能不是 git 检出或 PATH 中没有 git。DevDesk 依赖 EUI-NEO 本地 main "
        "的较新提交（当前最低依赖 rev ${DEVDESK_EUI_NEO_MIN_REV}，见 CLAUDE.md）；若后续编译报缺成员/缺头文件，请先检查 EUI-NEO 版本。")
endif()

# T3：MinGW 全静态链接（默认 ON，仅 MinGW 工具链生效，macOS/MSVC 不受影响）
# SQLite3/CURL/OpenSSL/zlib 全部用静态库 + libcurl.a 传递依赖闭包，
# 配合 EUI-NEO 给 app target 的 -static（运行时静态），产出单 exe 零外部 DLL。
option(DEVDESK_STATIC "MinGW: link everything statically into a single exe (no external DLLs)" ON)

# 系统依赖查找 + Windows/MSYS2 提示 + FTS5 检测（须在 add_subdirectory(EUI-NEO) 之前）
include(cmake/Dependencies.cmake)
# 编译警告函数（-Werror 只对 DevDesk 自己的 target 生效，见 devdesk_apply_warnings）
include(cmake/CompilerWarnings.cmake)

# EUI-NEO 子目录：不带 DevDesk 的警告选项（曾因 -Werror 泄漏产生 517 个第三方假错）
add_subdirectory(${EUI_NEO_DIR} ${CMAKE_BINARY_DIR}/_eui_neo_build EXCLUDE_FROM_ALL)

# Core + Platform 是 static lib
add_subdirectory(src/core)
add_subdirectory(src/platform)

# EUI-NEO include 路径（暴露给所有目标）
set(EUI_NEO_INCLUDE_DIR "${EUI_NEO_DIR}/include")
include_directories(${EUI_NEO_INCLUDE_DIR})

# 把 EUI-NEO 的 include 路径和 3rd 依赖加给所有目标
function(devdesk_add_eui_includes target)
    target_include_directories(${target} PRIVATE
        ${EUI_NEO_INCLUDE_DIR}
        ${EUI_NEO_DIR}
        ${EUI_NEO_DIR}/3rd/yyjson-0.12.0/src
        ${EUI_NEO_DIR}/3rd/md4c/src
    )
endfunction()

# 功能模块（home / markdown / todo / claude_proxy / settings）
add_subdirectory(src/module_home)
add_subdirectory(src/module_markdown)
add_subdirectory(src/module_todo)
add_subdirectory(src/module_claude_proxy)
add_subdirectory(src/module_settings)
add_subdirectory(src/module_pet)

# 测试
enable_testing()
add_subdirectory(tests)

# 主程序（app_sidebar.cpp = 侧边栏 + 根布局，单测复用同一份实现）
add_executable(DevDesk src/main.cpp src/app_sidebar.cpp)
devdesk_apply_warnings(DevDesk)
eui_neo_configure_app(DevDesk)

# EUI-NEO 会给非 Debug 的 app target 强加 -Os -fno-exceptions -fno-rtti
# （eui_apply_compile_options，无缓存开关）。其中 -Os 在 MSYS2 GCC 16.2 下
# 踩中 C4 ABI 缺口：-Os 时 GCC 对 std::string 移动构造发出新式 C4 引用
# （constexpr 构造的压缩 mangling，C++20 起 string 移动构造为 constexpr），
# 而 libstdc++.a 只提供 C1/C2 定义 → Release 链接 undefined reference
# （-O3 不触发，已实测；Debug 的 genex 本来就不展开，不受影响）。
# 这里只摘 -Os（app 回到 CMake 默认的 -O3），保留 EUI 的 -fno-exceptions/-fno-rtti。
# 根治应走上游：EUI 不应给消费者 app target 强加优化级别，或工具链补 C4 别名。
if(MINGW)
    get_target_property(_devdesk_compile_opts DevDesk COMPILE_OPTIONS)
    if(_devdesk_compile_opts)
        list(FILTER _devdesk_compile_opts EXCLUDE REGEX ":-Os>")
        set_target_properties(DevDesk PROPERTIES COMPILE_OPTIONS "${_devdesk_compile_opts}")
    endif()
    unset(_devdesk_compile_opts)
endif()
target_link_libraries(DevDesk PRIVATE
    devdesk_core
    devdesk_platform
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_module_claude_proxy
    devdesk_module_settings
    devdesk_module_pet
    eui_neo
)
target_include_directories(DevDesk PRIVATE
    src
    # 不加 src/core：src/core/time.h 会劫持系统 <time.h>/<ctime>（详见 src/core/CMakeLists.txt）
    src/platform
    src/module_home
    src/module_markdown
    src/module_todo
    src/module_claude_proxy
    src/module_settings
    src/module_pet
    ${EUI_NEO_INCLUDE_DIR}
    ${EUI_NEO_DIR}
)
target_include_directories(DevDesk PRIVATE
    ${EUI_NEO_DIR}/3rd/yyjson-0.12.0/src
    ${EUI_NEO_DIR}/3rd/md4c/src
)

# 安装 / 拷贝资源
# fonts/ 不入 git（.gitignore 按文件名忽略，下载前目录可能不存在/为空），
# 必须守卫，否则新克隆/新 worktree 的 POST_BUILD 拷贝直接失败
if(EXISTS "${CMAKE_SOURCE_DIR}/fonts")
    add_custom_command(TARGET DevDesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:DevDesk>/fonts"
        COMMENT "Copying fonts to build directory"
    )
endif()

# 桌宠美术皮肤包（assets/pets/<name>/pet.json + 帧 PNG，随仓库分发；程序
# 生成的内置皮肤走 petsDir()，不在这里）。皮肤根候选含 cwd/assets/pets，
# 从 build 目录启动 DevDesk 即可发现。assets/pets 入 git，无需守卫。
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/pets" "$<TARGET_FILE_DIR:DevDesk>/assets/pets"
    COMMENT "Copying pet skins to build directory"
)

# 构建 .app bundle（仅 macOS 平台生效）
if(APPLE)
    add_custom_target(devdesk_app
        COMMAND ${CMAKE_SOURCE_DIR}/platform/macos/build-app.sh
        DEPENDS DevDesk
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building DevDesk.app bundle"
    )
    add_dependencies(devdesk_app DevDesk)
endif()