# 01 · 401（token 失效）无定向引导

Status: `needs-triage`（**实现已完成，待复核关票**）
建议优先级: 高
来源: handoff §3.C「S9」+ 2026-10-17 静态复核
实现: 已完成 `d5c8147`（2026-10-17）；待 D 类真机走查确认 401 时确实出现定向引导

## 现象

token 失效（401）时走**通用错误弹窗**（标题「同步失败详情」，主按钮「重试」，另有「复制详情」「关闭」）。用户拿不到「下一步该干什么」的动作 —— 弹窗里没有一个指向「去更新 Token」的路径。

## 证据

- `src/module_settings/git_error_dialog.h:2` — 弹窗定位是通用件：「中文摘要 + 可滚动请求日志列表 + 重试/复制详情/关闭」
- `src/module_settings/git_error_dialog.h:22` — 变体只有 `Error`（主按钮「重试」）
- `src/module_settings/git_error_dialog.h:23` — 另一个变体是 `Conflict409`（主按钮「拉取远端后再推」+ 二次确认）
- → **没有第三个「认证失败」变体**；401 与网络失败、其他 HTTP 失败共用同一个 `Error` 变体
- `src/module_settings/github_sync_card.cpp` 内**无** 401 专有分支（`grep 401` 零命中）

## 与既有断言的关系（重要）

底层**文案**其实是对的方向：`tests/test_platform/test_github_api.cpp:193` 明确断言「401 文案必须给出可执行动作（指向 PAT），只报『令牌无效』用户无从下手」。所以缺口**不在 API 层文案，而在 UI 层动作**：文案里说了「检查/更新 PAT」，但弹窗没给出执行它的入口（Token 输入框在卡片下方，弹窗没把你导过去）。

→ 修的时候要注意：**别改坏 `test_github_api.cpp:193` 已钉住的 API 层文案**。

## 修复方向（待裁）

1. **新增 `AuthFailed` 弹窗变体**：主按钮「去更新 Token」（关闭弹窗 + 聚焦/高亮卡片里的 Token 输入框），次按钮「复制详情」
2. **在 Error 变体上按错误码切换主按钮**：不新增变体，401 时主按钮换文案与动作（改动更小，但混用了两种语义）
3. **仅文案强化**：在现有 `Error` 弹窗摘要里补「请到上方 Token 输入框更新后重试」—— 零结构变化，但没有动作

规格依据：`docs/superpowers/specs/2026-10-15-github-sync-api-design.md` §6 错误处理表已把「认证失败（401）」单列一行为「弹窗文案指向『请检查/更新 PAT』」—— 即**规格要求定向引导**，当前实现只做到了文案层。

## 验收判据（待定稿）

401 时用户从弹窗一步到达「更新 Token」的操作面；409 与普通失败的行为不回归；API 层 401 文案断言仍绿。

## Comments

- **2026-10-17 实现完成（`d5c8147`）** —— 采用「修复方向 1」的轻量版：
  - `git_error_dialog.h` 新增 `GitErrorDialogVariant::AuthFailed` + 纯函数 `variantForSyncFailure(logs, conflict)`；判据取**请求日志状态码 401**（`SyncRunResult` 不带错误码，`ApiLogEntry.statusCode` 是 UI 层唯一可靠信号，胜过匹配摘要文案），优先级 `409 > 401 > 通用`，空日志（未观察到任何请求）→ 通用
  - 弹窗：标题「认证失败（Token 无效或已过期）」/ 主按钮「去更新 Token」；401 下主按钮**不触发通用重试**（重试只会再吃一次 401）
  - `settings_module.cpp` 失败分支改用该纯函数；`updateToken` = 关弹窗送回卡片 + toast「请在上方 Token 输入框更新后重新同步」
  - TDD 轨迹：先红（401 决策 + 绘制分支两条断言按预期失败，**对照组全绿**证明缝行为保持）→ 实现 → 定点转绿 `2/2` → 全量门禁 `1439/1433/0` + probe 7/7
  - 测试落点：`tests/test_modules/test_github_sync_card.cpp` 的 `github_error_dialog_auth_variant_decision`（纯函数；含「非 401」「空日志」「409 优先」「混合日志」四组对照）与 `github_error_dialog_auth_variant`（绘制 + 点击分派；含通用变体对照）
  - **未做**：没有「高亮/聚焦 Token 输入框」——卡片未暴露聚焦 API、设置页无 scrollTo；当前是关弹窗回到卡片（Token 框就在卡片顶部）+ 一句提示。若走查认为引导不够强，另开子票。
  - **验证边界**：模块接线（失败路径调用纯函数、`updateToken` 的实际动作）无 UI 注入级测试 → 待 D 类真机走查确认。
