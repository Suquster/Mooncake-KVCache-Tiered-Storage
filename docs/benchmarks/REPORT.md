# FAST'25 真实 Trace 基准报告

基准框架：`python -m bench.framework <trace.jsonl>`（python/bench/framework.py，
确定性回放模拟；baseline 为 LRU 单层池，project 为两级分层池（fast/HBM 当量
+ slow/DRAM 当量 4×，fast=S3-FIFO / slow=LRU 混合选型）+ 前缀感知预取，
策略经四条真实 trace 选型胜出，见下文「缓存策略选型」与「两级分层 + 前缀感知
预取」）。

- 上游 Mooncake 锁定版本：`0.3.6.post1`（commit `356d99fb2874`）
- Trace 来源：上游子模块 `third_party/mooncake/FAST25-release/`（arxiv / conversation / synthetic / toolagent）
- 环境无 GPU/NVMe/RDMA 硬件，`gpu_direct_bench` / `nvme_tier_bench` / `rdma_transfer_bench` 按能力探测显式跳过并记入报告溯源（需求 5.x）

## 结果（baseline → project）

| Trace | 吞吐 (tok/s) | TTFT p50 (ms) | TTFT p99 (ms) | Hit Rate | Reuse Rate |
|---|---|---|---|---|---|
| mooncake (arxiv) | 159,980 → **205,225** (+28.3%) | 8 → 8 | 476 → **392** | 0.341 → **0.516** | 0.004 → **0.182** |
| conversation | 113,779 → **145,345** (+27.7%) | 52 → **28.5** | 664 → **604** | 0.044 → **0.292** (6.6×) | 0.004 → **0.181** |
| synthetic | 129,687 → **266,335** (+105%) | 56 → **4.5** (−92%) | 520 → **416** | 0.084 → **0.588** (7.0×) | 0.112 → **0.396** |
| toolagent | 159,967 → **205,236** (+28.3%) | 8 → 8 | 476 → **392** | 0.341 → **0.516** | 0.004 → **0.182** |

## 两级分层 + 前缀感知预取

project 目标的回放模型升级为两级分层（bench/tiered.py，对应 C++ HBM▸DRAM 分层
与 `TieredStorageManager::Prefetch`）：

- **两级池**：fast 层（4096 块）+ slow 层（4×）；fast 命中零开销，slow 命中付
  0.5ms/块层间搬运后晋升，未命中付 4ms/块 prefill；fast 淘汰降级到 slow 层。
- **分层混合选型**：在两级模式下重跑策略选型（fast × slow 全组合），
  **fast=S3-FIFO + slow=LRU** 在全部四条 trace 上胜出（synthetic 吞吐
  264,879 tok/s，优于纯 LRU 的 259,531 与纯 S3-FIFO 的 200,310）：单层选型中
  S3-FIFO 的优势在于拒污染，而两级结构下 slow 层天然充当「实体化幽灵队列」，
  slow 层再用 S3-FIFO 的小队列会过早丢弃降级块；LRU 作 slow 层保留更完整的
  重用窗口。slow 容量 2×→4×→8× 单调提升（+5%/+10% 吞吐），报告取 4×（与
  典型 HBM:DRAM 配比一致）；预取深度 ≥4 后收益饱和，取 8。
- **S3-FIFO 参数扫描**（fast 层，小队列占比 5–50% × 幽灵容量 1×/2×）：
  小队列占比不敏感（保留论文默认 10%）；**幽灵队列容量加倍（2×）在四条
  trace 上稳定 +1% 吞吐**（幽灵仅存键，内存成本可忽），已同步到 Python 策略
  库与 C++ `TieredStorageManager`（kGhostCapacity 4096→8192）。
- **前缀感知预取**：维护块后继图（请求内 hash_ids 相邻关系）。请求到达时以
  块序列中每个在存块为锚点，沿后继链把驻留 slow 层的预测块提前批量晋升
  ——搬运与未命中块 prefill 重叠。synthetic trace 上预取把 slow 命中从
  41,451 压到 **2,893（−93%）**，TTFT p50 8ms → **4.5ms**；预取仅对在存块
  生效，不引入污染。锚点选型（仅首块 vs 首个在存块 vs 全在存锚点）：
  **全在存锚点**胜出（arxiv/toolagent 吞吐 +1.6%、conversation +1.6%、
  TTFT p50 32→28.5ms）——覆盖「前缀部分被冲刷、中段仍在存」的对话续写
  场景；命中率微降（0.519→0.516）但搜寻代价被流水线遮蔽，吞吐/TTFT 净赚。
