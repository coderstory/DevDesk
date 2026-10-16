# 审计：worktree-sync-engine（9501514/674655a/c40a57f，基 dev a78a3e7）

被审树 /Users/coderstory/dev/gui/wt-sync-engine（HEAD c40a57f，终态 git status/diff 干净）。

**A1 等价性**：9501514 删除行（731）与 engine.cpp（824）归一化全量比对（this->/deps_.、六缝映射、api->/api. 归一）：**非纯搬家逻辑行 = 零**。非等块仅四类：新文件样板与 #include 重排；注入缝本体（makeApi/publish/aborted/now/四路径 + mergeSubResult 等）；staged_/出参写→SyncRunResult 机械重排；注入点替换。逐块核：无条件翻转/默认值变/文案改/请求序变。staged 中途写→run 后锁内一次性搬：唯一读者 harvest（join 后）grep 证实，可观察等价；dbImported 原子位同理。线程 lambda 次序/onStop join/detach/harvest 逐字未动。mergeSubResult「先到先得」≡ 旧 staged 累计写（各字段单链至多一个写者）。
**A2 注入面**：缝只加不改，全空回退生产默认，engine 零 #ifdef/测试分支/调试残留；api 工厂走 setSender 实例缝。
**A3 钉点**：失败节流记时点行逐字未动（新 892/922=旧 1588/1618）；LocalDeleted 双分支、409 收集中止零写远端、branch 全链显式、RemoteModified→Put（未触碰的 logic 层）均 engine+测试双核通过。

**B 测试**：21 用例全行为级（假发送器→真 Api→真 engine→真 store/临时目录），断言落请求序/store/落盘/进度序。**变异实证**：engine.cpp:408 conflict=true→false → --filter gsync 红 1（push_conflict_diverged_aborts，exit 1）→ 还原 → gsync 21/21 绿、树干净。恒真断言未发现（chmod 失效会红不假绿）。

**C 卫生**：settings 2209→1513；编排残留 grep 零命中（仅测试缝/exit hook 属预期）；模块隔离守卫实跑过；分支仅 7 文件无夹带；674655a 仅测试侧 8+/6- 属实（与 dev 0115ddc 修同一处，合并裁决归主会话）。

**D 实跑**：build-test-run.sh --no-run exit 0，Total 1436 / Passed 1430 / Failed 0（差 6=平台 SKIP）。

**Findings**：F1（info）engine.cpp:287/569 db/store 指针捕获由子步骤时提前到线程启动时——单例/模块生命周期成员，无实际差，无需修。F2（info）674655a 与 dev 0115ddc 同位冲突待取舍。

**裁决：放行。**
