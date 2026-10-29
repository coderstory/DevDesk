# developer 角色经验沉淀（一事一行带日期与来源，与既有规则重复不记）

- 2026-10-29（memcpp MCP 集成，审查 P1 打回 R1）：`platform/json` 的 JsonImpl Int/Double 字段互斥写入（json.cpp valToImpl），`asInt()` 对 Double 形态恒返回 0——凡「客户端可传浮点」的数值字段禁止裸 `asInt()`：先按形态分路，Double 走 `asDouble()` + `floor(d)==d` 整值判定，域内钳制后再收窄；单测必须覆盖 double 形态（3.7→默认 / 2.0→采纳 / 1e20→钳顶）。
- 2026-10-29（memcpp MCP 集成 R2）：「写盘失败可上报」类修复要查全 `fwrite`/`fclose` 返回值——磁盘满最常在短写/冲刷显形，只查 fopen 等于没堵住；半截文件 + 下次 load 全默认 = 静默状态复活链。
