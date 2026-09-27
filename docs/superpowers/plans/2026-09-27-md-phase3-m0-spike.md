# Markdown Phase 3 · M0 技术验证 spike（门禁）——结论与手工点验清单

- **日期**：2026-09-27
- **依据 spec**：`docs/superpowers/specs/2026-09-27-markdown-phase3-block-render-design.md`（§2.2 待验证项 V1–V4 / §4.1 关键选型 / §5 性能目标 / §6 风险 R1–R11）
- **无头基座**：`tests/test_modules/test_md_live_spike.cpp`（9 个用例，用例名统一 `mdspike` 前缀）
- **复跑命令**：`cmake --build build --target devdesk_tests` → `./build/tests/devdesk_tests.exe mdspike`
- **实测结果**：9/9 PASS（2026-09-27，Windows 11 / MinGW64 / 无头）
- **被测框架版本**：EUI-NEO 本地 main（configure 打印 rev `8a1227c`）；文中 `file:line` 均对该 rev

---

## 0. M0 门禁结论

**四项全部有解，可以进入 M1**——但带 **3 条接线硬约束**（§3），其中 1 条会改变现有代码写法（`requestFocus` 的 id 口径），1 条限定 R7 的 `releaseStateScope` 作用范围，1 条给出 R5 的归零时机。

| # | 待验证 | 结论 | 一句话 |
|---|--------|------|--------|
| V1 | 稳定 id 的 input 跨帧换位/换父级 | **成立（有条件）** | 同 id 搬到别的父级/位次后，元素身份、焦点、`InputState`、几何、键派发目标全部存活；条件 = 编程式聚焦必须打到 `<input id>.hit` |
| V2 | 焦点元素父级被合并/删除时是否丢焦点 | **不成立（方向反了）** | 元素消失**不会**清 `focusedId_`，而是留成"陈旧焦点"（键盘落空但不丢状态）；真正会清焦点的是 `releaseStateScope` 覆盖该 id |
| V3 | input 作为 scrollView 流式 content | **成立** | 声明高度参与流式布局与内容测量（滚动范围随之变）；滚动偏移下 pointer→caret 落在点击行；`imeRect` 在元素局部坐标系 |
| V4 | 换文本瞬间滚动残余是否造成一帧跳位 | **条件成立** | 贴合高度（D2 默认）下**无**跳位；仅「封顶块（R4）+ `followCaret=false`」同时成立时首帧跳位，按 R5 同帧归零消除 |

**R1（兜底方案 B/C）本次不需要**：V1 成立，单输入方案（D1）的立身之本是安全的。兜底 B（独立 overlay 层绝对定位）/ C（上游 PR wrapContentHeight）**备而不用**。

---

## 1. 逐条证据（用例名 + 关键断言）

### V1 成立（有条件）——`mdspike_v1_relocate_keeps_identity`

两帧之间把同一 id 的 input 从「根 column 第 1 位」搬到「嵌套 column 第 1 位」（换位次 + 换父级），断言：

```cpp
DOCTEST_CHECK(env.ui->find(kInputHit)->focusable);            // ① 元素身份：同 id 仍在帧树中
DOCTEST_CHECK(env.ui->isFocused(kInputHit));                  // ② 焦点保持
DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);  //    组件仍按 focused 渲染（画 caret）
DOCTEST_CHECK_EQ(st.text, text);           DOCTEST_CHECK_EQ(st.cursor, 5);       // ③ InputState 存活
DOCTEST_CHECK_EQ(st.styleRevision, 42ULL); //    build() 从不触碰的字段 → 是同一个 state 对象
DOCTEST_CHECK_EQ(st.textRevision, rev1);   //    没被当"外部改文本"重灌
DOCTEST_CHECK(near(st.lastBounds.y, 2.0f));//    瞬态字段也活着
DOCTEST_CHECK(near(y2, nx->frame.y + nx->frame.height + 4.0f));  // ④ 几何按新位置重算
DOCTEST_CHECK(hit2->onKeyEvent(right));                     // ⑤ 键派发目标仍是该实例
DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).cursor, 6);   //    右移一位 → 确是本实例在收键
```

