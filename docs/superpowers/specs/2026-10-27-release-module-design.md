# DevDesk 上线模块设计（Release）

> **创建日期**：2026-10-27
> **模块**：`module_release`（新）
> **状态**：设计定稿；实施计划见 `docs/superpowers/plans/2026-10-27-release-module-implementation-plan.md`
>
> **2026-10-27 修订（D-27-01~06）**：增补侧边栏二级菜单、任务列表瀑布流、单据号门槛、原型一致性四闸、最小侵入实现约束与文档漂移备注。与旧文冲突处已改写为新口径，并保留一行旧口径说明供追溯。
>
> **实现基准**：本 spec + `docs/superpowers/specs/2026-10-27-release-module-tokens.json`（待 T0.1 产出）+ 代码 token。设计规范存档与旧 demo 不得作为实现基准，见 §13.6。

---

## 1. 定位与边界

把「一个人负责的、多环境多服务的发布流程」变成一份可留痕的清单式执行台。

**做什么**：配置管理、完整性校验、按时间点提醒、按步骤逐条确认、全程留痕。

**明确不做**（第一版）——

| 不做 | 原因 |
|------|------|
| 触发 GitLab 流水线 | 工具是记录台不是执行器；上线不可逆，不经工具的手更安全 |
| 调用 nacos / 数据库 / 网关 API | 同上，且需要各系统凭据 |
| 校验分支是否真实存在 | 需要 GitLab 凭据与网络；写错分支在执行那一步自然会撞上 |
| 多人协作 / 远端同步 | 本地 sqlite，单机使用 |

后续若要接外部系统，以扩展的形式挂在既有模型上，不改核心数据。

---

## 2. 领域模型

```
上线任务（一天一个，提前建）
├── 任务级前置项：提测单 · CAB 单 · 工单（nacos/数据库）· 上线公告
└── 环境（可多个）
    └── 服务（按 服务 × 环境 展开）
        ├── 合并分支
        ├── 配置项（仅后端：nacos · 数据库 · 定时任务 · 菜单 · 网关）
        ├── 部署
        └── 验证完成
```

环境与服务的定义放在**基础配置**里长期维护；任务引用它们时**做快照**，此后互不影响。

---

## 3. 基础配置

### 3.1 服务

| 字段 | 说明 |
|------|------|
| 名称 | 唯一 |
| 类型 | 后端 / PC 前端 / 移动前端 |
| 默认分支 | 新建任务时预填到每个环境，可改 |

**配置项由类型决定，不是逐服务勾选**：后端固定 5 项（nacos、数据库、定时任务、菜单、网关）；两类前端一项都没有。

### 3.2 环境

| 字段 | 说明 |
|------|------|
| 名称 | EM / EU / AM / US 等 |
| 包含的服务 | 从服务目录里选 |
| 上线时间窗 | 开始 + 结束；允许跨零点（结束 ≤ 开始视为次日） |

环境定义全局共用；一个任务里同一环境只出现一次。

---

## 4. 任务

### 4.0 任务列表（2026-10-27 新增）

任务列表采用**多列自适应卡片瀑布流**（多列不等高卡片、逐列向下排布；「瀑布流」= 卡片按列堆叠，不是等高网格）。

| 契约 | 值 |
|------|----|
| 最小卡片宽 | 320（逻辑像素） |
| 列间距 / 垂直间距 | 16 / 16 |
| 卡片高度 | 随内容不等高；同一列内向下排 |
| 首屏数量 | 10 |
| 加载更多 | 滚动到列表底部自动再加 10，直到没有更多 |
| 排序 | 计划日期降序；同日按 id 降序（同日可能有多个作废任务，作废不占名额） |
| 加载占位 | 加载中时在列表底部居中显示「加载中…」，不占卡片位置 |
| 列数公式 | `max(1, floor((内容宽 + 16) / (320 + 16)))`；内容宽 = 页面区宽 − 64（左右各 32） |
| 实现 | EUI-NEO `components::virtualmasonry` 的 `columns / gap / itemHeight / onEndReached`；不另写瀑布流布局 |

- 空状态：无任务时显示统一空状态（说明 + 「新建任务」按钮），不留大片空白。
- 点击卡片进入该任务的执行页；已作废任务卡片置灰但仍可查看。
- 分页只在内存窗口内：SQL 用 `LIMIT 10 OFFSET n` 取数；加载中禁止再次触发 `onEndReached`（防重复条目）。
- 加载动作延后一帧执行：`onEndReached` 先置 `loading=true` 并请求下一帧，再查库追加；保证「加载中…」至少可见一帧，但不做最小展示时长。
- 若 10 条不足以填满一屏（内容高度 < 视口 → 无法滚动），`virtualMasonry` 会继续触发 `onEndReached` 直到可滚或数据耗尽；这是首屏 10 条的必要补充，不是分页 bug。
- 卡片高度由**固定行高**组合得到（标题 24 + 进度条 6 + 环境 chips 22 + 摘要最多 3 行 × 22 + 间距），不依赖浏览器或 FreeType 的自动换行度量；摘要超过 3 行截断。
- 环境 chips 只排一行；放不下时末尾显示「+N」，不换行（否则 chip 换行会把卡高变成字体度量的函数，破坏闸 3 的确定性）。
- 1600×1000 默认窗口（侧栏 208、页面 1392）下内容宽 1328，恰好 4 列 × 320；最小窗口 1000×640 下内容宽 728，2 列 × 356。

