# PIM-ALEX 实现计划（Plans）

本文档给出在现有 PIMLex 框架基础上实现 PIM-ALEX 的分步计划。目标是在保证每一步可编译、可运行、便于 debug 的前提下，逐步完成从 PGM 到 ALEX 的迁移。

---

## 阶段划分总览

1. **Phase 0：环境与基线确认**
2. **Phase 1：数据结构与公共头文件重构**
3. **Phase 2：Host 侧 ALEX internal node & leaf 构建 + 下发**
4. **Phase 3：DPU lookup kernel（只读路径）**
5. **Phase 4：DPU write kernel（插入/删除）**
6. **Phase 5：Host overflow 集成 + 写路径闭环**
7. **Phase 6：scan_static 支持**
8. **Phase 7：Benchmark 集成与调优**

每个 Phase 完成后都应能单独运行一小组回归测试（可在 `test.sh` 中增加对应条目）。

---

## Phase 0：环境与基线

**目标**：确认现有 PIMLex 在本机 UPMEM 平台上可稳定运行，为后续对比提供 baseline。

- [ ] 在目标机器上安装 UPMEM SDK，并配置好 `dpu-pkg-config`。
- [ ] 使用原版 PIMLex（PGM 实现）在两个典型数据集上跑通：
  - `genome` 200M，search / insert / mix / scan。
  - `books_800M_uint64` 800M，search / insert / mix。
- [ ] 记录：
  - 吞吐量（ops/s）与 p50/p99 延迟。
  - Host↔DPU 传输字节数/批（可以通过简单的计数器估计）。
  - DPU 数量与 tasklets 数配置。

---

## Phase 1：数据结构与公共头文件重构

**目标**：在不触动 PGM 逻辑的前提下，引入 ALEX 所需的数据结构。

- [ ] 在 `support/common.h` 中新增或替换：
  - `mram_global_cfg_t`：全局配置。
  - `leaf_desc_t`：叶描述结构。
  - 新的 request / response buffer 结构体（query / write）。
  - 统一的 `status_code` / `err_code`。
- [ ] 确保所有 struct：
  - 字节大小是 8B 的整数倍；
  - 字段布局在 Host 和 DPU 上完全一致。
- [ ] 在 `host/flags.h` 中预留新的 CLI 选项（暂时可以不生效）：
  - `--search_mode`, `--window_W`, `--d_l`, `--d_u`, `--leaf_capacity`, `--max_q_per_dpu` 等。
- [ ] 编译 Host/DPU 代码，确认增加 struct 后仍可通过编译（PGM 路径暂时不使用它们）。

---

## Phase 2：Host 侧 ALEX internal nodes 与 leaf 构建

**目标**：基于 CPU 版 ALEX，实现 internal nodes + leaf 列表的构建，并完成叶数据的序列化和下发。

- [ ] 在 `host/` 新增适配代码，将 `ALEX_core/` 中的实现封装为：
  - 构建函数：`build_alex_index(sorted_keys, values)`。
  - 遍历函数：获取所有 leaf 节点的 key 范围与元素序列。
- [ ] 从每个 leaf 中抽取：
  - `key_min`, `key_max`, `records`。
  - 计算 `leaf_capacity`（基于 `d_l`, `d_u`）。
  - 通过 `pos_i = floor(i / d_target)` 决定物理 slot 布局。
- [ ] 对每个 leaf：
  - 拟合局部线性模型（浮点），并量化为 PSQR `(A, B, s, t)`。
  - 构造 occupancy / tombstone bitmap 与 block index。
  - 将 KV + bitmap + block index 按 DPU MRAM 布局打包。
- [ ] Leaf→DPU 映射策略：
  - 简单起见先用 round-robin：leaf 依次分配给 DPU。
  - 为每个 leaf 生成 `LeafRef{leaf_id, dpu_id, leaf_local_id}`。
- [ ] 在 `host/pimlex_host.cpp` 中增加一个新的 index 类型（可通过 flag 选择）：
  - 保留 PGM 路径作为 fallback，新增 ALEX 路径用于后续实验。
  - 但此阶段可以只实现 build + 下发，query/write 仍临时用 CPU-only ALEX 做 sanity check。

---

## Phase 3：DPU lookup kernel（只读）

**目标**：替换 DPU 端 PGM 查找逻辑，实现 ALEX leaf 的 lookup 两种模式，并保持 write 路径暂时禁用。

- [ ] 重写 `dpu/task.c`：
  - MRAM 符号：`mram_global_cfg`, `leaf_table`, `req_buf`, `resp_buf`。
  - WRAM 结构：
    - `leaf_table_cache[num_leaves]`。
    - `dma_buf[NR_TASKLETS][2048]`。
  - `main()` 中根据 `cmd` 区分 `CMD_QUERY` 和（未来的）`CMD_WRITE`。
- [ ] 实现 query kernel：
  - tasklet 0 将 `leaf_table` 整块读入 WRAM 并 `barrier_wait`。
  - 每个 tasklet 以 stride 方式处理 query entries。
  - 根据 `cfg.search_mode` 调用：
    - `leaf_lookup_psqr(leaf, key, cfg)`;
    - `leaf_lookup_block(leaf, key, cfg)`;
  - 将结果写回 `resp_buf`。
