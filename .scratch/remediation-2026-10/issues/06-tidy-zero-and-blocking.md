# 06: tidy 存量清零并转 blocking

**What to build:** 05 号票让 clang-tidy 在 CI 上以报告模式跑起来后，存量告警是既有代码欠账。本票分两段：**清零**——按检查项分类成批修复存量 error（每批保持 ctest 绿，不搞大爆炸）；**转 blocking**——清零后将 tidy 步骤设为 required，error 即红。修复取舍遵循「行为不变优先，确需改行为的在票面逐条记录」。

**Blocked by:** 05（存量清单来自其报告）

**Status:** ready-for-agent

- [ ] 当前代码 tidy error 存量清零（清零前后报告对比留档）
- [ ] tidy 步骤转 required（CI error 即失败）
- [ ] 每批修复 ctest 全绿（Failed: 0，Total 不降）
- [ ] 合入门槛：9 个 `check-*.sh` 守卫全过
