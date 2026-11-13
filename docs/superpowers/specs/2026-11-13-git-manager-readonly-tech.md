# git 管理器（只读）v1 技术方案

> slug：2026-11-13-git-manager-readonly-tech｜type=tech｜need_id=7｜开发者：developer（2026-11-13）
> 上游：PRD `2026-11-12-git-manager-readonly` v3（doc id=6）｜调研 `docs/research/2026-11-12-git-manager-readonly-research.md`｜线索 clue#38~#45
> **PRD 契约依赖标注**：本方案按 PRD §6 **三栏契约**（左树/中列表/右内容-diff）落视图层；UI A/B 两路设计稿（`2026-11-13-git-manager-readonly-ui-a/b.md`）拍板后若选 B 两栏，需同步修订本文 §6 视图层清单的布局行，其余章节不受影响。
> POC 边界声明：本任务只出方案不写码不跑构建；调研标「待验证」的项全部收敛为**批次一 POC 清单**（§5 步骤 8），POC 通过前不进入批次二。

## 1. 模块结构（文字版结构图）

```
main.cpp / app_assembly（装配层）
  │  app_registry.h 新增一行（位置=笔记之后、命名「Git」，决策④）＋ PageId::Git 枚举 ＋
  │  main.cpp extern "C" create_git_module 声明 ＋ module_settings 新增 kModuleIdGit 门控键
  ▼
module_git（devdesk_module_git STATIC；模块间禁互依——不 include 任何 module_*）
  ├─ git_module.cpp           Router 页面生命周期（onStart/onStop：worker 起停、watcher 起停、git 作用域）
  ├─ git_page_controller.*    N1~N10 流转状态机（空态/校验/主界面/切走切回恢复范围=仓+模式+打开文件）
  ├─ git_worker.*             后台任务队列（单 worker 线程，见 §3 线程模型）
  ├─ git_tree_model.*         「全部文件」= std::filesystem 单层 walk + ignore 过滤；
  │                           「仅修改」= status 快照过滤（不 walk）；展开状态机 + ▶/▼ 行模型
  ├─ git_changes_model.*      status 快照 → 「变更 / 未跟踪」两组行模型（决策⑤）
  ├─ git_diff_model.*         diff 快照 → patch/hunk/行 三级行模型 + hunk 折叠状态
  ├─ git_content_model.*      worktree 读文件 + 二进制/编码/1 MiB 阈值判定（F-11）
  ├─ git_views_*.cpp          virtualList 行盒渲染（树/变更列表/内容/diff 四视图，见 §6）
  └─ git_repo_store.*         记忆仓持久化（core paths 数据根 + json；决策⑥，含失效清除）
  │ 依赖方向（单向，架构 §六 铁律）：
  ▼
platform/git（libgit2 RAII 包装层；platform ✅ 可依赖 core 的 Result<T>）
  ├─ git_libgit2.h/.cpp       git_libgit2_init/shutdown RAII 作用域 + libgit2 错误码→ErrorCode 映射
  ├─ git_repo.h/.cpp          Repo：open / status / diff / isIgnored / blame（v1 留接口）
  └─ 无 *_win/_macos 拆文件：libgit2 是可移植 C 库，平台差异封装在库内；
      我们的业务代码零平台宏（平台纪律）；Windows 侧 UTF-8 路径行为=批次一 POC 验证点
  ▼
third_party/libgit2（vendored submodule @ v1.9.7，STATIC，双平台同一 add_subdirectory）

跨模块共享（批次四）：
src/ui_kit/text_highlight/   md_highlight 自 module_markdown 平移至此 + java/js/ts/sql/json 五类新词法扫描器
                             （理由：module_git 禁依赖 module_markdown，复制=负资产；ui_kit=presentation
                              能力层先例，module_markdown 同步改 include 路径继续消费）
```

