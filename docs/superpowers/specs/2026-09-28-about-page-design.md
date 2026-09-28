# 关于页面设计

## 概述

把"关于"从 settings 第 4 tab 拆成独立侧边栏模块（`module_about`，`PageId::About`），重新设计 UI。
侧边栏图标：fa-circle-info `\xF05A`。

## 改动面

### 1. PageId 加 About（module_interface.h:14）
```cpp
enum class PageId {
    Home,
    Markdown,
    Todo,
    ClaudeProxy,
    Settings,
    About,      // NEW
};
// pageIdToString 同步补 "About"
```

### 2. 新模块 src/module_about/
- `about_module.cpp`：实现 Module（~250 行 compose）
- `CMakeLists.txt`：`add_library(devdesk_module_about STATIC about_module.cpp)`
- 顶层 CMakeLists：`add_subdirectory(src/module_about)` + link `devdesk_module_about`

## 构建期信息注入

### 版本号（自动递增）

`DevDesk/VERSION` 文件存版本号 `x.y.z`（每值 0-9，满 10 进一，如 0.1.9 → 0.2.0、0.9.9 → 1.0.0）。
VERSION **不入 git**（加到 .gitignore），每机器各自递增。首次 configure 缺则自动创建 "0.1.0"。

CMakeLists 顶部读 VERSION → `project(DevDesk VERSION ${...})` → CMake 自动生成 `DEVDESK_VERSION` 宏。
PRE_BUILD 自定义步骤调 `cmake/bump-version.cmake` 把当前版本 +1 写回 VERSION（**本 build 版本不变，供下次 build 用**）。

`cmake/bump-version.cmake`：
```
string(REPLACE "." ";" V "${CURRENT_VERSION}")
list(GET V 0 X); list(GET V 1 Y); list(GET V 2 Z)
math(EXPR Z "${Z} + 1")
if(Z GREATER 9) set(Z 0); math(EXPR Y "${Y} + 1") endif()
if(Y GREATER 9) set(Y 0); math(EXPR X "${X} + 1") endif()
if(X GREATER 9) set(X 9) endif()
set(NEW "${X}.${Y}.${Z}")
file(WRITE "${VERSION_FILE}" "${NEW}")
```

### 其他编译期变量（CMakeLists.txt 加）
```cmake
# DevDesk 自身 git rev（configure 期快照，与 EUI-NEO 版本诊断同款）
execute_process(COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE DEVDESK_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT DEVDESK_GIT_REV)
    set(DEVDESK_GIT_REV "unknown")
endif()
target_compile_definitions(DevDesk PRIVATE DEVDESK_GIT_REV="${DEVDESK_GIT_REV}")
target_compile_definitions(DevDesk PRIVATE DEVDESK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
target_compile_definitions(DevDesk PRIVATE DEVDESK_CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
```
已有定义（settings_config.cpp 消费）：
- `DEVDESK_VERSION`（project VERSION）
- `DEVDESK_BUILD_TYPE`
- `DEVDESK_EUI_NEO_REV`
- `DEVDESK_EUI_NEO_MIN_REV`
- `__DATE__` `__TIME__`（编译时间戳）

about 页新增消费：
- `DEVDESK_GIT_REV`
- `DEVDESK_COMPILER`（如 "GNU 16.2.0"）

### 4. 清理 settings 内旧关于
- `settings_ui_logic.h:71`：删 `kTabAbout = 3`
- `settings_ui_cards.cpp:847-925`：删 `drawAboutCard`
- `settings_module.cpp:162-163`：删 kTabAbout 行卡规格（`kCardSpecs` 表的 About 条目）
- `settings_ui_logic.h:87`：删 `kAboutCardHeight`

### 5. app_assembly.cpp
`assembleAndStart` 加 `router.registerModule(new module::about::AboutModule())`（约 line 40+）

## About 页面 UI 设计

### 布局概览（8px 网格，居中，主内容区 720px 宽）