### 4.1 创建

- **计划上线日期**必填，默认只允许**周二、周四**；选其他日子弹二次确认「非常规发布日，确定要建吗？」。
- 「一天一个」按**计划日期**算：同一天已有**非作废**任务时，点「新建」直接打开已有那个，不新建；只剩作废任务时可以新建（§7）。
- 任务提前建，挂未来的发布日；跨到凌晨完成不算超期。

### 4.2 内容

| 部分 | 说明 |
|------|------|
| 加入环境 | 从基础配置挑，可加多个；加入即快照（环境名称、时间窗、服务清单、服务类型与默认分支） |
| 移除环境 | 整个环境从任务里拿掉，二次确认 |
| 移除服务 | 服务级「跳过」= 从任务移除，二次确认；不占进度、不提醒、不出现在执行清单 |
| 分支 | 每个「服务 × 环境」各填一个，默认带服务定义的默认分支；纯手填，不校验真伪 |
| 配置内容 | 仅后端；5 项都是文本，粘贴填写 |
| 配置项跳过 | 某项本次没有 → 勾跳过；该步显示「已跳过」并划掉，校验时不报缺 |
| 工单卡 | 任务级；任一「未移除且未跳过」的服务填了非空 nacos 内容 → 出一张 nacos 工单卡；数据库内容同理（与 §4.5 同口径） |
| 公告 | 任务级唯一；内容用户填；时间自动取当天**最早环境的开始时间** |

### 4.3 校验

- 触发：点「校验」按钮随时跑；点「提交」时自动跑一次，不过不放行。
- 规则：
  - 至少有一个未移除的环境，且该环境至少有一个未移除的服务（否则完成判定会「真空真」直接完成，2026-10-27 补齐）
  - 本次未移除的服务，分支不能为空
  - 未勾跳过的配置项，内容不能为空
  - 内容为空又没勾跳过 → 报错
- 不校验：分支真伪、配置内容对错、工单号、各类前置单（这些在执行期卡）。
- 呈现：不合格项标红，并列出「哪儿不完整」清单。

### 4.4 修改

- 提交后仍可改配置；改完自动重新校验。
- **改配置内容或改分支 → 自动撤回该步的已确认状态**，并记一条日志（内容变了，旧确认不作数）。「改」指值实际变化；打开即保存、值未变不撤回。

### 4.5 单据号门槛（2026-10-27 新增）

适用于四个任务级前置项：

| 前置项 | 完成勾选 | 单据号字段 |
|--------|----------|------------|
| 提测单 | 「已通过」 | 提测单号 |
| CAB 单 | 「已通过」 | CAB 单号 |
| nacos 工单 | 「已审批」 | 工单号 |
| 数据库工单 | 「已审批」 | 工单号 |

- 规则：单据号去除首尾空白后非空，才允许勾选完成；**只校验非空，不校验格式**（不校验数字、长度、前缀）。
- 单据号为空时，完成复选框禁用，并在其旁提示「请先填写单据号」。
- 禁用只是视觉层；勾选入口必须有程序守卫（守卫拒绝 + 记日志），防绕过 UI 直接写库。
- 公告无单据号，不受本门槛约束。
- 工单卡是否存在由内容决定：任务里任一「未移除且未跳过」的服务 nacos 内容非空 → 显示 nacos 工单卡；数据库内容同理。卡不存在时不要求对应单据号。
- 本门槛只卡「勾选完成」这一动作；勾选后再清空单据号不自动撤回（见 §8 边界约定 #7）。

---

## 5. 执行

### 5.1 步骤

**任务级前置**（可提前完成，有到期时间）

| 步骤 | 形式 | 到期时间 |
|------|------|----------|
| 提测单 | 填单号 + 勾「已通过」 | 时间基准 − 1h |
| CAB 单 | 填单号 + 勾「已通过」 | 时间基准 − 1h |
| nacos 工单 | 填工单号 + 勾「已审批」 | 时间基准 − 1h |
| 数据库工单 | 填工单号 + 勾「已审批」 | 时间基准 − 1h |
| 上线公告 | 填内容 + 勾「已发」 | 时间基准（最早环境开始时间） |

> **时间基准** = 任务里所有环境开始时间的最小值。
>
> 任务级前置项的「已通过 / 已审批」勾选受 §4.5 单据号门槛约束；服务级步骤没有单据号。

**服务级清单**（每个 服务 × 环境 各一套）

| 顺序 | 步骤 | 何时可做 |
|------|------|----------|
| 1 | 合并分支 | 随时（建单后即可勾） |
| 2~6 | 5 项配置（顺序可上下移） | 时间窗开始后 |
| 7 | 部署 | 时间窗开始后 |
| 8 | 验证完成 | 时间窗开始后 |

前端服务的清单只有 `合并分支 → 部署 → 验证完成`。

### 5.2 推进方式

- **自由推进 + 引导**：任意服务的任意可做步骤都能点，同时给一个「下一步」默认路径。
- **服务顺序可拖拽**，调整结果记进任务。
- **配置项顺序可上下移**（行内上下移按钮；另有拖拽手柄），但只在「配置中 / 待执行」阶段可调，进入执行后锁定。
- 过了时间窗仍可继续做，列表标红「已超时」。

