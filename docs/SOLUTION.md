# 方案文档：Mooncake KVCache 分层存储设计与性能优化

> 第八届 CCF 开源创新大赛「Mooncake KVCache 存储设计和性能优化」赛道参赛作品
>
> 源代码：本仓库（GitHub 托管）｜基准报告：[docs/benchmarks/REPORT.md](benchmarks/REPORT.md)
> ｜上游分析：[docs/analysis/upstream-mooncake-analysis.md](analysis/upstream-mooncake-analysis.md)

## 1. 问题与目标

LLM 推理中 prefill 阶段的 KVCache 计算昂贵，Mooncake 的「以存换算」思想通过
池化共享 KVCache 复用前缀计算。本作品围绕 KVCache **存储层**做设计与优化，
目标：

1. **更高命中率**：同等 HBM 预算下，把空闲主机 DRAM 变现为第二层缓存；
2. **更优淘汰/准入**：抵抗一次性长前缀扫描冲刷，保住高复用块；
3. **更低搬运开销**：前缀感知预取把 slow→fast 搬运隐藏到请求间隙；
4. **生产可用**：C++ 核心 + pybind11 绑定 + vLLM connector + 上游
   Transfer Engine（RDMA→TCP 回退）接线，属性测试与消毒器矩阵守护正确性。

## 2. 总体架构

```
vLLM (Python)
   │  KVConnector（python/vllm_adapter/connector.py）
   ▼
pybind11 绑定（src/bindings）
   ▼
C++ 核心
 ├─ TieredStorageManager（src/storage/tiered_storage_manager.*）
 │    HBM(fast, S3-FIFO) ▸ DRAM(slow, 纯 LRU) ▸ NVMe(可选)
 ├─ ShardedTieredStore（16 分片并发，src/storage/sharded_tiered_store.h）
 ├─ PrefixPrefetcher（后继图 + 多锚点链式预取，src/storage/prefix_prefetcher.*）
 ├─ Scheduler（前缀索引调度，src/scheduler）
 └─ TransferEngineAdapter（上游 Mooncake Transfer Engine C ABI，
      RDMA→TCP 回退、异步 future、RAII 注销，src/adapter）
```

## 3. 核心技术点

### 3.1 分层存储（HBM ▸ DRAM ▸ NVMe）

- **fast 层（HBM 当量）淘汰器：S3-FIFO（SOSP'23）**。小队列（10%）拦截
  一次性访问，主队列二次机会保住热块，幽灵队列（2×容量）记录被逐冷块键，
  幽灵命中的再写入以「热身份」直接入主队列——对一次性长前缀扫描有天然抗性。
- **slow 层（DRAM 当量）淘汰器：纯 LRU**。降级入层刷新逻辑时钟（=按降级
  顺序排序），命中即晋升离层。经四条真实 trace 的分层混合选型
  （fast×slow 策略矩阵）验证 fast=S3-FIFO + slow=LRU 全面胜出。
- **fast 淘汰即降级**：被逐块降级至 slow 而非丢弃——slow 层等价于
  「实体化的幽灵队列」，消融实验（no-demote）显示该链路贡献吞吐 14%～42%。
- **O(1) 淘汰路径**：受害者选择用有序索引（S3-FIFO 队列 / LRU 有序集）
  单次选取，conversation 全量回放 >280s → 0.68s（>400×），命中率逐位一致。

### 3.2 前缀感知预取

- 维护块间**后继图**（前缀链 A→B→C…），请求到达时以其末端命中块为锚点，
  沿链预测后续块并把 slow 层驻留者**提前晋升**到 fast 层；
- **多锚点**：对请求中每个命中块并行发起链预测，深度预算含请求长度项；
- 预取搬运计入请求间隙而非关键路径，TTFT 直接受益。

### 3.3 并发与工程化

- **ShardedTieredStore**：按块键哈希 16 分片，分片内独立
  TieredStorageManager + 独立锁；2 vCPU 上混合读写吞吐 1.56×
  （3.54M→5.54M ops/s），多核近线性。
