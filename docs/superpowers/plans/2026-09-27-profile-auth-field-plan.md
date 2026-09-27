# Profile「认证字段」（鉴权头三态）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 DevDesk 的 Claude Profile 增加「认证字段」配置（`both` / `bearer` / `api_key`），让代理转发按 profile 决定往上游注入哪个鉴权头。

**Architecture:** DB 加一列 `claude_profiles.auth_field`（TEXT，DEFAULT `'both'`，migration v7）；`Profile` 存该列原文、使用点经 `provider_config` 的 `AuthField` 枚举纯函数解析/规范化；鉴权头注入收敛在既有唯一注入口 `UpstreamDialect::applyHeaders`（新增第三参），流式 / 非流式 / 连通测试三处传参；UI 在表单「基础」页加一个三选项下拉；粘贴导入按键名证据自动选。

**Tech Stack:** C++20 / CMake + Ninja + MSYS2 MinGW64 + ccache（本仓 `scripts/build-test-run.sh`）/ SQLite / 自制迷你 doctest（`REGISTER_TEST` + `DOCTEST_CHECK*`，**不是**真 doctest）。

**Spec:** `docs/superpowers/specs/2026-09-27-profile-auth-field-design.md`（**V2**，commit 见其 git log）—— plan 与 spec 一起读；两者冲突时**以 spec 为准**并回报。

## Global Constraints

- **执行环境**：本项目规定「涉及编译/测试的子任务必须 worktree 隔离开发，主工作树禁跑构建」；完成后在 worktree 内原子 commit（显式 pathspec，禁 `git add -A`）并报 sha。
- **命令**：构建 + 定向测试一律 `bash scripts/build-test-run.sh --no-run --filter <子串>`（脚本自动处理工具链 PATH 与 ccache；worktree 首建会自动 configure）。**禁全量重建、禁全量套件**。
- 测试二进制支持子串过滤：`./build/tests/devdesk_tests.exe <子串>`；本计划的过滤串用：`dialect` / `provider_config` / `claude_service` / `claude_ui` / `migration`。
- **测试框架是自制迷你 doctest**：`REGISTER_TEST(name, ...)` 静态自注册 + `DOCTEST_CHECK` / `DOCTEST_CHECK_EQ`（fail-fast）。`REGISTER_TEST` 宏实参**不能有裸逗号**（模板实参、`{a, b}` 初始化列表都算）→ 用类型别名或逐项构造。grep `TEST_CASE` 找不到用例是正常的。
- **不抛 C++ 异常**：错误一律 `core::Result<T>`（`DevDesk/src/core/result.h`）。`-Wall -Wextra -Werror` 全开。
- **模块隔离**：`module_*` 之间不可互依赖；`core` 不依赖 `platform` / EUI-NEO。
- 代码注释与 UI 文案**一律中文**；行尾 LF。
- 范围纪律：**只做 spec §7 清单内的改动**，不做顺手重构 / 顺手修 bug / 清理死代码。
- commit 粒度：每个 Task 完成后一次原子 commit；消息用本计划的给定文案。

## 文件结构（改动落点一览）

| 文件 | 职责 | 动作 |
|------|------|------|
| `src/core/migrations.h` | DB schema 版本化迁移 | 加 v7 |
| `src/module_claude_proxy/provider_config.h` / `.cpp` | 配置语义纯函数（枚举 / 文本互转 / 导入解析） | 加 `AuthField` + 两 helpers + `ImportedConfig.authFieldFromEnv` |
| `src/module_claude_proxy/claude_service.h` / `.cpp` | Profile CRUD（**三处 SELECT** / INSERT / 两条 UPDATE / 返回值 / 复制） | 加列读写 |
| `src/module_claude_proxy/upstream_dialect.h` / `.cpp` | 上游鉴权头注入唯一收敛点 | 加第三参 + 三态分支 |
| `src/module_claude_proxy/proxy_router.cpp` / `streaming_proxy.cpp` / `claude_service.cpp` | 三处调用点 | 传参 |
| `src/module_claude_proxy/claude_module.cpp` | 表单 UI 与四条生命周期路径 | 加下拉 + 状态 |
| `tests/...`（5 个文件） | 用例 | 加/改用例 |

