# DevDesk 三视角审计 —— 修复方案（2026-11-21）

> 上游：`2026-11-21-audit-cpp.md` / `2026-11-21-audit-uiux.md` / `2026-11-21-audit-enduser.md` / `2026-11-21-audit-mainsession-facts.md`
> 本文只出**方案与优先级**，不含实现（实现走各自条目派发）。
> 判据：先修 P0/P1（真实缺陷），再修机制（防复发），最后修体验（一致性）。

---

## 0. 总览：15 条发现 → 9 个修复批次

| 批次 | 内容 | 来源 | 优先级 | 预估 |
|---|---|---|---|---|
| **B1** | 壁纸服务退出崩溃（P0） | C++ P0-2 | **P0 立即** | 3 行 |
| **B2** | Windows 回收站标志位未校验（P1） | C++ 子代理 P1 | **P1** | 5 行 |
| **B3** | 模块隔离守卫名不副实（P1） | C++ P1-02 | **P1** | 30 行 |
| **B4** | 异常纪律两处（try/catch + 内部字段名） | C++ P1-01/P1-03 | **P1** | 约 60 行 |
| **B5** | 平台编码纪律（SQLite/壁纸路径 UTF-8） | C++ 子代理 P1 | **P1** | 约 40 行 |
| **B6** | ARCHITECTURE 表与 54 处 module_→eui 实践互斥 | C++ 子代理 P1 | **P1 文档** | 半天 |
| **B7** | z 序具名常量层 | C++ P1-04 / UI U-03 | P2 | 约 80 行 |
| **B8** | 文档-实现对齐（4 项数字 + 截图重建） | UI U-01/U-02 / 用户 E-08 | P2 | 1 天 |
| **B9** | 体验补齐（空态 3 处 / 首启引导 / 术语） | UI U-05 / 用户 E-01/E-02/E-03 | P2 | 2-3 天 |

**不做/待裁决**：侧边栏是否分组（U-07）、260 vs 200 哪个是对的（U-01）、Git/终端是否可关（E-08）。

---

## 1. B1 —— 壁纸服务退出崩溃（P0，立即修）

### 缺陷链（已在代码中确认）

```
requestDeleteCurrent()                        // :575-620，无 started_ 前置门
  └─ deleteThread_ = std::thread(...)         // 服务已 stop 也照样创建
       └─ deleteCurrentItem()                 // :489 见 !started_ → 立刻 return NoCurrent
            （线程体执行完，线程对象仍 joinable）

~WallpaperService() → stop()                  // :50-52
  └─ :163 早退判据 = !started_ && !scanThread_.joinable() && !tickThread_.joinable()
       ↑ 不看 importThread_ / deleteThread_
     → 早退 return，跳过 :190 的 deleteThread_.join()
  └─ 成员 std::thread deleteThread_ 析构 → joinable → std::terminate()  💥
```

**触发条件**（真实可达）：托盘菜单已展开时点了「退出」，随后确认弹窗晚到并点了确认；或 `stop()` 与确认回调竞态。

### 修法（两处，任一即可，建议全做）

**改法 A（必需，防御性，3 行）**——早退判据补全所有线程：

```cpp
// src/module_wallpaper/wallpaper_service.cpp:163
if (!started_ && !scanThread_.joinable() && !tickThread_.joinable() &&
    !importThread_.joinable() && !deleteThread_.joinable()) {
    return;
}
```

**改法 B（推荐，治本）**——给 `requestDeleteCurrent()` 加前置门，服务已停就不起线程：

```cpp
// src/module_wallpaper/wallpaper_service.cpp:575 起，CAS 前插入
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;              // 服务已停：删除无意义，且不让线程对象诞生
}
```

**为什么不只做 A**：A 保证不崩溃，但仍会在退出后白起一个线程跑一次空操作。B 从源头避免。

### 验证方式

新增 doctest 用例（`tests/test_modules/test_wallpaper_delete.cpp`）：
1. `start()` → `stop()` → 调 `requestDeleteCurrent()` → 断言 `waitDeleteIdleForTest()` 通过且无异常
2. 服务从未 start 时直接 `requestDeleteCurrent()` → 断言不崩、不建线程
3. 析构路径：构造→stop→析构，断言无 terminate（需在测试进程里可观察，或用 `--filter` 单独跑）

