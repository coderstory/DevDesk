# 任务：记忆类型前缀细则文档（记忆线 ③ 裁决成文）

mode: edit
从 dev 新建分支 wt-memory-prefix-doc，只许新建/修改 docs/ 下文档，禁碰 src/。

## 目标
把已裁决的「结构化记忆=正文前缀约定」落成实现细则文档，供需-3 开工直接照做。

## 内容要点（裁决已定，成文即可）
1. 四类前缀：`[踩坑|主题]` / `[决策|主题]` / `[契约|主题]` / `[环境|主题]`；踩坑注入置顶
2. 落点：入库走 addDirect 带前缀（telemem 第三方引擎不动 schema）；注入侧 assembleContext（src/module_memory/hook/inject.cpp）解析前缀按类型分组渲染「⚠ 踩坑 N 条 / 📌 决策 N 条」
3. 实现期核实项：telemem 的 infer 提炼 prompt 能否经 TELEMEM_CONFIG 定制（mem0 系支持 custom prompt）；能→云端提炼自动打前缀，不能→DevDesk 侧后处理补前缀；两条路都写清
4. 已知坑：telemem search 返回融合段落，逐条 metadata 拿不到——前缀方案不依赖 metadata，此点写明
5. 验收口径：直存带前缀记忆→hook 真跑注入→注入文本按类型分组（截图）；infer 真调一次→产物带前缀+控耗面板 token 非零

## 产出
`docs/superpowers/specs/2026-10-30-需-3-记忆前缀细则.md`，带日期戳与非目标段；参考 docs/RESEARCH-INDEX.md 既有调研勿重复联网。

## 回单
sha + 文件清单 + 说明。