## 执行方式（本计划的批处理口径）

Task 1 与 Task 2 **有交叠文件**（`claude_service.cpp` 在 Task 1，`claude_module.cpp` 在 Task 2 但依赖 Task 1 的 `AuthField` 与 `Profile::authField`），且整条改动是**引用点已查清、无需设计判断的机械改动** → 按 `DevDesk/CLAUDE.md`「机械任务合并…粒度为复杂度而非文件数」：**同一 worktree 内由一条实施线串行完成 Task 1 → Task 2**，末尾派一次独立复核（对照 spec §7 清单逐行核 diff + 复跑定向用例）。

---

## Task 1: 后端链路（迁移 + CRUD + 方言层 + 导入联动）

**Files:**
- Modify: `src/core/migrations.h`
- Modify: `src/module_claude_proxy/provider_config.h` / `provider_config.cpp`
- Modify: `src/module_claude_proxy/claude_service.h` / `claude_service.cpp`
- Modify: `src/module_claude_proxy/upstream_dialect.h` / `upstream_dialect.cpp`
- Modify: `src/module_claude_proxy/proxy_router.cpp` / `streaming_proxy.cpp`
- Test: `tests/test_modules/test_provider_config.cpp` / `test_upstream_dialect.cpp` / `test_claude_service.cpp` / `tests/test_core/test_migrations.cpp`

**Interfaces:**
- Produces（Task 2 依赖）：
  - `module::claude::AuthField`：`enum class AuthField { Both, Bearer, ApiKey };`（`provider_config.h`）
  - `AuthField authFieldFromText(const std::string&);` / `const char* authFieldToText(AuthField);`
  - `Profile::authField`：`std::string`，规范值为 `"both"` / `"bearer"` / `"api_key"`
  - `ClaudeService::createProfile(..., bool activateIfNoneActive = false, int64_t sharedConfigId = 0, const std::string& authField = "both")`
  - `void UpstreamDialect::applyHeaders(std::map<std::string,std::string>&, const std::string& apiKey, AuthField) const;`
  - `ImportedConfig::authFieldFromEnv`：`std::string`，取值 `"api_key"` / `"bearer"` / `"both"` / `""`（空 = 无证据，不覆盖表单）

- [ ] **Step 1: 迁移 v7 + 三处既有断言**

在 `src/core/migrations.h` 的 `kClaudeSharedConfigs`（v6 块）之后新增：

```cpp
// Profile 认证字段（version 7，2026-09-27 profile-auth-field 设计 §3）
//   - 三态：'both'（默认，双发 x-api-key + Authorization: Bearer）/ 'bearer' /
//     'api_key'——代理转发按 profile 的该字段决定注入哪个鉴权头
//   - 读侧对未知值 / 空串兜底为 'both'（auth_field_from_text）
// 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证
// 只执行一次（runMigrations 事务内，失败整体回滚）。
inline const Migration kClaudeProfileAuthField = {
    .version = 7,
    .description = "claude profiles add auth field (both/bearer/api_key)",
    .sql = R"(
        ALTER TABLE claude_profiles ADD COLUMN auth_field TEXT NOT NULL DEFAULT 'both';
    )"
};
```

在 `all()` 列表末尾（`kClaudeSharedConfigs` 之后）加 `kClaudeProfileAuthField,`。

同步改三处既有断言（否则必红）：
- `tests/test_core/test_migrations.cpp:210`、`:291`：`DOCTEST_CHECK_EQ(db.schemaVersion(), 6);` → `7`
- `tests/test_modules/test_provider_config.cpp:380`：同上 → `7`

- [ ] **Step 2: `AuthField` 枚举与两个纯函数**

`src/module_claude_proxy/provider_config.h`（放在 `parseExtraEnv` 声明附近）：

