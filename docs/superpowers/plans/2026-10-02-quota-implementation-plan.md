# 供应商余量功能 实施计划（2026-10-02）

> 依据 spec：`docs/superpowers/specs/2026-10-02-provider-quota-and-storage-design.md`（rev.3，已过独立审计）
> 执行方式：subagent-driven development（TDD），每任务 实现者 → 独立评审 → fix 轮
> **前置**：批次 E（存储统一）必须先落地——本功能要读 profile token（E 之后 token 在 DB 里），且要新增的两张表走同一套 `core/schema.h` kSql 惯例。

## 0. 任务依赖图与派单顺序

```
E（存储统一，已就绪的简报 batch-E）
      ↓
G0 抽象层  ──┬─→ G1 opencode_go  ┐
             ├─→ G2 minimax_cn    ├─（四个可并行，各自独立文件）
             ├─→ G3 deepseek      │
             └─→ G4 volcano       ┘
      ↓
G5 存储 + 定时器 ──→ G6 配置弹窗 ──→ G7 首页卡片
      ↓
G8 失败韧性 + 集成验收
```

- G1–G4 可并行（不同 `.cpp`，注册表各加一行，但**注册表文件 `quota_registry.cpp` 是共享写者** → 派单时明确：各自只在自己的 provider 文件里实现，注册表那一行由控制器在合并时统一加，或每个 agent 改自己那一行（不同行、不冲突）。**裁决：各自加自己那一行，评审核无重复无遗漏。**
- G0 必须先合（否则四个 provider 无共同契约）。

## 1. 全局约束（每条任务隐含包含）

- 模块隔离红线：`module_home` **不得** include `module_claude_proxy` 任何头；quota 结果表在 `core/schema.h`，home 直读（先例：现有首页卡片读 `claude_call_logs`）；触发查询走 `IClaudeProxyService` + `ServiceLocator`（先例 `main.cpp:277`）。
- 不新增数据库连接：全进程唯一 `sqlite3*`，沿用既有锁纪律（所有句柄操作入锁）。
- 定时器用 **Scheduler 式真后台线程**（`module_agent/scheduler.cpp:128-143` 先例），**不用 UI `onTimer`**（窗口隐藏时整帧不进 compose → 静默停刷）。
- 平台适配接口隔离：零 `#ifdef` 在业务代码；Windows/macOS 分文件。
- 构建只走 `scripts/build-test-run.sh`；Bash timeout ≤3 分钟，长构建 `run_in_background`。
- 测试期间**绝不绑 8888、绝不动 claude 代理开关**（本会话推理链路由它承载）。
- 每个任务的红→绿都要贴失败行；修复类任务额外要「回退即红」证据。
- 报告写到 **worktree 内** `.superpowers/sdd/2026-10-03-quota/report-G<N>.md`（主检出路径会被隔离守卫拒）。

---

## G0 抽象层（TDD）

**Files**
- Create: `src/module_claude_proxy/quota/quota_provider.h`（类型定义）
- Create: `src/module_claude_proxy/quota/quota_registry.h` / `.cpp`（显式 constexpr 注册表）
- Modify: `src/module_claude_proxy/CMakeLists.txt`（加源文件）
- Test: `tests/test_modules/test_claude_quota.cpp`（新建）

**Interfaces（后续四个 provider 只依赖这些签名，逐字实现）**

```cpp
namespace module::claude::quota {

enum class QuotaKind { Tiered, Balance };

enum class QuotaError { None, Auth, Entitlement, Shape, Transient, NotConfigured };

struct QuotaWindow {
    std::string key;        // "five_hour" / "weekly" / "monthly"
    double usedPercent = 0.0;  // 0-100，double 不截断
    std::string resetsAt;      // ISO8601 或空
};

struct QuotaResult {
    QuotaKind kind = QuotaKind::Tiered;
    std::vector<QuotaWindow> windows;
    double balanceRemaining = 0.0;
    std::string currency;
    std::string planName;
    QuotaError error = QuotaError::None;   // 唯一错误通道
    std::string errorMessage;
};

struct ProviderField { std::string key; std::string label; bool secret = false; };

// HTTP 注入缝：照 module_agent/llm_client.h:22-25 的函数指针范式
struct HttpSendFn;  // 具体签名对齐 platform::http::send(const Request&) 的可注入包装

class QuotaProvider {
public:
    virtual ~QuotaProvider() = default;
    virtual std::string id() const = 0;
    virtual std::string displayName() const = 0;
    virtual std::vector<ProviderField> fields() const = 0;
    virtual QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) = 0;
};

struct QuotaProviderEntry { const char* id; QuotaProvider* (*create)(); };
extern const QuotaProviderEntry kQuotaProviders[];   // 显式表，非静态自注册
const QuotaProvider* findProvider(const std::string& id);
const QuotaProvider* detectByBaseUrl(const std::string& baseUrl);  // 可选，默认选中用

}  // namespace
```