### 风险

极低。两处都是加严判据，不改变正常路径行为。

---

## 2. B2 —— Windows 回收站标志位未校验（P1）

### 缺陷

`src/platform/file_trash_win.cpp:169`：
```cpp
op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                      FOF_NOERRORUI | FOFX_RECYCLEONDELETE);   // ← 返回值丢弃
```

`SetOperationFlags` 失败时，后续 `DeleteItem` 可能走**无 FOF_ALLOWUNDO 语义**的路径 → 直接删除而非进回收站。**破坏"误删可回收"的产品前提**。

### 修法

```cpp
const HRESULT flagHr = op->SetOperationFlags(
    FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_RECYCLEONDELETE);
if (FAILED(flagHr)) {
    attempt.createOp = flagHr;   // 归入失败档
    op->Release();
    sinkRef->Release();
    if (comInitialized) ::CoUninitialize();
    return attempt;
}
```

另建议补一条**兜底断言**：若 `PostDeleteItem` 从未被调用（`attempt.itemDelete` 仍是初值）且 `PerformOperations` 返回 S_OK，视为不确定 → 归 `TrashUnavailable`，不当作成功。宁可报"不确定"，也不能谎报"已回收"。

### 验证

Windows 真机 probe（计划里的 `tests/probes/wallpaper_trash_probe.cpp` 未落地；仓内现行 `tests/probes/wallpaper_trash_macos_probe.cpp` 为 macOS 版）：临时文件 → `moveToTrash` → 断言回收站里能找到该项。若今条件不具备，至少加静态断言 + 代码注释说明"标志位失败即失败"。

---

## 3. B3 —— 模块隔离守卫重建（P1，机制层）

### 现状

`scripts/check-module-isolation.sh` 名为"模块隔离"，实际只断言 2 组点对点关系（settings→claude 一条边、agent 一个中心点），11 个模块（>60% 代码）无任何约束。且残留 `module_pet`（已退役模块）死引用。

### 修法：改为动态全枚举

```bash
# 在现有 B/C 段之前插入「全模块互斥」段
MAPS="$ROOT/src"
MODULES=""
for d in "$MAPS"/module_*; do
    [ -d "$d" ] || continue
    MODULES="$MODULES $(basename "$d")"
done

for own in $MODULES; do
    own_dir="$ROOT/src/$own"
    for other in $MODULES; do
        [ "$own" = "$other" ] && continue
        hits="$(grep -rn --include='*.h' --include='*.cpp' --include='*.mm' \
                     -F "#include \"$other/" "$own_dir" || true)"
        [ -n "$hits" ] && while IFS= read -r line; do
            report "$(rel "${line%%:*}")${line#*:}"   # 保留行号
        done <<< "$hits"
        # 也查 <> 形态（虽约定用 ""，防绕过）
        hits2="$(grep -rn --include='*.h' --include='*.cpp' -F "#include <$other/" "$own_dir" || true)"
        [ -n "$hits2" ] && report "疑似 <> 形态跨模块 include: $hits2"
    done
done
```

### 配套三条

1. **删死引用**：清掉 `module_pet`（已退役模块）/ `module_pet_bridge` 相关断言与注释（`src/module_pet` 已不存在）。
2. **补 `--self-test` 坏树**：加一棵 `cross_module_include` 坏树（在 `module_wallpaper` 里插一行 `#include "module_todo/..."`），断言恰好红。
3. **补平台宏守卫**（子代理 P2 提议，一并做）：`src/module_*/`、`src/ui_kit/`、`src/app_*.cpp` 零 `#ifdef _WIN32|__APPLE__`，白名单只放 `*_win.cpp|*_macos.*|*_posix.cpp|*_stub.cpp|*_mm`。

### 验证

```bash
bash scripts/check-module-isolation.sh        # 应绿（当前代码干净）
bash scripts/check-module-isolation.sh --self-test   # 应全绿（含新坏树）
```

