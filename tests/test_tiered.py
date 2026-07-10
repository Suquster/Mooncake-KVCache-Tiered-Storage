# 两级分层模拟 + 前缀感知预取的性质测试（bench.tiered）。
import sys
from pathlib import Path

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from bench.tiered import TieredSimulator  # noqa: E402

_REQUESTS = st.lists(
    st.lists(st.integers(min_value=0, max_value=200), min_size=1, max_size=16).map(
        tuple
    ),
    min_size=1,
    max_size=40,
)


@pytest.mark.property
@settings(max_examples=100)
@given(reqs=_REQUESTS, fast=st.integers(min_value=1, max_value=32))
def test_hit_decomposition_consistent(reqs, fast) -> None:
    """fast+slow+miss == 块数，block_hits 与命中分解一致（确定性）。"""
    sim = TieredSimulator(fast, fast * 4, "s3fifo")
    for req in reqs:
        o = sim.process_request(req)
        assert o.fast_hits + o.slow_hits + o.misses == len(req)
        assert sum(o.block_hits) == o.fast_hits + o.slow_hits


@pytest.mark.property
@settings(max_examples=100)
@given(reqs=_REQUESTS, fast=st.integers(min_value=1, max_value=32))
def test_deterministic_tiered_replay(reqs, fast) -> None:
    """同一请求序列重放两次产生完全相同的结果（可复现，需求 5.6）。"""
    s1 = TieredSimulator(fast, fast * 4, "s3fifo")
    s2 = TieredSimulator(fast, fast * 4, "s3fifo")
    for req in reqs:
        assert s1.process_request(req) == s2.process_request(req)


def _demoted_chain_sim(prefetch_depth: int) -> tuple[TieredSimulator, tuple[int, ...]]:
    """构造「链已整体降级到 slow 层」的场景：先访问链建后继图，再用一轮
    等容量扫描把链从 fast 层挤到 slow 层。"""
    chain = tuple(range(1, 9))
    scan = tuple(range(100, 108))
    sim = TieredSimulator(len(chain), 64, "lru", prefetch_depth=prefetch_depth)
    sim.process_request(chain)
    sim.process_request(scan)
    return sim, chain


def test_prefetch_promotes_predicted_chain() -> None:
    """首块命中时，后继链上驻留 slow 层的块被提前晋升为 fast 命中。"""
    sim, chain = _demoted_chain_sim(prefetch_depth=8)
    outcome = sim.process_request(chain)
    assert outcome.misses == 0
    assert outcome.slow_hits <= 1  # 仅首块自身可能仍是 slow 命中
    assert outcome.fast_hits >= len(chain) - 1


def test_prefetch_disabled_pays_promotion_cost() -> None:
    """预取关闭时，链上块以 slow 命中回归（对照组）。"""
    sim, chain = _demoted_chain_sim(prefetch_depth=0)
    outcome = sim.process_request(chain)
    assert outcome.misses == 0
    assert outcome.slow_hits == len(chain)