```cpp
// ---- 认证字段（鉴权头三态；2026-09-27 profile-auth-field 设计 §3/§4）----
// both    = 双发 x-api-key + Authorization: Bearer（默认，两种网关都兼容）
// bearer  = 只发 Authorization: Bearer
// api_key = 只发 x-api-key
enum class AuthField { Both, Bearer, ApiKey };

// 文本 → 枚举：'bearer' / 'api_key' / 其余（'both'、未知、空串）→ Both（设计 D-8 读侧兜底）
AuthField authFieldFromText(const std::string& text);

// 枚举 → 文本：规范值（写侧规范化用，保证库里只有三个规范值之一）
const char* authFieldToText(AuthField field);
```

`src/module_claude_proxy/provider_config.cpp`（放在 `applyEnvAliases` 之后即可）：

```cpp
AuthField authFieldFromText(const std::string& text) {
    if (text == "bearer") return AuthField::Bearer;
    if (text == "api_key") return AuthField::ApiKey;
    return AuthField::Both;   // 'both' / 未知 / 空串
}

const char* authFieldToText(AuthField field) {
    switch (field) {
        case AuthField::Bearer: return "bearer";
        case AuthField::ApiKey: return "api_key";
        case AuthField::Both: break;
    }
    return "both";
}
```

- [ ] **Step 3: `applyHeaders` 第三参 + 三态分支 + 三个调用点**

`src/module_claude_proxy/upstream_dialect.h`：现有 include 只有 `<map>` / `<string>`，`AuthField` 定义在 `provider_config.h` → 补 `#include "provider_config.h"`（同模块，**先确认无头文件环依赖**：`provider_config.h` 不得 include `upstream_dialect.h`；若成环，改用 `enum class AuthField : uint8_t;` 前向声明 + 在 `.cpp` 内 include）。签名改为：

```cpp
    // 就地把请求头调整成该方言期望的形态：
    //   - 鉴权：清掉客户端送来的**所有大小写变体**凭据，再按 authField 注入：
    //     both → x-api-key + Authorization: Bearer；bearer → 只 Authorization；
    //     api_key → 只 x-api-key；空 key 一律不注入
    //   - User-Agent：无条件覆盖为 kProxyUserAgent（见其注释）
    //   - anthropic-version：客户端没带时补默认值
    //   - x-opencode-session（仅 OpencodeZen）：客户端没带时注入代理实例级 id；
    //     客户端已带则不覆盖（尊重其会话亲和意图）
    void applyHeaders(std::map<std::string, std::string>& headers,
                      const std::string& apiKey, AuthField authField) const;
```

`src/module_claude_proxy/upstream_dialect.cpp` 的 `applyHeaders` 第 1 段改成（其余三段 UA / anthropic-version / 方言专属头**一字不动**）：

```cpp
    // 1. 鉴权：客户端的凭据一律丢弃（代理只用 profile 的 key，客户端那份
    //    打到这个网关本来就无效），再按认证字段注入
    eraseHeaderCI(headers, "x-api-key");
    eraseHeaderCI(headers, "authorization");
    if (!apiKey.empty()) {
        if (authField != AuthField::Bearer) {
            headers["x-api-key"] = apiKey;
        }
        if (authField != AuthField::ApiKey) {
            // 兼容期望 Bearer 的 Anthropic-compatible 网关（bailian 等）；
            // 真 Anthropic API 收 x-api-key、忽略 Bearer
            headers["Authorization"] = "Bearer " + apiKey;
        }
    }
```

三个调用点各加第三参 `authFieldFromText(profile.authField)`（实参保持原样）：
- `src/module_claude_proxy/proxy_router.cpp:237`：`dialect.applyHeaders(fwd, apiKey, authFieldFromText(profile.authField));`
- `src/module_claude_proxy/streaming_proxy.cpp:332`：`dialect.applyHeaders(upstream.headers, profile.apiKeyPlain, authFieldFromText(profile.authField));`
- `src/module_claude_proxy/claude_service.cpp:1103`（`testConnection`）：`dialect.applyHeaders(req.headers, key, authFieldFromText(profile.authField));`

**既有测试的调用点**（`tests/test_modules/test_upstream_dialect.cpp` 的 `dialect_apply_headers_default` / `dialect_apply_headers_client_variants` / `dialect_apply_headers_opencode_session` 等）会因缺参编译失败——这是**有意的**（不设默认参数就是为了不漏改），逐个补 `AuthField::Both`。

