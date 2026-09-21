# Windows 恢复开发计划（2026-09-21）

> 背景：项目从 macOS 迁移到 Windows 11，macOS 上被强行终止的 P9/P10a 开发现场已收编。
> 本计划覆盖从当前状态到「单 exe 发布 + 全部需求完成」的全部任务。
> 状态标记：⬜ 未开始 / 🔄 进行中 / ✅ 完成。

## 已完成的前置工作（2026-09-21）

- ✅ 状态分析：构建/测试/平台差距/未提交代码四个维度全部摸清
- ✅ Windows 编译通过（workaround flags）+ DevDesk.exe 启动验证（四模块、DB 迁移、代理 8888、干净退出）
- ✅ CLAUDE.md 双份更新（平台现状、缺口表、工具链路径）
- ✅ 新增架构约定：平台适配代码接口隔离（见 DevDesk/CLAUDE.md 核心约束第 5 条）

## 任务清单

### 阶段一：止血收编

#### T1 三连 commit ✅
- A `feat(claude): 收编 macOS 中断现场 P9/P10a`——src/** 全部修改 + streaming_proxy.* + 3 个新测试文件 + tests/CMakeLists.txt + doctest.h
- B `fix(tests): Windows 文件锁崩溃 + test_main 容错`——test_migrations/sqlite/logging/todo_service/test_main
- C `docs: 平台迁移现状 + 缺口清单 + 本计划`——DevDesk/CLAUDE.md + 本文件
- 附：spec §16 增补（P14 升必选 + 单 exe 要求）
- 验收：git log 三条新 commit，工作区干净

#### T2 干净编译 5 处修复 ✅
- `src/platform/json.h` 加 `#include <cstdint>`
- `src/platform/CMakeLists.txt` 补 `if(WIN32) ws2_32 crypt32`
- migrations.h designated initializers：顶层 `CMAKE_CXX_STANDARD` 升 20（1 行）
- `src/module_claude_proxy/proxy_router.cpp:81` `const std::string&` 绑 `const char*` 临时 → 改 `const char* const`
- `-Werror` 收窄：只对 DevDesk 自己 target 生效，不压 EUI-NEO 子目录
- FTS5 检测修复：`check_cxx_source_compiles` 补 SQLite3 库链接
- 验收：删 build/ 全新 configure（零 workaround flag）+ 全量构建过 + 测试套件跑到尾

### 阶段二：发布形态 + 基线

#### T3 静态链接单 exe ✅
- CMake 指到静态库：`SQLite3_LIBRARY=libsqlite3.a`、`CURL_LIBRARY=libcurl.a`、`OPENSSL_USE_STATIC_LIBS=TRUE`、zlib 静态
- libcurl 传递依赖按链接器报错补齐（MSYS2 均有静态版）
- 验收：`objdump -p DevDesk.exe | grep "DLL Name"` 零外部 DLL；拷贝到无 MSYS2 PATH 的干净目录 + fonts/assets 能跑；Release 体积记录在案
- 边界：v1 接受 exe 旁带 fonts/assets 目录；资源嵌入（windres）留待需要时

#### T4 Windows 测试基线 ✅
- 初次实测（2026-09-21 验收）：103 例 = **84 过 / 19 挂**；19 个失败全部为 keychain DPAPI stub（`NotImplemented`）连锁——keychain 2 + claude_service 10 + proxy_router 7（原预期"仅剩 2 个"偏低，连锁波及依赖 keychain 的 service/router 用例）
- **最终数字（2026-09-22 W2 验收）：103 例全绿（103 过 / 0 挂）**——T7 DPAPI 落地后 19 个连锁失败全部转绿；加 T8 新增 6 例压测，全量 109 例全过
- 结果记录进 KNOWN_LIMITATIONS（T12 前先记在 CLAUDE.md）

#### T5 P10a 流式修复 ✅
- 已知 bug：`sendStreamingClaude forwards upstream sse chunks` 和 `backpressure preserves all chunks under load` 报 libcurl 错误 18 `Transferred a partial file`（暂停/恢复路径丢传输尾部，或 mock 上游 Content-Length 时序差异）
- 诊断路径：先最小复现（mock 上游 + curl 暂停），区分「上游提前断」vs「恢复时序丢尾」
- 验收：14 个 streaming 测试全绿（Windows + 逻辑层面）

### 阶段三：平台架构 + P14 完成

#### T6 接口隔离重构 ✅
- keychain.cpp 拆 `keychain_win/macos/posix.cpp`；paths.cpp 同样拆
- 新增 `core/time.h` + `time_posix.cpp`（localtime_r，macOS/Linux 共用）+ `time_win.cpp`（localtime_s），替换 3 处散布的 #ifdef（logging/todo_module/claude_module）
- `main.cpp` argv 捕获抽 per-platform 文件（Windows 直接读 argv）；keychain/paths/time/file_open 均新增 `*_posix.cpp` 兜底，保证 Linux else 分支可链接
- CMake `target_sources` 按平台选择
- 实施备注（2026-09-22）：
  - 接口名定 `core::time::localTime(std::time_t, std::tm*)`——3 处调用方都是转换任意时间戳而非"当前时间"，故不用 localNow
  - `src/core/time.h` 与系统 `<time.h>` 重名：`-I` 目录优先于系统目录，实测 MinGW 下 `<mutex>→<ctime>` 链条会被劫持导致编译爆炸。已同步移除无人使用的 `-I src/core`（src/core/CMakeLists.txt PUBLIC 与顶层 DevDesk PRIVATE 两处；core 内部全是同目录引号 include，外部全走 `-I src` 的 `core/xxx.h`，无消费者受影响）
  - 遗留（范围外记录）：`file_open_win.cpp` 用 ANSI `__argv`，非 ASCII 文件路径可能乱码，完整做法 GetCommandLineW + CommandLineToArgvW 转 UTF-8，留后续
- 验收：`grep -rn '_WIN32\|__APPLE__\|_WIN64' src/`（*.cpp/*.h）零命中；构建过；103 例 84 过 19 挂（keychain 2 + claude_service 10 + proxy_router 7，与 T4 基线一致，纯重构零行为变化）

#### T7 DPAPI keychain ✅
- 在 T6 拆好的 `keychain_win.cpp` 实现 `CryptProtectData/CryptUnprotectData`（+ 文件存储，commit a534f5d）
- 实测（2026-09-22 W2 验收）：19 个连锁失败全转绿——keychain 2 + claude_service 10 + proxy_router 7 全部 PASS，全量 109 例全过（103 基线 + T8 新增 6 压测），0 失败

#### T8 P10b 流式质量 ✅
- 新增 `tests/test_modules/test_streaming_stress.cpp` 6 个压测用例（commit 458cf50）：10MB 流式 e2e / 10MB 缓冲路径 / soak 60s / soak 300s（`DEVDESK_SOAK=1` 门控）/ 上游断连恢复 / 内存采样
- 实测（2026-09-22 W2 验收，全部 PASS）：
  - 10MB 流式 e2e：44ms、1440 次 receiver 调用，全串比对 + FNV-1a 哈希 + 160 块块号三重校验一致
  - 10MB 缓冲路径：48ms，字节一致
  - soak 60s：收到 1200/1200 chunk（上游发 1200），误差 0%（验收口径 < 5%），seq 严格递增
  - soak 300s：未设 `DEVDESK_SOAK` 时打印 `[SKIP]` 直接通过（常规套件不背 5 分钟）
  - 断连恢复：上游发到一半 closesocket，流以 End 干净收尾（非 Error），已收数据与上游已发字节完全一致（直接层 + 经代理端到端两场景）
  - 内存采样：20 轮 × 10MB 流式，内存增量 12,660 KB（阈值 102,400 KB），无泄漏趋势

#### T9 收尾杂项 ✅（2026-09-22，commit 50ff663）
- `ClaudeService::global()` 启动顺序 WARN 理顺：`startClaudeProxy()` 挪到 `getRouter()` 之前，模块 onStart 拿到非 null global()；启动冒烟日志零 WARN、"Claude 代理已启动"仅一次（代理只由 main 启动，onStart 仅兜底检查）
- `proxy_router.cpp` `#if 0` 死代码清理：getHeader 块删除，grep 全仓库零命中
- Home 快捷按钮接线：并入 T10 一并落地（降级为导航，跨模块禁依赖，见 T10）
- 验收：ctest 109/109 全过

### 阶段四：剩余需求

#### T10 P12 Home 真实数据 ✅（2026-09-22，commit 883d681）
- 新增 HomeService：只读聚合 markdown_index/todo_tasks/claude_call_logs——今日笔记/待办/逾期/今日 API 调用/最近编辑前 5；4 张统计卡 + 最近编辑列表接真数据，卡片/整行可点击跳转
- 快捷按钮接线（自 T9 移入）：新建笔记/新建任务/切换 Profile → 对应模块（跨模块禁依赖，降级为导航，范围外项已记 KNOWN_LIMITATIONS）
- `Database::pruneCallLogs(retentionDays)`：启动时滚动清理调用日志（幂等，失败仅 WARN 不阻断启动；后经 750b7fa 集成接线改读设置页 logRetentionDays，0=永久跳过清理）
- `core::Router` 增加 registerAppRouter/appRouter()（镜像 registerAppDatabase 模式）
- 验收：测试 109→111 全过；应用冒烟 Home 日志输出真数据行，种子库验证今日边界/逾期口径/清理行为，无新 ERROR

#### T11 P13 设置页 ✅（2026-09-22，commit a18c8a8 + 1f273f9）
- 新增 module_settings：主题切换（下一帧全页生效）/ 字号缩放档位（85–130%，正文基准 15px）/ 代理 token 只读显示 + 重新生成（同端口 stop/start 重启 + 同步重写 proxy_token.txt）/ Markdown 目录只读展示 / 日志保留天数（0–3650，0=永久）
- SettingsStore 读写 config.json（core::paths::configFile() + platform::json；文件缺失/损坏/字段缺失回退默认值，数值钳制）；前置 1f273f9 实装 platform::json（yyjson 解析 + 自写 RFC 8259 序列化，修复 parse 桩与 stringify 恒输出 "{}" 桩）
- 入口：应用无侧边栏（导航接线是 T9 遗留），main.cpp compose 顶层浮动齿轮按钮（设置页内变返回箭头）桥接
- 已知限制如实登记：字号缩放仅作用于设置页自身（core::theme SizeTokens 无运行时缩放入口，配置已持久化待全局接线，见 KNOWN_LIMITATIONS）
- 验收：ctest 122/122（109 基线 + 10 settings + 3 json，零回归）；启动冒烟两轮（默认 Dark、预设 Light/115%/90 天配置均正确加载，日志自证），零 ERROR/FATAL

#### T12 P15 文档 ✅（2026-09-22，本提交）
- docs/USER_GUIDE.md：五模块用法 / Claude 代理配置流程（127.0.0.1:8888 + %APPDATA%/DevDesk/proxy_token.txt Bearer 鉴权）/ 数据位置一览 / 主题·字号·token 再生成·日志保留设置法
- docs/KNOWN_LIMITATIONS.md：Windows 4 项 + 跨平台 12 项如实记录；另收录审计源码新发现——Claude 切换器操作按钮未接线（该页面 0 个 onClick，数据真实但按钮为展示态，服务层 CRUD 完整）
- README.md：项目简介 / 三分钟上手（裸 cmake configure，工具链见 CLAUDE.md）/ 测试跑法（ctest + DEVDESK_SOAK 门控 + 按名过滤 + --log）/ 架构一图流（4 层 + 平台接口隔离）/ 文档索引
- CHANGELOG.md：a1a106a → HEAD 按波（W1/W2/W3）分组的条目记录
- docs/MAINTENANCE-EVALUATION.md：P16 长期维护评估（EUI-NEO Metal 后端 vs OpenGL、Markdown 编辑器自实现 vs input 组件——均为"现在不动"+触发条件，YAGNI 基调）

## 贯穿纪律

1. 每个 T 完成即 commit（用户要求及时 commit）
2. 每个 T 动手前出变更清单，仅清单内修改
3. 发现缺陷只记录不顺手修（除非属于当前 T 范围）
4. EUI-NEO 的问题走上游 PR，不在 DevDesk hack
5. 每 T 验收标准如上，未达验收不算完成

## 关键路径

T1 → T2 → T3 → T5 → T6 → T7 → T8/T9 → T10 → T11 → T12

## 全部完成（2026-09-22 收官）

T1–T12 全部 ✅。最终基线：**124/124 测试全绿**（109 基线 + 2 Home + 10 Settings + 3 JSON），工作区干净，全部成果已 commit。

关键成果清单：

- **发布形态**：单 exe 零外部 DLL——MinGW 全静态链接（sqlite3/curl/ssl/crypto/C++ 运行时），objdump 仅 16 个 Windows 系统 DLL（Debug+Release 双验证）；Release 15.4 MiB（16,152,064 字节）；裸 cmake configure 零 workaround flag
- **平台架构**：keychain/paths/time/file_open 四族平台 API 接口隔离（`*_win/*_macos/*_posix.cpp`，CMake 按平台选择编译），业务代码零 `#ifdef _WIN32`/`__APPLE__`（grep src/ *.cpp/*.h 零命中）
- **Windows keychain**：DPAPI（CryptProtectData/CryptUnprotectData）+ 文件存储，T4 记录的 19 个连锁失败（keychain 2 + claude_service 10 + proxy_router 7）全转绿
- **流式代理质量（P10b）**：10MB 端到端流式吞吐实测约 240–320 MB/s（e2e 44ms、1440 chunk、全串比对 + FNV-1a + 块号三重校验一致）；soak 60s 1200/1200（0% 丢失，seq 严格递增）；300s soak 由 DEVDESK_SOAK=1 门控；断连恢复以 End 干净收尾且字节一致；20 轮 × 10MB 内存采样增量 12,660 KB（阈值 102,400 KB）无泄漏趋势
- **功能补全**：P12 Home 真实数据仪表盘（统计卡/最近编辑/快捷按钮/调用日志滚动清理）、P13 设置页（主题/字号/token 再生成/日志保留，config.json 持久化 + 数值钳制）、platform::json 实装（yyjson 解析 + 自写序列化）、启动顺序理顺零 WARN
- **文档（P15/P16）**：USER_GUIDE / KNOWN_LIMITATIONS（Windows 4 + 跨平台 12 如实记录）/ README / CHANGELOG / MAINTENANCE-EVALUATION
- **macOS 侧**：分支代码保留但自迁移后未重新构建验证（P0-P8 曾 66 测试通过；回 macOS 开发时先全量构建 + 测试，见 KNOWN_LIMITATIONS §跨平台-9）

## 终验通过（2026-09-22，恢复计划关闭）

全项目终验（fresh eyes，只验证与收尾，未改业务代码）：

- **提交链**：a1a106a → a0043c5 共 24 个提交完整可读，工作区干净
- **构建 + 测试**：README 的裸 cmake configure 命令原样可跑（干净目录验证）；Debug 与 Release 双构建 ctest 均 **124/124 全绿**（各实测约 85s）
- **依赖**：`objdump -p` Debug 与 Release 复验均仅 **16 个 Windows 系统 DLL**，零外部依赖
- **启动冒烟**：进程存活 8s+；日志零 WARN/ERROR、"Claude 代理已启动"恰 1 次、五模块齐启（含设置）；Home 仪表盘经 HomeService 真实 SQL 聚合（markdown_index / todo_tasks / claude_call_logs）
- **文档交叉核对**：USER_GUIDE §5 六条路径与 paths_win.cpp / paths.cpp / keychain_win.cpp 实现及 %APPDATA% 磁盘实际目录一一对应；KNOWN_LIMITATIONS 抽查 5 条（ANSI argv / keychain 幂等删留空目录 / fonts+assets 旁挂 / Claude 页 0 个 onClick / soak SKIP 门控）全部属实；README 测试过滤命令原样执行通过（`stress` 子串过滤 6 例全过）
- **一处数字更正**：Release exe 终验实测 **16,402,944 字节（15.6 MiB）**——上文 15.4 MiB（16,152,064 字节）为 T3/W2 验收时点数字，W3 新增 Settings/Home 代码后增大约 250 KB（+1.5%）；README 与 KNOWN_LIMITATIONS 已同步更正为最新实测，CHANGELOG T3 历史条目保留原值
