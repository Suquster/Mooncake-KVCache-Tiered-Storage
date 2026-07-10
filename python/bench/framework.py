"""Benchmark_Framework：FAST'25 trace 回放、指标计算与报告溯源（任务 12）。

设计依据 design.md「Benchmark_Framework (Python)」：
  * 解析 FAST25 trace（mooncake_trace.jsonl：timestamp / input_length /
    output_length / hash_ids），构造保持到达顺序的回放计划（需求 5.2）。
  * 纯函数指标：p50/p90/p99 分位数与 Cache_Hit_Rate / Reuse_Rate（需求 5.3/5.4）。
  * replay：在同一工作负载/硬件上运行 Project 与 Baseline，报告写入配置、
    软件版本与锁定的上游 Mooncake commit（需求 5.1/5.5）；缺失硬件能力时记录
    并跳过受影响基准（需求 5.7）；固定种子 + 锁定版本 + 容差带保证可复现
    （需求 5.6）。
"""

from __future__ import annotations

import json
import math
import platform
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Literal

from bench.policies import make_policy
from bench.provenance import read_mooncake_lock
from bench.tiered import TieredCosts, TieredSimulator

# 回放模拟的默认 LRU 池容量（块数）。Project 目标为分层池（更大有效容量），
# Baseline 模拟 vLLM 前缀缓存的单层池。
_PROJECT_POOL_BLOCKS = 4096
_BASELINE_POOL_BLOCKS = 1024

# 默认缓存策略：baseline 固定 LRU（vLLM 前缀缓存参照）；project 默认取策略选型
# 胜出者（见 docs/benchmarks/REPORT.md 策略对比）。
_PROJECT_POLICY = "s3fifo"
_BASELINE_POLICY = "lru"

# project 目标两级分层模拟：fast 层（HBM 当量）+ slow 层（DRAM 当量，4×）。
# slow 层默认 LRU：分层混合选型（fast=s3fifo + slow=lru）在四条 trace 上胜出，
# 见 docs/benchmarks/REPORT.md。
_PROJECT_SLOW_FACTOR = 4
_PROJECT_PREFETCH_DEPTH = 8
_PROJECT_SLOW_POLICY = "lru"


@dataclass(frozen=True)
class TraceRecord:
    """FAST25 trace 单条记录（字段与上游 mooncake_trace.jsonl 对齐）。"""

    timestamp: int
    input_length: int
    output_length: int
    hash_ids: tuple[int, ...]


@dataclass(frozen=True)
class ReplayStep:
    """回放计划中的一步：保序 + 保留原始记录字段（需求 5.2）。"""

    arrival_index: int
    record: TraceRecord


@dataclass(frozen=True)
class Percentiles:
    """延迟分位数（需求 5.3）。"""

    p50: float
    p90: float
    p99: float


@dataclass(frozen=True)
class AccessEvent:
    """访问日志事件：某块键的一次查找是否命中。"""

    hash_id: int
    hit: bool


@dataclass(frozen=True)
class AccessLog:
    """回放期间的块访问日志（命中率/复用率的输入，需求 5.4）。"""

    events: tuple[AccessEvent, ...]


@dataclass(frozen=True)
class HardwareProfile:
    """硬件能力画像（缺失能力触发跳过，需求 5.7）。"""

    rdma: bool = False
    gpu_direct: bool = False
    nvme: bool = False


@dataclass(frozen=True)
class RunConfig:
    """单次基准运行配置。"""

    target: Literal["project", "baseline"]
    trace_path: str
    hardware: HardwareProfile = field(default_factory=HardwareProfile)
    seed: int = 0  # 固定种子（需求 5.6）
    mooncake_commit: str = ""  # 留空则自动从 mooncake.lock 读取
    policy: str = ""  # 缓存策略名（bench.policies）；留空按 target 取默认
    prefetch: bool = True  # project 目标：是否启用前缀感知预取


@dataclass(frozen=True)
class RunReport:
    """基准报告：含完整溯源（配置 + 软件版本 + 锁定 commit，需求 5.5）。"""

    config: RunConfig
    software_versions: dict[str, str]
    mooncake_commit: str
    throughput_tok_s: float
    ttft_ms: Percentiles
    e2e_latency_ms: Percentiles
    cache_hit_rate: float
    reuse_rate: float
    skipped: tuple[str, ...]


