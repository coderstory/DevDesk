// src/module_claude_proxy/profile_repository.h
// ProfileRepository：claude_profiles 行 + SharedConfig 片段 + Keychain 条目的
// 唯一持久化入口
// （R10-A2 自 ClaudeService 抽出；职责边界见 R10 spec §2.2，方法搬移不改语义）
//
// 持有的不变式（现状同款，见 R10 spec §2.2）：
//   - API key 明文永不入 SQLite（全部 API key 由 platform::keychain 保管）
//   - keychain_account 与 claude_profiles 行一一对应（Keychain 条目 = 行的
//     密钥，orphan 清理以本不变式为前提）
//   - is_active 全局唯一（UNIQUE INDEX WHERE is_active=1 + activateProfile 事务）
//   - SharedConfig 悬空 id 读路径容错（退化为未绑定）
// 线程约定：无自有锁，随调用方（SqliteDb recursive_mutex + Keychain 自身）
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::claude {

// 单条 Claude API 配置（spec §8.3）
struct Profile {
    int64_t id = 0;
    std::string name;
    std::string baseUrl;        // 例如 https://api.anthropic.com
    std::string apiKeyPlain;    // **仅内存** 用，不入 SQLite；loadApiKey() 后才有值
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示用）
    std::string apiKeyCheck;    // 前 8 字符的 SHA256 hex（防误操作校验）
    std::string keychainAccount;// Keychain account（约定：claude:<id>）
    std::string model;          // 默认模型（旧列；语义降级为 default 槽位未配置时的 fallback）
    std::string modelsConfigJson; // 6 槽位模型配置 JSON 原文（空串 = 未配置；provider-config §4.1）
    std::string extraEnvJson;   // 额外 env 键值 JSON 原文（空串 = 未配置）
    int64_t sharedConfigId = 0; // 绑定共享配置片段 id（0 = 未绑定；0..1、不嵌套）
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
    // 上游方言覆盖（审计 CPX-13）：空串 = 未覆盖 = 按 baseUrl 的 host 推断
    //（UpstreamDialect::forBaseUrl 的第二参数）；值域 'anthropic_default' /
    // 'opencode_zen'，未知 / 空串读侧回落 host 推断、写侧归一为空串。
    // 自建网关 / 内网镜像的 host 后缀推断不出正确方言时用它钉住；
    // UI 未暴露（经 DB / 配置文件设置），取值域见 upstream_dialect.h
    std::string dialectOverride;
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 共享配置片段（2026-09-27 shared-config-and-failover 设计 §2.1/§2.2）：
// 纯 env 装饰层——只有名字 + extra_env，**不含 base_url / 密钥**，故不进路由、
// 不进凭据链（Keychain 零交互、is_active 唯一性零影响）；一个 profile 绑定
// 0..1 个片段，不嵌套（片段不引用片段）
struct SharedConfig {
    int64_t id = 0;
    std::string name;
    std::string extraEnvJson;   // 与 Profile::extraEnvJson 同构同语义（空串 = 未配置）
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 审计 CPX-07：两段写助手。
// 「先写 Keychain、再写 DB 元数据」原本在 ClaudeService 里手写多份、补偿策略
// 各不相同（删行 / 回写旧值 / 干脆没有补偿）。收敛到本类后语义统一为：
//   段一 writeKey()：把 Keychain 写成新值（空串 = 删除该条目）
//   段二 writeDb() ：落 DB 元数据
// 任一段失败 → 按构造时捕获的旧值回写 Keychain（旧值为空 = 删除条目），
// 保证密钥与 DB 里的 api_key_hint / api_key_check 始终对得上（失败时回到
// 「改之前」的状态，而不是「密钥已换、元数据没换」的中间态）。
//
// 用法：
//   TwoPhaseKeyWrite tx = TwoPhaseKeyWrite::capture(keychainService_, account);
//   auto kc = tx.writeKey(newKey);
//   if (kc.isFail()) return fail(kc...);          // 段一失败：已回写旧值
//   return tx.writeDb([&] { ...UPDATE...; });     // 段二失败：已回写旧值
// （R10-A2：随 ProfileRepository 自 claude_service.{h,cpp} 整类迁出；
// 唯一留在门面的使用点是 syncCaptureActiveProfile 的两段写）
class TwoPhaseKeyWrite {
public:
    TwoPhaseKeyWrite(std::string keychainService, std::string account,
                     std::string oldKey);

    // 捕获 Keychain 当前值作为旧值（读不到 = 该条目不存在，按空串处理）
    static TwoPhaseKeyWrite capture(const std::string& keychainService,
                                    const std::string& account);

    // 段一：把 Keychain 写成 newKey（空串 = 删除条目）。
    // 失败 → 尽力回写旧值后返回该错误；调用方不应继续段二。
    core::Result<void> writeKey(const std::string& newKey);

    // 段二：执行 DB 写；失败 → 尽力回写旧值后返回 DB 的错误。
    core::Result<void> writeDb(const std::function<core::Result<void>()>& dbWrite);

private:
    // Keychain 已改且尚未回到旧值 → 回写旧值（失败只记 ERROR：结果由调用方
    // 从 Keychain 实际状态判断）
    void compensate();

    std::string service_;
    std::string account_;
    std::string oldKey_;
    bool keyTouched_ = false;   // 段一改动过 Keychain 且尚未回到旧值
};

class ProfileRepository {
public:
    // 构造时不持有所有权，db 由调用方确保生命周期；keychainService 是
    // Keychain service 名（生产 "DevDesk"，测试经 ClaudeService 传
    // "DevDeskTest" 隔离）
    explicit ProfileRepository(core::data::SqliteDb& db,
                               std::string keychainService);

    // ---- Profile CRUD ----

    // 列出所有 profile（apiKeyPlain 全部为空字符串）
    core::Result<std::vector<Profile>> listProfiles();

    // 取单个 profile（apiKeyPlain 为空）
    core::Result<Profile> getProfile(int64_t id);

    // 当前激活 profile（无激活 → NoActiveProfile + 可操作指引；该文案是
    // 503 JSON body 的原样透传内容，proxy_router 唯一出口）
    core::Result<Profile> getActiveProfile();

    // 创建并保存到 SQLite + Keychain：校验 + INSERT ... RETURNING 取回 rowid +
    // 两段写（段一 Keychain、段二回填 keychain_account）+ 任一段失败回滚 INSERT
    // 的行。**不含「首建自动激活」编排**（那是门面 createProfile 的策略尾，经
    // 门面 setActiveProfile 走，代理运行中会同步接管 settings.json）。
    // 返回的 Profile.isActive 恒 false（激活与否由门面按策略回填）；
    // created_at / updated_at 为写侧统一取值的 now
    core::Result<Profile> createProfile(const std::string& name,
                                        const std::string& baseUrl,
                                        const std::string& apiKey,
                                        const std::string& model,
                                        int64_t sharedConfigId = 0,
                                        const std::string& authField = "both",
                                        const std::string& dialectOverride =
                                            std::string());

    // 更新 profile（profile.id 必须已存在；apiKeyPlain 非空则覆盖 Keychain，
    // 空则只更新 name / url / model / 片段绑定等元数据）。写侧规范化与 JSON
    // 信任边界校验同 createProfile。oldProfile（可空出参）：成功且旧行可读到
    // 时回填更新前的旧行——门面 updateProfile 的 re-takeover 策略尾
    //（片段绑定变化 && 旧行激活 && 代理运行）据此判断，本类不做该编排
    core::Result<void> updateProfile(const Profile& profile,
                                     Profile* oldProfile = nullptr);

    // 删除 profile（两段写：段一清 Keychain 条目、段二删 SQLite 行；任一段失败
    // 按旧值回写 Keychain，SQLite 行保留可重试）。wasActive（可空出参）：
    // 成功时回填被删行的 is_active——门面 deleteProfile 的停代理策略尾
    //（删激活行 && 代理运行 → stopProxy）据此判断，本类不做该编排
    core::Result<void> deleteProfile(int64_t id, bool* wasActive = nullptr);

    // 复制 Profile（设计 §4.2/§4.3，M4）：名称自动加「副本」后缀（重名递增
    // 避让，100 次仍冲突 → fail 不建行），API key 一并复制（源 key 缺失 →
    // fail，不建无 key 的副本），**副本不激活**（S-4）。
    // 二段补写（models_config / extra_env）失败 → best-effort 删除刚建的副本
    // 回滚：回滚成功 = fail 且零残留；回滚失败 = fail 且文案注明不完整副本 id
    // shared_config_id 一并复制（属于配置，设计 §2.6）；
    // dialect_override 一并复制（同上，审计 CPX-13）
    // 纯行 + Keychain 层面操作，无代理 / 接管交互，R10-A2 整方法搬入
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // 启动时孤儿清理：把 keychainService_ 命名空间里没有对应
    // claude_profiles 行的密钥清掉（历史 bug 遗留的 claude:* 孤儿；
    // 「Keychain 条目与行一一对应」= 本类不变式）。返回清理数量
    core::Result<int> cleanupOrphanKeychainEntries();

    // 显式从 Keychain 加载明文 API key（仅在需要发请求时调用，用完即丢弃）
    core::Result<std::string> loadApiKey(int64_t profileId);

    // setActiveProfile 的 DB 段：事务内「清零所有 is_active → 置位目标」
    //（is_active 全局唯一）。调用方（门面 setActiveProfile）负责前置校验
    //（id / 存在性）与捕获编排（syncCaptureActiveProfile 必须先于本事务
    // 发生，否则会把自己刚写进去的托管值当用户值捕获）
    core::Result<void> activateProfile(int64_t id);

    // ---- 共享配置片段（设计 §2.1/§2.4/§2.6）----
    // 生命周期语义（改动当场生效，避免 cc-switch 的「改了片段不生效」头号 bug）：
    //   改（改名 / 改 env）→ 绑定它的 profile 下次接管生效；代理在跑且属激活
    //     profile → 立即 re-takeover（策略尾在门面）
    //   删 → 同事务解绑绑定它的 profile（shared_config_id = 0，不留悬空 id），
    //     随后若代理在跑且激活 profile 在其中 → re-takeover（策略尾在门面）
    //   解绑（经 updateProfile 把 sharedConfigId 置 0）→ 同款当场生效，同名键
    //     回落 profile 自身值（片段值从不写进 profile 行）
    core::Result<std::vector<SharedConfig>> listSharedConfigs();
    core::Result<SharedConfig> createSharedConfig(const std::string& name,
                                                  const std::string& extraEnvJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id, bool* activeBound = nullptr);

    // 片段冻结键集（G10，设计 §2.4）：profile 所绑片段 extra_env 的全部键
    //（别名归一后，与写进 live 的键形一致）。未绑定 / 片段已删 / JSON 非法 →
    // 空集（读路径容错）
    core::Result<std::vector<std::string>> sharedEnvKeysFor(int64_t profileId);

    // 片段 extra_env 原文（0 / 片段不存在 / 读失败 → 空串 = 未绑定；读路径容错，
    // 悬空 id 退化成「未绑定」而不是让接管/G10 失败）。门面的 syncTakeOver /
    // runProfile 直接经此读片段合成 env（接管 / 直连）
    std::string sharedExtraEnvJsonFor(int64_t sharedConfigId) const;

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    // 从一行 SqlRow 构造 SharedConfig
    core::Result<SharedConfig> rowToSharedConfig(const core::data::SqlRow& row) const;

    // 片段存在性校验（update / delete 前置，缺失 → NotFound）
    core::Result<void> requireSharedConfigExists(int64_t id) const;

    static std::string sha256HexPrefix(const std::string& s);
    static std::string makeApiKeyHint(const std::string& apiKey);

    core::data::SqliteDb& db_;
    std::string keychainService_;
};

} // namespace module::claude