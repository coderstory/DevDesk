# DevDesk 波 1 修复设计（2026-09-27）

## 0. 定位与口径

- **覆盖范围**：六片审计 66 条待排中的第一批；本批 **11 条线 / 34 条 finding**。
- **输入**：`2026-09-27-full-audit-*.json` 六份 + `2026-09-27-full-audit-consolidated.md` + `2026-09-27-open-questions-review-A/B.md` + 本批 6 份只读侦察。
- **口径**：只修确认项、只修需求交付物缺陷；与审计原文不一致处逐条标「偏差」。
- **本文状态**：未改代码、未跑构建。

## 1. 全局约定

### 1.1 验收命令口径

`./build/tests/devdesk_tests.exe <子串>` 匹配的是 REGISTER_TEST 第二参数（描述串），**大小写敏感**（`tests/test_main.cpp:105` 用 find）。故服务层用例要写 `TodoService` 而非 `todo`；`home` 至少命中 7 条（`test_font_em_layout.cpp:54/111`、`test_home.cpp:53`、`test_module_gating.cpp:141`、`test_theme_hierarchy.cpp:261`、`test_app_assembly.cpp:148/175`），统计用例须用 `HomeService`。

### 1.2 用例数

基线全量实测 `Total: 621 / Passed: 619 / Failed: 1`（唯一失败为模块隔离守卫，见 L10）。预期变化：删守卫 −1 → 620；新增 http_client 3 条 +3 → 623；其余线 ±0（新增用例见各条）。**禁止在文档写死用例数**，一律「以运行输出 Total 为准」。

### 1.3 每线交付流水线

git worktree 隔离 → `CMAKE_BUILD_PARALLEL_LEVEL=3`（脚本默认 `DEVDESK_JOBS=3`）→ 只跑该线定向过滤词，禁全量（全量只在合并关口由主会话跑）→ 显式 pathspec 原子 commit → 主会话串行合 master → `git worktree remove` + `git worktree prune`。

### 1.4 不做清单（本波明确排除）

PAB-08 皮肤生成器保留不动；THS-12 不加 `(position)` 唯一索引；THS-13 文案保持「逾期N天」不变；THS-18 只做字段收窄不改 UI 路径；THS-17 不做「可测版」（函数不迁移文件）；CP-10 保留 `end()`（20+ 调用点），只删 `ended_`/`ended()`；FileWatcher 不引入 shared_ptr 方案；TIN-01 不保留 headless 冒烟用例。

## 2. 线划分与写者表

| 线 | 条目 | 独占文件 |
|---|---|---|
| L1 platform-http | CP-06、CP-10、NOPROXY | `src/platform/http_server.{h,cpp}`、`src/platform/http_client.cpp`、新 `src/platform/http_headers.h`、`src/core/sqlite_db.{h,cpp}`、`tests/test_modules/test_proxy_router.cpp` |
| L2 platform-family | CP-09、CP-12、FileWatcher | `src/platform/file_open_*.cpp`、新 `src/platform/file_open_common.h`、`src/platform/file_watcher.{h,win.cpp}`、新 `src/platform/system_theme_macos.cpp`、`src/platform/CMakeLists.txt`、`src/core/paths*.{h,cpp}` |
| L3 proxy-sync | CPX-09、CPX-12、G8/G9、CPX-15 | `src/module_claude_proxy/provider_config.{h,cpp}`、`settings_sync.{h,cpp}`、`tests/test_modules/test_provider_config.cpp`、`tests/test_modules/test_claude_service.cpp`、`tests/test_modules/test_settings_sync.cpp` |
| L4 proxy-json | CPX-11 | 新 `src/module_claude_proxy/json_scan.h`、`streaming_proxy.cpp`、`upstream_dialect.cpp`、对应测试 |
| L5 todo | THS-07、THS-11、THS-12、THS-13、THS-14、THS-17、THS-18 | `src/module_todo/todo_service.{h,cpp}`、`todo_module.cpp`、`src/core/migrations.h`、`tests/test_modules/test_todo_service.cpp`、`tests/test_core/test_migrations.cpp` |
| L6 home | THS-10、THS-16 | `src/module_home/home_service.{h,cpp}`、`src/core/time.h`、`src/module_todo/todo_ui_logic.h` |
| L7 pet+settings | PAB-09、PAB-12、PAB-13、CPX-02 残 | `src/module_pet/pet_window.{h,cpp}`、`src/module_pet_bridge.h`、`src/app_sidebar.cpp`、`src/module_settings/settings_module.cpp`、`src/module_claude_proxy/claude_module.cpp`（仅注释 + `:603` 三元收敛（文案行为变更））、`tests/test_modules/test_pet.cpp`、`tests/test_modules/test_menu_beautify.cpp` |
| L8 markdown | MD-06、MD-07、THS-15 | 新 `src/module_markdown/md_ids.h`、`markdown_module.cpp`、`md_live_edit.{h,cpp}`、`markdown_service.cpp`、5 个测试文件 |
| L9 tray | CP-05、TIN-05 | `src/core/tray_menu_spec.h`、`src/main.cpp`、`tests/test_modules/test_tray_dynamic.cpp` |
| L10 基建 | TIN-01、TIN-04、TIN-09、TIN-11、PAB-06、PAB-11、TIN-13 | `scripts/*`（含新 `scripts/check-module-isolation.sh`）、`tests/test_modules/test_settings.cpp`、`test_md_block_model.cpp`、`tests/test_platform/test_http_client.cpp`、`tests/test_core/test_router.cpp`、`tests/CMakeLists.txt`、`src/core/module_interface.h`（注释）、`scripts/check-consistency.sh` |
| L11 docs（最后跑） | TIN-12、CP-05 文档、CP-12 登记、G8/G9 说明 | `CLAUDE.md`、`docs/superpowers/plans/2026-09-27-HANDOFF.md`、`docs/USER_GUIDE.md`、`docs/KNOWN_LIMITATIONS.md`、`docs/superpowers/plans/2026-09-27-full-audit-consolidated.md` |

## 3. 逐条改法

### L1 platform-http

#### CP-06

- **现状**：`src/platform/http_server.cpp:33-41 toHeaderMap` 把 `httplib::Headers`（大小写不敏感，`3rd/cpp-httplib/httplib.h:506-508`）降级成裸 `std::map`（大小写敏感）；唯一调用点 `:51`；消费面唯一 `src/module_claude_proxy/proxy_router.cpp:155`。**响应侧同缺陷且线上可达**：`http_server.cpp:107` 在裸 map 上 `find("Content-Type")`，而上游头名经 `http_client.cpp:37` 保留原始拼写，上游回 `content-type: application/json` 时命中失败 → body 被标 `text/plain`。
- **改法**：新增 `src/module_claude_proxy/../platform/http_headers.h`（即 `src/platform/http_headers.h`），`inline std::string toLowerAscii(std::string)` + `inline std::map<std::string,std::string> toLowerKeyHeaderMap(const httplib::Headers&)`（键统一小写、保留首个 value、值原样）；`toHeaderMap` 保留原签名、函数体改调它（调用点零改动）；**响应侧改为大小写不敏感查找**——新增 `inline const char* findHeaderCI(const std::map<std::string,std::string>&, const char* lowerKey)`（或返回迭代器），`:107` 改调它，**不得只把查询键改成小写**（`proxy_router.cpp:93`、`:287`、`:421` 均用 `out.setHeader("Content-Type", ...)` 写头，而 `3rd/cpp-httplib/httplib.h:5842-5846` 的 `set_header` 是 `emplace`（不去重）：只改小写键会全部失配、并可能产生第二条头，使 502/503 错误响应的 body 由标对变标错）；`tests/test_modules/test_proxy_router.cpp:161-163`（该处注释「ctx.headers 已按精确键去重」失真）需同步改文案。建议**不合并**两条 header 回调（职责不同）。
- **验收**：`proxy_router`、`http_server`、`dialect`；新增 `http_server_header_case_insensitive_dedup`（发 `Authorization`+`authorization` 两条 → handler 内 `ctx.headers.count("authorization")==1` 且无大写键）+ 一条响应侧断言（handler 写 `content-type: application/json` → 断言响应头不是 `text/plain`）。
- **风险**：转发上游头名拼写变全小写（HTTP/1.1 合规、线上可见）；方言层 `putHeader`（`upstream_dialect.cpp:48-52`）用 canonical 拼写不受影响。
- **偏差**：审计原文未给类型名（只给 `httplib.h:506`），本 spec 补 `httplib::Headers`；`HttpHeader` 全仓命中 1（本节自述），非 0；审计只写请求侧，响应侧为本 spec 补充。

#### CP-10

