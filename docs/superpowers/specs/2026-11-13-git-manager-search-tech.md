# Git 管理器 v2 全局搜索 技术方案卡（need:14 / PRD doc#6 v3 F-08）

> 2026-11-13 developer 出稿。上游：PRD doc id=6 v3（F-08 验收口径 + §6 顶栏搜索框契约位）、UI 权威=方案 A `2026-11-13-git-manager-readonly-ui-a.md`（§5.1 顶栏）、v1 技术 `2026-11-13-git-manager-readonly-tech.md`。口径来源：need:14 简报已证事实（v1 全绿基线 file:test_git_ 51/51；clue#14 内容锚 / clue#22 行盒幂等；bug#21 池 id=21）。

## 0. 结论

**复用面近乎全覆盖，新增代码集中在一块纯函数匹配模型 + controller 搜索状态面 + 顶栏搜索框/结果浮层 UI。** 匹配=纯子串不分词（ASCII 大小写不敏感）；后台任务走既有 GitWorker 单线程队列，取消=共享请求序号（contentReq_/diffReq_ 同型）；过滤判据零新写（walkChildren + readFromDisk 全复用）；点击结果复用 N6（selectTreeFile）+ 内容锚定位（clue#14 口径）；bug#21 顺路同批修、独立 commit。

## 1. F-ID 对照

| F-ID | 内容 | 本方案落点 |
|---|---|---|
| F-08① | 搜索框→全仓内容匹配→结果列表 | §7.1 顶栏搜索框 + §6 结果模型 |
| F-08② | 点击结果→定位 | §6.3 N6 复用 + 内容锚 |
| F-08③ | 大仓后台线程不冻 UI + 渐进出果 | §2 数据流（增量 flush） |
| F-08④ | 可取消 | §3 三处取消路径 + ESC |
| F-08 四态 | 空/扫描/取消/错误 | §7.2 结果浮层四态设计增补 |
| bug#21 | diff 横滚按字节当字符（CJK 假横滚 ~70%） | §8 顺路修 |

## 2. 数据流（walk + match 后台任务）

```
UI 线程                                    worker 线程（GitWorker 单线程队列，零改动）
────────────────────────                   ────────────────────────────────────────────
搜索框输入 debounce 300ms（或 Enter）
  → controller.submitSearch(q)
      searchSeq_++（atomic）
      q 为空 → 清结果回无搜索态，不投任务
      快照 untracked 目录清单（来自 tree_
        现有 statusByPath_，见 §4）
      worker.post(搜索闭包)  ────────────→  ① ensureHandle（既有语义）
                                              ② walk：迭代栈从根逐目录复用
        composes 说                            GitTreeModel::walkChildren 枚举
        composePump() 排 inbox              （同 .git/ignore/symlink/Subrepo/
        增量 append searchResults_           NoAccess 语义）→ 全量文件清单
        更新进度 scanned/total               ③ match：逐文件
                                                FileContent::readFromDisk（既有四态判据）
                                                state != Ok → skip；Ok → 逐行
                                                std::search(行, needle, ASCII 折叠比较器)
        ←── 每 32 文件或 250ms 一包 ──────     命中 → SearchHit 入批
        （postToUi 闭包：hits + 进度）         每文件边界检查 seq ≠ 捕获值 → return
                                              达 200 条入库上限 → truncated=true 提前收
        终包 {done, truncated} → Done 态      ④ 每 32 文件/250ms postToUi 增量 flush
```

- worker 零改动：`git_worker.h:36` postWithRepo + `git_worker.h:43` ensureHandle 已够用；取消旗标在 controller，闭包捕获。
- 渐进出果（F-08③）：结果按增量包 append，扫描中即可点击已出结果（跳转用当时快照的 relPath，不受后续取消影响）。
- 进度口径：walk 与 match 同任务内完成；进度 = 已 match 文件数 / 清单总数（walk 极快，不单独报进度）。

## 3. 取消语义（三处 + ESC）

