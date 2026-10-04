// 只读服务定位器：提供方在模块 onStart 前 set<T>(provider)，消费方按需 requireService<T>()。
// 边界：T 是接口类型；只读（不创建/不持有/不管生命周期，谁注册谁注销）；缺失返回 fail 而非
// 裸空指针，强制调用方写降级路径。线程模型：注册/注销只在启动装配段与测试体内（单线程），
// 运行期只读
#pragma once

#include "core/result.h"

#include <vector>

namespace core {

namespace detail {

// 每接口类型一个槽：inline 静态成员每类型一份，跨 TU 唯一
template <typename T>
struct ServiceSlot {
    static inline T* instance = nullptr;
};

// 清零回调登记表（类型擦除）：C++ 无反射，由 set<T>() 首次调用登记，供 resetForTest() 遍历清零
inline std::vector<void (*)()>& slotResetters() {
    static std::vector<void (*)()> resetters;
    return resetters;
}

// 幂等登记：同一类型只登记一次（函数局部 static）。只从装配段/测试体内单线程调用，故不做同步
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

    // 查找：未注册 → fail(NotFound)。不返回裸空指针——缺失必须由调用方显式处理
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

    // 清零全部已用槽位（测试 per-case 复位入口，生产不调）。类型无关——覆盖匿名空间的测试本地
    // 桩接口，新增接口类型也不必回来补登记
    static void resetForTest() {
        for (void (*clear)() : detail::slotResetters()) {
            clear();
        }
    }
};

// RAII 注册：构造即注册、析构还原原值。不抛异常故析构一定跑，用例 fail-fast 也不留悬垂注册
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
