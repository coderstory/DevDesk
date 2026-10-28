add_library(devdesk_platform STATIC
    notification.h                      # 平台通知抽象（实现按平台选择）
    notification.cpp                    # facade：清洗/截断/去重/限速（平台无关，全平台编译）
    http_client.cpp
    http_server.cpp
    json.cpp
    github_api.cpp                      # GitHub Contents API 客户端（libcurl，不依赖外部 git）
    utf8.cpp                            # UTF-8 良构校验/清洗（M2 审计 F1：跨模块纯函数）
    autostart.h                         # 开机自启动抽象：注入缝+纯逻辑在头（UAT6），实现按平台拆
    process_runner.h                    # 进程执行器抽象：注入缝+纯逻辑在头（UAT6），实现按平台拆
    process_host.h                      # 常驻子进程托管抽象：拉起/树杀/探活（memcpp S2），实现按平台拆
    wallpaper_capability.h              # 壁纸宿主能力抽象：实现按平台拆（Windows true / 其余 false）
    window_visibility.h                 # 主窗可见态抽象：接口平台无关，实现按平台拆（_win/_macos）
    file_open_common.cpp
    terminal_launch_common.cpp          # 跨平台纯函数（三支之外，无条件编译）
)

# 平台适配实现按平台选择编译（接口隔离：头文件平台无关，实现 *_win/_macos/_posix，
# 业务代码零平台宏 —— 与 EUI-NEO 后端选择同一模式）
if(WIN32)
    target_sources(devdesk_platform PRIVATE
        exec_capture_win.cpp
        folder_picker_win.cpp           # IFileDialog COM 目录选择（FOS_PICKFOLDERS）
        keychain_win.cpp
        credential_protection_win.cpp  # DPAPI 密文入库（不得降级明文）
        file_open_win.cpp
        system_theme_win.cpp
        file_watcher_win.cpp
        http_client_ca_win.cpp
        terminal_launch_win.cpp
        tray_icon_win.cpp              # 托盘图标走真 .ico（ExtractIconExW 不认 PNG）
        message_box_win.cpp             # 启动期阻塞对话框（损坏还原流程）
        single_instance_win.cpp        # 单实例互斥：命名 mutex + 事件监听线程
        notification_win.cpp           # 系统通知：EUI-NEO 托盘通知（Shell_NotifyIconW NIF_INFO）
        autostart_win.cpp              # 开机自启动：HKCU Run 注册表写/删/查（UAT6）
        process_runner_win.cpp          # 进程执行器：CreateProcess + 管道全捕获（UAT6）
        process_host_win.cpp            # 常驻子进程托管：CreateProcessW + taskkill 树杀（memcpp S2）
        wallpaper_capability_win.cpp    # 壁纸宿主能力：WorkerW/mpv 路线可用（true）
        window_visibility_win.cpp       # 主窗可见态：EnumWindows 捕获 HWND + IsIconic/IsWindowVisible
    )
elseif(APPLE)
    target_sources(devdesk_platform PRIVATE
        exec_capture_posix.cpp
        folder_picker_macos.cpp         # osascript choose folder（免 ObjC 链接）
        keychain_macos.cpp
        credential_protection_posix.cpp  # macOS 与 Linux 同实现，共用 _posix
        file_open_macos.cpp
        system_theme_macos.cpp
        file_watcher_macos.cpp
        http_client_ca_posix.cpp
        terminal_launch_macos.cpp
        tray_icon_macos.cpp            # 托盘图标走 22×22 template 蒙版（不变）
        message_box_macos.cpp          # osascript display dialog（启动期阻塞对话框）
        single_instance_macos.cpp      # .app bundle 系统保证单实例，恒 true
        notification_macos.mm          # UNUserNotificationCenter（ARC，见下方编译选项）
        autostart_macos.cpp            # 开机自启动：LaunchAgents plist 写/删/查（UAT6）
        process_runner_macos.cpp        # 进程执行器：posix_spawn + 管道全捕获（UAT6）
        process_host_posix.cpp          # 常驻子进程托管：fork/setsid + 组杀（memcpp S2）
        wallpaper_capability_macos.cpp  # 壁纸宿主能力：第二阶段 ObjC++ 平台层前恒 false
        window_visibility_macos.cpp     # 主窗可见态：当前 GLFW 上下文窗口的 ICONIFIED/VISIBLE（无句柄 fail-open）
    )