### 5.3 撤回

- 任务未进入「已完成」前，**任何一步都能撤回**，弹二次确认；撤回后该服务退回未完成。
- 撤回记日志（哪一步、何时撤的）。
- 任务已完成 → 整单只读，不能撤回；要改只能作废重建。
- 已作废任务只读；要改先恢复（恢复受 §7 同日冲突约束）。

### 5.4 完成判定

- **环境完成** = 该环境里所有未移除的服务都走完（含验证完成）。
- **任务完成** = 所有环境完成 **且** 任务级前置全部完成。
- 环境里服务被全部移除 → 该环境直接算完成（没服务可做 = 不欠账）。

---

## 6. 提醒引擎

### 6.1 模型

每一项待办都带一个**到期时间**。引擎只做一件事：

> 任何时刻，把「**已经到期但还没完成**」的项收集起来，弹**一个**窗。

这样天然解决时间重叠——同一时刻该发的公告、该做的部署，列在同一个窗里，按环境分组；四个环境撞在一起也还是一个窗。

### 6.2 分级

| 颜色 | 含义 |
|------|------|
| 红 | 已到环境上线时间，部署未完成；或任务已到期仍停在「配置中」（这任务根本没准备好） |
| 橙 | 已到「时间基准 − 1h」，前置项未完成 |
| 黄 | 已到公告时间，公告未发 |

一个窗里混着多档时，**窗取最高优先级的色**，各条目自带色标。

### 6.3 行为

- **独立全屏窗**（新开窗口，不是应用内遮罩）。
- 窗里**只列、只跳转**，不能直接勾选；点某条 → 关窗跳到执行框对应位置，在那里操作。
- 关闭后 **5 分钟**再弹，直到该项完成。
- 全部做完了就不弹。
- 应用关着期间到期的，**重开时补弹**，不因错过而吞掉。
- 应用完全退出时无法提醒（不装后台服务、不开机自启）——已知边界。
- 任务处于「配置中」且未到时间基准 → 不弹任何提醒。

**实现形态（2026-10-27 补）**

- 独立子窗：EUI-NEO `app::openWindow`（工作区尺寸、置顶、可关闭），关闭走 `core::window::requestWindowClose`；窗口句柄在 `windowCreatedHandler` 捕获，模块停止时先关窗再释放状态（防 compose 捕获悬垂）。
- 引擎节拍：主窗每帧在 app 层拉一次（与 `agent::pollRunEnd` 同模式）；但**禁止每帧查库**——到期扫描按 1s 节流，任务/进度内存快照按 5s 或模块变更事件刷新。主窗隐藏到托盘或最小化不 compose 期间不提醒，恢复可见时按「已到期但未完成」补弹——与「应用关着期间到期，重开补弹」同一规则（已知边界，写入 §8）。
- 单窗约束：同一时刻最多一个提醒窗；窗已开时只刷新不重开。关闭后 `suppressUntil = now + 5min`，期间不弹；到期仍未完成再弹。
- 提醒窗只读：点条目 → `requestWindowClose` 关窗 → 唤起主窗并导航到对应任务/步骤位置；勾选仍在主窗执行。
- 窗口取全屏工作区尺寸时注意 EUI-NEO `windowSize` 是**物理像素**、`Screen` 是逻辑单位（DevDesk 已知单位坑），换算在窗口创建处只做一次。

---

## 7. 状态

```
配置中 ──提交──▶ 待执行 ──时间窗到──▶ 执行中 ──全部完成──▶ 已完成
   │                │                  │
   └────────────────┴──────────────────┴──▶ 已作废
```

- **状态只跟时间走**：提前勾前置不算「执行中」，只体现在进度上。
- **列表状态按时间推导**：列表显示的状态由纯函数 `deriveStatus` 按当前时间推导，不写库；库内状态只由提交 / 作废 / 恢复三个动作维护。
- **超时**是标记不是状态。
- **已作废**：停止提醒、进度冻结但保留；**可恢复**（按当前进度与时间自动回到对应状态）；**不占当天名额**，所以作废后当天可再建一个。若恢复时当天已有另一个非作废任务 → 拒绝恢复并提示（唯一约束不允许静默覆盖）。
- 状态值是英文枚举（`Draft/Pending/Running/Done/Voided`），界面显示中文；「超时」是计算出的标记，不入库。

---

## 8. 边界约定

| # | 约定 |
|---|------|
| 1 | 任务引用环境/服务一律**快照**，基础配置后续改动不影响已建任务 |
| 2 | 基础配置删服务/环境不影响历史任务；环境仍引用被删服务时，删服务要二次确认并自动从环境摘掉 |
| 3 | 时间窗允许跨零点，结束 ≤ 开始视为次日 |
| 4 | 提醒只在应用运行时生效；关闭期间到期的重开补弹 |
| 5 | 一个窗里多档颜色时窗取最高优先级，条目各自标色 |
| 6 | 配置项顺序只在配置/待执行阶段可拖，执行后锁定 |
| 7 | 单据号门槛只卡「勾选完成」动作；勾选后再清空单据号不自动撤回（本期明确不做） |
| 8 | 任务列表加载更多是本地 SQL 分页，不是网络请求；加载占位只在本帧/查询期间显示，不做最小展示时长 |
| 9 | 二级菜单是通用能力，但本期只有上线模块声明子项；其他模块的渲染结果必须与原版逐项一致 |
| 10 | 主窗隐藏到托盘 / 最小化（EUI 主循环不 compose）期间不弹提醒，恢复可见时补弹；完全退出不提醒 |

