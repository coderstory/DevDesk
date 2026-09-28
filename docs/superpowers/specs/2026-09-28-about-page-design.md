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

CMakeLists 顶部读 VERSION → `project(DevDesk VERSION ${...})` → CMake 设置 `PROJECT_VERSION`。
`file(READ)` 后需 `string(STRIP)` 去 CRLF。

```cmake
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" DEVDESK_VERSION)
    string(STRIP "${DEVDESK_VERSION}" DEVDESK_VERSION)
else()
    set(DEVDESK_VERSION "0.1.0")
    file(WRITE "${CMAKE_SOURCE_DIR}/VERSION" "${DEVDESK_VERSION}")
endif()
project(DevDesk VERSION ${DEVDESK_VERSION} LANGUAGES C CXX)
# 告诉 CMake：VERSION 文件变了就重新 configure
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
```

**自动递增：`add_custom_target(bump_version ALL ...)`**（不用 PRE_BUILD——Ninja 下 PRE_BUILD = PRE_LINK，仅目标重链时跑。`ALL` 保证每次 `cmake --build` 都跑）。VERSION 经 `CMAKE_CONFIGURE_DEPENDS` 注册后，bump 写入下一 configure 拾取——本次 build 显示当前版本、下个 build 得到 +1 后的版本。

`cmake/bump-version.cmake`（**自己读 VERSION → 自增 → 写回，不依赖外部 CURRENT_VERSION 参数**）：
```cmake
file(READ "${VERSION_FILE}" VER)
string(STRIP "${VER}" VER)
string(REPLACE "." ";" V "${VER}")
list(GET V 0 X); list(GET V 1 Y); list(GET V 2 Z)
math(EXPR Z "${Z} + 1")
if(Z GREATER 9)
    set(Z 0); math(EXPR Y "${Y} + 1")
endif()
if(Y GREATER 9)
    set(Y 0); math(EXPR X "${X} + 1")
endif()
if(X GREATER 9)
    set(X 9); set(Y 9); set(Z 9)  # 饱和 9.9.9，不退回 9.0.0
endif()
set(NEW "${X}.${Y}.${Z}")
file(WRITE "${VERSION_FILE}" "${NEW}")
message(STATUS "Build version: ${VER} -> ${NEW}")
add_custom_target(bump_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D VERSION_FILE="${CMAKE_SOURCE_DIR}/VERSION"
        -P "${CMAKE_SOURCE_DIR}/cmake/bump-version.cmake"
    COMMENT "Bumping build version")
```

### 编译期变量（挂 `devdesk_module_about` target，非 DevDesk exe）

`module_about` 是独立 static lib，exe 的 PRIVATE 定义对它不可见。所有定义写在 `src/module_about/CMakeLists.txt`：
```cmake
target_compile_definitions(devdesk_module_about PRIVATE
    DEVDESK_VERSION="${PROJECT_VERSION}"
    DEVDESK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    DEVDESK_EUI_NEO_REV="${DEVDESK_EUI_NEO_REV}"
    DEVDESK_EUI_NEO_MIN_REV="${DEVDESK_EUI_NEO_MIN_REV}"
    DEVDESK_GIT_REV="${DEVDESK_GIT_REV}"
    DEVDESK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    DEVDESK_PLATFORM="${CMAKE_SYSTEM_NAME}"
    DEVDESK_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)
```
`DEVDESK_EUI_NEO_REV` / `DEVDESK_GIT_REV` 等变量在顶层 CMakeLists 里 `execute_process(git ...)` + `set(...)`，由 `add_subdirectory` 继承到子目录。平台/架构走 CMake 注入（`DEVDESK_PLATFORM` / `DEVDESK_ARCH`），**不用 `#ifdef _WIN32` / `__APPLE__` / `__x86_64__`**——违反项目核心约束 #5（业务代码零平台宏）。`DEVDESK_GIT_REV` 为非 git 环境回退 "unknown" 不 fail configure（D-03）。

