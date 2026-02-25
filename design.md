# PIM-ALEX 设计文档（Design）

本文档给出在 UPMEM PIM 硬件上分层卸载 ALEX learned index 的详细设计，包括架构、索引操作语义、数据布局以及 Host↔DPU 协议。实现时请优先遵循本文档的约束和不变量。

---

## 1 架构设计

### 1.1 分层卸载总览

整体架构分为三部分：

- **CPU 侧 ALEX internal nodes**
  - 使用 ALEX 原生的 internal node 结构构建路由层，只保留在 CPU 运行。
  - 查找时，只需通过线性模型（如 `child = a * key + b` 量化取整）计算子节点下标，然后跟随指针；internal nodes 按构造保证 perfect accuracy，不需要在 internal 层做搜索。
  - internal node 的 children 指向：
    - 其他 internal node，或
    - 逻辑 leaf 的描述结构 `LeafRef`（绑定到具体 DPU + DPU 内 leaf index）。

- **DPU 侧 leaf nodes（数据节点）**
  - 每个 leaf 存在于某个 DPU 的 MRAM 中，以 **gapped array + occupancy/tombstone bitmap + block index** 形式存储。
  - 叶内支持：
    - Lookup：PSQR 预测 + window 扫描 + fallback 到 block index 搜索。
    - Insert：在物理有序的 KV 中找插入点，搜索最近 gap，通过块化 DMA 搬移制造空位。
    - Delete：仅设置 tombstone，不做实时 compaction。
  - 每个 leaf 固定绑定到某个 DPU 和该 DPU 内的 `leaf_local_id`，MVP 阶段不做跨 DPU 迁移或复制。

- **Host 侧 per-leaf overflow hash map**
  - 当 leaf 在 DPU 上空间不足（或插入会导致密度超过上界）时，DPU 返回 `OVERFLOW_NEEDED`，主机将该写入放进对应 leaf 的 overflow hash map。
  - 查找优先访问 overflow：
    - 命中 `PRESENT`：返回值。
    - 命中 `TOMBSTONE`：直接视为 NOT_FOUND，忽略 DPU 上可能残留的旧值。
    - 未命中：再下发给 DPU leaf。

通过上述分层，ALEX 的 internal node 逻辑完全留在 CPU；DPU 只负责叶内的局部搜索和更新。

---

## 2 索引操作语义

### 2.1 键值与重复键

- 键类型：`uint64_t`。
- 值类型：`uint64_t`（后续可扩展为 payload）。
- MVP 语义默认 **不允许重复键**：
  - `insert(key, value)`：
    - 若该 key 在“可见集合”（overflow + DPU leaf）中已存在，则返回 `DUPLICATE`。
  - `update(key, value)`：
    - MVP 实现为 `delete(key)` + `insert(key, value)` 的等价序列（同一 write epoch 内保持顺序）。
- 保留对 `multimap` 场景的扩展空间，但不在 MVP 实现范围内。

### 2.2 可见性与优先级规则

- **overflow 优先级**：查询时顺序为
  1. 访问 host 侧 per-leaf overflow hash map；
  2. 若 overflow `PRESENT`：返回该值；
  3. 若 overflow `TOMBSTONE`：直接返回 NOT_FOUND；
  4. 否则下发到 DPU leaf。
- **delete 的效果**：
  - DPU 侧通过 tombstone 标记某个 occupied 槽位为“逻辑删除”。
  - host 侧 overflow 的 `TOMBSTONE` 可以屏蔽 DPU 上旧值。
- **update 的等价性**：
  - 在 epoch 模型中，读不与写并发；因此 `delete + insert` 和 DPU 端“原地修改 value 并清除 tombstone”在最终可见性上是等价的。
- **scan 语义**：
  - 仅在**完全静态阶段**（从建表到扫描期间没有任何写操作）支持 `scan_static`。
  - 一旦发生任意写批次，后续所有 scan 请求都应在 host 直接返回 `SCAN_UNSUPPORTED_AFTER_UPDATES`。

### 2.3 epoch / batch 模型

- 系统一次只处于两种模式之一：
  - **query epoch**：只发送 lookup 批处理；host overflow 只读，DPU leaf 只读。
  - **write epoch**：只发送 insert/delete/update 批处理；host overflow 只写，DPU leaf 只写。
