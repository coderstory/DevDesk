# MD-01 残留修复：重载后活动块内容映射

> 状态：审计修订版（2026-09-28，3 视角 skeptic 审计 CONFIRMED 项已全合）。前置：M3 已合 master；本修复原属 M3 批未覆盖项（review-B 第 1 条 CONFIRMED），用户拍板「先修再开 M7」。
> 依赖 spec：`2026-09-27-markdown-phase3-block-render-design.md` §4.1 D3（块身份锚 = 文档字节偏移）。

## 1. 问题（实证）

外部静默重载 / 冲突重载（`reloadCurrentNote`，`markdown_module.cpp:3266`）把文档整篇换成磁盘版本后，实时态活动块锚仍指向**旧文档的字节偏移**，下一帧被新块模型重新解释 → 活动块落到「覆盖该字节的新块」上，而非内容上对应的块。

- 复现（review-B 无头探针）：外部前插一段 → 旧文档块 1（`"\nbravo\n"` cursor=4）→ 重载后活动块变成新文档块 0（`"seed line one\n"` cursor=10）。无数据丢失，当帧收敛，但「跟内容走」不成立。
- 可达路径：① 实时态 + 无未保存编辑 + 外部改文件（mtime 按秒比、5s 轮询，最坏 ~6s）；② 冲突 toast「重载磁盘版本」（`resolveConflict(true)` → `reloadCurrentNote(false)`，可带未保存编辑）。**两条入口共用同一 reloadCurrentNote 实现，映射逻辑同一份**。

## 2. 根因（审计修正后的论据）

D3 决策：编辑态活动块身份 = `cursorByte → MdBlockModel::blockIndexAt`（纯字节偏移二分，`md_block_model.cpp:483-492`），stableId 只作渲染/缓存键。这对**打字/拆块/并块**是对的（偏移天然跟随光标）。

`reloadCurrentNote` 路径**不碰锚状态**（`liveCursorValid_` / `liveCursorByte_` / `liveActiveBlock_` / `liveInputStart_` 零写入，实证见 `markdown_module.cpp:3266-3296`）。下一帧 `drawLiveColumn` 落入 1817 分支（valid 且非 pending 且无组字）：用 `liveInputStart_`（**旧文档**块起点）+ 块内光标当**新文档**的字节偏移跑 `mdLiveActiveBlockIndex` → 跟错。

### 为什么不能照搬 `liveCursorValid_ = false`（review-B 已证）

- reload 场景编辑态光标恰好等于旧字节偏移 → 复位后结果逐位相同（白改）。
- 用户在实时态打过字时，编辑态光标是进态那一刻的陈旧值 → 复位把光标拉回更早位置（比现状更差）。

### 映射键选型（审计修正：stableId 作主键、(kind,hash) 作回退）

两个内容身份概念：
- **`(kind, hash)` = 内容等价类**：FNV-1a 全字节哈希 + 块类型。重复块（相同代码围栏、相同段落）共享同一等价类。**不区分实例**。
- **`stableId` = 位置性实例身份**：`MdBlockModel::update`（`md_block_model.cpp:446-462`）按 (kind,hash) **双端对齐**继承——前缀命中块 + 后缀命中块继承旧 stableId，中间变化窗口新分配。

stableId 的失效模式（审计证伪后的正确描述，v2 修订 F3）：
- **变化窗口语义**：`update()` 双端对齐后，变化窗口 = `[p, k-s)`（`md_block_model.cpp:465-470`）。**只有落进这个窗口的块拿新 id**。
- **纯头插 / 纯平移**：后缀对齐 s=m（旧块逐对命中）→ 窗口仅含新插入块 → **全部旧块 stableId 保留**，stableId 是精确实例指示器。
- **单点尾改**（只改末块）：p=m-1、s=0 → 窗口=[m-1,k) **只含被改的末块**（其 hash 也变 → 走零命中钳位，非 (kind,hash) 回退）；未改的中间块**保留 id**。
- **纯尾追加**：p=m → 窗口只含新增块 → 旧块**一个不丢 id**。
- **不相邻双改**（A B C → A' B C'）：p=0、s=0 → 窗口=[0,k) 包含全部块 → 中间未改块 B 也进窗口 → **丢 id**（但 (kind,hash) 仍能命中 B）。这才是 stableId 失效而 (kind,hash) 救回的场景。
- **活动块内容本身被改**：落窗口 → stableId 失效，(kind,hash) 也失效（内容变了）→ 走零命中。

