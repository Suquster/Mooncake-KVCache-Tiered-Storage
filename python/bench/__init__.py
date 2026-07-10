"""基准框架包（Benchmark_Framework）。

负责 FAST'25 trace 回放、与 vLLM PagedAttention 基线对比，并产出
吞吐 / TTFT / 时延分位、命中率/复用率报告与可复现性保障（需求 5）。

本模块在脚手架阶段先提供「依赖溯源」工具（见 :mod:`bench.provenance`），
它读取 mooncake.lock 并被基准报告（需求 5.5）与冒烟测试复用；
``BenchmarkFramework`` 等实现由任务 12 填充。
"""

from bench.provenance import MooncakeLock, read_mooncake_lock

__all__ = ["MooncakeLock", "read_mooncake_lock"]