CMake 接线（照抄现例，禁手敲 cmake）：根 CMakeLists.txt `add_subdirectory(third_party/libgit2)`（置于 `include(cmake/Dependencies.cmake)` 之后、`add_subdirectory(src/platform)` 之前）；新增 `add_subdirectory(src/module_git)`（照 `src/module_markdown/CMakeLists.txt` 模板：`add_library(devdesk_module_git STATIC ...)` + `devdesk_apply_warnings` + include `src/` + link `devdesk_core / devdesk_platform / devdesk_ui_kit`）；`DevDesk` target_link_libraries 追加 `devdesk_module_git`；tests 目录新增 `test_git_*.cpp` 进 devdesk_tests。

## 2. clue#44 两点落定

### 2.1 diff 比对基准（落定：v1 单一基准 worktree↔HEAD）

**API**：`git_diff_tree_to_workdir_with_index`（tree=HEAD；`with_index` 使 diff 穿过 index）。

**理由（按 libgit2 API 实际形态论证）**：
1. v1 无 staging UI，且 staged/unstaged 二分显示已被决策⑤列为**增量**（不在 v1）——单一基准下「同一文件只出一行 diff」，不存在双计数；
2. 该 API 语义 = HEAD 到工作树的全部差异（已暂存+未暂存合并口径），等价 IDEA Local Changes，正是无 staging 概念查看器的正确口径；
3. 着色与分组所需的分类信息由 `git_status` 提供（status entry 天然带 head-to-index / index-to-workdir 双侧 flag），与 diff 基准**解耦**——树着色不受单基准影响；
4. 增量预留：RAII 层 `DiffBasis` 枚举三值（`TreeToIndex`=index↔HEAD、`IndexToWorkdir`=worktree↔index、`WorkdirToHead`=上述单基准）。增量批次落 staged/unstaged 二分时，diff 调用改传 `TreeToIndex + IndexToWorkdir` 双次即可，层签名零改动；
5. untracked 口径：diff 侧 `GIT_DIFF_INCLUDE_UNTRACKED`（新文件=全增行、untracked 目录整体一个条目不递归——防 GitAhead #161 多 GB untracked hang，展开留树模式）；status 侧 `GIT_STATUS_OPT_INCLUDE_UNTRACKED` 且**不开** `RECURSE_UNTRACKED_DIRS`，`GIT_STATUS_OPT_EXCLUDE_SUBMODULES`（F-12③：submodule 内部改动不误报外层条目）；rename 检测开 `GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX / RENAMES_INDEX_TO_WORKDIR` 与 diff `GIT_DIFF_FIND_RENAMES`（F-06② 旧名→新名）。

### 2.2 FileWatcher 监视范围（落定：单 watcher 监 worktree 根递归 + 消费侧路径过滤 + 1s 节流）

复用 `platform::FileWatcher`（契约实读：目录级递归监听、不按扩展名过滤、事件在 watcher 后台线程、`Unknown`=缓冲溢出全量重扫兜底、stub 平台 `active()==false`）。过滤规则（消费侧，`git_page_controller`）：

| 事件路径形态 | 动作 |
|---|---|
| 不以 `.git/` 开头的工作树路径 | 标记 status 脏 → 节流队列 |
| `.git/index`、`.git/HEAD`、`.git/refs/**` | 标记 status 脏（外部 commit/stage/checkout）→ 节流队列 |
| `.git/` 其余（objects/logs/pack 等 gc 洪泛） | 忽略 |

- **build 噪声**：事件洪泛由两层节流吸收——watcher debounce 提到 300ms（契约参数）+ 刷新 1s 节流（F-09④）；ignored 文件不进 status 结果，重算成本被 ignore 判定挡住；缓冲溢出 `Unknown` 走既有全量重扫语义。
- **自触发**：本模块纯只读不写工作树，零自触发面；唯一写事件源=libgit2 刷新 index stat cache 可能触碰 `.git/index` mtime——批次一 POC 第 5 项专验「连续 100 次 status 后 watcher 事件收敛不循环」（收敛机制：结果不变 → 无 UI 变化 → 不再置脏）。
- **macOS 轮询兜底**：macOS arm64 走 FSEvents 真监听（`file_watcher_macos.cpp` 在位）；stub 只落通用 POSIX 档——`active()==false` 时降级 2s 轮询 status（仅模块可见时跑），沿用仓内已知「FileWatcher 轮询兜底」降级先例。