共享取消面：controller 持 `std::atomic<std::uint64_t> searchSeq_`（UI 线程写、worker 读）；任务闭包捕获（指针, 提交时 seq 快照），**每文件边界**检查不一致即 return（丢弃未 flush 增量）。取消粒度=文件级，上界≈单文件处理时长（≤1MiB，毫秒~十毫秒级）。

| 触发 | 动作 |
|---|---|
| ①新查询 | submitSearch 先 `searchSeq_++` 再投新任务——旧任务在下一文件边界自灭，无需出队（序号检查不依赖任务位置） |
| ②切仓 | switchRepo/closeRepo → `searchSeq_++` + 清 searchResults_ 回无搜索态（resetRightPane 同处接线） |
| ③切模块 | GitModule::onStop 现顺序（watcher 停 → controller 停 → worker stop join）中 controller 停时先 `searchSeq_++`：join 等待中的长任务在下一文件边界立即退出，`git_worker.h:32` stop 的「丢弃未执行任务」兜底队列尾 |
| ESC（显式） | 浮层内 ESC → `searchSeq_++` + 回无搜索态；**浮层失焦收起不取消**（后台继续出果，搜索框 spinner 指示，再点开看进度——保渐进出果价值） |

请求序号先例：`git_page_controller.h:187`（contentReq_）/ `:193`（diffReq_）同型，过期结果按序号丢弃的既有纪律直接平移。

## 4. 扫描范围与过滤判据（逐项落既有函数，零新写）

| 判据 | 落点（既有） |
|---|---|
| .git 跳过 / ignored 过滤 / symlink 目录不跟随 / 子仓库不递归 / 无权限降级 | `GitTreeModel::walkChildren`（`git_tree_model.h:92`）——搜索 walk 直接逐目录调它，语义零漂移 |
| untracked 目录不递归（防 hang：node_modules 等） | **不在 worker 重算 status**：submitSearch 时 UI 线程把 tree_.statusByPath_ 中「尾 `/` 的 New 条目」（折叠 untracked 目录，v1 F-12 同源快照）拷贝进任务闭包，walk 遇之跳过。实现时核对一点：statusByPath_ 对 untracked 目录的键形态（尾 `/`）——一行核对，不阻塞 |
| untracked 文件本身 | 照常搜索（快照单文件 WT_NEW 不拦截文件行） |
| 空 0 字节 | `FileContent::readFromDisk`（`git_content_model.cpp:60`）→ `ContentState::Empty` skip |
| 二进制（首 8KiB 含 NUL） | 同上 → `Binary` skip |
| 超大 >1MiB | 同上 → `Oversize` skip（`kOversizeLimit` 同源，内容视图点开也是占位——搜索不收一致） |
| 非 UTF-8 | 同上 → `BadEncoding` skip（中文路径宽字符修复版口径） |
| 无权限/读取失败 | `ReadError` skip，静默计数不弹错（搜索是宽松语义，降级不阻断） |

匹配主循环 = `readFromDisk` 返回的 Ok 态 lines 逐行扫描——四态判据、切行、行尾 \r 剥除全部复用，搜索代码只写「子串查找 + 命中收录」。

## 5. 匹配语义（方案必答①）

| 项 | 决策 | 理由 |
|---|---|---|
| 分词 | **纯子串不分词** | 零依赖（v1 建议采纳）；PRD F-08 只要求「内容匹配」，分词器是负资产 |
| 大小写 | ASCII 折叠（tolower 比较器）；非 ASCII 不折叠（CJK 无大小写，拉丁扩展 Ä/ä 不互匹配） | `std::search` + 自定义比较器一行拼接（梯子⑤）；完整 Unicode casefold 需 ICU 级依赖，放弃 |
| 首尾空白 | trim；内部空格按字面子串 | 最小 90% |
| 单文件上限 | **5 条命中** + hasMore 标记（「此文件还有更多」） | 防单巨文件淹结果 |
| 总量上限 | **200 条即停**（truncated=true，提前结束扫描不再计数） | 时长可控（10k+ 仓高频词不至扫全仓）；「仅计数不停扫」推迟 |
| 命中窗口 | preview = match 前后各 ~40 码点，UTF-8 码点边界截断 + 省略号；match 段字节偏移记录供高亮 | 码点边界防切半个多字节字符 |