```
═══════════════════════════════════════════
           [fa-laptop-code]  48px primary
              DevDesk        32px title
           v0.1.0 · Debug    15px secondary
       构建 Sep 28 2026 16:45 13px tertiary
         commit b095fef · GCC 16.2.0
                  ↑ hero 段，垂直居中，spacingLarge 间距

  ┌─────────────────────────────────────┐
  │ 📱 应用信息                          │  ← info-card（3 行 key-value）
  │  版本       v0.1.0 Debug             │
  │  Git commit b095fef                  │
  │  构建时间   Sep 28 2026 16:45        │
  │  编译器     GNU 16.2.0               │
  │  平台       Windows 11 · x64         │
  │  C++ 标准   C++20                    │
  └─────────────────────────────────────┘

  ┌─────────────────────────────────────┐
  │ ⚙️ 依赖与框架                         │
  │  GUI 框架   EUI-NEO (OpenGL)         │
  │  EUI-NEO    rev 7164d55 (min 7164d55)│
  │  数据库     SQLite 3 + FTS5          │
  │  HTTP       libcurl (静态链接)        │
  │  TLS        OpenSSL 3.x (静态链接)    │
  │  字体       FreeType + FontAwesome 7 │
  └─────────────────────────────────────┘

  ┌─────────────────────────────────────┐
  │ 📂 数据                               │
  │  配置文件   <config path>    [打开]   │
  │  日志文件   <log path>       [打开]   │
  │  数据目录   <pets dir>       [打开]   │
  └─────────────────────────────────────┘
                 © 2026 DevDesk
```

### 卡片组件
`aboutCard(ui, id, title, rows)` — 可复用 helper，每个 card 内是 key-value 行（label 13px secondary 左 + value 13px primary 右，行高 24px）。最后一行可含按钮（如"打开"）。

### 布局计算
```cpp
const float kAboutContentW = 720.0f;    // 主内容区宽
const float cardW = kAboutContentW;      // 卡片宽 = 内容区
const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f); // 居中
// 垂直从 y=24 开始排，卡片间距 8px
// 卡片内：padding 20px, 标题行 28px + 各行 24px * N + 底部 12px
```

### 颜色走 theme token
- 卡片底：`t.colors.surface` + `panelShadow`
- hero icon：`t.colors.primary`
- 标题：`t.colors.textPrimary` fontSizePage(32px) fontWeight(700)
- 副标题：`t.colors.textSecondary` fontSizeBody(15px)
- label：`t.colors.textTertiary` fontSizeSmall(13px)
- value：`t.colors.textPrimary` fontSizeSmall(13px)
- 灰色分割线：`t.colors.borderMuted`
- 短 hash/id：`t.colors.primary`（commit SHA、rev 等代码标识符）

### 键按钮（"打开"配置/日志目录）
用 `components::button` 或 `ButtonStyle::Outline + Small`，theme primary 色。点击调 `core::paths::openDirectory()`。

### 平台 + 架构检测（CMake 注入）
```cpp
// about_module.cpp 内或 config 内定义
#ifdef _WIN32
    const char* kPlatform = "Windows";
#elif __APPLE__
    const char* kPlatform = "macOS";
#else
    const char* kPlatform = "Linux";
#endif
#if defined(__x86_64__) || defined(_M_AMD64)
    const char* kArch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* kArch = "ARM64";
#endif
// C++ 标准用 __cplusplus >= 202002L 判 "C++20"
```

## 不变
- `Module` 接口零改动（`pageId/displayName/shortName/iconGlyph/compose` 全现有虚函数满足）
- `Router` 零改动（`registerModule` 现有签名足够）
- `drawSidebar` 零改动（自绘导航会自动拾取 router.modules() 中新增的 AboutModule，显示 iconGlyph + displayName + 当前页高亮）
- `discoverSkins` 只读皮肤目录，about 页读它 getSkinCount 或直接列名即可

## 测试
- `test_app_assembly.cpp`：`core_pageid_order` 用例补 `About`（枚举顺序断言）
- 新增 `tests/test_modules/test_about.cpp`：验证 AboutModule pageId/displayName/shortName/iconGlyph 返回值非空且合法，验证 aboutVersionLine 非空、aboutGitRev 非空
- 模块隔离守卫：`about_module` 不反向依赖任何其他 module（CMakeLists 不 link 兄弟模块；grep 零 include `module_`）

## 设计决策
- D-01：about 模块**不**有独立的 onStart/onStop/storage（纯 compose 页，只读静态信息 + 构建期注入常量）
- D-02：版本/编译信息沿用 settings_config.cpp 的 aboutVersionLine/aboutEuiNeoLine 函数签名，about 模块直接 include settings_config.h（settings 是 devdesk_module_settings lib，about 可 link 它；若不希望 link 是为了兄弟模块隔离，就把 about* 函数迁到 core/about_info.cpp，两边 include）。**建议**：迁到 `src/core/about_info.cpp/h`（core 层），settings 和 about 各自 include。保持模块隔离——不新增 module_about ↔ module_settings 依赖。
- D-03：CMake 的 execute_process(git rev-parse) 在非 git 环境（如源码 zip 下载）返回 "unknown"，不 fail configure
- D-04：about 页不滚动（内容固定 ≤ 一屏，fit 1600×1000 窗口；若窗口 < 900px 高则内容溢出不处理，min window 1200×720 下应全部可见）