- [ ] **Step 4: Profile 与 CRUD 落列（含三处 SELECT 与返回值）**

`claude_service.h`：

```cpp
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
```

`createProfile` 追加尾参（`sharedConfigId` 之后）：

```cpp
                                       int64_t sharedConfigId = 0,
                                       const std::string& authField = "both");
```

`claude_service.cpp`（**注意 spec §7 标粗的三处，漏一处就是"功能假生效"**）：

1. **三处 SELECT 列清单**都加 `auth_field`（`listProfiles` `:159-163`、`getProfile` `:189-193`、**`getActiveProfile` `:614-618`**）：把 `"shared_config_id, is_active, created_at, updated_at "` 改成 `"shared_config_id, auth_field, is_active, created_at, updated_at "`。
2. `rowToProfile`（`:118-154`）加：

```cpp
    auto authField = row.getString("auth_field");
    // 读侧规范化：未知/空值一律兜底 both（设计 D-8 双保险之一）
    if (authField.isOk()) {
        p.authField = authFieldToText(authFieldFromText(authField.value()));
    }
```

3. `createProfile`（`:217` 起）：函数体开头加规范化局部量

```cpp
    const std::string cleanedAuthField = authFieldToText(authFieldFromText(authField));
```

INSERT 列清单与占位符改为（`shared_config_id` 后插 `auth_field`）：

```cpp
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, keychain_account, "
        " model, shared_config_id, auth_field, is_active, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?) RETURNING id");
```

对应 bind：在 `shared_config_id` 与 `is_active`(字面 0) 之间插入 `stmt.bind(<该位序>, cleanedAuthField);`（**按实际 bind 序调整后续编号**，现有 bind 是 1..7 顺序绑定，插入后 `created_at` / `updated_at` 的编号各 +1）。

4. **`createProfile` 返回值构造（`:376-389`）** 补一行（否则二段补写会静默覆盖用户选择）：

```cpp
    out.sharedConfigId = sharedConfigId;
    out.authField = cleanedAuthField;   // 必须带上：二段补写以本返回值为基
    out.isActive = autoActivated;
```

5. `updateProfile`（`:392` 起）：函数体开头加 `const std::string cleanedAuthField = authFieldToText(authFieldFromText(profile.authField));`；两条 UPDATE 都加列与 bind：
   - 换 key 分支（`:449-454`）：`"... extra_env = ?, shared_config_id = ?, auth_field = ?, updated_at = ? WHERE id = ?"`（bind 顺延）
   - 不换 key 分支（`:481-484`）：同款
6. `duplicateProfile`（`:707-760`）：`createProfile(...)` 调用追加第 7 实参 `src.authField`。

- [ ] **Step 5: 导入联动（`ImportedConfig.authFieldFromEnv`）**

`provider_config.h` 的 `ImportedConfig` 加：

```cpp
    // 粘贴里两个键名的证据（设计 §5）：'api_key'（只出现 ANTHROPIC_API_KEY）/
    // 'bearer'（只出现 ANTHROPIC_AUTH_TOKEN）/ 'both'（同现）/ ""（都没出现
    // = 无证据，UI 不覆盖表单现值）
    std::string authFieldFromEnv;
```

`provider_config.cpp` 的 `parseImportedEnv`：在函数返回前（`sawApiKey` / `sawAuthToken` 已知处）加

```cpp
    // 认证字段证据（设计 §5）：键名 = 该配置原本以哪种形态发送且可用
    if (sawApiKey && sawAuthToken) {
        out.authFieldFromEnv = "both";
    } else if (sawApiKey) {
        out.authFieldFromEnv = "api_key";
    } else if (sawAuthToken) {
        out.authFieldFromEnv = "bearer";
    }
```

**边界（必守）**：`parseEnvForReverseSync`（反向同步解析）**不得**跟着填这个字段——spec §5 明确反向同步不改。若两函数共用代码路径，用参数/分叉保证只有导入路径填。

- [ ] **Step 6: 测试**

`tests/test_modules/test_provider_config.cpp` 加两条：