---

## 4. B4 —— 异常纪律两处（P1）

### B4-a `http_client.cpp` 的 try/catch + std::stoi

**位置** `src/platform/http_client.cpp:323-327`

**修法**：换手写解析（照抄仓内范式 `module_todo/todo_view_state.cpp:20`）：

```cpp
// 状态行第三段是 HTTP 状态码（3 位十进制）。手写解析：本仓铁律不碰异常，
// 且此处位于 libcurl 的 C 回调，异常越过 C 边界是 UB。
int code = 0;
bool ok = !codeStr.empty() && codeStr.size() <= 4;
for (char c : codeStr) {
    if (c < '0' || c > '9') { ok = false; break; }
    code = code * 10 + (c - '0');
}
if (!ok) code = 0;
```

### B4-b 内部字段名泄漏进用户可见文案

**位置** `src/core/schedule_rule.cpp:249/256/289/317/324/344`

**修法（分两层，不必全仓改造）**：

1. `core/` 层消息保留技术细节（入日志）——**不动**
2. 新增 `src/core/user_message.h`，提供：
   ```cpp
   // 把 ErrorCode + 上下文字段名翻译成用户能懂的话。
   // 已知字段 → 具体指引；未知 → 兜底通用话术（不暴露内部名）。
   std::string humanize(ErrorCode code, std::string_view fieldContext);
   ```
   映射示例：
   - `once.at_local` → 「执行时间格式不对，请按 `2026-11-21 09:30` 这样的格式填写」
   - `calendar.unit` → 「重复周期填错了，请选择 天 / 周 / 月」
   - `calendar.workdays_only` → 「工作日设置无效，请勾选 1-7 中的一个或多个」
3. `schedule_rule.cpp` 的 6 处消息改写为人话（技术细节移入 `LOG_WARN`）

**范围控制**：只改 schedule 这一族（字段名暴露型）。网络/DB 类错误消息本身可读，不动。

### 验证

- 新增 doctest：`humanize()` 对 6 个已知字段返回不含内部名的文案、对未知字段返回兜底话术
- 现有 schedule 规则解析用例保持绿（错误码不变，只改消息文本）

---

## 5. B5 —— 平台编码纪律（P1，Windows 中文用户名）

### 缺陷

`src/core/sqlite_db.cpp:265` 与 `src/core/db_backup.cpp:32`：
```cpp
sqlite3_open_v2(file.string().c_str(), ...)
```

`sqlite3_open_v2` 要求 **UTF-8**，而 Windows 上 `fs::path::string()` 按本机 ANSI 代码页（ACP）转换。数据根 `%USERPROFILE%\DevDesk` 在中文用户名机器上 → 传进去的路径编码错误 → 打开的是乱码路径对应的另一个库（或 `SQLITE_CANTOPEN`）。

**典型症状**：「我的配置全没了」/「首次启动建了空库」——而且开发机是英文用户名，**永远复现不了**。

### 修法

在 `src/core/paths.h` 增统一工具：

```cpp
// fs::path → UTF-8 std::string（跨平台统一口径）。
// Windows：u8string() 产出 UTF-8；POSIX：narrow 已是 UTF-8。
// 禁止直接用 path::string() 喂给需要 UTF-8 的 C API（sqlite3_open_v2 / libcurl 等）。
inline std::string toUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}
```

改两处调用点。**同时扫全仓同族风险**：所有 `path::string()` 喂给 C API 的地方。

子代理另指出壁纸侧三处（`video_library.cpp:121`、`image_library.cpp:123`、`folder_policy.cpp:34`）最终喂给 mpv JSON 命令，同因——一并改。

### 验证

- 单测：`toUtf8()` 对含中文路径返回正确字节（可在 macOS/Linux 上验 UTF-8 输出，Windows 上验 `u8string` 行为）
- **必须补一条 Windows 真机验证**（中文用户名机器打开 app → 检查数据根路径与日志）——这条无法在开发机替代

---

## 6. B6 —— ARCHITECTURE 表与 54 处实践互斥（P1，文档）

### 矛盾