## 3. 线程模型

- **全局初始化**：`git_libgit2_init` / `git_libgit2_shutdown` 由 `git_module` onStart/onStop 经 `git_libgit2.h` RAII 作用域配对调用（refcount 语义，v1.9 线程安全）。
- **单 worker 线程（v1）**：任务队列（status / diff / tree 层 ignore 批判定）；UI 线程零 libgit2 调用；结果以模块内事件投递回 UI 线程（长任务派发先例：`md_note_git.h` NoteGitAutoCommit 后台线程+节流模式）。
- **handle 策略（倾向：per-task open，验证点入批次一）**：worker 每任务经 RAII `Repo::open` 独立开 handle、任务结束即毁——零共享对象、零锁、无过期 handle；备选=worker 线程内单 handle 按 repo 路径缓存（open 成本实测 >1ms 时才切换）。互斥方案（全局单 handle+锁）直接排除：串行化自 hinder 且长任务持锁风险。**验证点**：①v1.9 文档线程边界落注（`git_libgit2_init` refcount、对象禁跨线程）；②双线程各自 handle 并发 status+diff 同仓 1000 轮无崩；③`git_repository_open` 单次成本实测（判定 per-task vs 缓存）。
- **1s 节流接线点**：watcher 事件 / 手动刷新 → 原子 dirty 标记（UI 线程 compose 帧检查，节流先例在位）→ 距上次重算 ≥1s 才投 worker 任务 → 结果回 UI。刷新期间变更列表显加载态（F-09③ UI 可交互）。

## 4. libgit2 引入方式（三选一论证）+ 双平台工序

### 4.1 选型：源码 vendored submodule（third_party/libgit2，锁 tag v1.9.7）

| 准绳 | ① vendored submodule（选） | ② MSYS2 包静态变体 | ③ 自编静态库脚本 |
|---|---|---|---|
| CI 离线可用 | ✅ 源码随 checkout 进 CI（submodule init），configure/build 零网络 | ❌ 依赖 CI 镜像预装且版本随发行版漂移（版本不受本仓控制） | ✅ 但脚本若联网下载源码即破 |
| 双平台工序同构 | ✅ 两平台同一 `add_subdirectory`，选项化裁剪 | ❌ macOS 无 MSYS2，仍需另一条自编路（工序分裂） | ⚠️ 两平台各写一套构建脚本 |
| 与仓内先例一致 | ✅ `third_party/libvterm`（submodule 锁 commit + add_library STATIC）、`third_party/Corta`、EUI-NEO 全同款 | ⚠️ openssl 是 find_package+静态 .a 路径先例，但那是「系统已有库」场景，libgit2 不是 | ❌ libgit2 自带成熟 CMake，自编=重造（复用梯子③④违反） |
| 生产参考 | ✅ Gittyup 同款 bundled submodule 生产使用（调研 §2） | — | — |

**版本锁**：submodule 锁 tag `v1.9.7`（稳定线；v2.0.0-rc 不上——rc 线进 vendored 库违反稳定性）。注：AGENTS.md「不锁版本」令 scope=EUI-NEO 框架；第三方 vendored 库按 libvterm 先例锁版本，升级=显式动作。

### 4.2 工序步骤（批次一执行）