**为什么显式表不用静态自注册**：静态库不拉未引用 TU，自注册构造函数不会跑，注册表会「神秘地少一个 provider」且无编译错误。仓库既有模式是显式 constexpr 表（`app_registry.h:42`）。

**TDD 步骤**
1. 失败用例：`findProvider("opencode_go")` 在注册表有该行时返回非空、未知 id 返回 nullptr、重复 id 断言失败；`QuotaResult` 默认值语义；`fetch` 契约注释存在。
2. 跑 `bash scripts/build-test-run.sh --filter claude_quota --no-run` 确认红。
3. 实现最小注册表 + 一个 **FakeProvider**（测试用，验证 `fields()` 驱动渲染、空 fields、secret 标记）。
4. 绿。
5. 变异：删注册表一行 → 用例红。

---

## G1 OpenCode Go（TDD）

**Files**: Create `src/module_claude_proxy/quota/provider_opencode_go.{h,cpp}`；Modify 注册表；Test `tests/test_modules/test_claude_quota.cpp` 增段。

**口径**（spec §3.1）
- `GET https://opencode.ai/zen/go/v1/usage`，`Authorization: Bearer {cfg.apiKey}`（**不是** `x-api-key`）
- 响应：`{"usage":{"rolling":{"status":"ok","percent":38,"resetsAt":"..."},"weekly":{...},"monthly":{...}}}`
- 窗口映射：`rolling`→`five_hour`、`weekly`→`weekly`、`monthly`→`monthly`（**按 key 名映射**）
- `percent == 0` → `resetsAt` 是占位值 → 清空 `resetsAt`
- `percent` 小数不截断（`usedPercent` 是 double）
- HTTP 403 → `QuotaError::Entitlement`（「key 有效但该 workspace 无 Go 订阅」）；401 → `Auth`；其它 4xx/5xx → `Transient`
- 响应缺 `usage` 或窗口结构不认识 → `QuotaError::Shape`（**不返回空卡**）
- base_url 探测：命中 `opencode.ai/zen/go` 与 `/zen/go/v1` 两档；**不命中** `opencode.ai/zen/v1`（按量版，实测 404）

**必测 fixture**（逐条来自 cc-switch 记录的真实坑）
1. 三窗口齐全正常响应 → 3 个窗口、key 序固定、`usedPercent` == percent
2. `percent == 0` → 窗口保留、`resetsAt` 为空
3. 小数 percent（38.7）→ 断言值 == 38.7 **不被截断**
4. HTTP 403 → `Entitlement`；401 → `Auth`
5. 旧扁平形态（`rollingUsage/usagePercent/resetInSec`）→ `Shape`
6. `detectByBaseUrl` 两档命中、`/zen/v1` 不命中

---

## G2 MiniMax 中国站（TDD）

**Files**: Create `provider_minimax_cn.{h,cpp}`。

**口径**（spec §3.3）
- `GET https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains`（**旧域**，即使 profile base_url 是 `api.minimax.cn`；同账号同 key）
- `Authorization: Bearer`
- 响应 `model_remains[]` 取 `model_name == "general"`；业务错误在 `base_resp.{status_code,status_msg}`（**HTTP 200 也可能是失败**）
- 五小时：`current_interval_remaining_percent` → **反转** `used = 100 - remain`，重置 `end_time`
- 周：`current_weekly_status == 1` 才渲染，否则**不出周窗口**（不画 0%）
- **无月窗口**
- `fields()` 返回空（沿用 profile 的 api key）
- base_url 探测：host 标签边界匹配（防 `api.minimax.cn.example.com` 伪造），照 cc-switch `codex_url_host_matches_any`

**必测 fixture**
1. 正常响应 → 五小时窗口 `used == 100 - remain`（**反转是唯一符号级错误源，必测**）
2. `current_weekly_status != 1` → 只有五小时窗口
3. `base_resp.status_code != 0` 且 HTTP 200 → `Transient` 或 `Auth`（按码值定，写清依据）
4. `model_remains` 里没有 `general`（只有 video）→ `Shape`
5. 探测：两个 host 都命中，伪造后缀不命中

---

## G3 DeepSeek（Balance）（TDD）

**Files**: Create `provider_deepseek.{h,cpp}`。