- **生产接线**：绑定层新增 `TieredStore.prefetch`（包装 C++ `Prefetch`），
  connector 的 `load_kv` 装载前批量预取目标键，层间搬运与重算重叠（需求 2.6）。
  块后继图已移植到 C++（`storage::PrefixPrefetcher`，互斥量保护、边数定容、
  环安全）：connector 的 `store_kv` 经 `record_sequence` 记边，`load_kv` 经
  `prefetch_chain` 从末块预测后续块并批量晋升——与回放模型同一机制；
  多锚点选型胜出后，`load_kv` 改用 `prefetch_chain_multi`（全部目标键为
  锚点，单次加锁遍历、去重合并）。

## C++ 生产路径回放一致性

`python -m bench.replay_cpp <trace.jsonl> 4096 4 3000`：把真实 trace 直接重放
到 pybind11 绑定的 C++ `TieredStore`（HBM+DRAM 两层 + `record_sequence` /
`prefetch_chain_multi`，与 connector 生产链路同一 API），与 bench.tiered
回放模型对比命中率（各 trace 前 3000 请求，同容量 4096/16384 块）：

| Trace | C++ 生产路径 hit | 回放模型 hit |
|---|---|---|
| arxiv | 0.470 | 0.469 |
| conversation | 0.283 | 0.280 |
| synthetic | 0.433 | 0.433 |
| toolagent | 0.469 | 0.469 |

偏差 ≤0.3pp（两侧同构：C++ fast=S3-FIFO / slow=纯 LRU + 幽灵准入，模型
fast=S3-FIFO / slow=LRU），验证报告中的模型收益可迁移到交付的 C++ 生产
路径。前 3000 请求下 slow 层尚未进入重淘汰区间，故此抽样表变化甚微；slow
层纯 LRU 化的收益在全量回放（下节）体现。

**淘汰路径 O(1) 化**：受害者选择原为全表线性扫描（每次淘汰 O(n)，n 为在存
块数）；改为每层有序受害者索引（`std::set` 按 (最近性, 键) 升序，即纯 LRU
全序），单次选取 O(1)、索引维护 O(log n)。conversation 全
trace（12,031 请求 / 288,500 块）回放耗时 >280s → **0.68s（>400×）**，命中
率逐位一致（淘汰顺序不变）。

**C++ 最快层淘汰器升级为 S3-FIFO + slow 命中立即晋升**（对齐模型胜出配置：
fast=S3-FIFO 小队列 10%+主队列二次机会+幽灵 2×，slow 命中读路径立即晋升入
主队列）。**较慢层由「分段 LRU+频次加权」改为纯 LRU**（对齐模型 slow=LRU
胜出选型）：降级入层时刷新 last_access_ts 为当前逻辑时钟（等价模型
`slow.admit` 的 move_to_end→MRU），命中即晋升离层、驻留期间不再 touch，故
受害者有序索引退化为 (最近性, 键) 单键——纯 LRU 全序。全 trace 回放命中率
（fast 4096 块 / slow 4×，`high_water_ratio=1.0` 下字节计容≡块数计容）：

| Trace | 分段 LRU | S3-FIFO+晋升 | **+ slow 纯 LRU** | 模型参考 |
|---|---|---|---|---|
| arxiv（全量 23,608 请求） | 0.462 | 0.471 | **0.519** | 0.516 |
| conversation（全量 12,031） | 0.222 | 0.229 | **0.297** | 0.292 |
| synthetic（全量 3,993） | 0.588 | 0.592 | **0.586** | 0.588 |
| toolagent（全量 23,608） | 0.462 | 0.471 | **0.519** | 0.516 |

