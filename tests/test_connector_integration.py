"""vLLM connector 集成测试（任务 11.2）：store→load 往返、未命中显式返回、
能力上报与跨节点登记。以内存态替身模拟分层存储与调度器（无需 GPU / vLLM）。"""

from __future__ import annotations

from hypothesis import given, settings
from hypothesis import strategies as st
import pytest

from vllm_adapter.connector import (
    BlockKey,
    Capabilities,
    ConnectorConfig,
    ProjectKVConnector,
)


class FakeTieredStore:
    """TieredStore 协议的内存态替身。"""

    def __init__(self) -> None:
        self.data: dict[BlockKey, bytes] = {}
        self.prefetched: list[BlockKey] = []
        self.recorded: list[list[BlockKey]] = []
        self.chain_heads: list[BlockKey] = []
        self.chain_anchors: list[list[BlockKey]] = []

    def put(self, key: BlockKey, payload: bytes) -> None:
        self.data[key] = payload

    def get(self, key: BlockKey) -> bytes | None:
        return self.data.get(key)

    def exists(self, key: BlockKey) -> bool:
        return key in self.data

    def prefetch(self, keys: list[BlockKey]) -> None:
        self.prefetched.extend(keys)

    def record_sequence(self, keys: list[BlockKey]) -> None:
        self.recorded.append(list(keys))

    def prefetch_chain(self, head: BlockKey, budget: int = 8) -> int:
        self.chain_heads.append(head)
        return 0

    def prefetch_chain_multi(
        self, anchors: list[BlockKey], budget: int = 8
    ) -> int:
        self.chain_anchors.append(list(anchors))
        return 0


class FakeScheduler:
    """Scheduler 协议的内存态替身：记录登记事件。"""

    def __init__(self) -> None:
        self.registered: list[tuple[BlockKey, str]] = []

    def register_block(self, key: BlockKey, node: str) -> None:
        self.registered.append((key, node))

    def route(self, keys: list[BlockKey]) -> str:
        return "127.0.0.1:0"


def _connector() -> tuple[ProjectKVConnector, FakeTieredStore, FakeScheduler]:
    store = FakeTieredStore()
    scheduler = FakeScheduler()
    connector = ProjectKVConnector(
        ConnectorConfig(capabilities=Capabilities(rdma=True)),
        scheduler,
        store,
        self_node="10.0.0.1:8080",
    )
    return connector, store, scheduler


@pytest.mark.property
@settings(max_examples=100)
@given(
    st.dictionaries(
        st.integers(min_value=0, max_value=10**9),
        st.binary(max_size=64),
        max_size=16,
    ),
    st.sets(st.integers(min_value=10**10, max_value=10**11), max_size=4),
)
def test_store_load_roundtrip_and_miss(
    stored: dict[int, bytes], absent_ids: set[int]
) -> None:
    """store_kv 写入的块可被 load_kv 原样读回；未写入的键显式列入 missing。"""
    connector, _, scheduler = _connector()
    keys = [BlockKey(hash_id=h) for h in stored]
    payloads = [stored[k.hash_id] for k in keys]
    connector.store_kv("req-1", keys, payloads)

    absent = [BlockKey(hash_id=h) for h in absent_ids]
    result = connector.load_kv("req-1", keys + absent)

    assert result.loaded == {k: stored[k.hash_id] for k in keys}
    assert set(result.missing) == set(absent)
    # 每次写入都在跨节点索引登记了本节点的持有关系。
    assert scheduler.registered == [(k, "10.0.0.1:8080") for k in keys]


def test_load_kv_prefetches_target_keys() -> None:
    """load_kv 装载前先批量预取目标键（提前晋升，需求 2.6）。"""
    connector, store, _ = _connector()
    keys = [BlockKey(hash_id=1), BlockKey(hash_id=2)]
    connector.store_kv("req-p", keys, [b"a", b"b"])
    connector.load_kv("req-p", keys)
    assert store.prefetched == keys


def test_store_records_sequence_and_load_prefetches_chain() -> None:
    """store_kv 记录块后继图；load_kv 以全部目标键为锚点链式预取。"""
    connector, store, _ = _connector()
    keys = [BlockKey(hash_id=1), BlockKey(hash_id=2), BlockKey(hash_id=3)]
    connector.store_kv("req-c", keys, [b"a", b"b", b"c"])
    assert store.recorded == [keys]
    connector.load_kv("req-c", keys[:2])
    assert store.chain_anchors == [keys[:2]]


def test_store_kv_rejects_mismatched_lengths() -> None:
    connector, _, _ = _connector()
    with pytest.raises(ValueError):
        connector.store_kv("req-2", [BlockKey(hash_id=1)], [])


def test_supports_reports_configured_capabilities() -> None:
    connector, _, _ = _connector()
    caps = connector.supports()
    assert caps == Capabilities(rdma=True, gpu_direct=False, nvme=False)
