// src/module_claude_proxy/profile_repository.h
// ProfileRepository：claude_profiles 行 + SharedConfig 片段的唯一持久化入口
// （R10-A2 自 ClaudeService 抽出；职责边界见 R10 spec §2.2，方法搬移不改语义）
//
// 持有的不变式（2026-10-02 存储统一设计 §4.2 改写——旧三条里的前两条已作废）：
//   - ~~API key 明文永不入 SQLite~~ **已作废**：凭据本体现在就在
//     claude_profiles.api_key_encrypted 列（分平台形态见
//     platform/credential_protection.h：macOS/POSIX 明文 + 文件权限 0600；
//     Windows DPAPI 密文）。SQLite 不再是「只存引用」的元数据仓库。
//   - ~~keychain_account 与行一一对应~~ **已作废**：keychain_account 列已删，
//     platform::keychain 不再是凭据来源，只剩启动时的一次性残留清理
//     （cleanupOrphanKeychainEntries，claude:<id> 前缀白名单 + 空 profile 护栏）。
//   - api_key_hint 是 UI / 导出用的脱敏形态（前6+***+尾4）；api_key_check 是
//     全量 SHA256 hex（「密钥是否变更」判据）。两者都随密钥写入同批更新。
//   - is_active 全局唯一（UNIQUE INDEX WHERE is_active=1 + activateProfile 事务）
//   - SharedConfig 悬空 id 读路径容错（退化为未绑定）
// 线程约定：无自有锁，随调用方（SqliteDb recursive_mutex）
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
    // **仅内存** 用；loadApiKey() 后才有值。非空 = 本次 update 要覆盖凭据列。
    // 凭据本体存在 claude_profiles.api_key_encrypted（本结构不暴露存储形态——
    // 谁都不该直接读那一列，一律经 loadApiKey / TwoPhaseKeyWrite）。
    std::string apiKeyPlain;
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示 / 导出占位用，脱敏）
    std::string apiKeyCheck;    // 全量 SHA256 hex（64 字符；「密钥是否变更」判据）
    std::string model;          // 默认模型（= models_config 的 default 槽位，双向同步）
    std::string modelsConfigJson; // 6 槽位模型配置 JSON 原文（空串 = 未配置；provider-config §4.1）
    std::string extraEnvJson;   // 额外 env 键值 JSON 原文（空串 = 未配置）
    // 高级设置 JSON（2026-09-30 advanced-json-editor 设计 §3.1，migration v11）：
    // settings.json **除 env 外全部顶层属性**（model / permissions / statusLine /
    // hooks / …）整体作为一份原始 JSON 存储，空串 = 未配置。**仅存原文，不拆
    // 字段**；UI 落库统一 pretty-print（设计 D-6），读侧容错见 provider_config
    // 的 effectiveExtraSettingsJson / applyExtraSettingsToSettingsDoc
    std::string extraSettingsJson;
    int64_t sharedConfigId = 0; // 绑定共享配置片段 id（0 = 未绑定；0..1、不嵌套）
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
    // 上游方言覆盖（审计 CPX-13）：空串 = 未覆盖 = 按 baseUrl 的 host 推断
    //（UpstreamDialect::forBaseUrl 的第二参数）；值域 'anthropic_default' /
    // 'opencode_zen'，未知 / 空串读侧回落 host 推断、写侧归一为空串。
    // 自建网关 / 内网镜像的 host 后缀推断不出正确方言时用它钉住；
    // UI 未暴露（经 DB / 配置文件设置），取值域见 upstream_dialect.h
    std::string dialectOverride;
    // 备用 Profile id（0 = 无；failover 单跳，2026-09-27 shared-config-and-
    // failover 设计 §3.7）：主 profile 返回可重试错误（401/403/429/5xx/网络
    // 失败）时，代理用备用重试一次（resolveFallbackProfile，不改 is_active、
    // 不重写 settings.json）。写侧校验拒自引用（== 自身）；删除时同事务解绑
    //「备用指向它」的行；复制时一并携带
    int64_t fallbackProfileId = 0;
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 共享配置片段（2026-09-27 shared-config-and-failover 设计 §2.1/§2.2 建立；
// 2026-09-30 advanced-json-editor 设计 §8 重构）：只有名字 + 高级属性 JSON 预设，
// **不含 base_url / 密钥**，故不进路由、不进凭据链（凭据列零交互、is_active
// 唯一性零影响）；一个 profile 绑定 0..1 个片段，不嵌套（片段不引用片段）。
// v11（migration kClaudeExtraSettingsJson）起 extra_env 列已删（旧片段 env 数据
// 弃用不转换，设计 §8.3），片段内容 = settings.json 非 env 顶层属性 JSON
struct SharedConfig {
    int64_t id = 0;
    std::string name;
    std::string extraSettingsJson;   // 高级属性 JSON 预设原文（空串 = 未配置）
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 审计 CPX-07：两段写助手。
// 「先写凭据、再写行元数据」原本在 ClaudeService 里手写多份、补偿策略各不相同
// （删行 / 回写旧值 / 干脆没有补偿）。收敛到本类后语义统一为：
//   段一 writeKey()：把 api_key_encrypted / api_key_hint / api_key_check 三列
//                    写成新值（空串明文 = 三列清空，等价于「该行没有密钥」）
//   段二 writeDb() ：落该行的其余元数据
// 任一段失败 → 按构造时捕获的旧值回写三列，保证密钥与 api_key_hint /
// api_key_check 始终对得上（失败时回到「改之前」的状态，而不是「密钥已换、
// 元数据没换」的中间态）。
//
// 2026-10-02 存储统一：两段从「Keychain + SQLite 两个存储」变成「同一行的两组列」。
// 结构**照旧保留**（而不是并成一条 UPDATE）——四种宿主（create / update / delete /
// 反向捕获）的失败补偿语义与失败文案都是围绕它写的，合并会把语义差异抹平。
// 旧值是「存储形态」原样字节：回写走 oldStored_ 直写（不再经 protect 重编码），
// 所以 DPAPI 那种「同一明文两次加密结果不同」的平台也能回到逐字节的旧状态。
//
// 用法：
//   TwoPhaseKeyWrite tx = TwoPhaseKeyWrite::capture(db_, profileId);
//   auto kc = tx.writeKey(newKey);
//   if (kc.isFail()) return fail(kc...);          // 段一失败：已回写旧值
//   return tx.writeDb([&] { ...UPDATE...; });     // 段二失败：已回写旧值
class TwoPhaseKeyWrite {
public:
    TwoPhaseKeyWrite(core::data::SqliteDb& db, int64_t profileId,
                     std::string oldStored, std::string oldPlain);

