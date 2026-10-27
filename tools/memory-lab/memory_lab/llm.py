"""Anthropic Messages API 客户端（纯标准库 urllib，不引入 requests）。

设计约定：
- 网关为内网直连，**必须绕过系统代理**（ProxyHandler({})）。
- 模型名尾部的 ``[1M]`` / ``[1m]`` 上下文后缀在构造时剥掉。
- 429 / 5xx / 网络超时按 0.5s、1s、2s 指数退避重试；其它 4xx 立即抛
  :class:`LlmError`，错误信息里不回显 token。
- 只取响应里的 text block；thinking block 不计入返回文本。
"""

from __future__ import annotations

import json
import re
import threading
import time
import urllib.error
import urllib.request

_MODEL_SUFFIX_RE = re.compile(r"\s*\[1[mM]\]\s*$")
_RETRY_DELAYS = (0.5, 1.0, 2.0)
_ANTHROPIC_VERSION = "2023-06-01"


class LlmError(RuntimeError):
    """LLM 调用失败。``status`` 为 HTTP 状态码（网络错误时为 None）。"""

    def __init__(self, message: str, status: int | None = None, body: str = ""):
        super().__init__(message)
        self.status = status
        self.body = body


def strip_model_suffix(model: str) -> str:
    """剥掉模型名尾部的 ``[1M]``/``[1m]`` 后缀。"""
    return _MODEL_SUFFIX_RE.sub("", model or "").strip()


