// 出厂任务模板（需-6）：新建任务弹窗 step 0「一键套用」的数据源。纯常量表、零逻辑，
// 与 kCronTemplates（agent_module.h）同定位——表内容是产品文案不是代码结构，加条目只改这里。
//
// 字段契约（由 tests/test_modules/test_agent_task_templates.cpp 钉住，改坏即红）：
//   - name / goal 非空；triggerType 必须是 kTriggerKeys（task_draft_job.h）里的合法键
//   - schedule 型 cron 非空且过 parseCron；manual 型 cron 必须留空（防「切回定时」残留）
//   - goal 里出现的「小写字母.小写字母」记号（工具名形态）必须都在生产注册清单内
//     （agent_builtin_tools 的 14 件 + app.now/app.notify）——禁止臆造工具名；
//     文件扩展名（.md 等）不算工具名
// goal 文案约束：除已注册工具名外，不得出现其它「小写字母.小写字母」序列（URL 域名、
// 英文文件名都会撞这条），需要写示例文件名时用中文衔接 .md 后缀。
#pragma once

namespace module::agent {

struct TaskTemplate {
    const char* name;         // 预填 editName_
    const char* triggerType;  // kTriggerKeys 值；当前只用 manual / schedule
    const char* cron;         // 仅 schedule 型非空
    const char* goal;         // 预填 editGoal_（任务提示词）
};

inline constexpr TaskTemplate kTaskTemplates[] = {
    {"日报草稿", "schedule", "0 18 * * *",
     "你是日报助手。先用 app.now 取当前日期，再用 git.log 拉取最近 50 条提交（参数 n=50），"
     "按提交时间筛出今天的提交；按「今日完成 / 进行中 / 明日计划」三段整理成中文日报草稿，"
     "每条附提交首行摘要。整理完成后用 fs.write 把草稿写入笔记目录 "
     "notes/daily/日报-当天日期.md（会自动创建父目录），并在最终回复里给出文件路径、"
     "今日提交数与两三句总结。"},
    {"发版 Changelog", "manual", "",
     "你是发版助手。先用 bash.run 执行 git tag --sort=-creatordate 取最近两个 tag"
     "（不足两个 tag 时在最终回复说明原因并停止，不写任何文件）；再用 bash.run 依次执行 "
     "git log 旧tag..新tag --oneline 与 git diff --stat 旧tag..新tag，拿到该区间全部提交"
     "与文件变更面；按「新增 / 修复 / 优化 / 兼容性说明」四段模板整理成中文 changelog 草稿"
     "（每条提交一行，保留原文语义）；最后用 fs.write 写入 "
     "notes/changelog/发版记录-新tag.md，并在最终回复给出文件路径、区间提交数与变更文件数。"},
    {"测试环境巡检", "schedule", "0 9 * * *",
     "你是测试环境巡检助手。对本任务目标里配置的地址（默认给出一个，可自行改成本环境实际"
     "地址，形如 http://测试环境主机/health）逐个用 bash.run 执行 curl 探测（超时 10 秒，"
     "记录 HTTP 状态码与耗时）；正常的条目一句话带过，异常条目（非 2xx 状态、超时、连接失败）"
     "置顶并详述现象与可能原因。只要存在异常条目，就用 app.notify 发一条系统提醒，标题"
     "「测试环境巡检异常」，正文列出全部异常条目；最后用 fs.write 把完整巡检报告写入 "
     "notes/inspect/巡检-当天日期.md，并在最终回复给出正常数与异常数。"},
    {"变更雷达晨报", "schedule", "0 9 * * 1-5",
     "你是变更雷达晨报助手。先用 app.now 取当前时间；对本机登记的业务仓库"
     "（仓库根路径列表——用户首次使用时补全，模板不自动探测仓库布局）逐个检查："
     "git.status（参数 repo=该仓库路径）读分支与工作区状态——porcelain 行即未提交改动、"
     "behind 计数即落后远端的分支、ahead 计数即未推送的本地提交；git.log"
     "（参数 repo=该仓库路径、n=50）拉最近提交，按提交时间筛出近 24 小时内的提交；"
     "汇总后按仓库逐行给出结论（一行 = 仓库名 + 三类风险摘要），有风险的仓库排前面，"
     "无风险的仓库一句话带过；全部仓库都干净时整体一句话汇报。"},
};

inline constexpr int kTaskTemplateCount =
    static_cast<int>(sizeof(kTaskTemplates) / sizeof(kTaskTemplates[0]));

} // namespace module::agent