第三帧再搬回根 column 末位，同样存活（反复搬家幂等）。**机制（源码取证）**：焦点是 `focusedId_` 字符串、每帧 `Ui::begin` 后由 `Runtime::compose` 重新灌回（`core/runtime/runtime_lifecycle.h:22`），键/文本派发每帧用 `ui.find(focusedId_)` 现查（`runtime_input.h:295` 键 / `:285` 文本），元素 id 只受 `pageId + 显式 scope 栈`影响、**与树位置/父级无关**（`core/dsl.h:1607-1628`；只有 `LoaderBuilder::content` 会 `pushScope`，DevDesk 未用 loader）。

### V1 的**条件**——`mdspike_v1b_focus_target_must_be_hit_id`

`requestFocus` 的目标 id 必须是 `<input id>.hit`（input 组件判定聚焦用的是 `ui_.isFocused(id + ".hit")`，`components/input.h:113-114`）：

```cpp
// ① 发到基 id（DevDesk 现有写法）
DOCTEST_CHECK(env.ui->isFocused(kInput));                     // 焦点确实"设上了"
DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));            // 但组件判定未聚焦
DOCTEST_CHECK(env.ui->find("live.input.cursor") == nullptr);  // → 不画 caret
DOCTEST_CHECK(env.ui->find(kInput)->onKeyEvent == nullptr);   // → 基 id 元素没有键处理器，按键落空
// ② 发到 .hit
DOCTEST_CHECK(env.ui->isFocused(kInputHit));
DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);
DOCTEST_REQUIRE(env.ui->find(kInputHit)->onKeyEvent != nullptr);
```

**这是本次 spike 挖出的真问题**（不是框架 bug，是调用口径）：DevDesk 现有 4 处
`ui.requestFocus("md.editor.input")`（`src/module_markdown/markdown_module.cpp:1611 / 2425 / 2453 / 2616`）
发的都是**基 id**。后果：① 焦点被搬到 stack 元素上（若此前焦点在 `.hit` 上，还会**把已有焦点抢走**）；
② 组件判定未聚焦 → caret 不画、`hasComposition` 恒 false（IME 参考帧被关）；
③ 按键落到 `keyEventHandler_` 兜底，进不了 input 内置编辑。
本 spike **只验证、不改生产代码**（生产代码只读）。M3 接线建议：
- 统一常量（如 `kLiveInputHitId = "md.live.input.hit"`）并只发 `.hit`；
- 顺手核对编辑态那 4 处是否为既有缺陷（属 M0 范围外，登记给 Phase 3 backlog 决定是否单独修）。

**另注（一帧延迟，可接受）**：`requestFocus` 是帧末消费（`runtime_lifecycle.h:38-42`），所以发起帧内组件的 `focused` 判定仍是旧值 → caret/边框**次帧**出现（≈16ms）。点击渲染块定位时不必额外补偿；若要求"发起帧内即见光标"，只能同帧直写 `state` 并接受下一帧才画。

### V2 不成立（方向反了）——`mdspike_v2a_unmount_leaves_stale_focus`

input 整体从帧树消失的那一帧：

```cpp
DOCTEST_CHECK(env.ui->find(kInput) == nullptr);
DOCTEST_CHECK(env.ui->find(kInputHit) == nullptr);
DOCTEST_CHECK(env.ui->isFocused(kInputHit));   // ← 焦点未清（isFocused 只比字符串，不校存在性）
```

次帧同 id 重新出现（块合并回来）→ **焦点 + 旧 `InputState`（光标/选区/undo 场景）静默恢复**，无需补发 `requestFocus`：

```cpp
DOCTEST_CHECK(env.ui->isFocused(kInputHit));
DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);
DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).dragAnchor, 11);  // StateStore 未被释放
```

**结论**：spec §2.2 V2 担心的"合并瞬间丢焦点 → 打字丢失"**不会发生**（`focusedId_` 的清点只有三处：指针按下重定焦、禁用子树、`releaseStateScope` 前缀命中——`runtime_input.h:227-249`、`runtime_lifecycle.h:28-45`；元素消失不在其列）。风险换成另一形态：**陈旧焦点**——焦点指向不存在的元素期间，按键静默落到 `keyEventHandler_`（打字无处可去），且重挂载时会带着旧文本/光标"复活"。运行期语义上这与"焦点还在，只是元素暂时不在"一致，够用；但 M3 需要明确两件事：① 无活动块时是否要显式清焦点/禁用输入；② 重挂载恢复是"想要"还是"要避免"。