def _balanced_object(text: str, start: int) -> str | None:
    """从 text[start]（必须是 '{'）起取第一个平衡的大括号子串。"""
    depth = 0
    in_string = False
    escaped = False
    for i in range(start, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    return None


def extract_json(text: str) -> dict | None:
    """从模型输出里提取第一个可解析的 JSON 对象。

    容忍 ```json 围栏、前后解释性废话；只返回 dict（数组/标量返回 None）。
    """
    if not isinstance(text, str) or not text.strip():
        return None

    stripped = text.strip()
    # 1) 整段就是 JSON
    try:
        data = json.loads(stripped)
        if isinstance(data, dict):
            return data
    except ValueError:
        pass

    # 2) 围栏代码块
    for block in re.findall(r"```(?:json|JSON)?\s*(.*?)```", text, re.S):
        candidate = block.strip()
        if not candidate:
            continue
        try:
            data = json.loads(candidate)
        except ValueError:
            continue
        if isinstance(data, dict):
            return data

    # 3) 扫描每个 '{'，取第一个能解析的平衡对象
    for index, ch in enumerate(text):
        if ch != "{":
            continue
        candidate = _balanced_object(text, index)
        if candidate is None:
            continue
        try:
            data = json.loads(candidate)
        except ValueError:
            continue
        if isinstance(data, dict):
            return data
    return None


class LlmClient:
    """Anthropic Messages 格式的最小客户端。"""

    def __init__(
        self,
        base_url: str,
        token: str,
        model: str,
        timeout: float = 90,
        retries: int = 3,
        temperature: float = 0.2,
        max_concurrency: int = 2,
    ):
        self.base_url = (base_url or "").rstrip("/")
        self.token = token or ""
        self.model = strip_model_suffix(model)
        self.timeout = float(timeout)
        self.retries = max(0, int(retries))
        self.temperature = float(temperature)
        self.max_concurrency = max(1, int(max_concurrency))
        self._semaphore = threading.BoundedSemaphore(self.max_concurrency)
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    # ------------------------------------------------------------------
    # 内部工具
    # ------------------------------------------------------------------
    def _endpoint(self) -> str:
        if self.base_url.endswith("/v1"):
            return self.base_url + "/messages"
        return self.base_url + "/v1/messages"

    def _redact(self, text: str, limit: int = 300) -> str:
        value = str(text or "")
        if self.token:
            value = value.replace(self.token, "***")
        return value[:limit]

    def _payload(self, messages, max_tokens, temperature) -> dict:
        system_parts: list[str] = []
        chat: list[dict] = []
        for message in messages:
            if not isinstance(message, dict):
                continue
            role = message.get("role")
            content = message.get("content", "")
            if role == "system":
                if isinstance(content, str):
                    system_parts.append(content)
                else:
                    system_parts.append(json.dumps(content, ensure_ascii=False))
            else:
                chat.append({"role": role or "user", "content": content})
        payload: dict = {
            "model": self.model,
            "max_tokens": int(max_tokens),
            "messages": chat,
        }
        if system_parts:
            payload["system"] = "\n\n".join(system_parts)
        if temperature is not None:
            payload["temperature"] = float(temperature)
        return payload

    def _request(self, payload: dict) -> tuple[str, dict]:
        request = urllib.request.Request(
            self._endpoint(),
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={
                "Authorization": "Bearer " + self.token,
                "Content-Type": "application/json",
                "Accept": "application/json",
                "anthropic-version": _ANTHROPIC_VERSION,
            },
            method="POST",
        )
        try:
            with self._opener.open(request, timeout=self.timeout) as response:
                status = int(response.status)
                body = response.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            assert exc.code is not None
            body = ""
            try:
                body = exc.read().decode("utf-8", errors="replace")
            except Exception:  # noqa: BLE001 - 读错误响应体失败不影响主错误
                body = ""
            detail = self._redact(body)
            raise LlmError(
                f"LLM HTTP {exc.code}: {detail}" if detail else f"LLM HTTP {exc.code}",
                status=int(exc.code),
                body=detail,
            ) from exc
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            reason = getattr(exc, "reason", exc)
            raise LlmError(f"LLM 网络错误: {self._redact(reason)}") from exc

        if status != 200:
            detail = self._redact(body)
            raise LlmError(
                f"LLM HTTP {status}: {detail}" if detail else f"LLM HTTP {status}",
                status=status,
                body=detail,
            )
        try:
            data = json.loads(body)
        except ValueError as exc:
            raise LlmError(
                f"LLM 响应不是合法 JSON: {self._redact(body)}", status=status
            ) from exc
        if not isinstance(data, dict):
            raise LlmError("LLM 响应不是 JSON 对象", status=status)

        text_parts: list[str] = []
        for block in data.get("content") or []:
            if isinstance(block, dict) and block.get("type") == "text":
                value = block.get("text")
                if isinstance(value, str):
                    text_parts.append(value)
        usage_raw = data.get("usage") if isinstance(data.get("usage"), dict) else {}
        usage = {
            "input_tokens": int(usage_raw.get("input_tokens") or 0),
            "output_tokens": int(usage_raw.get("output_tokens") or 0),
        }
        return "".join(text_parts), usage

    # ------------------------------------------------------------------
    # 公开接口
    # ------------------------------------------------------------------
    def complete(
        self,
        messages: list[dict],
        max_tokens: int = 1500,
        temperature: float | None = None,
    ) -> tuple[str, dict]:
        """调用 Messages API，返回 ``(文本, usage)``。

        429/5xx/超时最多重试 ``retries`` 次（退避 0.5s/1s/2s）；
        其它 4xx 直接抛 :class:`LlmError`。
        """
        payload = self._payload(
            messages,
            max_tokens,
            self.temperature if temperature is None else temperature,
        )
        attempts = self.retries + 1
        last_error: LlmError | None = None
        for attempt in range(attempts):
            try:
                with self._semaphore:
                    return self._request(payload)
            except LlmError as exc:
                last_error = exc
                retryable = exc.status is None or exc.status == 429 or exc.status >= 500
                if not retryable or attempt >= attempts - 1:
                    raise
                delay = _RETRY_DELAYS[min(attempt, len(_RETRY_DELAYS) - 1)]
                time.sleep(delay)
        assert last_error is not None  # 循环必然以 return 或 raise 结束
        raise last_error

    def __repr__(self) -> str:  # 避免 token 出现在日志/repr 里
        return (
            f"LlmClient(base_url={self.base_url!r}, model={self.model!r}, "
            f"timeout={self.timeout!r})"
        )