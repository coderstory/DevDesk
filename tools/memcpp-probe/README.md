# memcpp-probe

P0.5 风险探针：验证「llama.cpp 静态链接 + 量化 embedding 模型进单 exe」，
只测三个数字——包体（exe+模型）、冷启动（进程起→首个 embedding）、峰值 RSS。
独立于 DevDesk 主构建（`scripts/build-test-run.sh` 不含本探针目标）。

## 复现

```bash
export PATH="/d/SoftWare/msys64/mingw64/bin:/d/SoftWare/cmake/cmake-4.3.3-windows-x86_64/bin:/d/SoftWare/ninja:$PATH"

# 1) llama.cpp 锁定 commit（与可行性报告 §9.3 一致），不入库
mkdir -p llama.cpp && cd llama.cpp && git init -q \
  && git remote add origin https://github.com/ggml-org/llama.cpp.git \
  && git -c http.proxy=http://127.0.0.1:51295 fetch --depth 1 origin c6fb3b5be2209f6c12339833198190f6f6829581 \
  && git checkout -q FETCH_HEAD && cd ..

# 2) 模型（不入库；GGUF，26,472,640 B = 25.25 MiB）
curl -L --fail -x http://127.0.0.1:51295 -o models/bge-small-zh-v1.5-q8_0.gguf \
  https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf/resolve/main/bge-small-zh-v1.5-q8_0.gguf

# 3) 构建 + 运行
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
build/memcpp-probe.exe models/bge-small-zh-v1.5-q8_0.gguf "今天天气"
```

## 代表性边界

模型为 bge-small-zh-v1.5 Q8_0 GGUF（第三方量化）；最终模型选型是可行性报告 §8.2 拍板项，
本探针只回答「体积 / 启动 / 内存的量级是否成立」，不回答检索质量与 pooling 口径。

三数字实测基准（2026-10-27，逐字节数为准）：exe 10,423,028 B（9.94 MiB）+ 模型 26,472,640 B
（25.25 MiB）= 总包体 36,895,668 B（35.19 MiB）。三数字仅 Windows 11 / MSYS2 MinGW64 实测，
macOS 未验证，不可当跨平台结论引用。