**结论**：stableId 在平移/单点改场景是精确实例键（重复块也能区分），在不相邻双改/重排场景失效；`(kind,hash)` 是 stableId 的超集（stableId 保留 ⟹ (kind,hash) 必命中），但不区分重复实例。故映射键采用**三元组 `(kind, hash, stableId)` 优先**（命中即精确实例），**三元无命中再回退 `(kind, hash)` 取最近 start**（覆盖双改场景，重复块用 tie-break 决胜）。

## 3. 方案（重载路径加内容映射层；编辑态 D3 不动）

在 `reloadCurrentNote` 一条路径上加「旧活动块 → 新文档」的内容定位；编辑态键盘流、打字、拆块仍走字节偏移锚，D3 本身不变。

### 3.1 映射算法（setEditorText 前后三步）

**前置条件**：`mode_ == Live && liveCursorValid_ && !blockModel_.blocks().empty()`。否则跳过映射（非实时态或未初始化，走原路径）。

**步骤 1 — 捕获**（setEditorText **前**）：
- **先对当前模型重 derive 活动块下标**（审计 F6：`blockIndexAt` 对非空模型恒返回 [0,size) 内，`mdLiveActiveBlockIndex` 同语义，故 `idx >= size()` 分支对非空模型不可达——重 derive 本身已防越界；保留重 derive 作为 onChange 收缩 blocks 后的纠正，不必另加显式边界分支）：
  ```
  if (blockModel_.blocks().empty()) { 跳过映射，走原路径 }
  size_t idx = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);  // 对非空恒 in-range
  ```
- 记下旧活动块五元组 `(kind, hash, stableId, start, local)`：
  - `(kind, hash, stableId, start)` = `blockModel_.blocks()[idx]`
  - `local = mdLiveBlockLocalOffset(blockModel_.blocks()[idx], liveCursorByte_)`（`md_live_edit.cpp:15` 现成函数，块内字节偏移；审计 F1：步骤 3 命中分支写 `liveCursorByte_` 要用它，**无 ui 依赖**）

**步骤 2 — 替换**：`setEditorText(r.value())`（既有，内部 `blockModel_.update` 重建块模型）。

**步骤 3 — 定位**（setEditorText **后**、既有 `resetLiveUndoBaseline` **前**——见 §3.3 顺序约束）：在新 `blockModel_.blocks()` 里查找：
1. **三元组优先**：找 `(kind, hash, stableId)` 全等的块。命中（唯一，因 stableId 单调递增不重复）→ 精确实例，重定位到该块。
2. **三元组无命中 → 回退 (kind,hash)**：收集 `(kind, hash)` 等价的块；多命中取 `|start - 旧 start|` 最小者；**tie-break：等距取下标小者**。
3. **零命中**（内容被外部删改消失或文档变空）→ 按 §3.1-zerohit 处置。

**重定位赋值**（命中分支，v2 修订 F1+F2）：
- `liveActiveBlock_ = new_idx`
- `liveInputStart_ = new_block.start`
- `liveCursorByte_ = new_block.start + min(local, new_block.len)`（**审计 F1：必须显式写**，否则步骤 4 `resetLiveUndoBaseline` 读旧字节 → 撤销基线锚错；local 在步骤 1 已捕获，无 ui 依赖）
- `liveCursorValid_ = true`
- `liveInputStateFresh_ = true` **且** `liveFocusPending_ = true`（**审计 F2：两者必须成对**——:1775-1777 明文 scope 释放清焦点须与 `liveFocusPending_` 同帧补发配对；单独置 fresh 会掉焦点 + 块内光标位置丢失。enterLiveState :2777+:2792 即成对置位）
- **不**触发 `liveFollow_`（保持 false/原值，reload 不强拉滚动，与 D-B「越界才拉」一致）、**不**触发 `liveCaretPending_`