## 6. 结果模型与定位

### 6.1 模型（新文件 `git_search_model.h/cpp`，纯函数 doctest 直打，先例 git_content_model）

```cpp
struct SearchHit {
    std::string relPath;   // 相对 workdir POSIX 路径
    int line = 0;          // 1 起
    std::string preview;   // 截断预览（§5 命中窗口）
    size_t matchStart = 0; // preview 内 match 段字节偏移（高亮 runs 用）
    size_t matchLen = 0;
    bool moreInFile = false;
};
struct SearchQuery { std::string needle; std::vector<std::string> skipUntrackedDirs; };
// 纯函数：matchLines(lines, needle) → hits（ASCII 折叠、单文件 5 条上限）
//         asciiFold(s)、truncateAtCodepoint(s, center, radius)
```

### 6.2 controller 数据面（git_page_controller 增补）

`searchState_ {None, Scanning, Done, Error}` + `searchResults_` + `scanned_/totalFiles_` + `truncated_` + `searchError_`；composePump 排增量 inbox（既有泵不改结构）；`searchSeq_` atomic；`pendingContentLine_`（定位锚）。结果行渲染 runs 在 UI 线程现算（≤200 条，成本可忽略；match 段 --c-primary 色 TextRun）。

### 6.3 点击定位（N6 复用，方案必答④）

点击结果行 → `controller.jumpToHit(hit)`：
1. `selectTreeFile(hit.relPath)`（`git_page_controller.cpp:420`，N6/I-03 同一路径——右栏切内容视图，树同步选中）；
2. 记 `pendingContentLine_ = hit.line`；
3. 内容 load 完成（RightPaneState::Ready 首帧）→ 视图按**内容锚**把该行滚入可视区（clue#14 口径：锚=行号由视图按行盒几何现算 scrollIntoView 语义，**禁 `setContentScrollOffset` 偏移量直设**——`git_views.cpp:891-899` 的 `.offset(c.contentScrollOffset())` 是禁改示范点）；virtualList 内容锚 API 的具体签名在批次第一步对 EUI-NEO docs/现用法核对（不阻塞方案）；
4. 命中行视觉 = 选中底色 --c-selected-bg 常亮（不加淡出动画，推迟）。

## 7. UI 接入点

### 7.1 顶栏搜索框（ui-a.md §5.1 扩展——决策① v2 反转，PRD §6 已契约预留）

落 §5.1「弹性间隔」位（仓名/路径右侧、「切换仓库」左侧）：

| 元素 | 参数 |
|---|---|
| 容器 | 宽 320px、高 32px、radius 6px、底 --c-bg-sidebar（顶栏白底上的降档区分）、边 1px --c-border-1、聚焦边 --c-primary |
| 图标 | 放大镜 16px 描边 1.5px --c-text-3 |
| placeholder | 「搜索全仓内容…」13px --c-text-3 |
| 清除 × | 有输入时显示，16px --c-text-3，点击=清空+取消（走 §3① 同路径） |
| 扫描指示 | 框右侧 12px 旋转圈（复用刷新图标参数 §5.1） |
| 键盘 | Enter 立即提交（跳过 debounce）；ESC=取消 |

**交付方式**：本表为「设计增补节」提案，实现批次原文并入 ui-a.md（标注「need:14 增补，决策① v2 反转」），不动方案 A 已拍板其它参数。

### 7.2 结果浮层四态（F-08 四态设计增补，浮层锚定搜索框下方）