---

## 9. 数据表（2026-10-27 定稿）

> 旧草案只列关键列；本节补齐类型、约束与索引。全部为**新增表**，不改任何既有表与既有迁移路径：新表以 `CREATE TABLE IF NOT EXISTS` 追加进 `core/schema.h` 的 `kSql`，`applySchema` 启动时对老库同样幂等建表（不需要新增 `addMissing*` 补列函数）。

时间口径：`plan_date` = 本地日期字符串 `YYYY-MM-DD`；`*_at` = Unix epoch 秒；`start_minute/end_minute` = 当天分钟数 0~1439；`end_minute <= start_minute` 视为次日结束。

| 表 | 列与约束 |
|----|----------|
| `release_service` | `id INTEGER PK AUTOINCREMENT`、`name TEXT NOT NULL UNIQUE`、`kind TEXT NOT NULL`（backend/pc_web/mobile_web）、`default_branch TEXT NOT NULL DEFAULT ''`、`sort INTEGER NOT NULL DEFAULT 0`、`created_at INTEGER NOT NULL` |
| `release_env` | `id`、`name TEXT NOT NULL UNIQUE`、`start_minute INTEGER NOT NULL`、`end_minute INTEGER NOT NULL`、`sort INTEGER NOT NULL DEFAULT 0`、`created_at INTEGER NOT NULL` |
| `release_env_service` | `env_id INTEGER NOT NULL`、`service_id INTEGER NOT NULL`、`PRIMARY KEY(env_id, service_id)`、双 FK `ON DELETE CASCADE` |
| `release_task` | `id`、`plan_date TEXT NOT NULL`、`status TEXT NOT NULL`（Draft/Pending/Running/Done/Voided）、`announce_text TEXT NOT NULL DEFAULT ''`、`announce_sent_at INTEGER`、`pretest_no TEXT NOT NULL DEFAULT ''`、`pretest_ok INTEGER NOT NULL DEFAULT 0`、`cab_no`/`cab_ok`、`ticket_nacos_no`/`ticket_nacos_ok`、`ticket_db_no`/`ticket_db_ok`、`created_at/updated_at INTEGER NOT NULL` |
| `release_task_env` | `id`、`task_id INTEGER NOT NULL`、`env_name TEXT NOT NULL`、`start_minute`、`end_minute`、`removed INTEGER NOT NULL DEFAULT 0`、`sort INTEGER NOT NULL DEFAULT 0`、`UNIQUE(task_id, env_name)` |
| `release_task_service` | `id`、`task_env_id INTEGER NOT NULL`、`service_name TEXT NOT NULL`、`service_kind TEXT NOT NULL`、`default_branch TEXT NOT NULL DEFAULT ''`、`branch TEXT NOT NULL DEFAULT ''`、`removed INTEGER NOT NULL DEFAULT 0`、`sort INTEGER NOT NULL DEFAULT 0`、`UNIQUE(task_env_id, service_name)` |
| `release_task_config` | `id`、`task_service_id INTEGER NOT NULL`、`kind TEXT NOT NULL`（nacos/db/cron/menu/gateway）、`content TEXT NOT NULL DEFAULT ''`、`skipped INTEGER NOT NULL DEFAULT 0`、`conf_order INTEGER NOT NULL DEFAULT 0`、`done_at INTEGER`、`UNIQUE(task_service_id, kind)` |
| `release_task_step` | `id`、`task_service_id INTEGER NOT NULL`、`kind TEXT NOT NULL`（branch/deploy/verify）、`done_at INTEGER`、`UNIQUE(task_service_id, kind)` |
| `release_task_log` | `id`、`task_id INTEGER NOT NULL`、`scope TEXT NOT NULL`、`step TEXT NOT NULL`、`action TEXT NOT NULL`（create/submit/confirm/revoke/void/restore）、`at INTEGER NOT NULL`、`note TEXT NOT NULL DEFAULT ''` |

索引 / 唯一约束：

- `CREATE UNIQUE INDEX release_task_plan_date_active ON release_task(plan_date) WHERE status <> 'void'` —— 同一天只能有一个**非作废**任务；作废后当天可再建（§7）。
- 普通索引：`release_task_env(task_id)`、`release_task_service(task_env_id)`、`release_task_config(task_service_id)`、`release_task_step(task_service_id)`、`release_task_log(task_id, at)`。
- 删除基础配置的服务/环境：`release_env_service` 级联删除；历史任务不受影响（任务侧全部是快照列，不引用基础配置主键）。

---

## 10. 模块接入点

侧边栏是按登记表 `parent` 列组织的**可折叠树**。`ModuleEntry` 的 `parent` 类型为 `std::optional<PageId>`：空 = 顶级项，走既有平铺路径；非空 = 挂在对应分组父项下。既有 8 行的元数据、节点 id、相对顺序与缩进保持不变；几何不再逐行保持——「上线」父项占一整行，位于其后的 Settings / About 整体下移 44px。

上线模块在登记表占 3 行，共用门控键 `kModuleIdRelease`：

