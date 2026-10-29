# 任务：需-11 项目卡（多业务系统结构化档案）实现

mode: edit
从 dev 新建分支 wt-project-card，worktree 内改码，构建/测试/提交由派单方收尾。

## 目标
每个业务系统登记一张结构化卡：仓库路径、启动命令、测试命令、部署方式、测试库地址、关键模块说明；MCP 工具 `project_info(name)` 供 Claude Code 查询。

## 范围（S/M，全走既有模式，不碰 core/框架）
1. 存储：复用自家 SQLite 基建模式建 `project_cards` 表（name 主键 + 字段如上六项 + created/updated）；老库 ALTER 迁移
2. UI：设置页或独立小页一个卡片列表（增删改查，样式对齐既有页面 token）
3. MCP：`project_info(name)` 薄封装 SQL（接线照 memory_mcp_bridge.cpp 既有样板）
4. 测试：按场景写用例（建卡→查卡→改卡→MCP 命中），注明防的事故；涉 UI 需 probe 或无头几何断言

## 非目标
- 不做卡片模板/导入导出；不做 GitLab 对接（路线图后置）；不动 EUI-NEO

## 验收标准（回单自证）
- F-01 建卡→重启→卡还在（持久化）
- F-02 MCP 调 project_info 命中中文名卡片
- F-03 缺 name 查询返回明确错误不崩
- F-04 定向用例全绿 + 先红后绿证据

## 回单
sha + 文件清单 + 说明三件套；报告极简风格。