- **现状**：`src/core/sqlite_db.cpp:377-380` 的 `setSchemaVersion(int)` 是空实现（`(void)version;`），声明在 `sqlite_db.h:92`，全仓零调用；`src/platform/http_server.h:31 void end();`、`:56 bool ended() const`、`:68 bool ended_`，实现 `http_server.cpp:128/130`（write 清、end 置）且 `:139 setChunkProvider` 也清，适配层 `toLibHandler` 从不读 `ended()`。
- **改法**：删 `sqlite_db.h:92` + `sqlite_db.cpp:377-380`；删 `http_server.h:31/56/68` 的 `ended()` 与 `ended_`、`http_server.cpp:128/130/139` 三行赋值，`end()` **保留为空操作**（全仓 20+ 调用点）。
- **验收**：`migrate`（`tests/test_core/test_migrations.cpp:210/291` 断言 schemaVersion，锁住迁移链不变）、`http_server`、`proxy`。
- **偏差**：审计原文建议「删 end()」，按范围纪律收窄为只删真死代码。

#### NOPROXY（新发现，超出原审计）

- **现状**：`src/platform/http_client.cpp` 全文无 `CURLOPT_NOPROXY`，本机 curl 会读 `http_proxy`/`no_proxy` 环境变量（`tests/test_modules/test_claude_service.cpp:112-149` 已实证）→ 本地 127.0.0.1 请求可能被公司代理劫持。
- **改法**：`http_client.cpp` 加 `CURLOPT_NOPROXY`（值 `localhost,127.0.0.1,::1`）。
- **验收**：`http_` 全绿；并在 `HTTP_PROXY=http://127.0.0.1:1` 的 shell 下复跑同一过滤词仍绿。

### L2 platform-family

#### CP-09

- **现状**：`shellSingleQuote` 逐字两份（`src/platform/file_open_macos.cpp:30-38` ≡ `file_open_posix.cpp:15-23`）；UTF-8↔wide 两份（`file_open_win.cpp:53-63 utf8ToWide` 有 `if (wlen <= 0) return {};` vs `file_watcher_win.cpp:259-264` 内联同款**无校验**——但 `:250` 已过 `is_directory` 校验，缺守卫**不可达**，属一致性债务）；env 分支三份（`paths_win.cpp:17-25` / `paths_macos.cpp:17-24` / `paths_posix.cpp:17-24`，`DEVDESK_CLAUDE_CONFIG_DIR` → `fs::path(env)/"settings.json"` 逐字相同）。
- **改法**：扩既有 `src/platform/file_open_common.cpp`，加 `shellSingleQuote` 与 `platform::detail::utf8ToWide`/`wideToUtf8`（新头 `src/platform/file_open_common.h`；该 .cpp **已在** CMake 基线源列表，无需改构建）；`platform` 两处删本地定义改 include；`file_open_win.cpp` 与 `file_watcher_win.cpp` 改调共享转换；env 分支下沉：`src/core/paths.h` 加 `fs::path claudeConfigDirOverride();`，`paths.cpp` 实现，三个 `paths_*` 改调。硬约束：`file_open_win.cpp:31 wideToUtf8ForTest` 是对外测试缝（`tests/test_platform/test_file_open.cpp:108-144` 的用例 `file_open_win_wide_to_utf8` 依赖），**行为必须逐字不变**。
- **验收**：`md_file_open`（描述串前缀是 `md_file_open: `；`md_open` 对描述 0 命中＝假绿）、`paths`、`FileWatcher`；新增 `paths_claude_settings_dir_override`（设/清 `DEVDESK_CLAUDE_CONFIG_DIR` 两分支都断言）。
- **存疑（未取证）**：新头放 `platform::file_open` 还是 `platform::detail` 更合既有约定。

#### CP-12（拍板：补齐实现）

- **现状**：`src/platform/CMakeLists.txt:18-25` 的 APPLE 分支编译的是 `system_theme_posix.cpp`（`:30` else 分支同款），故 macOS 上 `prefersDarkTheme()` 恒 false（`system_theme_posix.cpp:11`）→ `settings_module.cpp:289` 恒解析浅色。
- **改法**：新建 `src/platform/system_theme_macos.cpp`，用纯 C 的 CFPreferences（不用 NSUserDefaults，避免引入 .mm/ObjC++）：`CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"), CFSTR("NSGlobalDomain"))`（该 API 只接受 2 参，第二参即应用域）与 `CFSTR("Dark")` 比较；CMake `elseif(APPLE)` 内把 `system_theme_posix.cpp` 换成它（`:22`），并在 `if(APPLE)` 链接块（`:59-61`）加 `-framework CoreFoundation`；**绝不能加入 `:1-6` 基线 target_sources**（Windows 构建会找不到 CoreFoundation）；`system_theme_posix.cpp` 必须留在 else 分支。
- **验收（本机为 Windows，该 TU 无法编译验证）**：`settings`（含 `settings_theme_auto_follows_platform`）+ `theme`；Windows 全量构建零新增告警；静态核对三项（签名与 `system_theme.h:18` 逐字一致、`grep -rn system_theme_macos src/` 只在 CMake APPLE 分支出现一次、文件不含非 Apple 可用头）；在 `docs/KNOWN_LIMITATIONS.md` 登记「macOS 编译未验证」+ 验证条件（macOS 侧 `cmake --build` 通过 + 浅/深系统外观下 `prefersDarkTheme()` 与系统一致）。
- **存疑（未取证）**：无 GUI 会话/SSH 下 `CFPreferencesCopyAppValue` 的返回值；`kCFPreferencesCurrentUser` vs `AnyUser`。

#### FileWatcher（review-A 结论，CONFIRMED）

- **现状**：现网可达性 = 不可达，唯一消费方 `markdown_service.cpp:131 start`/`:143` 析构 stop，回调 `onWatchEvent:155-198` 从不 stop。`src/platform/file_watcher_win.cpp:125-138 joinWorker` 在回调线程内走 `detach()` 分支后立即 `CloseHandle(dirHandle)` / `CloseHandle(stopEvent)`，而 `runWorker` 每轮仍直读 Impl 字段（`:158`、`:174`、`:179`、`:200`），退出路径 `:219-220` 只 `running.store(false)` + 关 `ov.hEvent`，**不碰这两个句柄** → 头文件 `file_watcher.h:60`「析构自动 stop()：join 后台线程」与 `:75-76`「回调线程内调用安全」两条契约不成立；若消费方在回调内**析构**，`impl_`（`unique_ptr<Impl>`）析构后 detached worker 仍在 `Impl` 成员函数里跑 → UAF。
- **改法**：① 回调线程分支只做 `running.store(false); if (stopEvent) SetEvent(stopEvent); return;`（不 detach、不关句柄）；② 两个 `CloseHandle` 移入 `runWorker` 退出路径（判空幂等）；③ `runWorker` 改用局部句柄副本（入口 `HANDLE dh = dirHandle; HANDLE se = stopEvent;`，循环内全用副本），退出前关副本并 `if (impl_->dirHandle == dh) impl_->dirHandle = nullptr;`（同 stopEvent）；④ `file_watcher.h:60/75-76` 注释改成真实契约（对象必须活到回调返回）。**不引入** shared_ptr 方案。
- **验收**：`FileWatcher`（描述首段是 `FileWatcher:`，`fwatch` 0 命中）现有 5 例 + 新增 `fwatch_stop_from_callback_thread`（handler 内首次收到事件即 `w.stop()`，join 后断言 `active()==false`、不崩、可再次 `start()` 并收到事件）。
- **存疑（未取证）**：detach 后 `running` 串改与 Impl 生命周期两项由本批侦察补出，**未做运行期复现**。

### L3 proxy-sync

#### CPX-09

- **现状**：`src/module_claude_proxy/env_var_hints.h:16 kEnvVarHints[]` 实测 **256** 条（无重名）；`provider_config.cpp:43-63 knownOfficialEnvKeys()` 手写 **18** 条；交集 **16**（缺 2 条 DevDesk 自造键 `ANTHROPIC_DEFAULT_FABLE_MODEL` / `ANTHROPIC_DEFAULT_FABLE_MODEL_NAME`）→ 被判「未识别」的官方键 = **240**。唯一判据消费点 `provider_config.cpp:221`；红框判据 `claude_module.cpp:1814-1829` 已由 hints 驱动（不误红），真噪点是导入预览 `claude_module.cpp:1935-1951`。
- **改法**：`provider_config.cpp` 加 `#include "env_var_hints.h"`，删 `:40-65` 整块，`:221` 判据改 `envVarHint(key) == nullptr && !isDevDeskExtendedEnvKey(key)`，保留 2 条 FABLE 键的小扩展集。**必带测试改**：`tests/test_modules/test_provider_config.cpp:282-283`、`:465-466`、`:679` 三处把 `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE` 当「未知键」样例，而它在 `env_var_hints.h:133` → 换真未知键（如 `NOT_A_REAL_VAR_XX`）。
- **验收**：`provider_config`；新增 `provider_config_official_keys_derived_from_hints`（断言 `applyEnvAliases({{"ANTHROPIC_DEFAULT_SONNET_MODEL_NAME","x"}}).unrecognizedKeys.empty()`、FABLE 两键仍不标记、真未知键仍标记）。
- **存疑（未取证）**：256 条是否 100% 官方来源（禁联网未核对）。

