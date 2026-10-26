# 12: http_client 改 unique_ptr pimpl（P2）

**What to build:** 意见书 P2 项：http_client 析构里的裸 `delete impl_` 换成 `std::unique_ptr<Impl>`，消除手动析构（泄漏/双删风险点），行为零变化。**P2 排期原则：随常规迭代。**计划依据：`docs/plans/开发计划-2026-10-25.md` 阶段三 T10。

**Blocked by:** 无（P2 backlog，按迭代认领）

**Status:** done

- [x] 裸 `delete impl_` 消失，Impl 由 unique_ptr 持有
- [x] http_client 相关单测全绿（Failed: 0，Total 不降）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过

**证据摘要（2026-10-26，分支 wt-12-http-client-pimpl）：**
- 改动：`src/platform/http_client.h` 的 `StreamingResponse::impl_` 从裸
  `Impl* impl_ = nullptr;` 改为 `std::unique_ptr<Impl> impl_;`（`<memory>` 头本已在）；
  `src/platform/http_client.cpp` 构造函数 `new Impl()` → `std::make_unique<Impl>()`，
  析构函数删去 `delete impl_; impl_ = nullptr;`（由 unique_ptr 成员析构接管，
  cancel() → join 的顺序与原析构时点完全一致），`sendStreaming` 三处裸指针取值改
  `.get()`（CURLOPT_WRITEDATA / CURLOPT_HEADERDATA / 线程 lambda 捕获的 implPtr）。
- 经典坑处理：`unique_ptr<Impl>` 的默认 deleter 在**析构函数定义点**实例化、要求
  Impl 完整——析构函数保持定义在 http_client.cpp（Impl 定义之后）不内联，头文件
  成员处与析构函数处均有注释说明，防止后人挪回头文件编译炸。
- 行为零变化：全量单测 Total 1586 / Failed: 0（与基线 1586 / 0 持平，无增删用例）；
  probe 门禁段 exit 1（agent_e2e_probe / home_probes×2）为本机缺凭据的环境性既有
  状态（任务口径明示忽略）。
