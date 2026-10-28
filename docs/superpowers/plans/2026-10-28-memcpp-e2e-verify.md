# memcpp 端到端联调验收报告（S3+S4 遗留 live 对账补齐）

- 日期：2026-10-28；执行者：功能实证子代理（独立于实现/测试）
- 环境：Windows 10.0.19045 x64；worktree `D:/wt-memcpp`（feat/memcpp，基线 e4795b71）；
  独立数据根 `C:/Users/coderstory/AppData/Local/Temp/devdesk_memcpp_e2e`（venv 复用 S2 产物拷贝，
  telemem 1.10.1 + anthropic 补包已装）；`DEVDESK_CLAUDE_CONFIG_DIR` 指向伪造 settings.json
  （哑 key，`sk-devdesk-e2e-dummy-key-not-real-000000`），全程未触碰用户真实数据根与 ~/.claude
- 引擎形态：llama-server（bge-small-zh-v1.5-q8_0，回环 11435）+ telemem MCP（回环 8421），
  `GET /health` 与 `POST /mcp initialize` 探活均 200；LLM 真调未做（infer=false 直存，哑 key 即可）

## 总结论：可交付（S3+S4 遗留对账全过；发现并修复 5 处真实缺陷，全部钉单测）

## 八项逐条

1. **造环境**：过 —— 独立数据根 + `claude/projects/` 三个伪造会话 jsonl（2 干净技术会话 +
   1 含假手机号 13800138000 / 假密钥 sk-e2eFakeKey… 的会话）。
2. **引擎起**：过 —— venv 内 `import telemem`=1.10.1；llama-server health ok（11435）；
   telemem initialize 200（8421）；哑 key 仅经环境变量注入子进程。
3. **采集→脱敏→提案**：过 —— 真 Collector.pollOnce 出 3 段（4KB 内合段，6/4/3 事件）；
   敏感段被拦截（sanitize hits=2，类别 phone+secret，无原文泄漏，审计行
   `sanitize:applied hits=2 bytes=303`）；两干净段各生成 pending 提案
   （source=collect、evidence.sessionId 正确、createdAt 贴当前、正文零敏感原文）。
4. **审核→入库**：过 —— PanelState::acceptProposal 真引擎 add：检索 0→1 命中、faiss
   0B→4789B 落盘；reject 后检索恒 0（未入库）；撤销链：createUndoCard→accept→检索
   0 命中（引擎真删除）。telemem add 响应无 id（live 实证）——修复 addDirect 经
   get_memories 正文精确匹配回填 UUID 条目 id，撤销链由此闭合。
5. **hook 注入真跑**：过 —— `devdesk.exe memory hook`（真二进制）喂 UserPromptSubmit 事件：
   exit 0、stdout 恰为 `{"hookSpecificOutput":{"additionalContext":…,"hookEventName":
   "UserPromptSubmit"}}`、命中在库记忆、注入 2 条/82 字（≤5 条 ≤500 字）、带「来源:telemem」
   标注；ledger 注入账 0→2 行（inject_items=2 / inject_chars=82，预算环数据源闭环）；
   引擎停时复跑：**stdout 空 + exit 0**（fail-open 达成）。日期位显示「未知日期」——telemem
   search 融合段不携带 createdAt，属引擎侧形态边界（如实记录）。
6. **触发线评测真跑**：过 —— runFrozenEval 对真引擎：15/15 查询前 5 命中，
   **Recall@5 = 100% ≥ 85% 触发线**（本地 embedding，零 API 费）；评测后 27 条 doc
   经 delete 通道清出引擎（S7 映射修复后 mark-forgotten 清理真实生效）。
7. **live 对账（S3+S4 遗留）**：过（发现 5 缺陷全修）——
   - **会话缝**：telemem 1.10.1 streamable-http 除 initialize 外一律要求 Mcp-Session-Id
     头（缺失 400 -32600 "Missing session ID"，curl 实证）→ 旧 TelememHttpRetriever 全挂。
     修复：initialize 一次缓存会话 id，tools/call 全部回传，引擎重启致 404 自动重取一次。
   - **工具形参**：真实工具集 add/search/get_memories/get_memory/update/delete/
     delete_all/memory_history；update_memory required=[memory_id,text]——旧
     setMemoryForgotten 只发 metadata 必挂（isError=true，curl 实证）。
   - **遗忘映射**：打 forgotten 标记后 search 照回（检索无 metadata 过滤缝，curl 实证）
     →「移出自动想起」唯一真实生效通道 = delete_memory；修复映射 true→delete_memory；
     false（恢复）引擎无恢复缝→如实报 NotImplemented，由提案层按原卡证据正文重录入
     （createUndo 透传原卡 evidence）。
   - **id 形态**：引擎条目 id = UUID 串（非整型）——旧 accept 回填只认十进制数字串、
     UUID 被丢弃→撤销链 targetIds 恒空必挂；修复 targetIds 改 vector<string> 原样入列。
   - **search 融合段落逐条化**：search 实测唯一形态 content[0].text =
     `{"query":…,"memories":"<融合段落>"}`——旧解析把 JSON 信封原文当记忆正文注入；
     修复取 memories 正文按单条、空段落=无命中；评测命中判定补「gold 正文包含于融合段」
     等价口径（旧判定只认逐条 id/全文相等，对真引擎恒 0/15）。
8. **key 红线**：过 —— 数据根全部产物（telemem.log / llama-server.log / proposals.db /
   usage_ledger.db / 配置 yaml）grep 零 `sk-` 真值；proposals.db 无敏感原文
   （13800138000 / sk-e2eFakeKey 零命中）；哑 key 仅存在于伪造 settings.json 输入；
   引擎日志 0 命中；telemem-config.yaml `api_key: ""` 恒空。

## 全量单测
- 修复后 devdesk_tests 全量：**Total 1512 / Passed 1503 / Failed 0 / Skipped 9**
  （memory 过滤 41/41，含新增 2 条钉版用例：live 信封解析、UUID 回填+遗忘撤销重录入链）

## 修复提交
| commit | 内容 |
|---|---|
| 本次单提交 | gateway 会话缝 + 工具形态对账（session 头/UUID id/delete 映射/融合段解析）+ eval 融合段命中判定 + createUndo 证据透传 + memory_e2e_probe（无头全链探针，39 项断言）+ 2 条钉版单测 |

## 未验证（如实列出）
- 引擎 startAsync 全链（venv 检测/安装/拉起）：S2 已验（1331450c/90cf043f 等），本轮为
  避免端口冲突未重复拉起；数据链场景全部经等价真实进程（同参数手拉 llama+telemem）。
- hook 注入的日期来源：融合段不携带 createdAt（telemem 形态边界），「未知日期」如实展示；
  逐条 id/日期需 telemem 上游提供非融合检索形态。
- macOS/Linux 平台面；add infer=true 云端提炼链路（需真 LLM key，哑 key 不做）。
- 面板 UI 交互截图：S2 报告已含三视图截图，本轮无 UI 变更未重复截图。

## 证据留存
- 探针全量输出：`devdesk_memcpp_e2e/probe.log`（39 PASS / 0 FAIL）
- hook 真跑：`devdesk_memcpp_e2e/hook-out.json`（正常）、`hook-down-out.json`（0 字节）
- 会话 recon 原始响应：本报告 §7 各 curl 实证行（会话记录可溯）
- 引擎日志：`devdesk_memcpp_e2e/{telemem,llama-server}.log`
