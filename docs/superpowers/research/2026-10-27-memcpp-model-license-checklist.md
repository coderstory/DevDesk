# memcpp 模型资产许可证清单（拍板②直接输入）

- 日期：2026-10-27；分支 feat/memcpp @ 0f851612
- 对象：BAAI/bge-small-zh-v1.5（本地 embedding）+ llama.cpp（推理引擎）+ 其 GGUF 量化版
- 方法：仅一手来源（HF 模型卡 raw README、FlagEmbedding 仓库 LICENSE、llama.cpp 仓库 LICENSE），每条附 URL 与原文短引。
- 约定：MIT 许可证 = 一种近乎"随便用"的开源许可，唯一硬性要求是把作者的版权声明和许可文本原样带走；"再分发" = 把文件副本交给别人（无论免费还是收费）。

## 1. bge-small-zh-v1.5（原版模型）

- 许可证：MIT。
  来源：https://huggingface.co/BAAI/bge-small-zh-v1.5/raw/main/README.md （YAML 首行 `license: mit`）
- 商用：明确允许。原文："The released models can be used for commercial purposes free of charge."（同上 URL）
- 许可文本出处：模型卡声明 "FlagEmbedding is licensed under the MIT License"；FlagEmbedding 仓库 LICENSE 文件版权行 "Copyright (c) 2022 staoxiao"。
  来源：https://github.com/FlagOpen/FlagEmbedding/blob/master/LICENSE
- 分发义务仅一条：保留声明。原文："The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software."
- 署名：无强制署名义务；star + 引用 C-Pack 论文（arXiv:2309.07597）为"恳请"而非条款。

## 2. GGUF 量化版

- 主要发布者：CompendiumLabs（第三方，非 BAAI 官方；HF 搜索 "bge-small-zh-v1.5 gguf" 仅此一仓库）。
  来源：https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf
- 量化产物许可证：仓库自标 MIT（YAML `license: mit`）；提供 F32(91MB)/F16(46MB)/Q8_0(26MB)/Q4_K_M(15MB) 四档。
- 卡片声明上游："Source model: https://huggingface.co/BAAI/bge-small-zh-v1.5"
- 再分发：允许（按 MIT 标签），义务同第 1 节（保留声明）。专门再分发条款文字：未查到（已查：该模型卡页面全文 + raw README 全文，无超出 MIT 标签的许可文字）。
- 附声明要求：卡片未单独要求；保守做法 = 随分发物附 BAAI 原模型 MIT 声明 + CompendiumLabs GGUF 转换出处。
- 风险注记：量化版许可证由第三方发布者自标，与上游 MIT 一致、无冲突信号；未见 BAAI 对该转换的官方背书或禁止声明（已查 BAAI 官方模型卡，无 GGUF 相关内容）。

## 3. llama.cpp

- 许可证：MIT。版权人 "The ggml authors"（2023-2026）。
  来源：https://github.com/ggerganov/llama.cpp/blob/master/LICENSE
- 静态链接进闭源/内部 exe：允许。MIT 授权含 "use, copy, modify, merge, publish, distribute, sublicense, and/or sell"（含转授权与出售），无开源自家代码的义务。
- 义务：在分发物中保留版权声明 + MIT 许可文本（"include in all copies or substantial portions of the Software"）。放在"关于/第三方声明"弹窗或随附 LICENSE 文件即可满足。

## 4. 分发形态冲突分析

- "模型打进安装包"：构成再分发（把副本交给用户即再分发，与收不收费无关）。义务：安装包内随附三份 MIT 声明（llama.cpp / BAAI 原模型 / CompendiumLabs GGUF）。
- "首次使用时下载"：用户自行从 HF 取副本，公司不经手副本，再分发义务更弱；但建议仍在产品内附第三方声明与模型出处（成本低，避免争议）。
- 冲突点结论：许可证层面两种形态义务几乎相同且都极轻；真正的冲突在工程而非法务——"单 exe 零依赖"与 15~91MB 模型文件必须二选一，法务不构成否决项。
- 公司内部使用（非对外销售）：MIT 无使用领域限制、无版税、无对外/对内条款差异；内部复制同样只要求保留声明。未查到 BAAI 或 CompendiumLabs 有任何"仅限研究/非商用"类附加条款（已查：两个 HF 模型卡全文、FlagEmbedding LICENSE、llama.cpp LICENSE）。

## 5. 结论表

| 分发形态 | 许可义务清单 |
|---|---|
| 纯本地离线（模型随安装包） | 安装包内附三份 MIT 声明：llama.cpp、BAAI bge-small-zh-v1.5、CompendiumLabs GGUF 出处 |
| 本地+网关（模型仍在本机，网关转发） | 同上；网关服务自带同一份声明文件即可，无新增义务 |
| 私有化（部署进客户内网） | 同上；声明文件必须随部署物交付（客户拿到副本，再分发义务落在交付物内） |

**拍板②还差什么**：法务输入已齐、许可证侧零阻塞（风险低），剩余只是技术取舍——量化档位（Q4_K_M 15MB 或 Q8_0 26MB）与"打进安装包 vs 首次下载"。