# ---------------------------------------------------------------------------
# trace 解析与回放计划（需求 5.2）
# ---------------------------------------------------------------------------
def parse_trace_lines(lines: Iterable[str]) -> list[TraceRecord]:
    """把 jsonl 行解析为 TraceRecord 列表（保持输入顺序）。"""
    records: list[TraceRecord] = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        obj = json.loads(stripped)
        records.append(
            TraceRecord(
                timestamp=int(obj["timestamp"]),
                input_length=int(obj["input_length"]),
                output_length=int(obj["output_length"]),
                hash_ids=tuple(int(h) for h in obj["hash_ids"]),
            )
        )
    return records


def parse_trace(path: str | Path) -> list[TraceRecord]:
    """从 jsonl 文件解析 FAST25 trace。"""
    with open(path, encoding="utf-8") as fh:
        return parse_trace_lines(fh)


def build_replay_plan(records: list[TraceRecord]) -> list[ReplayStep]:
    """构造回放计划：逐条保留记录字段并以 arrival_index 固化到达顺序。"""
    return [ReplayStep(i, record) for i, record in enumerate(records)]


# ---------------------------------------------------------------------------
# 纯函数指标（需求 5.3 / 5.4）
# ---------------------------------------------------------------------------
def percentiles(samples: list[float]) -> Percentiles:
    """最近秩法（nearest-rank）分位数：p50/p90/p99。

    非空样本上保证 p50 <= p90 <= p99 且各值落在 [min, max] 内。
    """
    if not samples:
        raise ValueError("样本集为空，无法计算分位数")
    ordered = sorted(samples)

    def nearest_rank(p: float) -> float:
        rank = max(1, math.ceil(p / 100.0 * len(ordered)))
        return ordered[rank - 1]

    return Percentiles(nearest_rank(50), nearest_rank(90), nearest_rank(99))


def rates(access_log: AccessLog) -> tuple[float, float]:
    """(Cache_Hit_Rate, Reuse_Rate)。

    * Cache_Hit_Rate = 命中事件数 / 总查找事件数。
    * Reuse_Rate     = 至少命中过一次的不同块 / 出现过的不同块。
    空日志两率均为 0.0。二者恒在 [0, 1]。
    """
    events = access_log.events
    if not events:
        return 0.0, 0.0
    hits = sum(1 for e in events if e.hit)
    seen: set[int] = set()
    reused: set[int] = set()
    for event in events:
        seen.add(event.hash_id)
        if event.hit:
            reused.add(event.hash_id)
    return hits / len(events), len(reused) / len(seen)


# ---------------------------------------------------------------------------
# 端到端回放编排（需求 5.1 / 5.5 / 5.6 / 5.7）
# ---------------------------------------------------------------------------
def _software_versions() -> dict[str, str]:
    """当前运行环境的软件版本快照（写入报告，需求 5.5）。"""
    return {
        "python": platform.python_version(),
        "platform": platform.platform(),
        "bench_framework": "0.1.0",
    }


def _simulate_replay(
    plan: list[ReplayStep], pool_blocks: int, policy_name: str = "lru"
) -> tuple[AccessLog, list[float], list[float], float]:
    """确定性缓存池回放模拟（策略可插拔，见 bench.policies）。

    返回 (访问日志, TTFT 样本 ms, 端到端延迟样本 ms, 吞吐 tok/s)。
    模型（确定性，保证可复现，需求 5.6）：
      * 命中块免 prefill：TTFT = 未命中块数 * 单块 prefill 开销。
      * decode 开销与 output_length 成正比。
    """
    policy = make_policy(policy_name, pool_blocks)
    events: list[AccessEvent] = []
    ttft_samples: list[float] = []
    e2e_samples: list[float] = []
    total_tokens = 0
    total_time_s = 0.0
    prefill_ms_per_block = 4.0
    decode_ms_per_token = 0.05

    for step in plan:
        record = step.record
        misses = 0
        for block_index, hash_id in enumerate(record.hash_ids):
            hit = policy.access(hash_id, block_index)
            events.append(AccessEvent(hash_id, hit))
            if not hit:
                misses += 1
        ttft_ms = max(misses, 1) * prefill_ms_per_block
        e2e_ms = ttft_ms + record.output_length * decode_ms_per_token
        ttft_samples.append(ttft_ms)
        e2e_samples.append(e2e_ms)
        total_tokens += record.input_length + record.output_length
        total_time_s += e2e_ms / 1000.0

    throughput = total_tokens / total_time_s if total_time_s > 0 else 0.0
    return AccessLog(tuple(events)), ttft_samples, e2e_samples, throughput


