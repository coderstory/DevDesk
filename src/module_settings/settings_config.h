// src/module_settings/settings_config.h
// P13 设置页：配置模型 + 持久化（spec §8.5 / 任务 T11）
//
// 设计决策：
//   - Config 结构放 module 内部（任务允许"core 或 module 内部皆可，选最小改动"），
//     core 不感知具体设置项
//   - 持久化：core::paths::configFile() + platform::json（yyjson 薄封装）
//   - SettingsStore 的文件路径由构造注入，便于单测用临时文件做读写往返
//
// 字段说明：
//   - theme: "dark" | "light"（默认 dark = 方案 B Dark）
//   - fontScalePercent: 字号缩放百分比，load 时钳制到
//     [kMinFontScalePercent, kMaxFontScalePercent]。
//     限制（如实记录）：core::theme 的 SizeTokens 不支持运行时缩放
//     （setDark/setLight 用硬编码 token 重建全局，无缩放入口；core/theme
//     不在本任务文件范围），该值当前仅作用于设置页自身可控的字号，
//     持久化先行，全局生效需后续在 core::theme 加 API + 清理各模块硬编码字号
//   - logRetentionDays: 调用日志保留天数，0 = 永久保留
//     集成点：与 w3-t10-home 并行分支的"30 天日志清理任务"联动——
//     T10 的清理逻辑应读取本字段（config.json 的 logRetentionDays），
//     接线留给集成阶段
#pragma once

#include <filesystem>

#include "core/result.h"

namespace module::settings {

// 字号缩放范围（%）
constexpr int kMinFontScalePercent = 85;
constexpr int kMaxFontScalePercent = 130;
// 日志保留天数上限（0 = 永久）
constexpr int kMaxLogRetentionDays = 3650;

struct SettingsConfig {
    bool darkTheme = true;         // 默认深色（方案 B Dark）
    // W9 默认 100：EUI-NEO 0.7.0 已修字号 CSS em 语义（fix/font-em-size，
    // scalable 字体 fontSize 直接作 em 尺寸），此前 hhea 缩水 ~0.75 不复存在。
    // 存量用户值通过换键 fontScalePercentV3 一次性重置回 100（旧值都是
    // 缩水语义下选的，修正后失去意义）。
    int fontScalePercent = 100;    // 100 = 设计字号原样（正文 15px token）
    int logRetentionDays = 30;     // 0 = 永久保留
    // Claude 代理随应用启动（provider-config Phase B 硬性需求 1：默认 **不**
    // 启动，代理由用户手动启动；false 时 main.cpp 只创建 ClaudeService
    // 注册全局单例，不拉起 127.0.0.1:8888）
    bool claudeProxyAutoStart = false;
};

// config.json 读写（不抛异常；文件不存在/损坏一律回退默认值）
class SettingsStore {
public:
    // file：配置文件完整路径（生产传 core::paths::configFile()，测试传临时文件）
    explicit SettingsStore(std::filesystem::path file);

    // 读取配置；文件不存在 / 解析失败 / 字段缺失 → 对应默认值
    SettingsConfig load() const;

    // 序列化写入 JSON（pretty）；失败返回 fail Result
    core::Result<void> save(const SettingsConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）
    static SettingsConfig sanitized(const SettingsConfig& cfg);

private:
    std::filesystem::path file_;
};

} // namespace module::settings