- 不允许将读写请求混合打包给 DPU，也不允许在 DPU 内同时处理读写。
- 每个 epoch 内的批量操作通过 rank pipeline 进行 Host↔DPU 传输与执行（详见下节）。

---

## 3 Host↔DPU 协议与批处理

### 3.1 批处理约束

- 所有 DPU 在一次并行 `dpu_push_xfer` 调用中，transfer size 必须相同，因此：
  - `req_buf` / `resp_buf` 在 MRAM 中设计为固定大小的 mailbox；
  - 每个 batch 内实际使用条目数 `n` 存在 header 中。
- 所有 Host↔MRAM 传输的 offset/size 必须是 8 字节的整数倍。
- 所有 MRAM↔WRAM DMA 调用必须满足：
  - 长度 ≤ 2048 字节；
  - 且是 8 字节的整数倍；
  - 起始地址 8 字节对齐。

### 3.2 Query 批格式（CMD_QUERY）

- Header（32B，对齐 8）：
  - `magic:uint32_t`
  - `cmd:uint16_t`（=CMD_QUERY）
  - `n:uint16_t`（实际请求条数，≤ MAX_Q_PER_DPU）
  - `epoch_id:uint32_t`
  - `flags:uint32_t`（例如 BIT0=NEED_VALUE）
  - `reserved[16]`
- Entry（16B/条，对齐 8）：
  - `key:uint64_t`
  - `leaf_local_id:uint16_t`
  - `reserved0:uint16_t`
  - `reserved1:uint32_t`

### 3.3 Write 批格式（CMD_WRITE）

- Header（32B）：
  - `magic:uint32_t`
  - `cmd:uint16_t`（=CMD_WRITE）
  - `n_ops:uint16_t`（总 op 数）
  - `n_segs:uint16_t`（leaf 段数量）
  - `reserved0:uint16_t`
  - `epoch_id:uint32_t`
  - `flags:uint32_t`（例如 BIT0=ALLOW_UPDATE_INPLACE）
  - `reserved[12]`
- Leaf segment 描述（8B/段）：
  - `leaf_local_id:uint16_t`
  - `start:uint16_t`（在 op 数组中的起始下标）
  - `len:uint16_t`（本 leaf 的 op 数）
  - `reserved:uint16_t`
- WriteOp（24B/op，对齐 8）：
  - `key:uint64_t`
  - `value:uint64_t`
  - `leaf_local_id:uint16_t`
  - `op:uint8_t`（INS/DEL/UPD）
  - `reserved0:uint8_t`
  - `reserved1:uint32_t`

### 3.4 DPU→Host 响应格式

- Header（32B）：
  - `magic:uint32_t`
  - `cmd:uint16_t`
  - `n:uint16_t`
  - `epoch_id:uint32_t`
  - `status:uint32_t`（整体错误码等）
  - 其余 reserved
- RespEntry（16B）：
  - `status:uint16_t`（0=OK/HIT,1=NOT_FOUND,2=ERR）
  - `err:uint16_t`（DUPLICATE/OVERFLOW_NEEDED/DELETED/...）
  - `aux:uint32_t`（例如插入位置、移动字节数等）
  - `value:uint64_t`（lookup 命中时返回值）

---

## 4 数据布局与内存组织

### 4.1 Host 侧结构

#### LeafRef（CPU 路由结果）

```cpp
struct LeafRef {
    uint32_t leaf_id;        // 全局 leaf id
    uint16_t dpu_id;         // 目标 DPU
    uint16_t leaf_local_id;  // DPU 内 leaf_table 索引
    uint64_t key_min_init;   // 构建期 key 范围（debug）
    uint64_t key_max_init;
    OverflowTable* overflow; // 指向 per-leaf overflow hash map
    uint32_t flags;          // 边界 leaf 等标记
    uint32_t pad;            // 对齐填充
};
```

#### Overflow hash map（per-leaf）

- 状态机：
  - `EMPTY=0`, `PRESENT=1`, `TOMBSTONE=2`
- 入口（24B，对齐 8）：

```cpp
struct OverflowEntry {
    uint64_t key;
    uint64_t value;
    uint8_t  state;
    uint8_t  pad[7];
};
```

- OverflowTable 策略：
  - capacity 为 2 的幂，线性探测；
  - load factor 超过阈值或满时返回 `OVERFLOW_TABLE_FULL`（MVP 可直接统计失败）。

### 4.2 DPU MRAM 布局（每个 DPU）

