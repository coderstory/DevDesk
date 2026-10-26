# memory-lab（记忆实验台）

DevDesk 的本地记忆实验台。目标是在不碰 DevDesk C++ 代码的前提下，用 Python
验证「从 Claude Code 会话里抽取记忆 → 压缩 → 检索 → MCP 提供」整条算法链路，
为后续 memcpp 技术方案提供可量化参数。

## 目录

```
run.py                 命令行入口（ingest / stats / init）
memory_lab/config.py   配置（路径、Claude Code 数据根、LLM 网关）
memory_lab/models.py   数据模型（冻结契约，其他并行任务依赖）
memory_lab/db.py       SQLite schema 与读写
memory_lab/ingest.py   会话 jsonl 增量摄取
tests/                 unittest 用例
```

## 快速开始

```bash
python run.py ingest --project D--project-gui --limit 2   # 摄取真实会话
python run.py stats                                       # 查看统计
python run.py init                                        # 只建库
```

运行测试：

```bash
python -m unittest discover -s tests -v
```

## 配置

| 项 | 默认值 | 环境变量 |
|----|--------|----------|
| home | `~/.devdesk/memory-lab` | `MEMORY_LAB_HOME` |
| db_path | `<home>/memory.db` | 通过 `--db` 或 overrides |
| cc_projects_root | `~/.claude/projects` | `MEMORY_LAB_CC_ROOT` |
| llm_base_url | `~/.claude/settings.json` 的 `env.ANTHROPIC_BASE_URL` | `MEMORY_LAB_LLM_BASE_URL` |
| llm_token | settings 的 `env.ANTHROPIC_AUTH_TOKEN` | `MEMORY_LAB_LLM_TOKEN` |
| llm_model | `qianfan/deepseek-v4.1-flash`（自动剥掉尾部 `[1M]/[1m]`） | `MEMORY_LAB_LLM_MODEL` |

`settings.json` 缺失、JSON 损坏或字段不全时不会抛异常：对应字段返回空串，
由真正需要网关的调用方（压缩/嵌入）自行报错。

## 数据与索引

- **存储**：SQLite（WAL）。表结构见 `memory_lab/db.py` 的 `SCHEMA_SQL`；
  `init_schema()` 幂等。
- **全文索引**：`fts_observations` 使用 FTS5 external content 模式，
  `tokenize='trigram'` 在本机 SQLite 3.50.4 实测可用（中文子串检索必需）。
  INSERT/UPDATE/DELETE 由三个触发器同步，业务代码不需要手动维护索引。
- **列表编码**：`facts` / `files` 在库里是换行分隔文本，`concepts` 是逗号分隔文本；
  `db.py` 负责 list ↔ text 转换，对外只暴露 list。

## 摄取规则

- 输入：`~/.claude/projects/<项目目录>/*.jsonl`，`session_id` = 文件名去扩展名。
- 只抽 `type=user` / `type=assistant`：
  - user 文本块 → `kind=user`；`tool_result` → `kind=tool_result`（摘要截 2000 字）
  - assistant 的 text / thinking / tool_use → 对应 kind，tool_use 记录工具名与输入 JSON
  - 其余顶层 type（mode / permission-mode / attachment / system 等）忽略
- 单条事件文本上限 4000 字；坏 JSON 行计入 `skipped` 并继续。
- **增量**：`ingest_state` 记录每个文件的 offset/size/mtime。
  - 追加写：从上次 offset 续读
  - 文件变小或被同尺寸重写：从头重读并清掉该会话旧事件
  - 只消费以换行结尾的完整行，正在写入的半行留到下一次
- **幂等**：`events` 上有 `UNIQUE(session_id, seq)`，重复摄取不会产生重复事件。

## 已实测

- 单元测试：21 条全过（`unittest`）。
- 真实数据：`--project D--project-gui --limit 2` 首次摄取 1412 条事件、坏行 0；
  第二次同样的命令新增 0 条（幂等验证通过）。