slow 层纯 LRU 化后，C++ 生产路径命中率在 arxiv/conversation/toolagent 三条
trace 上**已追平并略超模型参考**（+0.3～0.5pp，得益于 C++ 侧额外的多锚点链
式预取 + slow 命中立即晋升），synthetic 微降 0.6pp（该 trace 复用高度集中，
分段冷段优先淘汰恰好略优）。C++↔模型命中率差距（原 arxiv 0.471 vs 0.516）
基本收敛。

## 分层容量扩展（tiered capacity scaling）

分层设计的核心价值：把空闲主机 DRAM 变成 KV 缓存的第二层，在不占用宝贵 HBM
的前提下扩大有效缓存容量。在**交付的 C++ 生产路径**（`bench.replay_cpp`，
fast=4096 块固定，slow 层随因子增长）上全量回放，命中率随 slow 容量单调抬升：

| slow 因子 | arxiv | conversation | synthetic | toolagent |
|---|---|---|---|---|
| 1×（slow=fast） | 0.447 | 0.194 | 0.395 | 0.447 |
| 2× | 0.483 | 0.243 | 0.477 | 0.483 |
| 4×（默认） | **0.519** | **0.297** | **0.586** | **0.519** |
| 8× | 0.547 | 0.343 | 0.638 | 0.547 |

从 1×→8× 命中率单调增长（arxiv +10pp、conversation +15pp、synthetic +24pp、
toolagent +10pp），且 HBM 占用恒定——验证「HBM 装不下的复用块降级到 DRAM
而非丢弃」的分层收益随可用 DRAM 线性变现。生产部署可据主机内存预算把 slow
因子调至 8×+，进一步压低 TTFT。

## 真实 GPU 在线验证（vLLM 0.26 + RTX 5090）

单卡 RTX 5090（32GB，driver 610.43，CUDA 13）+ vLLM 0.26.0 +
Qwen2.5-7B-Instruct（bf16，max_model_len=16384）真实推理服务在线测量。
本项目 S3-FIFO 策略经 `vllm_adapter.offloading_spec`（OffloadingSpecFactory
`spec_module_path` 插件，零上游源码修改）接入 vLLM OffloadingConnector 的
CPU 卸载层。负载：前缀复用多轮会话（共享 2000 词系统前缀 + 会话内历史
累积，`scripts/bench_online.py`，temperature=0、并发 8、流式 TTFT）。

GPU KV cache 限制为 2GiB（37,440 tokens）制造真实容量压力：

| 配置（24 会话 × 4 轮） | TTFT p50 | TTFT p95 | 吞吐 tok/s |
|---|---:|---:|---:|
| GPU-only（无压力，12.7GiB KV） | 0.186s | 0.804s | 434 |
| GPU-only（压力，2GiB KV） | 0.666s | 1.285s | 242 |
| + CPU 卸载 12GB（lru） | 0.273s | 1.130s | 417 |
| + CPU 卸载 12GB（**s3fifo，本项目**） | 0.274s | 1.136s | 416 |

分层卸载把压力场景 TTFT p50 拉回 **2.4×**（0.666→0.274s）、吞吐 **1.7×**
（242→416 tok/s）——验证分层 KVCache 的核心价值主张。CPU 容量充足时
（12GB ≳ 全工作集）lru 与 s3fifo 等价（均全命中）。

CPU 卸载层也承压（容量 < 工作集，40 会话 × 4 轮）时策略差异显现。
完整矩阵：3 策略 × 3 CPU 容量 × 2 次独立冷启动重复（下表为两次重复均值）：

| CPU 容量 | 指标 | lru | arc | **s3fifo（本项目）** |
|---|---|---:|---:|---:|
| 2GB | TTFT mean | 0.671s | 0.615s | **0.594s（−11.5%）** |
| 2GB | TTFT p95 | 1.017s | 0.994s | **0.983s** |
| 3GB | TTFT mean | 0.665s | 0.641s | **0.600s（−9.8%）** |
| 3GB | TTFT p95 | 1.148s | 1.095s | **0.971s（−15.4%）** |
| 4GB | TTFT mean | 0.599s | 0.582s | 0.587s（持平） |
| 4GB | TTFT p95 | 0.972s | 1.133s | **0.965s** |