else()
    target_sources(devdesk_platform PRIVATE
        exec_capture_posix.cpp
        folder_picker_posix.cpp         # 无 GUI 环境 stub（NotImplemented）
        keychain_posix.cpp
        credential_protection_posix.cpp
        file_open_posix.cpp
        system_theme_posix.cpp
        file_watcher_stub.cpp
        http_client_ca_posix.cpp
        terminal_launch_posix.cpp
        tray_icon_posix.cpp            # 其它 POSIX 兜底：返回空串（用应用图标）
        message_box_posix.cpp          # 无原生阻塞对话框：诚实降级（LOG + Cancel）
        single_instance_posix.cpp      # 暂 no-op 恒 true
        notification_posix.cpp         # 无原生通知后端：诚实 NotImplemented
        autostart_posix.cpp            # 开机自启动兜底：恒不支持（UAT6）
        process_runner_posix.cpp        # 进程执行器：fork/exec + 管道全捕获（UAT6）
        process_host_posix.cpp          # 常驻子进程托管：fork/setsid + 组杀（memcpp S2）
        wallpaper_capability_posix.cpp  # 壁纸宿主能力：无原生实现，恒 false
        # 主窗可见态：非 Win/macOS 桌面复用同一 GLFW 查询实现（只依赖 GLFW C
        # 查询 API；SDL2 后端经 EUI_WINDOW_BACKEND_SDL2 编译期走 fail-open true），
        # 保持 POSIX/UNIX 构建链路可链接。
        window_visibility_macos.cpp
    )
endif()

target_include_directories(devdesk_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
    # cpp-httplib header-only (P0a PoC)
    ${CMAKE_SOURCE_DIR}/3rd/cpp-httplib
)

# Windows 系统库只在 WIN32 链接：它们必须紧跟 CURL::libcurl 之后（GNU ld 左→右解析，
# 早位满足不了 libcurl.a 的 Winsock 符号），但 macOS/Linux 没有这些库，无条件链接即
# CI 链接期炸（ld: library 'ws2_32' not found）。用变量占位保序：WIN32 下展开五个库、
# 其他平台展开为空（CMake 丢弃空 list 项，不产生空 -l）
if(WIN32)
    set(DEVDESK_WIN_SYSTEM_LIBS ws2_32 crypt32 shell32 advapi32 shlwapi)
endif()

target_link_libraries(devdesk_platform
    PUBLIC
        devdesk_core
        CURL::libcurl
        # Windows 系统库紧跟 CURL::libcurl 之后：GNU ld 左→右解析，早位满足不了
        # libcurl.a 的 Winsock 符号——顺序即语义，别把变量占位挪走
        ${DEVDESK_WIN_SYSTEM_LIBS}
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

# 启用 cpp-httplib 的 OpenSSL 支持
target_compile_definitions(devdesk_platform PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
)

# macOS Security.framework (Keychain) + CoreFoundation（APPLE 分支系统深浅检测
# 实现读 NSGlobalDomain AppleInterfaceStyle 用；两者都只在 APPLE 分支链接）
# CoreServices：file_watcher_macos.cpp 的 FSEvents（FSEventStreamCreate 等）
if(APPLE)
    # UNUserNotificationCenter 在 UserNotifications.framework；REQUIRED 让 SDK 缺框架时
    # configure 期直接失败，而不是链接期才报未定义符号。
    find_library(USER_NOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    target_link_libraries(devdesk_platform PUBLIC
        "-framework Security"
        "-framework CoreFoundation"
        "-framework CoreServices"
        ${USER_NOTIFICATIONS_FRAMEWORK}
    )

    # notification_macos.mm 单独开 ARC：只影响该源文件，不给整个 target 打开
    #（其它 .cpp 不受影响；ObjC 内存管理在 ObjC 文件内自洽）。
    set_source_files_properties(notification_macos.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
endif()

# Windows 系统库：Winsock2（网络）+ Crypt32（TLS 证书链）+ Shell32（file_open_win.cpp
# CommandLineToArgvW）+ Advapi32（system_theme_win.cpp RegGetValueW，spec 缩水 A）
# + Shlwapi（folder_picker_win.cpp 路径解析）。统一经上方 WIN32 变量占位
# ${DEVDESK_WIN_SYSTEM_LIBS} 排在 CURL::libcurl 之后；早位声明会被 CMake 去重保首位
# ——别在别处再声明 Windows 系统库
# DevDesk 自己的警告等级（-Werror 只作用于 DevDesk target，不泄漏给第三方）
devdesk_apply_warnings(devdesk_platform)