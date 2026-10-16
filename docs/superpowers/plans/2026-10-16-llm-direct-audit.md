# 审计报告：worktree-agent-llm-direct（智能体 LLM 直连解耦）

基线 `589d8f5...b0e5d02`（3 提交）。实跑于 `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-llm-direct`，只读 + 一次已还原变异。

## A. 正确性
1. **解耦完整性 PASS**：全仓 grep `resolveModelSlot` 零活代码（仅 direct_llm_client.cpp:79 注释提及旧口径）；`llm_client.{h,cpp}` 已删；module_agent 零 include claude_proxy 头。agent_module.cpp:184 的 `PageId::ClaudeProxy` 是 core 枚举页导航（去 Claude 页引导），非服务依赖，合规。
2. **配置源 PASS**：main.cpp `ClaudeDirectProfileSource` 六字段全映射——baseUrl/authField/modelsConfigJson/model/dialectOverride 直取 Profile，key 经 `loadApiKey`→`api_key_encrypted` 列 + unprotect（profile_repository.cpp:706），不碰 settings.json。
3. **槽位解析 PASS**：resolveSlotModel 与已删代理侧实现逐行为同口径（def≡default、default 回落 profile.model、未配置/未知键/畸形 JSON→失败或回落）；kSlotKeys 由 model_slots.h X 宏派生，无双源。
4. **前缀一致 PASS（info）**：kConfigErrorPrefix＝「LLM 配置不可用：」(direct_llm_client.cpp:30)，needsClaudePageHint 匹配无冒号短前缀 (agent_module.cpp:58)——匹配面只会多配不会漏；网络/401→HttpError 不带前缀，负例已钉。
5. **契约 PASS**：LlmMessage/LlmResponse 未动，tool_use/tool_result 经 contentJson 原文往返；system 空不发；全程 Result 不抛。
6. **请求组装 PASS**：stripTrailingSlash+/v1/messages；authField 三态与 upstream_dialect.applyHeaders 同语义（未知/空→双发）；anthropic-version、UA 逐字同代理；max_tokens 4096 同旧。超时 60s→120s（有注释依据，合理）。方言复刻（hostOf/isOpencodeHost/stripOneM）与 upstream_dialect.cpp 逐行一致。
7. **线程 PASS**：send 仍在 run 线程 (agent_module.cpp:534)；适配器每次重取 `ClaudeService::global()`，无悬空；DB 访问模式与旧路径相同；注册与 IClaudeProxyService 同块成对 (main.cpp:351)。

findings：
- 低：直连无 failover（代理路径 fallback_profile_id 单跳重试未随迁）——CHANGELOG 已自报边界①，需求未要求。
- info：方言判定两处复刻需双改——注释+CHANGELOG 已写明（边界②）。

## B. 用例真实性
8. **PASS**：七条全经 HttpFn 真缝组真请求（URL/头/body 字节级断言）。硬判据 `direct_llm_client_sends_without_any_proxy_service` (test_direct_llm_client.cpp:289-307) 逐行核得：REQUIRE `ClaudeService::global()==nullptr` + REQUIRE `requireService<IClaudeProxyService>` 失败 + send 仍成功且 URL 正确。变异验证：`/v1/messages`→`/v1/message` 恰红此条+主链条（5/7），还原后复绿——非恒真。
- 低：authField 未知/空串→双发缺正例（三态只测字面值）。
9. **无真空**：旧 3 条中 preflight 随功能消亡（本票就是删预检），余两条由新用例承接。
- info：test_agent_loop.cpp:229、:715 注释仍指已删 llm_client——注释漂移。

## C. 卫生
10. **PASS**：claude_service.{h,cpp} 删减恰为被删接口方法的 decl+impl（必然伴随，非夹带）；无调试残留、无密钥硬编码（sk-test-key 为替身）；反重构注释（声明序、方言双处同步）在位。+349 行新客户端承接原散在代理侧的槽位解析+方言+鉴权三块职责，行数比合理。
11. **PASS**：两处 CMake 均单词级替换；全仓无编译残留引用。

## D. 实跑
- `--filter agent`：**Total 93 / Passed 93 / Failed 0**（注：该 filter 不覆盖 direct_llm_client_* 七条名字，已补跑）
- `--filter direct_llm`：**Total 7 / Passed 7 / Failed 0**
- 变异 5/7 红（恰硬判据两条）→ checkout 还原 → status 干净 → 7/7 复绿。
- 均走 build-test-run.sh，加 `--no-run`（避免脚本杀/起 DevDesk 进程波及在飞会话），构建测试完整未截断。

## 总裁决：**放行**

低危（无 failover 已自报 / authField 缺省态无正例）与 info（注释漂移、方言双源）均不挡门，可随后续批清理。
