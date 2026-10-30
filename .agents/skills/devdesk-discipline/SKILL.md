---
name: devdesk-discipline
description: DevDesk 仓库开发纪律与测试入口速查：构建唯一入口、汇报三要素、先红后绿、复现优先、平台口径、已知跳过项
---

# DevDesk 开发纪律（接手必读）

适用：任何在 /Users/coderstory/dev/DevDesk 仓库干活的会话/Bot。

1. 构建与测试唯一入口：`bash scripts/build-test-run.sh`（定向 `--filter X`；全量只在收口跑；禁手敲 cmake）。UI probe：`(cd build && ctest -L probe)`。
2. 汇报三要素：干了啥/结果/需拍板啥，一次≤3 条；证据落文件报路径（测试清单卡 docs/plans/<slug>-tests.md，F-ID 制）；禁大块 markdown。
3. 质量判据：先红后绿（修复必须证明用例在修复前会红）；UI 改动必须真窗口 probe 几何断言 + 截图（--screenshot 进程内捕获，无需系统授权）才算完成；mock 绿不算数，收口必须真模型/真窗口实操。
4. BUG 铁律：复现优先——写不出能变红的复现命令禁止推理根因；只改致因代码。
5. 平台口径：先跨平台底座（core 缝/EUI-NEO/成熟库），底座真没有才开 *_win/_macos 平台文件且保持薄；业务代码禁 #ifdef。
6. 测试成本：研发阶段只跑受影响域定向用例；全量回归只在发布收口跑一次。
7. 已知设计内跳过（非失败）：wallpaper probe、DST Windows 侧、memory stress、Windows 交叉编译 5 条缺 curl 头。
8. EUI-NEO 用 coderstory fork（EUI-NEO/ 仓内 submodule），禁用上游 sudoevolve（编译失败）；上游只读永不 push。
9. 测试框架是自制迷你 doctest：仅 DOCTEST_CHECK/EQ/NE/TRUE/FALSE/REQUIRE*/CHECK_MESSAGE/SKIP，无 CHECK_LE；bool 用例体 `return true;` 收尾。
10. 验收陷阱：build/DevDesk.app 可能是陈旧 bundle，启动裸二进制 build/DevDesk；测试用独立 DEVDESK_DATA_ROOT，不碰 /Applications/DevDesk.app。