| 登记行 | PageId | 短名 | 父级 | 工厂行为 |
|--------|--------|------|------|----------|
| 分组父项 | `ReleaseGroup` | 上线 | 无 | `create_release_group_module()` 恒返回 `nullptr`：不可导航，不注册 Router，只承载侧边栏父级身份 |
| 任务 | `ReleaseTasks` | 任务 | `ReleaseGroup` | `create_release_tasks_module()` 返回任务页 Module |
| 基础配置 | `ReleaseConfig` | 基础配置 | `ReleaseGroup` | `create_release_config_module()` 返回配置页 Module |

接入涉及的文件与职责：

| 文件 | 职责 |
|------|------|
| `src/core/module_interface.h` | `PageId` 含 `ReleaseGroup` / `ReleaseTasks` / `ReleaseConfig`；`pageIdToString` 对应三项 |
| `src/app_registry.h` | `ModuleEntry` 含 `std::optional<PageId> parent`；登记表含上述 3 行；声明 3 个 `extern "C"` 工厂；提供 `isGroupPage()` |
| `src/main.cpp` | 三个静态实例 + `moduleForPage` 三个 case |
| `src/app_sidebar.h` | 分组展开态接口 `sidebarGroupExpanded` / `toggleSidebarGroup`（进程内状态，默认折叠，不落库） |
| `src/app_sidebar.cpp` | 分组父项渲染（图标 + 短名 + chevron 箭头、当前页在组内时的选中条）+ 子项缩进 16 + 展开/收起；父项点击只切换展开；折叠态父项画居中图标，子项按展开态继续列出（缩进按 0） |
| `src/app_assembly.cpp` | 同一门控键匹配多行时逐行拨开关，不在首行命中后返回；无实例的分组父项行跳过；「键命中但页面无实例」与「键未知」分开诊断 |
| `src/module_settings/settings_config.h` | `kModuleIdRelease` |
| `src/module_settings/settings_config.cpp` | 合法 ID 清单 + 默认启用集 + 版本化默认模块迁移（`moduleDefaultsVersion`，版本 1 并入 release） |
| `src/module_settings/settings_ui_cards.cpp` | 模块开关卡含第 5 项「上线」；开关行数由 `kToggleCount` 派生 |
| `src/module_settings/settings_ui_logic.h` | `kModulesCardHeight = 254`（5 项 3 行的卡高） |
| 根 `CMakeLists.txt`、`src/module_release/CMakeLists.txt` | 模块静态库、`add_subdirectory`、app 链接、覆盖率插桩清单 |
| `tests/CMakeLists.txt` | 测试源登记 + `devdesk_module_release` 链接 |
| `tests/probes/uat62_page_shots_probe.cpp` | 页面截图 slug 清单加入 `release_group` / `release_tasks` / `release_config`，并用 `static_assert` 锁登记表行数 |
| `tests/probes/wallpaper_probe.cpp` | PageId 遍历 switch 补齐三个新枚举值 |

模块注册在根 `CMakeLists.txt`（`add_subdirectory` 段与 app `target_link_libraries` 段），仓库内不存在 `src/CMakeLists.txt`。

### 10.1 侧边栏分组树

- 分组父项由登记表行的 `parent` 列识别；父项自身也是登记表一行，工厂返回 `nullptr`，装配层跳过注册，侧边栏用登记表行渲染父级。
- 父项点击只切换展开/收起，不导航；子项点击导航到对应 `PageId`。当前页属于某分组时，父项保持选中条（子项收起后选中信息由父项承载）。
- 子项缩进 16（展开态）；折叠侧栏（72 宽）下父项画居中图标，子项按展开态继续以图标列出，缩进按 0 参与命中区几何。
- 分组展开态是进程级状态（`std::set<PageId>` 记录已展开的分组，空集 = 全折叠），默认折叠，不落库；`resetSidebarGlobals()` 将其复位。
- 同一门控键可对应多行：`dispatchModuleToggle` 遍历全部匹配行逐页注册 / 注销并 `onStart` / `onStop`；分组父项无实例，跳过。
- 节点 id：`app.sidebar.item.ReleaseGroup.hit` / `.icon` / `.label` / `.chevron`，`app.sidebar.item.ReleaseTasks.hit`，`app.sidebar.item.ReleaseConfig.hit`。

### 10.2 已实现结构（文件与职责）

