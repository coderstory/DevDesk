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
        # -Werror 不按构建档分支，全档统一开启（2026-09-28，PAB-10）：
        # 旧写法只在 Debug 档加 -Werror，Release 档（含顶层
        # `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Release)` 兜底出来的默认档，
        # 以及 RelWithDebInfo / MinSizeRel）只有 -Wall -Wextra → 只有 -O2/-O3 才冒的告警
        # （-Wmaybe-uninitialized / -Wstringop-* / strict-aliasing 等）永远不会失败。
        # 不按档判定还避开了两条静默失效：多配置生成器（Xcode/VS）在 configure 期
        # CMAKE_BUILD_TYPE 为空，按档分支会让整个 -Werror 从未命中。
        # 误报一律走文末 -Wno-error= 白名单单独压，不要恢复按档分支。
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        # 顺序在后才能覆盖 -Wall/-Wextra 触发的该类警告
        target_compile_options(${target_name} PRIVATE -Wno-unused-parameter)
        # EUI-NEO 头文件（theme.h/markdown.h 的聚合初始化）会触发该警告，
        # 属第三方代码：降级为非致命，DevDesk 自己的新增告警仍可见
        target_compile_options(${target_name} PRIVATE -Wno-error=missing-field-initializers)
        # 以下四条是 Release 档（-O3）才出现的 GCC 误报，源头都不在 DevDesk 代码里，
        # 只降级为非致命（仍打印、不静默），理由逐条写死；且只对 GNU 生效——
        # 这些告警名是 GCC 专属（Clang 不认 `-Wstringop-overflow` 等，会以
        # -Wunknown-warning-option 反噬到无条件开启的 -Werror 上，把 macOS 构建打死）。
        # ① -Warray-bounds：3rd/cpp-httplib/httplib.h 的 SSLClient 析构 + 内联进来的
        #    libstdc++ 析构，GCC 把 `new httplib::ClientImpl`（976 字节）当成同尺寸数组后
        #    再按 SSLClient 基类偏移（984/1000）判越界——devirtualization 误报，
        #    实测只出现在 tests/test_streaming_proxy.cpp:1138 一个构造/析构点上。
        # ② -Wrestrict / -Wstringop-overflow：libstdc++ stl_uninitialized.h:294 的
        #    memcpy 被内联进 EUI-NEO components/styledmenu.h 的 flattenRecursive
        #    （std::vector<int> 拷贝构造），GCC 报出 9223372036854775804 / 9223372036854775808
        #    字节（SIZE_MAX 量级）的尺寸——优化器丢真尺寸后的模板化数值，非真实越界。
        # ③ -Walloc-size-larger-than：同一条 flattenRecursive 内联链的另一面（GCC 把
        #    vector 尺寸当成了 SIZE_MAX），在 main.cpp 上冒出来；该选项带参数，故按
        #    GCC 要求的 `-Wno-error=选项=参数` 形式给出（参数取默认上限 PTRDIFF_MAX）。
        # ④ -Wconversion-null：httplib.h:19037 CertOpenSystemStoreW(NULL, store_name)——
        #    上游把 NULL 传给 HCRYPTPROV_LEGACY（整数形参），MinGW 的 NULL 定义触发
        #    整数转换告警，叠全仓 -Werror 即编译红；MSVC 的 NULL 是 0 不报。与前三条
        #    同一性质（MinGW 特有告警豁免），比 patch vendored 头干净（升级 0.59 时
        #    vendor 文件保持原样）。
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target_name} PRIVATE
                -Wno-error=array-bounds
                -Wno-error=restrict
                -Wno-error=stringop-overflow
                -Wno-error=alloc-size-larger-than=9223372036854775807
                -Wno-error=conversion-null)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target_name} PRIVATE /W4)
    endif()
endfunction()
