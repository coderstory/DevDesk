cmake_minimum_required(VERSION 3.14)

# ---- 构建版本号：VERSION 文件 x.y.z ----
# VERSION 不入 git（.gitignore），只在首次 configure 自动创建 "0.1.0"、此后仅手工改。
# 每次构建的自增计数在 build 目录本地的 VERSION.build（add_custom_target(bump_version
# ALL)，见文件尾）。构建期绝不能写源树 VERSION：它注册在 CMAKE_CONFIGURE_DEPENDS
#（本文件下方），构建期写入 = 下一个构建必触发 cmake regen，且 project(VERSION)
# 变化改动 DEVDESK_VERSION define、把 121 个 TU 拖进全量重编（ninja -d explain 实证）。
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" DEVDESK_VERSION)
    string(STRIP "${DEVDESK_VERSION}" DEVDESK_VERSION)
else()
    set(DEVDESK_VERSION "0.1.0")
    file(WRITE "${CMAKE_SOURCE_DIR}/VERSION" "${DEVDESK_VERSION}")
endif()
project(DevDesk VERSION ${DEVDESK_VERSION} LANGUAGES C CXX)

if(APPLE)
  # notification_macos.mm 需要 ObjC++。EUI-NEO 子项目自己启用过 OBJCXX，但 DevDesk
  # 不能依赖子项目的副作用（子目录顺序/配置变化都会让 .mm 失去正确语言识别）。
  enable_language(OBJCXX)
endif()

if(WIN32)
  # httplib 0.58+ 要求 Win10+（头内 #error + CreateFile2 需要 _WIN32_WINNT>=0x0A00）。
  # MSYS2 MinGW 默认值偏低；本项目 Windows 门槛本就是 Win10+，全局统一定义
  #（含 EUI-NEO 子目录与全部测试目标，避免各 TU 取值不一致）。
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# VERSION 文件变更（手工编辑）→ 自动重新 configure；构建期不写本文件（见文件头）
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

