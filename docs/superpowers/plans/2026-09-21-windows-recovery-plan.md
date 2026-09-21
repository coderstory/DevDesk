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
- 实测（2026-09-21 验收）：103 例 = **84 过 / 19 挂**；19 个失败全部为 keychain DPAPI stub（`NotImplemented`）连锁——keychain 2 + claude_service 10 + proxy_router 7，待 T7 实现 DPAPI 后转绿（原预期"仅剩 2 个"偏低，连锁波及依赖 keychain 的 service/router 用例）
- 结果记录进 KNOWN_LIMITATIONS（T12 前先记在 CLAUDE.md）

#### T5 P10a 流式修复 ✅
- 已知 bug：`sendStreamingClaude forwards upstream sse chunks` 和 `backpressure preserves all chunks under load` 报 libcurl 错误 18 `Transferred a partial file`（暂停/恢复路径丢传输尾部，或 mock 上游 Content-Length 时序差异）
- 诊断路径：先最小复现（mock 上游 + curl 暂停），区分「上游提前断」vs「恢复时序丢尾」
- 验收：14 个 streaming 测试全绿（Windows + 逻辑层面）

### 阶段三：平台架构 + P14 完成

#### T6 接口隔离重构 ⬜
- keychain.cpp 拆 `keychain_win/macos/posix.cpp`；paths.cpp 同样拆
- 新增 `core/time.h` + `time_posix.cpp`（localtime_r，macOS/Linux 共用）+ `time_win.cpp`（localtime_s），替换 3 处散布的 #ifdef（logging/todo_module/claude_module）
- `main.cpp` argv 捕获抽 per-platform 文件（Windows 直接读 argv）
- CMake `target_sources` 按平台选择
- 验收：`grep -rn '_WIN32\|__APPLE__' src/` 零命中（CMakeLists 除外）；全平台逻辑等价（现有测试全绿）

#### T7 DPAPI keychain ⬜
- 在 T6 拆好的 `keychain_win.cpp` 实现 `CryptProtectData/CryptUnprotectData`
- 验收：19 个连锁失败测试全转绿（keychain 2 + claude_service 10 + proxy_router 7）

#### T8 P10b 流式质量 ⬜
- 5 分钟长连接回归 + 10MB 大 body 压测 + 异常断连恢复
- 验收：压测不丢块不挂起不泄漏（内存采样前后对比）

#### T9 收尾杂项 ⬜
- `ClaudeService::global()` 启动顺序 WARN 理顺（onStart 时不再为 null）
- `proxy_router.cpp` `#if 0` 死代码清理
- Home 快捷按钮接线检查（当前 mock）

### 阶段四：剩余需求

#### T10 P12 Home 真实数据 ⬜
- 4 张统计卡接真数据（今日笔记/待办/逾期/API 调用）+ 最近编辑列表 + 30 天日志清理任务

#### T11 P13 设置页 ⬜
- 主题切换 / 目录配置 / token 管理 UI / 字号 / 日志保留策略

#### T12 P15 文档 ⬜
- USER_GUIDE + KNOWN_LIMITATIONS（Windows 边界如实记录）+ README + CHANGELOG

## 贯穿纪律

1. 每个 T 完成即 commit（用户要求及时 commit）
2. 每个 T 动手前出变更清单，仅清单内修改
3. 发现缺陷只记录不顺手修（除非属于当前 T 范围）
4. EUI-NEO 的问题走上游 PR，不在 DevDesk hack
5. 每 T 验收标准如上，未达验收不算完成

## 关键路径

T1 → T2 → T3 → T5 → T6 → T7 → T8/T9 → T10 → T11 → T12