#### CPX-12

- **现状**：`settings_sync.cpp:26-38 conflictingEnvKeys()` 硬编码 **8** 键，唯一消费点 `:253`（在 `detectOverlayConflicts:244-266` 内），唯一调用点 `:415`（takeOver 尾部）；`provider_config.cpp:355-408 buildProxyEnv` 的槽位键与 4 个 `_NAME` 键**仅槽位非空才写**（`:390-397`）。缺口：`buildProxyEnv("","",...)` 只产 **2** 键（BASE_URL + 占位 token）→ `_NAME` 与 extra_env 键全不在检测集。
- **改法**：`provider_config.cpp` 把 `:390-397` 提为文件级 `constexpr SlotEnvRow kSlotEnvRows[]`，新增导出纯函数 `managedEnvKeyNames()`（**12 键** = BASE_URL + 占位 token + 6 槽位键 + 4 `_NAME`，与值无关），`buildProxyEnv` 改遍历同一张表；`settings_sync.cpp` 的 `detectOverlayConflicts` 入参加 `const std::vector<std::string>& extraManaged`，键集 = `managedEnvKeyNames()` ∪ `takeOver:294-296` 已算出的实键 `managedKeys`，`:415` 传 `managedKeys`；`settings_sync.h:22-25` 头注释同步措辞。
- **偏差**：不照抄审计建议的 `buildProxyEnv(空 profile, 端口)`——那只有 2 键，会丢掉 6 个槽位键的检测（回归）。只扩不缩（union）→ 警告只会变多，通道 `out.warnings` 不阻断。
- **验收**：`sync_overlay`（现有三形态须仍绿）+ `sync_service_warnings`；新增 `sync_overlay_detection_managed_derived`（profile 带 sonnet 模型 + `extra_env {"MY_KEY":"1"}`，overlay 写 `ANTHROPIC_DEFAULT_SONNET_MODEL_NAME` + `MY_KEY` → 断言 2 条警告含键名；overlay 只写未托管键 → 0 条）。
- **存疑（未取证）**：真实环境的 `*_NAME` 出现频率。

#### G8/G9（review-A 第 6 条，CONFIRMED）

- **现状**：`settings_sync.cpp:107-131 parseJsonObjectFile`（`:109 fileExists` → `:113 out.exists=true` → `:114 readTextFile` → `:115 text.empty() → out.ok=true` **两形态在此合流**），根因 `:46-52 readTextFile` 的 `if (!f) return {};`（打开失败与 0 字节同为空串）；消费点 `:540`（G9 no-op）与 `:531-536`（G8）。spec 依据：`docs/superpowers/specs/2026-09-27-profile-reverse-sync-and-copy-design.md:80/:48/:92`。
- **改法**：`readTextFile(const fs::path&, bool* readFailed = nullptr)`（匿名 namespace 内，公有接口不变），`if (!f) { *readFailed = true; return {}; }`，末尾补 `if (f.bad()) *readFailed = true;`；`parseJsonObjectFile:114-118` 读失败 → `out.error = "读取失败（IO/权限）: " + p.string();` 且 `out.ok` 保持 false（= G8 中止），仅 `text.empty()` 走 `out.ok = true`（= G9 no-op）。
- **偏差**：审计原案用 `fs::file_size` 判据，对「目录形态」无效（目录 st_size 多为 0）→ 改用读流失败信号。
- **行为变更点**：`takeOver:299` 与 `restore:456` 由「静默当空文件后整体重写」变「中止不写」（`restore` 原会抹掉快照里没有的 permissions/hooks），`clearResidualProxyKeys:198` 由 no-op 变多一条 WARN；`detectOverlayConflicts:248` 已判 `!doc.ok` 不变。
- **验收**：`sync_reverse`（现有「0 字节空文件 → no-op」用例须保持绿）+ 新增 `sync_reverse_read_unreadable_aborts`（Windows 独占句柄 `CreateFileW(path,GENERIC_READ,0,...,OPEN_EXISTING)` 造「存在但打不开」→ 断言 `ok==false`、`present==false`、error 含路径；同用例内对照 0 字节文件仍 `ok==true && !present`）。
- **存疑（未取证）**：MinGW 下 `ifstream` 打开目录的行为。

#### CPX-15

- **现状**：`settings_sync.h:2-39` 头注释已分四段（takeOver 6 步 / restore / 崩溃兜底 / 热切换），但**「settings.json 唯一 IO 边界」不成立**——`claude_module.cpp:1458 doImportCurrentConfig`（原写 `1462-1480`，起点漂移 4 行）自行 `std::filesystem::exists` + `ifstream` 直读。
- **改法**：只补/修头注释（零签名变更），措辞为「DevDesk 对 settings.json 的**唯一写入口**（读-改-写 + 原子写 + sidecar 协议）；读侧另有导入路径直读」。
- **偏差**：审计建议措辞与现状不符，本 spec 收窄。

### L4 proxy-json

#### CPX-11

