# MD 目录树「根目录选中 → 新建」验证留证（2026-09-30）

分支 `fix/md-tree-root-create`（自 master `a1451e9` 切出）。修复 commit：见分支 tip。

## 症状（用户实测）

内置根目录行选中 → 新建目录：新目录成为根的**兄弟**（读起来像「落在笔记根之外/上一级」）；
选中其它目录新建 = 正确落在其子下。预期：根选中时新建 = 根的子目录（顶层目录）。

## 根因（坐实到 file:line）

诊断结论：**落点派生逻辑本来就正确，错的是树把「顶层夹」与「根组行」画成同一层级**——
建出来的顶层夹在树里与根行同级，读成「根的兄弟/笔记根之外」。两个面分别坐实：

1. **逻辑面（无缺陷，已核）**：
   - `src/module_markdown/markdown_module.cpp:858` 目录行点击 `selectDir(r.relPath)`，
     根组行 relPath = `""`（`src/module_markdown/md_tree.cpp:76`）。
   - `markdown_module.cpp:3776-3782` `newItemParentDir()`：`selectedDir_ == ""` → 返回 `""`
     = 笔记根（不是 else 分支、不是 parentPath——本仓库没有节点 parentPath 概念）。
   - `markdown_module.cpp:3862` `commitNewFolder`：parent `""` → relPath = 名字。
   - `markdown_service.cpp:1333-1349` `createFolder`：`isSafeNewNotePath` 挡死 `..` /
     绝对路径 / 盘符（:1281-1288），`absPath = notesDir_ / name`（:1338）——
     **磁盘上不可能落根外**。
   - 弹窗「创建在：根目录/」提示与落点同源（`markdown_module.cpp:2875-2879`）。
2. **几何面（缺陷，本次修复点）**：
   - `src/module_markdown/md_tree.cpp`（修前 :77，修后 :83）`folder.depth = folderDepth(dir)`：
     顶层夹（`"工作"`）depth = 0，与根组行（`""` → 0）**同级**；
   - `markdown_module.cpp`（修前 :841，修后 :843）目录行缩进 = `depth * 16` → 顶层夹 x == 根行 x。

## 修法

`md_tree.cpp` 深度口径改为「根组行是整棵树的根」：`folder.depth = isRootGroup ? 0 : folderDepth(dir) + 1`。
根组行 0 / 顶层夹与其直接文件 1 / 再逐级 +1（根组直接文件行 depth 1 **不变**，本来就嵌在根行下）。
同步改：`md_tree.h` 层级注释、`markdown_module.cpp` 缩进注释（:840-842 / :912-914）。

「新建笔记」同源入口已核：根选中新建笔记 → 根文件行 depth 1，修前修后都嵌在根行下，无同病。

## 验证（红→绿 + 变异）

新增用例 `md_module_root_row_new_folder_inside`（tests/test_modules/test_markdown.cpp）：
真实 headless 链路（选 工作 → 点根行 → +文件夹 → 输名 → 创建），逻辑 + 几何双面断言。

1. **修前红**（未修复代码上实跑）：
   `[FAIL] ../tests/test_modules/test_markdown.cpp:1350 新顶层夹 name x=94.000000 ≠ 根行 x=94.000000 + 16（应为其子行）`
   ——同一跑里**全部逻辑断言先过**（`fs::is_directory(env.dir/"新目录")` ✓、
   `!fs::exists(env.dir.parent_path()/"新目录")` ✓、下拉显示「根目录」✓），几何断言红 = 兄弟位。
2. **变异红**（把修复合回 `folder.depth = folderDepth(dir)` 后重跑）：
   - 新用例 FAIL（同上）；
   - 既有数值锁 `md_tree_row_geometry` FAIL：`xOf("md.tree.node.2.arrow") != xOf("md.tree.node.0.arrow") + 16.0f`。
3. **修复后绿**：`bash scripts/build-test-run.sh --no-run --filter markdown`
   → `Total: 87 Passed: 87 Failed: 0`（基线 86 + 新 1）+ 模块隔离守卫通过。

## 真机（独立数据根，worktree 构建）

`DEVDESK_DATA_ROOT/DEVDESK_NOTES_DIR=/tmp/mdroot-rf` 启动，cliclick 走完
「点根行 → +文件夹 → 输名 → 创建」：

- 磁盘：`/tmp/mdroot-rf/notes/verifydir`（**笔记根内**；`/tmp/mdroot-rf/` 下无新建目录）；
- 树呈现：`verifydir` 嵌在根行**之下**（与 solo.md 同层）且为选中高亮 —— 截图
  `docs/screenshots/2026-09-30-md-tree-root-create-fixed.png`（修复后建后态）、
  `2026-09-30-md-tree-root-create-tree.png`（修复后种子数据树：工作已嵌在根行下）。

## 影响面与口径

- 顶层夹及以下所有行整体右移一级（+16px），树读法从「根行是散文件组标题」变为
  「根行 = 树根」——与用户预期一致（根选中新建 = 根的子目录）。
- `contentKey`（`markdown_module.cpp:825`）含 depth → 树内容 key 变化仅触发滚动区重建，无行为影响。
- 既有测试同步更新（depth 断言 ×4 用例 + `md_tree_row_geometry` x 关系式），全部随新口径绿。
