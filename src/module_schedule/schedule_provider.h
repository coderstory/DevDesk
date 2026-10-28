// src/module_schedule/schedule_provider.h
// 系统定时只读来源（§4.2 / 调研 §13）：各来源模块实现 ScheduleProvider，列表刷新
// 时与 DB 用户规则合并展示。系统项不落库、只读派生、执行归来源模块——来源注册
// executable=true 才由新引擎执行（v1 默认 false，避免上线提醒双弹）。
// origin_key 用稳定合成键（如 "release:task:<id>:precheck"），运行记录以
// origin='system:<module>' 写入 schedule_run（不建 FK）。
//
// 本波（S7a）只建接口与空注册表；release 等具体来源 S8 接。
#pragma once

#include "core/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule {

// 系统定时项视图（§4.2 视图字段 + 调研 §9 系统派生只读项）
struct ScheduleItemView {
    std::string source;   // 来源模块名（如 "release"）
    std::string key;      // 稳定合成键 origin_key
    std::string title;
    std::string summary;  // 规则摘要（「每两周 周二 09:00」口径，由来源模块生成）
    std::optional<int64_t> nextFireAt;  // Unix 秒；无值 = 无下次时刻（事件/常驻类）
    bool enabled = true;
    std::string sourceRoute;  // 点击跳转来源模块（PageId 名或 deeplink）
    bool executable = false;  // true = 由新引擎执行；false = 只读展示（默认）
};

// 系统定时来源：items() 返回只读视图。来源内部错误以空列表降级（列表页不得因
// 单个来源故障整页不可用）。
class ScheduleProvider {
public:
    virtual ~ScheduleProvider() = default;
    virtual std::vector<ScheduleItemView> items() const = 0;
};

// 系统定时来源注册表。本波为空注册表：模块持有实例，来源模块 S8 起在此注册。
// 注册只发生在装配段/测试体内（单线程），运行期只读——与 ServiceLocator 同口径，
// 不加锁。
class ScheduleProviderRegistry {
public:
    // 注册来源并接管所有权。重复注册同一来源（按 items() 里 source 名判重）→
    // fail(AlreadyExists) 且不替换。
    core::Result<void> registerProvider(std::unique_ptr<ScheduleProvider> provider);

    // 全部来源视图合并（保注册序）；未知/重复 source 的视图原样合并（不拦截）。
    std::vector<ScheduleItemView> allItems() const;

    // 测试缝：清空全部注册（生产不调用）
    void resetForTest();

private:
    std::vector<std::unique_ptr<ScheduleProvider>> providers_;
    std::vector<std::string> sources_;  // 与 providers_ 平行：判重依据
};

} // namespace module::schedule