- **上游接线**：锁定上游 Mooncake commit（mooncake.lock），Transfer Engine
  经 C ABI 封装（RDMA 优先、TCP 回退、异步 future、RAII 注销），CI 编译
  门禁保证 ABI 兼容。
- **质量守护**：22 项 C++ RapidCheck 属性测试 + Python Hypothesis 属性
  测试（各 100 迭代）、ASan/UBSan/TSan 三套消毒器矩阵、clang-tidy 0 警告
  门禁、pytest 29 项、ctest 9 项，CI 全绿。

## 4. 评测方法与结果

### 4.1 方法论

FAST'25 真实 trace（arxiv / conversation / synthetic / toolagent，共
1,229,349 块访问）确定性回放：先在 Python 模型上做策略选型（bench.shootout
/ bench.tiered），胜出配置落地 C++ 生产路径，再用 `bench.replay_cpp` 驱动
真实 C++ 存储引擎全量回放验证迁移性。固定种子 + 锁定上游版本保证可复现。

### 4.2 命中率（C++ 生产路径，fast=4096 块 / slow=4×）

| Trace | 基线（分段 LRU） | 本方案 | 模型参考 |
|---|---|---|---|
| arxiv（23,608 请求） | 0.462 | **0.519** | 0.516 |
| conversation（12,031） | 0.222 | **0.297** | 0.292 |
| synthetic（3,993） | 0.588 | **0.586** | 0.588 |
| toolagent（23,608） | 0.462 | **0.519** | 0.516 |

C++ 生产路径在 3/4 trace 上**追平并略超**模型参考（多锚点预取 + slow 命中
立即晋升的额外收益）。

### 4.3 分层容量扩展（fast=4096 固定，slow 因子递增）

| slow 因子 | arxiv | conversation | synthetic | toolagent |
|---|---|---|---|---|
| 1× | 0.447 | 0.194 | 0.395 | 0.447 |
| 2× | 0.483 | 0.243 | 0.477 | 0.483 |
| 4×（默认） | 0.519 | 0.297 | 0.586 | 0.519 |
| 8× | 0.547 | 0.343 | 0.638 | 0.547 |

HBM 占用恒定，命中率随可用 DRAM 单调抬升（最高 +24pp）——「以存换算」
的分层收益随主机内存预算线性变现。

### 4.4 端到端回放指标（vs vLLM 前缀缓存基线，详见 REPORT.md）

- synthetic 吞吐 **+18.8%**、TTFT p50 **−71%**；
- arxiv/toolagent 吞吐 **+7.1%**；conversation 命中率 **2.7×**。

### 4.5 负结果（已探索未采纳，避免重复探索）

SIEVE 替代 fast 层（容量敏感、小缓存回退）、后继频次投票（持平）、slow
读穿不晋升（微降）、预取深度 >8（饱和）、no-demote（吞吐 −14%～−42%）。
详见 REPORT.md「已探索未采纳」。

## 5. 复现指引

```bash
cmake -B build -DBUILD_PYTHON_BINDINGS=ON -DENABLE_RAPIDCHECK=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure          # C++ 测试
export PYTHONPATH=python:build/src
python3 -m pytest tests                              # Python 测试
python3 -m bench.replay_cpp \
  third_party/mooncake/FAST25-release/arxiv-trace/mooncake_trace.jsonl 4096 4
python3 -m bench.framework \
  third_party/mooncake/FAST25-release/traces/conversation_trace.jsonl
```

## 6. 局限与后续工作

- 真实 GPU/RDMA 硬件基准（GPUDirect、eRDMA）按能力探测显式跳过，待硬件
  就绪后可直接补测（框架已预留）；
- vLLM 在线验证已打通 connector 链路与端到端测试，生产部署验证待 NVIDIA
  环境；
- 分片间统计共享经分析为无效项（哈希分片下幽灵记录天然本地化）。