- **现状**：`streaming_proxy.cpp:19-156` 与 `upstream_dialect.cpp:99-220` 两套 JSON 顶层扫描器；逐行比对：`skipWs`、`readJsonString`（含 `\` 溢出分支）、`skipJsonValue` **逐字节相同**；`}`/`,` 判定顺序互换无语义差；出口 `break` vs `return false` 等价；`findTopLevelField` 内 `streaming_proxy.cpp:120` 赋值、`:138` `(void)` 无读取的 `keyStart` 是死变量。真差异只有 2 条且都在 wrapper 层（值类型判定）→ **可无损合并**。重复小工具实测 **3 类 6 份**：`trim`（**仅收录 `streaming_proxy.cpp:151` 一处**，删除后由 `claude_service.cpp:48` 保持原样）、`stripTrailingSlash`（**本轮不收录**：调用点 `claude_service.cpp:238/:408/:1098/:1550`、`proxy_router.cpp:227` 均不在本波写者表内）、`toLowerAscii`（`upstream_dialect.cpp:22` 用 std::tolower vs `provider_config.cpp:415` 手写 A-Z，ASCII 等价）。
- **改法**：新建 `src/module_claude_proxy/json_scan.h`，`namespace module::claude::json_scan`，**全部 `inline`**（Debug 下 `-Werror -Wall -Wextra`，anon/static 会撞 `-Wunused-function`）：`skipWs` / `readJsonString` / `skipJsonValue` / `findTopLevelValueSpan(s,key,begin,end)`（唯一扫描器，不做类型判定）/ `trim`；`streaming_proxy.cpp` 删 `:18-156`，`wantsStream` 用 span + `body.compare(b,4,"true")==0`；`upstream_dialect.cpp` 删 `:99-220`，`findTopLevelStringSpan` 降为 3 行薄包。**建议两份 `toLowerAscii` 不合并**（要择一语义，收益低于风险）。`src/module_claude_proxy/CMakeLists.txt` 只列 `.cpp` → **无需改 CMake**。
- **验收**：`streaming`、`dialect`；新增 `json_scan_top_level_span_equivalence`（把旧实现留在测试内做对照 corpus：顶层 key / 嵌套同名 key / 非字符串值 / 值含逗号 / 截断 body / 非对象）。
- **存疑（未取证）**：未编译运行，「无损」是逐分支静态比对结论。

### L5 todo

#### THS-07

- **现状**：`todo_service.h:107 static core::Result<int64_t> nowEpochSeconds();`，`todo_service.cpp:50-52` 唯一 return 为 `ok(...)`，无失败路径。**8 个调用点已核实**：`todo_service.cpp:247`(createColumn)、`319`(renameColumn)、`464`(createTask)、`534`(updateTask)、`674`(moveTask 事务内层 lambda，变量名是 `nowR`)、`696`(completeTask)、`719`(uncompleteTask)、`779`(listOverdue)。
- **改法**：签名改 `static int64_t nowEpochSeconds();`，函数体单行 `return static_cast<int64_t>(std::time(nullptr));`；8 处各把 4 行折成 `const int64_t now = nowEpochSeconds();`；`247`/`464` 后续已有 `int64_t now = nowResult.value();` → 直接改初始化，**勿留重复声明**；`674` 的 `nowR` 用后即删（`nowR.value()` 见 `:683`）。
- **风险**：与 THS-11 在 `renameColumn` 同函数重叠 → 必须同一写者串行。
- **验收**：`TodoService`。

#### THS-11（拍板：实现校验）

- **现状**：`todo_service.h:59` 注释承诺 `id 不存在 → NotFound`，`todo_service.cpp:318-337 renameColumn` 只发 UPDATE 后 `return ok()`；`:335` 注释提到 `changes()` 但从未调用；唯一真实 NotFound 在 `moveTask` `:608`。全仓同类承诺扫描：**只有 renameColumn 一处**。
- **改法**：照先例 `src/core/database.cpp:54-78 pruneCallLogs` —— `std::lock_guard<std::recursive_mutex> lock(db.mutex());` 覆盖 UPDATE 与取数，UPDATE 后 `if (sqlite3_changes(db_.handle()) == 0) return fail(core::ErrorCode::NotFound, "column not found");`（`<sqlite3.h>` 经 `src/core/CMakeLists.txt:49-54` 的 PUBLIC 链接传递可达；替代方案 `db_.prepare("SELECT changes() AS n")` 须同样持锁）。**只做 renameColumn**，不扩散到 delete/create 系列。
- **风险**：UI `todo_module.cpp:441-448 doRenameColumn` 对 fail 走 `"重命名失败：" + errorMessage()` → NotFound 文案会进 toast。
- **验收**：`renameColumn`；新增 `todo_svc_rename_missing_notfound`。

#### THS-12（拍板：删负位段）

- **现状**：`todo_service.cpp:373-403`，`:375` 注释「先全部设为 -(position+1)-1000000（避免唯一约束冲突）」+ `:376` 全域 UPDATE 负位；`src/core/migrations.h:59-65 todo_columns` 定义**无 UNIQUE**，全仓 `CREATE INDEX` 除 `migrations.h:82-85`（`todo_tasks` 两条）外还有 `:101`、`:118`、`:120`；`UNIQUE` 除 31/40/90/101/194/211（markdown/claude 表）外还有 `:126` → **(position) 唯一索引实证不存在**（枚举补全后结论仍成立）。
- **改法**：删 `:375-383` 整段（负位 UPDATE + 其错误分支），保留 `:387-399` 循环（`position = i`），注释改为实际理由（「v1 建表 position 无唯一约束，逐行覆盖即可」）+ 写明「ids 必须是全部列的一次排列」。
- **边界**：同列内并列 = ids 含重复 id → 后写者胜；首尾 = i=0 / i=n-1 无特殊处理；唯一调用点 `todo_module.cpp:473-495 doShiftColumn` 按 `columns_` 顺序传全集，故「非全集」不可达。
- **验收**：`reorder` + `todo_ui`。

#### THS-13（拍板：显示 max(1,…)，文案不变）

- **现状**：入选按秒级（`todo_service.cpp:785-789` 的 SQL，`now` 取 `779-783`），显示 `todo_module.cpp:1976-1977` 用 `ui::overdueDays(task.dueAt.value_or(0), now)`（行号漂移：审计写 1972）。
- **改法**：`todo_module.cpp:1976-1977` 一行改 `std::max(1, ui::overdueDays(...))`（`<algorithm>` 已在 `:26` include）。
- **风险**：既有 `todo_ui_overdue_dialog`（`:1462`，断言含 "2"）与 `todo_ui_overdue_zero_grey`（`:1539`）均不红。
- **验收**：`todo_ui`；新增 `todo_ui_overdue_min_one_day`（`dueAt = now - 1` → 断言行文案 `"逾期1天"` 且 badge 为 `1`）。

#### THS-14（拍板：补 kind='archive'）

- **现状**：`todo_module.cpp:1006 const bool isArchive = (col.title == "已归档");`（漂移 +4），用于 `:1073` 空态文案；`findColumnByKind` 注释在 `:297-298`（漂移 +1）；`TODO` 迁移最高版本 **v6**（`migrations.h:205-218`）；v4 先例 `migrations.h:163-171 kTodoColumnKind`（ALTER + 两条 UPDATE 按 title 回填）；`src/` 内 `已归档` 命中为 `src/core/migrations.h:138` + `src/module_todo/todo_module.cpp:1006`（`:1073` 的文案实为「归档的任务在这里」，不含「已归档」）。
- **改法**：新增 `migrations.h` **v8** 条目 `UPDATE todo_columns SET kind = 'archive' WHERE title = '已归档';`（master 上 v7 已被 profile 认证字段 `kClaudeProfileAuthField` 占用，归档 kind 迁移顺延为 v8；kind 列 v4 已建，**无需 ALTER**），append 进 `all()` 末尾（`:221-231`）；`todo_module.cpp:1006` 改 `col.kind == "archive"`；`todo_service.h:30-32` kind 值域注释补 `'archive'`。**必带 4 处测试改**：`tests/test_modules/test_todo_service.cpp:617`（断言 `list.value()[3].kind == ""` → 改 `"archive"`）、`tests/test_core/test_migrations.cpp:210` 与 `:291`（`schemaVersion()==6` → `==8`）、`tests/test_modules/test_provider_config.cpp:380`（`schemaVersion` 6→8；该文件属 L3，需跨线协调）。
- **已知限制（与 v4 同病，不引入新机制）**：升级前已改过归档列名的库回填落空。
- **验收**：`migrate`、`列 kind`；新增 `migrate_archive_kind_v8`。

#### THS-17（拍板：零迁移版）

- **现状**：`todo_module.cpp:47-54 priorityLabel`（switch + `default: return "无"`）、`:56-63 priorityColor`（switch + default）、`todo_service.cpp:18-25 intToPriority`（switch 于 int + default None）、`:27-29 priorityToInt`（纯 `static_cast<int>` 包装）；枚举连续 `todo_service.h:22 None=0, Low=1, Medium=2, High=3`；调用点 `priorityColor` :1186/:1205、`priorityLabel` :1205/:2159、`priorityToInt` 仅 :558、`intToPriority` 仅 :149。
- **改法**：① 删 `priorityToInt`，`:558` 改 `static_cast<int64_t>(task.priority)`；② `intToPriority` 换 range check（`v < 0 || v > (int)Priority::High` → None，否则 `static_cast<Priority>(v)`）——switch 于 int 无法去 default；③ `priorityLabel`/`priorityColor` 原地改 `static constexpr` 数组 + `static_assert(std::size(...) == static_cast<size_t>(Priority::High)+1)`，越界走 `[0]`（新增枚举 → 编译期失败）。**不做**「可测版」（不迁移文件、不动 `test_font_em_layout.cpp:85`）。
- **验收**：编译即证（`todo_ui`）。

#### THS-18

- **现状**：`todo_service.cpp:542-546` UPDATE 字段含 `column_id = ?, … position = ?`，绑定 `:551`（`task.columnId`）、`:565`（`task.position`）；UI `todo_module.cpp:2023-2044 collectDetailFormTask` 的 `columnId` 取 `detailColumnId_`（`:2026`）、`position` 取 `findTask(...)->position`（`:2038-2039`），而 `detailColumnId_` 仅在 `:1992`（打开详情，取当前列）与 `:2292`（在 `:2291 saveDetailForm()` **之后**、`:2295 doMoveTask` 之前）赋值 → **保存路径上 columnId 恒等于库中现值，是「防御不存在的路径」**。
- **改法**：`:542-546` 删 `column_id = ?,` 与 `position = ?,` 两字段 → 绑定序号前移（title=1、description=2、priority=3、due_at=4、tags=5、completed_at=6、updated_at=7、id=8），删 `:551`/`:565` 两行绑定，其余 `bind(n,…)` 连号重排；`todo_service.h:81-82` 注释改「整字段更新（只改内容字段；改列/改序一律走 moveTask）」。
- **语义**：误用 updateTask 改列变「静默不改列」（优于静默留空洞）。
- **已核对**：5 用例 10 处直调（`test_todo_service.cpp:330/375/427/432/438/664/677` + `test_todo_ui.cpp:1485/1489/1492`）断言不受影响。
- **验收**：`updateTask`、`编辑保存不改序`；新增 `todo_svc_update_task_ignores_column`。

### L6 home

#### THS-10

- **现状**：`home_service.cpp:89-112`（1 参）与 `:114-141`（2 参）逐行重复，唯一差异是 `:121-123` 三行 bind；声明 `home_service.h:66-67`；调用点 `:193`(2 参)、`:200`(**1 参**)、`:206`(2 参)、`:214`(2 参)。
- **改法**：删 `:114-141`，合成 `core::Result<int64_t> countBySql(const char* sql, std::optional<int64_t> boundValue = std::nullopt);`，实现内 `if (boundValue.has_value()) { ... }`；`home_service.h` 补 `#include <optional>`（勿依赖传递）。4 个调用点零改动。
- **验收**：`HomeService`（**注意大小写**：`home` 至少命中 7 条——`test_font_em_layout.cpp:54/111`、`test_home.cpp:53`、`test_module_gating.cpp:141`、`test_theme_hierarchy.cpp:261`、`test_app_assembly.cpp:148/175`——统计用例须用 `HomeService`）。

#### THS-16

