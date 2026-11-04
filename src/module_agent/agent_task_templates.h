// 出厂任务模板（需-6 原批 + need 219 预设批）：新建任务弹窗 step 0「一键套用」的数据源。
// 纯常量表、零逻辑，与 kCronTemplates（agent_module.h）同定位——表内容是产品文案不是
// 代码结构，加条目只改这里。
//
// 字段契约（由 tests/test_modules/test_agent_task_templates.cpp 钉住，改坏即红）：
//   - name / goal 非空；triggerType 必须是 kTriggerKeys（task_draft_job.h）里的合法键
//   - schedule 型 cron 非空且过 parseCron；manual 型 cron 必须留空（防「切回定时」残留）
//   - goal 里出现的「小写字母.小写字母」记号（工具名形态，含下划线段如
//     claude.list_profiles）必须都在生产注册清单内——禁止臆造工具名；
//     文件扩展名（.md 等）不算工具名
// goal 文案约束：除已注册工具名外，不得出现其它「小写字母.小写字母」序列（URL 域名、
// 英文文件名都会撞这条），需要写示例文件名时用中文衔接 .md 后缀。
// 注册口径：预设批引用的 schedule.runs / agent.runs / memory.stats / memory.health /
// mcp.health / todo.list / claude.* 由所属模块启停时注册（模块禁用 → 工具未注册 →
// run 内 LLM 收到既定的「工具未找到」错误，任务以失败收场并在回复注明）。
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
    // ── need 219 预设批 P-01..P-09（下标 4..12；原 4 条下标不动）──
    {"日期笔记", "manual", "",
     "你是每日笔记助手。先用 app.now 取当前日期，然后用 fs.write 在笔记目录新建 "
     "notes/daily/日期笔记-当天日期.md（会自动创建父目录），写入今日日期标题与三个空白"
     "小节（今日完成 / 进行中 / 备注）。若当天文件已存在，改为在原文末尾追加分隔线与"
     "更新时间，不覆盖原内容。写完后在最终回复给出文件路径。"},
    {"逾期待办巡检", "schedule", "0 9 * * *",
     "你是逾期待办巡检助手。先用 todo.list 取全部任务（参数 overdue=true），拿到逾期"
     "清单；清单为空时，最终回复一句话汇报「今日无逾期待办」，不写文件也不发通知；"
     "有逾期项时逐条列出标题与逾期天数，用 app.notify 发一条系统提醒（标题「逾期待办」，"
     "正文汇总条数与最紧急的三项），最后用 fs.write 把巡检报告写入 "
     "notes/inspect/待办巡检-当天日期.md，并在最终回复给出逾期总数。"},
    {"Profile 可用性巡检", "schedule", "0 9 * * *",
     "你是配置档巡检助手。先用 claude.list_profiles 列出全部配置档，然后逐个用 "
     "claude.test_connection 检测可用性（每次检测消耗约 1 个 token，属正常成本）；"
     "全部可用时最终回复一句话汇报「全部配置档可用」，不发通知；存在不可用档位时，"
     "用 app.notify 发一条系统提醒（标题「配置档巡检异常」，正文列出异常档位名与失败"
     "原因），并在最终回复给出可用数与异常数。"},
    {"定时任务报错巡检", "schedule", "0 9 * * *",
     "你是定时任务巡检助手。先用 schedule.runs 查询执行记录（参数 status=failed、"
     "sinceHours=24）；没有失败记录时，最终回复一句话汇报「定时任务近 24 小时全部正常」，"
     "不写文件也不发通知；有失败记录时逐条列出规则与失败原因，用 app.notify 发一条系统"
     "提醒（标题「定时任务报错」，正文汇总失败条数与首条原因），最后用 fs.write 把巡检"
     "报告写入 notes/inspect/定时任务巡检-当天日期.md，并在最终回复给出失败总数。"},
    {"智能体报错巡检", "schedule", "0 9 * * *",
     "你是智能体巡检助手。先用 agent.runs 查询运行记录（参数 status=failed、"
     "sinceHours=24）；没有失败记录时，最终回复一句话汇报「智能体近 24 小时全部正常」，"
     "不写文件也不发通知；有失败记录时逐条列出任务名与失败原因，用 app.notify 发一条"
     "系统提醒（标题「智能体报错」，正文汇总失败条数与首条原因），最后用 fs.write 把"
     "巡检报告写入 notes/inspect/智能体巡检-当天日期.md，并在最终回复给出失败总数。"},
    {"记忆模块统计", "schedule", "0 9 * * 1",
     "你是记忆模块统计助手。先用 memory.stats 取提案分组计数与用量账面，然后用 fs.write "
     "把统计写入 notes/inspect/记忆统计-当天日期.md（待审 / 已接受 / 已拒绝 / 已过期"
     "计数、今日与本月 token 与金额）；统计完成后在最终回复用一句话给出待审条数与本月"
     "消耗。本任务不发通知。"},
    {"MCP 连通巡检", "schedule", "0 9 * * *",
     "你是 MCP 服务巡检助手。先用 mcp.health 查询本机 MCP 服务的监听状态与探活结果；"
     "服务运行且探活通过时，最终回复一句话汇报「MCP 服务正常」并附端口与工具数，"
     "不发通知；服务未启动或探活失败时，用 app.notify 发一条系统提醒（标题「MCP 服务"
     "异常」，正文给出现状与失败原因），并在最终回复注明可能原因。"},
    {"记忆引擎健康巡检", "schedule", "0 9 * * *",
     "你是记忆引擎巡检助手。先用 memory.health 查询记忆引擎的运行状态与本机探活结果；"
     "引擎运行且探活通过时，最终回复一句话汇报「记忆引擎正常」，不发通知；引擎未运行"
     "或探活失败时，用 app.notify 发一条系统提醒（标题「记忆引擎异常」，正文给出状态"
     "与失败原因，并提示可在记忆页主控台启动引擎），并在最终回复注明失败原因。"},
    {"一键系统晨检", "schedule", "0 9 * * *",
     "你是系统晨检助手，依次执行五项检查：先用 memory.health 查记忆引擎健康，再用 "
     "mcp.health 查 MCP 服务连通，然后用 schedule.runs（参数 status=failed、"
     "sinceHours=24）查定时任务报错，用 agent.runs（参数 status=failed、sinceHours=24）"
     "查智能体报错，最后用 todo.list（参数 overdue=true）查逾期待办。若某工具不存在"
     "（对应模块未启用），把该项记为「模块未启用」并继续其余检查，不视为任务失败。"
     "五项全部正常时，最终回复一句话汇报「晨检全部正常」，不发通知也不写文件；任何"
     "一项异常时，把全部异常项汇总成一条清单，用 app.notify 只发一条系统提醒（标题"
     "「晨检发现异常」，正文按项列出问题摘要），并用 fs.write 把完整晨检报告写入 "
     "notes/inspect/晨检-当天日期.md，最终回复给出异常项数。"},
};

inline constexpr int kTaskTemplateCount =
    static_cast<int>(sizeof(kTaskTemplates) / sizeof(kTaskTemplates[0]));

} // namespace module::agent
