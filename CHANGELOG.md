# CHANGELOG

本文件记录 **a1a106a（macOS 中断现场收编）起** 的条目式变更，按恢复计划的三波（W1 止血收编 / W2 平台架构 / W3 剩余需求）组织，波内按 feat / fix / test / docs 分组。更早的 macOS 主线历史（P0–P8，66 测试通过）见 git log 与 spec §16。

## [W8] 2026-09-22 —— Todo 卡片拖拽真 drop 语义 + 拖动视觉反馈

基线：**178/178 测试全绿**（177 基线 + 1 新增纯函数用例；1 个既有拖拽用例改写为新语义）。

### feat

- **Todo 卡片拖拽重构为真 drop 语义**：旧实现 onDrag 阈值（横向拖过 60% 列宽）即落库，一次拖拽可连环触发多列移动且无中间态。新实现：`onPress` 记锚点（按下时卡片 bounds + 源列/卡序）→ `onDrag` 只更新悬停列（视觉反馈）→ `onRelease` 按指针落点一次性落库（`computeAppendPosition` 追加到目标列尾）。落点换算是纯函数 `ui::dragDropTarget`（todo_ui_logic.h）：以按下卡片 bounds 为锚，dpi 用 `bounds.width / compose 卡宽` 探测、板面原点用卡片左缘回推，同时消掉指针像素空间与 compose 空间的缩放和偏移差；列内滚动偏移一并还原（命中框跟随滚动 transform）。落点在列间隙 / 板外 / Cancel / 源列 → 取消，卡片回原位。顺带消灭旧缺陷：落库只依赖稳定 taskId，不再使用 reloadAll 后会过期的列/卡索引。
- **拖动视觉中间态**：被拖卡片整体半透明（opacity 0.45，含优先级条 / 标题 / chip / 打勾）；指针悬停列边框高亮（2px primary）。拖拽松手回落在本卡上不误开详情弹窗（onClick 抑制，运行时 onClick 先于 onRelease 派发）。
- **删除死代码 `core::drag::DragController`**：审计确认无任何调用方；其"平行拖拽框架"需要应用层指针接线，而 EUI-NEO 元素级 `onPress`/`onDrag`/`onRelease` 回调已直接提供同等数据，无法复用，删除 .h/.cpp 与 CMake 条目。

### test

- **`todo_ui_drag_hit_pure`（新增）**：`dragDropTarget` 纯函数——命中/列间隙/板外/板上下方/HiDPI（dpi=2 全坐标翻倍）/列内滚动偏移/退化输入。
- **`todo_ui_move_appends_to_end`（改写）**：真实 layout 帧驱动完整拖拽序列（press → drag → release），断言中间态（被拖卡 opacity 0.45 + 目标列 border 2px + 源列不亮）、松手落库追加列尾、拖出板外取消、落回源列取消、拖拽松手不开详情弹窗（dialog 常驻 compose，断言 panel opacity）。

## [W5] 2026-09-22 —— 遗留项清零（三链并行 + 集成）

最终基线：**137/137 测试全绿**（124 基线 + 6 Router payload + 2 Claude UI 冒烟 + 5 字号缩放/Router 集成）。

### feat

- **`c3901a8` 跨模块跳转负载（w5-payload 链）**：`Router::navigateTo` 增加可选 payload（默认空，既有调用零改动兼容），`consumePendingPayload(PageId)` 一次性消费语义（取出即清空、页面隔离、纯导航不清挂起负载）；Home「+ 新建笔记 / + 新建任务」带 `action:*` 负载在目标页触发既有新建弹窗，「最近编辑」行点击带 `open:<路径>` 负载直接打开指定笔记。测试 +6。
- **`843cb2c` Claude 模块操作按钮全量接线（w5-claude-ui 链）**：claude_module.cpp 从 0 个 onClick 到全量真接 `ClaudeService`——新建/编辑（dialog 表单，编辑预填、Key 留空=不换）、启用（setActiveProfile）、删除（确认 dialog + Keychain 清理）、测试连通（testConnection + toast）、代理开关（startProxy/stopProxy）。测试 +2（无头 compose 冒烟）。
- **`90d5792` 侧边栏导航 + 全局字号缩放 + UTF-8 argv + clearColor 跟随主题（w5-nav-global 链）**：main.cpp 自绘常驻侧边栏（260px，5 模块入口，当前页高亮），移除 T11 浮动齿轮桥接；`core::theme` 增加 `fontScale`（钳制 [0.5,2.0]，token 读取层统一施加、重建后重放不叠加），设置页字号档位改写全局缩放；`file_open_win.cpp` 改 `GetCommandLineW + CommandLineToArgvW` 宽字符 argv 转 UTF-8（非 ASCII 路径不再乱码，链接补 shell32）；`clearColor` 构造时从持久化主题快照（运行中切换主题窗口底色仍需重启，EUI-NEO 无运行时 API，见已知限制）。测试 +5。
- **`317bcf6` / `68182cc` / `0a20a65` 三链合并集成**：w5-claude-ui、w5-nav-global、w5-payload 依次合并回 master，合并后全量 137/137 全绿。