1. `git submodule add -b v1.9.7 https://github.com/libgit2/libgit2.git third_party/libgit2` 并锁 tag（若 clone 受限则本机已有源码入库同效——离线纪律）。
2. 根 CMakeLists.txt 增 `add_subdirectory(third_party/libgit2)`，**选项裁剪**（只读本地仓零网络面）：`BUILD_SHARED_LIBS=OFF`、`BUILD_TESTS=OFF`、`BUILD_CLI=OFF`、`USE_SSH=OFF`、`USE_HTTPS=OFF`、zlib 用 bundled（零外部依赖、离线确定）、regex 用默认兜底、iconv OFF。**最终选项名以 v1.9.7 版 CMakeLists 实读为准（POC 第 0 步，1.x 与 2.x 选项命名有差异）**。
3. Windows 系统库补齐：预期 `ws2_32 / advapi32 / crypt32 / rpcrt4 / ole32` 级别，按链接报错逐个补（沿用 Dependencies.cmake curl 静态闭包「缺了再按链接报错补」口径）；**发布形态守卫自证**：链接后 objdump/依赖检查确认零新 DLL（单 exe 红线）。
4. macOS：同一 `add_subdirectory` 直通，Homebrew 零新依赖（zlib bundled、无 TLS 后端）。
5. devdesk_platform 的 git 包装 target link vendored `git2` STATIC target。
6. **接线变更代价（写进批次一预算）**：add_subdirectory+新链接边 → 按纪律 `rm -rf build build-release` 全量重建一次（CMake 缓存旧路径）；此后恢复 `--dev` 增量内循环。
7. 构建入口唯一：`bash scripts/build-test-run.sh`（ccache launcher、DEVDESK_JOBS=3 封顶全由脚本封装）；libgit2 约百个 C 文件首编后进 ccache，后续增量零成本。
8. **批次一 POC 清单**（调研待验证项全部收敛于此，全绿才进批次二）：①POC-0 选项名实读与 configure 通过（双平台）；②中文路径仓 open+status+diff 实测（Windows 窄/宽行为实证，doctest 夹具临时建中文仓）；③静态链验证+零新 DLL 断言；④per-thread handle 并发 1000 轮无崩 + open 成本实测；⑤status 循环 `.git/index` 事件收敛验证（§2.2 自触发）。

## 5. RAII 层 API 草案（platform/git，Result<T> 映射）

```cpp
namespace platform::git {

// 作用域：模块 onStart 构造、onStop 析构（配对 git_libgit2_init/shutdown）
class Libgit2Scope { public: Libgit2Scope(); ~Libgit2Scope(); };

enum class StatusKind { New, Modified, Deleted, Renamed, Untracked, Ignored };
struct StatusEntry { std::string path; StatusKind kind; bool staged; };   // staged=分类着色用
struct StatusSnapshot { std::vector<StatusEntry> entries; };

enum class DiffBasis { WorkdirToHead, TreeToIndex, IndexToWorkdir };     // v1 用 WorkdirToHead（§2.1）
struct DiffLine  { char origin; std::optional<int> oldNo, newNo; std::string content; };
struct DiffHunk  { std::string header; std::vector<DiffLine> lines; };
struct DiffPatch { std::string oldPath, newPath; bool binary; bool isUntracked; std::vector<DiffHunk> hunks; };
struct DiffResult{ std::vector<DiffPatch> patches; };

class Repo {
public:
    // 非 git 仓 → fail(ValidationError,「该目录不是 git 仓库」)；无权限 → fail(FilePermissionDenied)
    static core::Result<Repo> open(const std::filesystem::path& dir);
    const std::filesystem::path& workdir() const;

    core::Result<StatusSnapshot> status();                       // flags 见 §2.1（untracked 不递归、排除 submodule、rename 检测）
    core::Result<DiffResult> diff(DiffBasis basis, const std::string& pathFilter = {});
    core::Result<bool> isIgnored(const std::string& path);       // git2/ignore.h，树模式逐条目过滤
    core::Result<DiffResult> blame(const std::string& path);     // v1 返回 fail(NotImplemented)——留接口（PRD 非目标 2）
private:
    struct Impl; std::unique_ptr<Impl> impl_;                    // PIMPL，非拷贝（file_watcher.h 同款形态）
};

} // namespace platform::git
```

**错误码映射表**（零 core 改动——复用现有 ErrorCode，libgit2 原始 message 附入 Result 供内联错误展示，前缀带 libgit2 错误码）：

| libgit2 错误 | core::ErrorCode | 消费场景 |
|---|---|---|
| open 返回 not-a-repo（GIT_ENOTFOUND 系） | `ValidationError` | F-01①② 内联错误三因区分之一 |
| `EACCES`/`EPERM` 系 | `FilePermissionDenied` | F-01② 无权限 |
| 路径/blob 不存在（GIT_ENOTFOUND） | `FileNotFound` | F-04③ 读取失败（文件被删） |
| GIT_EBUFS / GIT_EINVALIDSPEC | `ValidationError` | 参数类 |
| 其余全部 | `Internal` + `git_error_last()->message` | 兜底（F-13② 刷新失败保留旧数据） |