**口径**（spec §3.4）
- `GET https://api.deepseek.com/user/balance`，`Authorization: Bearer`
- `balance_infos[].{currency,total_balance,...}`、`is_available`
- → `QuotaResult{kind=Balance, balanceRemaining=total_balance, currency}`
- `is_available == false` → 「余额不足」（`Entitlement`）
- **无窗口、无重置** → `windows` 必须为空（不许硬造一个窗口）

**必测 fixture**：正常多币种 / 空 `balance_infos` → `Shape` / `is_available=false` / 币种原样保留（不换算）

---

## G4 火山方舟（TDD）

**Files**: Create `provider_volcano.{h,cpp}`。

**口径**（spec §3.2）
- 控制面 `POST https://open.volcengineapi.com/?Action=<A>&Region=<r>&Version=2024-01-01`
- **签名 V4（AK/SK）**，三处偏离标准 SigV4（**签名是本批最易错处**）：
  1. canonical headers 固定序 `host;x-date;x-content-sha256;content-type`（不按字母序）
  2. algorithm 串 `HMAC-SHA256`（**无 `AWS4` 前缀**）
  3. credential scope 结尾 `request`（非 `aws4_request`），`kDate = HMAC(SK, date)`（SK 不加 AWS4 前缀）
- canonical query 按 key 字母序；service = `ark`；空 body
- 先 `GetAFPUsage`（Agent Plan）→ `Result.AFPFiveHour/AFPWeekly/AFPMonthly.{Quota,Used,ResetTime(ms)}`，`Quota <= 0` 跳过；未命中再 `GetCodingPlanUsage` → `Result.QuotaUsage[]`，`Level ∈ {session,weekly,monthly}` + `Percent` + `ResetTimestamp`(秒)
- Region 从 base_url host 段取 `cn-*` / `ap-*`，默认 `cn-beijing`
- `AFPDaily` 跳过；DouBaoSeed 按量（`/api/v3`）不命中
- 错误分类 Auth / Soft / Transient；鉴权错误码子串匹配 `auth/signature/accessdenied/denied/unauthorized/forbidden/credential/token`
- `fields()` 返回 `{accessKeyId(secret=false), secretAccessKey(secret=true)}`

**必测 fixture**
1. **签名结构契约**（无服务端金标准向量，只锁结构）：固定 headers 顺序、algorithm 串无 AWS4、scope 以 `request` 结尾、canonical query 排序 —— 给定固定 AK/SK/时间/请求，断言签名串**逐字节**等于预期值
2. AFP 三窗口正常
3. `Quota <= 0` → 该窗口跳过
4. Coding Plan `Level` 映射（**按 Level 不按 ResetTimestamp**）
5. 鉴权错误码 → `Auth`，且错误文案明确指向「AK/SK 或签名」
6. Region 提取：两个 host 段各一例、缺省一例
7. 探测：`volces.com/api/plan` 与 `/api/coding` 命中、`/api/v3` 不命中

---

## G5 存储 + 定时刷新（TDD）

**Files**: Modify `src/core/schema.h`（kSql 加两表）；Create `src/module_claude_proxy/quota/quota_store.{h,cpp}`、`.cpp` 定时器；Test `tests/test_modules/test_claude_quota.cpp` + `test_schema.cpp` 逐列断言。

**表**：spec §4.1 已给完整 DDL，照抄。**没有 migration 机制**（`core/schema.h:4-20` 明写，且 `test_schema.cpp:126` 断言 `schema_version` 表不存在）。

**定时器**
- Scheduler 式后台线程（照 `module_agent/scheduler.cpp:128-143` 的 sleep 切片 tick）
- 逐 profile：到点 → `fetch` → 落库 → 请求 UI 收割（**先就绪后唤醒**，照 `image_source.cpp:949` / `network.cpp:250-257`，别踩 D 批次那个竞态）
- 默认 `interval_minutes = 15`，`enabled` / `auto_refresh` 可关
- app 退出时线程可中断退出，不挂死

**必测**：两表 CRUD / schema 逐列断言 / 定时器到点触发（用可注入时钟或短间隔测试配置，**不要真等 15 分钟**）/ 落库结果被 home 直读 / 线程退出不挂死

---

## G6 配置弹窗（TDD）

**Files**: Modify `src/module_claude_proxy/claude_ui_detail.cpp`（加「余量查询」按钮；**必须在批次 C 合并后**做——C-3 改了同一动作行）、`claude_ui_form.*` 或新 `quota/quota_dialog.{h,cpp}`；Test `test_claude_ui.cpp` + `claude_probes.cpp`。