- **现状**：`home_service.cpp:28-31` 用两个本地零点相减后 `const int64_t dayGap = (todayStart - modDayStart) / 86400;`，`:39 if (dayGap == 1) return "昨天";`；`days_from_civil` 仓内**无独立实现**——仅是 `todo_ui_logic.h:222-233` 内 `ui::overdueDays` 的局部 lambda（另一处提及是 `:221` 注释）。
- **改法**：`src/core/time.h`（`namespace core::time` **已存在** `:8`；须补 `#include <cstdint>`，否则加 `inline int64_t` 函数后 `time_win.cpp`/`time_posix.cpp` 编不过）加 `inline int64_t daysFromCivil(int y, unsigned m, unsigned d)` + `inline int64_t localDayNumber(std::time_t t)`（照搬 Hinnant 实现）；`todo_ui_logic.h:215-236` 删内联 lambda 改调 `core::time::localDayNumber`；`home_service.cpp:31` 改 `const int64_t dayGap = core::time::localDayNumber(now) - core::time::localDayNumber(static_cast<std::time_t>(modifiedAt));`；`startOfDayEpoch` 保留（`:188/193/214/216` 仍用于 SQL）；**另删 `home_service.cpp:28-30` 两处 `startOfDayEpoch` 局部**（改后即成未用局部，`-Werror` 报错）——此项未编译验证，属头文件级推断。报告点名的 `todo_ui_logic.h:177-185 ymdToEpoch`（正午在 `:182`；原写 `:161` 系过期快照）已用「正午取 epoch」规避 DST，**本轮不改**。
- **验收**：`HomeService` + 新增 `core_time_days_from_civil`（1970-01-01→0、2024-02-29 闰日→19782、相邻日差 1、12-31→01-01 差 1）。**不加 DST 断言**（`tests/test_main.cpp:18-28 setEnvVar`：Windows 分支 `::_putenv_s` 在 `:20`、`:22-25` 是 POSIX 分支；对 MinGW msvcrt 的 `mktime` TZ 行为不保证，且仓库无 TZ 注入先例）。
- **未取证**：DST 修复在本机（CST 无 DST）无法复现。

### L7 pet+settings

#### CPX-02 残（review-B 第 3 条，CONFIRMED）

- **现状**：`settings_module.cpp:534 if (managed && svc) {` → `:535 warnLines = std::min<size_t>(svc->lastSyncWarnings().size(), 2);`，`:1303` 渲染门同款，警告行 y=`118 + 16*i`（`:1311/1320`），高度源 `:542-545`、contentKey `:601-604`；对照已修 `claude_module.cpp:585-605`（无 managed 门），警告源 `claude_service.cpp:1683-1687`。可达性：`takeOver` 在写 sidecar **之前**中止的失败（`settings_sync.cpp:299-304`「解析失败，中止接管（未写入）」、`:365-372` sidecar 序列化/写失败）会把「接管失败: …」压进 `lastSyncWarnings_`，此时 sidecar 不存在 ⇒ `managed=false`（`settings_module.cpp:1598`）⇒ 设置页既不显示也不留高度。
- **改法**：两处 `managed && svc` → `svc`（**必须同批**，否则高度与渲染行错拍）；`managed` 仅保留给 `:1298-1300` 状态行文本/色；对齐 `claude_module.cpp:597-604` 注释（「设置页同样按 managed 门控」已失效）+ 三元收敛为恒带后缀。
- **风险**：未接管+有警告时卡高 132 → 150/166 px，`proxyH` 进 contentKey 会自动失效重测。
- **验收**：`settings`；新增 `settings_takeover_warn_unmanaged`（断言 `ui.find("settings.card.proxy.takeover.warn.0") != nullptr`）。
- **存疑（未取证）**：无头构造「未接管 + 警告非空」的路径。

#### PAB-09

- **现状**：`pet_window.cpp:6`（头注释仍列「穿透」）、`:144`、`:151`、`:774`、`:798/810`；`app_sidebar.cpp:275` 写「260/72…+188px」而实际 `app_sidebar.h:19-20` = **208/72**（`:5`/`:112` 已写 208/72）；`settings_module.cpp:1765` 残留成员删除注释；用例名漂移 `tests/test_modules/test_menu_beautify.cpp:45`（标题「6 顶层项」实断 5）、`:62`（标题「5 顶层项（穿透文案随态）」实断 4）。
- **改法**：注释写实为当前顺序 `[换肤子组] / 位置重置 / 隐藏 / 分隔线 / 退出`；删 `:151` 穿透行、`pet_window.cpp:144` 一处，与 `settings_module.cpp:1765`；删除清单**另纳入** `pet_config.h/.cpp` 的历史说明、`pet_window.h:52`、`settings_module.cpp:540/:647`；`app_sidebar.cpp:275` 260→208、+188→+136；用例名改「含换肤子组（5 顶层项）」「无皮肤时 4 顶层项」。**勿动**：`test_menu_beautify.cpp:139/178` 的 `"穿透"`（styled 组件样例菜单文本）、`test_pet.cpp:349/364` 的 `mousePassthrough` 断言（真实语义）。
- **验收**：`menu`；`grep -rn 穿透 src/` 应只剩 `pet_config.h/.cpp` + `pet_window.h:52` + `settings_module.cpp:540/:647`（此三处已纳入改法删除清单，删后应零命中）。

#### PAB-12

- **现状**：`pet_window.cpp:114 return table[static_cast<size_t>(std::rand()) % table.size()];`（`pickPhrase` 在匿名 ns，不可单测）；`:366-367 roll = std::rand() / RAND_MAX`；include 仅 `<cstdlib>`（`:18`）；全仓 `srand` 0 命中；先例 `claude_service.cpp:1337` 用 `std::random_device`。
- **改法**：匿名 ns 加 `std::mt19937& rng() { static std::mt19937 g(std::random_device{}()); return g; }` + `#include <random>`；`:114` 改 `std::uniform_int_distribution<size_t>(0, table.size()-1)(rng())`；`:366-367` 改 `std::uniform_real_distribution<double>(0.0, 1.0)(rng())`（阈值 `dt/150`、`dt/60` 不变）；为可测把 `pickPhrase` 提为 `module::pet::pickPetPhrase(const PetSkin&)` 并在 `pet_window.h` 声明。
- **风险**：仅渲染线程访问，单线程安全。
- **验收**：`pet`；新增 `pet_phrase_randomized`（连调 64 次断言 ≥2 个不同短语）。
- **存疑（未取证）**：跨进程不再同序列在单进程用例中不可断言。

#### PAB-13

- **现状**：Hide 分支 `pet_window.cpp:274-279`（`savePetPosition(); requestWindowClose(runtime().handle);`），菜单项 `:788`；设置页桌宠卡 `settings_module.cpp:993-1073`（皮肤下拉 1037-1054、位置重置按钮 1058-1070，`(w-188,64)` 160×40）；桥 `src/module_pet_bridge.h`（4 函数 + static_assert）；`startPetWindow`（`pet_window.cpp:593`）保留 `rt.skin/rt.config`。
- **改法**：pet 侧加 `bool showPetWindow()`（`if (!rt.windowRequested) return false; if (rt.handle != nullptr) return false; startPetWindow(rt.config); return true;`），`PetRuntime` 加 `bool windowRequested = false;`（`startPetWindow` 入口置 true，防门控关时凭空开窗）；`pet_window.h` 与 `module_pet_bridge.h` 同步声明 + static_assert；设置页在「位置重置」左侧加 160×40 按钮 `id + ".show"`、位置 `(w-28-160-168, 64)`、文案「显示桌宠」，onClick 调桥并按返回值 setStatus（false → 「桌宠本次会话未启用，重启后生效」）。同排 y=64，**卡高保持 120、contentKey 不变**。
- **风险**：卡内水平占位 28+240+8+160+8+160+28 = 632（未取证最小窗内容宽）；`runtime()` 是进程级 static，`windowRequested` 会跨用例留存。
- **验收**：`pet`；新增 `pet_show_window_reenqueues`、`settings_pet_show_button_renders`。
- **存疑（未取证）**：无头下 `onWindowCreated` 是否触发。

### L8 markdown

#### MD-06

- **现状**：`markdown_module.cpp:191` 定义 `kEditorInputHitId = "md.editor.input.hit"`；裸字面量 `"md.editor.input"` **实测 12 处**（319/1030/1059/1092/1130/1318/1675/1922/1964/2323/2349/2986），`"md.editor.input.hit"` 另有 3 处（191 定义 + 1070 + 2985）；测试侧裸字面量 **实测 58 处**（test_markdown 41 / test_md_keyboard 12 / test_md_highlight 3 / test_editor_font 1 / test_md_live_spike 1），另有 16 处派生子 id + 1 处用例标题提及。
- **改法**：新建 `src/module_markdown/md_ids.h`（`namespace module::markdown { inline constexpr const char* kEditorInputId = "md.editor.input"; inline constexpr const char* kEditorInputHitId = "md.editor.input.hit"; }`）；`markdown_module.cpp` 替 12 处裸字面量、`:191` 定义改 include 复用、`:1070/:2985` 改用常量；5 个测试文件 include 该头并替 58 处。**常量必须落独立头**（markdown_module 无头文件，测试只有 `extern "C" create_markdown_module`，引不到 .cpp 内常量）。
- **风险**：`tests/CMakeLists.txt:63-67` include 已含 `${CMAKE_SOURCE_DIR}/src`，`#include "module_markdown/md_ids.h"` 可用；5 文件漏加 include 即编译失败。
- **偏差**：处数 12/58 非审计的 13/46。
- **验收**：编译即证 + `markdown`。