| 文件 | 职责 |
|------|------|
| `release_types.h` | 领域类型：`ServiceKind` / `ConfigKind` / `StepKind` / `TaskStatus` / `RemindLevel`，以及 `ServiceDef` / `EnvDef` / `Task` / `TaskEnv` / `TaskService` / `TaskConfig` / `TaskStep` / `TaskLog` / `DueItem` |
| `release_logic.h/.cpp` | 纯函数：常规发布日判定、时间基准、状态推导 `deriveStatus`、到期聚合 `dueItems`（带 `steps` 入参）、提交校验、进度与完成判定（分母按期望槽位 `expectedConfigs`）；无 IO、无 UI、无系统时钟依赖（`now` 由调用方传入） |
| `release_store.h/.cpp` | 9 表数据访问：基础配置 CRUD、任务及环境/服务/配置/步骤/日志快照、原子建单 `createTaskWithEnvs`（单事务）、按任务拉取步骤 `listTaskStepsByTask`；错误走 `Result`，SQL 全参数绑定 |
| `release_config_ui.h/.cpp` | 基础配置页：服务目录、环境定义、跨零点时间窗、删除二次确认 |
| `release_list_ui.h/.cpp` | 任务列表：多列瀑布流 + 分页器 + 卡片 + 加载占位；日期与状态文案工具 |
| `release_dialogs.h/.cpp` | 新建任务弹层：日期滚轮、非常规发布日二次确认、同日复用、环境与服务快照 |
| `release_detail_ui.h/.cpp` | 执行页：任务级前置、环境/服务步骤、页内全屏配置编辑层、配置查看与顺序、跳过 / 移除二次确认、撤回、作废/恢复、按时间推导状态 |
| `release_reminder.h/.cpp` | 提醒引擎（到期聚合调度、关闭后 5 分钟抑制、同内容指纹去重）+ 独立全屏提醒窗 |
| `release_module.h/.cpp` | 三个工厂 + `ReleaseTasksModule` / `ReleaseConfigModule` 两个 Module；数据访问走进程级 `appDatabase()` 共享连接 |
| `release_checkbox.h` | 自绘勾选框：选中 / 跳过 / 禁用 / hint 四态 |
| `release_view_state.h/.cpp` | 视图状态骨架：选中任务 id、筛选文本，不落库 |
| `CMakeLists.txt` | `devdesk_module_release` 静态库，依赖 `devdesk_core` / `devdesk_platform` / `devdesk_ui_kit` |

### 10.3 已知限制与待办

- `components::checkbox` 的 `build()` 中 hover / pressed 两个局部变量只赋值、从不读取（`components/checkbox.h:125-126`），在本仓库 `-Werror=unused-but-set-variable` 下编译失败；且该组件没有 disabled 态。本模块全部使用自绘勾选框 `release_checkbox.h`。建议后续向 EUI-NEO 提上游修复。
- 详情页「分支编辑」（修改任务快照中服务的分支）尚未实现：分支行以文本展示，`updateTaskService` 无 UI 调用方，属后续票据。（「提交（Draft→Pending）」、服务顺序拖拽、配置编辑与跳过、移除环境/服务均已实现，不再列入限制。）

---

## 11. UI 要点与 EUI-NEO 缺口（2026-10-27 核对源码后修订）

**已核实可用**：

- 瀑布流：`components/virtualmasonry.h` 提供 `columns / gap / itemHeight(index,width) / onEndReached / bind(signal)`，直接满足 §4.0。
- 独立子窗：`eui::app::openWindow(DslWindowConfig, compose)`；关闭用 `core::window::requestWindowClose(handle)`；句柄由 `windowCreatedHandler` 取。
- 多行文本：`components::input` 的 `.multiline(true)` + `.scrollbar(true)`。
- 全屏编辑：`DialogBuilder.screen(screen.width, screen.height)` 模态层，已落地于执行页的页内全屏配置编辑层（多行大文本框 + 字数统计 + 保存 / 取消，不新开窗口）。
- 复选框在本模块使用自绘 `release_checkbox.h`（选中 / 跳过 / 禁用 / hint 四态；原因见 §10.3）；开关 `ui_kit::wswitch`、下拉 `components::dropdown`、日期 `components::datepicker`、时间 `components::timepicker`、进度 `components::progress` 可用。
- 禁止用「`onChange` 里拒绝但仍显示可点」的伪禁用：单据号门槛的锁定态由自绘勾选框的 disabled 态 + 提示行表达。
- 块面式 Tab：用 `ui_kit::wtab`；`components::tabs` 是**下划线式**，本项目已明确淘汰，禁用。
- 拖拽：EUI-NEO 提供 `onDrag` 原语，无现成「拖拽排序」组件；排序计算自绘（组合实现，不新增通用组件）。
- 文本测量：`core::TextPrimitive::measureTextSize / measureTextWidth`（任务卡高度仍按固定行高而非测量值）。

**缺口与决定**：

- **折叠面板（Accordion）仍缺失**：环境分组**不做折叠**，实现与 demo 都改为常展开分组（标题行 + 内容），用 `rect/text/stack` 组合。若将来需要折叠，先在上游补组件再改，禁止实现期临时降级。（旧口径：环境分组若用折叠，需上游补 Accordion 或改 Tab/自绘面板；2026-10-27 决策改为常展开。）
- **skeleton / 转圈组件缺失**：加载占位只用文字「加载中…」（§4.0），不画 spinner，不新增自绘动画组件。
- **tree 组件缺失**：侧边栏二级菜单沿用现有自绘侧栏（`app_sidebar.cpp`），不引入新组件。
- **等宽字体**：EUI-NEO 的字体 fallback 与 HTML `font-family` 栈不等价；本期 branch / 单号字段用正文字体，demo 同步去掉 mono 字体（防闸 3 字体差异）。
- **全屏编辑决定**：用 `DialogBuilder.screen(...)` 模态层，不新开窗口（少一条窗口生命周期与焦点链）。（旧口径：screen 或 openWindow 二选一；新决策定为 screen。）
- **提醒窗**：唯一新增的独立窗口，按 §6.3 实现形态。

---

## 12. 后续可扩展（不在本期）

- 接 GitLab 只读 API：分支真伪校验、仓库地址字段
- 从 CI 读流水线状态，自动勾「部署」
- 上线记录导出（Markdown / CSV）
- 多机同步

---

