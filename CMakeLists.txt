cmake_minimum_required(VERSION 3.14)

# ---- 构建版本号（2026-09-28 about 页设计）：VERSION 文件 x.y.z ----
# VERSION 不入 git（.gitignore），每机器各自递增；首次 configure 缺则自动
# 创建 "0.1.0"。每次构建由 add_custom_target(bump_version ALL) 自增（见文件尾），
# 新值经 CMAKE_CONFIGURE_DEPENDS 在下一个 configure 拾取（本次构建显示当前版本、
# 下个构建得到 +1 后的版本）。
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" DEVDESK_VERSION)
    string(STRIP "${DEVDESK_VERSION}" DEVDESK_VERSION)
else()
    set(DEVDESK_VERSION "0.1.0")
    file(WRITE "${CMAKE_SOURCE_DIR}/VERSION" "${DEVDESK_VERSION}")
endif()
project(DevDesk VERSION ${DEVDESK_VERSION} LANGUAGES C CXX)

if(WIN32)
  # httplib 0.58+ 要求 Win10+（头内 #error + CreateFile2 需要 _WIN32_WINNT>=0x0A00）。
  # MSYS2 MinGW 默认值偏低；本项目 Windows 门槛本就是 Win10+，全局统一定义
  #（含 EUI-NEO 子目录与全部测试目标，避免各 TU 取值不一致）。
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# VERSION 文件变更（bump_version 写入）→ 自动重新 configure
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# 大气档编译选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 编译缓存 ccache 自动探测（2026-09-27 起为仓库默认，不再依赖手敲 -D）----
# 背景：scripts/ 一体化脚本本来就带 launcher，但手工 configure 的路径全靠人记得加
# -DCMAKE_*_COMPILER_LAUNCHER——新会话 / 新 worktree / 新 agent 读不到这个约定就吃不到缓存。
# 这里在 configure 期探测一次，命中即全树生效（含 EUI-NEO 子目录，普通变量会继承下去；
# 也覆盖 Dependencies.cmake 里的 try_compile 探测）。视作**默认开**，两条路可显式关闭：
#   1) -DDEVDESK_CCACHE=OFF
#   2) 自行传 CMAKE_C/CXX_COMPILER_LAUNCHER（含 -DCMAKE_CXX_COMPILER_LAUNCHER= 置空）
# 未安装 ccache 时只打一行 STATUS 说明，不 warning、构建行为与从前完全一致。
option(DEVDESK_CCACHE "自动探测 ccache 并作为编译缓存（-DDEVDESK_CCACHE=OFF 显式关闭）" ON)
if(DEFINED CACHE{CMAKE_C_COMPILER_LAUNCHER} OR DEFINED CACHE{CMAKE_CXX_COMPILER_LAUNCHER})
    # 调用方自己指定了 launcher（含显式置空）→ 完全尊重，不覆盖
    message(STATUS "ccache: 跳过（调用方已指定 launcher: C='${CMAKE_C_COMPILER_LAUNCHER}' CXX='${CMAKE_CXX_COMPILER_LAUNCHER}'）")
elseif(NOT DEVDESK_CCACHE)
    message(STATUS "ccache: 已关闭（DEVDESK_CCACHE=OFF）")
else()
    find_program(DEVDESK_CCACHE_BIN NAMES ccache)
    if(DEVDESK_CCACHE_BIN)
        set(CMAKE_C_COMPILER_LAUNCHER "${DEVDESK_CCACHE_BIN}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${DEVDESK_CCACHE_BIN}")
        message(STATUS "ccache: 已启用 (${DEVDESK_CCACHE_BIN})")
    else()
        message(STATUS "ccache: 未找到（构建不受影响）")
    endif()
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
# EUI-NEO 新 API 后两处同步更新（当前门槛取 U1 尺度契约合入点，含
# app::contentScale / core::dsl::Screen::scale 与 yyjson writer 放开；
# 2026-09-30 起另含 dropdown 展开态弹层 z 修复——tests/probes 的弹层 z 序
# 门禁依赖，见 tests/probes/settings_dropdown_z_probe.cpp）。
set(DEVDESK_EUI_NEO_MIN_REV "72a5187")
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

# ---- 构建期信息注入（2026-09-28 about 页设计；消费方 = src/module_about）----
# DevDesk 自身 git rev：非 git 环境（源码 zip 下载等）回退 "unknown"，不 fail
# configure（D-03）。用 -C 显式指定仓库，不依赖 execute_process 的工作目录。
set(DEVDESK_EUI_NEO_REV "unknown")
if(NOT EUI_NEO_GIT_REV STREQUAL "")
    set(DEVDESK_EUI_NEO_REV "${EUI_NEO_GIT_REV}")
