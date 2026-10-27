// memcpp P0.5 风险探针：llama.cpp 静态链接 + 量化 embedding 模型进单 exe。
// 只测三个数字：包体（构建后量文件）、冷启动（进程起->首个 embedding）、峰值 RSS。
// 不代表检索质量：pooling 取 MEAN 即可，最终模型是可行性报告 §8.2 的拍板项。
#include "llama.h"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// 契约：中文输入必须按 UTF-8 传给 llama_tokenize；MinGW main 的 ANSI argv 不可靠，
// 统一从 GetCommandLineW 转 UTF-8，与控制台代码页解耦。
static std::string to_utf8(const wchar_t * w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

int main() {
    const Clock::time_point t_main = Clock::now();

    int argcW = 0;
    wchar_t ** argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (argcW < 2) {
        fprintf(stderr, "usage: memcpp-probe <model.gguf> [prompt]\n");
        return 2;
    }
    const std::string model_path = to_utf8(argvW[1]);
    const std::string prompt = argcW >= 3 ? to_utf8(argvW[2]) : "\xE4\xBB\x8A\xE5\xA4\xA9\xE5\xA4\xA9\xE6\xB0\x94" /*今天天气*/;
    LocalFree(argvW);

    llama_backend_init();
    // 探针只关心三个数字；llama.cpp 的 INFO 日志不进输出，避免干扰测量与解析
    llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0; // 纯 CPU，量化模型走 CPU 是产品基线路径
    const Clock::time_point t_load0 = Clock::now();
    llama_model * model = llama_model_load_from_file(model_path.c_str(), mp);
    if (!model) {
        fprintf(stderr, "model load failed: %s\n", model_path.c_str());
        return 3;
    }
    const double model_load_ms = ms_since(t_load0);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512;            // bge-small-zh-v1.5 上限即 512
    cp.n_batch = 512;
    cp.n_threads = 1;          // 小模型多线程收益为负且抖动大；探针取单线程可复现口径
    cp.n_threads_batch = 1;
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED; // 让 GGUF 元数据决定（bge=CLS），不硬改 pooling 语义
    const Clock::time_point t_ctx0 = Clock::now();
    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        fprintf(stderr, "context init failed\n");
        return 3;
    }
    const double ctx_init_ms = ms_since(t_ctx0);

    const int n_embd = llama_model_n_embd(model);
    const llama_vocab * vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(32);
    const int n_tokens = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                                        tokens.data(), (int32_t)tokens.size(), true, true);
    if (n_tokens <= 0) {
        fprintf(stderr, "tokenize failed\n");
        return 3;
    }
    tokens.resize(n_tokens);

    llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    for (int i = 0; i < n_tokens; ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = false;
    }
    batch.n_tokens = n_tokens;

    const Clock::time_point t_dec0 = Clock::now();
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "decode failed\n");
        return 3;
    }
    const float * emb = llama_get_embeddings_seq(ctx, 0);
    if (!emb) {
        fprintf(stderr, "embeddings unavailable (pooling=%d)\n", (int)llama_pooling_type(ctx));
        return 3;
    }
    const double first_embed_ms = ms_since(t_dec0);

    // 验证辅助：设 MEMCPP_PROBE_DUMP=<path> 时写全量向量（逗号分隔），供外部算余弦复核；
    // 不设该环境变量时分支不进入，维持「不传开关零开销」契约。dump 打不开必须非零退出：
    // 静默失败会让复核者把旧文件或缺文件当成新向量。
    if (const char * dump = getenv("MEMCPP_PROBE_DUMP")) {
        FILE * f = fopen(dump, "w");
        if (!f) {
            fprintf(stderr, "dump open failed: %s (errno=%d %s)\n", dump, errno, strerror(errno));
            return 3;
        }
        for (int i = 0; i < n_embd; ++i) fprintf(f, i ? ",%.6f" : "%.6f", emb[i]);
        fclose(f);
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    double peak_rss_mb = -1.0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        peak_rss_mb = (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    }

    printf("MODEL        %s\n", model_path.c_str());
    printf("PROMPT       %s (tokens=%d)\n", prompt.c_str(), n_tokens);
    printf("N_EMBD       %d\n", n_embd);
    printf("EMB[0..3]    %.6f %.6f %.6f %.6f\n", emb[0], emb[1], emb[2], emb[3]);
    printf("MODEL_LOAD_MS   %.1f\n", model_load_ms);
    printf("CTX_INIT_MS     %.1f\n", ctx_init_ms);
    printf("DECODE_MS       %.1f\n", first_embed_ms);
    printf("TOTAL_MS        %.1f\n", ms_since(t_main));
    printf("PEAK_RSS_MB     %.1f\n", peak_rss_mb);

    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