# ---- 构建期信息注入（2026-09-28 about 页设计；消费方 = src/module_about）----
# EUI-NEO 消费政策（ADR 0006，2026-10-25 用户拍板）：同级源码目录引入、**不锁
# 版本、每次用最新源码**——configure 期不做任何基于 git rev 的版本校验（旧
# 「最低依赖 rev」软校验已于 2026-10-26 政策性删除：其 rev 基准在 EUI-NEO 历史
# 重写后已蒸发，检查每次 configure 都在误告警）。版本匹配的唯一权威判据 =
# scripts/check-eui-neo-version.sh 的内容级 API 守卫。此处仅取 EUI-NEO 当前检出
# rev 供关于页信息展示（非门槛）。DevDesk 自身 git rev：非 git 环境（源码 zip
# 下载等）回退 "unknown"，不 fail configure（D-03）。用 -C 显式指定仓库，不依赖
# execute_process 的工作目录。
set(DEVDESK_EUI_NEO_REV "unknown")
execute_process(
    COMMAND git -C "${EUI_NEO_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE EUI_NEO_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
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

# ---- 关于页「构建时间」行扩展（UAT6）：构建机设备名 + 系统标签原料 ----
# 目标格式 `年-月-日 时:分:秒@设备名@系统`（时间来自 __DATE__/__TIME__ 编译期宏，
# 运行时在 about_module.cpp 解析重组，可单测）。
# 设备名：Windows 取 ENV{COMPUTERNAME}（如 E-YUNFEI-PC）；macOS/Linux 无该
# 环境变量，用 hostname 命令取同等的机器标识（取不到保持 unknown）。
set(DEVDESK_BUILD_HOST "unknown")
if(WIN32 AND DEFINED ENV{COMPUTERNAME} AND NOT "$ENV{COMPUTERNAME}" STREQUAL "")
    set(DEVDESK_BUILD_HOST "$ENV{COMPUTERNAME}")
else()
    execute_process(
        COMMAND hostname
        OUTPUT_VARIABLE _devdesk_build_host
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _devdesk_build_host STREQUAL "")
        set(DEVDESK_BUILD_HOST "${_devdesk_build_host}")
    endif()
endif()
# 系统原料 = CMAKE_SYSTEM_NAME + CMAKE_SYSTEM_VERSION（如 "Windows 10.0.26200" /
# "Darwin 24.5.0"）；用户口径归并（Windows 11 / macOS）在 about_module.cpp 运行时做。
if(NOT CMAKE_SYSTEM_VERSION STREQUAL "")
    set(DEVDESK_BUILD_SYSTEM "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}")
else()
    set(DEVDESK_BUILD_SYSTEM "${CMAKE_SYSTEM_NAME}")
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

# 功能模块（home / markdown / todo / claude_proxy / settings / about / agent / wallpaper / schedule）
add_subdirectory(src/module_home)
add_subdirectory(src/module_markdown)
add_subdirectory(src/module_todo)
add_subdirectory(src/module_agent)
add_subdirectory(src/module_memory)
add_subdirectory(src/module_claude_proxy)
add_subdirectory(src/module_settings)
add_subdirectory(src/module_about)
add_subdirectory(src/module_wallpaper)
add_subdirectory(src/module_release)
add_subdirectory(src/module_mcp)
# 定时调度子系统（S3 存储层先行，S7a 完成 app_registry/PageId/宿主装配接线）
add_subdirectory(src/module_schedule)

# UAT6：构建机信息注入关于页构建行。定义挂在根而非 src/module_about/
# CMakeLists.txt——本批冻结清单只开根 CMakeLists（module_about 侧文件不动），
# target 已存在，add_subdirectory 之后追加 PRIVATE 定义等效同源。
target_compile_definitions(devdesk_module_about PRIVATE
    DEVDESK_BUILD_HOST="${DEVDESK_BUILD_HOST}"
    DEVDESK_BUILD_SYSTEM="${DEVDESK_BUILD_SYSTEM}"
)
# module_pet 已于 2026-10-13 整模块摘除（分支 feat/remove-pet-module）

# ---- 覆盖率插桩（2026-10-03 已知欠账落地）：DEVDESK_COVERAGE=ON 按编译器分路线 ----
# 配套 scripts/coverage.sh（独立 build-coverage/ 目录：configure → 编 devdesk_tests →
# 跑测试 → 汇总分层报告），build-test-run.sh 保持零改动。
# 默认 OFF：正常构建的编译 / 链接命令一字不变，CI 与日常路径不受影响。
# 插桩口径：只插桩 DevDesk 自己的库层（core / platform / ui_kit / module_*，含 agent）——
#   * EUI-NEO 子目录不插桩（外部框架，且其 add_subdirectory 在上方已处理完，
#     本块的目录级 / 目标级操作都够不着它，第三方代码不进分层口径）；
#   * 可执行文件（devdesk_tests / probes / DevDesk）只加**链接期** flag：它们链接了
#     插过桩的静态库，链接期不带 profile runtime 会 undefined symbol
#     （AppleClang 是 __llvm_profile_*，GCC 是 __gcov_* / gcov runtime）；
#     自身源码不插桩——测试代码与 UI 探针自身的行覆盖不是本工具要回答的问题，
#     插了只拖慢编译与运行（探针还要开 GL 窗口）。
# 两条路线（2026-10-26 08 号票补上 GCC/MinGW 分支）：
#   * AppleClang：llvm source-based（-fprofile-instr-generate -fcoverage-mapping）；
#   * GNU（含 Windows/MinGW GCC）：gcov（--coverage ≈ 编译 -fprofile-arcs -ftest-coverage
#     + 链接 -lgcov，一个词对编译/链接都成立）；
#   * 其余（MSVC、非 Apple 的 Clang……）直接 FATAL_ERROR，不静默产出带错 flag 的构建。
option(DEVDESK_COVERAGE "覆盖率插桩（AppleClang=llvm-cov / GNU=gcov；配套 scripts/coverage.sh，默认 OFF 不影响正常构建）" OFF)
if(DEVDESK_COVERAGE)
    # 插桩目标清单（两路线共用；与上方 add_subdirectory 清单同源维护；
    # 注意 module_agent 的库名是 devdesk_agent，无 module_ 前缀）
    set(DEVDESK_COVERAGE_LIBS
        devdesk_core devdesk_platform devdesk_ui_kit
        devdesk_module_home devdesk_module_markdown devdesk_module_todo
        devdesk_agent devdesk_module_memory
        devdesk_module_claude_proxy devdesk_module_settings
        devdesk_module_about devdesk_module_wallpaper devdesk_module_release
        devdesk_module_mcp devdesk_module_schedule)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        # 链接期（目录级，置于 add_subdirectory(tests) 与主程序定义**之前**）：
        # 之后创建的所有可执行文件都拉进 profile runtime
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
        foreach(_devdesk_cov_lib IN LISTS DEVDESK_COVERAGE_LIBS)
            if(TARGET ${_devdesk_cov_lib})
                target_compile_options(${_devdesk_cov_lib} PRIVATE
                    -fprofile-instr-generate -fcoverage-mapping)
            else()
                message(WARNING "DEVDESK_COVERAGE: 目标 ${_devdesk_cov_lib} 不存在（模块清单变动？），该层不会被插桩")
            endif()
        endforeach()
        message(STATUS "覆盖率插桩: ON（AppleClang llvm-cov，DevDesk 库层；EUI-NEO / 测试 / 探针源码不插桩）")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # GCC/MinGW 路线（08 号票，计划 T6）：结构同上，flag 换 --coverage。
        # 链接期目录级 —— devdesk_tests / DevDesk 链接时拉进 gcov runtime（-lgcov），
        # 跑完在对象文件旁落 .gcda；编译期目标级 —— 只给库层插桩。
        add_link_options(--coverage)
        foreach(_devdesk_cov_lib IN LISTS DEVDESK_COVERAGE_LIBS)
            if(TARGET ${_devdesk_cov_lib})
                target_compile_options(${_devdesk_cov_lib} PRIVATE --coverage)
            else()
                message(WARNING "DEVDESK_COVERAGE: 目标 ${_devdesk_cov_lib} 不存在（模块清单变动？），该层不会被插桩")
            endif()
        endforeach()
        message(STATUS "覆盖率插桩: ON（GNU gcov，DevDesk 库层；EUI-NEO / 测试 / 探针源码不插桩）")
    else()
        message(FATAL_ERROR "DEVDESK_COVERAGE=ON 支持 AppleClang（llvm-cov 路线）与 GNU/MinGW GCC（gcov 路线）；"
            "当前编译器 ${CMAKE_CXX_COMPILER_ID} 不支持，请勿开启")
    endif()
endif()

# 测试
enable_testing()
add_subdirectory(tests)

# 主程序（app_sidebar.cpp = 侧边栏 + 根布局，app_assembly.cpp = 装配层，
# 两者单测复用同一份实现）
add_executable(DevDesk src/main.cpp src/app_sidebar.cpp src/app_assembly.cpp
    src/module_toggle_bridge.cpp src/wallpaper_tray_bridge.cpp
    # MCP 桥实现缝（B3）：app_assembly / settings_module 引用 app::mcp::* 符号；
    # 缝本体只依赖 core，不产生 module_mcp 链接边
    src/mcp_service_bridge.cpp
    # 记忆 MCP 桥实现缝：main.cpp / module_mcp 页引用 app::memory_mcp::* 符号；
    # 缝本体只依赖 core，不产生 module_memory 链接边
    src/memory_mcp_bridge.cpp
    # toast 通知桥实现缝：schedule toast 渠道引用 app::toast_notice::* 符号；
    # 缝本体只依赖标准库，不产生 module_schedule / app 层链接边
    src/toast_notice_bridge.cpp)
devdesk_apply_warnings(DevDesk)
# 排除 EUI-NEO 的旧图标 RC（IDI_APP_ICON）：DevDesk 已在下方用 devdesk.rc.in
# 注入自己的 "A_DEVDESK_ICON" 命名资源。两份 RC 共用 RT_ICON ID 1-6 会导致
# windres/ld 合并时 .rsrc 冲突（duplicate leaf warning + PE 图标数据损坏）。
# 清空 EUI-NEO 全局属性后再调 eui_neo_configure_app，使其不再注入图标 RC。
set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "")
eui_neo_configure_app(DevDesk KEEP_EXCEPTIONS_RTTI)