endif()
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=7 HEAD
    OUTPUT_VARIABLE DEVDESK_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _devdesk_git_rev_rc
)
if(NOT _devdesk_git_rev_rc EQUAL 0 OR DEVDESK_GIT_REV STREQUAL "")
    set(DEVDESK_GIT_REV "unknown")
endif()
message(STATUS "DevDesk git rev: ${DEVDESK_GIT_REV} (build version ${PROJECT_VERSION})")

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

# presentation 能力层（任务 D / R1）：跨模块重复的 UI 接线（当前 = toast）。
# 顺序：晚于 src/core（依赖 devdesk_core）、早于消费它的 module_* 与主程序
add_subdirectory(src/ui_kit)

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

# 功能模块（home / markdown / todo / claude_proxy / settings / about / agent）
add_subdirectory(src/module_home)
add_subdirectory(src/module_markdown)
add_subdirectory(src/module_todo)
add_subdirectory(src/module_agent)
add_subdirectory(src/module_claude_proxy)
add_subdirectory(src/module_settings)
add_subdirectory(src/module_about)
add_subdirectory(src/module_pet)

# 测试
enable_testing()
add_subdirectory(tests)

# 主程序（app_sidebar.cpp = 侧边栏 + 根布局，app_assembly.cpp = 装配层，
# 两者单测复用同一份实现）
add_executable(DevDesk src/main.cpp src/app_sidebar.cpp src/app_assembly.cpp
    src/module_toggle_bridge.cpp)
devdesk_apply_warnings(DevDesk)
eui_neo_configure_app(DevDesk)

# EUI-NEO 曾给非 Debug 的 app target 强加 -Os（eui_apply_compile_options），
# 该 -Os 在 MSYS2 GCC 16.2 下踩中 C4 ABI 缺口：-Os 时 GCC 对 std::string 移动
# 构造发出新式 C4 引用（constexpr 构造的压缩 mangling），而 libstdc++.a 只提供
# C1/C2 定义 → Release 链接 undefined reference。上游 U8 已改：eui_neo_configure_app
# 传 NO_SIZE_OPT，不再向消费者 app target 强加优化级别（EUI-NEO rev 3d231be），
# DevDesk 侧摘 -Os 的 workaround 于 2026-09-27 随之删除（此处不再需要改写编译选项；
# EUI 的 -fno-exceptions/-fno-rtti 仍由其 genex 提供，未受影响）。
target_link_libraries(DevDesk PRIVATE
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_agent
    devdesk_module_claude_proxy
    devdesk_module_settings
    devdesk_module_about
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
    src/module_about
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

# 托盘图标（assets/tray-icon-template.png，2026-09-30 Bug A 修；macOS 侧
# template 蒙版，Windows 侧品牌 .ico tray-icon-win.ico —— 路径由
# platform::tray::defaultIconPath() 按平台分发，main.cpp 不带平台宏）。
# 运行时经 EUI-NEO resolveIconPath 的 exeDir/assets 候选在 .app bundle 内命中
# （build-app.sh 把 <build>/assets 整树拷进 Contents/MacOS/assets，本命令负责
# 先把图标送进 <build>/assets）。两个文件均入 git，无需平台守卫（同上 pets：
# macOS 多拷一个 .ico 无害，反而让「产物里两个文件都在」可被用例直接验证）。
# 单文件用 copy_if_different；目录由上面 pets 的 copy_directory 先建，
# make_directory 兜底防顺序依赖。
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DevDesk>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-template.png"
        "$<TARGET_FILE_DIR:DevDesk>/assets/tray-icon-template.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-win.ico"
        "$<TARGET_FILE_DIR:DevDesk>/assets/tray-icon-win.ico"
    COMMENT "Copying tray icons to build directory"
)

# 启动 splash shader（assets/shaders/splash.frag，随仓库分发；运行时经
# eui::platform::resolveResourcePath 从 exe 旁或 cwd 的 assets/ 下解析，
# 从 build 目录启动即可命中）
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders" "$<TARGET_FILE_DIR:DevDesk>/assets/shaders"
    COMMENT "Copying splash shader to build directory"
)

# 构建版本自增（2026-09-28 about 页设计）：每次 cmake --build 都跑 bump-version
# 脚本把 VERSION 文件 +1（不用 PRE_BUILD——Ninja 下 PRE_BUILD = PRE_LINK，仅目标
# 重链时跑；ALL 保证每次构建都执行）。VERSION 经 CMAKE_CONFIGURE_DEPENDS 注册，
# 写入的新版本在下一个 configure 拾取：本次构建显示当前版本、下个构建 +1。
# 放 DevDesk target 之后定义（不依赖它的产物，仅保证与构建同拍）。
add_custom_target(bump_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D VERSION_FILE="${CMAKE_SOURCE_DIR}/VERSION"
        -P "${CMAKE_SOURCE_DIR}/cmake/bump-version.cmake"
    COMMENT "Bumping build version"
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