```cpp
REGISTER_TEST(provider_config_auth_field_text_roundtrip, {
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::Both)), AuthField::Both);
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::Bearer)), AuthField::Bearer);
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::ApiKey)), AuthField::ApiKey);
    DOCTEST_CHECK_EQ(authFieldFromText("x"), AuthField::Both);
    DOCTEST_CHECK_EQ(authFieldFromText(""), AuthField::Both);
});
```
（`REGISTER_TEST` 的调用形态照该文件既有写法；宏实参不能有裸逗号。）

`provider_config_parse_import_auth_field_evidence`：四态各一条——`{"ANTHROPIC_API_KEY":"k"}` → `"api_key"`；`{"ANTHROPIC_AUTH_TOKEN":"k"}` → `"bearer"`；两键同现 → `"both"`；`{"ANTHROPIC_BASE_URL":"https://x"}` → `""`。

`tests/test_modules/test_upstream_dialect.cpp` 加 `dialect_apply_headers_auth_field_three_states`：

```cpp
    const std::string key = "sk-test";
    std::map<std::string, std::string> both, bearer, apiKeyOnly, emptyKey;
    const auto dialect = UpstreamDialect::anthropicDefault();
    dialect.applyHeaders(both, key, AuthField::Both);
    dialect.applyHeaders(bearer, key, AuthField::Bearer);
    dialect.applyHeaders(apiKeyOnly, key, AuthField::ApiKey);
    dialect.applyHeaders(emptyKey, "", AuthField::Both);
    // both：两个头都在
    DOCTEST_CHECK(both.count("x-api-key") == 1);
    DOCTEST_CHECK(both.count("Authorization") == 1);
    // bearer：只有 Authorization（负断言是核心）
    DOCTEST_CHECK(bearer.count("x-api-key") == 0);
    DOCTEST_CHECK(bearer.count("Authorization") == 1);
    // api_key：只有 x-api-key
    DOCTEST_CHECK(apiKeyOnly.count("x-api-key") == 1);
    DOCTEST_CHECK(apiKeyOnly.count("Authorization") == 0);
    // 空 key：两个都不注入
    DOCTEST_CHECK(emptyKey.count("x-api-key") == 0);
    DOCTEST_CHECK(emptyKey.count("Authorization") == 0);
    // UA 与上游契约头不受影响
    DOCTEST_CHECK(bearer.count("User-Agent") == 1);
    DOCTEST_CHECK(bearer.count("anthropic-version") == 1);
```
再加一条 `dialect_apply_headers_auth_field_clears_client_creds`：三态各自预置客户端 `x-api-key` / `authorization`（大小写变体）→ 断言被清（bearer 态下客户端 x-api-key 必须消失）。

`tests/test_modules/test_claude_service.cpp`（照该文件既有 `TestDb` + `kTestKeychainService` 模式）：
- `claude_service_auth_field_default_and_roundtrip`：不传参 → `getProfile().authField == "both"`；显式传 `"bearer"` → `getProfile` / **`getActiveProfile`（激活后）** 都读到 `"bearer"`（**这条是"漏改 SELECT 列静默失效"的回归闸，必须有**）。
- `claude_service_auth_field_update_both_branches`：①`p.apiKeyPlain` 非空 ②留空，两条 UPDATE 分支各改一次该字段并断言。
- `claude_service_auth_field_survives_two_stage_update`：`createProfile(..., "bearer")` → 取返回值构 `Profile` 补 `modelsConfigJson` 后 `updateProfile` → 断言仍 `"bearer"`（**防二段补写覆盖**）。
- `claude_service_duplicate_keeps_auth_field`：源为 `"api_key"` → 副本断言 `"api_key"`。
- `claude_service_auth_field_invalid_normalized`：直接构造 `Profile` 并 `p.authField = "X"` → `updateProfile` → 读回 `"both"`（写侧规范化）。

`tests/test_core/test_migrations.cpp`：Step 1 已改的两处断言随本 Task 一起绿。

- [ ] **Step 7: 定向构建与测试**

```bash
bash scripts/build-test-run.sh --no-run --filter dialect
bash scripts/build-test-run.sh --no-run --filter provider_config
bash scripts/build-test-run.sh --no-run --filter claude_service
```
Expected: 全绿；若 `--filter` 一次跑不全（用例名前缀不同），分别补跑 `migration`。

