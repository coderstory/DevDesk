"""配置加载：路径、Claude Code 数据根、LLM 网关参数。

设计约束：
- 读 ``~/.claude/settings.json`` 失败（文件缺失、JSON 坏、字段缺失）一律
  静默降级为空串，绝不抛异常；调用方看到空串自己报错。
- 所有环境变量覆盖：
  MEMORY_LAB_HOME / MEMORY_LAB_CC_ROOT /
  MEMORY_LAB_LLM_BASE_URL / MEMORY_LAB_LLM_TOKEN / MEMORY_LAB_LLM_MODEL
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass
from pathlib import Path

DEFAULT_MODEL = "qianfan/deepseek-v4.1-flash"
# 网关清单里常见 qianfan/deepseek-v4.1-flash[1M] 这类上下文长度后缀，
# 模型名本身不带后缀，调用前统一剥掉（大小写不敏感）。
_MODEL_SUFFIX_RE = re.compile(r"\s*\[1[Mm]\]\s*$")


@dataclass
class Config:
    home: Path
    db_path: Path
    cc_projects_root: Path
    llm_base_url: str
    llm_token: str
    llm_model: str


def _default_home() -> Path:
    return Path.home() / ".devdesk" / "memory-lab"


def _default_cc_root() -> Path:
    return Path.home() / ".claude" / "projects"


def _read_settings_env() -> dict:
    """读取 ~/.claude/settings.json 的 env 段，任何失败返回 {}。"""
    settings_path = Path.home() / ".claude" / "settings.json"
    try:
        with open(settings_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    env = data.get("env")
    return env if isinstance(env, dict) else {}


def strip_model_suffix(model: str) -> str:
    """剥掉模型名尾部的 [1M]/[1m] 后缀。"""
    return _MODEL_SUFFIX_RE.sub("", model or "").strip()


def load_config(overrides: dict | None = None) -> Config:
    """构建 Config。

    overrides 可覆盖任意字段（home/db_path/cc_projects_root/llm_*），
    优先级：overrides > 环境变量 > settings.json > 内置默认值。
    """
    overrides = dict(overrides or {})
    settings_env = _read_settings_env()

    home = Path(
        overrides.get("home")
        or os.environ.get("MEMORY_LAB_HOME")
        or _default_home()
    ).expanduser()
    db_path = Path(overrides.get("db_path") or (home / "memory.db")).expanduser()
    cc_root = Path(
        overrides.get("cc_projects_root")
        or os.environ.get("MEMORY_LAB_CC_ROOT")
        or _default_cc_root()
    ).expanduser()

    base_url = (
        overrides.get("llm_base_url")
        or os.environ.get("MEMORY_LAB_LLM_BASE_URL")
        or settings_env.get("ANTHROPIC_BASE_URL")
        or ""
    )
    token = (
        overrides.get("llm_token")
        or os.environ.get("MEMORY_LAB_LLM_TOKEN")
        or settings_env.get("ANTHROPIC_AUTH_TOKEN")
        or os.environ.get("ANTHROPIC_AUTH_TOKEN")
        or ""
    )
    model = (
        overrides.get("llm_model")
        or os.environ.get("MEMORY_LAB_LLM_MODEL")
        or settings_env.get("ANTHROPIC_MODEL")
        or DEFAULT_MODEL
    )
    model = strip_model_suffix(str(model)) or DEFAULT_MODEL

    return Config(
        home=home,
        db_path=db_path,
        cc_projects_root=cc_root,
        llm_base_url=str(base_url),
        llm_token=str(token),
        llm_model=model,
    )