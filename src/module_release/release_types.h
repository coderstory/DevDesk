// 上线域数据结构（骨架）。字段随后续任务按需求补齐；跨文件契约只在本头声明，
// 各 UI/存储/逻辑文件不得私自定义同义结构。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace module::release {

// 服务（被发布的业务系统）
struct Service {
    std::string id;
    std::string name;
};

// 环境（发布目标，如测试/预发/生产）
struct Environment {
    std::string id;
    std::string name;
    bool production = false;
};

// 服务实例（某服务在某环境下的已部署版本）
struct ServiceInstance {
    std::string serviceId;
    std::string environmentId;
    std::string version;
};

// 配置项（基础配置页维护）
struct ConfigItem {
    std::string key;
    std::string value;
};

// 执行步骤（任务执行页按序推进）
struct Step {
    std::string id;
    std::string name;
    bool done = false;
};

// 步骤日志
struct LogEntry {
    std::string stepId;
    std::int64_t atUnix = 0;
    std::string message;
};

enum class TaskStatus {
    Pending,
    Running,
    Done,
    Failed,
    Cancelled,
};

// 发布任务
struct ReleaseTask {
    std::string id;
    std::string title;
    std::string serviceId;
    std::string environmentId;
    TaskStatus status = TaskStatus::Pending;
    std::int64_t plannedAtUnix = 0;
    std::int64_t expiresAtUnix = 0;
    std::vector<std::string> stepIds;
};

} // namespace module::release