# EUI-NEO 曾给非 Debug 的 app target 强加 -Os（eui_apply_compile_options），
# 该 -Os 在 MSYS2 GCC 16.2 下踩中 C4 ABI 缺口：-Os 时 GCC 对 std::string 移动
# 构造发出新式 C4 引用（constexpr 构造的压缩 mangling），而 libstdc++.a 只提供
# C1/C2 定义 → Release 链接 undefined reference。上游 U8 已改：eui_neo_configure_app
# 传 NO_SIZE_OPT，不再向消费者 app target 强加优化级别（EUI-NEO rev 3d231be），
# DevDesk 侧摘 -Os 的 workaround 于 2026-09-27 随之删除（此处不再需要改写编译选项）。
#
# 同类问题的第二个实例（2026-10-17 修）：eui_apply_compile_options 还把「框架自身
# 不用异常/RTTI」的策略**无条件**加进消费者 app target（NO_SIZE_OPT 只免 -Os）。
# DevDesk 有 24 处 catch（多为 std::filesystem / std::thread 的 std::system_error，
# 属必须捕获）与 1 处 dynamic_cast<AgentModule*> → 非 Debug 直接无法编译：
#   event_bus.h:108     error: cannot use 'try' with exceptions disabled
#   app_sidebar.cpp:245 error: use of dynamic_cast requires -frtti
# 上游已加豁免开关：此处传 KEEP_EXCEPTIONS_RTTI 保留宿主语言能力。**EUI-NEO 自己的
# target 仍带 -fno-exceptions/-fno-rtti，策略未放宽**（只停止外溢给消费者）。
target_link_libraries(DevDesk PRIVATE
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_agent
    devdesk_module_memory
    devdesk_module_claude_proxy
    devdesk_module_settings
    devdesk_module_about
    devdesk_module_wallpaper
    devdesk_module_release
    devdesk_module_mcp
    devdesk_module_schedule
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
    src/module_agent
    src/module_wallpaper
    src/module_mcp
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

# 托盘图标（assets/tray-icon-template.png，2026-09-30 Bug A 修；macOS 侧
# template 蒙版，Windows 侧品牌 .ico tray-icon-win.ico —— 路径由
# platform::tray::defaultIconPath() 按平台分发，main.cpp 不带平台宏）。
# 运行时经 EUI-NEO resolveIconPath 的 exeDir/assets 候选在 .app bundle 内命中
# （build-app.sh 把 <build>/assets 整树拷进 Contents/MacOS/assets，本命令负责
# 先把图标送进 <build>/assets）。文件均入 git，无需平台守卫（同上 pets：
# macOS 多拷一个 .ico 无害，反而让「产物里文件都在」可被用例直接验证）。
# 单文件用 copy_if_different；目录由上面 pets 的 copy_directory 先建，
# make_directory 兜底防顺序依赖。
# 2026-10-20 C 方案图标批：三件套全部换终端窗 >_ 设计；tray-icon-template@2x.png
# 为 44×44 Retina 倍图（EUI-NEO eui_tray_image 按同名 @2x 命名惯例拾取，与
# 底图合并为双档 NSImage 表示，见 EUI-NEO rev 0f7f60b）。
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DevDesk>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-template.png"
        "$<TARGET_FILE_DIR:DevDesk>/assets/tray-icon-template.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-template@2x.png"
        "$<TARGET_FILE_DIR:DevDesk>/assets/tray-icon-template@2x.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-win.ico"
        "$<TARGET_FILE_DIR:DevDesk>/assets/tray-icon-win.ico"
    COMMENT "Copying tray icons to build directory"
)

