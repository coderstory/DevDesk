# 任务书：BUG-4 project_cards upsert created_at 测试覆盖补课（P2，S 级）

mode: edit；worktree：从 dev 新建 `wt-bug4-createdat`；开工前 `git log dev --oneline -3` 确认无人做过。

## 背景
`src/module_settings/project_card_store.h` 的 `upsertProjectCard` 用 `INSERT ... ON CONFLICT(name) DO UPDATE` 保证「created_at 建卡时写定、更新不动」。QA 变异注入 `created_at=excluded.created_at` 后断言仍绿——现有测试没有钉住这个语义（红证不可复现，见史密斯 10-30 验收报告）。

## 目标
在 `tests/test_modules/test_project_cards.cpp` 追加两条用例（先写测试、跑出红或确认既有绿是假绿来源）：
1. **建卡写定**：建卡后 `created_at` 非空且等于插入时刻值；同 name 二次 upsert（改其余字段）后 `created_at` 与首次一致、`updated_at` 刷新。
2. **变异必红**：把 SQL 里 `DO UPDATE SET` 的 `created_at` 改成 `excluded.created_at`（模拟回归）后，用例 1 必须红；改回后绿。测试正文加注释钉死防的回归（AI 顺手改 upsert 字段清单时破坏 created_at 语义）。

## 硬约束
- 只动 tests/，零 src 改动（若发现 src 语义真坏了——created_at 会变——升级报卡点，不自行修）。
- 构建测试 `bash scripts/build-test-run.sh --filter project_cards`，DEVDESK_JOBS=2。

## 验收（回单三件套）
- [ ] 两条用例绿（Total/Failed: 0 关键行）。
- [ ] 变异红证据：变异态下用例 1 的失败输出关键行（先红后绿）。
- [ ] sha + 文件清单 + 一句说明。