about_module.cpp 直接用宏拼字符串：
```cpp
std::string aboutPlatformLine() {
    return std::string(DEVDESK_PLATFORM) + " · " DEVDESK_ARCH;
}
// C++ 标准用 __cplusplus 宏判 "C++" + 两位年
```

### 4. 清理 settings 内旧关于（完整清单，缺一则留僵尸 tab）
- `settings_ui_logic.h:65-67`：`kSettingTabLabels[]` 去 "关于"（`{"通用","模块","日志"}`），`kSettingTabCount` 4→3
- `settings_ui_logic.h:71`：删 `kTabAbout = 3`
- `settings_ui_logic.h:87`：删 `kAboutCardHeight`
- `settings_module.cpp:162-163`：删 kTabAbout 行卡规格（`kCardSpecs` 表 About 条目）
- `settings_ui_cards.cpp:847-925`：删 `drawAboutCard` 函数体
- `settings_ui_cards.h:86`：删 `drawAboutCard` 声明
- 残留注释、test_settings.cpp 的 tab 点击助手用例（`clickTab(kTabAbout)`）同步裁

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

### 卡片底板
复用 `ui_kit::cardFrame(ui, id, x, y, w, h)`（`src/ui_kit/card.h`），自带 `t.colors.surface` 底色 + `components::theme::panelShadow()` 阴影 + 圆角，与设置页同一套视觉语言，无重复样式。

### 打开目录按钮
"打开"按钮用 `openDirectoryButton(ui, id, x, y, path)` 封装——内部调 `core::openDirectory(path)`（`src/platform/file_open.h:36`，返回 `Result<void>`），失败用 LOG_ERROR 记录。按钮样式：outlined+small 近似（`fontSize=13`、`primary=false`），走 EUI-NEO `components::button`（`components/button.h`）。

### 平台/架构检测
平台字符串走 CMake 注入（见编译期变量段），about_module.cpp 只消费宏，**零 `#ifdef`**（项目核心约束 #5）。

## 不变
- `Module` 接口零改动（`pageId/displayName/shortName/iconGlyph/compose` 全现有虚函数满足）
- `Router` 零改动（`registerModule` 现有签名足够）
- `drawSidebar` 零改动（自绘导航会自动拾取 router.modules() 中新增的 AboutModule，显示 iconGlyph + displayName + 当前页高亮）

## 测试
- `tests/test_core/test_router.cpp`：`core_pageid_order` 用例补 `About`（枚举顺序断言，该用例钉死 home=0…settings=4，新增 about=5）
- 新版本文递增 `cmake/bump-version.cmake` 纯字符串逻辑，可写 simple 单元测试（python/cmake script test；或加到 test_about.cpp 作 format 验证）
- 模块隔离守卫：`about_module` 不反向依赖任何其他 module（CMakeLists 不 link 兄弟模块；grep 零 include `module_`）
- 不新增 `test_about.cpp` 测试 compose 渲染（compose 纯 UI + 构建期常量，REGISTER_TEST 无法建 EUI 窗口——probes 工具链尚未就绪；模块级返回值非空断言留待将来 probe 覆盖）

## 设计决策
- D-01：about 模块**不**有独立的 onStart/onStop/storage（纯 compose 页，只读静态信息 + 构建期注入常量）
- D-02：about 模块**自给**版本信息——编译期定义全部挂在 `devdesk_module_about` target（`src/module_about/CMakeLists.txt`），不 include `settings_config.h`、不 link `devdesk_module_settings`。`aboutVersionLine()`/`aboutEuiNeoLine()` 等价逻辑在 about_module.cpp 内用同名宏重新定义（几行复制，换模块隔离）。settings_config.cpp 原有的 about* 函数在 settings About tab 清掉后成死代码，不属本需求范围
- D-03：CMake 的 execute_process(git rev-parse) 在非 git 环境（如源码 zip 下载）返回 "unknown"，不 fail configure
- D-04：about 页不滚动。默认窗口 1600×1000 内全部可见（mockup 估算 ~750px 高含间距）。窄窗（<900px）底部卡溢出被裁切——min window 1200×720 和可视区域不够放全量内容，后续视需要加 scrollView。**本 spec 目标 = 默认窗口完整可见**