# 应用图标（2026-10-20 C 方案终端窗 >_ 批）：build/assets 侧的两份品牌图。
#   icon.ico → 安装包 SetupIconFile（installer/windows/devdesk.iss，经
#     scripts/package-windows.sh 的 AssetsDir 暂存链取 <build-release>/assets）；
#   icon.png → EUI-NEO dsl_app 默认 windowIconPath "assets/icon.png"
#     （applyWindowIcon 每进程一次 glfwSetWindowIcon = Windows 任务栏/alt-tab
#     运行态图标；macOS build-app.sh 亦从 <build>/assets/icon.png 派生 .icns）。
# 执行序保证：EUI-NEO 的 assets 整树 bulk copy 是 eui_neo_configure_app(:226)
# 内附加的 DevDesk POST_BUILD（其 assets/ 自带一份旧 icon.ico/icon.png）；CMake
# 对同一 target 的 POST_BUILD 按定义顺序串行执行，本节定义在 :226 之后 → 恒后跑，
# 同名覆盖确定生效（上方托盘图标块沿用同一机制，EUI assets 里的同名托盘图从未反扑）。
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DevDesk>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/icon.ico"
        "$<TARGET_FILE_DIR:DevDesk>/assets/icon.ico"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/icon.png"
        "$<TARGET_FILE_DIR:DevDesk>/assets/icon.png"
    COMMENT "Copying app icons to build directory"
)

