# Mooncake KVCache 分层存储设计与性能优化

第八届 CCF 开源创新大赛「Mooncake KVCache 存储设计和性能优化」赛道参赛作品。

以 KVCache 为中心的「以存换算」：把空闲主机 DRAM/NVMe 变现为 HBM 之外的
二级/三级 KV 缓存，通过抗扫描淘汰、前缀感知预取与分片并发，在真实 LLM
推理负载上显著提升缓存命中率并压低 TTFT。

## 亮点结果（FAST'25 真实 trace 全量确定性回放，C++ 生产路径）

- 命中率：arxiv/toolagent **0.462→0.519**、conversation **0.222→0.297**（vs 分段 LRU 基线）
- 端到端：synthetic 吞吐 **+18.8%**、TTFT p50 **−71%**；conversation 命中率 **2.7×**
- 分层容量扩展：slow 层 1×→8×，命中率单调 **+10～24pp**，HBM 占用恒定
- 淘汰路径 O(1) 化：conversation 全量回放 >280s → **0.68s**（>400×）
- 16 分片并发存储吞吐 **1.56×**（2 vCPU，多核近线性）

详细数据与方法：[docs/benchmarks/REPORT.md](docs/benchmarks/REPORT.md)

## 架构

```
vLLM (Python) ── KVConnector（python/vllm_adapter）
      │ pybind11（src/bindings）
      ▼
C++ 核心
 ├─ TieredStorageManager：HBM(fast, S3-FIFO) ▸ DRAM(slow, 纯 LRU) ▸ NVMe(可选)
 ├─ ShardedTieredStore：16 分片并发
 ├─ PrefixPrefetcher：后继图 + 多锚点链式预取
 ├─ Scheduler：前缀索引调度
 └─ TransferEngineAdapter：上游 Mooncake Transfer Engine（RDMA→TCP 回退）
```

方案文档：[docs/SOLUTION.md](docs/SOLUTION.md)

## 快速开始

```bash
cmake -B build -DBUILD_PYTHON_BINDINGS=ON -DENABLE_RAPIDCHECK=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure      # C++ 测试（含 22 项属性测试）
export PYTHONPATH=python:build/src
python3 -m pytest tests                          # Python 测试
# 全量 trace 回放（真实 C++ 存储引擎）
python3 -m bench.replay_cpp \
  third_party/mooncake/FAST25-release/arxiv-trace/mooncake_trace.jsonl 4096 4
```

## 质量保障

ASan/UBSan/TSan 三套消毒器矩阵、clang-tidy 0 警告门禁、RapidCheck/Hypothesis
属性测试（各 100 迭代）、确定性回放可复现（固定种子 + `mooncake.lock` 锁定
上游版本）。

## 开源协议与第三方声明

- 本作品：Apache License 2.0（[LICENSE](LICENSE)）
- 第三方来源/协议/依赖：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## 赛题资料

- 官方页面：https://www.gitlink.org.cn/competitions/track2_2026Mooncake
- 本地资料：[docs/01-赛事介绍.md](docs/01-赛事介绍.md) ～ [docs/05-作品提交.md](docs/05-作品提交.md)
