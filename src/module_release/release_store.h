// 上线域数据访问层（骨架）：只声明接口，实现待后续任务填充。
// 错误一律走 Result，不抛异常；db 为空 = 未装配，各方法返回失败。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"
#include "release_types.h"

#include <string>
#include <vector>

namespace module::release {

class ReleaseStore {
public:
    explicit ReleaseStore(core::data::SqliteDb* db);

    core::Result<std::vector<Service>> listServices() const;
    core::Result<std::vector<Environment>> listEnvironments() const;
    core::Result<std::vector<ReleaseTask>> listTasks() const;
    core::Result<ReleaseTask> getTask(const std::string& id) const;
    core::Result<void> saveTask(const ReleaseTask& task);
    core::Result<std::vector<ConfigItem>> listConfigItems(
        const std::string& serviceId) const;

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::release