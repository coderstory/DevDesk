# 16: agent_ui 停止钮取消流程 macOS CI 失败诊断

**What to build:** 修掉 `agent_ui_stop_button_cancel_flow`（运行中渲染停止钮 → 点击置 cancel → 钮切「停止中」→ run 收尾 cancelled）在 macOS runner 上的失败。本地 Windows 全绿；macOS runner 上 81-139ms 失败、**零断言行零异常行零 lastFailNote**（7f053a2b 仪表化后依旧）——与 15 号票同款「静默 false」形态，但本测试体量更大（真线程 + 阻塞桩 + UI 轮询），失败点同样不在任何 CHECK 上。

**已知事实：**
- 测试已有 MinGW 线程创建可靠性守卫（`agentThreadCreationReliableUnderMinGW()` → SKIP）；macOS 不走该守卫直接跑，**很可能同为 std::thread 创建/调度类环境差异**（守卫注释描述的失败链：线程创建失败 → ActiveRun 被摘除 → 停止钮永不渲染 → REQUIRE_MESSAGE 失败）。
- 81-139ms 失败：大概率死在前 100×10ms 的「停止钮渲染轮询」窗口——ActiveRun 未注册（线程没起来）或 compose 依赖的 store 状态未就绪。
- macOS runner 的 agent_e2e_probe 也因缺 LLM 凭据失败（已知环境态），但本测试用的是阻塞桩不依赖真 LLM。

**排查建议：**
1. 先把「停止钮渲染轮询失败」时的观测面数据打进失败消息（hasActiveRun、任务状态、线程创建是否抛过），跑一次 CI 即可定位。
2. 评估把 `agentThreadCreationReliableUnderMinGW` 守卫推广为跨平台「线程创建可靠性探测」（macOS runner 也适用），不可靠即 SKIP——与 MinGW 同口径：工具链环境差异不做功能性硬门禁。
3. 若根因是真实调度 bug（非环境），转修复票。

**Blocked by:** 无。

**Status:** open（2026-10-26 整改会话移交）

- [ ] 拿到 macOS runner 失败点数据（观测面快照）
- [ ] 根因定位（环境差异 or 真实 bug）
- [ ] 修复或环境门控后，macOS CI agent_ui PASS