**零命中处置**（v2 修订 F4：空文档 guard）：
```
if (blockModel_.blocks().empty()) {
    // 文档变空：blocks()[0] 越界 UB，必须单列分支
    liveActiveBlock_ = 0;
    liveInputStart_ = 0;
    liveCursorByte_ = 0;
} else {
    size_t clamped = min(liveCursorByte_, blockModel_.text().size());
    liveActiveBlock_ = blockModel_.blockIndexAt(clamped);
    liveInputStart_ = blockModel_.blocks()[liveActiveBlock_].start;
    liveCursorByte_ = clamped;  // 审计 F1：零命中也要写，撤销基线锚定钳位后的字节
}
liveCursorValid_ = true;
liveInputStateFresh_ = true;
liveFocusPending_ = true;  // 审计 F2：与 fresh 成对
```
行为 = 「落到覆盖旧字节的新块」（与 pre-fix 现状一致，但消除陈旧基数 footgun；内容已删无正确答案，钳位是保守选择）。

### 3.2 副作用边界（v2 修订 F2）

重定位更新 4 个锚成员 + `liveInputStateFresh_` + `liveFocusPending_`（**两者成对**，见 §3.1）；**不**触发 `liveFollow_` / `liveCaretPending_`（reload 不强拉滚动，与 D-B「越界才拉」一致；不抢显式 caret 跳转）。

> v1 曾写「不触发 liveFocusPending_」——审计 F2 证伪：:1775-1777 明文 scope 释放清焦点必须与 `liveFocusPending_` 同帧补发配对，单独置 `liveInputStateFresh_` 会掉焦点 + 块内光标位置丢失（enterLiveState :2777+:2792 即成对置位）。故 reload 路径也成对置位；这不是「抢焦点」而是「保住焦点」。

`liveCaret_` 显示行备忘：键含 `(stableId, hash, local, width, fontSize)`（`markdown_module.cpp:1700`）。重定位命中分支 = 同 hash 同 local 同字号 → 缓存行本就正确，无需手动清；三元组命中但 stableId 不同（双改回退路径）或零命中 → hash 不同 → 自然失效重测。**两种情况都无需手动清**。

### 3.3 顺序约束（v2 修订 F1：v1 的顺序约束是空操作，必须配合显式写 liveCursorByte_）

`reloadCurrentNote` 内顺序必须为：
```
1. 捕获（setEditorText 前）—— 含 local
2. setEditorText(r.value())          // 既有 :3274
3. 定位（重定位 / 零命中处置）         // 新增，插在这里；命中/零命中都写 liveCursorByte_
4. resetLiveUndoBaseline()           // 既有 :3278（Live 态）—— 读 liveCursorByte_ 播种撤销基线
5. 其余（lastSavedText_/editorDirty_/mtime/toast）
```
**v1 只调顺序不写 `liveCursorByte_` → 步骤 4 读旧字节，顺序约束是空操作（审计 F1 实锤）**。v2 修正：步骤 3 命中/零命中分支都显式写 `liveCursorByte_`（命中 = `new_block.start + min(local, len)`；零命中 = `clamped` 或 0），步骤 4 才读到重定位后的字节 → 撤销基线锚对、`liveUndoBlock_` 对。

### 3.4 撤销基线

既有 `reloadCurrentNote` 已在 Live 态调 `resetLiveUndoBaseline()`（M5），本修复只调整它与定位步骤的相对顺序，不动其实现。

## 4. 改动清单

| 文件 | 改动 | 行为 |
|------|------|------|
| `src/module_markdown/md_block_model.h` | 新增自由函数 `mdFindBlockByContent(blocks, kind, hash, stableId, nearByte) -> size_t`（返回下标或 npos） | 纯函数，无副作用 |
| `src/module_markdown/md_block_model.cpp` | 实现该函数：① 三元组 `(kind,hash,stableId)` 全等命中即返回（唯一，因 stableId 单调递增不重复）；② 三元无命中 → 收集 (kind,hash) 等价块，取 `|start-nearByte|` 最小、等距取下标小者；③ 全无 → npos | O(n)，n=块数 |
| `src/module_markdown/markdown_module.cpp` | `reloadCurrentNote` 加 §3.1 三步 + §3.3 顺序；删除 3259-3265「已知缺口」注释，改写为「已修复（MD-01）」说明 | 仅此一条路径；编辑态/帧循环零触碰 |
| `tests/test_modules/test_md_block_model.cpp` | 新增 `md_block_model_find_by_content`：空文档→npos；三元唯一命中；三元无命中回退 (kind,hash) 最近；重复块（代码围栏×3）tie-break 取下标小者；零命中 npos | 纯函数单测 |
| `tests/test_modules/test_md_live_edit.cpp`（或 `test_markdown.cpp`） | 新增无头用例见 §5.2 | 无头 UI |

