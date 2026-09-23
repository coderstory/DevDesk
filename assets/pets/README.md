# 桌宠皮肤包目录

DevDesk 桌宠皮肤 = 一个目录：`pet.json`（帧时序/状态映射/授权信息）+ 同尺寸 RGBA PNG 序列帧。
皮肤名 = 目录名（合法字符：字母/数字/`-`/`_`，会拼回路径）。

## 放哪里能被发现

皮肤根候选（`skinRootCandidates()`，先者优先）：

1. `assets/pets/`（exe/工作目录旁边）——随仓库分发的内置美术皮肤在这里（构建期 POST_BUILD 自动拷到 build 目录）
2. `petsDir()`（appData 目录下的 `pets/`）——**用户自装皮肤放这里**，不动仓库

## pet.json 格式

```json
{
  "name": "anime-girl",
  "frameSize": [128, 128],
  "frameRate": 6,
  "license": "CC0（作者 …）",
  "source": "来源链接",
  "phrases": ["气泡短语1", "气泡短语2"],
  "states": {
    "idle":   ["idle_00.png", "idle_01.png"],
    "poked":  ["poked_00.png"],
    "sleep":  ["sleep_00.png"],
    "typing": ["typing_00.png"]
  }
}
```

- `idle` 必填；`poked`/`sleep`/`typing` 可选（缺失时该状态视觉回落 idle）
- 帧尺寸 = 桌宠窗口尺寸，128×128 为推荐规格（透明边距居中、脚踩底边）
- `license`/`source` 随包自带（内置分发的素材必须是 CC0 或宽松许可）

## 内置皮肤

| 皮肤 | 来源 | 方式 |
|------|------|------|
| clay-cat / duck / slime / robot / ghost | DevDesk 程序生成（首次启动落 petsDir） | 代码生成 |
| anime-girl | OpenGameArt "Anime Girl" (Michona) by Alicahordana，**CC0** | 本目录随仓库分发 |

## 手动安装第三方皮肤（如 itch.io 素材包）

以 itch.io 的 **Desktop Pet Sprite**（作者 tatsu-san，页面上是"原创作品、可放心使用"的口头许可，无正式开源许可——因此不进仓库，用户自行下载）为例：

1. 到 `https://tatsu-san.itch.io/desktop-pet-sprite` 下载 zip（免费，"Name your own price" 填 0）
2. zip 里是单个 exe（PyInstaller 打包）。帧图在 exe 内的 `eat/ grab/ left_run/ right_run/ sleep/` 等目录（可用 7-Zip 或 pyinstxtractor 解出，帧已是 128×128 RGBA）
3. 在 `petsDir()/tatsu-pet/` 下摆出标准结构，四态映射建议：

| pet.json 状态 | 用哪组帧 |
|---------------|----------|
| idle | `right_run/sprite_*.png`（原地小跑当待机） |
| poked | `grab/sprite_*.png`（被抓住 = 戳反应） |
| sleep | `sleep/sprite_*.png` |
| typing | `left_run/sprite_*.png`（走动 = 干活） |

4. 重启 DevDesk（或在桌宠右键「换肤」菜单）即可看到新皮肤

注意：CC-BY-NC / CC-BY-ND（非商用/禁止演绎）类素材与本项目红线冲突（调研报告 §3），不要装。
