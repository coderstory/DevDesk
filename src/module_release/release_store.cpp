// 上线域数据访问层（骨架空实现）：接口先固化，SQL 由后续任务填入。
#include "release_store.h"

namespace module::release {

namespace {
core::Result<void> notImplemented() {
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
                                    "release_store: 尚未实现");
}
} // namespace

ReleaseStore::ReleaseStore(core::data::SqliteDb* db) : db_(db) {}

core::Result<std::vector<Service>> ReleaseStore::listServices() const {
    (void)db_;
    return core::Result<std::vector<Service>>::fail(
        core::ErrorCode::NotImplemented, "release_store: 尚未实现");
}

core::Result<std::vector<Environment>> ReleaseStore::listEnvironments() const {
    (void)db_;
    return core::Result<std::vector<Environment>>::fail(
        core::ErrorCode::NotImplemented, "release_store: 尚未实现");
}

core::Result<std::vector<ReleaseTask>> ReleaseStore::listTasks() const {
    (void)db_;
    return core::Result<std::vector<ReleaseTask>>::fail(
        core::ErrorCode::NotImplemented, "release_store: 尚未实现");
}

core::Result<ReleaseTask> ReleaseStore::getTask(const std::string& id) const {
    (void)db_;
    (void)id;
    return core::Result<ReleaseTask>::fail(core::ErrorCode::NotImplemented,
                                           "release_store: 尚未实现");
}

core::Result<void> ReleaseStore::saveTask(const ReleaseTask& task) {
    (void)task;
    return notImplemented();
}

core::Result<std::vector<ConfigItem>> ReleaseStore::listConfigItems(
    const std::string& serviceId) const {
    (void)db_;
    (void)serviceId;
    return core::Result<std::vector<ConfigItem>>::fail(
        core::ErrorCode::NotImplemented, "release_store: 尚未实现");
}

} // namespace module::release