#### MD-07

- **现状**：`markdown_module.cpp:183 constexpr float kEditorLineHeightFactor = 1.2f;`（用于 `:1925`）与 `md_live_edit.cpp:14 constexpr float kMdLiveLineHeightFactor = 1.2f;`（用于 `:38`），该措辞仅在 `md_live_edit.cpp:12`；`markdown_module.cpp:183` 只标「input 内部行高系数」；上游真值 `EUI-NEO/components/input.h:128`（`fontSize * 1.2f`）。
- **改法**：`md_live_edit.h` 加 `inline constexpr float kInputLineHeightFactor = 1.2f;`（注释写出处），`md_live_edit.cpp:14` 与 `markdown_module.cpp:183` 删本地常量改用同一常量（`markdown_module.cpp:46` 已 include `md_live_edit.h`）。
- **验收**：`md_live` + `markdown`。

#### THS-15（拍板：切 core + 码映射）

- **现状**：`markdown_service.cpp:645` 注释引 `KNOWN_LIMITATIONS #16`，而 `docs/KNOWN_LIMITATIONS.md:124` 的 #16 是「settings.json 备份轮换不实现」（与原子写无关）；本地 `atomicWriteFile` 在 `:650`，被 `:698`(write) 与 `:745`(create 类路径) 调用；`src/core/atomic_write.h` + `.cpp` 已存在（THS-03 下沉）；`settings_sync.cpp:57` 另留自有 `atomicWrite`（**不在本线范围**）。
- **改法**：删 `:640-649` 旧注释 + 删本地 `atomicWriteFile` 定义，`:698/:745` 改调 `core::atomicWriteFile(absPath, content, "笔记")` 并 `#include "core/atomic_write.h"`；**失败码映射回 `core::ErrorCode::FileWriteFailed`**（本地版原语义，避免同函数内混用 `Internal`）。
- **风险**：错误文案带 label 前缀会变；测试侧无 `FileWriteFailed` 断言（grep tests/ 0 命中）。
- **验收**：`markdown_service` + `core`（`tests/test_modules/test_view_state.cpp:207 core_atomic_write`）。

### L9 tray

#### CP-05（拍板：删）

- **现状**：`src/core/tray_menu_spec.h:58-63` 基线只有两项（「显示主窗口」`0xF2D2` ShowMainWindow、「退出 DevDesk」`0xF011` Exit），零导航项；`:48-53 isNavigationAction` 只认 Navigate\*，`:76` 谓词仅在其为真时被消费 → `:77 item.disabled=true` 恒不执行、`:78` 后缀恒不追加、`:95` 分隔线合并恒不命中 ⇒ `:87-101 trayMenuSpecFiltered` 是**恒等映射**；消费点 `src/main.cpp:378`/`:396`；谓词 `:318 trayNavRegisteredInRouter`、`:331 trayNavAvailableFromConfig` 返回值无人消费。
- **改法（完整清单）**：`tray_menu_spec.h` 删 `isNavigationAction`(:46-53)、`trayDisabledHintSuffix`(:43-44)、`trayMenuSpec(pred)`(:65-82)、`trayMenuSpecFiltered`(:84-101)、`TrayMenuSpecItem::disabled` 字段(:38-40)，头注释 :1-15 压到 3-4 行（删「审计 [7]」段 :7-15）；`main.cpp`（匿名 ns 46-469，**未用即 -Werror**）删 `trayNavRegisteredInRouter`(:316-325)、`trayNavAvailableFromConfig`(:326-343)，`nativeTrayMenuItems`(:375-388) 与 `showTrayStyledMenu`(:395-415) 改直接遍历 `::core::tray::trayMenuSpec()` 并删 `:402 item.disabled = s.disabled;`，改注释 :309-314/:372-374/:390-394/:566-571。**保留不动**：`TrayMenuAction` 全 7 值、`executeTrayAction`(:345-370) 含 Navigate\* 分支、`app_assembly` 的 `pageForTrayAction`/`navigateFromTray`/`trayPageDisplayName`、`app_sidebar` toast 通道。删除后托盘行为：开 = 左键单击直唤主窗（`main.cpp:577-581`）+ 菜单项「显示主窗口」；关 = 菜单项「退出 DevDesk」。
- **验收**：`menu`（17→16）、`tray`（2→1）；`grep -rn "trayMenuSpecFiltered\|isNavigationAction\|trayDisabledHintSuffix" src/ tests/`（**路径限定 `src/ tests/`**；docs 内另有 15 处：`consolidated.md:45/68`、两份 audit JSON、本 spec `:258-267`，不在判死范围）零命中、`grep -rn "已关闭）" src/ tests/` 零命中。
- **偏差**：审计说「HANDOFF/CLAUDE.md 仍在描述它」**不成立**（grep 无命中），真描述在 `docs/USER_GUIDE.md:37` 与 `docs/KNOWN_LIMITATIONS.md:155`（归 L11）。

#### TIN-05（改判：删文件头声明，不补用例）

- **现状**：`tests/test_modules/test_tray_dynamic.cpp:1-11` 文件头列 3 项声明，实际只有 2 个 `REGISTER_TEST`（`:36 trayDynamicGatedSpecAllAvailableMatchesBase` 用 pred 重载、`:54 trayDynamicAppNoticeStateRoundtrip` toast）；`trayMenuSpecFiltered` 零调用零断言，仅 `test_tray_dynamic.cpp:6` 注释 1 处；零命中待本条删注释后成立；`:45 DOCTEST_CHECK(!all[i].disabled)` 是恒真断言。
- **改法**：因 CP-05 让该函数消失 ⇒ 删头注释 `:4-7` 两项声明 + `:2` 的「动态收缩」措辞、删用例 `:36-48`、删辅助 `:26-31 unavailableOnly`（匿名 ns 未用 → -Werror 必删）、删 `:24 using Action`；保留 `:8-9` toast 声明并重编号；文件名不改，头注释标注「托盘收缩链已删（2026-09-27），本文件现只守 toast 通道」。
- **验收**：`menu` Total 17→16、`tray` 1 条全绿；`grep -rn "trayMenuSpecFiltered\|unavailableOnly" tests/` 零命中。

### L10 基建

#### TIN-01（拍板：波 1 内做完整）

- **机制（已实证）**：编进二进制的 `__FILE__` 实为相对路径 `..\tests\test_modules\test_settings.cpp`（`build/build.ninja:4255` 传绝对路径但 ccache base_dir 改写，`.obj` 内 `grep -a` 取证），故 `tests/test_modules/test_settings.cpp:1731` 的候选 = `..`，`:1732` 第 4 次 `parent_path()` 得**空路径**候选；cwd=仓库根时 `:1737 fs::exists(c / marker, ec)` 里 `"" / marker` 退化成裸相对 `src/module_settings/CMakeLists.txt` → **恰好存在** → `:1738` 返回**空 path** → `:1754 root.empty()` 为真报「找不到仓库根」。cwd=`build/tests` 时该候选不存在，才轮到 `:1733-1735` 的 cwd 上溯 → PASS。脚本侧 `scripts/build-test-run.sh:22` 是 `cd "$(dirname "$0")/.."`、`:117` 从仓库根跑测试 → **全量必红**。
- **改法**：新建 `scripts/check-module-isolation.sh`（`SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"`；`ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"`；**全程绝对路径、脚本内不读 `$PWD`、不 cd**；`set -uo pipefail`）。断言：`src/module_settings/settings_module.cpp` 不含 `module_pet/`、`PetConfigStore`、`petConfigFile`、`namespace module::pet`；`src/module_settings/CMakeLists.txt` **非注释行**不含 `devdesk_module_pet`、`devdesk_module_claude_proxy`；必含 `#include "module_pet_bridge.h"` 恰好 1 次；`::module::pet::` × {discoverSkins、applyPetSkin、resetPetPosition、currentSkinName} 各 ≥1；**删掉 `test_settings.cpp:1780` 那条 `claude_service.h` 正向断言**；退出码 `0` 通过 / `1` 违规（打印 `文件:行:命中行`）/ `2` 环境错且**不得静默绿**。单测侧：删 `test_settings.cpp:1717-1795` 整块（含 `:1721-1749` 匿名 ns 的 `findRepoRoot`/`readWholeFile`）——`findRepoRoot` 全仓唯一定义+调用，无同符号重叠。`scripts/build-test-run.sh` 测试阶段后串跑该脚本、失败 `exit 1`；在 `CLAUDE.md`「常用命令」登记。**不保留 headless 冒烟用例**（同类 `__FILE__` 相对化陷阱必复发）。
- **验收**：脚本在 cwd=仓库根 / cwd=`build/tests` / cwd=`$TEMP` 三处退出码一致 = 0；用 `--root DIR` 指向仓库外 `cp -r` 拷贝树验证红路径（退出码 1 + 打印命中行）；全量 → `Total: 620 / Failed: 0`。
- **存疑（未取证）**：全量那 1 个 Failed 未运行验证归属（机制上必然是与负载无关的该守卫）；ccache base_dir 改写是否对所有 TU 都产出相对 `__FILE__`（只验了本 TU）。

