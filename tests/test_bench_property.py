"""基准框架属性测试（Hypothesis，最少 100 次迭代，Properties 14/15/16）。

标签格式：Feature: mooncake-kvcache-optimization, Property {n}: {text}
"""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from bench.framework import (
    AccessEvent,
    AccessLog,
    BenchmarkFramework,
    HardwareProfile,
    RunConfig,
    build_replay_plan,
    parse_trace_lines,
    percentiles,
    rates,
)
from bench.provenance import read_mooncake_lock

record_strategy = st.fixed_dictionaries(
    {
        "timestamp": st.integers(min_value=0, max_value=10**12),
        "input_length": st.integers(min_value=0, max_value=100_000),
        "output_length": st.integers(min_value=0, max_value=100_000),
        "hash_ids": st.lists(
            st.integers(min_value=0, max_value=10**9), max_size=32
        ),
    }
)


@pytest.mark.property
@settings(max_examples=100)
@given(st.lists(record_strategy, max_size=50))
def test_property_14_trace_replay_fidelity(raw_records: list[dict]) -> None:
    """Feature: mooncake-kvcache-optimization, Property 14: trace replay fidelity."""
    lines = [json.dumps(record) for record in raw_records]
    parsed = parse_trace_lines(lines)
    plan = build_replay_plan(parsed)

    assert len(plan) == len(raw_records)
    for i, (step, raw) in enumerate(zip(plan, raw_records)):
        # 保序 + 逐字段保真（需求 5.2）。
        assert step.arrival_index == i
        assert step.record.timestamp == raw["timestamp"]
        assert step.record.input_length == raw["input_length"]
        assert step.record.output_length == raw["output_length"]
        assert step.record.hash_ids == tuple(raw["hash_ids"])


@pytest.mark.property
@settings(max_examples=100)
@given(
    st.lists(
        st.floats(min_value=0.0, max_value=1e6, allow_nan=False), min_size=1
    ),
    st.lists(
        st.tuples(st.integers(min_value=0, max_value=64), st.booleans()),
        max_size=200,
    ),
)
def test_property_15_metric_correctness_and_bounds(
    samples: list[float], raw_events: list[tuple[int, bool]]
) -> None:
    """Feature: mooncake-kvcache-optimization, Property 15: metric computation
    correctness and bounds."""
    result = percentiles(samples)
    assert min(samples) <= result.p50 <= result.p90 <= result.p99 <= max(samples)

    log = AccessLog(tuple(AccessEvent(h, hit) for h, hit in raw_events))
    hit_rate, reuse_rate = rates(log)
    assert 0.0 <= hit_rate <= 1.0
    assert 0.0 <= reuse_rate <= 1.0
    # 参考计算（独立实现）一致性。
    if raw_events:
        ref_hit = sum(1 for _, hit in raw_events if hit) / len(raw_events)
        assert hit_rate == pytest.approx(ref_hit)
        seen = {h for h, _ in raw_events}
        reused = {h for h, hit in raw_events if hit}
        assert reuse_rate == pytest.approx(len(reused) / len(seen))
    else:
        assert (hit_rate, reuse_rate) == (0.0, 0.0)


@pytest.mark.property
@settings(max_examples=100, deadline=None)
@given(
    st.sampled_from(["project", "baseline"]),
    st.booleans(),
    st.booleans(),
    st.booleans(),
    st.lists(record_strategy, max_size=10),
)
def test_property_16_report_provenance_completeness(
    target: str,
    rdma: bool,
    gpu_direct: bool,
    nvme: bool,
    raw_records: list[dict],
) -> None:
    """Feature: mooncake-kvcache-optimization, Property 16: report provenance
    completeness."""

    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "trace.jsonl"
        trace.write_text(
            "\n".join(json.dumps(r) for r in raw_records), encoding="utf-8"
        )
        cfg = RunConfig(
            target=target,
            trace_path=str(trace),
            hardware=HardwareProfile(rdma=rdma, gpu_direct=gpu_direct, nvme=nvme),
        )
        report = BenchmarkFramework().replay(cfg)

    # 报告包含运行配置、软件版本与锁定 commit（需求 5.5）。
    assert report.config == cfg
    assert report.software_versions.get("python")
    assert report.mooncake_commit == read_mooncake_lock().commit
    # 缺失能力被记录为跳过（需求 5.7）。
    expected_skips = {
        name
        for name, capability in {
            "rdma_transfer_bench": rdma,
            "gpu_direct_bench": gpu_direct,
            "nvme_tier_bench": nvme,
        }.items()
        if not capability
    }
    assert set(report.skipped) == expected_skips