## 5. 验收

### 5.1 纯函数单测（`mdFindBlockByContent`）

- 空文档 → npos
- 三元组 `(kind,hash,stableId)` 唯一命中 → 返回该下标
- 三元无命中、(kind,hash) 唯一命中 → 返回该下标
- **重复块 fixture**（审计 major：空段落切分器产不出，必须用字节全同的非空块）：构造 `"\n```\n\n```\n"` 围栏连接 3 遍 → 3 个全同 CodeFence 块（同 kind 同 hash，stableId 各异）；三元组命中精确返回指定实例；三元无命中时 (kind,hash) 多命中取 `|start-nearByte|` 最小、**等距取下标小者**（fixture 避开等距：nearByte 贴近目标块 start）
- 零命中（kind 或 hash 无匹配）→ npos
- 显式禁止用「插入空行」当增量（切分器把空行并入邻块，造不出同 hash 独立块）

### 5.2 无头 UI 用例（实时态 + reload）

**执行前置（v2 修订 F5：轮询用例有真实时钟 + 秒级 mtime 依赖，必须显式记载）**：
- `pollDiskChangesIfDue` 节流 = 真实 `steady_clock`，`kPollIntervalMs=5000`（`markdown_service.cpp`）；无注入缝（`lastPollAt_` 私有、工厂 `create_markdown_module()` 藏 service、headless stub 平台 `hasWatchCredit` 恒 false）。
- `fileMtime` 秒级 + `checkCurrentNoteOnDisk` 严格 `>` 比较 → **同秒写盘检测不到**（代码库已知坑，`test_markdown.cpp:3260` 注释「避免同秒 mtime」）。
- 故每条静默/冲突用例须：外写盘前保证晚于 `lastKnownMtime_` 所在秒（≥1s 间隔）；静默重载帧须在上一 poll 之后 ≥5s（真睡）；或改走**冲突 toast 路径**（`editorDirty_=true` → 轮询弹 toast → 点击 action → `resolveConflict(true)` → `reloadCurrentNote(false)`，不依赖 5s 真时钟，确定性更高）。

公共断言（每条命中用例都要）：`ui.state<MdInputState>("md.live.input").text == liveBlockText(新活动块)`。