### V2 的兜底前提——`mdspike_v2b_requestfocus_when_absent_is_dropped`

```cpp
env.frame([&](Ui& ui) { ui.rect(...); ui.requestFocus(kInputHit); });  // 本帧树里没有该元素
DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));
env.frame([&](Ui& ui) { /* 元素就位 */ });                              // 请求不排队
DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));
```

→ 兜底/R2 的"同帧补发 `requestFocus`"**必须与元素同帧**，不能提前也不能事后补（与 `dsl.h:1518` 注释一致）。

### V2 的真正清焦点路径 + R2 缓解成立——`mdspike_v2c_release_scope_clears_then_same_frame_request_restores`

```cpp
env.frame([&](Ui& ui) { buildInput(ui); ui.releaseStateScope(kInput); });  // ① 只释放
DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));                 // 焦点被清（前缀匹配）
DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).cursor, 0);     // 且 InputState 连根清掉
env.frame([&](Ui& ui) {                                            // ③ 同帧「先释放、后补发」
    buildInput(ui);
    ui.releaseStateScope(kInput);
    ui.requestFocus(kInputHit);
});
DOCTEST_CHECK(env.ui->isFocused(kInputHit));                       // 焦点恢复
```

**两条硬约束**（对 R7 的"块切换释放旧块状态"）：
1. **单输入 `md.live.input` 的 id 绝不能落在被 `releaseStateScope` 释放的前缀下**——否则每次切块都会清焦点 + 把 `InputState`（光标/选区/undo/`followCaret`）清零重建。若实现上无法避免（例如把 input 放进块 scope），则必须在**同一帧**补发 `requestFocus`——顺序天然成立：`Runtime::compose` 先消费 released scopes（`runtime_lifecycle.h:28-34`）再消费 pending focus（:38-42），补发者后生效。
2. 释放后**状态不会自动回来**（光标归 0、`styleRevision` 归 0），焦点恢复 ≠ 编辑现场恢复；切块要保现场就得在释放前把值搬到新 scope，或干脆不释放单输入。

### V3 成立——`mdspike_v3a_input_as_flow_content`

scrollView（600×300，gap 20）内 content = `[top 60px] + input(声明高 H) + [bottom 60px]`：

```cpp
DOCTEST_CHECK(near(in->frame.y, top->frame.y + top->frame.height + 20.0f));   // ① 流式定位生效
DOCTEST_CHECK(near(bottom->frame.y, in->frame.y + kFitH + 20.0f));           //    兄弟被 input 高度推下去
DOCTEST_CHECK(near(sv->scrollMaxOffset, contentH - 300.0f));                 // ② 高度进入内容测量
DOCTEST_CHECK(sv->scrollMaxOffset > 0.0f);
DOCTEST_CHECK(page.ui.find("live.scroll.scroll.thumb") != nullptr);          //    可滚 → 有滚动条
DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll == nullptr);                 // ③ D2 贴合高度 ⇒ 无滚轮处理器
// ④ 换矮 input（40 < 内容 720）→ 内容不再超高
DOCTEST_CHECK(near(page.ui.find("live.scroll")->scrollMaxOffset, 0.0f));
DOCTEST_CHECK(page.ui.find("live.scroll.scroll.thumb") == nullptr);
DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll != nullptr);                 //    非贴合 ⇒ 有内部滚动 ⇒ 吃滚轮
// ⑤ 偏移由框架钳制
DOCTEST_CHECK(near(page.ui.find("live.scroll")->scrollOffset, 120.0f));
DOCTEST_CHECK(near(...->scrollOffset, ...->scrollMaxOffset));                // offset=99999 → 钳到 max
// ⑥ 对照：封顶块（300 < 720）
DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll != nullptr);
```