### docs

- **（本波收尾提交）**：KNOWN_LIMITATIONS 已修条目移入「已解决」存档小节（含修复 commit）；USER_GUIDE 导航改为侧边栏描述、代理配置流程改为全 UI 可完成；恢复计划追加 W5 段；CLAUDE.md 缺口表同步。

## [W3] 2026-09-22 —— 剩余需求（T9–T12 + P16）

最终基线：**124/124 测试全绿**（109 基线 + 2 Home + 10 Settings + 3 JSON）。

### feat

- **`883d681` P12 Home 真实数据仪表盘**：新增 `HomeService`（今日笔记 / 待办 / 逾期 / 今日 API 调用 / 最近编辑前 5，只读聚合 markdown_index / todo_tasks / claude_call_logs）；4 张统计卡 + 最近编辑列表接真数据并可点击跳转；快捷按钮接线（新建笔记 / 新建任务 / 切换 Profile，跨模块禁依赖故降级为导航）；`Database::pruneCallLogs` 启动时滚动清理调用日志；`core::Router` 增加 `registerAppRouter/appRouter()`。测试 109 → 111。
- **`a18c8a8` P13 设置页**：新增 `module_settings`——主题切换（下一帧全页生效）、字号缩放档位（85–130%，当前仅作用于设置页自身，见已知限制）、代理 token 只读显示 + 重新生成（同端口重启代理并重写 `proxy_token.txt`）、Markdown 目录只读展示、日志保留天数（0–3650，0=永久）；`SettingsStore` 读写 `%APPDATA%\DevDesk\config.json`，文件缺失 / 损坏 / 越界一律回退默认或钳制。设置入口为 main.cpp 顶层浮动齿轮按钮（应用无侧边栏）。测试 +10。
- **`750b7fa` W3 三链集成接线**：调用日志清理天数由硬编码 30 改为读 `config.json` 的 `logRetentionDays`（默认 30，0=永久时跳过清理——`pruneCallLogs(0)` 会删光日志，必须短路）。隔离 APPDATA 种子验证：retention=7 清掉 40 天前日志、retention=0 旧日志完好、Home 五项统计与种子吻合、objdump 零外部 DLL、启动冒烟五模块齐启。

### fix

- **`50ff663` 启动顺序理顺**：`startClaudeProxy()` 挪到 `getRouter()` 之前，模块 `onStart` 时 `ClaudeService::global()` 不再为 null，消除启动 WARN；代理只由 main 启动一次；删除 `proxy_router.cpp` 的 `#if 0` getHeader 死代码（全仓库零引用）。
- **`1f273f9` platform::json 实装**：`parse()` 此前是无条件返回 null 的桩、`stringify()` 一律输出 `{}`；实装 yyjson 递归解析（失败返回 JsonParseError）+ 自写 RFC 8259 序列化（yyjson 共享库被 EUI-NEO 以 `YYJSON_DISABLE_WRITER=1` 编译，writer 不可用）。测试 +3。

### docs

- **T12（本提交）**：USER_GUIDE + KNOWN_LIMITATIONS + README + CHANGELOG + P16 维护评估；恢复计划收官。

## [W2] 2026-09-21/22 —— 平台架构 + P14（T6–T8）

### feat

- **`a534f5d` Windows DPAPI keychain**：`CryptProtectData/CryptUnprotectData`（用户级，禁弹 UI）+ 密文落盘 `%APPDATA%\DevDesk\keychain\<service>\<account>.key`；T4 记录的 19 个连锁失败（keychain 2 + claude_service 10 + proxy_router 7）**全部转绿**，103 例基线全绿。