`docs/ARCHITECTURE.md:66` 依赖表写：「❌ `module_*` → `EUI-NEO`（必须经 core 适配）」
实测：**54 个 `src/module_*/` 文件**直接 `#include <eui/dsl.h>` 等；`src/module_git/CMakeLists.txt:23` 注释还把这个做法当合规解释。

### 判断

**实现是对的，文档过期**。理由：
- 模块必然要画 UI，而 UI 全部是 EUI DSL；经 core 中转需要 core 暴露整套 DSL 类型，违背"core 不依赖 EUI 运行时"的另一条铁律
- 实际约束应该是「module_* 可用 eui 的**头文件/类型/DSL 构建**，但不得 link `eui_neo` 目标、不得自己起事件循环/建窗口」
- 注①已经为 core 开了类型桥接口子，只是没给 module_* 开

### 修法

改 `docs/ARCHITECTURE.md` §2 依赖表：

| 原 | 改 |
|---|---|
| ❌ `module_*` → `EUI-NEO`（必须经 core 适配） | ✅ `module_*` → EUI-NEO **头文件（DSL / 类型 / 组件签名）**<br>❌ `module_*` → EUI-NEO **运行时入口**（事件循环 / 窗口 / 渲染后端）<br>❌ `module_*` link `eui_neo` 目标（include 经 core PUBLIC 传播） |

并在注①后追加注②说明该边界与判据（可静态检查的是"是否 link eui_neo"）。

### 验证

文档改动，无代码影响。合入时在 commit message 里写明"追认既有实践，非放宽约束"。

---

## 7. B7 —— z 序具名常量层（P2）

### 修法

`src/ui_kit/metrics.h` 增：

```cpp
// 弹窗/覆盖层层级（单一来源）。新增层级必须在此登记，禁止在调用点写裸数字。
// 层级语义见 docs/design/2026-11-07-dialog-standard-design.md。
inline constexpr int kZBase          = 1000;  // 常规弹窗（DialogShell 缺省）
inline constexpr int kZUnderDialog   = 990;   // 需压到常规弹窗之下（罕见，须注释理由）
inline constexpr int kZStackedDialog = 1100;  // 叠在另一弹窗之上的二级确认
inline constexpr int kZOverlay       = 1200;  // 应用级覆盖层（退出确认 / 壁纸删除确认）
inline constexpr int kZToast         = 1100;  // toast（与 stacked 同档，靠绘制顺序分开）
inline constexpr int kZSplash        = 1500;  // 启动 splash（必须盖住一切）
```

改 7 个调用点引用。给 `schedule_ui_history.cpp:446` 的 990 补意图注释，或复核是否该升到 1000。

### 验证

纯常量替换，行为等价。跑 `--filter dialog` 或相关 UI 用例确认绿。

---

## 8. B8 —— 文档-实现对齐（P2）

| 项 | 现状 | 改成 |
|---|---|---|
| 侧边栏宽 | 规范 260 / 指南 220 / 代码 200 | **待老板裁决**：若接受 200，三处统一 200 并把值提到 metrics.h；若想 260，立需求改代码 |
| 最小窗口 | 规范 1200×720 / 代码 1000×640 | 规范改 1000×640 + 附收窄理由链 |
| 模块可插拔数 | 指南 9 / README 9 / 代码 11 | 统一 11，列出全部（补 Git、终端） |
| 测试数 | README 2048（10-31） | 改动态指针（指向 CI 徽章），不写死 |
| 截图 | 10-29，6 项侧边栏 | **重建**：用 `ui_driver` 拍当前 14 项版本；每图记 commit |

### 截图重建方案

仓内 `tests/ui_driver/` 已有场景驱动与截图能力。建议：
1. 建 `tests/scenarios/screenshots.json`（tests/scenarios/ 目录当前不存在，随本项创建）：列出要拍的页面与视口尺寸
2. 脚本跑完输出到 `docs/screenshots/` 并覆盖旧的
3. 在 CI 的 release 档跑一次自动更新（或手动触发）

**若短期不做自动化**：至少手动拍一轮替换旧图，并在文档里标注"截图基线 commit"。