## 6. 视图层组件清单（按 PRD §6 三栏契约基线）

| 视图 | 现成/自绘 | 组件与要点 | 渲染陷阱适配 |
|---|---|---|---|
| 顶栏（仓名+路径+切换+刷新） | 现成 | `ui.row` + button | — |
| 左栏·目录树 | 自绘行模型 | `components::virtualList`（itemCount+rowHeight+overscan）+ ▶/▼ 字符 + 展开状态机（能力盘点 §10 官方口径 ~150 行） | clue#22：行模型与命中矩形=（行模型，宽度）的纯函数，measure 两跑幂等；展开=增量 append 行 |
| 中栏·变更列表 | 半现成 | `virtualList` 同列表混排组头行+条目行（10k+ 虚拟化，Fork #122 反例对策）；不用 dataTable（组头+着色定制不如行盒直接） | 同上 |
| 右栏·内容查看 | 自绘 runs | `virtualList` 行盒：行号列 + styled-runs 文本（批次四接 ui_kit/text_highlight）；先纯文本渲染后异步上色（F-05④ 不阻塞首屏） | clue#22：runs 数组按 measure 次重建，不缓存矩形 |
| 右栏·diff（最大单件） | 自绘 | `virtualList` 行盒：增/删/上下文三色底 + 行号列 + hunk 折叠条；巨 diff（>5000 行）分块喂行模型（F-07⑦） | clue#14：v1 unified 单栏**无跨栏联动滚动**（side-by-side 二期才碰 setScrollOffset）；hunk 折叠/行模型重建后滚动恢复用内容锚（hunk id）不用偏移量，避开 setScrollOffset 时序 |
| 四态空/加载/错误/占位 | 现成 | `ui.column` + label + toast（ui_kit toast 桥先例） | PRD §5 逐格对照 |
| 目录选择对话框 | 现成 | `platform::folder_picker` | F-01 |

## 7. 四个开发批次映射（池 #8~#11）

| 池条目 | 批次 | 范围 | 定向 filter（`--dev --filter`） | 验收锚（PRD F-ID） |
|---|---|---|---|---|
| #8 | libgit2 + RAII + POC | §4 工序全步（submodule/选项裁剪/静态链/零 DLL 断言）+ platform/git 三文件 + §5 POC 清单五项全绿 | `--filter file:test_git_`（test_git_raii / test_git_paths 中文路径 / test_git_threads） | F-10③（窄字符零直用代码评审）；调研风险 2/6 收口；接线重建代价一次性消化 |
| #9 | 骨架 + 树 + 变更列表 | 模块登记五处（app_registry/PageId/main.cpp/门控键/根 CMake）+ 三栏骨架 + git_tree_model 两模式 + git_changes_model + 记忆仓 | `--filter file:test_git_tree_,file:test_git_status_,file:test_git_registry_` | F-14①②③、F-01①~⑤、F-02①~⑤、F-03①~④、F-06①②③⑤ |
| #10 | diff + 内容 + 高亮 | git_diff_model/views + git_content_model（二进制/编码/1 MiB 判据）+ 批次首步 **md_highlight 平移 ui_kit** + java/js/ts/sql/json 五扫描器 | `--filter file:test_git_diff_,file:test_git_content_,file:test_text_highlight_` + markdown 高亮定向回归 | F-04①~⑤、F-05①~④、F-07①~⑦、F-11①②、F-06④（10k 条目虚拟化随列表落地） |
| #11 | 刷新 + 四态 + 隔离 | watcher 接线 + 1s 节流 + N9 刷新失败保旧数据 + N10 切走切回恢复 + submodule 降级 + 模块隔离守卫 | `--filter file:test_git_refresh_,file:test_git_watcher_,file:test_git_submodule_` | F-09①~④、F-10①②④（双平台实测）、F-11③、F-12①~③、F-13①~④ |