## 13. 原型与实现一致性四闸（2026-10-27 新增，硬门禁）

> 四闸的目的一句话：**demo 好看不算数，实现要与 demo 和代码 token 同时对齐**。任一闸红 → 不得进入验收。

### 13.1 闸 1：数值单一源

- **唯一源文件**：`docs/superpowers/specs/2026-10-27-release-module-tokens.json`（T0.1 产出）。内容包括：
  - `colors.light` / `colors.dark`：本模块用到的全部主题槽位值与 release 专属黄档；
  - `sizes`：`theme.cpp` 的 `SizeTokens` 全量；
  - `metrics`：`ui_kit/metrics.h`、`button.h`、`selection_bar.cpp`、`card.cpp` 的共享常量；
  - `release`：本模块专属几何（卡宽 / 间距 / 页 padding / 分页条数 / 提醒窗尺寸）；
  - `lineHeights`：页标题 32、区块标题 26、卡片标题 24、正文 22、辅助 18。
- **生成物**：
  - `src/module_release/release_tokens.generated.h`（由脚本生成，文件头写「禁止手改」）；
  - `docs/superpowers/demo/release-module-demo.html` 的 `/* BEGIN GENERATED TOKENS */ … /* END GENERATED TOKENS */` 标记块。
- **校验脚本**：`scripts/gen-release-tokens.py --write` 生成；`--check` 只读校验，不一致退出码非 0（禁止只打印警告）。脚本用 UTF-8 读写，Windows 控制台须 `reconfigure`，防 GBK 崩溃。
- **三方校验**：单测 `release_tokens_match_theme` 断言 `core::theme::current().sizes.*`（fontScale=1.0）与生成头常量逐项相等；`--check` 断言 demo 块与生成头一致；grep 断言 `src/module_release/` 无裸 hex（颜色引用 `t.colors.*`）。
- **release 专属黄档**：`theme.cpp` 没有黄档；提醒黄档固定 `#EAB308`，写入 token 清单的 `colors.release.reminderYellow`，不新增全局 theme token（避免影响其他模块）。
- 主题色仍从 `core::theme::current().colors.*` 取；生成头里的主题 hex 只用于校验与 demo。release 专属黄档由生成头提供命名常量（如 `tokens::kReminderYellow`），业务代码引用常量，不写裸 hex。

### 13.2 闸 2：控件映射表（demo 每个元素 → EUI-NEO / 项目现有组件）

| demo 元素 | 实现落点 | 缺口 / 决定 |
|-----------|----------|-------------|
| 侧栏一级 / 二级项、展开箭头 | `app_sidebar.cpp` 自绘 `rect + text` + `ui_kit::selectionBar` | EUI-NEO 无 tree 组件；沿用现有自绘侧栏 |
| 侧栏折叠开关 | 现有自绘 | 不改 |
| 页头标题 / 副标题 / 右侧主操作 | `text` + `row` + `ui_kit::kHeaderH` | 无缺口 |
| 主 / 次 / 文字 / 危险按钮 | `ui_kit::button`（四级） | 禁止自造按钮样式 |
| 块面式 Tab | `ui_kit::wtab` | `components::tabs` 是下划线式，**禁用** |
| 输入框 / 多行文本域 | `components::input` + `.multiline(true)` + `.scrollbar(true)` | 无缺口 |
| 复选框 | `components::checkbox`（启用态） | 无 disabled 属性：锁定态用模块内自绘灰框 + 提示行，不做伪禁用 |
| 开关 | `ui_kit::wswitch` | 与全局微信绿样式一致 |
| 下拉 | `components::dropdown` | 无缺口 |
| 日期 / 时间选择 | `components::datepicker` + `components::timepicker` | 不自绘月历 |
| 模态弹窗 / 全屏配置编辑 | `components::dialog` / `DialogBuilder.screen(...)` | 不新开编辑窗 |
| 卡片底板 | `ui_kit::cardFrame` | 圆角 / 描边 / 阴影同源 |
| 任务瀑布流 | `components::virtualmasonry` | 已验证 API |
| 普通滚动区 | `components::scrollView` | 无缺口 |
| 进度条 | `components::progress` | 无缺口 |
| 提醒全屏窗 | `app::openWindow` + `core::window::requestWindowClose` | 唯一新增独立窗 |
| 空状态 | `ui_kit::emptyState` | 无缺口 |
| Toast | `ui_kit::toast` | 无缺口 |
| 环境分组折叠 | **改为常展开分组**（`stack/rect/text`） | Accordion 缺失；**禁止实现期临时降级** |
| 拖拽排序 | EUI-NEO `onDrag` + 自写排序落点计算 | 无现成 reorder 组件；组合实现，不抽公共组件 |
| 加载占位 / 转圈 | 只用文字「加载中…」 | skeleton / spinner 缺失；不画 spinner |
| 图标 | FontAwesome glyph + `ui::text().icon()` | 码点须在 EUI-NEO 内置字体中（`fa-upload` U+F093） |
| 等宽字体（branch / 单号） | **不用等宽字体**，统一正文字体 | HTML font-family 与 FreeType fallback 不等价，防闸 3 字体差异 |

### 13.3 闸 3：视觉契约表 + 真窗口 probe 数值断言

屏幕坐标约定：页面区原点 = 侧栏右缘；本文「页面坐标」均相对页面区。页面 padding 左/右/下 32、上 0；页头高 40；正文起点 y = 56。

