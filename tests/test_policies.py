"""缓存策略（bench.policies）性质测试：容量上界、确定性、S3-FIFO 幽灵再准入。"""

from __future__ import annotations

from hypothesis import given, settings
from hypothesis import strategies as st

from bench.policies import POLICIES, make_policy

_ACCESS_SEQ = st.lists(
    st.tuples(st.integers(min_value=0, max_value=200), st.integers(min_value=0, max_value=63)),
    max_size=400,
)


def _resident_count(policy) -> int:
    if hasattr(policy, "_store"):
        return len(policy._store)
    return len(policy._small) + len(policy._main)


@settings(max_examples=100)
@given(seq=_ACCESS_SEQ, capacity=st.integers(min_value=1, max_value=64))
def test_capacity_bound_all_policies(seq, capacity):
    """任意访问序列后，各策略在存条目数不超过容量。"""
    for name in POLICIES:
        policy = make_policy(name, capacity)
        for hash_id, block_index in seq:
            policy.access(hash_id, block_index)
        assert _resident_count(policy) <= capacity, name


@settings(max_examples=100)
@given(seq=_ACCESS_SEQ, capacity=st.integers(min_value=1, max_value=64))
def test_deterministic_replay(seq, capacity):
    """同一序列重放两次产生完全相同的命中序列（可复现，需求 5.6）。"""
    for name in POLICIES:
        p1, p2 = make_policy(name, capacity), make_policy(name, capacity)
        hits1 = [p1.access(h, b) for h, b in seq]
        hits2 = [p2.access(h, b) for h, b in seq]
        assert hits1 == hits2, name


def test_s3fifo_ghost_readmission():
    """被小队列淘汰的一次性键，再次出现时经幽灵队列直接进入主队列。"""
    policy = make_policy("s3fifo", 20)  # small=2, main=18
    assert policy.access(1) is False
    assert policy.access(2) is False
    assert policy.access(3) is False  # 键 1 从小队列滚入幽灵队列
    assert 1 in policy._ghost
    assert policy.access(1) is False  # 幽灵命中 → 进主队列
    assert 1 in policy._main


def test_first_access_never_hits():
    """任何策略下，键的首次访问必为未命中。"""
    for name in POLICIES:
        policy = make_policy(name, 8)
        assert policy.access(42, 0) is False, name
