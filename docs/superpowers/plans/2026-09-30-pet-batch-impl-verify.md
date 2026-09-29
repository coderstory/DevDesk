# 桌宠三件套审计修正 — 实施与复证记录（2026-09-30）

审计输入：`docs/superpowers/plans/2026-09-30-pet-batch-audit.json`（7 findings，
verdict = 需修后合并）。本文为逐条处置结论 + 变异复证证据。分支
`feat/pet-bubble-and-size`，审计修正单提交（sha 见该提交）。

## 逐条处置

| # | finding | 处置 | 证据 |
|---|---------|------|------|
| F1 | kirari 360×480 大档 1.5× 逐维钳 512×512 + sprite 默认 Cover 中心裁切头脚各 12.5%（blocker） | **已修**：`scaledFrameSize` 改**等比单因子** `min(scale, 512/w, 512/h)`（pet_skin.h，一处纯函数）；kirari 大档 → 384×512，长宽比 0.75 保持、Cover 恰好贴合零裁切。下界不钳（16 是皮肤解析界非窗口界，16×0.75=12 小窗无害且等比优先） | ① `pet_scale_frame_size` 新增 kirari 384×512 断言 + **等比不变式**（9 组合宽高比偏差 < 0.02）；② 变异复证：改回逐维钳 → `pet_scale_frame_size` 红（Total 43/44，见下）→ 恢复复绿 44/44；③ kirari 大档视觉证据 `/tmp/pet_kirari_big.png`（真 GLFW 渲一帧 glReadPixels 落图，384×546 窗，头发顶到鞋全身可见零裁切） |
| F2 | applyPetScaleIndex 热切换重开用陈旧 rt.config.x/y（拖拽后切档跳回旧位） | **已修**：重开改传新鲜 `cfg.x/cfg.y`（上面刚从当前窗口读回钳制落盘），与 applyPetSkin 同形态（pet_window.cpp） | 代码级对齐；无头环境 handle 恒 null、hotSwap 路径不可达，**无独立断言**（登记为已知验证边界，与 applyPetSkin 同一形态信任其既有路径） |
| F3 | 拍板链缺失（三件套口径零登记 + TODO 旧条目未交叉引用） | **已修**：`docs/superpowers/specs/2026-09-30-batch-fix-design.md` 追加「§6 桌宠三件套（会话外追加）」三行记录（气泡条几何 / 三档与等比钳制 / 删 toast 口径）；`TODO.md` #1「桌宠最大高度设置」补交叉引用「已由三档方案取代并交付 → 指向 §6」 | 文件 diff |
| F4 | test_settings.cpp 注释「卡高 120 / 底沿 104」失实 | **已修**：改「卡高 168、内容底沿 152」 | 文件 diff |
| F5 | 隔离守卫 A3 桥清单未纳入 3 条新桥（showPetWindow 既有遗漏） | **已修**：A3 for 列表一行追加 `showPetWindow currentPetScaleIndex petScalePresetLabels applyPetScaleIndex`（守卫脚本一行可加，未触发登记不修条款） | 守卫随 `--filter pet` / `--filter settings` 两轮复跑通过（"桥调用齐全"） |
| F6 | scale 解析失败/白名单外值静默回落零日志 | **已修**：load 两处 `LOG_WARN`（scale 键非数字 / 值不在三档白名单回落 1.0，pet_config.cpp） | 代码 diff；行为覆盖：`pet_scale_config_roundtrip` 已断言收敛值正确 |
| F7 | 切档双 INFO 日志（settings + pet 各一条） | **登记不修**：pet 侧带热切换/下启生效状态、信息量高；settings 侧与模块内其他动作（主题/字号/日志级别）同构。删一侧属日志口径统一课题，不属本批（审计亦标"不强制"） | 本行 |

## 变异复证（F1，2026-09-30 实测）

1. **变异体**：`scaledFrameSize` 恢复旧逐维钳 `[16,512]`（`std::clamp(s, 16, 512)` 逐维）。
2. **结果**：`--filter pet` → `[FAIL] pet: 显示大小档位→帧尺寸换算（等比钳上界 512）`，
   `Total: 44 Passed: 43 Failed: 1` —— kirari 384×512 期望对 512×512 实际 +
   等比不变式（360:480 组合 1.0 ≠ 0.75）双双命中。
3. **恢复**：等比单因子原样 → `Total: 44 Passed: 44 Failed: 0`。
   （历史第一轮变异：scaledFrameSize 完全不缩放 → 两用例红 42/44，同样复绿。）

## 定向测试计数（修正提交后复跑）

- `--filter pet`：**Total 44 / Passed 44 / Failed 0**
- `--filter settings`：**Total 90 / Passed 90 / Failed 0**
- 模块隔离守卫两轮通过（A3 已含 9 条桥）。

## 视觉证据（真 GLFW 后端渲一帧 glReadPixels → PPM → PNG）

- `/tmp/pet_kirari_big.png` —— kirari 1.5×（F1 修正后：384×546 全身零裁切 + 气泡在上方）
- `/tmp/pet_visual_small.png` / `_mid.png` / `_big.png` —— robot 三档（0.75×/1.0×/1.5×，
  气泡带 max 237 / 间隙带 max 0 / 精灵带 max 240，三档全过）
- 一次性 dump 探针未提交（临时文件已删，模式照 about_probe.cpp --dump 先例）。

## 审计备注闭环

- 备注 1（44/44+90/90 未独立复跑）：本文计数为修正提交后两轮复跑实录。
- 备注 2（无截图证据）：kirari + robot 三档共 4 张 dump 落图补齐。