每批收口纪律：定向 doctest 绿 + 证据物（截图/日志）入汇报；全量 doctest+probe 不在研发批次跑（收口/QA 档触发）。

## 8. 调研 §7 八项风险逐条应对

| # | 风险 | 应对（落点） |
|---|---|---|
| 1 | 大仓性能 | F-09 四件套全进设计：树懒加载、双列表 virtualList、status/diff 后台 worker+1s 节流、1 MiB 阈值占位；untracked 目录不递归（§2.1）；10k+ 与多 GB untracked 两 probe 归批次 #11/QA 档 |
| 2 | 中文/非 ASCII 路径 | 全链 `std::filesystem::path` + 仓内 `src/platform/utf8.h` 转换（UTF-8 进 libgit2）；仓发现/status/diff 三处同口径（F-10）；libgit2 Windows 内部宽字符行为=POC-② 实证，失败则包装层加前置宽→UTF-8 规整（仍零平台宏） |
| 3 | 二进制 diff | patch binary 标记 + 尺寸阈值双判据 → 统一占位文案（F-11）；占位路径不读文件内容（GitAhead #161 反例） |
| 4 | submodule 仓 | `GIT_STATUS_OPT_EXCLUDE_SUBMODULES`（status 不误报）+ 树层 submodule 条目降级普通目录行不展开（F-12）；枚举细节对照 v1.9 `git2/submodule.h` 批次 #11 核对 |
| 5 | libgit2 特性滞后（无 histogram） | myers=git/IDEA 默认已对齐（调研 §0.2）；CLI 备选桥后补可行性留档（`local_git_probe` 先例在 module_settings） |
| 6 | 线程安全边界 | per-task handle 倾向（§3）+ 三项验证点全入批次一 POC |
| 7 | EUI-NEO 自绘陷阱 | clue#22 幂等行盒（§6 两处）+ clue#14 unified 避开联动滚动、折叠恢复用内容锚（§6） |
| 8 | tree-sitter 跟版 | v1 不引入（决策②路线 B）；增量批次引入时锁 minor 跟 patch（调研口径） |

## 9. 取舍（放弃 · 推迟及理由）

- **放弃：FetchContent 引入**——configure 期联网，违反 CI 离线；**放弃：MSYS2 libgit2 包静态变体**——版本漂移不受控且 macOS 无对应（工序分裂）；**放弃：互斥单 handle 线程模型**——串行化自 hinder。
- **推迟：staged/unstaged 二分显示**（决策⑤列增量）——`DiffBasis` 三值枚举已预留，增量零重构；**推迟：side-by-side diff**（决策③二期）——届时才接 clue#14 setScrollOffset 联动；**推迟：tree-sitter**（决策②渐进）。
- **md 复用=平移不复制**：module_git 禁依赖 module_markdown（架构铁律），PRD 所指「复用 md_highlight.cpp 先例」落地为**批次 #10 首步平移至 ui_kit**（含 markdown 模块 include 同步改+定向回归），非头文件复制。
- **core 零改动**：不加 Git 专属 ErrorCode，映射复用现有枚举 + message 附 libgit2 原始串（错误码表 §5）。
- **数据面零自研**：diff 算法/ignore 判定/status 分类全交 libgit2（复用梯子②命中即停）；自研面收敛在 EUI-NEO 集成层（调研 §6 口径，70%+ 代码量如实预期）。

## 10. 风险与未验证项（除批次一 POC 五项外）

1. libgit2 v1.9.7 在 MinGW GCC 16.2 下的编译警告面未实测（-Werror 只作用 DevDesk target，vendored 库不受影响，风险低）；2. 超大 index（10 万+ 文件仓）status 单次耗时未测——批次 #11 probe 口径覆盖；3. EUI-NEO virtualList 在 10k 行/帧的实测性能未测——批次 #10 落地时以变更列表为第一探针；4. UI A/B 拍板未回（并行子任务在途）——若选 B 两栏，同步本文 §6 布局行，模型/线程/工序章节不受影响。