- [ ] 在 Host 侧：
  - 将原 PGM 查找路径替换为 ALEX internal nodes → DPU leaf lookup。
  - 保持写路径仍走 CPU-only 或直接禁用 insert/delete，先保证 lookup-only 正确性。
- [ ] 测试：
  - 使用小规模数据集（如 1M key），比对 CPU-only ALEX 与 PIM-ALEX lookup 的结果一致性。
  - 检查：
    - DMA 调用是否全部满足 8 字节对齐与 2048 字节上限。
    - `leaf_table_cache` 是否在 WRAM 内存预算之内。

---

## Phase 4：DPU write kernel（插入/删除）

**目标**：在 DPU 端实现每 leaf 单写者的插入/删除逻辑，并打通 CMD_WRITE 基础路径。

- [ ] 扩展 `dpu/task.c`：
  - 增加 `CMD_WRITE` 分支。
  - 读取 write header 和 leaf segments 数组。
  - `for (seg_idx = tid; seg_idx < n_segs; seg_idx += NR_TASKLETS)` 为每个 leaf 段选择一个 tasklet。
- [ ] 实现 `leaf_insert`：
  - 检查 density 上界，超阈值直接返回 `OVERFLOW_NEEDED`。
  - 调用 `leaf_find` 检查重复键或墓碑“复活”。
  - `leaf_lower_bound_slot` + `find_nearest_gap` + 块化 `shift`。
  - 执行写入与 bitmap 更新，以及 block index 的局部维护。
- [ ] 实现 `leaf_delete`：
  - `leaf_find` + 设置 tombstone bit。
- [ ] Host 侧只需：
  - 调用 route + bucket + stable sort + segment build。
  - 将 write 请求打包为 CMD_WRITE，下发 DPU 并收集响应。
  - 先不接 overflow，DPU 直接返回 `OVERFLOW_NEEDED` 供统计观察。

---

## Phase 5：Host overflow 集成与写路径闭环

**目标**：在 Host 侧实现 per-leaf overflow hash map，使写路径达到论文中“ leaf 下沉 + overflow 兜底 ”的语义闭环。

- [ ] 在 Host 侧实现 `OverflowTable`：
  - 开放寻址 + 线性探测；
  - 支持 `get/insert/delete` 三种操作。
- [ ] 写路径处理：
  - 对每个写 op 的 DPU 响应：
    - `OVERFLOW_NEEDED`：将 op 转写到对应 leaf 的 overflow 表；
    - 其他错误码：按语义统计（如 `DUPLICATE`、`NOT_FOUND`）。
- [ ] 读路径处理：
  - 路由到 leaf 后，先查询 overflow：
    - `PRESENT`：直接返回；
    - `TOMBSTONE`：返回 NOT_FOUND；
    - `EMPTY`：才下发到 DPU。
- [ ] 回归测试：
  - 大量 insert，直到 DPU leaf 饱和，验证：
    - overflow 负载增长趋势；
    - 查询正确性（CPU-only ALEX 对照）。

---

## Phase 6：scan_static 支持

**目标**：实现在完全静态阶段的 scan 功能，并在第一次写入后强制禁用 scan。

- [ ] DPU 端：
  - 实现 `CMD_SCAN` 逻辑或复用查询命令中的专用模式：
    - 参数：`leaf_local_id`, `start_slot`, `max_out`。
    - 行为：从 `start_slot` 起按 block 扫描，输出最多 `max_out` 条有效 KV。
  - 要求 `cfg.writes_seen == 0`，否则返回错误码或空结果。
- [ ] Host 端：
  - 维护一个全局标志，一旦发生 write，就不再下发任何 scan 请求。
  - 对用户暴露统一错误 `SCAN_UNSUPPORTED_AFTER_UPDATES`。
- [ ] 验证：
  - 使用静态数据集跑大范围 scan，对比 CPU-only ALEX 的顺序输出。

---

## Phase 7：Benchmark 集成与调优

**目标**：将 PIM-ALEX 完整接入 PIMLex benchmark 框架，并进行参数 sweep 和性能评估。

- [ ] 集成 CLI：
  - `--search_mode={psqr,block}`；
  - `--window_W`；
  - `--d_l`, `--d_u`；
  - `--leaf_capacity`；
  - `--max_q_per_dpu`；
  - 以及原有 `--search`, `--insert`, `--mix`, `--scan` 等。
- [ ] 实现 metric 采集：
  - 吞吐（ops/s）、p50/p99 延迟；
  - Host↔DPU 传输字节数；
  - 写放大（每次 insert 搬移的字节数估计）。
- [ ] 实验计划（可写入论文 methodology）：
  - 不同 search_mode 的对比；
  - window W sweep；
  - d_u / leaf_capacity sweep；
  - MAX_Q_PER_DPU sweep；
  - tasklets 数的敏感性分析（如果硬件允许修改）。

---

## 里程碑与验收标准

- **M1：Lookup-only 成功**
  - Phase 3 完成，小规模数据集上 lookup 与 CPU-only ALEX 对齐。
- **M2：Write 路径打通**
  - Phase 4+5 完成，在 moderate 规模数据上 insert/delete 正确，溢出行为符合预期。
- **M3：Static scan 支持**
  - Phase 6 完成，可以对静态阶段的数据做全量或分段 scan。
- **M4：完整 benchmark**
  - Phase 7 完成，可以与 CPU-only ALEX / 原 PIMLex 做系统性对比实验。