- 按钮**常驻**动作行（用户明确要求：不管什么供应商都有）
- 弹窗：`ui_kit::DialogFrame` 范式
  1. 先选供应商类型下拉（opencode go / 火山 Coding Plan / deepseek / minimax cn / 不配置）
  2. 选定后按 `fields()` **动态渲染**：火山 → AccessKey ID + Secret（secret 输入框）；其余 → 无额外项
  3. `[测试连接]` → 真发一次 `fetch`，结果/错误直接显示在弹窗内
  4. `[保存]` / `[取消]`
- 动作行几何：按钮数变化要同步 gap/宽度计算（照 C-3 的 `(w-36)/N` 惯例），并更新既有断言

**必测**：字段集合按 provider 变化（**钉死具体 key 集合与 secret 类型，不能拿 `fields()` 自己当期望值**——那是自证）/ 测试连接错误可见 / 保存后配置表落库 / 动作行几何不错位（probe）

---

## G7 首页卡片（TDD）

**Files**: Modify `src/module_home/*`；Test `tests/test_modules/test_home.cpp` + `tests/probes/home_probes.cpp`。

- **全宽区块**放在统计网格**下方**（审计实测：1200 宽下统计卡 `statW ≈ 135px` × 144px 高，放不下多窗口进度条，**第 6 张统计格几何不可行**）
- 每个已配置 profile 一行：名称 + 供应商名 + 状态
- Tiered：每窗口一条 `components::progress` 线性条（EUI-NEO 无环形原语）；语义 = 已用百分比；色带 ≥90 红 / ≥70 橙 / 其余绿；有 `resetsAt` 标倒计时
- Balance：一行金额条 + 币种
- 右上 `[刷新]`
- **五态**（spec §5.1）：未配置（去配置）/ 配置不完整（NotConfigured，不画 0%）/ 正常 / 失败但有过成功值（旧条 + 「可能过期」）/ 失败且从未成功（**只渲染错误行，绝不画 0% 条**）
- 高度自适应 + `scrollView` 兜底

**必测**：五态各一例（每态断言渲染了什么、没渲染什么）/ 全宽不挤压既有统计卡（probe 几何）/ 倒计时格式

---

## G8 失败韧性 + 集成验收

- `Transient` → 保留上次成功值 + 「可能过期」角标
- `Auth` / `Entitlement` / `NotConfigured` / `Shape` → 立即透出，不掩盖，不重试风暴
- 集成路径用例：配置 provider → 定时器跑一轮 → 结果落库 → 首页卡片渲染出对应状态
- **真机出网只测 opencode go**（用真实 profile 手测一次，**测试期间绝不碰代理开关**）；火山若有账号必须真测并写明「签名对不对」，没有账号则明确标注「未测，签名正确性未验证」

---

## 2. Review Focus（spec 推演出的、任务文本没直说的失败模式）

按最可能咬人排序，每条在下面标了归属任务——**写测试时必须覆盖**：

1. **量纲归一化方向搞反**（G1/G2）：把「剩余 %」当「已用 %」用，UI 显示全错且测试若写成「范围 0-100」恒真 → 断言具体数值（归属 G1/G2）
2. **窗口分类靠 resetsAt 排序**（G1/G4）：周期末尾周桶比 5h 桶更早重置 → 必须按显式字段映射（归属 G1/G4）
3. **从未成功过却画 0% 条**（G7）：用户看到「还有 100% 可用」的假象 → 空结果必须只渲染错误行（归属 G5/G7）
4. **凭据缺失落进「失败」而非「未配置」**（G6/G7）：用户不知道该改什么 → 状态枚举必须分开（归属 G5/G7）
5. **火山签名三处偏离**（G4）：错一处就是 400，且错误文案指向不明 → 结构契约 + 文案分流（归属 G4）
6. **窗口遮罩盖住元信息**（G6）：这条在批次 A 已经出现过一次（弹窗内下拉展开盖住下方行），同类问题在配置弹窗会重演 → probe 必须覆盖展开态（归属 G6）
7. **定时器不刷新**（G5）：用 `onTimer` 而非后台线程，窗口隐藏时静默停刷 → 归属 G5
8. **Data 根进 git 带出凭据**（归属批次 E，但 G5 新表同处一库）→ E 的 `.gitignore` 与权限处置必须覆盖新表（归属 E + G5 交叉，合并时复核）

## 3. 执行纪律

- 每个任务：worktree 隔离 → 实现者（TDD，红先行）→ 独立评审（实现者 ≠ 评审者）→ fix 轮 → 合并 dev
- **worktree 基点校验**（这轮四批全踩过）：派单时写死「先 `git merge-base dev HEAD`，落后先 `git merge --ff-only dev`」
- 合并后全量 + 安装 + 真机点验；余量功能涉及出网，**绝不绑 8888 / 不动 claude 代理开关**
