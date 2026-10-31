// src/module_todo/todo_observability.h
// 归档折叠动画只读观察缝（池15，测试驱动设施；EUI-NEO aba0079 hitTestInteractiveAt 先例同构）。
// 只读三标量快照，无 setter、不动状态机；装配/卸载由 TodoModule onStart/onStop 主线程做（闸语义）。
#pragma once

#include <functional>
#include <utility>

namespace module::todo::obs {

struct ArchiveAnimSnapshot {
    bool animating = false;        // 动画在途（进行中）
    bool targetExpanded = false;   // 当前目标态（true = 展开方向）
    float progress = 0.0f;         // 当前进度 0..1（0=全折叠 1=全展开）
};

using Provider = std::function<ArchiveAnimSnapshot()>;

inline Provider& providerSlot() {
    static Provider slot = nullptr;
    return slot;
}

inline void setArchiveAnimProvider(Provider p) { providerSlot() = std::move(p); }

// 无提供方（模块未启动/已停止）→ 空闲默认快照，不崩溃。
inline ArchiveAnimSnapshot archiveAnimSnapshot() {
    const Provider& p = providerSlot();
    if (p) return p();
    return ArchiveAnimSnapshot{};
}

} // namespace module::todo::obs
