# PIM-ALEX Agents Guide

本仓库在真实 UPMEM PIM 硬件上实现分层卸载的 ALEX learned index，基于 PIMLex 框架复用其主机↔DPU 通信、任务分发和 benchmark 逻辑，将原本的 PGM 索引内核替换为 ALEX 内核（CPU 侧 internal nodes + DPU 侧 leaf nodes）。

> 更详细的架构与数据布局见 `design.md`，更细粒度的开发分步计划见 `plans.md`。

---

## 项目概览

PIM-ALEX 的核心思想是将 ALEX 拆分为两层：

- **CPU 侧：ALEX internal nodes**
  - 保留完整的 ALEX 内部节点结构，用线性模型 `a * key + b` 进行路由，内部节点通过构造保证 perfect accuracy，无需搜索。
  - 路由结果是逻辑 leaf，映射到具体 DPU 和 DPU 内的 `leaf_local_id`。

- **DPU 侧：ALEX-style leaf nodes**
  - 每个 leaf 以 gapped array + bitmap 的形式存放在 MRAM 中，负责局部搜索和动态插入/删除。
  - 叶内查找走整数化的 PSQR 模型或 block index 搜索，插入通过“找最近 gap + 块化搬移”完成，删除用 tombstone 标记。

- **Host 侧 overflow**
  - 每个 leaf 有一个 host 端 overflow 结构（开放寻址 hash map + tombstone），承接 DPU 报告 `OVERFLOW_NEEDED` 的写入，并在查询时优先于 DPU 返回结果。

- **批处理与 epoch 模型**
  - 系统以 query epoch / write epoch 为单位推进，查询批和写入批绝不混发，从而避免 DPU 内部的读写并发控制。

---

## 仓库结构与关键文件

本分支：`PIM-ALEX`。

```text
PIMLex_FAST/
├── agents.md          # 面向 AI 代理 / 开发者的总览与导航（本文件）
├── design.md          # 详细设计：架构、语义、数据布局
├── plans.md           # 实现计划：分阶段任务与优先级
├── Makefile           # 编译配置（host + dpu）
├── README.md          # 原 PIMLex README（使用方法与数据集说明）
├── test.sh            # 典型 benchmark 命令示例
├── ALEX_core/         # ALEX 参考实现（只读，CPU 端）
│   ├── alex.h
│   ├── alex_base.h
│   ├── alex_nodes.h
│   ├── alex_fanout_tree.h
│   ├── alex_map.h
│   └── alex_multimap.h
├── host/              # 主机侧 C++17 代码
│   ├── pimlex_host.cpp         # 主流程：数据加载、任务编排、host↔DPU 通信、benchmark
│   ├── pgm_index.hpp           # 现有 PGM 索引实现（将在 PIM-ALEX 中被 ALEX 替换）
│   ├── piecewise_linear_model.hpp # PGM 模型拟合（后续可逐步废弃）
│   ├── dram_index.h            # DRAM 端索引抽象（外部接口）
│   ├── flags.h                 # 命令行参数定义
│   ├── utils.h                 # 常用工具函数
│   └── zipf.h                  # Zipf 分布样本生成器
├── dpu/
│   └── task.c                  # DPU 端 kernel：请求解析、leaf 处理（将重写为 ALEX 叶逻辑）
└── support/                    # Host/DPU 共享头文件
    ├── common.h                # 公共常量、buffer 结构体、DPU 启动参数等
    ├── concurrent.h            # 并发相关辅助
    ├── opt_overflow_tree.h     # PIMLex 的 overflow 结构（将演化为 per-leaf hash map）
    ├── timer.h                 # 计时工具
    ├── tscns.h                 # TSC 纳秒计时
    └── typedefine.h            # 自定义类型（例如 string payload）
```

**后续在 PIM-ALEX 中的预期改动方向：**

- **复用为主**：
  - `host/pimlex_host.cpp` 中的 DPU 分配、rank pipeline、workload 生成与统计逻辑。
  - `host/zipf.h`, `host/utils.h`, `support/timer.h`, `support/tscns.h` 等 benchmark 相关工具。
- **重写 / 替换**：
  - `host/pgm_index.hpp` 与 `host/piecewise_linear_model.hpp`：PGM 路由逻辑将被 ALEX internal nodes 替换。
  - `dpu/task.c`：从基于 PGM 的定位改为 ALEX-style leaf（PSQR / block index + gapped array）。
  - `support/common.h`：模型参数、buffer 布局、错误码等结构体按 ALEX 设计重构。
  - `support/opt_overflow_tree.h`：从“tree”演化为每个 leaf 一个开放寻址 hash map。

---

## 硬件与 SDK 约束（UPMEM 必备知识）

> 详细的数据布局和 DMA 颗粒度设计在 `design.md` 中有完整说明，这里只列出对编码有直接影响的关键约束。

### DPU 计算与内存模型

- 每个 DPU 具有约 **64 KB WRAM**（工作内存，无 cache）和最大 **64 MB MRAM**（主存，通过 DMA 访问）。
- DPU 上运行若干 tasklet（硬件线程），本项目默认使用 `NR_TASKLETS = 16`。
- **DPU 无硬件浮点单元**：DPU 代码必须全部使用整数运算（加减乘移位），PSQR 模型必须是整数定点形式。

### MRAM ↔ WRAM DMA 规则

所有 `mram_read()` / `mram_write()` 必须满足：

- 传输大小是 **8 字节的整数倍**，且不超过 **2048 字节**（本项目用 2048B 作为 block 粒度）。
- MRAM/WRAM 地址均需 **8 字节对齐**，否则可能出现静默对齐导致数据错位。
- WRAM 中用作 DMA 缓冲的数组需要按 `__dma_aligned` 对齐。

### Host ↔ DPU 传输规则

- 通过 `dpu_prepare_xfer()` + `dpu_push_xfer()` 完成主机与 DPU 之间的并行传输。
- 同一次并行 `dpu_push_xfer` 中，**所有参与的 DPU 传输大小必须相同**，因此需要为每个 DPU 设计固定大小的 `req_buf` / `resp_buf`。
- 传输的 offset 与 size 必须都是 8 字节的整数倍。

### 构建与运行（简要）

- 编译：
  - Host：`g++ -std=c++17 -fopenmp -ltbb -ljemalloc ... \`dpu-pkg-config --cflags --libs dpu\``
  - DPU：`dpu-upmem-dpurte-clang ...`
- 典型构建命令（根据实际硬件调整）：
  - `NR_DPUS=512 NR_TASKLETS=16 make all`
- 运行方式与 PIMLex 一致，例如：
  - `./bin/pimlex_host --keys_file=... --init_num_keys=... --query_num=... --total_num_keys=... --search`

更多关于 ALEX 分层卸载、索引语义、数据结构和批处理协议，请参考 `design.md`。关于如何逐步在现有 PIMLex 基础上实现 PIM-ALEX，请参考 `plans.md`。