def _simulate_tiered_replay(
    plan: list[ReplayStep],
    fast_blocks: int,
    slow_blocks: int,
    policy_name: str,
    prefetch: bool,
) -> tuple[AccessLog, list[float], list[float], float]:
    """两级（fast/slow）分层回放：slow 命中免 prefill，仅付层间搬运开销；
    前缀感知预取把搬运提前到请求间隙（见 bench.tiered）。"""
    costs = TieredCosts()
    sim = TieredSimulator(
        fast_blocks,
        slow_blocks,
        policy_name,
        prefetch_depth=_PROJECT_PREFETCH_DEPTH if prefetch else 0,
        costs=costs,
        slow_policy_name=_PROJECT_SLOW_POLICY,
    )
    events: list[AccessEvent] = []
    ttft_samples: list[float] = []
    e2e_samples: list[float] = []
    total_tokens = 0
    total_time_s = 0.0
    for step in plan:
        record = step.record
        outcome = sim.process_request(record.hash_ids)
        for hash_id, hit in zip(record.hash_ids, outcome.block_hits):
            events.append(AccessEvent(hash_id, hit))
        ttft_ms = outcome.ttft_ms
        e2e_ms = ttft_ms + record.output_length * costs.decode_ms_per_token
        ttft_samples.append(ttft_ms)
        e2e_samples.append(e2e_ms)
        total_tokens += record.input_length + record.output_length
        total_time_s += e2e_ms / 1000.0
    throughput = total_tokens / total_time_s if total_time_s > 0 else 0.0
    return AccessLog(tuple(events)), ttft_samples, e2e_samples, throughput


class BenchmarkFramework:
    """FAST25 回放基准框架（Project vs. Baseline，同工作负载/硬件，需求 5.1）。"""

    # 能力要求表：跳过判定的唯一依据（需求 5.7）。
    _REQUIRED_CAPABILITIES = {
        "rdma_transfer_bench": "rdma",
        "gpu_direct_bench": "gpu_direct",
        "nvme_tier_bench": "nvme",
    }

    def replay(self, cfg: RunConfig) -> RunReport:
        """回放 trace 并产出带完整溯源的报告。"""
        records = parse_trace(cfg.trace_path)
        plan = build_replay_plan(records)
        pool = (
            _PROJECT_POOL_BLOCKS if cfg.target == "project" else _BASELINE_POOL_BLOCKS
        )
        policy_name = cfg.policy or (
            _PROJECT_POLICY if cfg.target == "project" else _BASELINE_POLICY
        )
        if cfg.target == "project":
            access_log, ttft_samples, e2e_samples, throughput = (
                _simulate_tiered_replay(
                    plan,
                    pool,
                    pool * _PROJECT_SLOW_FACTOR,
                    policy_name,
                    cfg.prefetch,
                )
            )
        else:
            access_log, ttft_samples, e2e_samples, throughput = _simulate_replay(
                plan, pool, policy_name
            )
        hit_rate, reuse_rate = rates(access_log)

        skipped = tuple(
            name
            for name, capability in sorted(self._REQUIRED_CAPABILITIES.items())
            if not getattr(cfg.hardware, capability)
        )

        commit = cfg.mooncake_commit or read_mooncake_lock().commit
        empty = Percentiles(0.0, 0.0, 0.0)
        return RunReport(
            config=cfg,
            software_versions=_software_versions(),
            mooncake_commit=commit,
            throughput_tok_s=throughput,
            ttft_ms=percentiles(ttft_samples) if ttft_samples else empty,
            e2e_latency_ms=percentiles(e2e_samples) if e2e_samples else empty,
            cache_hit_rate=hit_rate,
            reuse_rate=reuse_rate,
            skipped=skipped,
        )

    @staticmethod
    def percentiles(samples: list[float]) -> Percentiles:
        return percentiles(samples)

    @staticmethod
    def rates(access_log: AccessLog) -> tuple[float, float]:
        return rates(access_log)


def main(argv: list[str] | None = None) -> int:
    """命令行入口：``python -m bench.framework <trace.jsonl>``。"""
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("用法：python -m bench.framework <trace.jsonl>", file=sys.stderr)
        return 2
    framework = BenchmarkFramework()
    for target in ("baseline", "project"):
        report = framework.replay(RunConfig(target=target, trace_path=args[0]))
        print(
            f"[{target}] throughput={report.throughput_tok_s:.1f} tok/s "
            f"ttft_p50={report.ttft_ms.p50:.2f}ms ttft_p99={report.ttft_ms.p99:.2f}ms "
            f"hit_rate={report.cache_hit_rate:.3f} reuse_rate={report.reuse_rate:.3f} "
            f"mooncake_commit={report.mooncake_commit[:12]} skipped={list(report.skipped)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