    // 捕获该行当前的凭据（读不到 / 读失败 = 按「没有密钥」= 空串处理）
    static TwoPhaseKeyWrite capture(core::data::SqliteDb& db, int64_t profileId);

    // 段一：把三列写成 newKey（空串 = 清空三列）。
    // 失败 → 尽力回写旧值后返回该错误；调用方不应继续段二。
    core::Result<void> writeKey(const std::string& newKey);

    // 段二：执行 DB 写；失败 → 尽力回写旧值后返回该错误。
    core::Result<void> writeDb(const std::function<core::Result<void>()>& dbWrite);

    // 捕获到的旧明文（供调用方做「密钥是否已回到旧值」的诊断比较）
    const std::string& oldKeyPlain() const { return oldPlain_; }

private:
    // 三列已改且尚未回到旧值 → 回写旧值（失败只记 ERROR：结果由调用方
    // 从列的实际状态判断）
    void compensate();

    core::data::SqliteDb& db_;
    int64_t profileId_;
    std::string oldStored_;     // 旧存储形态原样字节（回写用，不经 protect）
    std::string oldPlain_;      // 旧明文（诊断比较用）
    bool keyTouched_ = false;   // 段一改动过该行且尚未回到旧值
};

class ProfileRepository {
public:
    // 构造时不持有所有权，db 由调用方确保生命周期；keychainService 是**旧
    // Keychain 命名空间**的名字——凭据已迁进本库，这个 service 名如今只用于启动
    // 时的一次性残留清理（生产由 ClaudeService 按数据根派生 "DevDesk-<短哈希>"，
    // 每个数据根独立命名空间；测试经 ClaudeService 传 "DevDeskTest…" 隔离）
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

