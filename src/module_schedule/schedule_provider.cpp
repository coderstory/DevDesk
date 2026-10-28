#include "module_schedule/schedule_provider.h"

namespace module::schedule {

core::Result<void> ScheduleProviderRegistry::registerProvider(
    std::unique_ptr<ScheduleProvider> provider) {
    if (!provider) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "ScheduleProviderRegistry：来源为空");
    }
    const auto items = provider->items();  // 来源自述 source 名（判重依据）
    std::string source;
    if (!items.empty()) {
        source = items.front().source;
    }
    if (!source.empty()) {
        for (const auto& existing : sources_) {
            if (existing == source) {
                return core::Result<void>::fail(
                    core::ErrorCode::AlreadyExists,
                    "ScheduleProviderRegistry：来源重复注册被拒绝（source=" +
                        source + "）");
            }
        }
    }
    providers_.push_back(std::move(provider));
    sources_.push_back(source);
    return core::Result<void>::ok();
}

std::vector<ScheduleItemView> ScheduleProviderRegistry::allItems() const {
    std::vector<ScheduleItemView> out;
    for (const auto& provider : providers_) {
        const auto items = provider->items();
        out.insert(out.end(), items.begin(), items.end());
    }
    return out;
}

void ScheduleProviderRegistry::resetForTest() {
    providers_.clear();
    sources_.clear();
}

} // namespace module::schedule
