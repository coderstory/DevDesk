"""memory-lab 脱敏层：云端抽取外发前的最后清洗（feasibility §5 风险 8 前置实现）。

对外 API 只有一个::

    from sanitize import sanitize
    result = sanitize("任意待外发文本")
    result.text   # 脱敏后文本
    result.hits   # 命中清单（类别 / 位置 / 替换形式）

边界见同目录 README.md。
"""

from __future__ import annotations

from .core import (
    PLACEHOLDER_TEMPLATE,
    Hit,
    SanitizeResult,
    sanitize,
    scan,
)

__all__ = [
    "PLACEHOLDER_TEMPLATE",
    "Hit",
    "SanitizeResult",
    "sanitize",
    "scan",
]