| 态 | 参数 |
|---|---|
| 浮层容器 | 宽 560px（≥搜索框宽）、max-height 480px、radius 8px、底 --c-bg-card、边 1px --c-border-2、阴影同 dropdown 惯例；失焦收起（扫描不取消） |
| 空态（无查询） | 浮层不显示 |
| 扫描态 | 头部进度条：「正在扫描… x/y 文件」12px --c-text-3 + 旋转圈；已出结果实时渐显（virtualList 定高 28px 行制） |
| 结果行 | 28px 槽内压缩双行（§5.3 修订先例同型）：上行=relPath 11px --c-text-3 +「:行号」；下行=preview 12px --c-text-1，match 段 --c-primary；hover --c-border-2 |
| 计数头 | 「N 处命中 · M 个文件」12px --c-text-2；truncated 时追加「结果过多，仅显示前 200——请细化关键词」12px --c-text-3 |
| 取消态 | ESC/清除 → 回无搜索态浮层消失；扫描中失焦收起≠取消（§3） |
| 错误态 | 浮层内错误块：底 --c-danger-tint radius 6px padding 12px，13px --c-danger-text + 原因 12px --c-text-3（中栏错误块参数缩档） |
| 空结果 | 「未找到匹配内容」13px --c-text-2 居中 |

## 8. bug#21 顺路修（独立 commit `fix:`）

- 病灶：`git_content_model.cpp:119` `maxLineLen = max(maxLineLen, end - start)` 按字节当字符 → UTF-8 CJK 3 字节/字，diff 横滚内容宽虚胖 ~3 倍（CJK 仓假横滚 ~70%）。
- 修法：**码点计数**——Ok 态文件已过 `isValidUtf8` 严格校验（`git_content_model.cpp:19`），切行循环内顺带按 UTF-8 序列长数码点，零额外校验成本；`maxLineLen` 语义字节→码点（`git_content_model.h:20` 注释同步改）。CJK 加权 1.1em 属渲染层，推迟（码点计数已消假横滚主因；CJK 行宽略窄残留为已知边界）。
- 用例：先红后绿——同文件「1 行 10 个中文字符 vs 1 行 10 个 ASCII 字符」断言 maxLineLen 相等；放既有 test_git_ 内容模型用例组。
- 消费点核查（改一头必查另一头）：maxLineLen 消费方=git_views 内容/diff 横滚内容宽计算，语义从字节变码点后消费侧公式（码点×单字宽）随之正确，无第二处字节口径残留（实现时 grep maxLineLen 全消费点复核）。

## 9. 批次划分（方案必答⑤：单批，含 bug#21 顺路）

| # | 环节 | 内容 | 定向验证 |
|---|---|---|---|
| B0 | 核对 | EUI virtualList 内容锚 API 签名（EUI-NEO docs/现用法）；statusByPath_ untracked 目录键形态 | 读代码，零构建 |
| B1 | 模型 | `git_search_model.h/cpp`：matchLines/asciiFold/truncate + 单测先红后绿 | `bash scripts/build-test-run.sh --dev --filter test_git_search` |
| B2 | 接线 | controller 搜索状态面 + worker 任务闭包 + 三处取消 + jumpToHit/内容锚 + 顶栏搜索框/浮层四态 + ui-a.md 增补节并入 | 同上 filter + 实操证据 |
| B3 | 顺路 | bug#21 码点计数 + 用例（先红后绿） | `--filter maxLineLen`（或同文件 filter） |
| 收尾 | 汇报 | F-08①~④ 实操走查（DevDesk 仓 + 一个 CJK 大仓）+ 截图/日志入 evidence，bug#21 横滚对照图 | F-08 锚见 §10 |

- B3 独立 `fix:` commit（bug#21 可回滚），B1+B2 squash 为单个 `feat:`（wip: 检查点提交按惯例）。
- 禁全量：doctest 全量/probe/守卫链归收口 qa（AGENTS §五）。

## 10. 验收锚（F-08①~④ + bug#21）

