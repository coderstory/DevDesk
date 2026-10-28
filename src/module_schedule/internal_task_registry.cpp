#include "module_schedule/internal_task_registry.h"

namespace module::schedule {

core::Result<void> InternalTaskRegistry::registerTask(const std::string& key,
                                                      InternalTaskFn fn) {
    if (key.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "InternalTaskRegistry：任务 key 为空");
    }
    if (!fn) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "InternalTaskRegistry：任务回调为空（key=" +
                                            key + "）");
    }
    const auto it = tasks_.find(key);
    if (it != tasks_.end()) {
        return core::Result<void>::fail(
            core::ErrorCode::AlreadyExists,
            "InternalTaskRegistry：任务重复注册被拒绝（key=" + key + "）");
    }
    tasks_.emplace(key, std::move(fn));
    return core::Result<void>::ok();
}

const InternalTaskFn* InternalTaskRegistry::find(const std::string& key) const {
    const auto it = tasks_.find(key);
    return it == tasks_.end() ? nullptr : &it->second;
}

std::vector<std::string> InternalTaskRegistry::keys() const {
    std::vector<std::string> out;
    out.reserve(tasks_.size());
    for (const auto& [key, fn] : tasks_) {
        (void)fn;
        out.push_back(key);
    }
    return out;
}

void InternalTaskRegistry::resetForTest() { tasks_.clear(); }

} // namespace module::schedule