结论：CPU 层承压（2–3GB）时本项目 S3-FIFO 的 TTFT mean 稳定优于内置
lru **9.8–11.5%**、尾时延 p95 全容量点最优（较 lru 最高 −15.4%）；压力
缓解（4GB）时三者收敛——与离线 trace 结论（S3-FIFO 优势来自容量受限
时的抗扫描淘汰）一致。
（方法注记：每配置独立冷启动服务、同一确定性负载 seed=42；5090 需
`VLLM_USE_FLASHINFER_SAMPLER=0` + `VLLM_ATTENTION_BACKEND=TRITON_ATTN`
规避 flashinfer 对 sm_120 的 JIT 限制。原始数据 18 行 JSONL 见
`docs/benchmarks/online_matrix.jsonl`。）

### 扫描干扰在线验证（抗冲刷尾时延）

复用会话（24 会话 × 4 轮）叠加 16 路扫描干扰（每请求全新 2000 词前缀、
零复用，模拟批量文档摘要/RAG 检索流量），CPU 3GB，2 次独立冷启动重复：

| 淘汰策略 | TTFT p95（rep1 / rep2） | TTFT p50 |
|---|---:|---:|
| lru（vLLM 内置） | 2.028s / 2.058s | ≈0.50s |
| **s3fifo（本项目）** | **1.675s / 1.676s（−17~19%）** | ≈0.51s |

扫描块一次性通过 S3-FIFO 小队列即被逐出，不冲刷复用会话的热前缀——
复用流量的 p95 尾时延两次重复稳定 **−17~19%**，p50 持平。这是离线
trace 结论「S3-FIFO 抗扫描」在真实在线服务上的直接复现，也是多租户
混部（在线对话 + 批量离线任务）场景的核心 SLO 保障能力。

### 三层在线验证（GPU→CPU→NVMe，S3FIFOTieringOffloadingSpec）

把本项目「HBM/DRAM/NVMe 三层降级链」的完整形态搬到真实在线路径：
`S3FIFOTieringOffloadingSpec`（TieringOffloadingSpec 子类，主层 S3-FIFO +
`fs` 次级层落盘 NVMe）。同一负载（40 会话 × 4 轮）、同一 CPU 容量
1.5GB 直接对比：

| 配置（CPU 1.5GB） | TTFT p50 | TTFT mean | 吞吐 tok/s |
|---|---:|---:|---:|
| 两层：GPU→CPU（s3fifo） | 0.652s | 0.653s | 243 |
| **三层：GPU→CPU→NVMe（本项目）** | **0.292s** | **0.366s** | **394** |

NVMe 第三层吸收 CPU 层被逐块（实测落盘 5.9GB），TTFT p50 **2.2×**
（−55%）、吞吐 **1.6×**——在 CPU 内存受限的真实部署形态下，三层设计
的收益在线兑现。这与离线容量扩展研究（slow 1×→8× 命中率单调抬升）
互相印证：分层链越深，同等快层预算下可留存的复用集越大。

### 在线负载再扩展（ShareGPT 真实数据集 / 长上下文 / 重扫描）

把在线验证从合成负载扩展到真实分布（2026-08-04，RTX 5090，各配置
2 重复；原始逐次结果见 `docs/benchmarks/online_matrix3.jsonl`）：

**ShareGPT 真实对话**（`--sharegpt`：32 会话 × 4 轮，会话前缀与轮次
问题均取自真实对话文本，前缀 ≈3000 词）。同一负载、CPU 1.5GB 下
两层 vs 三层：

| 配置（ShareGPT，CPU 1.5GB） | TTFT p50 | TTFT mean | p95 | 吞吐 tok/s |
|---|---:|---:|---:|---:|
| 两层：GPU→CPU（s3fifo） | 0.67s | 0.63s | 0.95s | 237 |
| **三层：GPU→CPU→NVMe（本项目）** | **0.23s** | **0.31s** | **0.78s** | **404** |

三层收益在真实数据分布下**复现且更强**：TTFT p50 **2.9×**（−66%）、
mean −51%、吞吐 **1.7×**（两重复一致：p50 0.234/0.230 vs 0.668/0.674）
——合成负载的结论（2.2×）不是构造出来的。

