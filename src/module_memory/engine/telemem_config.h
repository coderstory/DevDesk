// src/module_memory/engine/telemem_config.h
// TELEMEM_CONFIG 生成与密钥边界（纯逻辑，零平台宏；单测钉全部契约）。
// 密钥红线：LLM key 只存在于「运行时内存 → 子进程环境变量」一条通道，
// 绝不写入配置文件、数据库、日志；本单元全部输出（YAML/日志串）必须可断言无密钥。
#pragma once

#include <filesystem>
#include <string>

namespace module::memory::engine {

// ── 端点与模型定案（端点定案块 + 本批任务书；改动即契约变更，单测必挂）──
inline constexpr int kTelememPort = 8421;  // telemem MCP HTTP，只绑回环
inline constexpr int kEmbedServerPort = 11435;  // 本机 llama-server（OpenAI 兼容）
inline constexpr const char* kLlmProvider = "anthropic";
inline constexpr const char* kLlmBaseUrl = "https://open.bigmodel.cn/api/anthropic";
inline constexpr const char* kLlmModel = "glm-5.3-flash";
// embedding 模型名常量（维度在建索引前定死，换档即全量重嵌——不要轻动）
inline constexpr const char* kEmbedModel = "bge-small-zh-v1.5";
// LLM key 的注入通道名：拉起 telemem 时经该环境变量喂给子进程；配置文件里恒为空串
inline constexpr const char* kLlmKeyEnvName = "TELEMEM_LLM_KEY";

// 记忆数据根：<appDataDir>/memory（venv、faiss_db、history.db、模型文件都落这里，
// 随 DEVDESK_DATA_ROOT 隔离，测试用独立根）
std::filesystem::path memoryDataDir();

// 生成 telemem YAML 配置（纯函数、确定性输出；路径统一 forward-slash）。
// 契约：
// - llm.provider=anthropic + kLlmBaseUrl + kLlmModel；api_key 恒空串，
//   api_key_env 指向 kLlmKeyEnvName（telemem 侧的运行时注入口）。
// - embedder 指向本机 llama-server OpenAI 兼容端点（回环 + kEmbedServerPort + kEmbedModel）。
// - 向量库与历史库路径全部收敛进 dataDir，不写家目录、不改 telemem 默认配置。
std::string generateTelememConfigYaml(const std::filesystem::path& dataDir);

// 从 settings.json 文本提取智谱 LLM key（纯函数）。查找序：
// env.ANTHROPIC_AUTH_TOKEN → env.ANTHROPIC_API_KEY；占位符 "PROXY_MANAGED" 视为无 key
//（那是代理托管标记，不是凭据）。缺键/坏 JSON → 空串。返回值只允许注入子进程环境。
std::string extractLlmKeyFromSettingsJson(const std::string& settingsJsonText);

// 运行时读用户真实 settings.json（core::paths::claudeSettingsFile()）并取 key。
// 读取失败静默返回空串——密钥有无只反映在引擎能否提炼，不打日志（内容红线）。
std::string readLlmKeyFromClaudeSettings();

// 日志脱敏：text 中的 secret 原文替换为 "<redacted>"；secret 为空原样返回。
// 任何可能携带密钥的文本进日志前必须过这道（单测钉：密钥原文不得出现在输出）。
std::string redactSecretsForLog(const std::string& text, const std::string& secret);

} // namespace module::memory::engine
