// LLM 工具执行器公共脚手架（need:380 批C）：各 module_* 工具文件的重复副本收敛。
// 五件套语义来自 todo_tools/agent_runs_tool 等原副本，逐字段等价，不改行为。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/tool.h"
#include "platform/json.h"

namespace platform::tool_kit {

// 工具级错误：Result ok + isError + {"error":"<中文>"}（错误回喂 LLM）
[[nodiscard]] core::Result<core::ToolResult> toolError(const std::string& message);

// 参数解析：空串视为 {}（LLM 的 input 可能为空对象省略）；其余必须解析成 object
[[nodiscard]] core::Result<platform::json::JsonObject> parseArgs(const core::ToolArgs& args);

// 可选字符串字段：缺失 / 非字符串回空串
[[nodiscard]] std::string strField(const platform::json::JsonObject& obj, const char* key);

// 可选整数字段：缺失 / 非数字回 nullopt
[[nodiscard]] std::optional<int64_t> intField(const platform::json::JsonObject& obj, const char* key);

// JSON 数组组装：platform::json 无通用数组追加 API，逐元素 stringify 后文本
// 拼接，再 parse 校验（todo_tools 同款口径）
[[nodiscard]] core::Result<std::string> arrayOfJsonTexts(const std::vector<std::string>& items);

}  // namespace platform::tool_kit
