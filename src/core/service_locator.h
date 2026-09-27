// src/core/service_locator.h
// 只读服务定位器（任务 D / R0，审计 D6「跨模块耦合消解」）
//
// 背景：设置页要用 Claude 代理的运行时状态，但 module_* 之间不能互相依赖。
// 解耦前 settings_module.cpp 直接 include 兄弟模块的头并直呼其单例
//（ClaudeService::global()）——编译期就绑了兄弟模块的头，且「服务是否已装配」
// 对消费方只剩一个裸空指针可判断。本定位器把「消费方 → 提供方」的接触面收成
// 两步：
//   提供方（装配层 main.cpp，或测试桩）在模块 onStart 之前
//     ServiceLocator::set<T>(provider)；
//   消费方（module_*）在需要时 ServiceLocator::requireService<T>()。
//
// 边界（守模块隔离，CLAUDE.md「架构要点」）：
//   - 本头只依赖 core/result.h，不 include 任何 module_* / platform / EUI-NEO
//     头；T 是接口类型（抽象基类），由 core 或消费方定义；
//   - **只读**：定位器只做「按类型查找」，不创建、不持有所有权、不管理生命
//     周期——谁注册谁负责注销（生产 = main.cpp 的常驻对象，测试 = ScopedService
//     作用域）；
//   - 缺失语义：requireService<T>() 返回 fail（**不**返回裸空指针），强制调用
//     方写出「服务不可用」时的降级路径；把「没有」当正常状态的调用方在拿到
//     fail 后就地转成自己的缺省语义（见 settings_module.cpp 的 claudeProxy()）。
//
// 线程模型：注册 / 注销只发生在启动装配段与测试用例体内（单线程），运行期只读；
// 与 core::appRouter / core::appDatabase 同款「进程级单例槽」，故复位收在
// tests/test_main.cpp 的 TIN-07 全局清单里——清单调 resetForTest() 全清，
// 新增槽位（新接口类型）无需再逐类型登记（登记在 set<T> 首次调用时自动发生）。
#pragma once

#include "core/result.h"

#include <vector>

namespace core {

namespace detail {

// 每个接口类型一个槽：类模板的 inline 静态成员 = 每类型一份，跨 TU 唯一
template <typename T>
struct ServiceSlot {
    static inline T* instance = nullptr;
};

// 已用槽位的「清零回调」登记表（类型擦除）：槽位按类型实例化，要一次清空全部
// 槽位就得有一份类型清单——C++ 无反射，清单只能由 set<T>() 首次调用时登记
//（见 ensureSlotResettable）。供 ServiceLocator::resetForTest() 遍历清零。
inline std::vector<void (*)()>& slotResetters() {
    static std::vector<void (*)()> resetters;
    return resetters;
}

// 幂等登记：同一类型只登记一次（函数局部 static）。线程模型同 set——只从装配段
// / 测试用例体内（单线程）调用，故登记表不做同步。
template <typename T>
void ensureSlotResettable() {
    static const bool registered = [] {
        slotResetters().push_back([] { ServiceSlot<T>::instance = nullptr; });
        return true;
    }();
    (void)registered;
}

} // namespace detail

// 纯静态门面（不构造实例）
class ServiceLocator {
public:
    ServiceLocator() = delete;

    // 注册（传 nullptr = 注销）；返回旧值，便于调用方保存 / 还原
    template <typename T>
    static T* set(T* service) {
        detail::ensureSlotResettable<T>(); // 幂等：登记本类型的清零回调
        T* previous = detail::ServiceSlot<T>::instance;
        detail::ServiceSlot<T>::instance = service;
        return previous;
    }

    // 查找：未注册 → fail（ErrorCode::NotFound）。不返回裸空指针——缺失必须
    // 由调用方显式处理（这是与本项目其它「返回 Result」口一致的口径）
    template <typename T>
    static Result<T*> requireService() {
        T* service = detail::ServiceSlot<T>::instance;
        if (service == nullptr) {
            return Result<T*>::fail(
                ErrorCode::NotFound,
                "core::ServiceLocator：服务未注册（装配段漏注册，或提供方已被注销）");
        }
        return Result<T*>::ok(service);
    }

    // 清零**全部**已用槽位（出厂态 = 全部未注册）。测试 per-case 复位入口：
    // tests/test_main.cpp 的 TIN-07 进程级全局清单在每用例前调用（生产路径不调）。
    // 类型无关——含测试本地桩接口（如 test_app_assembly 的 IProbeService，
    // 声明在匿名空间、本文件无从点名），这正是逐类型 set<T>(nullptr) 白名单式
    // 清零覆盖不到的；新增接口类型也不必回来补登记。
    static void resetForTest() {
        for (void (*clear)() : detail::slotResetters()) {
            clear();
        }
    }
};

// RAII 注册：构造即注册、析构还原原值（含「原本未注册」→ 还原成 nullptr）。
// 测试桩与生产都可直接用；本项目不抛异常，析构一定会跑——用例体内 fail-fast
// 提前 return 也不会留下悬垂注册。
template <typename T>
class ScopedService {
public:
    explicit ScopedService(T* service)
        : previous_(ServiceLocator::set<T>(service)) {}
    ~ScopedService() { ServiceLocator::set<T>(previous_); }

    ScopedService(const ScopedService&) = delete;
    ScopedService& operator=(const ScopedService&) = delete;
    ScopedService(ScopedService&&) = delete;
    ScopedService& operator=(ScopedService&&) = delete;

private:
    T* previous_;
};

} // namespace core