- [ ] **Step 8: Commit**

```bash
git add src/core/migrations.h src/module_claude_proxy/provider_config.h src/module_claude_proxy/provider_config.cpp \
        src/module_claude_proxy/claude_service.h src/module_claude_proxy/claude_service.cpp \
        src/module_claude_proxy/upstream_dialect.h src/module_claude_proxy/upstream_dialect.cpp \
        src/module_claude_proxy/proxy_router.cpp src/module_claude_proxy/streaming_proxy.cpp \
        tests/test_modules/test_provider_config.cpp tests/test_modules/test_upstream_dialect.cpp \
        tests/test_modules/test_claude_service.cpp tests/test_core/test_migrations.cpp
git commit -m "feat(claude): Profile 认证字段（鉴权头三态）后端链路 —— 迁移 v7 + CRUD 落列 + 方言层三态 + 导入证据"
```

---

## Task 2: 表单 UI + CHANGELOG

**Files:**
- Modify: `src/module_claude_proxy/claude_module.cpp`
- Modify: `CHANGELOG.md`
- Test: `tests/test_modules/test_claude_ui.cpp`

**Interfaces:**
- Consumes（Task 1 产出）：`AuthField` / `authFieldFromText` / `authFieldToText`、`Profile::authField`、`createProfile` 第 7 尾参、`ImportedConfig::authFieldFromEnv`
- Produces：表单状态 `AuthField formAuthField_` + `bool formAuthFieldOpen_`

- [ ] **Step 1: 表单状态与标签常量**

`claude_module.cpp` 表单状态块（`formKey_` 附近，约 `:2564-2575`）加：

```cpp
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
```

文件内（与其他 `constexpr` 标签数组同区，如 `kCallsStatusLabels` 附近）加：

```cpp
// 认证字段下拉选项（**顺序必须与 AuthField 枚举声明一致**：Both/Bearer/ApiKey）
constexpr const char* kAuthFieldLabels[] = {
    "双发（默认）", "只发 Authorization: Bearer", "只发 x-api-key"};
constexpr int kAuthFieldCount = 3;
```

- [ ] **Step 2: 基础页新增一行（含既有三块下移 64px）**

在 `drawFormBasicTab` 的 API Key 输入框（y=262，高 40）之后插入：

```cpp
        // 认证字段（鉴权头三态；2026-09-27 profile-auth-field 设计 §6）：
        // 标签与灰字说明同行，下拉紧随其后；其下三块（副提示/错误/接管警告）
        // 整体下移 64px 让位
        ui.text("claude.form.basic.authfield.label")
            .position(28.0f, 314.0f).size(72.0f, 16.0f)
            .text("认证字段").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.basic.authfield.hint")
            .position(104.0f, 314.0f).size(w - 132.0f, 16.0f)
            .text("网关只接受一种鉴权头时切换；默认两种都发")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.stack("claude.form.basic.authfield.ddwrap")
            .position(28.0f, 332.0f)
            .size(w - 56.0f, 40.0f)
            .content([&] {
                components::dropdown(ui, "claude.dialog.form.authfield.dd")
                    .size(w - 56.0f, 40.0f)
                    .theme(themeTokens)
                    .items(std::vector<std::string>(
                        kAuthFieldLabels, kAuthFieldLabels + kAuthFieldCount))
                    .selected(static_cast<int>(formAuthField_))
                    .open(formAuthFieldOpen_)
                    .onOpenChange([this](bool v) { formAuthFieldOpen_ = v; })
                    .onChange([this](int idx) {
                        formAuthField_ = static_cast<AuthField>(
                            std::clamp(idx, 0, kAuthFieldCount - 1));
                    })
                    .build();
            })
            .build();
```

既有三块 y 值 +64：副提示 `316.0f` → `380.0f`（icon 与 text 两处）、错误红字 `340.0f` → `404.0f`、接管警告 icon/text `362.0f` → `426.0f`（text 高 36 → 底 462，仍 < 底部按钮 540）。

- [ ] **Step 3: 四条生命周期路径**