**要点**：
- spec §2.1 的核心收益（**贴合高度 ⇒ input 不注册滚轮处理器 ⇒ 滚轮穿透外层**）被直接证实（③）。
- 同一机制的反面：**任何非贴合高度（含"太矮"）都会让 input 吃掉滚轮**（④⑥）——这就是 §5 #4 那句 `[live] input over=N` 日志校验的实机意义：高度公式错一点，用户就会遇到"轮盘死区"。
- input 的声明高度确实进入 `scrollView` 的内容测量，滚动范围随它变（②④）→ 换块时 `contentKey` 必须含**内容 hash + 宽度**（spec §3.3），否则 `MeasureCache`（`components/scrollview.h:142-158`）会命中旧高、滚动范围与几何错位。本 spike 用"不传 `contentKey`"的每帧现算路径验证了测量这一半；缓存失效那一半属 M6。

### V3 成立（指针→caret）——`mdspike_v3b_pointer_to_caret_under_scroll`

滚动到 `offset=120` 后点视口中部（屏幕 y=150）：

```cpp
const eui::Rect shifted(in->frame.x, in->frame.y - offset, hitW, in->frame.height);  // = Runtime 会传的 bounds
spikePress(hit, shifted, 300.0, 150.0);
DOCTEST_CHECK(lineOfByte(s2.cursor) == 7);                 // 落在点击的那一行（第 7 行，0 基）
DOCTEST_CHECK(near(s2.lastBounds.y, shifted.y));           // lastBounds 记的是变换后的 bounds（onDrag 用它）
// 反控：bounds 不折算滚动位移
spikePress(hit, unshifted, 300.0, 150.0);
DOCTEST_CHECK(lineOfByte(s3.cursor) != 7);
DOCTEST_CHECK(lineOfByte(s3.cursor) == 7 - 5);             // 正好差 offset/lineH = 5 行
// IME 候选框锚点
DOCTEST_CHECK(hit2->hasImeRect);
DOCTEST_CHECK(near(hit2->imeRect.height, kLineH));
DOCTEST_CHECK(hit2->imeRect.y > 0.0f);                     // 元素局部坐标（非屏幕坐标）
```

**机制（源码取证，解释了为什么"折算后的 bounds"是承重的）**：滚动位移是内容列的 **render transform**（`runtime_state_bindings.h:157-178`，`translate.y -= offset`，由 `runtime_update.h:802/863/938/978` 灌进实例），命中测试把父级变换继承给子元素（`runtime_input.h:156-167`），`onPress` 拿到的是变换后的 `interactionBounds`（`runtime_input.h:374-382/459-463`）；而 `InputModel::cursorFromPointer` 用 `pointerY - bounds.y` 反算行（`components/input_model.h:202-208`）→ 两者配对才正确。IME 侧同理：`imeRect` 记在**元素局部坐标**（`components/input.h:198`），平台桥再叠加 `toPixelRect(frame) + hitMatrixForElement`（`runtime_input.h:318-360`）→ 候选框随滚动走。

**无头没覆盖的部分**：真窗口下"框架确实把变换后的 bounds 传进 onPress"没有在无头里跑通（事件队列需真窗口），上表两次点击是**按框架算法手工折算**的——该环节靠源码取证 + 手工点验 ⑥⑦ 兜底。

### V4 条件成立——`mdspike_v4a_swap_text_scroll_residual_capped_block`

封顶块（高 300 < 内容 720，`maxVerticalScroll=436`），先在块内滚一格（`followCaret=false`，`verticalScroll=132`），再用 caret 元素的 y 读首帧几何（`caret.y = inset + 末行号×lineH − verticalScroll`）：

```cpp
DOCTEST_CHECK(near(caret->frame.y, lastLineY));                        // 未滚动基准（704）
hit->onScroll(wheel);                                                  // 滚一格 → 残余 132
DOCTEST_CHECK(near(s.verticalScroll, residual));  DOCTEST_CHECK_FALSE(s.followCaret);
DOCTEST_CHECK(near(caret->frame.y, lastLineY - residual));             // 残余可见（572）
// ① 换文本（A→B）且 followCaret=false → 首帧带残余 = 一帧跳位成立
DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).text, t2);
DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY - residual));
// ② R5 缓解：切块帧（B→C）compose 前归零 → 无跳位
{
    InputState& s = env.ui->state<InputState>(kInput);
    s.verticalScroll = 0.0f;
    s.followCaret = false;
}
env.frame(...t3...);
DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).text, t3);
DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY));
// ③ 打字路径自愈：followCaret=true 时框架同帧按光标重算滚动
DOCTEST_CHECK(near(env.ui->state<InputState>(kInput).verticalScroll, 0.0f));
DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, kInset));
```