    // 创建并保存到 SQLite（含凭据三列）：校验 + protect(apiKey) + 一条 INSERT
    // ... RETURNING id 取回 rowid。
    // 2026-10-02 存储统一：凭据与元数据如今在**同一行**——旧实现的「段一 Keychain
    // + 段二回填 keychain_account + 任一失败回滚 INSERT」在单条 INSERT 里天然原子，
    // 不再有「行了但密钥没落」的中间态（旧回滚路径随之删除，行为等价且更强）。
    // protect 失败（Windows DPAPI 出错）→ 在建行**之前**失败，不留行、不留半截凭据。
    // **不含「首建自动激活」编排**（那是门面 createProfile 的策略尾，经
    // 门面 setActiveProfile 走，代理运行中会同步接管 settings.json）。
    // 返回的 Profile.isActive 恒 false（激活与否由门面按策略回填）；
    // created_at / updated_at 为写侧统一取值的 now
    // fallbackProfileId：建时即带（0 = 无；同 sharedConfigId「建时即设」口径——
    // 先建后 update 会留下一次「未配备用」的中间态，经尾参一次写入）
    core::Result<Profile> createProfile(const std::string& name,
                                        const std::string& baseUrl,
                                        const std::string& apiKey,
                                        const std::string& model,
                                        int64_t sharedConfigId = 0,
                                        const std::string& authField = "both",
                                        const std::string& dialectOverride =
                                            std::string(),
                                        int64_t fallbackProfileId = 0,
                                        // 高级设置 JSON 原文（空串 = 未配置；
                                        // 尾插默认参与 dialect_override 同款先例，
                                        // duplicateProfile「复制即配置」经此携带）
                                        const std::string& extraSettingsJson =
                                            std::string());

    // 更新 profile（profile.id 必须已存在；apiKeyPlain 非空则覆盖凭据三列，
    // 空则只更新 name / url / model / 片段绑定等元数据）。写侧规范化与 JSON
    // 信任边界校验同 createProfile。oldProfile（可空出参）：成功且旧行可读到
    // 时回填更新前的旧行——门面 updateProfile 的 re-takeover 策略尾
    //（片段绑定变化 && 旧行激活 && 代理运行）据此判断，本类不做该编排
    core::Result<void> updateProfile(const Profile& profile,
                                     Profile* oldProfile = nullptr);

    // 删除 profile（两段写：段一清凭据三列、段二删 SQLite 行；任一段失败
    // 按旧值回写三列，SQLite 行保留可重试）。wasActive（可空出参）：
    // 成功时回填被删行的 is_active——门面 deleteProfile 的停代理策略尾
    //（删激活行 && 代理运行 → stopProxy）据此判断，本类不做该编排
    core::Result<void> deleteProfile(int64_t id, bool* wasActive = nullptr);