**长上下文**（16 会话 × 4 轮，前缀 ≈12.4k token，逼近 16k 窗口）：
CPU 3GB 两层下 lru vs s3fifo 中性（±5%），系统在 12k+ 上下文稳定
工作（TTFT p50 0.29–0.35s）。工作集（≈11GB KV）远超 CPU 层时两种
策略同样受限于容量，与离线容量扩展结论一致。

**重扫描（32 路扫描干扰）**：s3fifo p95 1.87/2.00s vs lru 2.07/2.00s
（rep1 −10%，rep2 持平）。对比此前 16 路扫描的 p95 −15~19%：扫描
压力过饱和后（扫描流量 >> 复用流量）策略间差异收敛——S3-FIFO 的
抗扫描收益存在于「混部」区间而非「淹没」区间，如实记档边界。

## 自适应小队列预算（负结果，记档避免重复探索）

实验分支 `devin/adaptive-capacity`：`TierConfig::adaptive_small_ratio`
按幽灵命中率反馈（窗口 128 事件，命中率 >20% 增大 / <5% 收缩，步长
2pp）在 [5%, 30%] 内动态调节最快层 S3-FIFO 小队列占比。实测：

- 敏感性扫描（模型侧，fast=4096，占比 5%→30%）：三 trace 命中率单调
  微降且总幅 ≤1.4pp（arxiv 0.4145→0.4051），固定 10% 已在平坦区。
- 自适应 vs 固定 10%（C++ 路径，四 trace + 两组相位切换混合负载，
  fast=1024/4096）：全部 ±0.1pp 内，中性。

原因：S3-FIFO 的幽灵热准入本身就是「过早逐出」的自纠机制——复用块
被误逐后第二次写入直接热准入主队列，预算失配的代价已被结构性吸收，
留给占比调节的空间不足。结论：保留固定 10%（与模型选型一致），不为
无收益的运行时复杂度买单；分支保留供溯源。

## 多租户配额隔离（tenant quota）

`TierConfig::tenant_weights` 非空即启用加权 max-min 配额：租户每层份额 =
容量 × 权重/总权重；淘汰时**超额最多的租户优先受害**（组内纯 LRU），无超额
租户时回退全局策略（work-conserving：空闲份额可被借用）。配额关闭时零开销、
行为与命中率逐位不变（全量回放回归验证）。

`python -m bench.replay_cpp` 同款 C++ 生产路径，conversation（租户 A，受保护
业务）× toolagent（租户 B，重负载邻居）交错共享 fast=4096/slow=4×：

| 配置 | A 命中率 | B 命中率 |
|---|---|---|
| 无配额 | 0.252 | 0.469 |
| 配额 75/25 | 0.256 | 0.393 |
| 配额 90/10 | **0.278** | 0.350 |

加权配额把命中率（≈TTFT SLO）在租户间**可控分配**：A 权重 90% 时命中率
+2.6pp（相对 +10%），代价由低优租户 B 承担。另验证对抗性纯扫描洪水租户
（`FLOOD` 模式）下配额与无配额命中率几乎持平——S3-FIFO 小队列 + 幽灵准入
本身已把一次性块拦在主队列外，扫描抗性无需配额兜底（反向印证 fast 层
选型）。Property 23 守护「未超额租户免受邻居洪水淘汰」不变量。

## 已探索未采纳（负结果）

以下变体均在同一确定性回放下对比过，因无增益或负收益而未采纳（保留记录
避免重复探索）：

- **后继边频次投票**（跟随出现次数最高的后继，而非后写覆盖）：四 trace
  吞吐/命中持平（±0.05%）——这些负载的块后继关系高度稳定，后写覆盖已足够。
- **slow 命中读穿不晋升**（块留在 slow 层，避免搅动 fast 层）：持平偶有
  微降（synthetic −0.1%）；立即晋升 + 预取已把搬运成本遮蔽。
- **预取深度 4→64 扫描**：多锚点下深度 ≥4 即饱和（链预算已含请求长度项），
  维持默认 8。