**触发条件（三者同时）**：① 该块被封顶（`maxVerticalScroll > 0`，即 R4 的超大块分支）；② `followCaret == false`（用户在该块内滚过轮、或流程里刚被置 false）；③ 换文本时 `focused == true`（实时态恒真 → `input.h:143-147` 只把光标搬到新文本末尾，**不清 `verticalScroll`**；只有未聚焦才走"水平/垂直滚动归零"分支）。
**兜底（= R5 缓解，已验证有效）**：切块帧 compose 之前把 `verticalScroll` 归零（`followCaret` 保持 false）→ 新块首帧干净；顺序上模块在 `drawLiveColumn` 内直接写 `state` 即可，属同帧生效。

### V4 在 D2 默认路径下不成立——`mdspike_v4b_fit_height_has_no_residual`

```cpp
DOCTEST_CHECK(env.ui->find(kInputHit)->onScroll == nullptr);   // 贴合 → 连滚轮处理器都没有 ⇒ 无残余来源
env.frame(...t2...);                                          // 直接换文本，无任何缓解
DOCTEST_CHECK(near(env.ui->state<InputState>(kInput).verticalScroll, 0.0f));
DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY));
```

→ 只要高度按 D2 公式贴合（`h = inset×2 + 行数×lineH`），残余恒被钳到 0，**不需要**任何额外缓解。

---

## 2. 对 M1/M3 的接线硬约束（spike 额外产出）

| # | 约束 | 来源 | 影响里程碑 |
|---|------|------|-----------|
| C1 | 编程式聚焦只发 `<input id>.hit`；建议定义常量避免再写错 | V1b | M3（并回溯编辑态 4 处调用） |
| C2 | 单输入 id **不得**位于被 `releaseStateScope` 释放的前缀下；否则切块清焦点+清状态；若不可避免，必须同帧补发 `requestFocus`（顺序天然安全） | V2a/V2c | M3（R7） |
| C3 | 切块帧需显式 `verticalScroll = 0`（封顶块场景；贴合路径无需） | V4a | M3/M6（R5） |
| C4 | input 高度必须是内容贴合值；任何"偏矮"都会让 input 吃滚轮 | V3a ④⑥ | M2（D2、§5 #4 日志校验） |
| C5 | `requestFocus` 帧末消费 ⇒ caret 次帧出现（≈16ms）；点击定位无需补偿 | V1 | M3（观感） |

---

## 3. 无头覆盖边界（本 spike **没有**证明什么）

1. **事件队列段**：按键/指针从窗口队列到 `Runtime::update` 的这一跳需要真窗口，无头不可驱动。键用例调的是 `updateKeyInput` 内部同一调用点（`focusedElement->onKeyEvent(key)`）；指针用例按框架算法手工折算 bounds 后直接调 `onPress`/`onScroll`。→ 由手工清单 ①–⑦ 兜底。
2. **真实 IME**：`compositionText`/预编辑快照、候选框真实落点、组字中切块的安全性（R3 冻结）均未覆盖 → 手工清单 ⑧。
3. **真实渲染/脏区**：光栅化、脏矩形正确性、首帧跳位的**肉眼**观感（本 spike 用几何代理证明）→ 手工清单 ⑨⑩。框架侧机制已取证：元素结构变化 ⇒ `fullPaintRequested_ = true` + `pruneInstancesRequested_ = true`（`runtime_lifecycle.h:47-51`），且每次 compose 后下一帧为全量树更新（:57）。
4. **性能（§5 #1–#5）**：本次不做（M9）。
5. **`contentKey` 缓存失配**：见 V3a 说明，属 M6。
6. **`stateStore_` 条目数增长（R7/§5 #6）**：无公开 API，按 spec 的日志计数近似方案留 M9。

