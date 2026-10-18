// 闹钟时间文本（YYYY-MM-DD HH:MM）↔ Unix 秒的双向换算 + ASCII trim 小工具。
//
// **行为等价搬迁**：2026-10-18 随「自然语言生成任务草稿」（task_draft.h）把这组函数
// 从 agent_module.cpp 的匿名命名空间原样搬到这里，判定规则、报错文案、边界值逐字未改
// ——闹钟 UI（新建/编辑回填/预览/保存）与历史任务的口径不变。搬出来的唯一目的是让
// 草稿解析层（task_draft.cpp）与 AgentModule 共用**同一份**闹钟校验，不允许出现
// 「模型草稿说合法、UI 说非法」的双真源。
//
// 本文件全部是**纯函数**：无 IO、不改全局状态、不读真时钟 ——
//   - **时钟由调用方注入**：defaultOnceAtText(now) 收 now；本文件任何一处都不调
//     core::time::nowUnix()。想钉住「某个时刻」的用例自己传值。
//   - 唯一的平台接触面是 core::time::localTime（Windows localtime_s /
//     POSIX localtime_r），业务代码里没有 `#ifdef _WIN32` / `__APPLE__`。
//
// **「本地时刻」语义**：所有 epoch ↔ 文本换算都按**运行机器的本地时区**，文本里不含
// 时区/偏移字段。闹钟语义就是「我本地墙钟的 8 点」，跨时区迁移不在本层处理。
//
// **同名副本不合并**：app_notify_tool.cpp 有一份刻意重复的私有 trimCopy（模块间互不
// include，见 scripts/check-module-isolation.sh 的断言 C），那一份**刻意不动**。
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "core/logging.h"
#include "core/result.h"
#include "core/time.h"
#include "module_agent/agent_store.h"     // AgentTaskRow（onceTextOf 的入参）
#include "module_agent/trigger_config.h"  // decodeTriggerConfig / kOnceAtUpperBound

namespace module::agent {

// ASCII 空白裁剪：只认 ' ' / '\t' / '\r' / '\n'，与 app_notify_tool.cpp 的私有副本
// 同口径；全角空格不算空白（中文文案里出现的 U+3000 不该被静默吃掉）。
inline std::string trimCopy(const std::string& s) {
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t b = 0, e = s.size();
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// s[pos .. pos+count) 视作 ASCII 数字取值；调用方已保证这些位全是数字
inline int digitsAt(const std::string& s, size_t pos, size_t count) {
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        value = value * 10 + (s[pos + i] - '0');
    }
    return value;
}

// 严格解析闹钟时间「YYYY-MM-DD HH:MM」（本地时区，24 小时制）。
// 校验分三层：字面形状 → 字段范围 → mktime 归一化后必须与输入逐字段一致。
// 第三层专门拦「2026-02-30」和夏令时跳变不存在的本地时刻（mktime 会把它们悄悄
// 归一化到别的日期/时刻；只看 epoch 范围会把脏输入当合法值存进库）。
// 不抛异常：格式/范围/不存在/越界统一返回 Result fail。
inline core::Result<int64_t> parseOnceAtLocalText(const std::string& text) {
    const std::string s = trimCopy(text);
    if (s.size() != 16 || s[4] != '-' || s[7] != '-' || s[10] != ' ' ||
        s[13] != ':') {
        return core::Result<int64_t>::fail(
            core::ErrorCode::ValidationError,
            "闹钟时间格式应为 YYYY-MM-DD HH:MM");
    }
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15}) {
        if (!isAsciiDigit(s[i])) {
            return core::Result<int64_t>::fail(
                core::ErrorCode::ValidationError,
                "闹钟时间格式应为 YYYY-MM-DD HH:MM");
        }
    }
    const int year = digitsAt(s, 0, 4);
    const int month = digitsAt(s, 5, 2);
    const int day = digitsAt(s, 8, 2);
    const int hour = digitsAt(s, 11, 2);
    const int minute = digitsAt(s, 14, 2);
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return core::Result<int64_t>::fail(
            core::ErrorCode::ValidationError,
            "闹钟时间的年月日或时分超出范围");
    }
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;   // 让 mktime 按目标日期判定夏令时
    const std::time_t t = std::mktime(&local);
    if (t == static_cast<std::time_t>(-1)) {
        return core::Result<int64_t>::fail(
            core::ErrorCode::ValidationError, "闹钟时间无效（无法转换为本地时刻）");
    }
    if (local.tm_year + 1900 != year || local.tm_mon + 1 != month ||
        local.tm_mday != day || local.tm_hour != hour ||
        local.tm_min != minute) {
        return core::Result<int64_t>::fail(
            core::ErrorCode::ValidationError,
            "闹钟时间在本地时区不存在（日期无效或夏令时跳变）");
    }
    const int64_t epoch = static_cast<int64_t>(t);
    if (epoch < 1 || epoch > kOnceAtUpperBound) {
        return core::Result<int64_t>::fail(
            core::ErrorCode::ValidationError,
            "闹钟时间超出支持范围（1970-01-01 至 2100-01-01）");
    }
    return core::Result<int64_t>::ok(epoch);
}

// epoch → 本地「YYYY-MM-DD HH:MM」；走 core::time::localTime（Windows localtime_s /
// POSIX localtime_r），不在业务代码里写平台分支
inline std::string formatOnceAtText(int64_t epoch) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(epoch), &local);
    char buf[32];
    const std::size_t n =
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return n == 0 ? std::string() : std::string(buf, n);
}

// 新建闹钟缺省：now + 1 小时，分钟精度（秒在文本里本就不显示，保存时也按 :00 解析）。
// now 由调用方注入 —— 本文件不读真时钟，测试可钉住任意基准时刻。
inline std::string defaultOnceAtText(int64_t now) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(now) + 3600, &local);
    char buf[32];
    const std::size_t n =
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return n == 0 ? std::string() : std::string(buf, n);
}

// once_at 的展示文本（详情摘要）；解析失败返回空串并留痕——非空配置必须显形
inline std::string onceTextOf(const AgentTaskRow& task) {
    auto cfg = decodeTriggerConfig(task.triggerConfig);
    if (cfg.isOk() && cfg.value().onceAt) {
        return formatOnceAtText(*cfg.value().onceAt);
    }
    if (!task.triggerConfig.empty()) {
        LOG_WARN("Agent: 任务 " + std::to_string(task.id) +
                 " 的 trigger_config 无法取出 once_at（详情摘要留空）：" +
                 (cfg.isFail() ? cfg.errorMessage()
                               : std::string("缺少 once_at 字段")));
    }
    return {};
}

} // namespace module::agent