- **fast 淘汰不降级（no-demote）**：四 trace 吞吐 −14%～−42%——slow 层作为
  「实体化幽灵队列」的价值得到反向验证，降级链路必须保留。
- **在线后继图预取（NVMe→CPU 预测性晋升，分支 `devin/online-prefetch`）**：
  `S3FIFOPrefetchTieringOffloadingSpec` 把 C++ 侧 PrefixPrefetcher 同款后继图
  搬进 vLLM 三层编排器（`prepare_store` 记边、lookup 命中/晋升键作锚点、
  `on_schedule_end` 沿链预测并复用 `_initiate_promotion` 批量晋升；零上游修改，
  主层满立即让路、预算限流、误预测由 S3-FIFO 小队列结构性吸收）。RTX 5090
  在线 A/B（40 会话 × 4 轮 + 有/无 16 路扫描干扰，各 2 重复，CPU 1.5GB +
  NVMe fs 层）：TTFT p50/p95/mean 与吞吐全部 ±5% 噪声内中性，每轮仅 ~8 次
  预取触发。**根因**：vLLM `_maximal_prefix_lookup` 遇 RETRY 不停扫（只在
  MISS 停），按需晋升已在同一调度步把整个可用连续前缀批量提交给异步
  NVMe 读线程——预取能抢的提前量为零；而请求间隙预取缺乏触发信号（块
  被逐至 NVMe 的时机不可预知，下一轮到达前无 lookup 可锚定）。离线 C++
  路径预取有效（+5.7pp 命中）与在线中性并不矛盾：离线是同步逐块读、
  链式预取真正并行化了搬运；vLLM 的调度步内批量异步晋升已把同样的并行
  化做掉了。分支保留（40 pytest 全绿）供溯源，不合入 main。
- **SIEVE（NSDI'24）替代 S3-FIFO 作 fast 层**：两级模型下跨容量扫描
  （fast∈{1024,2048,4096,8192}）显示 SIEVE **容量敏感且不稳健**——仅在
  fast≥4096 时于 arxiv/conversation/toolagent 微胜 S3-FIFO（+0.2～0.34pp），
  synthetic 反降；在常见的较小 fast（1024/2048）区间 SIEVE 全面落后 S3-FIFO
  达 −0.65～−1.5pp。块加权总命中率仅 +0.18pp 且伴随小缓存回退，收益不足以
  承担生产 fast 路径重写风险，维持 S3-FIFO。

## 缓存策略选型（policy shootout）

`python -m bench.shootout <trace.jsonl> 4096`：同容量下对比 LRU / SIEVE /
S3-FIFO / 前缀感知 LRU / S3-FIFO+前缀准入（bench/policies.py，均为确定性实现）。

| Trace | LRU hit | SIEVE hit | **S3-FIFO hit** | prefix_lru hit |
|---|---|---|---|---|
| arxiv | 0.373 | 0.370 | **0.393** | 0.373 |
| conversation | 0.088 | 0.086 | **0.118** | 0.088 |
| synthetic | 0.237 | 0.158 | **0.238** | 0.236 |
| toolagent | 0.372 | 0.370 | **0.393** | 0.373 |

S3-FIFO（SOSP'23）在全部四条 trace 上胜出（对 LRU：conversation +35%、
arxiv/toolagent +5.5%），已设为 project 目标默认策略；其「幽灵队列 + 热准入」
机制同步移植到 C++ `TieredStorageManager`（被彻底淘汰的冷块键记入定容幽灵
队列，幽灵命中的新写入以热身份准入，免遭一次性访问流量冲刷）。

## C++ 并发存储微基准

`./build/tests/cpp_sharded_store_test --bench`（单锁 TieredStorageManager vs
16 分片 ShardedTieredStore，混合读写）：

- 2 vCPU 虚机：3.54M ops/s → **5.54M ops/s（1.56×）**；分片数 ≥ 核数时随核数近线性扩展。

## 复现

```bash
git submodule update --init --depth 1 third_party/mooncake
PYTHONPATH=python python3 -m bench.framework third_party/mooncake/FAST25-release/traces/synthetic_trace.jsonl
cmake -S . -B build && cmake --build build -j && ./build/tests/cpp_sharded_store_test --bench
```