# exe 内嵌图标 + 版本资源（仅 Windows；资源管理器文件图标 / 桌面快捷方式 /
# 「应用和功能」卸载项全靠它。macOS 无内嵌资源一说，.app 图标走 build-app.sh）。
# windres 解析 .rc 里的相对路径按调用 CWD，不可靠——沿用 EUI-NEO eui_app_icon.rc
# 的约定：configure 期把绝对原生路径（反斜杠双写转义）写进生成文件。图标用命名
# 资源 "A_DEVDESK_ICON"（非数值 ID）：PE 资源目录命名条目排在数值 ID 之前，
# "A_DEVDESK_ICON" 按 Unicode 升序落在 "IDI_APP_ICON"（EUI-NEO 旧图标）之前 →
# Shell 取首个图标组 → DevDesk C 方案图标胜出；两份资源并存互不影响。
if(WIN32)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/assets/icon.ico" DEVDESK_ICON_ICO_NATIVE)
    string(REPLACE "\\" "\\\\" DEVDESK_ICON_ICO_ESCAPED "${DEVDESK_ICON_ICO_NATIVE}")
    configure_file(src/devdesk.rc.in "${CMAKE_BINARY_DIR}/devdesk.rc" @ONLY)
    target_sources(DevDesk PRIVATE "${CMAKE_BINARY_DIR}/devdesk.rc")
endif()

# 启动 splash shader（assets/shaders/splash.frag，随仓库分发；运行时经
# eui::platform::resolveResourcePath 从 exe 旁或 cwd 的 assets/ 下解析，
# 从 build 目录启动即可命中）
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders" "$<TARGET_FILE_DIR:DevDesk>/assets/shaders"
    COMMENT "Copying splash shader to build directory"
)

# 构建版本自增（2026-09-28 about 页设计）：每次 cmake --build 都把 build 目录本地的
# VERSION.build +1（不用 PRE_BUILD——Ninja 下 PRE_BUILD = PRE_LINK，仅目标重链时跑；
# ALL 保证每次构建都执行）。计数文件只落 build 目录，绝不写源树 VERSION——
# VERSION 注册在 CMAKE_CONFIGURE_DEPENDS（见文件头），构建期写入 = 每次构建后
# 下一个构建必 regen + 121 TU 全量重编。DEVDESK_VERSION define 取 configure 期的
# PROJECT_VERSION；VERSION.build 当前无 UI 消费方，保留计数供后续按需接线。
add_custom_target(bump_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D VERSION_FILE="${CMAKE_BINARY_DIR}/VERSION.build"
        -D VERSION_SEED="${PROJECT_VERSION}"
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