#### TIN-04

- **现状**：`tests/test_modules/test_md_block_model.cpp:374` 描述含「< 8ms」、`:401-403 DOCTEST_CHECK_MESSAGE(warmUs < 8000, ...)`；参照样例 = 已改好的 `tests/test_modules/test_md_preview_highlight.cpp:311-318`（注释「原 computeMs < 8.0 改为非时间断言」+ printf 记录 + `DOCTEST_CHECK(hl.ok)` / `!hl.runs.empty()`）。
- **改法**：`:401-403` 换 `std::printf("[perf] md_block_model: cold=%.1fms warm=%.1fms（预算 8ms；记录式，不设墙钟红线）\n", coldUs/1000.0, warmUs/1000.0);`（`coldUs` 保留用于打印，防 -Werror 未使用变量）；保留 `:399-400` 并**补确定性断言**（二次分段块序列与首次逐块等价：stableId/kind/start/end 比等）；`:374` 描述去「< 8ms」。
- **风险**：删掉的是该文件唯一性能证据，必须用等价性断言补位，不能只 print。
- **验收**：`md_block_model` 全绿且输出含 `[perf] md_block_model:` 实测行；连跑 5 次不红。
- **存疑（未取证）**：`warmUs` 是否真会越过 8000（未采样）。

#### TIN-09

- **现状**：`tests/test_modules/test_todo.cpp` 全文 28 行、**仅 1 个用例** `:7 REGISTER_TEST(todo_pageid, "todo: PageId ordering", ...)`，体内全是 `core::PageId` 序断言（`:16-26`）；`src/core/module_interface.h:11-17 enum class PageId { Home, Markdown, Todo, ClaudeProxy, Settings }`、`:19-28 pageIdToString`；`tests/` 下 3 个子目录 `test_core/`(11 文件)、`test_platform/`(6)、`test_modules/`(37) 共 54 个用例 .cpp，`tests/CMakeLists.txt:1-56` 手工源表。
- **改法**：整块并入 `tests/test_core/test_router.cpp` 末尾（**该文件 `:7` 已 include `core/module_interface.h` 且大量用 PageId → 零新增依赖、零 CMake 新增**），改名 `core_pageid_order`、描述改 ASCII 可过滤 `"core: PageId ordering contract (Home<Markdown<Todo<ClaudeProxy<Settings)"`；删 `tests/test_modules/test_todo.cpp` 并从 `tests/CMakeLists.txt` 删对应一行；`src/core/module_interface.h:11` 上方加指针注释指向 `core_pageid_order`。
- **风险**：CMake 漏删 → configure 期响亮报错（非静默）；用例数不变。
- **验收**：`PageId` 命中 1 条通过；`todo` 不再命中 PageId 契约；全量 Total 不变。

#### TIN-11

- **现状**：`tests/test_platform/test_http_client.cpp` 全文 30 行 / 3 条用例，只回显结构体默认值（`:4-13 http_build_request` 断言 method/url/timeoutMs==30000/verifyTls，**从不发送**）；产品侧错误语义 `src/platform/http_client.cpp:95-103`（`rc != CURLE_OK` → `networkError=true` + `NetworkUnreachable`）、`StreamingResponse::readChunk` 三终态 `:355-372`。
- **改法**：同文件追加 3 条（不动 CMakeLists）：(a) `http_send_refused_local`（用 `platform::http::Server("127.0.0.1",0)` 取 `actualPort()` 后 `stop()`，对该端口 `send()` → 断言失败、`errorMessage()` 非空、耗时 < 5s）；(b) `http_send_timeout_local`（本地 Server 路由 `GET /slow` handler 内 `sleep_for(1500ms)`，`Request.timeoutMs = 300` → 断言失败且 elapsedMs < 1000，**不断言文案**）；(c) `http_stream_error_terminal`（`sendStreaming` 打关闭端口 → `readChunk` 返回 `Error`、错误串非空，再读一次仍 `Error`；并 `cancelWithReason("test")` 打长驻本地流 → 返回 `Error` + 该 reason）。用例内设 `no_proxy=127.0.0.1,localhost`（照 `test_claude_service.cpp` 的 `setEnvVar` 作用域还原写法）。
- **风险**：`http_client.cpp:45-48` 用 `call_once` 自管 curl 全局；`test_http_server.cpp:20-29` 的 `CurlGlobalInit` 是函数局部 static → 新用例**不再引入自己的 `curl_global_cleanup`**。
- **验收**：`http_` 6 条全绿；在 `HTTP_PROXY=http://127.0.0.1:1 ALL_PROXY=http://127.0.0.1:1` 下复跑仍全绿。

#### PAB-06（拍板：System32 存在性判据）

- **现状（实测白名单）**：`objdump -p build/DevDesk.exe | grep "DLL Name"`（2026-09-27 23:22 产物）：17 项全系统 DLL（ADVAPI32、CRYPT32、GDI32、IMM32、IPHLPAPI、KERNEL32、SHELL32、Secur32、USER32、WINMM、WLDAP32、WS2_32、bcrypt、comdlg32、dwmapi、msvcrt、pdh）；无守护（`grep -rn objdump scripts/ cmake/ CMakeLists.txt` 零命中）；`CMakeLists.txt:83 option(DEVDESK_STATIC ... ON)` 可被单点关掉而无提示。
- **改法**：`scripts/build-test-run.sh` 在 `echo "✅ 构建成功"`（约 :112）之后、测试段（约 :114）之前插入「2b. 发布形态守卫」：取 `objdump -p build/DevDesk.exe | awk '/DLL Name:/{print $3}'`，**判据 = 该 DLL 名是否存在于 `%SystemRoot%/System32`**（用 `SystemRoot` 拼路径 + `-f` 判定），凡不在 System32 的即失败并 exit 1（MSYS2 升级新增 `api-ms-win-*` 不会误报）；另加 `grep -q "DEVDESK_STATIC:BOOL=ON" build/CMakeCache.txt` 断言。
- **风险**：只查 `DevDesk.exe`，不查 `devdesk_tests.exe`（测试产物不入发布约束）。
- **验收**：正路打印「✅ 发布形态」；反证在 worktree 里 `cmake -B build -DDEVDESK_STATIC=OFF` 后跑脚本应 exit 1。
- **存疑（未取证）**：`DEVDESK_STATIC=OFF` 时实际新增的 DLL 清单（未做该构建）。

#### PAB-11

- **现状**：`scripts/build-test-run.sh:97-102` 传 **5 个 `-D`**（`:97/:98/:100/:101/:102`；`:99` 是 `CCACHE_ARGS`），其中 `:101 -DZLIB_LIBRARY=C:/msys64/mingw64/lib/libz.dll.a`、`:102 -DZLIB_INCLUDE_DIR="$EUI_DIR/3rd/zlib-1.3.1"`；**产物不受影响（实证）**：`build/build.ninja` 中 `libz.a` 出现 4 次、`libz.dll.a` **0 次**，`build/CMakeCache.txt:550` 的 `ZLIB_LIBRARY:UNINITIALIZED` 只是缓存残留；真正进链接线的是 `cmake/Dependencies.cmake:119`（curl 静态闭包）+ `:123-126`，DevDesk 侧无任何 target 链 `ZLIB::ZLIB`。
- **改法**：删 `:101`、`:102` 两行，保留 `:98 -DEUI_NEO_DIR`、`:99 CCACHE_ARGS`、`:100 -DOPENSSL_ROOT_DIR`。
- **风险**：只影响 `--clean`/首建 configure；缓存值由 `libz.dll.a` 变 `libz.a` **不改产物**。
- **验收**：`grep -n "ZLIB" scripts/build-test-run.sh` 零命中；worktree 跑 `--clean --no-run` 成功且 `objdump` DLL 集与 17 项逐项一致。
- **存疑（未取证）**：删后 configure 是否冒新 WARNING。

#### TIN-13（拍板：补齐实现，Windows 化）

