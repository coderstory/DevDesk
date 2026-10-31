# 2026-10-31 UI 修复批（六项）

> 来源：用户 2026-10-31 反馈，一次批处理。范围 = 关于页 / Claude 切换器 / 目录树 / 首页 / 卡片质感。
> 证据：`evidence/20261031-ui-fix-batch/`（截图 + 测试输出）。

## 1. 需求与处置

| # | 需求 | 根因 | 改动 |
|---|---|---|---|
| 1 | 关于页彻底删除复制功能；删除「查看许可」按钮 | 关于页有 Ghost 复制图标 + 命中区 + 剪贴板 toast；「开源组件 / 查看完整列表」是 Ghost 链接行 | 删 `AboutRow.highlight/linkToast` 两字段与整套复制/链接/toast 代码；`setClipboardText`、`light_toast`、`chrono` 依赖一并摘除。行宽从「内容宽 − 24」回到满内容宽 |
| 2 | 切换器在 Windows 上读取 token 报错 | 双机同步：macOS 侧 `credential::protect` 是**明文透传**，库搬到 Windows 后 `hexDecode` 直接失败（「DPAPI blob 的 hex 形态不成立」）。反向同理：Windows 密文到 macOS 会被当明文 key 发出去 | ① Windows 密文加 `dpapi:` 前缀，`unprotect` 兼容无前缀旧数据；② `loadApiKey` 解密失败且形态像明文 Key（可打印 ASCII/8..512 字节/含字母数字）→ 就地升级为 DPAPI 密文并回写（是升级不降级）；③ POSIX 侧认出 `dpapi:` 前缀 → 明确报错「Windows 加密的，请重新填写」，不再静默当 key 用 |
| 3 | 添加 Profile 按钮改成列表头新增按钮 | 底部独立按钮行占 60px 高 | 列表头右侧加 28×28 幽灵「+」（`claude.profiles.new`，id 不变，点击链路与测试不变）；底部按钮行删除，60px 空间回收给列表（空态文案改「点右上角 + 新建」） |
| 4 | 目录树文件夹右键支持插入子目录与子 md | 文件夹右键只有 重命名/删除 | 菜单四项：新建子目录 / 新建子笔记 / 重命名 / 删除；前两项复用既有弹窗并把落点预置为右键的文件夹，同时展开该文件夹 |
| 5 | 首页 token 趋势卡片可折叠 | 图表卡固定 220 起步、吸收全部余高，占屏大且不可收 | 标题行整行可点（透明命中层）+ 右侧 chevron 图标；折叠态卡高 56、图表区整块不建、折叠态不参与伸展（余高留白）；状态写 `app_config` 键 `home.chart_collapsed` 跨会话保持 |
| 6 | todo 等内容卡片半透效果丢失 | `components::card` 骨架自带**不透明**默认底 `cardColor`，把内层 0.92 半透卡面「垫实」成纯白（H20 引入骨架后开始）；另有一批内容卡从未接入 `cardSurface` | ① todo 卡片/拖拽副本骨架底改全透明，卡面唯一载体 = 内层 `.bg`（`kCardGlassAlpha` + H16 浅染）；② memory 面板 15 处、md 预览纸卡、`ui_kit::cardFrame`（壁纸卡）实色 → `cardSurface(t)` |

## 2. 有意不做的

- **不改 `kCardGlassAlpha`（0.92）**：这是 H15 拍板值，本次是「恢复」不是「改档」。若仍嫌半透不够明显，属设计决策（参考原型用的是 `rgba(255,255,255,.65~.75)` + `backdrop-filter: blur(4px)`，EUI-NEO 无 backdrop blur，只能靠降 alpha 近似）。
- **不为折叠态改图表数据链路**：折叠只影响绘制，不停止统计轮询。

## 3. 验证

| 项 | 结果 |
|---|---|
| `bash scripts/build-test-run.sh --no-run` | 构建成功；`Total: 2052 / Passed: 2046 / Failed: 0` |
| `ctest -L probe` | 25/25 通过（wallpaper 5 项按平台跳过） |
| 新增/改写的用例 | `home_chart_card_collapsible`（折叠往返 + 卡高 56 + 图表区不建 + 图标态）；`credential_dpapi_blob_cross_platform_guard`；`credential_plaintext_detection_bounds`；md 文件夹右键四项与「落点预置」；关于页改为「复制/链接/ toast 一律不得出现」的反向断言 |
| 视觉 | 关于页无复制图标、许可组只剩开源协议；切换器头部有「+」、底部无按钮；首页趋势卡右上角 chevron 可见；todo 卡片骨架透明 |

## 4. 挂账（本机 macOS，未验部分）

- **Windows 真机复跑**：需求 2 的三条路径（明文库→Windows 自动迁移、`dpapi:` 前缀写回、macOS 读 `dpapi:` 报错文案）本机只能验逻辑与单测，需在 Windows 机上跑一次真实切换 Profile。
- 0.92 半透在有数据的 todo 页表现为「近白微透」，与列底色差 2~7 个色阶；是否加强由用户拍板。