| 锚 | 判据 | 证据物 |
|---|---|---|
| F-08① | 搜索框输入→结果列表出命中 | 截图（扫描态+结果态各一） |
| F-08② | 点击结果→右栏内容视图+定位命中行 | 截图（命中行选中底色可见） |
| F-08③ | 10k+ 文件仓后台扫描 UI 不冻+渐进出果 | 扫描态截图 + 增量 flush 日志 |
| F-08④ | 新查询/切仓/ESC 三路取消 | 日志（seq 过期丢弃行） |
| bug#21 | CJK 仓 diff 横滚宽度修复对照 | 修复前后截图对照 |

## 11. 涉及文件（含另一平台核查）

| 文件 | 动作 |
|---|---|
| `src/module_git/git_search_model.h/cpp` | 新增（纯函数模型+单测宿主） |
| `src/module_git/git_page_controller.h/cpp` | 增补：搜索状态面/submitSearch/jumpToHit/searchSeq_/pendingContentLine_ |
| `src/module_git/git_views.cpp` | 增补：顶栏搜索框+浮层四态+内容锚滚动+命中行高亮 |
| `src/module_git/git_content_model.cpp/.h` | bug#21：maxLineLen 字节→码点（`:119`/`:20`） |
| `git_worker.h/cpp`、`git_tree_model.*` | **零改动**（复用） |
| `docs/superpowers/specs/2026-11-13-git-manager-readonly-ui-a.md` | 并入 §7 设计增补节（标注 need:14） |
| 测试 | 既有 test_git_ 系就近新增 test_git_search 组 |

平台核查：以上全部为平台无关单元（module_git 无 *_win/_macos 拆分文件）；walk/readFromDisk 已是 std::filesystem+libgit2 跨平台路径（中文路径 F-10 口径在库）；std::atomic/std::search 标准库双平台同源。无新增平台文件，Windows/macOS 同批编译。

## 12. POC 结论

**疑点清单清零，零 POC 进实现**：①worker 单线程长任务阻塞 status——已在 §13.1 风险声明（接受+升级触发），非不确定项；②匹配实现=std::search+比较器纯标准库，无环境不确定；③内容锚 API 存在性由 clue#14 口径背书，签名级核对降级为 B0 读代码项。简报未要求 poc_ 用例。

## 13. 风险与未验证项

1. **worker 单线程排队**：长扫描期间 status/walk/内容任务延迟数秒——接受（刷新 1s 节流本就容忍秒级）；升级触发：实测扫描中手动刷新延迟 >3s → 改分片投递（match 按 64 文件分片多任务，注释 shortcut: 分片上限=扫描期间刷新延迟 3s）。
2. **1MiB 上限文件不参与搜索**：与内容视图占位口径一致（点开也看不到内容），取舍非缺陷；PRD 若后续要求大文件搜索再议流式扫描。
3. **非 ASCII 大小写不折叠**（Ä/ä 不互匹配）：边界声明，不改。
4. **status 快照过期**：untracked 目录清单取自提交时快照，watcher 刷新后旧清单可能多跳过一个新 untracked 目录（下个查询自愈）——搜索场景可容忍。
5. **内容锚 API 签名**：B0 核对项，若 EUI 无现成 scrollIntoView 语义则按 clue#14 口径在视图层按行盒现算目标偏移（一次计算一次过渡，仍禁直设终值语义以外的持续偏移管理）——实现细节，不阻塞。
6. **macOS 未实测口径**延续 v1（逻辑平台无关，doctest 双机同跑）。

## 14. 取舍（放弃 · 推迟）

- 放弃：分词/正则（零依赖纪律）；ICU 级 Unicode casefold；文件名匹配并入首版（PRD F-08 只要求内容匹配，文件名匹配推迟至用户反馈）；「结果过多仍扫完只计数」（时长不可控，改 200 条即停）。
- 推迟：CJK 加权 1.1em 宽度（渲染层改动大，bug#21 码点计数已消主因）；命中行淡出动画；搜索历史/下拉建议；worker 分片投递（见 §13.1 升级触发）。