- **现状**：`scripts/check-consistency.sh`（236 行）三段：① Playwright headless Chromium 截 `docs/mockups/devdesk-prototype.html`（`:86-123`）② 启 `build/DevDesk` + `screencapture` 截屏（`:126-214`，失败回退 Pillow 合成占位图）③ 打印**硬编码**结论（`:223-233`，仍写「C++ 4 个 module_\* 的 compose() 是 stub」——已过期）。macOS 硬依赖：`:18` 前置声明、`:27 CPP_BIN=build/DevDesk` + `:61 -x`（Windows 无扩展名 → 直接 exit 1）、`:66 python3`、`:133 open -a`、`:138 screencapture`、`:157-159` macOS 字体、`:212 pkill`、`:88/121/145/207 /tmp/*.py`、`:26 HTML_URL=file:///d/...`。本机 Python 3.14.7 存在但 **PIL / playwright 均未安装**（实测 ModuleNotFoundError）。
- **改法**：`CPP_BIN` 按平台取 `build/DevDesk.exe` 并用 `-f`（非 `-x`）；`open -a` → 直启；`screencapture` → 复用仓库既有 Windows 范式 `scripts/probe_winfx.ps1`（`:22/37/172-177` 的 `SetProcessDPIAware + EnumWindows + GetWindowTextW + Graphics.CopyFromScreen + Bitmap.Save(Png)`，需为它加 `-ShotMain` 开关或新增 `scripts/capture-window.ps1`）；`pkill` → `taskkill -F -IM DevDesk.exe`（同 `build-test-run.sh:81`）；临时目录 → `mktemp -d`；`HTML_URL` 走 `cygpath -m`；结论段 `:223-233` 改写成实况或改为读 `docs/CONSISTENCY-CHECK.md`；playwright/PIL 缺失 → 降级 WARN 不 fail。**必须同步**改 `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`：`:105` 按本次拍板「补齐实现」改写（**该行不含「② 标注」**，「② 标注」唯一在 `:191`）、`:104`（TIN-12 finding 行）同步更新 → 该文档编辑归 L11。
- **风险**：截屏需真实交互桌面会话，RDP 断开/锁屏时 `CopyFromScreen` 可能黑图（未取证）；透明帧缓冲窗口可能截到桌面背景（未取证）。
- **验收**：`bash scripts/check-consistency.sh --cpp-only`（**不带 `--no-launch`**：Pillow 合成在 `:143-208` screencapture 回退分支内，带 `--no-launch` 不产 PNG）exit 0 且生成非 0 字节、非全黑 PNG——或把截图生成移出回退分支后继续用 `--cpp-only --no-launch`；`grep -n "screencapture\|open -a\|pkill" scripts/check-consistency.sh` 零命中。

### L11 docs（最后跑）

#### TIN-12

- **现状**：`CLAUDE.md:198` 写「共 **530 个用例**（2026-09-27 在 master 上实测 530/530 全绿…）」；`CLAUDE.md:176/178/179` 是带日期的历史里程碑（保留）；`CLAUDE.md:52/:54`（ccache 实测段，同段重复出现两处）写「589/589 绿」；`docs/superpowers/plans/2026-09-27-HANDOFF.md:26/:30/:64/:71/:84/:88/:129` **7 处** `530`（`:192` 是表内历史行，保留）；`CHANGELOG.md:114/128/147-148` 流水账（**不动**）。**本次静态复核**：`grep -rn "^\s*REGISTER_TEST(" tests/{test_core,test_platform,test_modules}` = **621**，与运行输出 `Total: 621` 一致（旧审计「静态 561」是过期快照）。
- **改法**：`CLAUDE.md:198` 改「用例总数**以运行输出 `Total:` 为准，不在文档写死**（2026-09-27 实测 `Total: 621 / Passed: 619 / Failed: 1`，其中 Failed 为模块隔离守卫的 cwd 缺陷，已由波 1 修复）；481/514/530 仅存史」；HANDOFF **7 处**就地改「以运行输出为准（当时快照 530）」或回填实测数；`CLAUDE.md:52/:54` 两处「589/589 绿」同 `:198` 口径改；CHANGELOG/`expert-audit.md`/`md-phase3-m0-spike.md` 不改。**未纳入本轮**：`README.md:12`、`docs/KNOWN_LIMITATIONS.md:3`、`docs/USER_GUIDE.md:3` 的 `330/330`（登记待后续）。

#### CP-05 文档

- **现状**：`docs/USER_GUIDE.md:37` 现写「显示主窗口 / 首页 / 笔记 / 待办 / Claude / 退出 DevDesk（被关闭的模块置灰并带「（已关闭）」后缀）」。
- **改法**：改为两项；`docs/KNOWN_LIMITATIONS.md:155` 删「+ 按模块动态收缩」短语。

#### CP-12 登记

- **改法**：`docs/KNOWN_LIMITATIONS.md` 新增条目「macOS 侧 `system_theme_macos.cpp` 编译未验证（本机无 macOS 工具链）」+ 验证条件。

#### G8/G9 行为变更说明

- **改法**：在 `docs/KNOWN_LIMITATIONS.md` 或 `HANDOFF.md` 记明「接管/还原在 settings.json 存在但读不出时由静默重写改为中止」。

#### TIN-01 登记

- **改法**：`CLAUDE.md`「常用命令」段加 `bash scripts/check-module-isolation.sh`。

#### consolidated 修正

- **改法**：`docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`：`:191` 的「② 标注」改标「已拍板：补齐实现（波 1 L10/L11）」；`:105` **不含「② 标注」**（该措辞唯一在 `:191`），按 TIN-13 拍板改写；`:104`（TIN-12 finding 行）同步更新；并在 §6 表注明其余已拍板项（CP-05 删 / THS-12 删负位 / THS-14 补 kind / THS-11 实现 / THS-13 max(1) 文案不变 / THS-17 零迁移版 / PAB-06 System32 判据 / PAB-08 保留 / THS-15 切 core + 码映射 / 新增 NOPROXY 项）。

## 4. 风险与未取证登记

| 项 | 风险 / 未取证内容 |
|---|---|
| CP-12 | macOS 不可编译：本机为 Windows，`system_theme_macos.cpp` 该 TU 无法编译验证（未取证）；无 GUI 会话/SSH 下 `CFPreferencesCopyAppValue` 返回值未取证 |
| THS-16 | DST 不可复现：本机为 CST（无 DST），DST 修复效果无法复现 |
| THS-14 | 已改名库回填落空：升级前已改过归档列名的库，v8 按 title 回填落空（与 v4 同病，不引入新机制） |
| TIN-13 | 截屏黑图与 playwright 缺失：RDP 断开/锁屏时 `CopyFromScreen` 可能黑图（未取证）；透明帧缓冲窗口可能截到桌面背景（未取证）；「PIL/playwright 未安装」只对 MSYS2 python3(3.14.7) 成立——Python311 已装 PIL 12.2.0 + playwright 1.60.0，但脚本 pip3 指 3.11、实际运行用 3.14 → 解释器错配，待人工判 |
| FileWatcher | 回调析构 UB 未复现：detach 后 `running` 串改与 Impl 生命周期两项由本批侦察补出，**未做运行期复现** |
| CPX-12 | 噪音统计：真实环境的 `*_NAME` 出现频率未取证 |
| CPX-09 | 256 条未联网核对：`kEnvVarHints[]` 实测 256 条是否 100% 官方来源（禁联网未核对） |
| TIN-11 | NOPROXY 环境变量实际影响：本机 curl 读 `http_proxy`/`no_proxy` 已实证，但公司代理实际劫持路径未取证 |
| PAB-13 | 最小窗宽：卡内水平占位 28+240+8+160+8+160+28 = 632（未取证最小窗内容宽）；无头下 `onWindowCreated` 是否触发未取证 |
| PAB-06 | OFF 态 DLL 清单：`DEVDESK_STATIC=OFF` 时实际新增的 DLL 清单未取证（未做该构建） |
| CPX-11 | 过滤词命中面与用例落点：过滤器匹配 description（`tests/test_main.cpp:89`），`streaming` 还会命中 `stress_body_10mb_streaming_e2e`；新用例是名称，落点建议既有 `test_streaming_proxy.cpp` / `test_upstream_dialect.cpp` 且描述串带 streaming/dialect |
| THS-17 | 语法先例与头文件缺口：`static_assert(std::size(...))` 在 `src/` 无先例；`todo_module.cpp:26-33` 未 include `<iterator>` / `<array>` |
| TIN-12 | 全量输出无落盘：全仓无落盘的全量运行输出，`Total 621 / Passed 619` 仅见本 spec；旁证 `2026-09-27-open-questions-review-A.md:58-64` |

## 5. 交付与审计流程

1. 起草：本 spec（已列全部线、条目、file:符号级改法）。
2. 独立审计子代理：逐条核 spec vs 代码（file:符号取证，出 CONFIRMED/REFUTED）。
3. 修 spec：按审计结论修正。
4. `writing-plans` 出实施 plan（每 step 完整代码块）。
5. 按 L1–L11 派 worktree（git worktree 隔离 → 定向构建 `CMAKE_BUILD_PARALLEL_LEVEL=3` → 只跑该线过滤词 → 显式 pathspec 原子 commit）。
6. 主会话审计合并清 worktree（串行合 master → `git worktree remove` + `git worktree prune`；全量只在合并关口由主会话跑）。

审计口径：只审 spec 与代码事实是否一致 + 范围纪律，不审实现风格。