    // 复制 Profile（设计 §4.2/§4.3，M4）：名称自动加「副本」后缀（重名递增
    // 避让，100 次仍冲突 → fail 不建行），API key 一并复制（源 key 缺失 →
    // fail，不建无 key 的副本；失败分两态：列空 = 该 Profile 没有密钥 /
    // 读失败（解不开等，原样透传错误码），首态文案带恢复提示「编辑该 Profile
    // 重新填一次 API Key 即可」），**副本不激活**（S-4）。
    // 二段补写（models_config / extra_env / extra_settings_json）失败 →
    // best-effort 删除刚建的副本回滚：回滚成功 = fail 且零残留；回滚失败 =
    // fail 且文案注明不完整副本 id
    // shared_config_id 一并复制（属于配置，设计 §2.6）；
    // dialect_override 一并复制（同上，审计 CPX-13）；
    // fallback_profile_id 一并复制（同上，设计 §3.7）；
    // extra_settings_json 一并复制（同上，advanced-json-editor 设计 §3.3 F-3：
    // 二段补写门槛条件含「仅 JSON profile」——两个 env 字段均空、仅
    // extraSettingsJson 非空时不得静默丢字段）
    // 纯行 + Keychain 层面操作，无代理 / 接管交互，R10-A2 整方法搬入
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // 启动时的**旧 Keychain 残留清理**（2026-10-02 存储统一 §6.3 硬约束 3）：
    // 凭据已迁进本库，keychainService_ 命名空间下剩下的 claude:* 条目全是没人管
    // 的旧钥匙 → 一次性清掉（不在白名单前缀内的 account 一律保留、不计数——
    // keychain.h shouldDeleteOrphan 的三平台统一语义）。返回清理数量。
    // 三道护栏（少一条都会误删用户真实密钥，事故各出过一次）：
    //   ① 当前数据根零 profile → 整体跳过 + LOG_WARN，一个条目不碰
    //      （service 命名空间的隔离级别弱于 DEVDESK_DATA_ROOT）
    //   ② 白名单前缀 claude:（前缀外保留不删、不计数）
    //   ③ **凭据真的已迁进库**才允许清理（2026-10-03 安全审计 F1 blocker）：
    //      api_key_encrypted 列必须可读，且至少一行真的存着凭据。只看「有没有
    //      profile」会被**旧形态的库**骗过——旧库同样有行，而 listProfiles 的
    //      SELECT 不引用新列，在旧库上照样成功返回行 → keep 传空 → 把用户
    //      Keychain 里真实可用的 API Key 全清空。
    core::Result<int> cleanupOrphanKeychainEntries();

    // 取该 profile 的明文 API key（仅发请求时调用，用完即丢弃）。
    // 列空 = 该 Profile 没有密钥 → fail(NotFound)（调用方按「没配过」处理）；
    // 存储形态解不开（Windows 换机/换用户、列被篡改）→ fail(原错误码) 且
    // **不静默回落空串**——「解不开」与「没配过」必须可分辨。
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
                                                  const std::string& extraSettingsJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id, bool* activeBound = nullptr);

    // 片段高级属性 JSON 原文（0 / 片段不存在 / 读失败 → 空串 = 未绑定；读路径
    // 容错，悬空 id 退化成「未绑定」而不是让接管失败）。v11 起片段内容为
    // settings.json 非 env 顶层属性 JSON（旧 extra_env 已弃用，设计 §8）。
    // 门面的 syncTakeOver / runProfile 经此读片段；导出链（doCopyConfig）不经
    // 此访问器——它走列表快照 cachedSharedConfigs_ 的 findSharedById
    //（原 sharedExtraEnvJsonFor 改名，advanced-json-editor 设计 §3.3）
    std::string sharedExtraSettingsJsonFor(int64_t sharedConfigId) const;

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    // 从一行 SqlRow 构造 SharedConfig
    core::Result<SharedConfig> rowToSharedConfig(const core::data::SqlRow& row) const;

    // 片段存在性校验（update / delete 前置，缺失 → NotFound）
    core::Result<void> requireSharedConfigExists(int64_t id) const;

    core::data::SqliteDb& db_;
    std::string keychainService_;   // 仅用于旧 Keychain 残留清理（见类头不变式）
};

} // namespace module::claude