### refactor

- **`14a2373` 平台适配接口隔离**：keychain / paths / time / file_open 四族平台 API 按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake `target_sources` 按平台选择；`core::time::localTime` 统一 3 处散布的 `#ifdef`；业务代码零 `#ifdef _WIN32` / `__APPLE__`（grep `src/` *.cpp/*.h 零命中）。纯重构零行为变化（103 例 84/19 与 T4 基线一致）。

### test

- **`458cf50` P10b 流式压测**（6 例，103 → 109）：
  - 10MB 流式 e2e：44ms、1440 次 receiver 调用，全串比对 + FNV-1a 哈希 + 160 块块号三重校验一致（**吞吐实测约 240–320 MB/s**）；
  - 10MB 缓冲路径：48ms 字节一致；
  - soak 60s：1200/1200 chunk（**0% 丢失**，验收口径 < 5%），seq 严格递增；
  - soak 300s：`DEVDESK_SOAK=1` 门控，常规套件打印 `[SKIP]`；
  - 断连恢复：上游中途 `closesocket`，流以 End 干净收尾，已收字节与上游已发完全一致（直接层 + 端到端两场景）；
  - 内存采样：20 轮 × 10MB 流式，增量 12,660 KB（阈值 102,400 KB），无泄漏趋势。

### docs

- **`0a76935` W2 验收**：spec §16 P14 升必选 + 发布形态约束（单 exe 零外部 DLL）；计划文件 T7 / T8 附实测数字；CLAUDE.md 缺口表更新。

## [W1] 2026-09-21 —— 止血收编 + 发布形态（T1–T5）

### feat

- **`a1a106a` 收编 macOS 中断现场 P9/P10a**：流式代理（SSE chunk 透传）全部修改 + `streaming_proxy.*` + 3 个新测试文件 + Windows 迁移现场快照（含平台迁移现状文档与本恢复计划）。

### fix

- **`90f2d77` / `642c257` Windows 测试基建**：`filesystem::remove` 抛异常 + sqlite 句柄未关导致的套件崩溃修复，test_main 容错（单测异常只挂本用例不炸套件）；补提 claude_service / proxy_router / streaming_proxy 共 32 例。
- **`5460487` Windows 干净编译**（T2）：`json.h` 补 `<cstdint>`；platform 链接 `ws2_32/crypt32`；`CMAKE_CXX_STANDARD` 升 20（designated initializers）；`proxy_router.cpp` const 引用绑临时修复；`-Werror` 收窄到 DevDesk 自己的 target（不再压 EUI-NEO，曾产生 517 个第三方假错）；FTS5 检测补 SQLite3 库链接（修假阴性，CJK 搜索测试全过）。验收：删 `build/` 全新 configure **零 workaround flag**。
- **`ff3ce3d` / `fefc6ab` / `e4c2cb3` P10a 流式修复**（T5）：缓冲转发路径也走注入的 profileProvider；上游截断（`CURLE_PARTIAL_FILE`）按流干净结束而非报错；背压改为 write callback 内阻塞（取消不再跨线程 `curl_easy_pause`）。14 个 streaming 测试全绿。
- **`11d474c` mock 上游 SSE 服务器**：补 releaser 兜底 + `is_connection_closed` 双保险，client disconnect 用例真实可观测。

### build

- **`95c59d5` 单 exe 零外部 DLL**（T3）：`DEVDESK_STATIC`（默认 ON，仅 MinGW）——SQLite3 / CURL / OpenSSL / zlib / C++ 运行时全静态，libcurl 传递依赖闭包按 GNU ld 单遍序补齐（ssh2/nghttp2/nghttp3/brotli/zstd/…）；Release 摘除 EUI-NEO 强加的 `-Os` 规避 GCC 16.2 constexpr std::string C4 mangle bug。验收：`objdump` 仅 16 个 Windows 系统 DLL（Debug+Release 双验证）、无 MSYS2 PATH 干净目录可跑、**Release 15.4 MiB（16,152,064 字节）**。

### test

- **`8dfc8a9` 测试入口增强**：devdesk_tests 支持按子串过滤（`./devdesk_tests.exe stress`）与 `--log` Debug 级日志。

### docs

- **`3700ce1` W1 验收**：T1–T5 完成标记与实测数字归档。