---

## 9. B9 —— 体验补齐（P2）

### B9-a 空态 3 处改统一组件

| 模块 | 现状 | 改为 |
|---|---|---|
| claude_proxy | `claude_ui_list.cpp:674` 单行「暂无 Profile，点右上角 + 新建」（方位词可能过时） | `EmptyStateOptions{icon, message, guide, action={"新建 Profile", ...}}` |
| terminal | `terminal_page.cpp:978` 单行「没有终端会话」 | 同上，action 指向「+ 新建会话」 |
| notepad | 无空态 | 补「还没有打开任何文件」+ 引导（无新建钮 = N-14 硬约束，故只给引导不给按钮） |

### B9-b 首启引导（用户 E-02）

零数据时首页加引导区：
- 3 个入口卡：记一条笔记 / 加一个待办 / 配置 Claude 模型
- 一句话产品定位
- 可选：「用不上的模块可以在设置里关掉」提示

**触发条件**：笔记数 = 0 且待办数 = 0 且无 Profile → 显示引导区；有任一数据后自动隐藏。

### B9-c 术语 tooltip（用户 E-03）

Claude 页四个术语加 tooltip：`Profile` / `Base URL` / `接管` / `共享配置`。框架已有 TooltipBuilder，成本低。

### B9-d 无障碍现状写进用户指南（UI U-08）

`USER_GUIDE.md` §5 加一条：「DevDesk 目前不支持屏幕阅读器与全键盘 Tab 导航（现状与规划见 `docs/ACCESSIBILITY.md`）」。

---

## 10. 需老板裁决的 3 项

| # | 问题 | 选项 | 我的推荐 |
|---|---|---|---|
| **A** | 侧边栏该多宽？ | ①接受代码现状 200px，改文档；②按规范 260px 改代码；③取中间 220px | **①** 改文档。260 是 v1 五模块时代的规范，14 模块下 260 会更挤。且代码 200 是实际调过的 |
| **B** | 14 个模块平铺是否要分组？ | ①接受平铺；②加分组（创作/工程/系统）；③加折叠二级 | **②** 但**不急着做**——等模块数再涨或用户明确抱怨。先记录问题 |
| **C** | Git / 终端是否真该可关？ | ①保持可关（现状 11 个开关）；②改成恒开（代码删 kModuleId） | **①** 保持可关。多一个能关的开关不伤害谁，但文档必须同步 |

---

## 11. 执行顺序建议

```
第 1 波（今天/明天，止血）
  B1 壁纸退出崩溃 ──────────── P0，3 行，最高优先
  B2 Windows 标志位校验 ────── P1，5 行

第 2 波（本周，修机制防复发）
  B3 守卫重建 ─────────────── P1，机制层，防止未来同类违规
  B4 异常纪律两处 ─────────── P1
  B5 平台编码 ─────────────── P1，Windows 真机验证不可省

第 3 波（下周，文档与一致性）
  B6 ARCHITECTURE 追认 ────── P1 文档
  B8 文档对齐 + 截图重建 ──── P2（除截图外都是小改）
  B7 z 序常量层 ───────────── P2

第 4 波（体验，可穿插）
  B9 空态 / 首启引导 / 术语 / 无障碍说明 ── P2
```

**并行约束**：B1/B2/B4/B5 都碰不同文件，可并行；B3 只碰守卫脚本；B7 碰 ui_kit + 7 个模块（注意与其它 UI 改动避让）。

---

## 12. 不建议做的事（避免过度工程）

1. **不要全仓重构 token**（U-04 的 673 处硬编码）——只给新代码定规矩 + 加趋势守卫，旧代码随改动替换。一次性重构 673 处风险远大于收益。
2. **不要拆所有 1500+ 行文件**——只拆正在频繁改的那几个。
3. **不要把 1443 处 errorMessage() 全改**——只改字段名暴露型（schedule 一族）。
4. **不要为 z 序做运行时校验框架**——具名常量 + 注释足够。
5. **不要为 doctest shim 重写框架**——先只补"全部失败都上报"，迁移官方 doctest 单独立项评估。
