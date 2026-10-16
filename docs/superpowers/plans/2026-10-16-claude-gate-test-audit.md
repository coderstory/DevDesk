# 审计：worktree-claude-gate-test（7871b8b，+246 行）

已核：4 用例真行为级——EventBus emit + Router 负载 + compose，未直调内部函数。观察缝成立：quotaStore_ 唯一写点 claude_module.cpp:807（startQuotaRefresh），弹窗预填 :848 读 loadConfig，dd.label 节点真存在（EUI-NEO dropdown.h:122）。
变异①删 :345 总开关合取 → 恰 2 红（gateoff / gateon 基线 :400），还原后 quota 179/179 绿，git status 干净。
变异②删 :800 幂等早退 → 仍 179 全绿。
用例④订阅序依赖：event_bus.h:81/96 vector 保序派发，结构稳定但契约未写明（low）。
卫生：+246 行恰为 4 gate 用例，零生产改动，无墙钟/时序依赖。
另：--filter claude 334/335，1 红 test_claude_service.cpp:2991 在 dev 主树同红，既有环境问题，与本票无关。

F1 [tests/test_modules/test_claude_quota_lifecycle.cpp:444-447] low：③「幂等不双起」断言经预填缝不可判异——双起后预填仍 DeepSeek，删幂等守卫不变红，名实不符；③的暂停/恢复段真实。
F2 [src/core/event_bus.h:75-84] low：派发序=订阅序仅是实现性质，头文件契约未写明；用例④靠它排序。

裁决：**放行带必修**——F1 改口径（删「幂等」声明）或补可判异断言；F2 补一行契约注释。
