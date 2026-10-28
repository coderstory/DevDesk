// 上线域数据结构（跨文件唯一契约）。UI / 存储 / 纯逻辑三方共用：字段名与类型
// 一律不得重命名或改型，只允许在末尾追加字段（追加需同步 core/schema.h 与 store）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace module::release {

// 服务类型 → release_service.kind：backend / pc_web / mobile_web（文本编码）
enum class ServiceKind { Backend, PcWeb, MobileWeb };
// 配置项类型 → release_task_config.kind：nacos / db / cron / menu / gateway（文本编码）
enum class ConfigKind  { Nacos, Db, Cron, Menu, Gateway };
// 服务级步骤 → release_task_step.kind：branch / config / deploy / verify（文本编码；
// MergeBranch 存 'branch'，与设计文档 §9 的 kind(branch/deploy/verify) 对齐）
enum class StepKind    { MergeBranch, Config, Deploy, Verify };
// 任务状态 → release_task.status：draft / pending / running / done / voided（文本编码）
enum class TaskStatus  { Draft, Pending, Running, Done, Voided };
// 提醒级别：不进库，只用于到期聚合结果
enum class RemindLevel { Yellow, Orange, Red };

// 服务目录（基础配置长期维护；任务引用时快照）
struct ServiceDef { std::int64_t id=0; std::string name; ServiceKind kind=ServiceKind::Backend; std::string defaultBranch; int sort=0; };
// 环境目录（serviceIds 存 release_env_service 关联表）
struct EnvDef     { std::int64_t id=0; std::string name; std::vector<std::int64_t> serviceIds; int startMinute=0; int endMinute=0; int sort=0; };
// 上线任务（planDate 为 YYYY-MM-DD；announceSentAt / 各 ok 为 0 = 未完成）
struct Task       { std::int64_t id=0; std::string planDate; /*YYYY-MM-DD*/ TaskStatus status=TaskStatus::Draft;
                    std::string announceText; std::int64_t announceSentAt=0;
                    std::string pretestNo; bool pretestOk=false; std::string cabNo; bool cabOk=false;
                    std::string ticketNacosNo; bool ticketNacosOk=false; std::string ticketDbNo; bool ticketDbOk=false; };
// 任务环境快照（startMinute/endMinute 为当天分钟；end <= start 视为次日）
struct TaskEnv    { std::int64_t id=0; std::int64_t taskId=0; std::string envName; int startMinute=0; int endMinute=0; bool removed=false; };
// 任务服务快照（按 服务 × 环境 展开）
struct TaskService{ std::int64_t id=0; std::int64_t taskEnvId=0; std::string serviceName; ServiceKind kind=ServiceKind::Backend; std::string defaultBranch; std::string branch; bool removed=false; };
// 配置项（仅后端服务；skipped = 本次没有；doneAt 为 epoch 秒，0 = 未完成）
struct TaskConfig { std::int64_t id=0; std::int64_t taskServiceId=0; ConfigKind kind=ConfigKind::Nacos; std::string content; bool skipped=false; int order=0; std::int64_t doneAt=0; };
// 服务级步骤（doneAt 为 epoch 秒，0 = 未完成）
struct TaskStep   { std::int64_t id=0; std::int64_t taskServiceId=0; StepKind kind=StepKind::MergeBranch; std::int64_t doneAt=0; };
// 操作留痕（at 为 epoch 秒；scope/step 为人类可读定位；action = confirm / revoke / submit / reorder）
struct TaskLog    { std::int64_t id=0; std::int64_t taskId=0; std::string scope; std::string step; std::string action; std::int64_t at=0; std::string note; };
// 到期条目：提醒窗只消费本结构（level 决定条目色标，最高级别决定窗色）
struct DueItem    { RemindLevel level=RemindLevel::Yellow; std::int64_t taskId=0; std::string scope; std::string title; std::string detail; };

} // namespace module::release