1. **前插**（平移场景）：旧文档块 = `"bravo"`，外部在头部插 `"seed line one\n"` → 重载后活动块仍是内容 `"bravo"` 的那块。**走三元组主键命中**（stableId 在平移下保留）。断言 `input.text == "bravo"`、`liveActiveBlock_` 指向 bravo 块。
2. **不相邻双改**（v2 修订 F8：单点改不足以失效——后缀对齐保 id；必须改 A **和 C**）：旧文档 3 块 A/B/C，活动块 = B；外部改 A 和 C 的内容（B 不动）→ A' B C'，p=0 s=0 → B 落窗口丢 id。重载后活动块仍 = B。**走三元组无命中 → (kind,hash) 回退命中**（B 内容未变 → hash 同；但 stableId 因落变化窗口而失配）。断言 `input.text == B 原文`、三元组未命中、(kind,hash) 命中 B。fixture 注：A' 须仍是独立块、不与 B 合并。
3. **删块**（零命中 + 空文档 guard）：旧活动块 B 被外部整段删除 → 零命中 → 走钳位。**多帧断言**：重载帧后 `liveActiveBlock_ == blockIndexAt(min(旧 liveCursorByte_, new_size))` 且 `input.text == 覆盖块原文`、不崩、当帧收敛。补一个**非空→空文档**重载子用例（零命中最坏形态，验证空文档 guard：`liveActiveBlock_=0`、`liveInputStart_=0`、`liveCursorByte_=0`、input.text 为空或单块原文、不崩）。
4. **重复块**（v2 修订 F7：`abc\n\nabc\n` 产不出全同块——空行并入下一块头 → 字节不等；改用 `a\n\na\n\na\n`）：旧文档 `a\n\na\n\na\n` → 切分器产 3 块，p1=`a\n`、p2=`\na\n`、p3=`\na\n`（p2==p3 全同，stableId 各异），活动块 = p2；外部前插一个 `a\n` 完整副本 → 3 个全同块（p2/p3 同 hash）。**走三元组主键命中**（stableId 区分实例）→ 活动块仍 = 原 p2 实例（不是第一个）。断言 `input.text == "\na\n"`、三元组命中、落在 stableId 对应的实例。fixture 避开等距 tie（nearByte 不取中点）。或复用 §5.1 的代码围栏×3 fixture。
5. **CRLF 改写**（审计 minor：行尾批量改写 = 全块 hash 失效）：同一正文 LF→CRLF 外部重载 → 所有块 hash 含 `\r` 变化 → 零命中走钳位。断言不崩、零命中、`input.text == 覆盖块原文`。**声明 by-design**（内容语义相同但字节变 → 映射无法命中；§6 风险表登记）。
6. **冲突 toast 路径**（v2 修订 F5：确定性高于静默轮询，推荐优先）：设 `editorDirty_=true`（先打一个字）→ 外写盘（≥1s 间隔避免同秒 mtime）→ 触发轮询弹冲突 toast → 点击「重载磁盘版本」（`resolveConflict(true)` → `reloadCurrentNote(false)`）→ 走同一映射断言（同用例 1 的命中语义）。验证两条入口共用实现。
7. **捕获越界竞态**（v2 修订 F5：原措辞「5s 轮询静默重载」与 compose 顺序不符，实为冲突路径）：构造「输入 onChange 当帧收缩 blocks（块首退格合并 2→1）→ editorDirty_=true → 外写盘 → 点击冲突 toast 重载」→ 步骤 1 在已收缩模型上 `mdLiveActiveBlockIndex` 重 derive（对非空恒 in-range，防越界），不崩、走零命中或正确命中。若 5s 真时钟难确定性构造，全用冲突路径覆盖。
8. **非实时态回归**：编辑态/预览态触发 reload → 不走映射（`mode_ != Live`），行为零变化。

> 用例 6/7 改走冲突 toast 路径后不依赖 5s 真时钟；若仍要覆盖静默轮询路径，每条须 ≥5s 真睡 + 秒级纪律，登记为 deferred 或要求给 `lastPollAt_`/`kPollIntervalMs` 开测试注入缝（friend 测试类或构造注入 interval——动生产类需列白名单）。

### 5.3 零回归

既有 `md_block_model_*` 全套用例 + `test_markdown.cpp` 入口回归用例零改动通过（`mdFindBlockByContent` 是新增自由函数，不触碰既有 `update`/`blockIndexAt` 签名）。

## 6. 风险与未知

| # | 风险 | 验证 / 处置 |
|---|------|------|
| 1 | 重复块三元组命中后，tie-break「下标小者」在等距时主观 | 用例 4 fixture 避开等距；现实重复块多为相同代码围栏，stableId 三元组命中即精确实例，不触发 tie-break |
| 2 | `(kind, hash)` 碰撞：两块内容完全相同但语义不同 | hash = FNV-1a 全字节，碰撞概率极低；kind 也参与匹配，已降一档；三元组用 stableId 区分实例 |
| 3 | 大文档 O(n) 扫描 | n=块数（百级），单次 reload 一次，远低于分块本身的 O(n) 行扫描 |
| 4 | 零命中走钳位 = 现状，用户可能期望「跳到块 0」 | 不改：内容已删，钳位是「留在最近位置」的保守选择；可后续 UX 拍板 |
| 5 | **CRLF 行尾批量改写**（外部编辑器 LF→CRLF）：全块 hash 失效 → 零命中钳位（by-design） | 用例 5 验证不崩；内容语义相同但字节变，映射无法命中，登记为已知限制 |
| 6 | 捕获越界竞态（onChange 收缩 blocks + 5s 轮询重载） | 步骤 1 重 derive + 边界检查；用例 7 验证 |

## 7. 流程

本 spec → 独立审计（已完成，3 视角 CONFIRMED 项已合入本修订版）→ 派 1 个开发子代理（worktree 隔离，定向测试 `md_block_model` + `test_md_live_edit` 的 reload 用例，原子 commit）→ 合并后真编译一次（`merge-tree` 无冲突 ≠ 能编译）→ 再开 M7。