```text
mram_global_cfg_t   (64B)
leaf_table[]        (num_leaves * 64B)
req_buf             (固定大小 mailbox)
resp_buf            (固定大小 mailbox)
leaves_region       (所有 leaf 的 KV / bitmaps / block index)
```

#### `mram_global_cfg_t`（64B）

见 agents.md 简要。这里略。

#### `leaf_desc_t`（64B）

见 agents.md 简要。这里略。

### 4.3 KV gapped array 与 bitmap

- 每个 slot 固定 16B：`{ key:uint64_t, value:uint64_t }`。
- 物理上按 2048B block 划分，每个 block 128 个 slot。
- occupancy bitmap：
  - 1 表示该 slot 被占用（无论是否 tombstone），0 表示 gap。
- tombstone bitmap：
  - 1 表示该 slot 为 tombstone，0 表示有效数据。
- 每个 block 的 bitmap 占用 32B（16B occupancy + 16B tombstone），保证可按 16B/32B 的粒度 DMA 访问。

### 4.4 Block index

- `block_index[b] = first_key(b)`，每条 8B。
- 要求：`n_blocks * 8B ≤ 2048B`，因此 `n_blocks ≤ 256`，上限 `leaf_capacity ≤ 32768`。
- 用途：
  - block 模式下的 block 定位；
  - PSQR 模式 fallback 时快速界定候选 block。

---

## 5 Leaf 内算法

### 5.1 PSQR 模型（模式一）

- 范围归一化：
  - `x = saturate((key - key_min) >> t, 0, x_max)`
- 预测：
  - `y = (A * x + B) >> s`
  - `center = clamp(y, 0, capacity_slots - 1)`
- Window 扫描：
  - 在 `[center - W, center + W]` 所覆盖的若干 block 内，顺序扫描 occupied 且非 tombstone 的 slot。
  - 若遇到第一个 occupied key > target，提前返回 NOT_FOUND。
  - 若 window 未命中且无法确定 NOT_FOUND，则 fallback。

### 5.2 Block index 模式（模式二）

1. DMA 读取整个 `block_index`（≤2048B）到 WRAM。
2. 在 WRAM 中用二分查找找到最大 `b` 使 `first_key[b] ≤ key`。
3. DMA 读取该 block（2048B），配合 bitmap 做线性扫描：
   - 跳过 gap；
   - 对 occupied 且非 tombstone 的 slot 做有序比较，>target 即早停。

### 5.3 Insert 算法

见设计文档原文，此处只强调关键步骤：

1. 检查 density 阈值：若 `(n_used + 1) / capacity_slots > d_u`，返回 `OVERFLOW_NEEDED`。
2. `leaf_find` 检查重复键或 tombstone 槽：
   - 已存在且有效：`DUPLICATE`。
   - 已存在且 tombstone：可以直接“复活”，原地写新 value 并清 tombstone。
3. `leaf_lower_bound_slot` 找到物理插入位置 `pos`。
4. `find_nearest_gap(pos)` 找最近 gap。
5. 根据 gap 在左/右决定向左或向右搬移，按 2048B block 颗粒做 DMA shift。
6. 在 pos 写入 KV，更新 bitmap 和 `n_used`，并更新相应 block 的 `first_key`。

### 5.4 Delete 算法

1. `leaf_find` 定位 key 所在的 slot。
2. 若未找到：`NOT_FOUND`。
3. 若已是 tombstone：`NOT_FOUND` + `DELETED`（仅用于统计）。
4. 否则设置 tombstone bit，并增加 `n_tomb`。

---

## 6 并行与一致性

- Host 侧通过 stable sort + leaf segment 将同一 leaf 的所有写 op 放在连续区间内，下发给 DPU 时保证“每个 leaf 在一个 batch 内只有一个 tasklet 写入”。
- DPU 侧通过 `for (seg_idx = tid; seg_idx < n_segs; seg_idx += NR_TASKLETS)` 将 leaf 段分配给不同 tasklet，避免 mutex。
- query epoch 与 write epoch 严格隔离，消除跨 epoch 的读写并发。

---

## 7 错误码与诊断

参见 agents.md 中的 unified status / err 定义。实现时：

- DPU 内部使用细粒度错误码便于 debug。
- Host 对用户暴露统一语义（例如将 `DELETED` 视为 NOT_FOUND）。