- `openCreateForm`（`:1360-1373`）：在既有重置语句旁加 `formAuthField_ = AuthField::Both; formAuthFieldOpen_ = false;`
- `openEditForm`（`:1375-1396`）：在既有回填旁加 `formAuthField_ = authFieldFromText(p.authField); formAuthFieldOpen_ = false;`
- `submitForm`（`:1547` 起）：
  - 新建路径 `createProfile(...)`（`:1570`）追加实参：`..., /*activateIfNoneActive=*/true, /*sharedConfigId=*/0, authFieldToText(formAuthField_)`
  - 编辑路径（`:1610-1616` 一带）加 `p.authField = authFieldToText(formAuthField_);`
- `doApplyImport`（`:1420`）：在既有"非空才覆盖"段尾加

```cpp
        if (!importPreview_.authFieldFromEnv.empty()) {
            formAuthField_ = authFieldFromText(importPreview_.authFieldFromEnv);
        }
```

- [ ] **Step 4: 无头 UI 用例**

`tests/test_modules/test_claude_ui.cpp` 加（照该文件既有的 `click(ui, id)` / `click(ui, id + ".bg")` 模式）：
- `claude_ui_form_auth_field_dropdown_exists`：`compose` 后 `ui.find("claude.dialog.form.authfield.dd")` 非空（无服务时不崩）。
- `claude_ui_auth_field_create_persists`：新建表单 → 下拉选到"只发 x-api-key" → 保存 → 断言 DB 该 profile `authField == "api_key"`。
- `claude_ui_auth_field_edit_shows_current`：编辑一个 `api_key` 的 profile → 断言下拉选中项 = 2（`api_key`）。

**驱动下拉的写法**：先在 `tests/test_modules/test_claude_ui.cpp` 里找既有驱动 dropdown 的先例（`grep -n "dd" tests/test_modules/test_claude_ui.cpp`）；有则照抄；没有则按"点 `.<id>` 打开 → 点选项节点（形如 `.<id>.item.<idx>`，以实现为准）"尝试；若选项节点 id 无法确证，则退化为"直接设置表单成员 + 触发保存"并在报告里写明降级原因（不得假称已覆盖真实点击路径）。

- [ ] **Step 5: 定向构建与测试**

```bash
bash scripts/build-test-run.sh --no-run --filter claude_ui
```

- [ ] **Step 6: CHANGELOG 条目**

在 `CHANGELOG.md` 最近批次节内追加（sha 用 Step 7 的代码 commit 短 sha，故先 commit 代码再补本条）：

```markdown
### [AuthField] `<sha>` —— Profile 认证字段（鉴权头三态、migration v7）
- 新增 profile 级「认证字段」：`both`（默认，双发 x-api-key + Authorization: Bearer，= 原行为）/ `bearer` / `api_key`；由代理转发按 profile 决定注入哪个鉴权头（流式 / 非流式 / 连通测试三处同源）。
- 接管 settings.json 的占位符仍固定 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`（不随该字段变）；粘贴导入按键名证据自动选（只带 ANTHROPIC_API_KEY → 只发 x-api-key）。
```

- [ ] **Step 7: Commit**

```bash
git add src/module_claude_proxy/claude_module.cpp tests/test_modules/test_claude_ui.cpp
git commit -m "feat(claude): 表单「认证字段」下拉 + 四条生命周期路径 + 无头用例"
# 取短 sha 回填 CHANGELOG 后再提交文档
git add CHANGELOG.md
git commit -m "docs(changelog): 回填 [AuthField] 条目"
```

---

## 验收（实施线与复核线都跑）

1. 定向用例全绿：`bash scripts/build-test-run.sh --no-run --filter dialect` / `provider_config` / `claude_service` / `claude_ui` / `migration`。
2. 逐条核对 spec §7 的每一行都已落实（**特别是三处 SELECT、`createProfile` 返回值、UI 新建传参三处阻断项**）。
3. 三处 `schemaVersion()` 断言已是 7。
4. `git status` 干净、commit 显式 pathspec、无越界文件。
5. 报告里说明：未截屏原因 + 替代人工点验步骤（本机无无头截屏手段，沿用 `2026-09-27-profile-reverse-sync-and-copy-design.md` 附记录的同一口径）。