| 元素 | 契约值（逻辑像素） | probe 断言节点 |
|------|--------------------|----------------|
| 侧栏宽 | 展开 208 / 折叠 72 | `app.sidebar` |
| 侧栏导航项 | x=16、h=40、项间距 4、首项 y=116 | `app.sidebar.item.<Page>.hit` |
| 侧栏二级项 | 缩进 16（x=32）、宽=208−32−16=160、h=40、间距 4 | `app.sidebar.child.Release.<id>.hit` |
| 品牌块 | y=32、h=56、logo 48、品牌块→导航间距 28 | `app.sidebar.brand` |
| 页标题 | 24px / 700 / 行高 32、`textPrimary` | `release.page.title` |
| 区块标题 | 18px / 600 / 行高 26 | `release.page.section.*` |
| 按钮 | 高 40、水平 padding 16、字号 14/600、圆角 8 | `release.*.btn.*` |
| 任务卡 | 最小宽 320、列间距 16、垂直间距 16、padding 16、圆角 12、边框 1px `borderDefault` | `release.tasks.card.<id>.bg` |
| 卡标题 / 正文 / 辅助 | 18/600/24、14/400/22（最多 3 行）、12/400/18 | `.title` / `.body` / `.meta` |
| 进度条 | 高 6、圆角 999、底色 `hover`、前景 `primary` | `.progress` |
| 环境 chip | 高 22、圆角 999、字号 11/600 | `.chip.<env>` |
| 加载占位 | 高 40、字号 12/18、`textTertiary`、上间距 16、居中 | `release.tasks.loading` |
| 环境分组 | 圆角 12、边框 1px、头 padding 16/24、体 padding 24、常展开 | `release.exec.env.<id>` |
| 步骤格 | 最小高 86、圆角 10、padding 12、间距 8；后端 4 列 / 前端 3 列 | `release.exec.step.<kind>` |
| 模态弹窗 | 圆角 16、padding 24、标题 18/600、遮罩 `rgba(31,41,55,.45)` | `release.dialog.*` |
| 提醒窗 | 宽 `min(880, 工作区宽−64)`、圆角 16、头部上边 4px 级色、高 ≤ 工作区高−64 | `release.reminder.root` |

- probe 必须跑 **真窗口**（GLFW + OpenGL），至少两个尺寸：1600×1000 与 1000×640；侧栏展开 / 折叠各一遍。
- 每条断言失败打印节点 id、期望值、实际值，非零退出（禁止只打印汇总）。
- 几何断言之外，颜色断言在 light 与 dark 各跑一次（取 `theme::setLight/setDark` 后重 compose）；accent 四档只测 clay 默认档，其余档不纳入本模块门禁。
- 文字换行相关：卡片摘要固定最多 3 行、行高 22，不按测量值自动增高（防 FreeType 与浏览器度量差异）。

### 13.4 闸 4：并排目检

1. probe 用 `--screenshot` 落 PNG（进程内 `glReadPixels`，不依赖系统录屏授权）。为与 demo 同坐标系，probe 以 `setCustomTitlebarMode(false)` 运行，窗口尺寸 = demo 画布尺寸。
2. demo 在浏览器中设成同一尺寸（1600×1000 与 1000×640），截图。
3. 两张图并排比对，逐条记录差异到 `docs/superpowers/plans/2026-10-27-release-module-visual-acceptance.md`。每条差异必须有处置：改实现 / 改 demo / 接受（写明理由）。
4. 只有「接受」项允许留白；其余差异清零才算闸 4 通过。
5. 对比排除非模块差异：系统标题栏 / 窗口阴影 / 字体抗锯齿 / 浏览器滚动条样式。排除项也要在验收单中写明，防「看起来差不多」蒙混。

### 13.5 字体差异处理（HTML vs FreeType）

- demo 与实现使用同一字体栈字符串（`core::theme::fontStack()`），但浏览器与 FreeType 的 fallback、字重、行高度量仍可能不同。
- 因此**所有契约文本必须显式写行高**（px，不用 CSS 无单位行高）；任务卡摘要固定最多 3 行，高度公式只用固定行高 × 行数。
- 对比只要求「框架尺寸 / 字号 / 行高 / 颜色 / 圆角」一致；字形栅格差异、抗锯齿、字间距差异不判红，但必须在验收单登记。
- 若后续发现同一文本在两环境换行位置不同，以**契约行高 + 截断**为准，不改实现去迁就浏览器自动换行。

### 13.6 已知文档漂移（不得作为实现基准）

- `docs/superpowers/specs/2026-10-21-devdesk-design-system-v1.md` 的数值已被代码覆盖：**页面标题实为 24、正文实为 14、卡标题 settings 18 / todo 14**。该文档只作参考。
- `docs/superpowers/demo/wechat-ui-demo.html` 是旧微信绿 `#07C160` 口径，与本项目当前主题（`#36D399`）不一致，**不得作为实现基准**。
- `src/core/theme.h` 的结构体注释里有历史值（如 `fontSizeBody 15`、`radius 10`），实际值以 `src/core/theme.cpp` 为准；本模块不因注释与值不一致去改全局 token（防波及其他模块）。
- 实现基准固定为：本 spec + `release-module-tokens.json` + 代码 token（`theme.cpp` / `ui_kit` 常量）。