**断言有效性（反幻觉自查）**：每个探针都配了**反向观测**，不会恒真——`find` 有 V2a（消失时为 null）；`isFocused` 有 V1b（基 id 为 false）+ V2a（元素不在仍为 true，说明它只比字符串）；`onScroll` 有 V3a 贴合/非贴合两极；caret 几何有 V4a 的 572/704 两极；`cursor` 落点有 V3b 的 7/2 两行。开发过程中 V4a 曾因预期写错而真实失败（`followCaret=true` 会把 caret 拉进视口）→ 说明探针读的是真实几何。

---

## 4. 手工点验步骤清单（用户执行）

> 前置：`bash scripts/build-test-run.sh`（增量构建 + 全量测试 + 起 app）；所有点验都在 **Markdown 笔记 → 「实时」态**下做。
> 判定口径：**未截屏 = 未完成**；每项记录"通过/不通过 + 截图"。

| # | 步骤 | 观察点（判定标准） | 对应 |
|---|------|------------------|------|
| ① | 进入实时态，点第 2 个渲染块的中间 | 该块变源码态、**光标落在点击那一行**、无需二次点击；顶栏"实时"高亮 | V3、V1 |
| ② | 接着直接用键盘连打 30 秒（含中文、英文、退格） | 字进得去、光标跟随、无丢字/无错位 | V1（键派发）、§5 #2 |
| ③ | 点击最后一个块 → 切到上一块 → 再切回 | 每次切换后**直接打字**都进得去（不用先点一下）；切换瞬间无"打字丢失" | V1、C1 |
| ④ | 把两段相邻文字用 Backspace 合成一段，合完立刻继续打字 | 合并瞬间无丢焦点、无丢字；观感上是同一块里继续编辑 | V2（陈旧焦点） |
| ⑤ | 在块底部空白行打字（把块撑长） | 新行出现、caret 始终可见；外层滚动条随内容增长（**不出现 input 自己的滚动条**） | V3（贴合）、V4 |
| ⑥ | 打开长文档（>2000 行），滚动到中部，然后**点击画面中部某个渲染块** | caret 落在**点击处那一行**（不是偏上/偏下几十行） | V3b（真窗口的变换链路） |
| ⑦ | 用鼠标滚轮在实时态连续滚动 10 秒（含在块上方/代码块上方滚） | 滚动流畅、**滚轮不被块吞掉**（无"滚两下不动"的死区）；对照 §5 #3 帧率 | V3a、§5 #3 |
| ⑧ | 中文拼音：输入拼音（不按空格）→ 在预编辑状态下直接点**另一个块** | 不应把组字撕裂/丢字；候选框贴住 caret（不飞到窗口角落、不随滚动漂移） | R3、V3（imeRect） |
| ⑨ | 长围栏代码块（>2000 行）内滚动 | 该块封顶（不超过 0.7×视口高）并出现**块内滚动条**；此时块内滚轮吃轮属预期（R4 已接受） | R4、§5 #5 |
| ⑩ | 切块瞬间盯住画面（或录屏逐帧看）：先在一个**已滚动过的封顶长块**里滚几下，再点另一个块 | 新块首帧**不应**带着上一块的滚动位置（不应出现"闪一下才归位"） | V4（R5） |
| ⑪ | 切到实时态 / 切回编辑态，各截一张同段落图 | 块间隙、圆角、代码块色表与双栏预览一致（实时 ↔ 预览观感一致） | spec §8.3 ⑨ |
| ⑫ | 100% / 130% 两档字号各试一次 | 活动块高度仍贴合（无内部滚动条出现）；输入不出现"吃滚轮" | R10、C4 |
| ⑬ | 编辑态里用大纲跳转 / 新建笔记自动聚焦（走 `requestFocus` 的既有路径） | 跳转后**直接打字能进**（若发现必须再点一次输入框才能打字 → 即 C1 的既有缺陷现形，请截图回报） | C1（现有 4 处） |

---

## 5. 未决 / 待拍板

1. **C1 的既有 4 处调用**（编辑态 `requestFocus("md.editor.input")`）：单独修还是随 M3 一并改？（本 spike 未动生产代码）
2. **V2a 的"重挂载复活"**：块合并/拆分导致 input 短暂消失时，是否要求显式清焦点与状态（避免"带着旧光标复活"）？
3. **V4 的封顶块残余**：接受 R5 的"同帧归零"，还是进一步在封顶块切换时也重置 `followCaret`？
