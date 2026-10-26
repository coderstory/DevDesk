# 11: schema 迁移定义改 enum + 白名单校验（P2）

**What to build:** 意见书 P2 项：schema.cpp 的迁移定义目前是字符串直接拼接（SQL 拼接点），改为 enum + 白名单校验后再拼接——非法值在构建期/加载期明确报错，而不是拼出坏 SQL 在运行期炸。**P2 排期原则：随常规迭代，不设硬里程碑。**计划依据：`docs/plans/开发计划-2026-10-25.md` 阶段三 T9。

**Blocked by:** 无（P2 backlog，按迭代认领）

**Status:** done

- [x] 迁移定义 enum 化，拼接前白名单校验，非法值明确报错（构建期或加载期）
- [x] 现有全部迁移在改后等价通过（ctest 迁移用例不降）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿

**证据摘要（2026-10-26，分支 wt-11-schema-enum）：**
- 改动：`src/core/schema.h` 新增 `MigrationTable` / `MigrationColumn` 枚举 + 两张
  `inline constexpr` 白名单表 + `migrationWhitelistsAligned()` 的 `static_assert`
  （构建期钉死白名单与枚举一一对应且同序）+ 三个白名单查询函数声明；
  `src/core/schema.cpp` 三个列定义函数改返回枚举向量，`ensureTableColumns` 改为
  「表名 + 全部列名/列定义先整体过白名单（all-or-nothing），任一非法值在拼接前显式
  报错（DatabaseMigrationFailed，错误信息带非法枚举值），通过后拼接只接触白名单返回的
  静态标识符」；`tests/test_core/test_schema.cpp` 新增
  `schema_migration_whitelist_rejects_invalid`（非法表/列枚举被拒 + 白名单全覆盖 +
  标识符字符集校验 + 真库端到端）。
- 等价性：DDL 片段逐字未动（白名单表内容 = 原字符串对），迁移语义零变化；
  全量单测 Total 1587 / Failed: 0（基线 1586 / 0，+1 新用例）；
  schema 子集 25 / 25 全绿（原 24 + 1）。
- 守卫：check-consistency / module-isolation / no-glass / probe-skip-gate /
  std-includes / toolguard-braces / windows-cross-compile 全 exit 0；
  check-eui-neo-version（--eui-dir /d/project/gui/EUI-NEO）exit 0。
  probe 门禁段 exit 1（agent_e2e_probe / home_probes×2）为本机缺凭据的环境性既有状态，
  与本票改动无关（任务口径明示忽略）。
